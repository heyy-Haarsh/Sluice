#pragma once

#include <chrono>
#include <list>
#include <string>
#include <vector>
#include <atomic>

// sluice means floodgate (gate to control a stream or water flow)
// because tihs is a 'gate
// and TCP is 'stream' oriented
const std::string DEFAULT_CONFIG_PATH =  "/etc/sluice/config.json";

using namespace std;

// One blacklist entry: an IP, optionally auto-expiring at an absolute wall-clock
// time (`until`). Absolute (not relative) so the block ends at the same moment
// across restarts. No `until` => permanent.
struct BlacklistEntry {
    string ip;
    bool has_until = false;
    chrono::system_clock::time_point until;  // absolute expiry (config gives local time)
};

// Whitelist + blacklist for one scope (global or per-backend).
struct AccessList {
    vector<string> whitelist;          // non-empty => only these IPs allowed
    vector<BlacklistEntry> blacklist;  // these IPs denied (deny wins over allow)
};

// Rate-limit settings (per-IP connections/minute + exponential-backoff blocks).
struct RateLimitConfig {
    int max_per_minute = 1000;
    int backoff_base_seconds = 60;    // first block duration on violation
    int backoff_max_seconds = 3600;   // cap
};

// Pins clients from `client` (an exact IPv4 or a CIDR range, e.g. "10.0.5.0/24")
// to a specific backend (host:port) within the owning route. If that backend
// isn't currently eligible (disabled/retiring/unhealthy/access-denied), the
// client falls back to the route's normal load-balancing instead.
struct AffinityRule {
    string client;
    string host;
    int port;
};

// One backend server for a route. host/port and the config fields come from the
// config; the atomics are runtime state for load balancing / health / stats.
struct Backend {
    string host;
    int port;
    bool heartbeat_enabled = true;  // config: false = excluded from heartbeats
    bool disabled = false;          // config: true = never selected
    bool proxy_protocol = false;    // config: true = send a PROXY v1 header to this backend
    AccessList access;              // per-backend whitelist/blacklist
    int rate_max_per_minute = 0;    // per-backend per-IP limit (0 = none)

    std::atomic<bool> healthy{true};            // optimistic until proven otherwise
    std::atomic<int> active_connections{0};     // live client pairs using this backend
    std::atomic<uint64_t> bytes_relayed{0};     // total bytes relayed through this backend
    std::atomic<uint64_t> total_connections{0}; // lifetime connections served
    std::atomic<uint64_t> datagrams_forwarded{0};   // UDP: datagrams relayed (both directions)
    std::atomic<uint64_t> datagrams_dropped_rate{0}; // UDP: datagrams dropped by rate-limit bucket
    int probe_fd = -1;              // in-flight heartbeat probe socket, else -1
    chrono::steady_clock::time_point probe_started;  // for tick-based probe timeout
    std::atomic<bool> retiring{false};  // set during hot-reload: drain existing, accept no new
                                        // atomic so the dashboard thread can read it safely
    // UDP passive health only (see gateway.cpp mark_udp_backend_failed /
    // recover_udp_backends): when this backend was last marked unhealthy by a
    // passive ECONNREFUSED. Touched only by the single event-loop thread, same
    // as probe_started, so a plain (non-atomic) time_point is safe.
    chrono::steady_clock::time_point udp_marked_down_at;

