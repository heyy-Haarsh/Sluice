#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>

#include "config.hpp"
#include "gateway.hpp"
#include "log.hpp"
#include "logwriter.hpp"
#include "master.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <config.json>\n";
}

}

int main(int argc, char** argv) {
    if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string config_path;
    if (argc == 1) {
        config_path = DEFAULT_CONFIG_PATH;
    }
    else {
        config_path = argv[1];

        if (config_path == "-h" || config_path == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    GatewayConfig config;
    try {
        config = parse_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Sluice: loaded " << config.routes.size() << " route(s)\n";
    for (const Route& r : config.routes) {
        std::cout << "  route \"" << r.name << "\"  listen :" << r.listen_port
                  << "  ->  " << r.backends.size() << " backend(s)\n";
        for (const Backend& b : r.backends) {
            std::cout << "      " << b.host << ":" << b.port
                      << (b.heartbeat_enabled ? "" : "  (heartbeat off)") << "\n";
        }
    }
    std::cout << "  heartbeat: "
              << (config.heartbeat.enabled
                      ? "every " + std::to_string(config.heartbeat.interval_seconds) + "s"
                      : "disabled")
              << "\n";
    std::cout << "  workers: " << config.worker_processes << "\n";
    std::cout << "  log: "
              << (config.log_file.empty()
                      ? "stderr"
                      : config.log_file + " (rotation: " + config.log_rotation + ")")
              << "\n";
    if (!config.dashboard.enabled) {
        std::cout << "  dashboard: disabled\n";
    } else if (config.worker_processes > 1) {
        std::cout << "  dashboard: off (only runs with worker_processes: 1)\n";
    } else {
        std::cout << "  dashboard: http://localhost:" << config.dashboard.port << "\n";
    }
    std::cout << "  rate limit: " << config.rate_limit.max_per_minute
              << "/min per IP (global)";
    if (config.worker_processes > 1) {
        std::cout << ", ~" << (config.rate_limit.max_per_minute / config.worker_processes)
                  << "/min per worker";
    }
    std::cout << "\n";

    // Start the dedicated log-writer process (if a log file is configured) before
    // forking any workers, so they inherit the pipe. -1 means "log to stderr".
    pid_t writer_pid = log_writer_start(config);

    // Access-control startup notices (via Log so they reach the configured sink)
    if (!config.access.whitelist.empty()) {
        Log() << "whitelisting enabled: only whitelisted clients can connect";
        if (!config.access.blacklist.empty()) {
            Log() << "warning: both whitelist and blacklist set globally; "
                     "blacklist (deny) takes precedence";
        }
    }
    for (const Route& r : config.routes) {
        for (const Backend& b : r.backends) {
            if (!b.access.whitelist.empty() && !b.access.blacklist.empty()) {
                Log() << "warning: backend " << b.host << ":" << b.port
                      << " has both whitelist and blacklist; blacklist takes precedence";
            }
        }
    }

    try {
        if (config.worker_processes > 1) {
            run_master(std::move(config));   // fork workers + supervise
        } else {
            run_gateway(std::move(config), config_path);  // single process, same as before
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        log_writer_stop(writer_pid);
        return 1;
    }

    log_writer_stop(writer_pid);
    return 0;
}
