# UDP Support — Design Plan

Goal: route **UDP** traffic to backends alongside the existing **TCP** support,
reusing as much of the current engine (LB, access control, dashboard, hot-reload,
multi-process, IPv6) as possible.

This is a design/planning document — no code has been changed.

---

## 1. The one idea that drives everything: UDP is connectionless

TCP support in Sluice is built around a **connection**: `accept()` gives a
dedicated socket per client, a `Connection` pairs two fds, and the pair ends on
`FIN`. **None of that exists in UDP.**

| | TCP (today) | UDP |
|---|---|---|
| Per-client socket | yes (`accept()`) | **no** — one socket receives from all clients |
| Lifecycle | SYN … FIN | **none** — datagrams just arrive |
| Ordering / retransmit | yes | no (by design) |
| Message boundaries | stream (no boundaries) | **preserved** — 1 `recvfrom` = 1 datagram |
| Source IP | verified by handshake | **spoofable** |
| Flow control / backpressure | yes | no — excess is dropped |

**Consequence:** we must *synthesize* a "session" (a client↔backend flow) keyed by
the client's source address, and — because there is no `FIN` — an **idle timeout is
the only way a session ever ends**. Idle timeout goes from *optional* (TCP) to
*mandatory* (UDP).

So UDP is **not** "TCP minus the handshake." It's a different relay model that
reuses the *policy* layer (routing, LB, access) but not the *transport* layer
(`Connection`, buffers, half-close).

---

## 2. Architecture

```
                    ┌──────────────── one UDP listen socket per UDP route ───────────┐
   Client X ─datagram▶  udp_listen_fd (SOCK_DGRAM, bound :9000, in epoll EPOLLIN)     │
   Client Y ─datagram▶      │  recvfrom() → (data, client_addr)                        │
                            │  look up session by client_addr                          │
                            │    └─ new? → access-check → select_backend → make session │
                            ▼                                                           │
                    ┌── UdpSession (client_addr → Backend) ──┐                          │
                    │  udp_backend_fd (SOCK_DGRAM, connect()'d to backend, in epoll)    │
                    │  send(datagram) to backend ───────────────────────▶ Backend      │
                    │  backend reply → recv(udp_backend_fd) ◀────────────── Backend      │
                    │  sendto(udp_listen_fd, reply, client_addr) ──────▶ Client         │
                    └──────────────────────────────────────────────────────────────────┘
```

Key decisions:

- **One connected UDP socket per session** (`socket(SOCK_DGRAM)` + `connect()` to
  the backend). Why connected:
  1. it can be added to epoll, so backend replies wake the loop like any other fd;
  2. `recv()` only returns datagrams from *that* backend, so the fd that fires
     identifies the session (no ambiguous demux);
  3. the kernel surfaces ICMP "port unreachable" as `ECONNREFUSED` on it — the one
     usable health signal for UDP.
  Cost: **one fd per session** → must be capped (see §8).
- **Replies go out the *listen* socket**, not the backend socket: a UDP client only
  accepts a reply whose source is the `ip:port` it sent to. We `sendto(udp_listen_fd,
  …, session.client_addr)`.
- **No buffers, no backpressure, no half-close.** `recvfrom` gives one whole
  datagram; we `send` it as one datagram. If the send would block (`EAGAIN`) we
  **drop** it — that is correct UDP behaviour ("nobody's waiting").

---

## 3. New data structures

```cpp
// connection.hpp (or a new udp_session.hpp)
struct UdpSession {
    sockaddr_storage client_addr;   // where replies go (raw, for sendto)
    socklen_t        client_len;
    string           client_ip;     // normalized, for logs/access (peer_endpoint)
    int              backend_fd;     // connected SOCK_DGRAM socket to the backend
    Backend*         backend;        // for LB counters / stats
    string           route_name;
    int              idle_timeout_seconds;
    chrono::steady_clock::time_point last_activity;
};
```

`GatewayState` additions (mirroring the TCP maps):

