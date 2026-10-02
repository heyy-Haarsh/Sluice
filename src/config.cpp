#include "config.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "access.hpp"

using nlohmann::json;
using namespace std;
namespace fs = std::filesystem;

namespace {

// Minimum non-zero UDP session idle timeout. A UDP session has no FIN, so it
// only ends via the idle sweep; any configured value below this (including 0)
// is floored to it so sessions can't leak.
constexpr int UDP_MIN_SESSION_TIMEOUT = 5;

// TCP port must fit in 1..65535
bool valid_port(int port) {
    return port >= 1 && port <= 65535;
}

// Check if route contains some required string fields
string require_string(const json& route, const char* field, size_t index) {
    if (!route.contains(field)) {
        throw runtime_error("route[" + to_string(index) +
                                 "]: missing required field \"" + field + "\"");
    }
    if (!route[field].is_string()) {
        throw runtime_error("route[" + to_string(index) +
                                 "]: field \"" + field + "\" must be a string");
    }
    return route[field].get<string>();
}
// Check if route contains some required int fields
int require_int(const json& route, const char* field, size_t index) {
    if (!route.contains(field)) {
        throw runtime_error("route[" + to_string(index) +
                                 "]: missing required field \"" + field + "\"");
    }
    if (!route[field].is_number_integer()) {
        throw runtime_error("route[" + to_string(index) +
                                 "]: field \"" + field + "\" must be an integer");
    }
    return route[field].get<int>();
}

// Parse the optional top-level "worker_processes" (integer >= 1, or "auto")
int parse_worker_processes(const json& root) {
    if (!root.contains("worker_processes")) {
        return 1;
    }
    const json& w = root["worker_processes"];
    if (w.is_string()) {
        if (w.get<string>() != "auto") {
            throw runtime_error("config: \"worker_processes\" string must be \"auto\"");
        }
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? static_cast<int>(n) : 1;
    }
    if (w.is_number_integer()) {
        int n = w.get<int>();
        if (n < 1) {
            throw runtime_error("config: \"worker_processes\" must be >= 1");
        }
        return n;
    }
    throw runtime_error("config: \"worker_processes\" must be an integer or \"auto\"");
}

// Parse the optional top-level "heartbeat" block
HeartbeatConfig parse_heartbeat(const json& root) {
    HeartbeatConfig hb;  // defaults: enabled, 10s
    if (!root.contains("heartbeat")) {
        return hb;
    }
    const json& h = root["heartbeat"];
    if (!h.is_object()) {
        throw runtime_error("config: \"heartbeat\" must be an object");
    }
    if (h.contains("enabled")) {
        if (!h["enabled"].is_boolean()) {
            throw runtime_error("config: heartbeat \"enabled\" must be a boolean");
        }
        hb.enabled = h["enabled"].get<bool>();
    }
    if (h.contains("interval_seconds")) {
        if (!h["interval_seconds"].is_number_integer()) {
            throw runtime_error("config: heartbeat \"interval_seconds\" must be an integer");
        }
        hb.interval_seconds = h["interval_seconds"].get<int>();
        if (hb.interval_seconds <= 0) {
            throw runtime_error("config: heartbeat \"interval_seconds\" must be > 0");
        }
    }
    return hb;
}

// Parse the optional top-level "dashboard" block
DashboardConfig parse_dashboard(const json& root) {
    DashboardConfig dc; // defaults
    if (!root.contains("dashboard")) {
        return dc;
    }
    const json& d = root["dashboard"];
    if (!d.is_object()) {
        throw runtime_error("config: \"dashboard\" must be an object");
    }
    if (d.contains("enabled")) {
        if (!d["enabled"].is_boolean()) {
            throw runtime_error("config: dashboard \"enabled\" must be a boolean");
        }
        dc.enabled = d["enabled"].get<bool>();
    }
    if (d.contains("port")) {
        if (!d["port"].is_number_integer()) {
            throw runtime_error("config: dashboard \"port\" must be an integer");
        }
        dc.port = d["port"].get<int>();
        if (!valid_port(dc.port)) {
            throw runtime_error("config: dashboard \"port\" out of range (1-65535)");
        }
    }
    if (d.contains("log_lines")) {
        if (!d["log_lines"].is_number_integer()) {
            throw runtime_error("config: dashboard \"log_lines\" must be an integer");
        }
        dc.log_lines = d["log_lines"].get<int>();
        if (dc.log_lines <= 0) {
            throw runtime_error("config: dashboard \"log_lines\" must be > 0");
        }
    }
    if (d.contains("bind")) {
        if (!d["bind"].is_string()) {
            throw runtime_error("config: dashboard \"bind\" must be a string");
        }
        dc.bind = d["bind"].get<string>();
        if (dc.bind.empty()) {
            throw runtime_error("config: dashboard \"bind\" must not be empty");
        }
    }
    if (d.contains("token")) {
        if (!d["token"].is_string()) {
            throw runtime_error("config: dashboard \"token\" must be a string");
        }
        dc.token = d["token"].get<string>();
    }
    if (d.contains("password_hash")) {
        if (!d["password_hash"].is_string()) {
            throw runtime_error("config: dashboard \"password_hash\" must be a string");
        }
        dc.password_hash = d["password_hash"].get<string>();
    }
    return dc;
}

// Parse an absolute datetime "YYYY-MM-DDTHH:MM:SS" (local time) into a time_point.
chrono::system_clock::time_point parse_datetime(const string& s, const string& where) {
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) {
        throw runtime_error(where +
            ": blacklist \"until\" must be \"YYYY-MM-DDTHH:MM:SS\" (local time), got: " + s);
    }
    tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    t.tm_isdst = -1;  // let mktime work out DST
    time_t tt = mktime(&t);
    if (tt == static_cast<time_t>(-1)) {
        throw runtime_error(where + ": invalid \"until\" datetime: " + s);
    }
    return chrono::system_clock::from_time_t(tt);
}

