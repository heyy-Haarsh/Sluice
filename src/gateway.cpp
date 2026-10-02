#include "gateway.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iterator>
#include <list>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "access.hpp"
#include "connection.hpp"
#include "udp_session.hpp"
#include "dashboard.hpp"
#include "drain_snapshot.hpp"
#include "log_buffer.hpp"
#include "log.hpp"

using namespace std;

namespace {

// Max events epoll_wait reports per call (batch size, not a connection limit)
constexpr int MAX_EVENTS = 256;

// Listen backlog for pending connections
constexpr int LISTEN_BACKLOG = 1024;

// Scratch buffer size for recv() and send()
constexpr size_t IO_CHUNK = 65536;

// Per-direction outbound-buffer cap. When a connection's queued-but-unsent
// bytes for one side reach this, we stop reading the other side (drop its
// EPOLLIN) until the buffer drains — backpressure that bounds memory instead of
// letting a fast sender + slow receiver grow the buffer without limit.
constexpr size_t BUFFER_HIGH_WATER = 1u << 20;  // 1 MiB

// UDP backends aren't actively probed (a TCP connect says nothing about a
// datagram service), so a backend marked down passively via ECONNREFUSED can
// only be given another chance by letting real traffic retest it. This default
// grace period applies when neither the global nor route config overrides it.
// Configurable via GatewayConfig::udp_health_cooldown_seconds (global) and
// Route::udp_health_cooldown_seconds (per-route override).
constexpr int UDP_HEALTH_COOLDOWN_DEFAULT = 10;

// Set by the signal handler to request shutdown
volatile sig_atomic_t g_stop = 0;
void on_stop_signal(int) { g_stop = 1; }

// Set by the signal handler to request a config hot-reload (SIGUSR1)
volatile sig_atomic_t g_reload = 0;
void on_reload_signal(int) { g_reload = 1; }

// Shared runtime state, passed to the event-loop helpers instead of globals
struct GatewayState {
    int epoll_fd;
    list<Route> routes;                            // owned, mutable (LB + health state)
    HeartbeatConfig heartbeat;
    AccessControl access;                          // whitelist/blacklist + rate limiting
    int timer_fd = -1;                             // heartbeat timerfd, -1 if disabled
    int idle_timer_fd = -1;                         // idle-connection sweep timerfd
    int idle_timeout_seconds = 0;                   // global default; 0 = disabled
    int max_connections = 0;                        // 0 = unlimited
    string config_path;                            // remembered for hot-reload re-parse
    unordered_map<int, Route*> listen_fd_to_route;
    unordered_map<int, Backend*> probe_fd_to_backend;  // in-flight heartbeat probes
    // Keyed by both client_fd and backend_fd -> same Connection
    unordered_map<int, Connection*> fd_to_connection;
    // Every live Connection*, deduped (fd_to_connection holds each one twice).
    // Used by the idle-timeout sweep and the max_connections check.
    unordered_set<Connection*> live_connections;
    // fds scheduled for teardown after the current epoll batch (deferred cleanup)
    unordered_set<int> pending_close;
    // Routes removed during hot-reload: listen fd closed, existing connections still drain
    list<Route> draining_routes;

    // ---- UDP ---------------------------------------------------------------
    // UDP listen fd -> its route (parallel to listen_fd_to_route for TCP).
    unordered_map<int, Route*> udp_listen_fd_to_route;
    // Backend-side connected UDP socket fd -> the session it belongs to.
    unordered_map<int, UdpSession*> udp_backend_fd_to_session;
    // Session lookup on the client->backend path, keyed by "listenfd|ip|port".
    unordered_map<string, UdpSession*> udp_sessions;
    // Every live UdpSession*, deduped — used by the idle sweep and the cap check.
    unordered_set<UdpSession*> live_udp_sessions;
    int udp_session_timeout_seconds = 30;  // global default (already floored)
    int max_udp_sessions = 0;              // 0 = unlimited
    int udp_datagram_rate_limit = 0;       // global default per-session cap; 0 = unlimited
    int udp_health_cooldown_seconds = UDP_HEALTH_COOLDOWN_DEFAULT; // global default (>= 1)
    // Mutex-guarded snapshot updated by the relay thread after each hot-reload event.
    // Dashboard thread calls drain_snap->read() to get a stable copy.
    DrainSnapshot* drain_snap = nullptr;
    // Guards STRUCTURAL access to `routes` (list nodes) against the dashboard
    // thread. Held by the relay thread only while inserting/erasing routes or
    // backends (apply_reload, connection cleanup), and by the dashboard while it
    // walks the list. Backend atomic fields don't need it (read/written atomically).
    std::mutex routes_mtx;
};

enum class ConnectResult { Connected, InProgress, Failed };

runtime_error errno_error(const string& what) {
    return runtime_error(what + ": " + strerror(errno));
}

bool make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

// ---- epoll helpers ------------------------------------------------------

bool epoll_add(GatewayState& gs, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(gs.epoll_fd, EPOLL_CTL_ADD, fd, &ev) != -1;
}

bool epoll_mod(GatewayState& gs, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(gs.epoll_fd, EPOLL_CTL_MOD, fd, &ev) != -1;
}

// ---- listening sockets --------------------------------------------------

// Create a non-blocking listening socket bound to port
// (socket -> SO_REUSEADDR -> SO_REUSEPORT -> [dual-stack] -> bind -> listen ->
// non-blocking). Prefers an IPv6 socket in dual-stack mode so one listener
// serves both IPv6 and IPv4 clients (the latter arrive as IPv4-mapped
// addresses); falls back to IPv4-only if the host has no IPv6. SO_REUSEPORT
// lets every worker bind the same port and the kernel load-balances across them.
// socktype selects TCP (SOCK_STREAM, default) or UDP (SOCK_DGRAM); a UDP socket
// binds identically but skips listen() (datagram sockets have no accept queue).
int create_listening_socket(int port, int socktype = SOCK_STREAM) {
    bool v6 = true;
    int fd = socket(AF_INET6, socktype, 0);
    if (fd == -1) {
        v6 = false;
        fd = socket(AF_INET, socktype, 0);
        if (fd == -1) {
            throw errno_error("socket()");
        }
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        throw errno_error("setsockopt(SO_REUSEADDR)");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        throw errno_error("setsockopt(SO_REUSEPORT)");
    }
    if (v6) {
        // Best-effort dual-stack. If this fails the socket still serves IPv6;
        // we don't hard-fail (IPv4 clients would then rely on an IPv4 listener).
        int zero = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    }

    sockaddr_storage ss{};
    socklen_t sslen;
    if (v6) {
        sockaddr_in6* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_any;
        a->sin6_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    } else {
        sockaddr_in* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&ss), sslen) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        throw errno_error("bind() on port " + to_string(port));
    }

    if (socktype == SOCK_STREAM && listen(fd, LISTEN_BACKLOG) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        throw errno_error("listen() on port " + to_string(port));
    }

    if (!make_nonblocking(fd)) {
        int e = errno;
        close(fd);
        errno = e;
        throw errno_error("make_nonblocking(listen_fd)");
    }
    return fd;
}

// ---- backend connect ----------------------------------------------------

// Open a non-blocking socket and start connecting to host:port (IPv4 or IPv6
// literal, no DNS). Used for both real client->backend connections and
// heartbeat probes. The socket family is chosen from the host literal.
int open_nonblocking_connect(const string& host, int port, ConnectResult& result) {
    sockaddr_storage ss{};
    socklen_t sslen;
    int family;
    in6_addr a6;
    in_addr a4;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        family = AF_INET6;
        sockaddr_in6* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_addr = a6;
        a->sin6_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    } else if (inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        family = AF_INET;
        sockaddr_in* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr = a4;
        a->sin_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    } else {
        Log() << "backend host \"" << host << "\" is not a valid IPv4 or IPv6 address";
        result = ConnectResult::Failed;
        return -1;
    }

    int fd = socket(family, SOCK_STREAM, 0);
    if (fd == -1) {
        result = ConnectResult::Failed;
        return -1;
    }
    if (!make_nonblocking(fd)) {
        close(fd);
        result = ConnectResult::Failed;
        return -1;
    }

    int rc = connect(fd, reinterpret_cast<sockaddr*>(&ss), sslen);
    if (rc == 0) {
        result = ConnectResult::Connected;
    } else if (errno == EINPROGRESS) {
        result = ConnectResult::InProgress;
    } else {
        result = ConnectResult::Failed;
        close(fd);
        return -1;
    }
    return fd;
}