```cpp
unordered_map<int, Route*>        udp_listen_fd_to_route;   // which UDP route owns a listen fd
unordered_map<int, UdpSession*>   udp_backend_fd_to_session;// backend reply fd → session
unordered_map<SessionKey, UdpSession*> udp_sessions;        // (route, client_addr) → session
unordered_set<UdpSession*>        live_udp_sessions;        // for the idle sweep + cap
```

`SessionKey` = `{ listen_fd, client_ip, client_port }` (or a packed 16-byte ip +
port). The listen fd disambiguates the same client hitting two UDP routes.

**Do not overload `Connection`** — it's TCP-shaped (two fds, deques, EOF flags).
`UdpSession` is smaller and separate.

---

## 4. Where it plugs into `gateway.cpp` (exact touch points)

1. **`create_listening_socket(int port)` → add a `type` param** (`SOCK_STREAM` /
   `SOCK_DGRAM`). For UDP: same dual-stack `SO_REUSEADDR`/`SO_REUSEPORT`/`IPV6_V6ONLY`
   setup and `bind()`, but **no `listen()`** and no backlog. Everything else (IPv6
   normalization via `peer_endpoint`) is reused.
2. **Startup + `apply_reload`:** for each route, branch on protocol — TCP routes go
   into `listen_fd_to_route`, UDP routes into `udp_listen_fd_to_route`.
3. **Main event-loop dispatch** (the `for` over epoll events): add two branches
   before the TCP `fd_to_connection` lookup —
   - `udp_listen_fd_to_route.count(fd)` → `handle_udp_client_datagrams(gs, fd, route)`
   - `udp_backend_fd_to_session.count(fd)` → `handle_udp_backend_reply(gs, fd)`
4. **New handlers:**
   - `handle_udp_client_datagrams`: `recvfrom` loop until `EAGAIN`; per datagram,
     find-or-create the session (reusing `select_affinity_backend` /
     `select_backend` / `allowed_global` / `record_global` exactly as
     `handle_new_clients` does), then `send()` to the backend fd.
   - `handle_udp_backend_reply`: `recv` the datagram (handle `ECONNREFUSED` →
     drop session), `sendto` it to the client via the listen fd, bump
     `last_activity` + byte counters.
5. **Idle sweep (`check_idle_connections`) → also sweep `live_udp_sessions`** and
   destroy expired ones (`close(backend_fd)`, erase from all three maps). This is
   the only reclamation path for UDP.
6. **`max_connections` check** applies to `live_udp_sessions.size()` too (or a
   dedicated `max_udp_sessions`).
7. **`run_heartbeats` skips UDP backends** (see §6 — a TCP connect probe is
   meaningless for UDP).
8. **`shutdown_all`** closes all UDP listen + session fds.
9. **`apply_reload` route diff** keys on **(protocol, listen_port)**; UDP routes
   removed → close listen fd, let sessions expire (draining).

The `routes_mtx` discipline (held while mutating the route/backend lists so the
dashboard thread can read safely) is unchanged and covers UDP routes too.

---

## 5. Config schema changes (`config.hpp` / `config.cpp`)

```jsonc
{
  "routes": [
    {
      "name": "game",
      "protocol": "udp",                 // NEW: "tcp" (default) | "udp"
      "listen_port": 9000,
      "udp_session_timeout_seconds": 30, // NEW: per-route; falls back to a global default
      "backends": [ { "host": "127.0.0.1", "port": 7001 } ]
    }
  ],
  "udp_session_timeout_seconds": 30      // NEW: global default (mandatory floor, e.g. min 5s)
}
```

- `Route.protocol` (enum, default `tcp`). `Backend` struct is **unchanged**.
- **Duplicate-port validation must change**: uniqueness is now per **(protocol,
  port)** — `tcp:9000` and `udp:9000` are legitimately different sockets.
- `proxy_protocol` on a UDP backend → **warn and ignore** in phase 1 (see §6).
- A global `udp_session_timeout_seconds` with a sane default (e.g. 30s) and a
  minimum floor, because a disabled UDP timeout leaks sessions forever.

---

## 6. Feature applicability — what reuses, what changes, what drops