// Entries may be an exact IPv4 address or a CIDR range ("10.0.0.0/24"). Only
// the CIDR form is validated here (bad exact IPs have always been tolerated
// silently — they just never match); a malformed CIDR is much easier to get
// wrong and would otherwise fail to match silently, so it's rejected at parse
// time with a clear error instead.
void validate_ip_or_cidr(const string& s, const string& where) {
    if (s.find('/') == string::npos) {
        return;
    }
    CidrRange range;
    if (!parse_cidr(s, range)) {
        throw runtime_error(where + ": invalid CIDR \"" + s +
                                 "\" (expected e.g. \"10.0.0.0/24\", prefix 0-32)");
    }
}

// Parse "whitelist"/"blacklist" fields from a container (global root or backend)
AccessList parse_access(const json& c, const string& where) {
    AccessList a;
    if (c.contains("whitelist")) {
        if (!c["whitelist"].is_array()) {
            throw runtime_error(where + ": \"whitelist\" must be an array");
        }
        for (const json& e : c["whitelist"]) {
            if (!e.is_string()) {
                throw runtime_error(where + ": whitelist entries must be strings (IPs or CIDR ranges)");
            }
            string ip = e.get<string>();
            validate_ip_or_cidr(ip, where + " whitelist");
            a.whitelist.push_back(ip);
        }
    }
    if (c.contains("blacklist")) {
        if (!c["blacklist"].is_array()) {
            throw runtime_error(where + ": \"blacklist\" must be an array");
        }
        for (const json& e : c["blacklist"]) {
            BlacklistEntry be;
            if (e.is_string()) {
                be.ip = e.get<string>();  // permanent block
            } else if (e.is_object()) {
                if (!e.contains("ip") || !e["ip"].is_string()) {
                    throw runtime_error(where + ": blacklist object needs a string \"ip\"");
                }
                be.ip = e["ip"].get<string>();
                if (e.contains("until")) {
                    if (!e["until"].is_string()) {
                        throw runtime_error(where + ": blacklist \"until\" must be a datetime string");
                    }
                    be.until = parse_datetime(e["until"].get<string>(), where);
                    be.has_until = true;
                }
            } else {
                throw runtime_error(where + ": blacklist entries must be a string or an object");
            }
            validate_ip_or_cidr(be.ip, where + " blacklist");
            a.blacklist.push_back(be);
        }
    }
    return a;
}

