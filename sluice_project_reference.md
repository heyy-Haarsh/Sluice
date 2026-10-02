# Sluice — Complete Project Reference

> **Sluice** (noun): a gate that controls a stream of water.
> Named because the gateway controls TCP streams — TCP is a stream-oriented protocol.

---

## Table of Contents

1. [What is Sluice?](#1-what-is-sluice)
2. [Architecture Overview](#2-architecture-overview)
3. [How a Connection Flows End-to-End](#3-how-a-connection-flows-end-to-end)
4. [Core Engine: epoll](#4-core-engine-epoll)
5. [Source File Guide](#5-source-file-guide)
6. [Key Data Structures](#6-key-data-structures)
7. [Feature Deep Dives](#7-feature-deep-dives)
   - [Load Balancing](#71-load-balancing-round-robin)
   - [Session Affinity (Stickiness)](#72-session-affinity-stickiness)
   - [Heartbeat & Failover](#73-heartbeat--failover)
   - [Hot Reload](#74-hot-reload-zero-downtime-config-change)
   - [Backpressure & Memory Safety](#75-backpressure--memory-safety)
   - [PROXY Protocol](#76-proxy-protocol)
   - [Access Control & Rate Limiting](#77-access-control--rate-limiting)
   - [Dashboard](#78-dashboard)
   - [Idle Timeout](#79-idle-timeout)
   - [Multi-process Mode](#710-multi-process-mode)
8. [Configuration File Format](#8-configuration-file-format)
9. [Benchmarking Suite](#9-benchmarking-suite)
10. [Build & Run](#10-build--run)

---

## 1. What is Sluice?

Sluice is a **Layer 4 (TCP) reverse proxy / load balancer** written in C++.

It sits between clients (browsers, apps, IoT devices, etc.) and a cluster of backend servers. When a client connects, Sluice picks the healthiest available backend, and silently pipes all data back and forth between the two — the client never knows there is a middleman.

```
   Client A ─┐
   Client B ─┤──▶  [ Sluice Gateway ]  ──▶  Backend 1
   Client C ─┘                          ──▶  Backend 2
                                         ──▶  Backend 3
```

**Why Layer 4?**  
Layer 4 means it works at the TCP level — it doesn't understand HTTP, WebSockets, or any specific application protocol. This makes it universal: it can proxy any TCP-based protocol (HTTP, HTTPS, SSH, gRPC, database connections, game servers, etc.) without needing separate plugins.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                     Sluice Process                      │
│                                                         │
│  ┌──────────────┐    ┌────────────────────────────────┐ │
│  │  Main Thread │    │     Dashboard Thread           │ │
│  │  (Event Loop)│    │  (HTTP API on port 8080)       │ │
│  │              │    │                                │ │
│  │  epoll_wait()│    │  Reads route/backend state     │ │
│  │      │       │    │  atomically (no mutex needed   │ │
│  │   handle_*() │    │  for atomic fields; routes_mtx │ │
│  │      │       │    │  for structural list access)   │ │
│  └──────┬───────┘    └────────────────────────────────┘ │
│         │                                               │
│   ┌─────▼─────────────────────────────────────┐        │
│   │              GatewayState                 │        │
│   │  epoll_fd, routes, connections,           │        │
│   │  access control, heartbeat timers         │        │
│   └───────────────────────────────────────────┘        │
└─────────────────────────────────────────────────────────┘
         │                          │
    Client side                Backend side
  (accept(), EPOLLIN)        (connect(), EPOLLIN/OUT)
```

**Key design principles:**
- **Single-threaded event loop**: One thread handles ALL clients via `epoll`. No thread per connection.
- **Non-blocking I/O everywhere**: Every socket is set to `O_NONBLOCK`. The program never sleeps waiting for data.
- **Zero-copy relay**: Data is read from one side and buffered, then written to the other side as soon as possible.
- **Atomic runtime stats**: Backend stats (connection count, bytes relayed) use `std::atomic` so the dashboard thread can safely read them without locking.

---

## 3. How a Connection Flows End-to-End

Here is the exact sequence of events when a client connects and sends data:

```
Step 1: CLIENT CONNECTS
  └─ epoll fires EPOLLIN on the listen socket
  └─ handle_new_clients() is called
     ├─ accept() returns a new client_fd
     ├─ Client IP is extracted (peer_endpoint())
     ├─ AccessControl checks: global whitelist/blacklist/rate limit
     ├─ select_affinity_backend() → tries pinned routing first
     ├─ select_backend() → round-robin if no affinity match
     ├─ open_nonblocking_connect() → non-blocking connect to backend
     ├─ A new Connection object is created (client_fd + backend_fd)
     └─ Both fds registered with epoll

Step 2: BACKEND CONNECTION COMPLETES
  └─ epoll fires EPOLLOUT on backend_fd
  └─ handle_backend_connect_complete() confirms connection via getsockopt
  └─ If proxy_protocol=true, PROXY header is prepended to backend buffer
  └─ start_relay() sets both fds to EPOLLIN — bidirectional relay begins

Step 3: DATA RELAY (both directions simultaneously)
  └─ Client sends data → epoll fires EPOLLIN on client_fd
     └─ handle_readable() → recv() into buffer → queue_outbound() to backend_fd
  └─ Backend sends response → epoll fires EPOLLIN on backend_fd  
     └─ handle_readable() → recv() into buffer → queue_outbound() to client_fd
  └─ If a send buffer fills up → EPOLLOUT is set → handle_writable() flushes it

Step 4: CONNECTION CLOSES
  └─ One side sends FIN → recv() returns 0
  └─ handle_eof() marks that side as EOF
  └─ Remaining buffered bytes are flushed to the other side
  └─ When BOTH sides are EOF and BOTH buffers are empty → cleanup_connection()
     ├─ Both fds removed from epoll
     ├─ Both fds closed
     ├─ Connection object deleted (memory freed)
     └─ Backend's active_connections counter decremented
```

---

## 4. Core Engine: epoll

`epoll` is the Linux kernel's event notification system. It is the reason Sluice can handle 10,000 simultaneous connections using only ~5 MB of RAM.

### How it works in Sluice

The gateway creates one epoll file descriptor at startup (`epoll_create1(0)`), then adds every socket it manages to this set:
- All listening sockets (one per route/port)
- Every client socket (client_fd)
- Every backend socket (backend_fd)
- The heartbeat timer socket (timerfd)
- The idle-timeout timer socket (timerfd)

The main loop calls `epoll_wait()` which blocks (uses 0% CPU) until any of these sockets has something to do. The kernel returns a batch of up to 256 ready events at once.

### The four epoll flags used

| Flag | Meaning | When Sluice uses it |
|---|---|---|
| `EPOLLIN` | Data ready to read | Normal relay: both sides of a connection |
| `EPOLLOUT` | Socket has space to write | When a send buffer has data but the kernel was full |
| `EPOLLERR` | An error occurred | Close the connection immediately |
| `EPOLLHUP` | The other end hung up | Close the connection gracefully |

### Smart epoll interest (backpressure)
Sluice uses `desired_mask()` to intelligently update what each socket is registered for.  
- If a client sends data faster than the backend can receive it, the backend buffer grows.
- When it hits the `BUFFER_HIGH_WATER` mark (1 MiB), Sluice removes `EPOLLIN` from the client socket — effectively telling the kernel "stop telling me about client data until I catch up."
- Once the backend drains its buffer, `EPOLLIN` is re-added for the client.

This prevents memory from growing unboundedly under load.

---

## 5. Source File Guide

| File | Role |
|---|---|
| `src/main.cpp` | Entry point. Parses args, calls `run_master()` or `run_gateway()`. |
| `src/master.cpp` | Multi-process manager. Forks N worker processes, restarts them on crash. |
| `src/gateway.cpp` | **The core.** The entire event loop, relay engine, heartbeat, hot-reload, failover. |
| `src/config.cpp` | JSON config parser. Converts `config.json` into `GatewayConfig` struct. |
| `src/config.hpp` | All config data structures: `Route`, `Backend`, `GatewayConfig`, etc. |
| `src/connection.hpp` | The `Connection` struct — represents one live client↔backend pair. |
| `src/access.cpp` | IP whitelist, blacklist, CIDR matching, and rate-limit logic. |
| `src/access.hpp` | `AccessControl` class interface + IP/CIDR parsing helpers. |
| `src/dashboard.cpp` | Background thread. Serves an HTTP dashboard + REST API on port 8080. |
| `src/dashboard_ui.hpp` | The full HTML/CSS/JS for the dashboard web UI (embedded as a string). |
| `src/log.cpp/hpp` | Thread-safe `Log()` stream. Timestamps every message. |
| `src/log_buffer.cpp/hpp` | A ring buffer that stores the last N log lines for the dashboard. |
| `src/logwriter.cpp/hpp` | Optional log file writer with hourly/daily rotation. |
| `src/drain_snapshot.cpp/hpp` | Thread-safe snapshot of draining routes for the dashboard. |
| `src/setup.cpp` | Systemd/daemon setup helpers. |

---

## 6. Key Data Structures

### `Connection` (connection.hpp)
Represents one live pair of connected sockets.

```
Connection {
    client_fd          — the file descriptor for the client's socket
    backend_fd         — the file descriptor for the backend's socket
    backend_connected  — false while the TCP connect() to backend is still in progress
    route_name         — name of the route (for logging)
    backend*           — pointer to which Backend this pair uses (for counting)

    idle_timeout_seconds  — how long to wait before closing an idle connection
    last_activity         — time of the last data read (for idle timeout)

    proxy_header       — the PROXY protocol v1 header string (sent once, then cleared)

    client_to_backend_buffer  — bytes received from client, not yet sent to backend
    backend_to_client_buffer  — bytes received from backend, not yet sent to client

    client_eof    — has the client finished sending? (TCP FIN received)
    backend_eof   — has the backend finished sending? (TCP FIN received)
    client_shut   — has shutdown(SHUT_WR) been called on client side?
    backend_shut  — has shutdown(SHUT_WR) been called on backend side?

    client_events   — current epoll interest mask for client_fd
    backend_events  — current epoll interest mask for backend_fd
}
```

### `Backend` (config.hpp)
Represents one backend server in a route.

```
Backend {
    host, port          — network address of the backend server
    heartbeat_enabled   — whether to probe this backend for health
    disabled            — never select this backend (config flag)
    proxy_protocol      — emit a PROXY v1 header before data (opt-in per backend)
    access              — per-backend whitelist/blacklist

    healthy (atomic)              — currently passing health checks?
    active_connections (atomic)   — number of live clients on this backend
    bytes_relayed (atomic)        — total bytes proxied through this backend
    total_connections (atomic)    — lifetime connection count
    retiring (atomic)             — being drained after hot-reload removed it
}
```

### `Route` (config.hpp)
Represents one "service" — a port that clients connect to.

```
Route {
    name              — human-readable label (e.g., "my-api")
    listen_port       — the port clients connect to (e.g., 9000)
    backends          — list<Backend> — the pool of servers to send traffic to
    next_index        — round-robin cursor: which backend was chosen last
    draining          — true if this route is being removed (hot-reload)
    idle_timeout_seconds  — per-route override for idle connection timeout
    affinity          — list of client-IP → preferred-backend rules (stickiness)
}
```

### `GatewayState` (gateway.cpp, internal)
The entire runtime state of the gateway, passed through every function.

```
GatewayState {
    epoll_fd                  — the master epoll file descriptor
    routes                    — list<Route> (the active routes)
    heartbeat                 — heartbeat timer config
    access                    — the AccessControl engine
    timer_fd                  — timerfd for heartbeat scheduling
    idle_timer_fd             — timerfd for idle connection sweeping
    config_path               — path to config file (for hot-reload)
    listen_fd_to_route        — map: which listen socket belongs to which route
    probe_fd_to_backend       — map: which socket is an in-flight heartbeat probe
    fd_to_connection          — map: given any fd, find its Connection (both sides)
    live_connections          — set of all active Connection* (for idle timeout sweep)
    pending_close             — fds to be cleaned up after the current epoll batch
    draining_routes           — routes removed by hot-reload, waiting to drain
}
```

---

## 7. Feature Deep Dives

### 7.1 Load Balancing (Round Robin)

**File:** `gateway.cpp` → `select_backend()`

Sluice distributes new connections evenly across all healthy, enabled backends using round-robin. The route tracks a `next_index` cursor which advances on each new connection.

```
Route has 3 backends: [A, B, C]
Connection 1 → A (next_index → 1)
Connection 2 → B (next_index → 2)
Connection 3 → C (next_index → 0)
Connection 4 → A (next_index → 1) ... and so on
```

A backend is skipped if it is:
- `disabled = true` (explicitly excluded in config)
- `healthy = false` (failing heartbeats)
- `retiring = true` (being drained after a hot-reload removal)
- Blocked by per-backend access control for this specific client IP

---

### 7.2 Session Affinity (Stickiness)

**File:** `gateway.cpp` → `select_affinity_backend()`

For use cases where the same client must always reach the same backend (e.g., stateful sessions), the config supports **affinity rules** that pin a client IP (or CIDR range like `10.0.5.0/24`) to a specific backend.

**How it works:**
1. When a new client connects, `select_affinity_backend()` is called first.
2. It checks if the client's IP matches any rule in the route's `affinity` list.
3. If a match is found AND the target backend is healthy, that backend is returned.
4. If the pinned backend is unhealthy/disabled/retiring, `nullptr` is returned and the caller falls back to normal round-robin.

This is "soft stickiness" — the client follows the rule when possible, but never gets a failed connection because of it.

---

### 7.3 Heartbeat & Failover

**File:** `gateway.cpp` → heartbeat/probe functions

Every `interval_seconds` (default: 10), the gateway probes each backend by opening a TCP connection to it. If the connection succeeds, the backend is `healthy = true`. If it fails or times out, `healthy = false` and no new clients are sent there.

The heartbeat uses the same `epoll` event loop — a `timerfd` is added to the epoll set, and when it fires (`EPOLLIN` on the timer), the gateway loops through all backends and opens non-blocking probe sockets (`probe_fd`). These probe sockets are also tracked in epoll so their results (`EPOLLOUT` = success, `EPOLLERR` = failure) are processed in the same event loop without blocking.

**Failover is automatic**: As soon as a backend's `healthy` flag goes `false`, `select_backend()` skips it for all future connections. Existing connections to that backend are unaffected and continue until they naturally finish.

---

### 7.4 Hot Reload (Zero-Downtime Config Change)

**File:** `gateway.cpp` → `apply_reload()`

The config can be updated without restarting or dropping a single connection:

```bash
kill -SIGUSR1 <gateway-pid>
# Or use the dashboard API: POST /reload
```

**What happens:**
1. A signal handler sets `g_reload = 1`.
2. The event loop notices this flag between epoll batches and calls `apply_reload()`.
3. `apply_reload()` re-parses the config file.
4. **Diff is computed** (new routes vs. current routes):
   - **New routes** → create listening socket, add to epoll, start accepting.
   - **Removed routes** → `retire_route()`: close listen socket, mark backends as `retiring`. Existing connections continue until they finish naturally.
   - **Modified routes** → backends added/removed/updated in-place.
5. The entire diff is done while holding `routes_mtx` so the dashboard thread sees a consistent state.

---

### 7.5 Backpressure & Memory Safety

**File:** `gateway.cpp` → `desired_mask()`, `BUFFER_HIGH_WATER`

Each `Connection` has two buffers:
- `client_to_backend_buffer`: bytes read from client, waiting to be sent to backend.
- `backend_to_client_buffer`: bytes read from backend, waiting to be sent to client.

If a slow receiver causes one buffer to grow beyond `BUFFER_HIGH_WATER` (1 MiB), Sluice automatically removes `EPOLLIN` from the fast sender. This pauses reading from the fast side until the slow side catches up.

This means **memory usage is bounded** even if one side is much slower than the other.

---

### 7.6 PROXY Protocol

**File:** `gateway.cpp` → `build_proxy_header()`, `send_proxy_header()`

**The problem:** Normally, the backend server sees Sluice's own IP address as the "client." The real client's IP is invisible to the backend, breaking access logs, IP-based rate limiting, and geo-blocking on the backend side.

**The solution:** PROXY Protocol v1 is a standard that prepends a short header line to the very first bytes of the TCP stream, telling the backend the real client IP:

```
PROXY TCP4 192.168.1.100 10.0.0.5 54321 9000\r\n
          ↑ real client IP      ↑ gateway IP
```

**Configuration:** Opt-in per backend (disabled by default, since backends must understand PROXY Protocol):

```json
"backends": [
  { "host": "127.0.0.1", "port": 8001, "proxy_protocol": true }
]
```

**Implementation:** When a new connection is accepted and the backend has `proxy_protocol = true`, the header string is stored in `conn->proxy_header`. The moment the backend TCP connection is established, `send_proxy_header()` prepends this header to the `client_to_backend_buffer`, so it is guaranteed to be the first data the backend receives.

---

### 7.7 Access Control & Rate Limiting

**File:** `access.cpp`, `access.hpp`

Two levels of access control:

**1. Global (applies to ALL routes):**
- **Whitelist**: If non-empty, only IPs in this list can connect. Everyone else is rejected.
- **Blacklist**: IPs in this list are always rejected. Supports CIDR ranges (e.g., `10.0.0.0/8`).
- **Rate limit**: Maximum connections per IP per minute (with exponential backoff).

**2. Per-Backend:**
- Same whitelist/blacklist/rate-limit but applied only when routing to a specific backend.

**Rate limiting uses a sliding window:**  
The `RateWindow` struct tracks two consecutive 1-minute windows and blends them based on how far into the current minute we are. This prevents the "2x burst at window boundaries" problem of a plain fixed-window counter.

**Exponential backoff:**  
When a client exceeds the rate limit, it's blocked for `backoff_base_seconds` (default: 60s). Each subsequent violation doubles the block time, up to `backoff_max_seconds` (default: 3600s = 1 hour).

---

### 7.8 Dashboard

**File:** `dashboard.cpp`, `dashboard_ui.hpp`

A background thread serves a live web dashboard on port 8080 (localhost only by default).

**What it shows:**
- All active routes and their listening ports
- Each backend's health status, active connection count, total bytes relayed
- Live log tail (last N lines)
- Draining routes (removed by hot-reload but still finishing active connections)

**REST API endpoints:**
- `GET /` → the web dashboard UI
- `GET /status` → JSON snapshot of all routes + backends
- `POST /reload` → triggers a hot-reload (same as `SIGUSR1`)
- `POST /config` → saves a new config and triggers reload (requires `X-Auth-Token` if configured)

**Thread safety:**
- Atomic fields on `Backend` (`healthy`, `active_connections`, `bytes_relayed`) are read without locking.
- `routes_mtx` is held briefly when the dashboard needs to walk the `routes` list structure (which can be modified by the event loop during hot-reload).

---

### 7.9 Idle Timeout

**File:** `gateway.cpp` → idle timer handling

A connection with no data transferred in either direction for longer than `idle_timeout_seconds` is automatically closed. This prevents "zombie" connections from accumulating and consuming file descriptors.

The idle check uses a second `timerfd` (also registered in epoll) that fires every second. When it fires, the event loop sweeps through all `live_connections` and closes any that have been silent too long.

Can be set globally or overridden per route (set to 0 to disable for a specific route).

---

### 7.10 Multi-process Mode

**File:** `master.cpp`

For high-traffic deployments, Sluice can run multiple worker processes. Set `"worker_processes": N` in config.

The `master.cpp` process:
1. Forks N worker processes.
2. Each worker runs a full independent event loop with its own epoll instance.
3. All workers bind the same port (using `SO_REUSEPORT`) — the Linux kernel automatically load-balances incoming connections across them.
4. If a worker crashes, the master automatically restarts it.

In single-process mode (default: `worker_processes: 1`), `master.cpp` is not used and `gateway.cpp` runs directly.

---

### 7.11 UDP Routing

**Files:** `gateway.cpp` (relay), `udp_session.hpp` (`UdpSession`), `config.cpp` (parsing)

Set `"protocol": "udp"` on a route and Sluice proxies datagrams instead of TCP
streams. A route is exactly one protocol; the same port may host both a `tcp` and
a `udp` route (uniqueness is enforced per `(protocol, port)`).

**Synthesized sessions.** UDP is connectionless — there is no accept and no FIN —
so Sluice invents a *session* the first time it sees a datagram from a given
client source `(ip, port)`. Creating a session runs the **same policy sequence as
TCP**: the `max_udp_sessions` cap, global access control + rate limiting, then
affinity/round‑robin backend selection. It then opens one **connected**
`SOCK_DGRAM` socket to the chosen backend and stores the session keyed by the
client endpoint.

**How replies find their way home.** The per‑session backend socket is
`connect()`ed, so the kernel demultiplexes the backend's reply back to that exact
fd (by the 4‑tuple). Sluice reads it there and **always `sendto`s it out the
listen socket** to the stored raw client address — never out the backend socket —
so the client only ever sees the gateway's address.

**Lifecycle.** With no FIN, a session ends only via the idle sweep: the same
1‑second `timerfd` that closes idle TCP connections also reclaims UDP sessions
whose `udp_session_timeout_seconds` (route override, else global, default 30) has
elapsed. The value is floored to 5s at parse time so a session can never be
un‑expiring. `max_udp_sessions` bounds concurrent sessions (the main defense
against spoofed‑source floods, since one fd is spent per session).

**What differs from TCP.** UDP backends are **not actively** health‑probed — a
successful TCP `connect()` says nothing about a datagram service — but failures
*are* detected and tracked passively (§7.12). The PROXY protocol is TCP‑only. And
because UDP source addresses are trivially spoofable, IP allow/deny and the
session‑creation rate limit are **advisory** rather than a hard guarantee — a
per‑session datagram cap (§7.13) provides a second, harder-to-spoof‑around backstop
once a session exists. Hot reload adds/removes UDP routes like TCP ones, except a
removed UDP route's sessions are dropped immediately (they can't drain without
their listen socket) rather than gracefully drained.

The dashboard tags each route TCP/UDP and labels the UDP count as *sessions*; see
§7.12 for what the backend health cell shows.

---

### 7.12 UDP Passive Health & Recovery

**Files:** `gateway.cpp` (`mark_udp_backend_failed`, `recover_udp_backends`), `config.hpp` (`Backend::udp_marked_down_at`)

UDP backends get no active probe (§7.11), but a failure is still a real, immediate
signal when one arrives: sending to (or receiving from) a UDP backend socket
surfaces `ECONNREFUSED` the moment the kernel sees an ICMP port‑unreachable for
it — a definitive "nobody's listening," not a guess. Sluice treats that exactly
like a TCP connect failure: `set_health(backend, false)` **immediately**, no
retry threshold, mirroring how `handle_new_clients` already marks a TCP backend
down on any connect failure. The failing session is torn down at the same time
(`destroy_udp_session`), and the backend is excluded from `select_backend`
rotation from that point on. Conversely, any datagram a backend actually replies
with is treated as passive proof of life and promotes it back to healthy.

The asymmetry this creates — a backend can only be marked *down* by traffic, but
once down it's excluded from all future traffic — would leave it exiled forever
with no active probe to re-test it. `recover_udp_backends()` closes that loop: on
the same 1‑second `timerfd` tick that sweeps idle TCP connections and UDP
sessions, any UDP backend that's been unhealthy for more than a fixed
`UDP_HEALTH_COOLDOWN_SECONDS` (10s, not configurable) is optimistically flipped
back to healthy — untested until the next new session actually routes to it and
either confirms it (a reply) or fails it again (restarting its own cooldown).
This recovery sweep runs unconditionally, independent of `heartbeat.enabled`,
since it isn't active probing and shouldn't depend on that TCP‑only feature.

On the dashboard, a UDP backend's health cell shows a neutral `—` while presumed
OK (never actively confirmed) but flips to a real `DOWN` the instant a passive
failure is recorded — the same signal driving `/api/health`'s aggregate count.

### 7.13 UDP Per‑Session Rate Limiting

**Files:** `gateway.cpp` (`udp_consume_token`), `udp_session.hpp` (`UdpSession` token fields), `config.cpp`/`config.hpp` (`udp_datagram_rate_limit`)

The session‑creation `rate_limit` (§7.7) caps how many *new* UDP sessions an IP
may open per minute — it says nothing about how many datagrams a session that
already exists may relay. Left uncapped, one client could flood its own session
indefinitely once past that gate. `udp_datagram_rate_limit` closes that gap: a
per‑session token bucket on the **client → backend** direction only (the backend
is a configured, trusted target, not attacker‑controlled, so replies aren't
separately throttled).

The bucket's capacity is deliberately set equal to its refill rate — up to
`rate` tokens, refilled continuously at `rate`/second, one token per forwarded
datagram. That makes it behave like a **leaky bucket**: a hard sustained cap
with at most ~1 second of burst tolerance, rather than a token bucket's usual
larger, separately‑tunable burst allowance. A session starts with a full bucket
(an initial burst up to the cap is allowed), resolved once at session creation
from `route.udp_datagram_rate_limit` (`-1` = inherit the global default) exactly
like `udp_session_timeout_seconds`. `0` means unlimited, the default — existing
configs are unaffected unless this is set.

A datagram that finds an empty bucket is dropped silently, the same as any other
UDP loss — the session stays alive and keeps relaying once tokens refill. The
first drop for a given session logs one `rate limit (N/s) exceeded — throttling`
line (edge‑triggered, not per‑datagram, to avoid log spam under a sustained
flood).

---

## 8. Configuration File Format

Default location: `/etc/sluice/config.json`

```json
{
  "routes": [
    {
      "name": "my-service",
      "listen_port": 9000,
      "idle_timeout_seconds": 30,
      "affinity": [
        { "client": "10.0.5.0/24", "host": "127.0.0.1", "port": 8001 }
      ],
      "backends": [
        {
          "host": "127.0.0.1",
          "port": 8001,
          "proxy_protocol": false,
          "heartbeat_enabled": true,
          "disabled": false,
          "access": {
            "whitelist": [],
            "blacklist": []
          }
        }
      ]
    },
    {
      "name": "game-udp",
      "listen_port": 9001,
      "protocol": "udp",
      "udp_session_timeout_seconds": 15,
      "udp_datagram_rate_limit": 50,
      "backends": [
        { "host": "127.0.0.1", "port": 7401 }
      ]
    }
  ],
  "heartbeat": {
    "enabled": true,
    "interval_seconds": 10
  },
  "dashboard": {
    "enabled": true,
    "port": 8080,
    "bind": "127.0.0.1",
    "token": ""
  },
  "access": {
    "whitelist": [],
    "blacklist": [
      { "ip": "1.2.3.4" },
      { "ip": "10.0.0.0/8", "until": "2025-12-31T23:59:59" }
    ]
  },
  "rate_limit": {
    "max_per_minute": 1000,
    "backoff_base_seconds": 60,
    "backoff_max_seconds": 3600
  },
  "worker_processes": 1,
  "idle_timeout_seconds": 0,
  "max_connections": 10000,
  "udp_session_timeout_seconds": 30,
  "max_udp_sessions": 10000,
  "udp_datagram_rate_limit": 0,
  "log_file": "",
  "log_rotation": "none"
}
```

Per-route keys: `protocol` (`"tcp"` default | `"udp"`) and, for UDP routes,
`udp_session_timeout_seconds` (idle timeout for a synthesized session; `-1`/absent
inherits the global, any value is floored to 5s) and `udp_datagram_rate_limit`
(per-session datagrams/sec cap, client→backend only; `-1`/absent inherits the
global, `0` = explicitly unlimited for this route — see §7.13). Global UDP keys:
`udp_session_timeout_seconds` (default 30), `max_udp_sessions` (default 10000,
0 = unlimited), and `udp_datagram_rate_limit` (default 0 = unlimited, see §7.13).

---

## 9. Benchmarking Suite

**Location:** `tests/benchmark/`

An automated 3-tier benchmark suite written in Python.

### Files
| File | Role |
|---|---|
| `orchestrator.py` | Main runner. Starts everything, runs all tiers, saves CSVs. |
| `load_generator.py` | Async TCP client that spawns N concurrent connections. |
| `resource_monitor.py` | Records CPU, RAM, and FD count every second. |
| `plot_graphs.py` | Reads CSVs and generates 6 PNG graphs. |

### Test Tiers

| Tier | What it tests | Connections | Duration |
|---|---|---|---|
| Tier 1 — Scalability | Latency + throughput at increasing load | 100, 500, 1000, 5000, 10000 | 15s each |
| Tier 2 — Sustained | Stability over time, memory leak detection | 1000 | 60s |
| Tier 3 — Failover | Behaviour when a backend is killed mid-test | 500 | 30s |

### Results

| Connections | Throughput | p95 Latency | Failed |
|---|---|---|---|
| 100 | 27.87 MB/s | 7.0 ms | 0 |
| 500 | 26.89 MB/s | 37.8 ms | 0 |
| 1,000 → 10,000 | — | — | **0** |
| Failover (before kill) | 29.16 MB/s | 33.8 ms | 0 |
| Failover (after kill) | 28.07 MB/s | 34.7 ms | 0 |

> The gateway accepted and held 10,000 simultaneous connections with zero failures. Latency measurement at that scale was limited by the Python benchmark tool (not the gateway).

### How to run

```bash
cd ~/Systematic_Chaos
ulimit -n 65535           # allow enough open file descriptors
python3 tests/benchmark/orchestrator.py
python3 tests/benchmark/plot_graphs.py
```

---

## 10. Build & Run

### Prerequisites
- Linux (Ubuntu/WSL)
- CMake
- g++ (C++17)

### Build

```bash
cd ~/Systematic_Chaos
mkdir build
cd build
cmake ..
make
```

### Run

```bash
./build/traffic_gateway config/config.json
```

### Hot Reload (no downtime)

```bash
kill -SIGUSR1 $(pgrep traffic_gateway)
```

### View Dashboard

Open your browser at: `http://localhost:8080`