| Feature | UDP status | Notes |
|---|---|---|
| **Load balancing** (`select_backend`) | ✅ reuse | Chosen **once per session** (first datagram), then sticky for the session's life — which is usually what UDP callers want anyway. |
| **Session affinity** (`select_affinity_backend`) | ✅ reuse | Same call at session creation. |
| **Access control** — whitelist/blacklist/CIDR | ✅ reuse | `allowed_global` / `allowed_backend` on the source IP, checked per new session. |
| **Rate limiting** | ⚠️ reinterpret | Today it counts *connections*/min per IP. For UDP, count **new sessions**/min (natural), and/or add optional **datagrams/sec** limiting. **Caveat: spoofable source IPs make IP-based limits weak for UDP** (§8). |
| **Health checks / failover** | ❌ active N/A | A TCP `connect()` probe means nothing for UDP (`connect()` on a datagram socket never contacts the peer). Options: (a) UDP backends are optimistically healthy (default); (b) **passive**: treat `ECONNREFUSED` on the session's backend socket as a failure signal; (c) optional companion TCP health port. `run_heartbeats` must skip UDP. |
| **Backpressure / bounded buffers** | ❌ N/A | No output buffering — drop on `EAGAIN`. Memory is bounded by the **session cap** instead of the byte high-water mark. |
| **Half-close / graceful FIN** | ❌ N/A | UDP has no close. |
| **Idle timeout** | ✅ **mandatory** | The only way a session ends; reuses the 1s sweep timer. |
| **PROXY protocol** | ⚠️ defer | v1 is TCP-only. v2 has UDP address types but must be prepended per-datagram (or first datagram) and backend support is rare. Phase 3. |
| **Multi-process (`SO_REUSEPORT`)** | ✅ works | Linux hashes UDP datagrams by flow, so **all of one client's datagrams hit the same worker** → the session (which lives in one worker) stays coherent. A worker restart loses its sessions; clients re-establish on the next datagram. |
| **IPv6 / dual-stack** | ✅ reuse | Same dual-stack bind + `peer_endpoint` normalization. |
| **Hot reload + draining** | ✅ adapt | Remove UDP route → close listen fd, let sessions expire (no forced drop). |
| **`max_connections` cap** | ✅ reuse/extend | Apply to `live_udp_sessions` (or a separate `max_udp_sessions`). |
| **Metrics** | ⚠️ rename | "active_connections" → active **sessions**; "total_connections" → total sessions; bytes_relayed still valid (datagram bytes). |

---

## 7. UDP-specific features to add

- **Session timeout** (mandatory, per-route + global default, with a minimum floor).
- **Session table + cap** — the primary memory/fd safety mechanism for UDP.
- **(Optional) datagram-rate limiting** — a per-IP or per-session packets/sec cap,
  since a single UDP flow can flood without opening new sessions.
- **Automatic stickiness** — inherent in the session model (no config needed).
- **(Optional) passive health** via `ECONNREFUSED` on backend sockets.

---

## 8. Concerns & precautions

**Security**
- **Source-IP spoofing:** UDP has no handshake, so an attacker can forge source IPs
  at will. IP whitelist/blacklist and per-IP rate limits are therefore *weaker* for
  UDP than TCP — call this out in docs. A spoofed-source flood also **creates a new
  session per fake IP** → this is why the **session cap is mandatory**, not optional.
- **Reflection / amplification:** if a backend returns more than it receives, Sluice
  could be abused as an amplifier. Session cap + datagram-rate limiting + not
  b:inding the dashboard/admin publicly all help; note the risk.

**Resource safety**
- **Session/fd exhaustion:** one backend fd per session → a flood exhausts fds fast.
  Mitigate with (a) mandatory idle timeout, (b) a hard session cap, (c) a short
  default timeout for UDP (30s, not 0).
- **Alternative considered & rejected:** a *single* unconnected backend socket with
  `sendto`/`recvfrom` demux would save fds, but replies can't be attributed to a
  session (multiple sessions to the same backend look identical), and you lose the
  ICMP error signal. The per-session connected socket is the correct trade-off.

**Correctness**
- **Buffer size:** `recvfrom` into a 65536-byte buffer (max UDP payload is 65507);
  detect truncation via `MSG_TRUNC` and drop oversize datagrams with a log line.