    // atomic is non-copyable; provide explicit copy ctor for vector construction
    Backend() = default;
    Backend(const Backend& o)
        : host(o.host), port(o.port), heartbeat_enabled(o.heartbeat_enabled),
          disabled(o.disabled), proxy_protocol(o.proxy_protocol), access(o.access),
          rate_max_per_minute(o.rate_max_per_minute),
          healthy(o.healthy.load()), active_connections(o.active_connections.load()),
          bytes_relayed(o.bytes_relayed.load()),
          total_connections(o.total_connections.load()),
          datagrams_forwarded(o.datagrams_forwarded.load()),
          datagrams_dropped_rate(o.datagrams_dropped_rate.load()),
          probe_fd(o.probe_fd),
          probe_started(o.probe_started),
          retiring(o.retiring.load()),
          udp_marked_down_at(o.udp_marked_down_at) {}
    // Move ctor and move-assign are needed so vector::erase / remove_if can
    // shift elements without invoking the deleted atomic copy-assign.
    Backend(Backend&& o) noexcept
        : host(std::move(o.host)), port(o.port),
          heartbeat_enabled(o.heartbeat_enabled), disabled(o.disabled),
          proxy_protocol(o.proxy_protocol),
          access(std::move(o.access)), rate_max_per_minute(o.rate_max_per_minute),
          healthy(o.healthy.load()), active_connections(o.active_connections.load()),
          bytes_relayed(o.bytes_relayed.load()),
          total_connections(o.total_connections.load()),
          datagrams_forwarded(o.datagrams_forwarded.load()),
          datagrams_dropped_rate(o.datagrams_dropped_rate.load()),
          probe_fd(o.probe_fd), probe_started(o.probe_started),
          retiring(o.retiring.load()),
          udp_marked_down_at(o.udp_marked_down_at) {}
    Backend& operator=(Backend&& o) noexcept {
        if (this == &o) return *this;
        host               = std::move(o.host);
        port               = o.port;
        heartbeat_enabled  = o.heartbeat_enabled;
        disabled           = o.disabled;
        proxy_protocol     = o.proxy_protocol;
        access             = std::move(o.access);
        rate_max_per_minute= o.rate_max_per_minute;
        healthy.store(o.healthy.load());
        active_connections.store(o.active_connections.load());
        bytes_relayed.store(o.bytes_relayed.load());
        total_connections.store(o.total_connections.load());
        datagrams_forwarded.store(o.datagrams_forwarded.load());
        datagrams_dropped_rate.store(o.datagrams_dropped_rate.load());
        probe_fd           = o.probe_fd;
        probe_started      = o.probe_started;
        retiring.store(o.retiring.load());
        udp_marked_down_at = o.udp_marked_down_at;
        return *this;
    }
};

// Transport protocol a route listens on. One route = one protocol; a service
// offered over both TCP and UDP on the same port is two separate routes.
enum class Protocol { Tcp, Udp };

// Route struct to keep track of backend to frontend connections
struct Route {
    string name;
    int listen_port;
    Protocol protocol = Protocol::Tcp;  // transport for this route (TCP default)
    // list (not vector) so that Backend addresses stay stable across
    // insert/erase — Connection, AccessControl and heartbeat probes all hold
    // raw Backend* into here, and a vector would dangle them on reallocation.
    list<Backend> backends;
    size_t next_index = 0;  // round-robin cursor
    bool draining = false;  // set during hot-reload: listen fd closed, connections still alive
    // Idle-connection timeout override for this route. -1 = inherit the global
    // GatewayConfig::idle_timeout_seconds; 0 = explicitly disabled for this route;
    // >0 = this route's own timeout in seconds, regardless of the global value.
    int idle_timeout_seconds = -1;
    // UDP-only: idle timeout for a synthesized client session, in seconds.
    // -1 = inherit the global GatewayConfig::udp_session_timeout_seconds. UDP
    // has no FIN, so this (never 0 — floored at parse) is how sessions end.
    // Unused for TCP routes (those use idle_timeout_seconds above).
    int udp_session_timeout_seconds = -1;
    // UDP-only: per-session datagram rate cap, in datagrams/second, enforced on
    // the client->backend direction only (a token bucket with capacity == rate,
    // so it behaves like a leaky bucket — a steady sustained rate with at most
    // ~1s of burst tolerance). Protects a backend/the gateway from one session
    // being flooded; independent of the session-creation rate_limit above.
    // -1 = inherit GatewayConfig::udp_datagram_rate_limit; 0 = explicitly
    // unlimited for this route regardless of the global value.
    int udp_datagram_rate_limit = -1;
    // UDP-only: maximum accepted datagram payload size in bytes. Datagrams
    // larger than this are silently dropped before forwarding — guards against
    // amplification and enforces protocol-level size contracts (e.g. DNS = 4096,
    // QUIC initial = 1200). 0 = unlimited (default). Only meaningful for UDP routes.
    int max_datagram_bytes = 0;
    // UDP-only: how long (seconds) an ECONNREFUSED-failed backend stays down
    // before being optimistically re-enabled for retry. -1 = inherit the global
    // GatewayConfig::udp_health_cooldown_seconds.
    int udp_health_cooldown_seconds = -1;
    vector<AffinityRule> affinity;  // optional client -> preferred-backend pins
};