// Parse the optional per-route "affinity" array: client (IP or CIDR) -> a
// preferred backend within the same route.
vector<AffinityRule> parse_affinity(const json& r, size_t route_index) {
    vector<AffinityRule> rules;
    if (!r.contains("affinity")) {
        return rules;
    }
    if (!r["affinity"].is_array()) {
        throw runtime_error("route[" + to_string(route_index) + "]: \"affinity\" must be an array");
    }
    for (const json& e : r["affinity"]) {
        if (!e.is_object()) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: each \"affinity\" entry must be an object");
        }
        AffinityRule rule;
        rule.client = require_string(e, "client", route_index);
        rule.host = require_string(e, "host", route_index);
        rule.port = require_int(e, "port", route_index);
        if (!valid_port(rule.port)) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: affinity \"port\" " + to_string(rule.port) +
                                     " out of range (1-65535)");
        }
        array<uint8_t, 16> addr;
        CidrRange range;
        if (!parse_ip(rule.client, addr) && !parse_cidr(rule.client, range)) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: affinity \"client\" \"" + rule.client +
                                     "\" must be a valid IP address or CIDR range");
        }
        if (!parse_ip(rule.host, addr)) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: affinity \"host\" \"" + rule.host +
                                     "\" must be a valid IP address");
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

// Parse the optional global "rate_limit" block
RateLimitConfig parse_rate_limit(const json& root) {
    RateLimitConfig rl;  // defaults: 1000/min, 60s base, 3600s cap
    if (!root.contains("rate_limit")) {
        return rl;
    }
    const json& r = root["rate_limit"];
    if (!r.is_object()) {
        throw runtime_error("config: \"rate_limit\" must be an object");
    }
    if (r.contains("max_per_minute")) {
        if (!r["max_per_minute"].is_number_integer()) {
            throw runtime_error("config: rate_limit \"max_per_minute\" must be an integer");
        }
        rl.max_per_minute = r["max_per_minute"].get<int>();
        if (rl.max_per_minute < 1) {
            throw runtime_error("config: rate_limit \"max_per_minute\" must be >= 1");
        }
    }
    if (r.contains("backoff_base_seconds")) {
        if (!r["backoff_base_seconds"].is_number_integer()) {
            throw runtime_error("config: rate_limit \"backoff_base_seconds\" must be an integer");
        }
        rl.backoff_base_seconds = r["backoff_base_seconds"].get<int>();
        if (rl.backoff_base_seconds < 1) {
            throw runtime_error("config: rate_limit \"backoff_base_seconds\" must be >= 1");
        }
    }
    if (r.contains("backoff_max_seconds")) {
        if (!r["backoff_max_seconds"].is_number_integer()) {
            throw runtime_error("config: rate_limit \"backoff_max_seconds\" must be an integer");
        }
        rl.backoff_max_seconds = r["backoff_max_seconds"].get<int>();
        if (rl.backoff_max_seconds < 1) {
            throw runtime_error("config: rate_limit \"backoff_max_seconds\" must be >= 1");
        }
    }
    return rl;
}

// Parse one backend object
Backend parse_backend(const json& b, size_t route_index) {
    if (!b.is_object()) {
        throw runtime_error("route[" + to_string(route_index) +
                                 "]: each backend must be an object");
    }
    Backend backend;
    backend.host = require_string(b, "host", route_index);
    backend.port = require_int(b, "port", route_index);

    if (backend.host.empty()) {
        throw runtime_error("route[" + to_string(route_index) +
                                 "]: backend \"host\" must not be empty");
    }
    if (!valid_port(backend.port)) {
        throw runtime_error("route[" + to_string(route_index) +
                                 "]: backend \"port\" " + to_string(backend.port) +
                                 " out of range (1-65535)");
    }
    // Optional per-backend "heartbeat" flag (false = exclude from probes)
    if (b.contains("heartbeat")) {
        if (!b["heartbeat"].is_boolean()) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: backend \"heartbeat\" must be a boolean");
        }
        backend.heartbeat_enabled = b["heartbeat"].get<bool>();
    }
    // Optional per-backend "disabled" flag (true = never selected)
    if (b.contains("disabled")) {
        if (!b["disabled"].is_boolean()) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: backend \"disabled\" must be a boolean");
        }
        backend.disabled = b["disabled"].get<bool>();
    }
    // Optional per-backend "proxy_protocol" flag (true = prepend a PROXY v1
    // header so the backend can see the real client IP). Default off.
    if (b.contains("proxy_protocol")) {
        if (!b["proxy_protocol"].is_boolean()) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: backend \"proxy_protocol\" must be a boolean");
        }
        backend.proxy_protocol = b["proxy_protocol"].get<bool>();
    }
    // Optional per-backend whitelist/blacklist + rate limit
    backend.access = parse_access(b, "route[" + to_string(route_index) + "] backend");
    if (b.contains("rate_limit")) {
        const json& r = b["rate_limit"];
        if (!r.is_object() || !r.contains("max_per_minute") ||
            !r["max_per_minute"].is_number_integer()) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: backend \"rate_limit\" must be an object with "
                                     "integer \"max_per_minute\"");
        }
        backend.rate_max_per_minute = r["max_per_minute"].get<int>();
        if (backend.rate_max_per_minute < 1) {
            throw runtime_error("route[" + to_string(route_index) +
                                     "]: backend rate_limit \"max_per_minute\" must be >= 1");
        }
    }
    return backend;
}

}

