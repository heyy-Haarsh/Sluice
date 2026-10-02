# UDP Support — Implementation Steps

Step-by-step build order for the design in [`UDP_Plan.md`](UDP_Plan.md). Read that
first for *why*; this doc is *how*, in order. Each step lists **Files / Do /
Verify**. Milestones A–C get you a working UDP proxy; D onward hardens it.

**Baked-in decisions** (the recommendations from `UDP_Plan.md` §12 — change here if
you disagree):
- Rate limiting = per-IP **new-session** rate (reuses the existing limiter).
- UDP backend health = **optimistic** in phase 1 (no active probe); passive
  `ECONNREFUSED` handling in phase 2.
- **One route = one protocol** (a TCP+UDP service on the same port = two routes).
- Session cap = a **separate `max_udp_sessions`** (UDP economics differ from TCP).
- PROXY protocol for UDP = **deferred** (phase 3 / skip).

> All UDP relay code lives in `gateway.cpp` — it needs the anonymous-namespace
> helpers there (`epoll_add`, `peer_endpoint`, `select_backend`, access control).
> Keep the TCP path untouched; UDP is additive.

---

## Milestone A — Config (compiles, no behavior change)

### Step 1 — Route protocol + UDP timeout in the schema
**Files:** `config.hpp`
- Add to `Route`: `enum class Proto { Tcp, Udp };` (or `string protocol`) defaulting
  to TCP; `int udp_session_timeout_seconds = -1;` (–1 = inherit global).
- Add to `GatewayConfig`: `int udp_session_timeout_seconds = 30;` and
  `int max_udp_sessions = 10000;`.
- `Backend` is unchanged.

