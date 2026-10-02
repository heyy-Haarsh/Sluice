# Sluice — Test Cases

**Owner:** Harshad Chavan  
**Project:** Systematic Chaos (hackathon)  
**Codebase:** `~/Systematic_Chaos` (WSL Ubuntu)

---

## How to use this document

Every test case follows the same structure:
- **Feature tested** — what specific behavior is being verified
- **Steps** — exact terminal commands, numbered by terminal (T1 = first terminal, T2 = open a new one, etc.)
- **Expected output** — what you should see when it works
- **Pass / Fail criteria** — how to decide the result
- **What could go wrong** — common failure modes and what they indicate

### Build first (do this once)
```bash
# Inside WSL, from the project root ~/Systematic_Chaos/
mkdir -p build && cd build
cmake ..
cmake --build .
cd ..
```

---

## File Map

```
tests/
  TEST_CASES.md                        ← this file
  configs/
    tc_single_route.json               ← :9000 → :7001, heartbeat off
    tc_two_routes.json                 ← :9100→:7101 and :9101→:7102
    tc_three_backends.json             ← :9200 → [:7201, :7202, :7203]
    tc_heartbeat.json                  ← heartbeat on (2s), two backends
    tc_invalid_port.json               ← listen_port: 0 (invalid)
    tc_empty_name.json                 ← route name: "" (invalid)
    tc_dup_name.json                   ← two routes with same name (invalid)
    tc_hb_zero_interval.json           ← for manual unknown-key test
  backends/
    large_response_backend.py          ← sends large deterministic payload (TC-09)
    counting_backend.py                ← unique reply per connection (TC-10)
    drop_backend.py                    ← crashes after N bytes (TC-14)
    tagged_echo_backend.py             ← echo prefixed with a tag (TC-18, TC-19)

  scripts/
    tc08_routing_isolation.py          ← automated
    tc09_data_integrity.py             ← automated
    tc10_concurrent.py                 ← automated
    tc18_load_balancing.py             ← automated
    tc19_lb_failover.py                ← automated

config/                                ← existing project configs
  config.json                          ← 3 routes (payments/inventory/orders)
  bad_syntax.json                      ← malformed JSON          (TC-02)
  missing_field.json                   ← missing listen_port     (TC-03)
  empty_backends.json                  ← backends: []            (TC-04)
  dup_port.json                        ← duplicate listen_port   (TC-05)
  lb_config.json                       ← multi-backend example

test_utils/                            ← existing project utilities
  echo_backend.py                      ← simple echo             (many TCs)
  slow_backend.py                      ← reads with 20ms delay   (TC-11)
  halfclose_backend.py                 ← replies after client FIN (TC-12)
```

---

## Group 1 — Config Validation (TC-01 to TC-07)

No network connections needed for these. They just start the gateway
with a bad config and verify it exits with the right error.

---

### TC-01 — Valid Config Loads Successfully

**Feature:** Gateway starts cleanly from a valid config and enters the event loop.

**Steps:**
```bash
# [T1]
./build/sluice config/config.json
```

**Expected output:**
```
Sluice: loaded 3 route(s)
  route "payments"   listen :9000  ->  ...
  route "inventory"  listen :9001  ->  ...
  route "orders"     listen :9002  ->  ...
listening on :9000 for route "payments"
listening on :9001 for route "inventory"
listening on :9002 for route "orders"
heartbeat: probing idle backends every 10s
Sluice: event loop started, 3 route(s) active
```
Gateway stays running (does not exit).

**Pass:** 3 routes printed, process stays alive.  
**What could go wrong:** `Address already in use` → another process is using port 9000/9001/9002. Run `ss -tlnp | grep 900` to find and kill it.

---

### TC-02 — Malformed JSON

**Feature:** Parser rejects syntactically broken JSON with a clear error.

```bash
# [T1]
./build/sluice config/bad_syntax.json
```

**Expected output:**
```
config error: invalid JSON in: ...
```
**Pass:** Process exits (non-zero), error mentions "invalid JSON".

---

### TC-03 — Missing Required Field

**Feature:** Parser detects missing mandatory fields.

```bash
# [T1]
./build/sluice config/missing_field.json
```

**Expected:**
```
config error: route[0]: missing required field "listen_port"
```
**Pass:** Process exits, error names the missing field and route index.