GatewayConfig parse_config(const string& path) {
    ifstream in(path);
    if (!in) {
        throw runtime_error("cannot open config file: " + path);
    }

    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        throw runtime_error(string("invalid JSON in: ") + e.what());
    }

    if (!root.contains("routes")) {
        throw runtime_error("config: missing \"routes\" array");
    }
    if (!root["routes"].is_array()) {
        throw runtime_error("config: \"routes\" must be an array");
    }
    if (root["routes"].empty()) {
        throw runtime_error("config: \"routes\" is empty");
    }

    GatewayConfig config;
    config.heartbeat = parse_heartbeat(root);
    config.dashboard = parse_dashboard(root);
    config.access = parse_access(root, "config");   // global whitelist/blacklist
    config.rate_limit = parse_rate_limit(root);      // global per-IP rate limit
    config.worker_processes = parse_worker_processes(root);

    // Optional global idle-connection timeout (seconds; 0/absent = disabled)
    if (root.contains("idle_timeout_seconds")) {
        if (!root["idle_timeout_seconds"].is_number_integer()) {
            throw runtime_error("config: \"idle_timeout_seconds\" must be an integer");
        }
        config.idle_timeout_seconds = root["idle_timeout_seconds"].get<int>();
        if (config.idle_timeout_seconds < 0) {
            throw runtime_error("config: \"idle_timeout_seconds\" must be >= 0");
        }
    }

    // Optional cap on simultaneous connections (0 = unlimited)
    if (root.contains("max_connections")) {
        if (!root["max_connections"].is_number_integer()) {
            throw runtime_error("config: \"max_connections\" must be an integer");
        }
        config.max_connections = root["max_connections"].get<int>();
        if (config.max_connections < 0) {
            throw runtime_error("config: \"max_connections\" must be >= 0");
        }
    }

    // Optional global UDP session idle timeout (seconds). Floored to a non-zero
    // minimum so UDP sessions always expire (they have no FIN).
    if (root.contains("udp_session_timeout_seconds")) {
        if (!root["udp_session_timeout_seconds"].is_number_integer()) {
            throw runtime_error("config: \"udp_session_timeout_seconds\" must be an integer");
        }
        config.udp_session_timeout_seconds =
            root["udp_session_timeout_seconds"].get<int>();
        if (config.udp_session_timeout_seconds < 0) {
            throw runtime_error("config: \"udp_session_timeout_seconds\" must be >= 0");
        }
        if (config.udp_session_timeout_seconds < UDP_MIN_SESSION_TIMEOUT) {
            config.udp_session_timeout_seconds = UDP_MIN_SESSION_TIMEOUT;
        }
    }

    // Optional cap on simultaneous UDP sessions (0 = unlimited).
    if (root.contains("max_udp_sessions")) {
        if (!root["max_udp_sessions"].is_number_integer()) {
            throw runtime_error("config: \"max_udp_sessions\" must be an integer");
        }
        config.max_udp_sessions = root["max_udp_sessions"].get<int>();
        if (config.max_udp_sessions < 0) {
            throw runtime_error("config: \"max_udp_sessions\" must be >= 0");
        }
    }

    // Optional global default for the per-session UDP datagram rate cap
    // (datagrams/second, client->backend direction). 0 = unlimited (default).
    if (root.contains("udp_datagram_rate_limit")) {
        if (!root["udp_datagram_rate_limit"].is_number_integer()) {
            throw runtime_error("config: \"udp_datagram_rate_limit\" must be an integer");
        }
        config.udp_datagram_rate_limit = root["udp_datagram_rate_limit"].get<int>();
        if (config.udp_datagram_rate_limit < 0) {
            throw runtime_error("config: \"udp_datagram_rate_limit\" must be >= 0");
        }
    }

    // Optional global UDP backend health-cooldown (seconds). Defaults to 10.
    // Must be >= 1 — a zero or negative cooldown would flip a failed backend
    // back immediately every sweep and effectively disable passive health tracking.
    if (root.contains("udp_health_cooldown_seconds")) {
        if (!root["udp_health_cooldown_seconds"].is_number_integer()) {
            throw runtime_error("config: \"udp_health_cooldown_seconds\" must be an integer");
        }
        config.udp_health_cooldown_seconds = root["udp_health_cooldown_seconds"].get<int>();
        if (config.udp_health_cooldown_seconds < 1) {
            throw runtime_error("config: \"udp_health_cooldown_seconds\" must be >= 1");
        }
    }

    // Optional file logging (a dedicated writer process owns the file)
    if (root.contains("log_file")) {
        if (!root["log_file"].is_string()) {
            throw runtime_error("config: \"log_file\" must be a string");
        }
        config.log_file = root["log_file"].get<string>();
    }
    if (root.contains("log_rotation")) {
        if (!root["log_rotation"].is_string()) {
            throw runtime_error("config: \"log_rotation\" must be a string");
        }
        string rot = root["log_rotation"].get<string>();
        if (rot != "none" && rot != "hourly" && rot != "daily") {
            throw runtime_error(
                "config: \"log_rotation\" must be \"none\", \"hourly\", or \"daily\"");
        }
        config.log_rotation = rot;
    }

    // Port uniqueness is per (protocol, port): tcp:9000 and udp:9000 may coexist.
    set<pair<int,int>> seen_ports;   // (protocol, listen_port)
    set<string> seen_names;

    const json& route_array = root["routes"];
    for (size_t i = 0; i < route_array.size(); ++i) {
        const json& r = route_array[i];
        if (!r.is_object()) {
            throw runtime_error("route[" + to_string(i) +
                                     "]: must be an object");
        }

        Route route;
        route.name = require_string(r, "name", i);
        route.listen_port = require_int(r, "listen_port", i);

        if (route.name.empty()) {
            throw runtime_error("route[" + to_string(i) +
                                     "]: \"name\" must not be empty");
        }
        if (!valid_port(route.listen_port)) {
            throw runtime_error("route[" + to_string(i) +
                                     "]: \"listen_port\" " +
                                     to_string(route.listen_port) +
                                     " out of range (1-65535)");
        }

        // Optional transport protocol: "tcp" (default) or "udp".
        if (r.contains("protocol")) {
            if (!r["protocol"].is_string()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"protocol\" must be a string");
            }
            string proto = r["protocol"].get<string>();
            if (proto == "tcp") {
                route.protocol = Protocol::Tcp;
            } else if (proto == "udp") {
                route.protocol = Protocol::Udp;
            } else {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"protocol\" must be \"tcp\" or \"udp\", got \"" +
                                         proto + "\"");
            }
        }

        // backends must be a non-empty array; parse them all
        if (!r.contains("backends")) {
            throw runtime_error("route[" + to_string(i) +
                                     "]: missing required field \"backends\"");
        }
        if (!r["backends"].is_array() || r["backends"].empty()) {
            throw runtime_error("route[" + to_string(i) +
                                     "]: \"backends\" must be a non-empty array");
        }
        for (const json& b : r["backends"]) {
            route.backends.push_back(parse_backend(b, i));
        }

        // Optional per-route idle-connection timeout override.
        // Absent = inherit the global idle_timeout_seconds (sentinel -1).
        if (r.contains("idle_timeout_seconds")) {
            if (!r["idle_timeout_seconds"].is_number_integer()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"idle_timeout_seconds\" must be an integer");
            }
            route.idle_timeout_seconds = r["idle_timeout_seconds"].get<int>();
            if (route.idle_timeout_seconds < 0) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"idle_timeout_seconds\" must be >= 0");
            }
        }

        // Optional per-route UDP session idle-timeout override (seconds).
        // Absent = inherit the global udp_session_timeout_seconds (sentinel -1).
        // Any specified value is floored so a UDP session can never be un-expiring.
        if (r.contains("udp_session_timeout_seconds")) {
            if (!r["udp_session_timeout_seconds"].is_number_integer()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_session_timeout_seconds\" must be an integer");
            }
            route.udp_session_timeout_seconds =
                r["udp_session_timeout_seconds"].get<int>();
            if (route.udp_session_timeout_seconds < 0) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_session_timeout_seconds\" must be >= 0");
            }
            if (route.udp_session_timeout_seconds < UDP_MIN_SESSION_TIMEOUT) {
                route.udp_session_timeout_seconds = UDP_MIN_SESSION_TIMEOUT;
            }
        }

        // Optional per-route UDP datagram rate cap (datagrams/second).
        // Absent = inherit the global udp_datagram_rate_limit (sentinel -1);
        // 0 = explicitly unlimited for this route.
        if (r.contains("udp_datagram_rate_limit")) {
            if (!r["udp_datagram_rate_limit"].is_number_integer()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_datagram_rate_limit\" must be an integer");
            }
            route.udp_datagram_rate_limit = r["udp_datagram_rate_limit"].get<int>();
            if (route.udp_datagram_rate_limit < 0) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_datagram_rate_limit\" must be >= 0");
            }
            // Meaningful for UDP routes only; warn (don't fail) on a stale/
            // copy-pasted setting on a TCP route, mirroring the proxy_protocol
            // on-UDP warning above.
            if (route.protocol == Protocol::Tcp) {
                fprintf(stderr,
                        "warning: route \"%s\" is TCP; \"udp_datagram_rate_limit\" "
                        "is ignored\n", route.name.c_str());
            }
        }

        // Optional per-route maximum datagram payload size (UDP only). Datagrams
        // larger than this are silently dropped before forwarding. 0 = unlimited.
        if (r.contains("max_datagram_bytes")) {
            if (!r["max_datagram_bytes"].is_number_integer()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"max_datagram_bytes\" must be an integer");
            }
            route.max_datagram_bytes = r["max_datagram_bytes"].get<int>();
            if (route.max_datagram_bytes < 0) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"max_datagram_bytes\" must be >= 0");
            }
            if (route.protocol == Protocol::Tcp && route.max_datagram_bytes > 0) {
                fprintf(stderr,
                        "warning: route \"%s\" is TCP; \"max_datagram_bytes\" "
                        "is ignored\n", route.name.c_str());
            }
        }

        // Optional per-route UDP backend health-cooldown override (seconds).
        // -1 = inherit the global udp_health_cooldown_seconds.
        if (r.contains("udp_health_cooldown_seconds")) {
            if (!r["udp_health_cooldown_seconds"].is_number_integer()) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_health_cooldown_seconds\" must be an integer");
            }
            route.udp_health_cooldown_seconds = r["udp_health_cooldown_seconds"].get<int>();
            if (route.udp_health_cooldown_seconds < 1) {
                throw runtime_error("route[" + to_string(i) +
                                         "]: \"udp_health_cooldown_seconds\" must be >= 1");
            }
            if (route.protocol == Protocol::Tcp) {
                fprintf(stderr,
                        "warning: route \"%s\" is TCP; \"udp_health_cooldown_seconds\" "
                        "is ignored\n", route.name.c_str());
            }
        }

        // Optional client -> preferred-backend pins for this route.
        route.affinity = parse_affinity(r, i);

        // Two routes cannot bind the same (protocol, listen_port) pair.
        if (!seen_ports.insert({static_cast<int>(route.protocol),
                                route.listen_port}).second) {
            throw runtime_error("config: duplicate " +
                                     string(route.protocol == Protocol::Udp ? "udp" : "tcp") +
                                     " listen_port " + to_string(route.listen_port) +
                                     " (used by more than one route)");
        }
        // Route names should be unique
        if (!seen_names.insert(route.name).second) {
            throw runtime_error("config: duplicate route name \"" +
                                     route.name + "\"");
        }

        config.routes.push_back(move(route));
    }

    return config;
}