// Open a *connected* non-blocking UDP socket to host:port (IPv4/IPv6 literal, no
// DNS). connect() on a datagram socket doesn't handshake — it fixes the default
// peer so we can send()/recv() without re-specifying the address, makes the
// kernel demultiplex replies back to this fd by the 4-tuple, and delivers ICMP
// port-unreachable as ECONNREFUSED. Returns the fd, or -1 on failure. The socket
// family is chosen from the host literal. Used per UDP session.
int open_udp_backend_socket(const string& host, int port) {
    sockaddr_storage ss{};
    socklen_t sslen;
    int family;
    in6_addr a6;
    in_addr a4;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        family = AF_INET6;
        sockaddr_in6* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_addr = a6;
        a->sin6_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    } else if (inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        family = AF_INET;
        sockaddr_in* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr = a4;
        a->sin_port = htons(static_cast<uint16_t>(port));
        sslen = sizeof(*a);
    } else {
        Log() << "udp backend host \"" << host << "\" is not a valid IPv4 or IPv6 address";
        return -1;
    }

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (!make_nonblocking(fd)) {
        close(fd);
        return -1;
    }
    // A datagram connect() shouldn't block, but tolerate EINPROGRESS defensively.
    if (connect(fd, reinterpret_cast<sockaddr*>(&ss), sslen) == -1 &&
        errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

// ---- load balancing + health -------------------------------------------

// Log-on-transition health update
void set_health(Backend& b, bool healthy) {
    if (b.healthy != healthy) {
        b.healthy = healthy;
        Log() << "backend " << b.host << ":" << b.port << " is now "
              << (healthy ? "healthy" : "unhealthy");
    }
}

// Passive UDP failure signal (a definitive kernel-level "nobody's listening"
// from ICMP port-unreachable, or a local socket error): mark the backend down
// immediately — same as TCP does on any connect failure, no threshold needed —
// and stamp when, so recover_udp_backends() can give it another chance later.
void mark_udp_backend_failed(Backend* b) {
    if (!b) return;
    set_health(*b, false);
    b->udp_marked_down_at = chrono::steady_clock::now();
}

// Round-robin over the route's eligible backends: enabled, healthy, and not
// blocking this client IP (per-backend whitelist/blacklist/rate). nullptr if none.
Backend* select_backend(Route& route, const string& ip,
                        chrono::steady_clock::time_point now, AccessControl& ac) {
    size_t n = route.backends.size();
    if (n == 0) {
        return nullptr;
    }
    // route.backends is a list (no random access), so walk from the round-robin
    // cursor circularly, tracking the logical offset to update next_index.
    size_t start = route.next_index % n;
    auto it = route.backends.begin();
    std::advance(it, start);
    for (size_t i = 0; i < n; ++i) {
        Backend& b = *it;
        if (!b.disabled && !b.retiring && b.healthy && ac.allowed_backend(&b, ip, now)) {
            route.next_index = (start + i + 1) % n;
            return &b;
        }
        if (++it == route.backends.end()) {
            it = route.backends.begin();
        }
    }
    return nullptr;
}

// Client-IP-pinning: if `ip` matches one of the route's affinity rules (exact
// IP or CIDR), try routing to that rule's backend. Only the FIRST matching
// rule is considered — if its target isn't currently eligible (disabled,
// retiring, unhealthy, or access-denied for this ip), we return nullptr and
// the caller falls back to normal round-robin, rather than trying further
// rules. Also nullptr if no rule matches at all.
Backend* select_affinity_backend(Route& route, const string& ip,
                                 chrono::steady_clock::time_point now, AccessControl& ac) {
    if (route.affinity.empty()) {
        return nullptr;
    }
    array<uint8_t, 16> ip_val;
    if (!parse_ip(ip, ip_val)) {
        return nullptr;
    }
    for (const AffinityRule& rule : route.affinity) {
        bool match;
        CidrRange range;
        if (parse_cidr(rule.client, range)) {
            match = cidr_contains(range, ip_val);
        } else {
            array<uint8_t, 16> rv;
            match = parse_ip(rule.client, rv) && rv == ip_val;
        }
        if (!match) continue;

        for (Backend& b : route.backends) {
            if (b.host != rule.host || b.port != rule.port) continue;
            if (b.disabled || b.retiring || !b.healthy) return nullptr;
            if (!ac.allowed_backend(&b, ip, now)) return nullptr;
            return &b;
        }
        return nullptr;  // rule matched but its target backend isn't (or no longer) in this route
    }
    return nullptr;
}

// ---- teardown -----------------------------------------------------------

// Close both fds, deregister from epoll, drop from map, free. Run after the batch.
void cleanup_connection(GatewayState& gs, Connection* conn) {
    if (conn->backend && conn->backend->active_connections > 0) {
        conn->backend->active_connections--;

        // Hot-reload: if this was the last connection on a retiring backend, erase it
        // from whatever route list still holds it. list::remove_if unlinks only the
        // dead node, so every other Backend* stays valid. Hold routes_mtx so the
        // dashboard thread can't be walking gs.routes during the structural change.
        if (conn->backend->retiring && conn->backend->active_connections == 0) {
            Backend* dead = conn->backend;
            std::lock_guard<std::mutex> lock(gs.routes_mtx);
            auto erase_backend = [dead](list<Route>& routes) {
                for (Route& r : routes) {
                    r.backends.remove_if([dead](const Backend& b) { return &b == dead; });
                }
            };
            erase_backend(gs.routes);
            erase_backend(gs.draining_routes);

            // If a draining route now has no backends at all, remove it too
            gs.draining_routes.remove_if([](const Route& r) { return r.backends.empty(); });
            // Notify the dashboard of the structural change (brief lock; rare event).
            if (gs.drain_snap) gs.drain_snap->update(gs.draining_routes);
        }
    }
    epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, conn->client_fd, nullptr);
    epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, conn->backend_fd, nullptr);
    close(conn->client_fd);
    close(conn->backend_fd);
    gs.fd_to_connection.erase(conn->client_fd);
    gs.fd_to_connection.erase(conn->backend_fd);
    gs.live_connections.erase(conn);
    delete conn;
}

// Schedule both fds for teardown after the current batch (idempotent)
void mark_close(GatewayState& gs, Connection* conn) {
    gs.pending_close.insert(conn->client_fd);
    gs.pending_close.insert(conn->backend_fd);
}

// ---- relay core ---------------------------------------------------------

// The buffer holding bytes destined for dest_fd
deque<char>& buffer_to(Connection* conn, int dest_fd) {
    return (dest_fd == conn->backend_fd) ? conn->client_to_backend_buffer
                                         : conn->backend_to_client_buffer;
}

// Desired epoll interest for a fd: EPOLLIN until its EOF (and only while the
// buffer its reads feed is below the high-water mark — backpressure), EPOLLOUT
// while bytes are queued for it.
uint32_t desired_mask(Connection* conn, int fd) {
    uint32_t m = 0;
    if (fd == conn->client_fd) {
        // client reads are queued into client_to_backend_buffer
        if (!conn->client_eof &&
            conn->client_to_backend_buffer.size() < BUFFER_HIGH_WATER) m |= EPOLLIN;
        if (!conn->backend_to_client_buffer.empty()) m |= EPOLLOUT;
    } else {
        // backend reads are queued into backend_to_client_buffer
        if (!conn->backend_eof &&
            conn->backend_to_client_buffer.size() < BUFFER_HIGH_WATER) m |= EPOLLIN;
        if (!conn->client_to_backend_buffer.empty()) m |= EPOLLOUT;
    }
    return m;
}

// Apply desired_mask() to a fd, issuing a MOD only if it changed
void apply_interest(GatewayState& gs, Connection* conn, int fd) {
    uint32_t desired = desired_mask(conn, fd);
    uint32_t& cur = (fd == conn->client_fd) ? conn->client_events
                                            : conn->backend_events;
    if (desired != cur) {
        cur = desired;
        if (!epoll_mod(gs, fd, desired)) {
            Log() << "route \"" << conn->route_name
                  << "\": epoll_mod failed: " << strerror(errno);
            mark_close(gs, conn);
        }
    }
}

// Once the source side is EOF and its buffer is drained, shutdown writes to dest_fd
void maybe_propagate_shutdown(Connection* conn, int dest_fd) {
    if (dest_fd == conn->backend_fd) {
        if (conn->client_eof && !conn->backend_shut) {
            shutdown(conn->backend_fd, SHUT_WR);
            conn->backend_shut = true;
        }
    } else {
        if (conn->backend_eof && !conn->client_shut) {
            shutdown(conn->client_fd, SHUT_WR);
            conn->client_shut = true;
        }
    }
}

// Both sides EOF and both buffers empty -> close the pair
void maybe_finish(GatewayState& gs, Connection* conn) {
    if (conn->client_eof && conn->backend_eof &&
        conn->client_to_backend_buffer.empty() &&
        conn->backend_to_client_buffer.empty()) {
        mark_close(gs, conn);
    }
}

// Send as much of buf to fd as the kernel accepts, popping the sent prefix.
// Returns false on a hard send error; check buf.empty() for full-drain vs EAGAIN.
bool raw_flush(int fd, deque<char>& buf) {
    char tmp[IO_CHUNK];
    while (!buf.empty()) {
        size_t chunk = min(buf.size(), sizeof(tmp));
        for (size_t k = 0; k < chunk; ++k) {
            tmp[k] = buf[k];
        }
        ssize_t sent = send(fd, tmp, chunk, 0);
        if (sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;  // socket full, leave the rest buffered
            }
            if (errno == EINTR) {
                continue;
            }
            return false;  // ECONNRESET / EPIPE / etc.
        }
        buf.erase(buf.begin(), buf.begin() + sent);
        if (static_cast<size_t>(sent) < chunk) {
            return true;  // kernel buffer full
        }
    }
    return true;
}

// Flush dest_fd's buffer, then update shutdown/interest/finish state
void flush_and_manage(GatewayState& gs, Connection* conn, int dest_fd) {
    deque<char>& buf = buffer_to(conn, dest_fd);
    if (!raw_flush(dest_fd, buf)) {
        Log() << "route \"" << conn->route_name << "\": send to fd=" << dest_fd
              << " failed: " << strerror(errno) << ", tearing down pair";
        mark_close(gs, conn);
        return;
    }
    if (buf.empty()) {
        maybe_propagate_shutdown(conn, dest_fd);
    }
    apply_interest(gs, conn, dest_fd);
    // Draining dest's buffer may have dropped it below the high-water mark, so
    // re-evaluate the source side's interest to release any backpressure hold.
    int src_fd = (dest_fd == conn->backend_fd) ? conn->client_fd : conn->backend_fd;
    apply_interest(gs, conn, src_fd);
    maybe_finish(gs, conn);
}

// Append new bytes to dest_fd's buffer (preserving order) and try to flush
void queue_outbound(GatewayState& gs, Connection* conn, int dest_fd,
                    const char* data, size_t len) {
    if (conn->backend) {
        conn->backend->bytes_relayed.fetch_add(len, std::memory_order_relaxed);
    }
    deque<char>& buf = buffer_to(conn, dest_fd);
    buf.insert(buf.end(), data, data + len);
    flush_and_manage(gs, conn, dest_fd);
}