---

### TC-04 — Empty `backends` Array

**Feature:** Parser rejects a route with no backends.

```bash
# [T1]
./build/sluice config/empty_backends.json
```

**Expected:**
```
config error: route[0]: "backends" must be a non-empty array
```

---

### TC-05 — Duplicate `listen_port`

**Feature:** Parser rejects two routes sharing the same listening port.

```bash
# [T1]
./build/sluice config/dup_port.json
```

**Expected:**
```
config error: duplicate listen_port 9000 (used by more than one route)
```

---

### TC-06 — Port Out of Range

**Feature:** Parser validates port numbers are in 1–65535.

```bash
# [T1]
./build/sluice tests/configs/tc_invalid_port.json
```

**Expected:**
```
config error: route[0]: "listen_port" 0 out of range (1-65535)
```

---

### TC-07 — Empty Route Name / Duplicate Route Name

**Feature:** Parser rejects empty names and duplicate names.

```bash
# [T1] — empty name
./build/sluice tests/configs/tc_empty_name.json
# Expected: config error: route[0]: "name" must not be empty

# [T1] — duplicate name
./build/sluice tests/configs/tc_dup_name.json
# Expected: config error: duplicate route name "payments"
```

---

### TC-07b — Heartbeat Config Validation

**Feature:** Heartbeat block is validated (interval_seconds > 0).

```bash
# [T1] — interval_seconds: 0 should be rejected
# First edit tests/configs/tc_hb_zero_interval.json to set:
#   "heartbeat": { "enabled": true, "interval_seconds": 0 }
./build/sluice tests/configs/tc_hb_zero_interval.json
```

**Expected:**
```
config error: heartbeat "interval_seconds" must be > 0
```

---

## Group 2 — Core Relay & Routing (TC-08 to TC-12)

---

### TC-08 — Routing Isolation (Multiple Routes)

**Feature:** Each listening port routes strictly to its own backend. Traffic on one port never leaks to another.

```bash
# [T1]
python3 test_utils/echo_backend.py 7101

# [T2]
python3 test_utils/echo_backend.py 7102

# [T3]
./build/sluice tests/configs/tc_two_routes.json

# [T4] — automated check
python3 tests/scripts/tc08_routing_isolation.py
```

**Expected [T4]:**
```
[TC-08] Routing isolation test
  port 9100: PASS — echoed b'secret-for-service-A'
  port 9101: PASS — echoed b'secret-for-service-B'

[TC-08] PASS
```

**Pass:** Script exits 0.  
**What could go wrong:**
- `FAIL — cannot connect` → gateway or backend not started.
- One port echoes the other's message → routing is broken (listen_fd_to_route lookup has a bug).

---

### TC-09 — Data Integrity / Large Transfer

**Feature:** The relay does not corrupt, truncate, or reorder bytes across many `recv`/`send` cycles, even for MB-scale transfers.

```bash
# [T1]
python3 tests/backends/large_response_backend.py 7001 2097152

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3] — automated check
python3 tests/scripts/tc09_data_integrity.py 9000 2097152
```

**Expected [T3]:**
```
[TC-09] Connecting to gateway 127.0.0.1:9000
[TC-09] Expecting 2,097,152 bytes
[TC-09] Received 2,097,152 bytes
[TC-09] PASS — all 2,097,152 bytes received correctly, no corruption
```

**Pass:** Script exits 0.  
**What could go wrong:**
- `size mismatch` → gateway truncated data. A buffer was dropped when the kernel was full.
- `corruption at byte N` → relay is overwriting a buffer in place.
- Script hangs → backend closed before it sent all bytes (check T1 output).

---

### TC-10 — Concurrent Connections

**Feature:** Single-threaded epoll handles many simultaneous clients fairly without mixing streams.

```bash
# [T1]
python3 tests/backends/counting_backend.py 7001

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3] — 20 concurrent clients
python3 tests/scripts/tc10_concurrent.py 20 9000
```

**Expected [T3]:**
```
[TC-10] Spawning 20 concurrent clients → :9000
  client-00: PASS  conn-1 ok received 24 bytes
  ...
  client-19: PASS  conn-20 ok received 24 bytes

[TC-10] PASS — all 20/20 clients succeeded
```

