#include "dashboard.hpp"
#include "dashboard_ui.hpp"
#include "drain_snapshot.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>
#include <fstream>
#include <chrono>
#include <unistd.h>
#include <random>
#include <iomanip>
#include <sstream>

using nlohmann::json;
using namespace std;

static string generate_random_token() {
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dis(0, 15);
    stringstream ss;
    for (int i = 0; i < 32; ++i) ss << hex << dis(gen);
    return ss.str();
}

void start_dashboard(
    const list<Route>*   live_routes,
    mutex*               routes_mtx,
    DrainSnapshot*       drain_snap,
    const DashboardConfig& dash_cfg,
    LogBuffer*           log_buffer,
    const string&         config_path,
    chrono::steady_clock::time_point start_time,
    int                   worker_processes,
    const string&         log_file,
    const string&         log_rotation)
{
    if (!dash_cfg.enabled) return;

    const int port = dash_cfg.port;
    const string bind = dash_cfg.bind;
    const string token = dash_cfg.token.empty() ? generate_random_token() : dash_cfg.token;
    // Use a shared_ptr so all lambdas share one heap-allocated string that /api/password can mutate
    const string DEFAULT_HASH = "4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2";
    auto pwd_hash = make_shared<string>(dash_cfg.password_hash.empty() ? DEFAULT_HASH : dash_cfg.password_hash);

    thread([live_routes, routes_mtx, drain_snap, port, bind, token, pwd_hash, log_buffer,
            config_path, start_time, worker_processes, log_file, log_rotation, DEFAULT_HASH]() {

        httplib::Server svr;

        // Returns true if the request is allowed to perform a mutating action.
        auto authorized = [token](const httplib::Request& req) {
            auto it = req.headers.find("X-Auth-Token");
            return it != req.headers.end() && it->second == token;
        };

        // ── GET / — embedded SPA (no injected token) ──
        svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(DASHBOARD_HTML, "text/html; charset=utf-8");
        });

        // ── POST /api/login — server-side auth ──
        svr.Post("/api/login", [pwd_hash, token](const httplib::Request& req, httplib::Response& res) {
            json body;
            try { body = json::parse(req.body); } catch(...) {}
            if (body.contains("password_hash") && body["password_hash"].is_string() && body["password_hash"].get<string>() == *pwd_hash) {
                res.set_content(json({{"ok", true}, {"token", token}}).dump(), "application/json");
            } else {
                res.status = 401;
                res.set_content(json({{"error", "unauthorized"}}).dump(), "application/json");
            }
        });

        // ── POST /api/password — change password ──
        svr.Post("/api/password", [config_path, pwd_hash, token, authorized, DEFAULT_HASH](const httplib::Request& req, httplib::Response& res) {
            // Require either active auth token OR that the current password is the default 'root'
            bool is_root = (*pwd_hash == DEFAULT_HASH);
            if (!is_root && !authorized(req)) {
                res.status = 401;
                res.set_content(json({{"ok",false},{"error","unauthorized"}}).dump(), "application/json");
                return;
            }
            json body;
            try { body = json::parse(req.body); } catch(...) {}
            if (!body.contains("password_hash") || !body["password_hash"].is_string()) {
                res.status = 400;
                res.set_content(json({{"ok",false},{"error","Missing password_hash"}}).dump(), "application/json");
                return;
            }
            string new_hash = body["password_hash"].get<string>();

            // Load existing config to inject the new password hash
            json existing_cfg;
            ifstream in(config_path);
            if (in) {
                try { existing_cfg = json::parse(in); } catch (...) { existing_cfg = json::object(); }
            } else {
                existing_cfg = json::object();
            }
            if (!existing_cfg.contains("dashboard")) existing_cfg["dashboard"] = json::object();
            existing_cfg["dashboard"]["password_hash"] = new_hash;

            ofstream out(config_path);
            if (!out) {
                res.status = 500;
                res.set_content(json({{"ok",false},{"error","Cannot save config file"}}).dump(), "application/json");
                return;
            }
            out << existing_cfg.dump(2) << "\n";
            out.close();

            // Update in-memory hash so /api/health and /api/login reflect the change immediately
            *pwd_hash = new_hash;

            // Return the token so the UI can seamlessly log in if this was the initial setup
            res.set_content(json({{"ok",true}, {"token", token}}).dump(), "application/json");
        });

        // ── GET /api/health — PUBLIC aggregate metrics (no sensitive data) ────
        svr.Get("/api/health",
            [live_routes, routes_mtx, drain_snap, start_time, pwd_hash, DEFAULT_HASH]
            (const httplib::Request&, httplib::Response& res) {

            auto uptime_s = chrono::duration_cast<chrono::seconds>(
                chrono::steady_clock::now() - start_time).count();

            int routes_total = 0, backends_total = 0, backends_healthy = 0;
            int active_connections = 0;
            uint64_t bytes_relayed = 0;

            {
                lock_guard<mutex> lock(*routes_mtx);
                for (const auto& r : *live_routes) {
                    routes_total++;
                    for (const auto& b : r.backends) {
                        backends_total++;
                        if (b.healthy.load() && !b.disabled && !b.retiring.load())
                            backends_healthy++;
                        active_connections += b.active_connections.load();
                        bytes_relayed += b.bytes_relayed.load();
                    }
                }
            }

            string status = "healthy";
            if (backends_total > 0 && backends_healthy == 0) status = "critical";
            else if (backends_healthy < backends_total) status = "degraded";

            json root;
            root["status"]              = status;
            root["uptime_seconds"]      = uptime_s;
            root["routes_total"]        = routes_total;
            root["backends_total"]      = backends_total;
            root["backends_healthy"]    = backends_healthy;
            root["active_connections"]  = active_connections;
            root["total_bytes_relayed"] = bytes_relayed;
            root["default_password_active"] = (*pwd_hash == DEFAULT_HASH);

            res.set_content(root.dump(), "application/json");
        });

        // ── GET /api/status — live atomic reads + drain snapshot (auth required) ─
        svr.Get("/api/status",
            [live_routes, routes_mtx, drain_snap, start_time, worker_processes, log_file, log_rotation, authorized]
            (const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req)) {
                res.status = 401;
                res.set_content(json({{"error","unauthorized"}}).dump(), "application/json");
                return;
            }

            auto uptime_s = chrono::duration_cast<chrono::seconds>(
                chrono::steady_clock::now() - start_time).count();

            json root;
            root["uptime_seconds"]   = uptime_s;
            root["worker_processes"] = worker_processes;
            root["log_file"]         = log_file;
            root["log_rotation"]     = log_rotation;

            // Live routes — only atomic fields + immutable config fields.
            // Hold routes_mtx so the relay thread's hot-reload / connection
            // cleanup can't mutate the list structure while we walk it.
            json routes_arr = json::array();
            {
                lock_guard<mutex> lock(*routes_mtx);
                for (const auto& r : *live_routes) {
                    bool udp = (r.protocol == Protocol::Udp);
                    json blist = json::array();
                    for (const auto& b : r.backends) {
                        blist.push_back({
                            {"host",               b.host},
                            {"port",               b.port},
                            {"healthy",            b.healthy.load()},
                            // UDP backends are not actively probed, so their
                            // "healthy" is a default, not a measurement — the UI
                            // renders a neutral indicator when this is false.
                            {"health_checked",     !udp},
                            {"disabled",           b.disabled},
                            {"retiring",           b.retiring.load()},
                            {"active_connections", b.active_connections.load()},
                            {"bytes_relayed",      b.bytes_relayed.load()},
                            {"total_connections",  b.total_connections.load()},
                            {"datagrams_forwarded",    b.datagrams_forwarded.load()},
                            {"datagrams_dropped_rate", b.datagrams_dropped_rate.load()}
                        });
                    }
                    routes_arr.push_back({
                        {"name",        r.name},
                        {"listen_port", r.listen_port},
                        {"protocol",    udp ? "udp" : "tcp"},
                        // UDP-only config fields (0 = unlimited / not set for TCP):
                        {"max_datagram_bytes",        r.max_datagram_bytes},
                        {"udp_health_cooldown_seconds", r.udp_health_cooldown_seconds},
                        {"backends",    blist}
                    });
                }
            }
            root["routes"] = routes_arr;

            // Draining routes — mutex-guarded snapshot, safe to read here
            json draining_arr = json::array();
            if (drain_snap) {
                for (const auto& e : drain_snap->read()) {
                    json blist = json::array();
                    for (const auto& b : e.backends) {
                        blist.push_back({
                            {"host",               b.host},
                            {"port",               b.port},
                            {"active_connections", b.active_connections}
                        });
                    }
                    draining_arr.push_back({
                        {"name",        e.name},
                        {"listen_port", e.listen_port},
                        {"backends",    blist}
                    });
                }
            }
            root["draining"] = draining_arr;

            res.set_content(root.dump(), "application/json");
        });

        // ── GET /api/logs — ring buffer snapshot (auth required) ──────────────
        svr.Get("/api/logs", [log_buffer, authorized](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req)) {
                res.status = 401;
                res.set_content(json({{"error","unauthorized"}}).dump(), "application/json");
                return;
            }
            json arr = json::array();
            if (log_buffer) {
                for (const auto& line : log_buffer->snapshot()) arr.push_back(line);
            }
            res.set_content(arr.dump(), "application/json");
        });

        // ── GET /api/config — raw config file for the form (auth required) ────
        svr.Get("/api/config", [config_path, authorized](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req)) {
                res.status = 401;
                res.set_content(json({{"error","unauthorized"}}).dump(), "application/json");
                return;
            }
            ifstream in(config_path);
            if (!in) {
                res.status = 500;
                res.set_content(json({{"error", "Cannot read config file"}}).dump(), "application/json");
                return;
            }
            string content((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
            res.set_content(content, "application/json");
        });

        // ── POST /api/config/save — validate + write ─────────────────────────
        svr.Post("/api/config/save", [config_path, authorized](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req)) {
                res.status = 401;
                res.set_content(json({{"ok",false},{"error","unauthorized"}}).dump(), "application/json");
                return;
            }
            json new_cfg;
            try {
                new_cfg = json::parse(req.body);
            } catch (const exception& e) {
                res.status = 400;
                res.set_content(json({{"ok",false},{"error",string("Invalid JSON: ")+e.what()}}).dump(), "application/json");
                return;
            }
            if (!new_cfg.contains("routes") || !new_cfg["routes"].is_array()) {
                res.status = 400;
                res.set_content(json({{"ok",false},{"error","Missing or invalid 'routes' array"}}).dump(), "application/json");
                return;
            }
            if (new_cfg["routes"].empty()) {
                res.status = 400;
                res.set_content(json({{"ok",false},{"error","'routes' must not be empty"}}).dump(), "application/json");
                return;
            }

            // Merge: preserve "dashboard" block from existing file
            json existing_cfg;
            ifstream in(config_path);
            if (in) {
                try { existing_cfg = json::parse(in); } catch (...) {}
            }
            if (existing_cfg.contains("dashboard")) {
                new_cfg["dashboard"] = existing_cfg["dashboard"];
            }

            ofstream out(config_path);
            if (!out) {
                res.status = 500;
                res.set_content(json({{"ok",false},{"error","Cannot open config file for writing"}}).dump(), "application/json");
                return;
            }
            out << new_cfg.dump(2) << "\n";
            out.close();
            res.set_content(json({{"ok",true}}).dump(), "application/json");
        });

        // ── POST /api/reload — send SIGUSR1 → apply_reload() in relay thread ──
        // kill(getpid(), SIGUSR1) sets g_reload atomically in the signal handler.
        // apply_reload() runs between epoll batches — never concurrent with IO.
        // (SIGHUP is reserved for log rotation; see logwriter.cpp / gateway.cpp.)
        svr.Post("/api/reload", [authorized](const httplib::Request& req, httplib::Response& res) {
            if (!authorized(req)) {
                res.status = 401;
                res.set_content(json({{"ok",false},{"error","unauthorized"}}).dump(), "application/json");
                return;
            }
            kill(getpid(), SIGUSR1);
            res.set_content(json({{"ok",true}}).dump(), "application/json");
        });

        // Bind the configured interface (loopback by default). The admin API can
        // rewrite the config and trigger reloads, so it must NOT be exposed to
        // the network unless the operator explicitly sets dashboard.bind (and,
        // ideally, dashboard.token). "::" gives a dual-stack listener on Linux.
        svr.listen(bind.c_str(), port);
    }).detach();
}
