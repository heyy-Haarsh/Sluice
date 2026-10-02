# Sluice — Robustness & Hardening Test Cases

Tests for the six hardening fixes:

1. Pointer-stable routes/backends → hot-reload no longer crashes.
2. Dashboard reads safe during a concurrent reload.
3. Backpressure → bounded memory under a fast-client / slow-backend mismatch.
4. Dashboard bound to loopback + optional auth token.
5. `max_connections` cap.
6. Reload moved to `SIGUSR1` (SIGHUP stays log-rotation only).

Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the config / gateway / dashboard / master changes
make
```

### Test methodology
- **No background processes.** Each server runs in the foreground in its own
  terminal, labelled **T1, T2, T3, ...** — open a new one for each.
- **Stop a foreground server with Ctrl+C** when a test says so.
- Python helpers are launched with **`python3`**.

### Helpers / configs used
- `tests/backends/echo_any.py <host> <port>` — echo backend (v4/v6).
- `tests/backends/slow_sink.py <host> <port>` — accepts but never reads (§C).
- `tests/backends/hold_conns.py <host> <port> <count>` — opens & holds N
  connections (§E).
- `config/robustness_config.json` — two routes (:9000→7001, :9001→7002),
  dashboard on (loopback). §A, §B, §D.
- `config/backpressure_config.json` — :9000→7001, dashboard off. §C.
- `config/maxconn_config.json` — :9000→7001, `max_connections: 3`. §E.

---

## A. Hot-reload stability (fix #1)

Before this fix, a reload that added/removed a route or backend could crash the
gateway (dangling pointers into a reallocated vector). It should now be rock
solid.

**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `python3 tests/backends/echo_any.py 127.0.0.1 7002`
**T3** `./build/sluice config/robustness_config.json`

### A1 — repeatedly add/remove routes and backends, no crash
Confirm both routes work first:
```bash
echo a | nc -N 127.0.0.1 9000    # -> echoes a
echo b | nc -N 127.0.0.1 9001    # -> echoes b
```
Now churn the config several times (edit `config/robustness_config.json` in an
editor, then reload). Do each of these, reloading after each edit with
`kill -USR1 $(pgrep -f 'sluice config/robustness_config.json')`:
1. Remove route `r2` → reload → `echo x | nc -N 127.0.0.1 9001` now refused.
2. Add it back → reload → `:9001` works again.
3. Add a second backend `{"host":"127.0.0.1","port":7002}` to `r1` → reload.
4. Remove that backend again → reload.
**Tests:** insert/erase of list nodes keeps every held pointer valid.
**Expect:** after every reload T3 logs `hot-reload: done — N active route(s)`
and **keeps running** (`pgrep` still shows it); each `nc` behaves per the
current config. No crash, no `Segmentation fault`.

### A2 — in-flight connection survives a reload that adds a backend
```bash
# T4: start a long-lived connection on r1, leave it open (type nothing)
nc 127.0.0.1 9000
```
While it's open, add a backend to `r1` and reload (as above). Back in T4, type a
line and press enter.
**Expect:** the pre-existing connection still echoes — it wasn't disturbed by
the backend being appended to the route.

---

## B. Dashboard safe during reload (fix #2)

**T1/T2/T3** as in §A (dashboard is enabled in `robustness_config.json`).

### B1 — hammer /api/status while reloading repeatedly
**T4** — poll the dashboard hard:
```bash
for i in $(seq 1 2000); do curl -s -o /dev/null http://127.0.0.1:8080/api/status; done
```
**T5** — while T4 runs, reload several times:
```bash
for i in 1 2 3 4 5; do
  kill -USR1 $(pgrep -f 'sluice config/robustness_config.json'); sleep 0.3
done
```
**Tests:** the dashboard thread holds `routes_mtx` while walking the route list,
so a concurrent structural reload can't corrupt its read.
**Expect:** T4 completes with no error; T3 never crashes; `/api/status` keeps
returning valid JSON throughout.

---

## C. Backpressure / bounded memory (fix #3)

**T1** — a backend that accepts but never reads:
```bash
python3 tests/backends/slow_sink.py 127.0.0.1 7001
```
**T2** `./build/sluice config/backpressure_config.json`

### C1 — a fast client to a stalled backend doesn't grow memory without bound
**T3** — record the gateway's baseline memory:
```bash
GW=$(pgrep -f 'sluice config/backpressure_config.json')
ps -o rss= -p $GW          # baseline RSS in KB
```
**T4** (new terminal) — fire a large stream at the gateway (this will stall,
which is the point — leave it running):
```bash
dd if=/dev/zero bs=1M count=1000 2>/dev/null | nc -N 127.0.0.1 9000
```
**T3** — after a few seconds, check memory again:
```bash
ps -o rss= -p $GW          # RSS after ~1 GB was streamed at a non-reading backend
```
**Tests:** once the client->backend buffer hits the ~1 MiB high-water mark, the
gateway stops reading the client (EPOLLIN dropped) instead of buffering the
whole 1 GB.
**Expect:** RSS rises only modestly (order of a couple of MB, not ~1 GB) and
**plateaus** — the `dd|nc` pipe in T4 stalls because the gateway applies
backpressure. Ctrl+C T4 when done.
> Contrast: without the fix, RSS would climb toward the full 1 GB.

### C2 — traffic resumes when the backend starts reading
This is inherent in the design: if `slow_sink` were replaced with a normal
reader (`echo_any.py`), the buffer drains, EPOLLIN is re-armed, and the transfer
completes. (Covered functionally by the existing relay tests.)

---

## D. Dashboard bind + auth token (fix #4)

**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `./build/sluice config/robustness_config.json`

### D1 — dashboard is bound to loopback, not all interfaces
```bash
ss -tlnp 2>/dev/null | grep ':8080'
```
**Tests:** the default `dashboard.bind` is `127.0.0.1`.
**Expect:** the listen address is **`127.0.0.1:8080`** (not `0.0.0.0:8080` /
`*:8080`). `curl http://127.0.0.1:8080/` returns HTML; a request to the host's
LAN IP (e.g. `curl http://<your-LAN-IP>:8080/`) is refused/times out.