**Pass:** Script exits 0.  
**What could go wrong:**
- Some clients fail with `Connection refused` → gateway's accept loop doesn't drain all pending clients per epoll event.
- Wrong byte count in some replies → stream mixing between connections (fd_to_connection lookup bug).
- Script hangs → a connection was accepted but never cleaned up.

---

### TC-11 — Backpressure (Slow Backend)

**Feature:** When the backend can't absorb data fast enough, the gateway buffers bytes and drains them gradually — no data dropped, no crash.

```bash
# [T1]
python3 test_utils/slow_backend.py 7001

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3] — send 500 KB
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 9000), timeout=60)
data = b'X' * (500 * 1024)
s.sendall(data)
s.shutdown(socket.SHUT_WR)
s.close()
print('client: sent', len(data), 'bytes')
"
```

**Expected:**
```
[T1] slow backend received total bytes: 512000
[T3] client: sent 512000 bytes
```

**Pass:** Both terminals print the correct byte count. No crash in gateway.  
**What could go wrong:**
- Byte count < 512000 → data was dropped when the backend's receive buffer was full (backpressure not working).
- Gateway at 100% CPU → `EPOLLOUT` is never deregistered after the buffer drains (busy-loop on writable socket).
- `send to fd=N failed` in gateway → hard write error is being triggered incorrectly.

---

### TC-12 — Graceful Half-Close

**Feature:** When client sends FIN (done sending), the gateway propagates it to the backend but keeps the backend→client channel alive so the response can still flow back.

```bash
# [T1]
python3 test_utils/halfclose_backend.py 7001

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3]
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 9000), timeout=10)
s.sendall(b'hello-halfclose')
s.shutdown(socket.SHUT_WR)   # FIN — done sending, still receiving
reply = b''
while True:
    chunk = s.recv(4096)
    if not chunk: break
    reply += chunk
s.close()
print('client received:', reply)
"
```

**Expected [T1]:**
```
backend got: b'hello-halfclose'
```
**Expected [T3]:**
```
client received: b'reply-after-your-eof\n'
```

**Pass:** Client receives the reply AFTER it sent FIN.  
**What could go wrong:**
- Client receives nothing → gateway closed the backend connection when it saw the client's FIN (handle_eof incorrectly calls mark_close instead of propagating shutdown).
- Client program hangs forever → maybe_finish never fires because eof/buffer state is inconsistent.

---

## Group 3 — Resilience & Lifecycle (TC-13 to TC-17)

---

### TC-13 — Backend Unreachable at Connect Time

**Feature:** Gateway drops the client cleanly and logs an error when no backend is listening. It must not crash or stop accepting other connections.

```bash
# [T1] — start gateway with NO backend running on :7001
./build/sluice tests/configs/tc_single_route.json

# [T2]
nc 127.0.0.1 9000
# (nc should exit immediately with nothing)
```

**Expected [T1]:**
```
backend connect failed for route "test-service" (127.0.0.1:7001): Connection refused — dropping client fd=N
```

**Pass:** Gateway prints error, stays running. nc exits. Port is still accepting (try nc again — same error, still alive).  
**What could go wrong:**
- Gateway crashes → error path in handle_new_clients has a null pointer or unhandled exception.
- nc hangs → client_fd was not closed after the backend connect failed.

---

### TC-14 — Backend Crashes Mid-Connection

**Feature:** If a backend abruptly disconnects while relaying, the gateway tears down the paired client connection cleanly and keeps running.

```bash
# [T1] — backend that crashes after 16 bytes
python3 tests/backends/drop_backend.py 7001 16

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3]
python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1', 9000), timeout=10)
s.sendall(b'first-chunk-here')   # 16 bytes, backend drops after this
time.sleep(0.3)
try:
    s.sendall(b'more data')
    reply = s.recv(4096)
    print('received:', reply)
except Exception as e:
    print('connection closed by gateway (expected):', type(e).__name__)
s.close()
"

# [T4] — verify gateway still works after the crash
nc 127.0.0.1 9000
# nc should connect and immediately disconnect (backend is gone, but gateway is alive)
```

**Expected [T2] (gateway):**
```
route "test-service": recv on fd=N failed: Connection reset by peer, tearing down pair
```
or:
```
route "test-service": send to fd=N failed: Broken pipe, tearing down pair
```