// Heartbeat settings (top-level "heartbeat" block in the config)
struct HeartbeatConfig {
    bool enabled = true;
    int interval_seconds = 10;
};

// Dashboard settings
struct DashboardConfig {
    bool enabled = true;
    int port = 8080;
    int log_lines = 200;
    // Interface to bind. Loopback by default so the admin API (which can rewrite
    // the config and trigger reloads) is NOT reachable from the network. Set to
    // "0.0.0.0"/"::" only behind a trusted network or an authenticating proxy.
    string bind = "127.0.0.1";
    // Optional shared secret; if non-empty, mutating API calls (config save,
    // reload) must present it in the X-Auth-Token header.
    string token;
    // Password hash for the dashboard login. If empty or absent, it defaults to the hash for "root".
    string password_hash;
};

// Everything parsed out of the config file
struct GatewayConfig {
    // list (not vector) so Route addresses stay stable across hot-reload
    // insert/erase — listen_fd_to_route holds raw Route* into here.
    list<Route> routes;
    HeartbeatConfig heartbeat;
    DashboardConfig dashboard;
    AccessList access;            // global whitelist/blacklist
    RateLimitConfig rate_limit;   // global per-IP rate limit
    int worker_processes = 1;     // number of worker processes (1 = single process)
    string log_file;              // empty = log to stderr; else a writer process owns this file
    string log_rotation = "none"; // "none" | "hourly" | "daily"
    // Global idle-connection timeout, in seconds. 0 = disabled (default).
    // A connection (client<->backend pair) with no bytes transferred in either
    // direction for this long is closed. Routes may override via
    // Route::idle_timeout_seconds.
    int idle_timeout_seconds = 0;
    // Max simultaneous client<->backend pairs. 0 = unlimited. New clients past
    // the cap are closed immediately, bounding fd + memory use under a flood.
    int max_connections = 10000;
    // UDP: default idle timeout (seconds) for a synthesized client<->backend
    // session when a route doesn't override it (Route::udp_session_timeout_seconds).
    // A non-zero minimum is enforced at parse time so UDP sessions can't leak
    // (there is no FIN to close them).
    int udp_session_timeout_seconds = 30;
    // UDP: cap on simultaneous synthesized sessions. 0 = unlimited. Separate from
    // max_connections because UDP economics differ (one fd per session, no accept).
    // The primary defense against spoofed-source datagram floods.
    int max_udp_sessions = 10000;
    // UDP: default per-session datagram/sec cap (client->backend direction) when
    // a route doesn't override it (Route::udp_datagram_rate_limit). 0 = unlimited
    // (default — preserves old behavior for configs that don't set this).
    int udp_datagram_rate_limit = 0;
    // UDP: how long (seconds) a backend stays down after a passive ECONNREFUSED
    // failure before being optimistically re-enabled. Not configurable per TCP
    // heartbeat interval — UDP recovery runs independently of heartbeat.enabled.
    // Default: 10s. Must be >= 1.
    int udp_health_cooldown_seconds = 10;
};

// Parse the JSON config at `path`.
// Returns the parsed config on success
GatewayConfig parse_config(const string& path);

// Creates default config file
void create_default_config(void);