// One side sent FIN (recv()==0): stop reading it and propagate the half-close
void handle_eof(GatewayState& gs, Connection* conn, int fd) {
    int dest_fd = (fd == conn->client_fd) ? conn->backend_fd : conn->client_fd;
    if (fd == conn->client_fd) {
        conn->client_eof = true;
    } else {
        conn->backend_eof = true;
    }
    Log() << "route \"" << conn->route_name << "\": "
          << (fd == conn->client_fd ? "client" : "backend")
          << " half-closed (fd=" << fd << ")";

    apply_interest(gs, conn, fd);  // drops EPOLLIN on the EOF side

    if (buffer_to(conn, dest_fd).empty()) {
        maybe_propagate_shutdown(conn, dest_fd);
    }
    maybe_finish(gs, conn);
}

// A fd is readable: read one chunk and forward it to the peer
void handle_readable(GatewayState& gs, int fd) {
    auto it = gs.fd_to_connection.find(fd);
    if (it == gs.fd_to_connection.end()) {
        return;
    }
    Connection* conn = it->second;
    int dest_fd = (fd == conn->client_fd) ? conn->backend_fd : conn->client_fd;

    char buf[IO_CHUNK];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);

    if (n > 0) {
        if (conn->idle_timeout_seconds > 0) {
            conn->last_activity = chrono::steady_clock::now();
        }
        queue_outbound(gs, conn, dest_fd, buf, static_cast<size_t>(n));
        return;
    }
    if (n == 0) {
        handle_eof(gs, conn, fd);
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return;
    }
    Log() << "route \"" << conn->route_name << "\": recv on fd=" << fd
          << " failed: " << strerror(errno) << ", tearing down pair";
    mark_close(gs, conn);
}

// A fd is writable: flush its pending outbound buffer
void handle_writable(GatewayState& gs, int fd) {
    auto it = gs.fd_to_connection.find(fd);
    if (it == gs.fd_to_connection.end()) {
        return;
    }
    flush_and_manage(gs, it->second, fd);
}

// ---- accept + connect-completion ----------------------------------------

// Start relaying once the backend is connected: watch both sides for reads.
// backend_fresh: backend fd not yet in epoll (immediate connect) vs already EPOLLOUT (async)
bool start_relay(GatewayState& gs, Connection* conn, bool backend_fresh) {
    conn->client_events = EPOLLIN;
    conn->backend_events = EPOLLIN;
    bool ok = epoll_add(gs, conn->client_fd, EPOLLIN);
    ok = ok && (backend_fresh ? epoll_add(gs, conn->backend_fd, EPOLLIN)
                              : epoll_mod(gs, conn->backend_fd, EPOLLIN));
    return ok;
}

// Extract a printable client IP + port from an accepted peer address.
// A dual-stack listener reports IPv4 clients as IPv4-mapped IPv6 addresses
// (::ffff:a.b.c.d); those are normalized back to plain dotted-quad so access
// control and logs treat them exactly like native IPv4.
void peer_endpoint(const sockaddr_storage& ss, string& ip_out, int& port_out) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET6) {
        const sockaddr_in6* a6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            in_addr a4{};
            memcpy(&a4.s_addr, a6->sin6_addr.s6_addr + 12, 4);
            inet_ntop(AF_INET, &a4, buf, sizeof(buf));
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        }
        port_out = ntohs(a6->sin6_port);
    } else {
        const sockaddr_in* a4 = reinterpret_cast<const sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
        port_out = ntohs(a4->sin_port);
    }
    ip_out = buf;
}

// Build a PROXY protocol v1 header line for a new client<->backend pair.
// src = the real client (accept peer); dst = the gateway address the client
// connected to (getsockname on the client fd). Family is TCP4/TCP6 from the
// normalized addresses; if either address is unavailable or the families
// disagree we emit the spec's "PROXY UNKNOWN" line, which a v1-aware backend
// accepts without address info.
string build_proxy_header(int client_fd, const sockaddr_storage& peer) {
    string src_ip;
    int src_port = 0;
    peer_endpoint(peer, src_ip, src_port);

    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    string dst_ip;
    int dst_port = 0;
    if (getsockname(client_fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
        peer_endpoint(local, dst_ip, dst_port);
    }

    bool src_v6 = src_ip.find(':') != string::npos;
    bool dst_v6 = dst_ip.find(':') != string::npos;
    if (src_ip.empty() || dst_ip.empty() || src_v6 != dst_v6) {
        return "PROXY UNKNOWN\r\n";
    }
    return string("PROXY ") + (src_v6 ? "TCP6" : "TCP4") + " " + src_ip + " " +
           dst_ip + " " + to_string(src_port) + " " + to_string(dst_port) + "\r\n";
}

// Build a PROXY Protocol v2 binary header for a single UDP datagram.
//
// PROXY v2 is the only version that can carry UDP traffic — v1 is text-only and
// has no datagram transport type. The binary layout is:
//
//   12 bytes  magic signature  "\r\n\r\n\0\r\nQUIT\n"
//    1 byte   version+command  0x21  (version=2, command=PROXY)
//    1 byte   family+protocol  0x11=TCP4  0x12=UDP4  0x21=TCP6  0x22=UDP6
//    2 bytes  length of rest   (big-endian)
//   [address block]            src_addr dst_addr src_port dst_port
//
// The src address is the real client; dst is the gateway's listen address
// (getsockname on the listen fd — may be 0.0.0.0 on a wildcard listener, which
// is accepted by all v2-aware backends as a valid "don't-know" destination).
//
// Writes into caller-supplied out[28] (28 bytes covers the largest IPv6 case).
// Returns the number of bytes written (16 for IPv4, 28 for IPv6), or 0 on
// failure (caller will fall back to sending without a header).
size_t build_proxy_v2_udp_header(const sockaddr_storage& client_addr,
                                  int listen_fd,
                                  uint8_t out[52]) {
    // 12-byte PROXY v2 signature (mandated by the spec)
    static const uint8_t SIG[12] = {
        0x0D, 0x0A, 0x0D, 0x0A,   // \r\n\r\n
        0x00, 0x0D, 0x0A, 0x51,   // \0\r\nQ
        0x55, 0x49, 0x54, 0x0A    // UIT\n
    };
    memcpy(out, SIG, 12);
    out[12] = 0x21; // version=2, command=PROXY

    // Get the gateway's local address for the destination field.
    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&local), &local_len);
    // getsockname failure leaves local zero-initialized — the resulting header
    // will contain 0.0.0.0 / [::] as the destination, which is spec-legal.

    if (client_addr.ss_family == AF_INET) {
        // ---- IPv4 path: 4+4+2+2 = 12 address bytes; total header = 16 bytes ----
        out[13] = 0x12; // AF_INET (1) + DGRAM (2)
        out[14] = 0;    // length high byte
        out[15] = 12;   // length low byte  (12 address bytes follow)

        const sockaddr_in* src = reinterpret_cast<const sockaddr_in*>(&client_addr);
        const sockaddr_in* dst = reinterpret_cast<const sockaddr_in*>(&local);

        // src IPv4
        memcpy(out + 16, &src->sin_addr,  4);
        // dst IPv4 (may be 0.0.0.0 on wildcard listener, or unknown family → zero)
        if (dst->sin_family == AF_INET) {
            memcpy(out + 20, &dst->sin_addr.s_addr, 4);
        } else {
            uint32_t zero = 0;
            memcpy(out + 20, &zero, 4);
        }
        // ports (big-endian — already in network byte order)
        memcpy(out + 24, &src->sin_port, 2);
        uint16_t dst_port = (dst->sin_family == AF_INET) ? dst->sin_port : 0;
        memcpy(out + 26, &dst_port, 2);
        return 16; // 4 (ver/cmd/family/len) + 12 (addresses) = 16
    }

    if (client_addr.ss_family == AF_INET6) {
        // ---- IPv6 path: 16+16+2+2 = 36 address bytes; total header = 52 bytes ----
        out[13] = 0x22; // AF_INET6 (2) + DGRAM (2)
        out[14] = 0;    // length high byte
        out[15] = 36;   // length low byte  (36 address bytes follow)

        const sockaddr_in6* src = reinterpret_cast<const sockaddr_in6*>(&client_addr);
        const sockaddr_in6* dst = reinterpret_cast<const sockaddr_in6*>(&local);

        memcpy(out + 16, &src->sin6_addr, 16);
        if (dst->sin6_family == AF_INET6) {
            memcpy(out + 32, &dst->sin6_addr, 16);
        } else {
            memset(out + 32, 0, 16); // wildcard
        }
        memcpy(out + 48, &src->sin6_port, 2);
        uint16_t dst_port = (dst->sin6_family == AF_INET6) ? dst->sin6_port : 0;
        memcpy(out + 50, &dst_port, 2);
        return 52;
    }

    return 0; // unknown family — caller sends without a header
}

// Prepend conn->proxy_header to the backend-bound buffer and flush it. Called
// exactly once, the moment the backend connection is established and relaying
// starts — before any client bytes have been read, so the header is guaranteed
// to lead the backend stream. A partial write is handled by the normal
// buffer/EPOLLOUT machinery; a hard error marks the pair for cleanup.
void send_proxy_header(GatewayState& gs, Connection* conn) {
    if (conn->proxy_header.empty()) {
        return;
    }
    deque<char>& buf = conn->client_to_backend_buffer;
    buf.insert(buf.begin(), conn->proxy_header.begin(), conn->proxy_header.end());
    conn->proxy_header.clear();
    flush_and_manage(gs, conn, conn->backend_fd);
}