### Step 2 — Parse + validate
**Files:** `config.cpp`
- Parse `"protocol"` (accept `"tcp"`/`"udp"`, default tcp; reject others).
- Parse route + global `udp_session_timeout_seconds` and `max_udp_sessions` (≥ 0;
  enforce a **minimum floor** for a non-zero UDP timeout, e.g. 5s, so sessions
  can't leak).
- **Change the duplicate-port check** so uniqueness is per **(protocol, port)** —
  `tcp:9000` and `udp:9000` must both be allowed.
- (Optional) if `proxy_protocol: true` on a UDP backend, log a warning that it's
  ignored.

**Verify:** existing TCP configs still parse identically; a UDP route parses;
`tcp:9000` + `udp:9000` coexist; a bad protocol value is rejected with a clear error.

---

## Milestone B — UDP listen sockets + state

### Step 3 — `UdpSession` + `GatewayState` maps
**Files:** new `udp_session.hpp`; `gateway.cpp`
- Define `UdpSession { sockaddr_storage client_addr; socklen_t client_len;
  string client_ip; int client_listen_fd; int backend_fd; Backend* backend;
  string route_name; int idle_timeout_seconds; steady_clock::time_point last_activity; };`
- Add to `GatewayState`:
  ```cpp
  unordered_map<int, Route*>       udp_listen_fd_to_route;
  unordered_map<int, UdpSession*>  udp_backend_fd_to_session;
  unordered_map<UdpKey, UdpSession*> udp_sessions;   // (listen_fd, client ip, port)
  unordered_set<UdpSession*>       live_udp_sessions;
  int max_udp_sessions = 0;
  ```
  Define `UdpKey { int listen_fd; array<uint8_t,16> ip; uint16_t port; }` + a hash
  (or a `string` key `"fd|ip|port"` for simplicity first).

### Step 4 — Create UDP listen sockets
**Files:** `gateway.cpp`
- Generalize `create_listening_socket(int port, int socktype = SOCK_STREAM)`:
  for `SOCK_DGRAM`, do the same dual-stack `SO_REUSEADDR`/`SO_REUSEPORT`/
  `IPV6_V6ONLY`/`bind`/non-blocking — but **skip `listen()`**.
- In `run_gateway` startup and (later) `apply_reload`, branch on `route.protocol`:
  TCP → `create_listening_socket(port, SOCK_STREAM)` into `listen_fd_to_route`;
  UDP → `create_listening_socket(port, SOCK_DGRAM)` into `udp_listen_fd_to_route`;
  add both to epoll with `EPOLLIN`.

**Verify:** a UDP route binds its port (`ss -ulnp` shows it); startup logs it; TCP
routes unaffected.

---

## Milestone C — Core relay (working UDP proxy)

### Step 5 — Client→backend: `handle_udp_client_datagrams`
**Files:** `gateway.cpp`
- On `EPOLLIN` for a UDP listen fd, loop `recvfrom(listen_fd, buf[65536], …, &client_addr)`
  until `EAGAIN`. For each datagram:
  1. `peer_endpoint(client_addr, client_ip, client_port)`; build the `UdpKey`.
  2. **Look up the session.** If found → reuse it.
  3. If not found → **create it** (mirrors `handle_new_clients`):
     - `max_udp_sessions` cap check (drop if at cap);
     - `allowed_global(client_ip)` (drop if denied) + `record_global`;
     - `select_affinity_backend` / `select_backend` (drop datagram if none) +
       `record_backend`;
     - open a **connected** backend UDP socket (new helper
       `open_udp_backend_socket(host, port)` = `socket(SOCK_DGRAM)` + non-blocking
       + `connect()`, family from the host literal like `open_nonblocking_connect`);
     - fill `UdpSession`, insert into `udp_sessions`, `udp_backend_fd_to_session`,
       `live_udp_sessions`; `epoll_add(backend_fd, EPOLLIN)`;
     - `backend->active_connections++`, `total_connections++`.
  4. Forward: `send(session->backend_fd, buf, n, 0)` (drop on `EAGAIN`; on
     `ECONNREFUSED` destroy the session — phase 2 refines this). Bump `last_activity`,
     `backend->bytes_relayed += n`.

### Step 6 — Backend→client: `handle_udp_backend_reply`
**Files:** `gateway.cpp`
- On `EPOLLIN` for a backend fd → `udp_backend_fd_to_session[fd]` → `recv(fd, buf, 65536)`.
- `sendto(session->client_listen_fd, buf, n, 0, &session->client_addr, session->client_len)`
  — **replies go out the listen socket**, never the backend socket.
- Bump `last_activity`, `bytes_relayed`. Handle `recv` error/`ECONNREFUSED` → destroy session.

### Step 7 — Wire into the event loop
**Files:** `gateway.cpp` (main `for` over epoll events)
- Add two dispatch branches **before** the `fd_to_connection` lookup:
  `udp_listen_fd_to_route.count(fd)` → step 5; `udp_backend_fd_to_session.count(fd)`
  → step 6. Respect the existing `pending_close`/deferred-cleanup ordering (UDP
  session destroy can be immediate or deferred — keep it consistent with TCP: mark
  and sweep, or destroy inline since UDP fds aren't reused mid-batch the same way).

**Verify (MVP!):** with a UDP echo backend and `nc -u <gw> 9000`, typing a line
echoes back. Two different client source ports create two sessions; the dashboard's
active-connection counter for the backend goes up.

---

## Milestone D — Lifecycle (mandatory for UDP)

### Step 8 — Session idle timeout + destroy helper
**Files:** `gateway.cpp`
- Add `destroy_udp_session(gs, session)`: `epoll_ctl(DEL, backend_fd)`, `close(backend_fd)`,
  erase from all three maps + `live_udp_sessions`, `backend->active_connections--`,
  `delete`. **If the backend is `retiring` and its last session just ended, erase the
  drained backend from `gs.routes` under `routes_mtx`** — mirror `cleanup_connection`.
- Extend the 1s idle sweep (`check_idle_connections`) to also walk `live_udp_sessions`
  and destroy any whose `last_activity` is older than its resolved timeout. Resolve
  the timeout at creation: `route.udp_session_timeout_seconds >= 0 ? route… : global`.

### Step 9 — Session cap
**Files:** `gateway.cpp`
- Set `gs.max_udp_sessions` from config at startup and in `apply_reload`.
- Enforce in step 5 before creating a session (already noted there).

**Verify:** an idle UDP session is reclaimed after the timeout (`ss -ulnp` / fd count
drops); exceeding `max_udp_sessions` refuses new sessions while existing ones keep working.

---

## Milestone E — Policy reuse + health + stats

### Step 10 — Confirm policy reuse
**Files:** `gateway.cpp` (review, mostly done in step 5)
- Verify access control, affinity, LB, and per-IP rate-limit accounting all run on
  session creation. Confirm `bytes_relayed` accrues both directions and
  `active_connections`/`total_connections` reflect sessions.

### Step 11 — Skip UDP backends in heartbeats
**Files:** `gateway.cpp` → `run_heartbeats`
- Skip backends belonging to UDP routes (a TCP connect probe is meaningless for UDP).
  Their `healthy` stays optimistically true. (Phase 2: passive `ECONNREFUSED` in
  steps 5/6 can flip health with a small failure threshold.)

**Verify:** no spurious probe sockets are opened for UDP backends; UDP relay still works.

---

## Milestone F — Hot reload, shutdown, multi-process

### Step 12 — Hot reload
**Files:** `gateway.cpp` → `apply_reload`
- Route diff must match on **(protocol, name/port)**. New UDP route → create+epoll the
  UDP listen socket. Removed UDP route → close its listen fd (stop new datagrams) and
  let its sessions **drain via the idle timeout** (don't force-close). Refresh
  route-level `udp_session_timeout_seconds`; refresh `gs.max_udp_sessions`.
- Retiring UDP backend: mark `retiring`; new sessions skip it; existing sessions
  finish and are erased by `destroy_udp_session` (step 8).

### Step 13 — Shutdown
**Files:** `gateway.cpp` → `shutdown_all`
- Close all UDP listen fds and all live session backend fds; clear the UDP maps.

### Step 14 — Multi-process
**Files:** none (verification only)
- `SO_REUSEPORT` is already set. Confirm the kernel steers each UDP flow to one worker
  consistently (so a session stays on one worker). Note a worker restart drops its
  sessions; clients re-establish on the next datagram.

**Verify:** add/remove a UDP route via reload with no crash and no dropped in-flight
sessions; clean shutdown; with `worker_processes > 1`, one client's flow stays on one worker.

---

## Milestone G — Dashboard / UI

### Step 15 — API
**Files:** `dashboard.cpp`
- Add `"protocol": "tcp"|"udp"` to each route in `/api/status` (and count UDP session
  bytes/sessions into `/api/health` aggregates — they already flow through
  `backend.active_connections`/`bytes_relayed`, so this is mostly the protocol field).
- Add `"health_checked": false` for UDP backends so the UI can render a neutral
  health cell instead of a misleading green/red.
- Keep the `routes_mtx` discipline (already held when walking the route list).

### Step 16 — Web UI
**Files:** `dashboard_ui.hpp`
- Status table: a **TCP/UDP badge** per route; label the count column "Sessions" for
  UDP rows; neutral health indicator for UDP backends.
- Config editor: a **protocol dropdown** per route; when UDP, show a **Session
  timeout (s)** field and **hide** `proxy_protocol` + heartbeat toggles (or disable
  with a "not applicable to UDP" note). Keep access-control + affinity panels.
- Add the "UDP is connectionless / IP rules are advisory (spoofable)" inline note.

**Verify:** a UDP route shows a UDP badge and live session counts; editing protocol +
timeout in the UI round-trips through save→reload.

---

## Milestone H — Tests, helpers, configs, docs

### Step 17 — UDP echo backend helper
**Files:** `tests/backends/udp_echo.py`
- `SOCK_DGRAM` server: `recvfrom` → `sendto` back; prints the client it sees. Binds
  IPv4/IPv6 from the host arg (like `echo_any.py`).

### Step 18 — Test document
**Files:** `tests/udp-tests.md`
- Cover: basic relay (`nc -u`), LB across backends by distinct client ports, session
  stickiness, idle-timeout reclamation (fd count drops), access-control block, dual-stack
  (`nc -u -6`), session cap, multi-process flow coherence.

### Step 19 — Demo/sample config
**Files:** `config/…`
- A config with both a TCP and a UDP route (e.g. `tcp:9000` web + `udp:9001` echo/game)
  to show they coexist.

### Step 20 — Docs
**Files:** `README.md`, `sluice_project_reference.md`
- Add UDP to the feature list and a short "UDP routing" section (session model, idle
  timeout, IP-rule caveat).

---

## Precautions checklist (cross-ref `UDP_Plan.md` §8)

- [ ] `recvfrom` buffer is 65536; detect `MSG_TRUNC` and drop oversize datagrams.
- [ ] Replies always `sendto` the **listen** fd with the stored raw `client_addr`.
- [ ] UDP idle timeout can't be 0/disabled without a floor (sessions would leak).
- [ ] `max_udp_sessions` enforced — the primary defense against spoofed-source floods.
- [ ] One fd per session — watch the fd budget; `ulimit -n` in prod / systemd `LimitNOFILE`.
- [ ] `destroy_udp_session` takes `routes_mtx` only when erasing a drained *retiring* backend.
- [ ] Document clearly: UDP source IPs are spoofable → IP ACL/rate-limit are advisory.
- [ ] `run_heartbeats` skips UDP backends; dashboard shows their health as "not checked".

---

## Deferred (later phases)
- Passive health via `ECONNREFUSED` (phase 2).
- `proxy_responses`-style "expected replies → end session early" (nice-to-have, phase 2).
- Per-flow datagram-rate limiting (phase 2).
- PROXY protocol v2 for UDP (phase 3).