### D2 — token enforced on mutating endpoints
Stop T2. Edit `config/robustness_config.json` and set
`"dashboard": { "enabled": true, "port": 8080, "token": "s3cret" }`, then restart
T2. Now:
```bash
# reload with NO token -> rejected
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://127.0.0.1:8080/api/reload
# -> 401

# reload WITH the token -> accepted
curl -s -o /dev/null -w "%{http_code}\n" -X POST \
  -H 'X-Auth-Token: s3cret' http://127.0.0.1:8080/api/reload
# -> 200
```
**Tests:** `/api/config/save` and `/api/reload` require `X-Auth-Token` when a
token is configured.
**Expect:** `401` without the header, `200` with it. (The dashboard UI injects
the token automatically, so the SAVE / RELOAD buttons keep working in the browser.)
Reading endpoints (`/`, `/api/status`) remain open — loopback binding is the
primary protection; the token stops config *changes* if you deliberately expose
the port.

---

## E. Connection cap (fix #5)

**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `./build/sluice config/maxconn_config.json`   (`max_connections: 3`)

### E1 — connections past the cap are dropped
**T3** — hold exactly the cap open:
```bash
python3 tests/backends/hold_conns.py 127.0.0.1 9000 3
```
**T4** — try one more:
```bash
echo hi | nc -N 127.0.0.1 9000 ; echo "rc=$?"
```
**Tests:** `live_connections.size()` is checked right after `accept()`.
**Expect:** T2 logs `connection limit (3) reached — dropping client ...`, and
the T4 connection is closed immediately with no echo. Ctrl+C the holder (T3) to
free a slot, then repeat T4 → it now succeeds.

### E2 — unlimited when set to 0
Set `"max_connections": 0` (or remove it), restart T2, repeat E1 → the 4th and
further connections all succeed (no cap). Default when the key is absent is
`10000`.

---

## F. Reload signal moved to SIGUSR1 (fix #6)

### F1 — SIGUSR1 reloads (single process)
**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `./build/sluice config/robustness_config.json`
Edit the config (e.g. remove route `r2`), then:
```bash
kill -USR1 $(pgrep -f 'sluice config/robustness_config.json')
```
**Expect:** T2 logs `hot-reload: done — ...`; `:9001` stops accepting.

### F2 — SIGHUP no longer means "reload"
`SIGHUP` is now reserved for the log writer's rotation. With a config that has no
`log_file`, `kill -HUP` on the gateway will terminate it (default disposition) —
it will **not** reload. Reload is `SIGUSR1` (or the dashboard button). For the
SIGHUP-drives-log-rotation behavior, see `logging-tests.md` §D1 (still valid).

### F3 — multi-process reload via the master
Make a copy of `robustness_config.json` with `"worker_processes": 2` and
`"dashboard": { "enabled": false }` → `/tmp/mp.json`.
**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `python3 tests/backends/echo_any.py 127.0.0.1 7002`
**T3** `./build/sluice /tmp/mp.json`
Find the **master** pid (the parent):
```bash
pgrep -f 'sluice /tmp/mp.json'          # several pids
MASTER=$(pgrep -of 'sluice /tmp/mp.json')   # -o = oldest = master
# edit /tmp/mp.json (e.g. remove r2), then:
kill -USR1 $MASTER
```
**Tests:** the master forwards `SIGUSR1` to every worker (new in this fix).
**Expect:** T3 logs `master: reload signal received, forwarding to workers`
followed by each worker's `hot-reload: done — ...`; `:9001` stops accepting on
all workers. (Before this fix, multi-process reload was impossible.)

---

## Notes

- **`max_connections` default is 10000**, so existing small-scale tests are
  unaffected; set it lower to exercise the cap.
- **Backpressure high-water is ~1 MiB per direction.** Peak per-connection
  memory is roughly that plus one 64 KiB read chunk.
- **Signals:** `SIGUSR1` = config reload; `SIGHUP` = log-file reopen (rotation);
  `SIGINT`/`SIGTERM` = graceful shutdown.
- **Dashboard is single-process only.** In multi-process mode reload is driven
  by `kill -USR1 <master>` (F3), not the dashboard.