void handle_new_clients(GatewayState& gs, int listen_fd, Route& route) {
    while (true) {
        sockaddr_storage peer{};
        socklen_t peer_len = sizeof(peer);
        int client_fd =
            accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            Log() << "accept() on route \"" << route.name
                  << "\" failed: " << strerror(errno);
            break;
        }

        string client_ip;
        int client_port = 0;
        peer_endpoint(peer, client_ip, client_port);

        // Connection cap: reject early (before any per-connection work) so a
        // flood can't exhaust fds or memory. live_connections holds one entry
        // per active client<->backend pair.
        if (gs.max_connections > 0 &&
            gs.live_connections.size() >= static_cast<size_t>(gs.max_connections)) {
            Log() << "connection limit (" << gs.max_connections
                  << ") reached — dropping client " << client_ip;
            close(client_fd);
            continue;
        }

        if (!make_nonblocking(client_fd)) {
            Log() << "make_nonblocking(client_fd) failed for route \""
                  << route.name << "\": " << strerror(errno);
            close(client_fd);
            continue;
        }

        auto now = chrono::steady_clock::now();

        // Global access control: whitelist/blacklist + dynamic rate-limit blocks
        if (!gs.access.allowed_global(client_ip, now)) {
            Log() << "blocked client " << client_ip << " on route \"" << route.name
                  << "\" (global access/rate)";
            close(client_fd);
            continue;
        }
        gs.access.record_global(client_ip, now);

        // Load balancing: an affinity rule match takes priority (falling back
        // to normal round-robin if its target isn't currently eligible), else
        // pick an enabled, healthy, access-permitted backend as usual.
        Backend* backend = select_affinity_backend(route, client_ip, now, gs.access);
        bool via_affinity = (backend != nullptr);
        if (!backend) {
            backend = select_backend(route, client_ip, now, gs.access);
        }
        if (!backend) {
            Log() << "no eligible backend for client " << client_ip
                  << " on route \"" << route.name << "\" — dropping";
            close(client_fd);
            continue;
        }
        gs.access.record_backend(backend, client_ip, now);
        if (via_affinity) {
            Log() << "route \"" << route.name << "\": client " << client_ip
                  << " pinned by affinity rule -> " << backend->host << ":" << backend->port;
        }

        ConnectResult result;
        int backend_fd = open_nonblocking_connect(backend->host, backend->port, result);
        if (result == ConnectResult::Failed) {
            set_health(*backend, false);  // passive marking
            Log() << "backend connect failed for route \"" << route.name
                  << "\" (" << backend->host << ":" << backend->port
                  << "): " << strerror(errno) << " — dropping client fd="
                  << client_fd;
            close(client_fd);
            continue;
        }

        Connection* conn = new Connection{};
        conn->client_fd        = client_fd;
        conn->backend_fd       = backend_fd;
        conn->backend_connected = (result == ConnectResult::Connected);
        conn->route_name       = route.name;
        conn->backend = backend;
        conn->idle_timeout_seconds =
            route.idle_timeout_seconds >= 0 ? route.idle_timeout_seconds
                                             : gs.idle_timeout_seconds;
        conn->last_activity = now;
        if (backend->proxy_protocol) {
            conn->proxy_header = build_proxy_header(client_fd, peer);
        }
        backend->active_connections++;
        backend->total_connections++;
        gs.fd_to_connection[client_fd] = conn;
        gs.fd_to_connection[backend_fd] = conn;
        gs.live_connections.insert(conn);

        Log() << "accepted client fd=" << client_fd << " from "
              << (client_ip.find(':') != string::npos ? "[" + client_ip + "]" : client_ip)
              << ":" << client_port << " on route \"" << route.name
              << "\" -> " << backend->host << ":" << backend->port
              << " (backend fd=" << backend_fd << ")";

        if (result == ConnectResult::Connected) {
            set_health(*backend, true);  // passive marking
            Log() << "backend connected immediately for route \"" << route.name
                  << "\" (fd=" << backend_fd << ")";
            if (!start_relay(gs, conn, /*backend_fresh=*/true)) {
                Log() << "epoll registration for relay failed: "
                      << strerror(errno);
                cleanup_connection(gs, conn);
            } else {
                send_proxy_header(gs, conn);
            }
        } else {
            conn->backend_events = EPOLLOUT;
            if (!epoll_add(gs, backend_fd, EPOLLOUT)) {
                Log() << "epoll_ctl(ADD backend_fd) failed: " << strerror(errno);
                cleanup_connection(gs, conn);
            }
        }
    }
}

void handle_backend_connect_result(GatewayState& gs, int backend_fd) {
    auto it = gs.fd_to_connection.find(backend_fd);
    if (it == gs.fd_to_connection.end()) {
        return;
    }
    Connection* conn = it->second;

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(backend_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == -1) {
        Log() << "getsockopt(SO_ERROR) failed for route \"" << conn->route_name
              << "\": " << strerror(errno);
        mark_close(gs, conn);
        return;
    }
    if (so_error != 0) {
        if (conn->backend) set_health(*conn->backend, false);  // passive marking
        Log() << "backend connection failed for route \"" << conn->route_name
              << "\": " << strerror(so_error) << " — closing client fd="
              << conn->client_fd;
        mark_close(gs, conn);
        return;
    }

    conn->backend_connected = true;
    if (conn->backend) set_health(*conn->backend, true);  // passive marking
    Log() << "backend connected for route \"" << conn->route_name
          << "\" (fd=" << backend_fd << ")";

    if (!start_relay(gs, conn, /*backend_fresh=*/false)) {
        Log() << "epoll registration for relay failed for route \""
              << conn->route_name << "\": " << strerror(errno);
        mark_close(gs, conn);
    } else {
        send_proxy_header(gs, conn);
    }
}

// ---- UDP relay ----------------------------------------------------------

// Session lookup key: the listen fd disambiguates identical client endpoints
// arriving on different UDP routes; ip+port identify the client flow.
string udp_key(int listen_fd, const string& ip, int port) {
    return to_string(listen_fd) + "|" + ip + "|" + to_string(port);
}

// Tear down a UDP session: drop its backend fd from epoll, close it, unlink from
// every map, decrement the backend's live-session count, and free it. Safe to
// call inline from the event loop — each fd surfaces at most once per epoll
// batch, so a just-closed backend fd can't collide with an already-dequeued
// event.
void destroy_udp_session(GatewayState& gs, UdpSession* s) {
    if (s->backend && s->backend->active_connections > 0) {
        s->backend->active_connections--;

        // Hot-reload: if this was the last session on a retiring backend, erase
        // it from whatever route list still holds it. Mirrors cleanup_connection
        // — list::remove_if unlinks only the dead node so every other Backend*
        // stays valid, and routes_mtx keeps the dashboard thread off gs.routes
        // during the structural change. s->backend dangles after this; we don't
        // dereference it again below (only the stored fd/key are used).
        if (s->backend->retiring && s->backend->active_connections == 0) {
            Backend* dead = s->backend;
            std::lock_guard<std::mutex> lock(gs.routes_mtx);
            auto erase_backend = [dead](list<Route>& routes) {
                for (Route& r : routes) {
                    r.backends.remove_if([dead](const Backend& b) { return &b == dead; });
                }
            };
            erase_backend(gs.routes);
            erase_backend(gs.draining_routes);
            gs.draining_routes.remove_if([](const Route& r) { return r.backends.empty(); });
            if (gs.drain_snap) gs.drain_snap->update(gs.draining_routes);
        }
    }
    epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, s->backend_fd, nullptr);
    close(s->backend_fd);
    gs.udp_backend_fd_to_session.erase(s->backend_fd);
    gs.udp_sessions.erase(udp_key(s->client_listen_fd, s->client_ip, s->client_port));
    gs.live_udp_sessions.erase(s);
    delete s;
}

// Token-bucket check for one outbound (client->backend) datagram on a session.
// Lazily refills based on elapsed time, capped at rate_limit_per_second tokens
// (capacity == rate, so this behaves like a leaky bucket: a steady sustained
// rate with at most ~1s of burst). Returns false — and the datagram must be
// dropped — if no token is available. rate_limit_per_second == 0 means
// unlimited (always allowed, no bucket accounting).
bool udp_consume_token(UdpSession* s, chrono::steady_clock::time_point now) {
    if (s->rate_limit_per_second <= 0) {
        return true;
    }
    double elapsed = chrono::duration<double>(now - s->bucket_updated).count();
    s->bucket_updated = now;
    s->tokens = min<double>(s->rate_limit_per_second,
                            s->tokens + elapsed * s->rate_limit_per_second);
    if (s->tokens < 1.0) {
        return false;
    }
    s->tokens -= 1.0;
    return true;
}

