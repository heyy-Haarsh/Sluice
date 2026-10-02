#include "logwriter.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.hpp"

using namespace std;

namespace {

// Read chunk size for draining the pipe (also the natural write batch size)
constexpr size_t READ_CHUNK = 65536;

// Write end held by this (parent) process, closed at shutdown
int g_write_fd = -1;

// The writer's pid, used by the SIGHUP forwarder in the parent
volatile sig_atomic_t g_writer_pid = 0;

// Set in the writer by SIGHUP: reopen the log file (for external rotation)
volatile sig_atomic_t g_reopen = 0;
void on_sighup_reopen(int) { g_reopen = 1; }

// Installed in the parent: forward SIGHUP to the writer so `kill -HUP <gateway>`
// triggers a log reopen (logrotate integration).
void on_sighup_forward(int) {
    if (g_writer_pid > 0) {
        kill(g_writer_pid, SIGHUP);
    }
}

// Build the target filename for time `t`, given the base path and rotation mode.
//   "none"   -> base as-is
//   "daily"  -> "-YYYY-MM-DD" inserted before the extension
//   "hourly" -> "-YYYY-MM-DD-HH" inserted before the extension
string target_filename(const string& base, const string& rotation, time_t t) {
    if (rotation == "none") {
        return base;
    }
    tm* lt = localtime(&t);  // writer is single-threaded, so localtime() is fine
    char stamp[32];
    if (rotation == "hourly") {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H", lt);
    } else {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d", lt);
    }
    size_t dot = base.find_last_of('.');
    size_t slash = base.find_last_of('/');
    if (dot == string::npos || (slash != string::npos && dot < slash)) {
        return base + "-" + stamp;  // no extension to split on
    }
    return base.substr(0, dot) + "-" + stamp + base.substr(dot);
}

int open_log(const string& name) {
    return open(name.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

// Write all n bytes, retrying partial writes and EINTR.
void write_all(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;  // give up on this chunk
        }
        off += static_cast<size_t>(w);
    }
}

// Reopen the current file (SIGHUP handling); falls back to stderr on failure.
void reopen_file(int& file_fd, const string& current) {
    if (file_fd == STDERR_FILENO) {
        return;
    }
    close(file_fd);
    file_fd = open_log(current);
    if (file_fd == -1) {
        file_fd = STDERR_FILENO;
    }
}

// The writer process: drain log lines from the pipe and append them to the file,
// rolling the filename on the clock (if configured) and reopening on SIGHUP.
// Shuts down when all write ends close (read() returns 0).
int writer_main(int read_fd, string base, string rotation) {
    signal(SIGINT, SIG_IGN);   // shut down on pipe EOF, not on Ctrl+C
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_sighup_reopen;
    sigaction(SIGHUP, &sa, nullptr);

    string current = target_filename(base, rotation, time(nullptr));
    int file_fd = open_log(current);
    if (file_fd == -1) {
        Log() << "log writer: cannot open " << current << ": " << strerror(errno)
              << " — writing logs to stderr";
        file_fd = STDERR_FILENO;
    } else {
        Log() << "log writer: writing to " << current
              << (rotation == "none" ? "" : " (" + rotation + " rotation)");
    }

    char buf[READ_CHUNK];
    while (true) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n == 0) {
            break;  // all write ends closed -> drain complete
        }
        if (n < 0) {
            if (errno == EINTR) {
                if (g_reopen) {
                    g_reopen = 0;
                    reopen_file(file_fd, current);
                }
                continue;
            }
            break;  // read error
        }

        if (g_reopen) {  // SIGHUP: external rotation renamed the file away
            g_reopen = 0;
            reopen_file(file_fd, current);
        }
        if (rotation != "none" && file_fd != STDERR_FILENO) {
            string want = target_filename(base, rotation, time(nullptr));
            if (want != current) {
                current = want;
                close(file_fd);
                file_fd = open_log(current);
                if (file_fd == -1) file_fd = STDERR_FILENO;
            }
        }
        write_all(file_fd, buf, static_cast<size_t>(n));
    }

    if (file_fd != STDERR_FILENO && file_fd != -1) {
        close(file_fd);
    }
    return 0;
}

}  // namespace

pid_t log_writer_start(const GatewayConfig& config) {
    if (config.log_file.empty()) {
        return -1;  // file logging disabled: logs stay on stderr
    }

    int fds[2];
    if (pipe(fds) == -1) {
        Log() << "log writer: pipe() failed: " << strerror(errno)
              << " — logging to stderr";
        return -1;
    }
    int read_fd = fds[0];
    int write_fd = fds[1];

    pid_t pid = fork();
    if (pid == -1) {
        Log() << "log writer: fork() failed: " << strerror(errno)
              << " — logging to stderr";
        close(read_fd);
        close(write_fd);
        return -1;
    }
    if (pid == 0) {
        // Writer process: keep the read end only. Its own logs stay on stderr
        // (it never calls log_set_fd), which is what we want.
        close(write_fd);
        _exit(writer_main(read_fd, config.log_file, config.log_rotation));
    }

    // Parent: keep the write end, non-blocking so a full pipe drops instead of
    // stalling the event loop.
    close(read_fd);
    int flags = fcntl(write_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(write_fd, F_SETFL, flags | O_NONBLOCK);
    }
    signal(SIGPIPE, SIG_IGN);  // a dead writer must not kill us with SIGPIPE

    g_write_fd = write_fd;
    g_writer_pid = pid;

    // Forward SIGHUP to the writer (logrotate sends HUP to the gateway pid).
    struct sigaction sa{};
    sa.sa_handler = on_sighup_forward;
    sigaction(SIGHUP, &sa, nullptr);

    log_set_fd(write_fd);
    return pid;
}

void log_writer_stop(pid_t writer_pid) {
    if (writer_pid <= 0) {
        return;
    }
    if (g_write_fd != -1) {
        close(g_write_fd);  // last write end -> writer sees EOF, drains, exits
        g_write_fd = -1;
    }
    waitpid(writer_pid, nullptr, 0);  // may already be reaped; result ignored
}