- **Reply source address:** must be the listen socket / original destination, or the
  client silently drops the reply. Store the client `sockaddr_storage` in the session.
- **Backend down at session start:** if `select_backend` returns none, drop the
  datagram (optionally rate-limited log). If a backend dies mid-session, its
  datagrams are simply lost (UDP) until the session idles out or `ECONNREFUSED` kills
  it; the client's next flow re-selects a healthy backend.
- **Dual-stack:** `sendto` to a stored IPv4-mapped client address works; keep the raw
  `sockaddr` from `recvfrom` rather than reconstructing from the normalized string.

---

## 9. Dashboard / UI-UX

**API (`/api/status`, `dashboard.cpp`)**
- Add `"protocol": "tcp" | "udp"` to each route object.
- For UDP routes, `active_connections` / `total_connections` now mean **sessions**;
  keep the JSON keys but the UI should label them "Sessions" for UDP rows.
- UDP backends have no meaningful `healthy` from active probing — send a flag like
  `"health_checked": false` so the UI can render "—" instead of a misleading green.

**Status view**
- A **protocol badge** per route: `TCP` / `UDP` chip next to the route name.
- Column header adapts per row: "Active" reads as connections (TCP) vs sessions
  (UDP). A tooltip clarifies.
- Health cell for UDP backends: neutral indicator ("no active health check") rather
  than up/down, unless passive-health is enabled.

**Config editor**
- **Protocol dropdown** per route (`TCP` default / `UDP`).
- When `UDP` is selected: show a **Session timeout (s)** field; **hide** the
  `proxy_protocol` toggle and the heartbeat/health toggles (or show them disabled
  with a "not applicable to UDP" note). Access-control and affinity panels stay.
- A small inline note on UDP rows: "connectionless — sessions expire after the idle
  timeout; IP rules are advisory since UDP source IPs can be spoofed."

**Terminology:** in UDP contexts, say **"sessions"/"flows"**, not "connections."

---

## 10. Testing plan (sketch)

- **Helper:** a UDP echo backend (`SOCK_DGRAM`, `recvfrom` → `sendto` back). Client:
  `nc -u`.
- Basic relay: `nc -u` → gateway → backend echoes → reply returns.
- LB: several distinct client source ports → sessions spread across backends; same
  client stays on its backend (stickiness).
- Idle expiry: open a session, go quiet past the timeout → session (and its fd) is
  reclaimed (verify fd count drops).
- Access control: blacklist a source IP → its datagrams are dropped.
- Dual-stack: IPv6 `nc -u -6` client reaches an IPv4 backend.
- Multi-process: with `worker_processes > 1`, one client's flow stays on one worker.
- Cap: exceed `max_udp_sessions` → new sessions refused, existing unaffected.

---

## 11. Suggested phasing

- **Phase 1 (MVP):** `protocol` flag + config parse/validation; UDP listen socket;
  session table + connected backend socket; bidirectional relay; **mandatory idle
  timeout**; LB + affinity + access control per session; dual-stack; session cap;
  dashboard protocol badge + session counts. Skip UDP backends in heartbeats.
- **Phase 2:** passive `ECONNREFUSED` health; multi-process validation; datagram-rate
  limiting; dashboard config-editor UDP fields + adapted health cell.
- **Phase 3 (optional):** PROXY protocol v2 for UDP; active app-level UDP health
  probes (send a configured probe payload, expect a reply).

---

## 12. Open decisions (need your call before implementing)

1. **Rate-limit semantics for UDP:** per-IP *new-session* rate (simplest, reuses the
   existing limiter) — or also per-flow datagrams/sec? (Recommend: new-session for
   phase 1.)
2. **Health for UDP backends:** optimistic-only (phase 1), passive `ECONNREFUSED`, or
   an optional TCP companion health port? (Recommend: optimistic phase 1, passive phase 2.)
3. **One route = one protocol** (recommended; use two routes for a TCP+UDP service on
   the same port) — or a single route serving both?
4. **Cap:** reuse `max_connections` for the combined total, or a separate
   `max_udp_sessions`? (Recommend: separate, since UDP session economics differ.)
5. **PROXY protocol for UDP:** phase 3 / skip — confirm it's not needed for judging.