void create_default_config(void) {

    json config = {
        {"heartbeat", {
            {"enabled", true},
            {"interval_seconds", 10}
        }},
        {"dashboard", {
            {"enabled", true},
            {"port", 8080},
            {"log_lines", 100}
        }},
        {"routes", {
            {
                {"backends", {
                    {
                        {"heartbeat", true},
                        {"host", "127.0.0.1"},
                        {"port", 5050}
                    }
                }},
                {"listen_port", 9000},
                {"name", "default_service"}
            }
        }}
    };

    std::string path = DEFAULT_CONFIG_PATH;

    // Extract the directory path from the full file path
    fs::path config_path(path);
    fs::path dir_path = config_path.parent_path();

    // Create directories if they don't exist
    if (!dir_path.empty() && !fs::exists(dir_path)) {
        std::error_code ec;
        if (!fs::create_directories(dir_path, ec)) {
            throw runtime_error("Error: Could not open file " + path + " for writing.\nEnsure you have proper permissions (e.g., run with sudo).");
        }
    }
    std::ofstream file(path);
    if (file.is_open()) {
        file << config.dump(2); // Indented with 2 spaces
        file.close();
    } else {
        throw runtime_error("Error: Could not open file " + path + " for writing.\nEnsure you have proper permissions (e.g., run with sudo).");
    }
}