// A UDP listen fd is readable: drain every queued datagram and relay each to its
// session's backend, creating the session on first sight of a client endpoint.
void handle_udp_client_datagrams(GatewayState& gs, int listen_fd, Route& route) {
    char buf[IO_CHUNK];
    while (true) {
        sockaddr_storage client{};
        socklen_t clen = sizeof(client);
        ssize_t n = recvfrom(listen_fd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&client), &clen);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            Log() << "udp recvfrom on route \"" << route.name
                  << "\" failed: " << strerror(errno);
            break;
        }
        // n == 0 is a legitimate empty datagram — relay it like any other.
        // buf is 65536, larger than the max UDP payload (65507), so a datagram
        // is never truncated.

        string client_ip;
        int client_port = 0;
        peer_endpoint(client, client_ip, client_port);
        auto now = chrono::steady_clock::now();

        string key = udp_key(listen_fd, client_ip, client_port);
        auto sit = gs.udp_sessions.find(key);
        UdpSession* s = (sit != gs.udp_sessions.end()) ? sit->second : nullptr;

        if (!s) {
            // ---- new session: same policy sequence as handle_new_clients ----

            // Session cap first — the primary defense against spoofed-source
            // floods (drop the datagram before doing any per-session work).
            if (gs.max_udp_sessions > 0 &&
                gs.live_udp_sessions.size() >= static_cast<size_t>(gs.max_udp_sessions)) {
                Log() << "udp session limit (" << gs.max_udp_sessions
                      << ") reached — dropping datagram from " << client_ip;
                continue;
            }

            // Global access control: whitelist/blacklist + rate-limit blocks.
            if (!gs.access.allowed_global(client_ip, now)) {
                Log() << "blocked udp client " << client_ip << " on route \""
                      << route.name << "\" (global access/rate)";
                continue;
            }
            gs.access.record_global(client_ip, now);

            // Load balancing: affinity pin first, else round-robin.
            Backend* backend = select_affinity_backend(route, client_ip, now, gs.access);
            bool via_affinity = (backend != nullptr);
            if (!backend) {
                backend = select_backend(route, client_ip, now, gs.access);
            }
            if (!backend) {
                Log() << "no eligible backend for udp client " << client_ip
                      << " on route \"" << route.name << "\" — dropping datagram";
                continue;
            }
            gs.access.record_backend(backend, client_ip, now);

            int backend_fd = open_udp_backend_socket(backend->host, backend->port);
            if (backend_fd == -1) {
                Log() << "udp backend socket to " << backend->host << ":" << backend->port
                      << " failed for route \"" << route.name
                      << "\": " << strerror(errno) << " — dropping datagram";
                mark_udp_backend_failed(backend);
                continue;
            }

            s = new UdpSession{};
            s->client_addr      = client;
            s->client_len       = clen;
            s->client_ip        = client_ip;
            s->client_port      = client_port;
            s->client_listen_fd = listen_fd;
            s->backend_fd       = backend_fd;
            s->backend          = backend;
            s->route_name       = route.name;
            s->idle_timeout_seconds =
                route.udp_session_timeout_seconds >= 0 ? route.udp_session_timeout_seconds
                                                       : gs.udp_session_timeout_seconds;
            s->last_activity    = now;
            s->rate_limit_per_second =
                route.udp_datagram_rate_limit >= 0 ? route.udp_datagram_rate_limit
                                                   : gs.udp_datagram_rate_limit;
            s->tokens            = s->rate_limit_per_second;  // start full: allow an immediate burst up to the cap
            s->bucket_updated    = now;
            // Inherit PROXY v2 preference from the chosen backend.
            // When true, every datagram sent to this backend is prefixed with
            // a binary PROXY v2 header carrying the real client source address.
            s->proxy_protocol_v2 = backend->proxy_protocol;

            if (!epoll_add(gs, backend_fd, EPOLLIN | EPOLLERR)) {
                Log() << "udp epoll_add(backend_fd) failed for route \"" << route.name
                      << "\": " << strerror(errno) << " — dropping datagram";
                close(backend_fd);
                delete s;
                continue;
            }
            gs.udp_sessions[key] = s;
            gs.udp_backend_fd_to_session[backend_fd] = s;
            gs.live_udp_sessions.insert(s);
            backend->active_connections++;
            backend->total_connections++;

            Log() << "udp session "
                  << (client_ip.find(':') != string::npos ? "[" + client_ip + "]" : client_ip)
                  << ":" << client_port << " on route \"" << route.name << "\" -> "
                  << backend->host << ":" << backend->port
                  << " (backend fd=" << backend_fd << ")"
                  << (via_affinity ? " [affinity]" : "");
        }

        // ---- per-session rate limit: cap client->backend throughput ----
        // Protects the backend (and the gateway) from one session being flooded
        // by its own client, independent of the session-creation rate_limit
        // above. Silently drops like any other UDP loss; logs once per session,
        // then re-enables logging once the bucket refills (edge-trigger reset).
        if (!udp_consume_token(s, now)) {
            s->datagrams_dropped_rate++;
            if (s->backend) {
                s->backend->datagrams_dropped_rate.fetch_add(1, std::memory_order_relaxed);
            }
            if (!s->rate_limit_logged) {
                Log() << "udp session "
                      << (s->client_ip.find(':') != string::npos ? "[" + s->client_ip + "]" : s->client_ip)
                      << ":" << s->client_port << " on route \"" << route.name
                      << "\": rate limit (" << s->rate_limit_per_second
                      << "/s) exceeded — throttling";
                s->rate_limit_logged = true;
            }
            continue;
        }
        // Bucket has tokens: reset the edge-trigger so the NEXT throttle episode
        // is also logged.
        s->rate_limit_logged = false;

        // ---- size guard: drop oversized datagrams before forwarding ----
        // Protects backends from amplification and enforces protocol size contracts
        // (e.g. DNS=4096, QUIC initial=1200). 0 = unlimited.
        if (route.max_datagram_bytes > 0 && n > static_cast<ssize_t>(route.max_datagram_bytes)) {
            Log() << "udp route \"" << route.name << "\": datagram from "
                  << (client_ip.find(':') != string::npos ? "[" + client_ip + "]" : client_ip)
                  << ":" << client_port << " exceeds max_datagram_bytes ("
                  << n << " > " << route.max_datagram_bytes << ") — dropped";
            continue;
        }

        // ---- forward this datagram to the backend ----
        //
        // When PROXY v2 is enabled for this session's backend, prepend a binary
        // PROXY v2 header to every datagram so the backend can recover the real
        // client IP. We use sendmsg() with two iovec entries (header + payload)
        // so the kernel assembles them into one datagram without any memcpy.
        ssize_t sent;
        if (s->proxy_protocol_v2) {
            // Stack-allocate 52 bytes — enough for the largest case (IPv6 header).
            // build_proxy_v2_udp_header() returns the actual length used.
            uint8_t hdr[52];
            size_t hlen = build_proxy_v2_udp_header(s->client_addr, s->client_listen_fd, hdr);
            if (hlen == 0) {
                // Unknown address family — send raw rather than silently drop.
                sent = send(s->backend_fd, buf, static_cast<size_t>(n), 0);
            } else {
                // Two-part scatter: [proxy_v2_header][datagram_payload]
                iovec iov[2];
                iov[0].iov_base = hdr;
                iov[0].iov_len  = hlen;
                iov[1].iov_base = buf;
                iov[1].iov_len  = static_cast<size_t>(n);
                msghdr msg{};
                msg.msg_iov    = iov;
                msg.msg_iovlen = 2;
                sent = sendmsg(s->backend_fd, &msg, 0);
            }
        } else {
            sent = send(s->backend_fd, buf, static_cast<size_t>(n), 0);
        }
        if (sent == -1) {
            if (errno == ECONNREFUSED) {
                // A prior datagram's ICMP port-unreachable surfaced here.
                Log() << "udp backend " << s->backend->host << ":" << s->backend->port
                      << " refused (route \"" << route.name << "\") — ending session";
                mark_udp_backend_failed(s->backend);
                destroy_udp_session(gs, s);
                continue;
            }
            // EAGAIN or a transient error: drop this datagram, keep the session.
            continue;
        }
        s->datagrams_sent++;
        s->last_activity = now;
        if (s->backend) {
            s->backend->bytes_relayed.fetch_add(static_cast<uint64_t>(n),
                                                std::memory_order_relaxed);
            s->backend->datagrams_forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// A UDP backend fd is readable: drain replies and send each back to the client.
// Replies always go out the *listen* socket to the stored raw client address —
// never out the backend socket — so the client sees them coming from the gateway.
void handle_udp_backend_reply(GatewayState& gs, int backend_fd) {
    auto it = gs.udp_backend_fd_to_session.find(backend_fd);
    if (it == gs.udp_backend_fd_to_session.end()) {
        return;
    }
    UdpSession* s = it->second;
    char buf[IO_CHUNK];
    while (true) {
        ssize_t n = recv(backend_fd, buf, sizeof(buf), 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECONNREFUSED) {
                Log() << "udp backend " << s->backend->host << ":" << s->backend->port
                      << " refused (route \"" << s->route_name << "\") — ending session";
            } else {
                Log() << "udp recv from backend fd=" << backend_fd
                      << " failed: " << strerror(errno) << " — ending session";
            }
            mark_udp_backend_failed(s->backend);
            destroy_udp_session(gs, s);
            return;
        }
        // A reply is passive proof of life — promote the backend back to
        // healthy (no-op if it was already healthy; set_health only logs on
        // an actual transition).
        if (s->backend) set_health(*s->backend, true);
        ssize_t sent = sendto(s->client_listen_fd, buf, static_cast<size_t>(n), 0,
                              reinterpret_cast<sockaddr*>(&s->client_addr), s->client_len);
        if (sent == -1) {
            Log() << "udp sendto client "
                  << (s->client_ip.find(':') != string::npos ? "[" + s->client_ip + "]" : s->client_ip)
                  << ":" << s->client_port << " failed: " << strerror(errno);
        }
        s->datagrams_received++;
        s->last_activity = chrono::steady_clock::now();
        if (s->backend) {
            s->backend->bytes_relayed.fetch_add(static_cast<uint64_t>(n),
                                                std::memory_order_relaxed);
            s->backend->datagrams_forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---- heartbeats ---------------------------------------------------------

// Tear down an in-flight probe socket
void close_probe(GatewayState& gs, Backend& b) {
    if (b.probe_fd == -1) {
        return;
    }
    epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, b.probe_fd, nullptr);
    gs.probe_fd_to_backend.erase(b.probe_fd);
    close(b.probe_fd);
    b.probe_fd = -1;
}

// Fire off one non-blocking probe connection to an idle backend
void launch_probe(GatewayState& gs, Backend& b) {
    ConnectResult result;
    int fd = open_nonblocking_connect(b.host, b.port, result);
    if (result == ConnectResult::Failed) {
        set_health(b, false);
        return;
    }
    if (result == ConnectResult::Connected) {
        set_health(b, true);  // handshake done instantly, close and done
        close(fd);
        return;
    }
    // InProgress: wait for EPOLLOUT to learn the outcome
    b.probe_fd = fd;
    b.probe_started = chrono::steady_clock::now();
    gs.probe_fd_to_backend[fd] = &b;
    if (!epoll_add(gs, fd, EPOLLOUT)) {
        set_health(b, false);
        close_probe(gs, b);
    }
}

// timerfd tick: expire hung probes, then probe every eligible idle backend.
// Runs after the event batch (see run_gateway), so no probe fd it closes can
// collide with an fd reused earlier in the same batch.
void run_heartbeats(GatewayState& gs) {
    auto now = chrono::steady_clock::now();
    auto timeout = chrono::seconds(gs.heartbeat.interval_seconds);
    for (Route& route : gs.routes) {
        // UDP backends get no active TCP probe — a successful connect() says
        // nothing about a datagram service. They stay optimistically healthy
        // (passive failure handling is a later phase).
        if (route.protocol == Protocol::Udp) continue;
        for (Backend& b : route.backends) {
            if (b.disabled) continue;              // disabled backend: never probe
            if (b.probe_fd != -1 && now - b.probe_started > timeout) {
                set_health(b, false);  // probe never completed -> treat as down
                close_probe(gs, b);
            }
            if (!b.heartbeat_enabled) continue;    // excluded
            if (b.active_connections > 0) continue; // busy = assumed alive
            if (b.probe_fd != -1) continue;         // probe already running
            launch_probe(gs, b);
        }
    }
}

// A probe fd is writable: the connect finished; SO_ERROR says if it succeeded
void handle_probe_result(GatewayState& gs, int fd) {
    auto it = gs.probe_fd_to_backend.find(fd);
    if (it == gs.probe_fd_to_backend.end()) {
        return;
    }
    Backend* b = it->second;
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    bool ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
              so_error == 0;
    set_health(*b, ok);
    close_probe(gs, *b);
}

// ---- idle timeout ---------------------------------------------------------

// idle_timer_fd tick: close any connection whose idle_timeout_seconds is set
// (>0) and has had no bytes read on either side for that long. Runs after the
// event batch alongside heartbeats, before the deferred-cleanup pass, so
// mark_close() here is guaranteed to be cleaned up in the same iteration.
void check_idle_connections(GatewayState& gs) {
    auto now = chrono::steady_clock::now();
    for (Connection* conn : gs.live_connections) {
        if (conn->idle_timeout_seconds <= 0) continue;
        auto idle = chrono::duration_cast<chrono::seconds>(now - conn->last_activity).count();
        if (idle >= conn->idle_timeout_seconds) {
            Log() << "route \"" << conn->route_name << "\": idle timeout ("
                  << conn->idle_timeout_seconds << "s) exceeded (fd="
                  << conn->client_fd << "), closing";
            mark_close(gs, conn);
        }
    }

    // UDP sessions have no FIN, so the idle sweep is the ONLY way they end.
    // Collect victims first: destroy_udp_session mutates live_udp_sessions (and
    // may erase a drained backend), so we can't tear down mid-iteration.
    vector<UdpSession*> expired;
    for (UdpSession* s : gs.live_udp_sessions) {
        if (s->idle_timeout_seconds <= 0) continue;  // always >0 for UDP; defensive
        auto idle = chrono::duration_cast<chrono::seconds>(now - s->last_activity).count();
        if (idle >= s->idle_timeout_seconds) {
            expired.push_back(s);
        }
    }
    for (UdpSession* s : expired) {
        Log() << "udp session "
              << (s->client_ip.find(':') != string::npos ? "[" + s->client_ip + "]" : s->client_ip)
              << ":" << s->client_port << " on route \"" << s->route_name
              << "\": idle timeout (" << s->idle_timeout_seconds << "s) exceeded, closing";
        destroy_udp_session(gs, s);
    }
}

// idle_timer_fd tick (same 1s cadence as check_idle_connections, called
// alongside it): give unhealthy UDP backends another chance once their cooldown
// elapses, by optimistically flipping them back to healthy so the next new
// session may route to them and test them for real. Runs unconditionally
// (the idle timer always exists), independent of heartbeat.enabled — UDP
// recovery isn't active probing, so it doesn't depend on that TCP-only feature.
// The cooldown duration is per-route (udp_health_cooldown_seconds, defaulting
// to the global gs.udp_health_cooldown_seconds).
void recover_udp_backends(GatewayState& gs) {
    auto now = chrono::steady_clock::now();
    for (Route& route : gs.routes) {
        if (route.protocol != Protocol::Udp) continue;
        int cooldown = route.udp_health_cooldown_seconds >= 1
                           ? route.udp_health_cooldown_seconds
                           : gs.udp_health_cooldown_seconds;
        for (Backend& b : route.backends) {
            if (b.healthy || b.disabled || b.retiring) continue;
            if (now - b.udp_marked_down_at >= chrono::seconds(cooldown)) {
                b.healthy = true;  // optimistic — unproven until the next session's traffic confirms it
                Log() << "udp backend " << b.host << ":" << b.port
                      << " cooldown elapsed — eligible for retry";
            }
        }
    }
}

// ---- shutdown -----------------------------------------------------------

void shutdown_all(GatewayState& gs) {
    set<Connection*> conns;
    for (auto& kv : gs.fd_to_connection) {
        conns.insert(kv.second);
    }
    for (Connection* conn : conns) {
        close(conn->client_fd);
        close(conn->backend_fd);
        delete conn;
    }
    gs.fd_to_connection.clear();
    gs.live_connections.clear();

    for (auto& kv : gs.probe_fd_to_backend) {
        close(kv.first);
    }
    gs.probe_fd_to_backend.clear();

    // UDP: close each session's backend fd (one per session) and free it, then
    // close the UDP listen fds.
    for (auto& kv : gs.udp_backend_fd_to_session) {
        close(kv.first);
        delete kv.second;
    }
    gs.udp_backend_fd_to_session.clear();
    gs.udp_sessions.clear();
    gs.live_udp_sessions.clear();
    for (auto& kv : gs.udp_listen_fd_to_route) {
        close(kv.first);
    }
    gs.udp_listen_fd_to_route.clear();

    if (gs.timer_fd != -1) {
        close(gs.timer_fd);
    }
    if (gs.idle_timer_fd != -1) {
        close(gs.idle_timer_fd);
    }
    for (auto& kv : gs.listen_fd_to_route) {
        close(kv.first);
    }
    close(gs.epoll_fd);
}

// ---- hot-reload ----------------------------------------------------------

// Stop accepting new connections on a route.
//
// TCP: close the listen fd, mark backends retiring, and move the route to
// draining_routes so already-open connections finish gracefully (their Backend*
// stays valid — a list move keeps node addresses stable).
//
// UDP: a session can't drain once its listen fd is gone — replies exit that fd —
// and UDP is connectionless anyway, so clients simply re-establish on their next
// datagram against the new config. We therefore force-close every session on the
// route's listen fd and don't move anything to draining_routes.
//
// Called only from apply_reload while holding routes_mtx. The UDP force-close
// runs while backends are NOT yet marked retiring, so destroy_udp_session takes
// its non-locking path (it must — routes_mtx is already held here).
void retire_route(GatewayState& gs, Route& route) {
    if (route.protocol == Protocol::Udp) {
        vector<int> udp_fds;
        for (auto it = gs.udp_listen_fd_to_route.begin(); it != gs.udp_listen_fd_to_route.end(); ) {
            if (it->second == &route) {
                udp_fds.push_back(it->first);
                it = gs.udp_listen_fd_to_route.erase(it);
            } else {
                ++it;
            }
        }
        vector<UdpSession*> victims;
        for (UdpSession* s : gs.live_udp_sessions) {
            for (int fd : udp_fds) {
                if (s->client_listen_fd == fd) { victims.push_back(s); break; }
            }
        }
        for (UdpSession* s : victims) {
            destroy_udp_session(gs, s);
        }
        for (int fd : udp_fds) {
            epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
        }
        Log() << "hot-reload: udp route \"" << route.name << "\" removed — closed "
              << udp_fds.size() << " listener(s), dropped " << victims.size()
              << " session(s)";
        return;
    }

    // Remove listen fd from epoll and close it so no new clients arrive.
    for (auto it = gs.listen_fd_to_route.begin(); it != gs.listen_fd_to_route.end(); ) {
        if (it->second == &route) {
            epoll_ctl(gs.epoll_fd, EPOLL_CTL_DEL, it->first, nullptr);
            close(it->first);
            it = gs.listen_fd_to_route.erase(it);
        } else {
            ++it;
        }
    }
    // Mark all backends as retiring so select_backend skips them for new connections.
    for (Backend& b : route.backends) {
        b.retiring = true;
        b.healthy  = false;   // also mark unhealthy to stop any in-flight probes routing here
    }
    route.draining = true;
    gs.draining_routes.push_back(std::move(route));
    Log() << "hot-reload: route \"" << gs.draining_routes.back().name
          << "\" removed — draining " << gs.draining_routes.back().backends.size()
          << " backend(s)";
}

// Re-parse config and apply the diff to the live gateway state.
// Called from the event loop after a SIGUSR1. Never throws — on parse failure
// we keep the old config and log the error.
void apply_reload(GatewayState& gs) {
    g_reload = 0;  // clear flag first so a second SIGUSR1 during reload queues another

    GatewayConfig new_cfg;
    try {
        new_cfg = parse_config(gs.config_path);
    } catch (const exception& e) {
        Log() << "hot-reload FAILED (keeping old config): " << e.what();
        return;
    }

    // Everything below mutates the route/backend list structure that the
    // dashboard thread also reads — hold routes_mtx for the whole diff. Parsing
    // above (file IO) is deliberately outside the lock.
    std::lock_guard<std::mutex> lock(gs.routes_mtx);

    // ---- routes: add new, merge existing, retire removed ------------------
    // Map each new route name to its protocol so we can detect both removals and
    // protocol changes (tcp<->udp needs a different listen socket, so the old
    // route is retired and the new one is created fresh below).
    unordered_map<string, Protocol> new_proto;
    for (Route& nr : new_cfg.routes) {
        new_proto[nr.name] = nr.protocol;
    }

    // Retire routes that disappeared from the new config, or whose protocol changed.
    for (auto it = gs.routes.begin(); it != gs.routes.end(); ) {
        auto np = new_proto.find(it->name);
        if (np == new_proto.end() || np->second != it->protocol) {
            retire_route(gs, *it);
            it = gs.routes.erase(it);
        } else {
            ++it;
        }
    }

    // Build a name->Route* map of current routes for fast lookup. Pointers are
    // stable in a list, so adding new routes below won't invalidate these.
    unordered_map<string, Route*> existing_idx;
    for (Route& r : gs.routes) {
        existing_idx[r.name] = &r;
    }

    for (Route& nr : new_cfg.routes) {
        auto found = existing_idx.find(nr.name);
        if (found == existing_idx.end()) {
            // Brand-new route: bind a TCP or UDP listening socket and start
            // serving. A bind failure skips just this route (don't abort the
            // whole reload / crash the event loop).
            bool udp = (nr.protocol == Protocol::Udp);
            int listen_fd;
            try {
                listen_fd = create_listening_socket(nr.listen_port,
                                                    udp ? SOCK_DGRAM : SOCK_STREAM);
            } catch (const exception& e) {
                Log() << "hot-reload: bind for new route \"" << nr.name
                      << "\" failed: " << e.what() << " — skipping";
                continue;
            }
            if (!epoll_add(gs, listen_fd, EPOLLIN)) {
                Log() << "hot-reload: epoll_add for new route \"" << nr.name
                      << "\" failed: " << strerror(errno) << " — skipping";
                close(listen_fd);
                continue;
            }
            gs.routes.push_back(std::move(nr));
            Route* added = &gs.routes.back();
            if (udp) {
                gs.udp_listen_fd_to_route[listen_fd] = added;
            } else {
                gs.listen_fd_to_route[listen_fd] = added;
            }
            Log() << "hot-reload: added " << (udp ? "udp " : "") << "route \""
                  << added->name << "\" on :" << added->listen_port;
        } else {
            // Existing route: diff backends.
            Route& er = *found->second;

            // Refresh route-level config-only fields that don't need a diff:
            // idle-timeout override, UDP session timeout, and affinity
            // (sticky-routing) rules. Already-open connections/sessions keep
            // whatever timeout was resolved at creation — only new ones see the change.
            er.idle_timeout_seconds = nr.idle_timeout_seconds;
            er.udp_session_timeout_seconds = nr.udp_session_timeout_seconds;
            er.udp_datagram_rate_limit = nr.udp_datagram_rate_limit;
            er.affinity = nr.affinity;

            // Build sets for fast comparison.
            set<pair<string,int>> existing_be, new_be;
            for (const Backend& b : er.backends) {
                if (!b.retiring) existing_be.insert({b.host, b.port});
            }
            for (const Backend& b : nr.backends) {
                new_be.insert({b.host, b.port});
            }

            // Mark removed backends as retiring (keep in vector until drained).
            for (Backend& b : er.backends) {
                if (new_be.find({b.host, b.port}) == new_be.end() && !b.retiring) {
                    b.retiring = true;
                    b.healthy  = false;
                    Log() << "hot-reload: retiring backend " << b.host << ":" << b.port
                          << " from route \"" << er.name << "\"";
                }
            }

            // Append genuinely new backends.
            for (const Backend& nb : nr.backends) {
                if (existing_be.find({nb.host, nb.port}) == existing_be.end()) {
                    er.backends.push_back(nb);
                    Log() << "hot-reload: added backend " << nb.host << ":" << nb.port
                          << " to route \"" << er.name << "\"";
                }
            }

            // Refresh config-only fields on backends that PERSIST across reload
            // (same host:port, present both before and after). Without this,
            // dashboard edits to an existing backend's whitelist/blacklist/
            // rate_limit/disabled/heartbeat never reach the live Backend object
            // — gs.access.init() below reads b.access straight off gs.routes,
            // so a persisting backend's ACL scope would keep being re-seeded
            // from stale data forever. Runtime/atomic fields (healthy,
            // active_connections, retiring, ...) are deliberately untouched.
            for (Backend& eb : er.backends) {
                if (eb.retiring) continue;
                auto match = find_if(nr.backends.begin(), nr.backends.end(),
                                     [&eb](const Backend& nb) {
                                         return nb.host == eb.host && nb.port == eb.port;
                                     });
                if (match != nr.backends.end()) {
                    eb.access              = match->access;
                    eb.rate_max_per_minute = match->rate_max_per_minute;
                    eb.disabled            = match->disabled;
                    eb.heartbeat_enabled   = match->heartbeat_enabled;
                    eb.proxy_protocol      = match->proxy_protocol;
                }
            }
        }
    }

    // ---- idle timeout: update global default -------------------------------
    // Route-level overrides were already refreshed above. Like those, this
    // only affects connections accepted AFTER this reload — each Connection
    // resolves and freezes its idle_timeout_seconds once, at accept() time.
    gs.idle_timeout_seconds = new_cfg.idle_timeout_seconds;

    // ---- connection cap: takes effect for subsequent accepts ---------------
    gs.max_connections = new_cfg.max_connections;

    // ---- UDP session cap + default timeout ---------------------------------
    // The cap takes effect for subsequent new sessions; the default timeout is
    // resolved and frozen per session at creation, so it only affects sessions
    // created after this reload (existing ones keep their timeout). Per-route
    // UDP-route add/remove diffing is handled separately (hot-reload of UDP
    // routes is a later milestone).
    gs.udp_session_timeout_seconds = new_cfg.udp_session_timeout_seconds;
    gs.max_udp_sessions = new_cfg.max_udp_sessions;
    gs.udp_datagram_rate_limit = new_cfg.udp_datagram_rate_limit;
    gs.udp_health_cooldown_seconds = new_cfg.udp_health_cooldown_seconds;

    // ---- heartbeat: update timerfd if interval changed --------------------
    if (new_cfg.heartbeat.enabled != gs.heartbeat.enabled ||
        new_cfg.heartbeat.interval_seconds != gs.heartbeat.interval_seconds) {
        gs.heartbeat = new_cfg.heartbeat;
        if (gs.timer_fd != -1) {
            // Rearm with new interval (or disarm if now disabled).
            itimerspec its{};
            if (gs.heartbeat.enabled) {
                its.it_value.tv_sec    = gs.heartbeat.interval_seconds;
                its.it_interval.tv_sec = gs.heartbeat.interval_seconds;
            }
            timerfd_settime(gs.timer_fd, 0, &its, nullptr);
            Log() << "hot-reload: heartbeat interval updated to "
                  << gs.heartbeat.interval_seconds << "s";
        }
    }

    // ---- immediate cleanup of empty retiring backends ----------------------
    // If a backend was marked retiring above but ALREADY had 0 connections,
    // no connection will ever close on it to trigger lazy cleanup. Prune it now.
    auto prune_empty_retiring = [](list<Route>& routes) {
        for (Route& r : routes) {
            r.backends.remove_if([](const Backend& b) {
                return b.retiring.load() && b.active_connections.load() == 0;
            });
        }
    };
    prune_empty_retiring(gs.routes);
    prune_empty_retiring(gs.draining_routes);
    gs.draining_routes.remove_if([](const Route& r) { return r.backends.empty(); });

    // ---- access control: re-initialize from new config ---------------------
    // apply_reload() only diffed routes/backends and heartbeat above.
    // Without this, whitelist/blacklist/rate-limit changes are silently ignored
    // until a full restart — the old AccessControl state would persist forever.
    // init() re-keys backend scopes by the now-stable Backend* addresses in
    // gs.routes (all push_back/erase mutations above are complete at this point).
    gs.access.init(new_cfg, gs.routes, chrono::steady_clock::now());

    Log() << "hot-reload: done — " << gs.routes.size() << " active route(s), "
          << gs.draining_routes.size() << " draining";
    // Rebuild the dashboard's draining snapshot after the full reload diff is applied.
    if (gs.drain_snap) gs.drain_snap->update(gs.draining_routes);
}

}  // namespace

void run_gateway(GatewayConfig config, const string& config_path) {
    auto start_time = chrono::steady_clock::now();
    // Ring buffer for the dashboard's live log panel. Declared here so it outlives
    // the detached dashboard thread; only wired to Log() when the dashboard runs.
    LogBuffer log_buf(config.dashboard.log_lines > 0 ? config.dashboard.log_lines : 200);
    // Ignore SIGPIPE so a send() to a closed peer fails with EPIPE instead of killing us
    signal(SIGPIPE, SIG_IGN);

    // Handle Ctrl+C / TERM. No SA_RESTART so epoll_wait returns EINTR and the loop sees g_stop
    struct sigaction sa{};
    sa.sa_handler = on_stop_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // SIGUSR1: config hot-reload. SA_RESTART is intentionally NOT set, so
    // epoll_wait wakes up (EINTR) and the loop can call apply_reload() safely.
    // SIGHUP is deliberately left alone — it's reserved for the log writer's
    // rotation-reopen (logwriter.cpp), which would otherwise be clobbered here.
    struct sigaction sa_reload{};
    sa_reload.sa_handler = on_reload_signal;
    sigaction(SIGUSR1, &sa_reload, nullptr);

    DrainSnapshot drain_snap;   // lives here so it outlives the detached dashboard thread
    GatewayState gs;
    gs.config_path = config_path;           // needed for hot-reload re-parse
    gs.routes = move(config.routes);        // own the routes (they carry mutable state)
    gs.heartbeat = config.heartbeat;
    gs.idle_timeout_seconds = config.idle_timeout_seconds;
    gs.max_connections = config.max_connections;
    gs.udp_session_timeout_seconds = config.udp_session_timeout_seconds;
    gs.max_udp_sessions = config.max_udp_sessions;
    gs.udp_datagram_rate_limit = config.udp_datagram_rate_limit;
    gs.udp_health_cooldown_seconds = config.udp_health_cooldown_seconds;
    gs.drain_snap = &drain_snap;            // dashboard reads draining-route snapshots via this
    // Access control keys backend scopes by the now-stable gs.routes addresses.
    gs.access.init(config, gs.routes, start_time);

    gs.epoll_fd = epoll_create1(0);
    if (gs.epoll_fd == -1) {
        throw errno_error("epoll_create1()");
    }

    for (Route& route : gs.routes) {
        bool udp = (route.protocol == Protocol::Udp);
        int listen_fd = create_listening_socket(route.listen_port,
                                                udp ? SOCK_DGRAM : SOCK_STREAM);
        if (!epoll_add(gs, listen_fd, EPOLLIN)) {
            int e = errno;
            close(listen_fd);
            close(gs.epoll_fd);
            errno = e;
            throw errno_error("epoll_ctl(ADD listen_fd)");
        }
        if (udp) {
            gs.udp_listen_fd_to_route[listen_fd] = &route;
        } else {
            gs.listen_fd_to_route[listen_fd] = &route;
        }
        Log() << "listening on " << (udp ? "udp :" : ":") << route.listen_port
              << " for route \"" << route.name << "\"";
    }

    // Heartbeat timer: fires every interval_seconds, in the same epoll set
    if (gs.heartbeat.enabled) {
        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (tfd == -1) {
            close(gs.epoll_fd);
            throw errno_error("timerfd_create()");
        }
        itimerspec its{};
        its.it_value.tv_sec = gs.heartbeat.interval_seconds;
        its.it_interval.tv_sec = gs.heartbeat.interval_seconds;
        if (timerfd_settime(tfd, 0, &its, nullptr) == -1) {
            int e = errno;
            close(tfd);
            close(gs.epoll_fd);
            errno = e;
            throw errno_error("timerfd_settime()");
        }
        if (!epoll_add(gs, tfd, EPOLLIN)) {
            int e = errno;
            close(tfd);
            close(gs.epoll_fd);
            errno = e;
            throw errno_error("epoll_ctl(ADD timer_fd)");
        }
        gs.timer_fd = tfd;
        Log() << "heartbeat: probing idle backends every "
              << gs.heartbeat.interval_seconds << "s";
    }

    // Idle-connection sweep timer: fires every second and checks every live
    // connection's idle_timeout_seconds. Always created (cheap — a no-op sweep
    // over connections whose idle_timeout_seconds is 0) so a hot reload can
    // turn idle timeout on/off at any time without re-plumbing the epoll set.
    {
        int ifd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (ifd == -1) {
            close(gs.epoll_fd);
            throw errno_error("timerfd_create() [idle sweep]");
        }
        itimerspec its{};
        its.it_value.tv_sec = 1;
        its.it_interval.tv_sec = 1;
        if (timerfd_settime(ifd, 0, &its, nullptr) == -1) {
            int e = errno;
            close(ifd);
            close(gs.epoll_fd);
            errno = e;
            throw errno_error("timerfd_settime() [idle sweep]");
        }
        if (!epoll_add(gs, ifd, EPOLLIN)) {
            int e = errno;
            close(ifd);
            close(gs.epoll_fd);
            errno = e;
            throw errno_error("epoll_ctl(ADD idle_timer_fd)");
        }
        gs.idle_timer_fd = ifd;
    }

    // Dashboard runs only in single-process mode: it reads this process's own
    // in-memory stats + log ring, which aren't shared across workers. Under
    // multi-process it's skipped (main() logs a note). Runs on its own thread and
    // port, separate from our epoll set. &gs.routes is stable (never resized after
    // this point), so the dashboard reads LIVE atomic fields, not a frozen copy.
    if (config.dashboard.enabled && config.worker_processes == 1) {
        log_set_buffer(&log_buf);  // mirror Log() lines into the dashboard's ring
        start_dashboard(&gs.routes, &gs.routes_mtx, &drain_snap, config.dashboard, &log_buf, config_path,
                        start_time, config.worker_processes, config.log_file, config.log_rotation);
        Log() << "dashboard: listening on :" << config.dashboard.port;
    }
    Log() << "Sluice: event loop started, " << gs.routes.size()
          << " route(s) active";

    epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        int n = epoll_wait(gs.epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                // SIGUSR1 wakes epoll_wait with EINTR — process reload before re-entering wait.
                if (g_reload) apply_reload(gs);
                continue;  // re-check the loop condition (handles g_stop too)
            }
            shutdown_all(gs);
            throw errno_error("epoll_wait()");
        }

        gs.pending_close.clear();
        bool timer_fired = false;
        bool idle_timer_fired = false;

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            // Heartbeat timer tick: drain the counter now, run probes after the batch
            if (fd == gs.timer_fd) {
                uint64_t expirations;
                while (read(fd, &expirations, sizeof(expirations)) > 0) {
                }
                timer_fired = true;
                continue;
            }

            // Idle-sweep timer tick: drain the counter now, sweep after the batch
            if (fd == gs.idle_timer_fd) {
                uint64_t expirations;
                while (read(fd, &expirations, sizeof(expirations)) > 0) {
                }
                idle_timer_fired = true;
                continue;
            }

            // Skip fds already scheduled for teardown earlier in this batch
            if (gs.pending_close.count(fd)) {
                continue;
            }

            auto route_it = gs.listen_fd_to_route.find(fd);
            if (route_it != gs.listen_fd_to_route.end()) {
                handle_new_clients(gs, fd, *route_it->second);
                continue;
            }

            // UDP listen fd: relay queued client datagrams to their sessions.
            auto udp_route_it = gs.udp_listen_fd_to_route.find(fd);
            if (udp_route_it != gs.udp_listen_fd_to_route.end()) {
                handle_udp_client_datagrams(gs, fd, *udp_route_it->second);
                continue;
            }

            // UDP backend reply: relay it back out the listen socket to the client.
            // EPOLLERR on the backend fd means the socket is in an unrecoverable error
            // state — tear the session down immediately to prevent an event-loop spin
            // (a subsequent recv() on an errored fd would keep returning errors).
            if (gs.udp_backend_fd_to_session.count(fd)) {
                if (ev & EPOLLERR) {
                    auto sit = gs.udp_backend_fd_to_session.find(fd);
                    if (sit != gs.udp_backend_fd_to_session.end()) {
                        UdpSession* s = sit->second;
                        Log() << "udp backend fd=" << fd << " error (route \""
                              << s->route_name << "\") — ending session";
                        mark_udp_backend_failed(s->backend);
                        destroy_udp_session(gs, s);
                    }
                } else {
                    handle_udp_backend_reply(gs, fd);
                }
                continue;
            }

            // Heartbeat probe completion
            if (gs.probe_fd_to_backend.count(fd)) {
                handle_probe_result(gs, fd);
                continue;
            }

            auto conn_it = gs.fd_to_connection.find(fd);
            if (conn_it == gs.fd_to_connection.end()) {
                continue;
            }
            Connection* conn = conn_it->second;

            // Pending backend connect completing (or failing)
            if (fd == conn->backend_fd && !conn->backend_connected) {
                handle_backend_connect_result(gs, fd);
                continue;
            }

            // Hard error on the socket
            if (ev & EPOLLERR) {
                mark_close(gs, conn);
                continue;
            }

            // Writable: flush queued outbound first
            if (ev & EPOLLOUT) {
                handle_writable(gs, fd);
                if (gs.pending_close.count(fd)) {
                    continue;
                }
            }

            // Readable (or hung up with final data): relay it
            if (ev & (EPOLLIN | EPOLLHUP)) {
                handle_readable(gs, fd);
                if (gs.pending_close.count(fd)) {
                    continue;
                }
            }

            // Full hangup: peer is gone, retire the pair after draining final bytes above
            if (ev & EPOLLHUP) {
                mark_close(gs, conn);
            }
        }

        // Idle sweep runs before cleanup (unlike heartbeats below) because it
        // can itself call mark_close() — running it here means those closes
        // are swept up in the SAME cleanup pass instead of sitting in
        // pending_close until some later, possibly-never event wakes the loop.
        if (idle_timer_fired) {
            check_idle_connections(gs);
            recover_udp_backends(gs);
        }

        // Deferred cleanup: safe now the whole batch (including the idle sweep)
        // is fully processed and no fd was reused mid-batch.
        for (int fd : gs.pending_close) {
            auto it = gs.fd_to_connection.find(fd);
            if (it != gs.fd_to_connection.end()) {
                cleanup_connection(gs, it->second);
            }
        }
        gs.pending_close.clear();

        // Heartbeats run after cleanup so probe sockets never collide with fds
        // freed/reused during this batch, and active_connections counts (used
        // to skip probing busy backends) are up to date.
        if (timer_fired) {
            run_heartbeats(gs);
        }

        // Hot-reload: apply_reload() is safe here — the epoll batch is fully
        // processed, all deferred cleanups are done, no Connection pointer is dangling.
        if (g_reload) {
            apply_reload(gs);
        }
    }

    Log() << "Sluice: shutting down ("
          << gs.fd_to_connection.size() / 2 << " active connection(s) closed)";
    shutdown_all(gs);
    log_set_buffer(nullptr);  // log_buf is about to be destroyed
}
