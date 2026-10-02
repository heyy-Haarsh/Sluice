#include "master.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

#include <csignal>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gateway.hpp"
#include "log.hpp"

using namespace std;

namespace {

// A worker that dies sooner than this counts as a crash loop
constexpr auto CRASH_MIN_LIFETIME = chrono::seconds(1);
// How long to wait before respawning a crash-looping worker
constexpr auto CRASH_BACKOFF = chrono::seconds(1);

// Fork one worker. In the child this runs the gateway and never returns.
// In the parent it returns the child's pid, or -1 on fork failure.
pid_t spawn_worker(const GatewayConfig& config, const sigset_t& blocked, int sfd) {
    pid_t pid = fork();
    if (pid == -1) {
        Log() << "master: fork() failed: " << strerror(errno);
        return -1;
    }
    if (pid == 0) {
        // Worker: undo the master's signal blocking and drop its signalfd,
        // then run the event loop (it installs its own SIGINT/SIGTERM handlers).
        sigprocmask(SIG_UNBLOCK, &blocked, nullptr);
        close(sfd);
        try {
            run_gateway(config);
        } catch (const exception& e) {
            Log() << "worker: fatal: " << e.what();
            _exit(1);
        }
        _exit(0);  // never fall back into master code
    }
    return pid;
}

}  // namespace

void run_master(GatewayConfig config) {
    int n = config.worker_processes;

    // Handle SIGINT/SIGTERM/SIGCHLD via a signalfd — race-free, unlike a
    // handler-flag + pause() loop.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);   // config reload — forwarded to workers below
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd == -1) {
        throw runtime_error(string("signalfd(): ") + strerror(errno));
    }

    vector<pid_t> workers(n, -1);
    vector<chrono::steady_clock::time_point> spawned_at(n);

    for (int i = 0; i < n; ++i) {
        workers[i] = spawn_worker(config, mask, sfd);
        spawned_at[i] = chrono::steady_clock::now();
        if (workers[i] == -1 && i == 0) {
            throw runtime_error("master: could not spawn any workers");
        }
    }
    Log() << "master: started " << n << " worker(s)";

    bool shutting_down = false;
    while (!shutting_down) {
        signalfd_siginfo si;
        ssize_t r = read(sfd, &si, sizeof(si));
        if (r != static_cast<ssize_t>(sizeof(si))) {
            if (errno == EINTR) continue;
            break;
        }

        if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
            shutting_down = true;
            Log() << "master: signal received, stopping workers";
            for (pid_t pid : workers) {
                if (pid > 0) kill(pid, SIGTERM);
            }
            break;
        }

        if (si.ssi_signo == SIGUSR1) {
            // Config hot-reload: fan out to every worker (each re-parses the
            // config file and applies the diff in its own event loop).
            Log() << "master: reload signal received, forwarding to workers";
            for (pid_t pid : workers) {
                if (pid > 0) kill(pid, SIGUSR1);
            }
            continue;
        }

        if (si.ssi_signo == SIGCHLD) {
            // A single SIGCHLD can cover several deaths — reap them all.
            int status;
            pid_t dead;
            while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
                int slot = -1;
                for (int i = 0; i < n; ++i) {
                    if (workers[i] == dead) { slot = i; break; }
                }
                if (slot < 0) continue;
                workers[slot] = -1;
                Log() << "master: worker pid=" << dead << " (slot " << slot
                      << ") exited";
                if (shutting_down) continue;

                // Crash-loop guard: back off if it barely lived
                auto lifetime = chrono::steady_clock::now() - spawned_at[slot];
                if (lifetime < CRASH_MIN_LIFETIME) {
                    Log() << "master: worker slot " << slot << " died in "
                          << chrono::duration_cast<chrono::milliseconds>(lifetime).count()
                          << "ms, backing off before respawn";
                    sleep(static_cast<unsigned>(
                        chrono::duration_cast<chrono::seconds>(CRASH_BACKOFF).count()));
                }
                workers[slot] = spawn_worker(config, mask, sfd);
                spawned_at[slot] = chrono::steady_clock::now();
                if (workers[slot] != -1) {
                    Log() << "master: respawned slot " << slot << " as pid="
                          << workers[slot];
                }
            }
        }
    }

    // Wait for every worker to exit before the master returns.
    for (pid_t pid : workers) {
        if (pid > 0) waitpid(pid, nullptr, 0);
    }
    close(sfd);
    Log() << "master: all workers stopped";
}