**Pass:** Gateway logs error, stays running, accepts T4's connection.  
**What could go wrong:**
- Gateway crashes → cleanup_connection has a bug (double-free, dangling pointer).
- Gateway stops accepting → teardown accidentally removed a listen fd from epoll.

---

### TC-15 — Graceful Shutdown via SIGINT (Ctrl+C)

**Feature:** SIGINT exits the gateway cleanly, closes all connections, and prints a count.

```bash
# [T1]
python3 test_utils/echo_backend.py 7001

# [T2]
./build/sluice tests/configs/tc_single_route.json

# [T3] — open an active connection and leave it open
nc 127.0.0.1 9000
# type: hello   (and press Enter to confirm relay works, then leave nc open)

# [T2] — press Ctrl+C
```

**Expected [T2]:**
```
^C
Sluice: shutting down (1 active connection(s) closed)
```
Back to shell prompt. T3's nc exits (receives EOF).

**Pass:** Gateway exits with return code 0, nc connection terminates.  
**What could go wrong:**
- Gateway doesn't respond to Ctrl+C → sigaction not set up correctly (check `SA_RESTART` is NOT set — if it is, epoll_wait resumes silently and g_stop is never checked).
- Gateway exits but port is still bound → shutdown_all didn't close the listen fds.
- Connection count wrong → fd_to_connection holds each connection under 2 keys so `/2` gives the pair count.

---

### TC-16 — Graceful Shutdown via SIGTERM

**Feature:** SIGTERM (used by systemd/process managers) exits the gateway cleanly.

```bash
# [T1]
./build/sluice tests/configs/tc_single_route.json &
GW_PID=$!
echo "gateway PID: $GW_PID"
sleep 1
kill -TERM $GW_PID
wait $GW_PID
echo "exit code: $?"
```

**Expected:**
```
gateway PID: NNNN
Sluice: event loop started, 1 route(s) active
...
Sluice: shutting down (0 active connection(s) closed)
exit code: 0
```

**Pass:** Exit code 0, no zombie process.

---

### TC-17 — Quick Restart (SO_REUSEADDR)

**Feature:** The gateway can restart immediately after being killed without "Address already in use" because all listening sockets use `SO_REUSEADDR`.

```bash
# [T1]
./build/sluice tests/configs/tc_single_route.json &
GW_PID=$!
sleep 0.5
kill $GW_PID
wait $GW_PID

# Immediately restart (port should be available)
./build/sluice tests/configs/tc_single_route.json
```

**Expected (second start):**
```
Sluice: loaded 1 route(s)
listening on :9000 for route "test-service"
Sluice: event loop started, 1 route(s) active
```
No `bind() on port 9000: Address already in use`.

**Pass:** Second start succeeds without error.

---

## Group 4 — Load Balancing (TC-18 to TC-19)

These tests require the WSL version which has full multi-backend support.

---

### TC-18 — Round-Robin Load Balancing

**Feature:** With 3 backends, each new client connection is assigned to the next backend in circular order: A→B→C→A→B→C→...

```bash
# [T1] — backend A
python3 tests/backends/tagged_echo_backend.py 7201 A

# [T2] — backend B
python3 tests/backends/tagged_echo_backend.py 7202 B

# [T3] — backend C
python3 tests/backends/tagged_echo_backend.py 7203 C

# [T4]
./build/sluice tests/configs/tc_three_backends.json

# [T5] — 9 requests, should go A B C A B C A B C
python3 tests/scripts/tc18_load_balancing.py 9 9200
```

**Expected [T5]:**
```
[TC-18] Load balancing test: 9 requests across 3 backends
  request-00: backend [A] replied: '[A] request-0'
  request-01: backend [B] replied: '[B] request-1'
  request-02: backend [C] replied: '[C] request-2'
  request-03: backend [A] replied: '[A] request-3'
  ...

[TC-18] PASS — round-robin order confirmed: A B C A B C A B C
```

**Pass:** Script exits 0 and the sequence matches A B C repeating.  
**What could go wrong:**
- All requests go to A → `select_backend` is always picking index 0 (next_index not advancing).
- Sequence is random → round-robin cursor is shared incorrectly.

---

### TC-19 — Load Balancing Skips Dead Backend

**Feature:** When one backend is down, passive health marking (from the failed connect) causes the gateway to skip it for all subsequent connections.

```bash
# [T1] — backend A running
python3 tests/backends/tagged_echo_backend.py 7201 A

# Backend B on :7202 — intentionally NOT started

# [T2] — backend C running
python3 tests/backends/tagged_echo_backend.py 7203 C

# [T3]
./build/sluice tests/configs/tc_three_backends.json

# [T4]
python3 tests/scripts/tc19_lb_failover.py 6 9200
```

**Expected [T4]:**
```
[TC-19] LB failover test: backend B (:7202) is intentionally DOWN
        Making 6 requests — none should go to B

  request-00: connection dropped (backend was B — acceptable once)
  request-01: PASS — backend [A] replied
  request-02: PASS — backend [C] replied
  ...

[TC-19] PASS — no traffic reached dead backend B
```

**Pass:** Script exits 0. The first request may fail (it hit B and got dropped + B marked unhealthy). All subsequent requests succeed and go only to A or C.  
**What could go wrong:**
- Multiple requests fail → the gateway is not marking B as unhealthy after the first failure (set_health not called on connect failure).
- Requests continue going to B → `select_backend` is not checking `b.healthy`.

---

## Group 5 — Heartbeat Health Checking (TC-20 to TC-21)

---

### TC-20 — Heartbeat Probes Idle Backend

**Feature:** When a backend has no active connections, the gateway periodically probes it. A backend that comes back up after being down should be re-detected as healthy.

```bash
# [T1] — Start gateway with heartbeat enabled (2s interval)
./build/sluice tests/configs/tc_heartbeat.json
# Note: backend :7301 is NOT running yet

# Wait ~5 seconds — watch the gateway log
# Expected: (silence or first probe failure)

# [T2] — Now start backend :7301
python3 test_utils/echo_backend.py 7301

# Wait ~3 seconds (next heartbeat tick)
# Expected [T1]: "backend 127.0.0.1:7301 is now healthy"
```

**Expected [T1] sequence (roughly):**
```
heartbeat: probing idle backends every 2s
Sluice: event loop started, 1 route(s) active
backend 127.0.0.1:7301 is now unhealthy     ← first probe, backend was down
...                                          ← 2 seconds pass
backend 127.0.0.1:7301 is now healthy       ← backend started, probe succeeded
```

**Pass:** The "now healthy" message appears within ~2–3 intervals of starting the backend.  
**What could go wrong:**
- "now healthy" never appears → timerfd not firing (check epoll registration of timer_fd), or launch_probe has a bug.
- Message appears immediately → backend was never actually probed, health state was not cleared.

---

### TC-21 — Per-Backend `heartbeat: false` Is Respected

**Feature:** A backend with `"heartbeat": false` in the config is never probed, even when idle.

**Config used:** `tests/configs/tc_heartbeat.json` — backend :7302 has `"heartbeat": false`.

```bash
# [T1]
./build/sluice tests/configs/tc_heartbeat.json

# Wait 10+ seconds and watch the log carefully
```

**Expected:** You will see probe-related log lines for `:7301` (the probed backend).  
You will **NOT** see any probe-related log lines for `:7302`.

**Pass:** No log output mentions `:7302` being probed.  
**What could go wrong:**
- `:7302` does appear in probe logs → `heartbeat_enabled` flag is not being checked in `run_heartbeats`.

---

## How to Re-use These Tests After Code Changes

1. **Rebuild**: `cd build && cmake --build .`
2. **Config tests (TC-01 to TC-07b):** Run exactly as written. Expected output is identical unless the error messages were deliberately changed.
3. **Relay tests (TC-08 to TC-12):** Re-run the automated scripts — they output PASS/FAIL with exit codes.
4. **Resilience tests (TC-13 to TC-17):** Follow the steps exactly; the core behaviors are unaffected by most feature additions.
5. **LB tests (TC-18 to TC-19):** Re-run automated scripts; they validate round-robin order strictly.
6. **Heartbeat tests (TC-20 to TC-21):** These are manual/observational — watch the gateway log.
7. **Adding new tests:** Append them here following the same format. Add supporting files to `tests/backends/`, `tests/configs/`, or `tests/scripts/` as appropriate.
