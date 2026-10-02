# Sluice — Dashboard Test Cases

Tests for the dashboard after the merge fixes: it runs **single-process only**,
its live-log panel is now fed from `Log()`, and `/api/config` reads/writes the
real config path. Run on WSL/Ubuntu.

Focus areas (what the merge fixes changed):
- `/api/logs` is **populated** (was always empty after we switched to `Log()`).
- Dashboard is **skipped under multi-process** (no port race / misleading per-worker stats).
- `/api/config` uses the **real config path** (was `""`).

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up httplib + dashboard.cpp + log_buffer.cpp
make
```

### Backends
- Multiple clients: `test_utils/echo_server <port>`
- Single client: `test_utils/echo_backend.py <port>`

### Tools
`curl` for the API; pipe JSON through `python3 -m json.tool` to read it.

### Note: the dashboard defaults to ON
`DashboardConfig.enabled` defaults to `true`, so even a config with **no**
`dashboard` block starts it on `:8080` in single-process mode. Set
`"dashboard": { "enabled": false }` to turn it off.

---

## A. Single-process dashboard — basics

Start backends + gateway with an explicit dashboard config:
```bash
test_utils/echo_server 7003 &
test_utils/echo_server 7004 &
test_utils/echo_backend.py 7001 &
./sluice ../config/dashboard_config.json
```

### A1 — startup summary + server up
**Expect:** the summary prints `dashboard: http://localhost:8080`, and a log line
`dashboard: listening on :8080`.
```bash
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/         # 200
curl -s http://localhost:8080/ | head -c 100                            # HTML
```

### A2 — /api/status returns live route/backend stats
```bash
curl -s http://localhost:8080/api/status | python3 -m json.tool
```
**Expect:** JSON with `uptime_seconds` and a `routes` array; each backend has
`host`, `port`, `healthy: true`, `active_connections`, `bytes_relayed`,
`total_connections` (all 0 before traffic).

---

## B. Live stats update with traffic

### B1 — counters move
With A running, push some traffic through the `orders` route:
```bash
for i in $(seq 1 10); do echo "hello-$i" | nc -N 127.0.0.1 9003 >/dev/null; done
curl -s http://localhost:8080/api/status | python3 -m json.tool
```
**Tests:** the dashboard reads the live atomic `Backend` fields.
**Expect:** for the `orders` backends (7003/7004), `total_connections` increased
(split across the two by round-robin), and `bytes_relayed` is non-zero. During an
open long transfer, `active_connections` would show > 0 (it returns to 0 after
connections close).

### B2 — health reflected live
Kill an idle backend and wait one heartbeat interval (~5s):
```bash
# Ctrl+C the echo_server on 7004  (or: pkill -f 'echo_server 7004')
sleep 6
curl -s http://localhost:8080/api/status | python3 -m json.tool
```
**Expect:** the `127.0.0.1:7004` backend now shows `"healthy": false`. Restart it →
within ~5s it flips back to `"healthy": true`.

---

## C. Live log panel (the key fix)

### C1 — /api/logs is populated (was empty before the fix)
With A running and after B1's traffic:
```bash
curl -s http://localhost:8080/api/logs | python3 -m json.tool
```
**Tests:** `Log()` now mirrors each line into the dashboard ring buffer.
**Expect:** a JSON array of recent log lines — each `[pid] ...` prefixed — e.g.
`listening on :9003 ...`, `accepted client fd=... on route "orders" ...`,
`backend connected ...`, `half-closed ...`. **Before this fix the array was `[]`.**

### C2 — ring buffer is bounded by log_lines
`dashboard_config.json` sets `log_lines: 100`. Generate more than 100 log-producing
events:
```bash
for i in $(seq 1 200); do echo x | nc -N 127.0.0.1 9003 >/dev/null; done
curl -s http://localhost:8080/api/logs | python3 -c "import sys,json; print(len(json.load(sys.stdin)))"
```
**Expect:** the count is capped at **100** (oldest lines dropped).

---

## D. Config read / save (path fix)

### D1 — /api/config returns the real file
```bash
curl -s http://localhost:8080/api/config | python3 -m json.tool
```
**Tests:** `config_path` is now threaded through (was `""` → 500 before the fix).
**Expect:** the JSON contents of `dashboard_config.json` (routes, dashboard,
heartbeat), **not** an error.

### D2 — /api/config/save validates and writes
```bash
# valid save (echo back the current config with a tweak)
curl -s -X POST http://localhost:8080/api/config/save \
  -H 'Content-Type: application/json' \
  --data '{"routes":[{"name":"payments","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}]}'
# -> {"ok":true}
cat ../config/dashboard_config.json    # file now holds the posted JSON (pretty-printed)

# invalid save (no routes) -> rejected, file unchanged
curl -s -X POST http://localhost:8080/api/config/save \
  -H 'Content-Type: application/json' --data '{"foo":1}'
# -> 400 {"ok":false,"error":"Missing or invalid 'routes' array"}
```
**Note:** saving rewrites the file but does **not** hot-reload — changes take
effect only on the next start. (Restore the file afterward if you want to keep the
two-route version: re-copy it from git or re-create it.)

> ⚠️ D2 overwrites `dashboard_config.json`. Back it up first if you care:
> `cp ../config/dashboard_config.json /tmp/dash.bak`.

---

## E. Multi-process — dashboard is skipped

### E1 — summary note + no server
Use a multi-worker config that also has the dashboard enabled:
```bash
# add "worker_processes": 4 to a copy of dashboard_config.json, or reuse one:
./sluice ../config/dashboard_config_mp.json
```
(where `dashboard_config_mp.json` = `dashboard_config.json` + `"worker_processes": 4`)
**Tests:** the dashboard only runs in single-process mode.
**Expect:**
- Summary prints `dashboard: off (only runs with worker_processes: 1)`.
- **No** `dashboard: listening` log line.
- Nothing is listening on 8080:
  ```bash
  curl -s -o /dev/null -w "%{http_code}\n" --max-time 2 http://localhost:8080/ ; echo "(connection refused expected)"
  ```
- `pgrep -fa sluice` shows master + 4 workers (no extra dashboard bind,
  no port-race errors in the logs).

---

## F. Dashboard + file logging together (single-process)

### F1 — logs go to the file AND appear in /api/logs
Make a single-process config that sets both `log_file` and keeps the dashboard on
(e.g. add `"log_file": "gateway.log"` to `dashboard_config.json`).
```bash
rm -f gateway.log
./sluice ../config/dashboard_logfile.json &
echo hi | nc -N 127.0.0.1 9003 >/dev/null
grep "accepted client" gateway.log            # line is in the file (via the writer)
curl -s http://localhost:8080/api/logs | grep "accepted client"   # same line in the dashboard ring
```
**Tests:** `Log()` mirrors to the ring buffer *and* writes to its fd (the pipe →
writer → file). Both sinks get the line.
**Expect:** the `accepted client ...` line appears in **both** the file and
`/api/logs`.

---

## G. Disabling the dashboard

### G1 — enabled:false
```bash
echo '{ "dashboard":{"enabled":false}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/nodash.json
test_utils/echo_backend.py 7001 &
./sluice /tmp/nodash.json
```
**Expect:** summary prints `dashboard: disabled`; nothing on 8080; no dashboard
log line.

---

## H. Config validation (dashboard block)

Each should fail fast, non-zero exit, clear message.
```bash
# port out of range
echo '{ "dashboard":{"port":70000}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/dp.json
./sluice /tmp/dp.json; echo "exit=$?"
# -> config error: config: dashboard "port" out of range (1-65535)   exit=1

# enabled wrong type
echo '{ "dashboard":{"enabled":"yes"}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/de.json
./sluice /tmp/de.json; echo "exit=$?"
# -> config error: config: dashboard "enabled" must be a boolean   exit=1

# log_lines <= 0
echo '{ "dashboard":{"log_lines":0}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/dl.json
./sluice /tmp/dl.json; echo "exit=$?"
# -> config error: config: dashboard "log_lines" must be > 0   exit=1
```

---

## Helper configs to create for these tests
- `config/dashboard_config_mp.json` — copy of `dashboard_config.json` + `"worker_processes": 4`.
- `config/dashboard_logfile.json` — copy of `dashboard_config.json` + `"log_file": "gateway.log"`.

---

## Known behavior / caveats (verify you're OK with these)
- **Dashboard defaults ON** — any config without a `dashboard` block serves `:8080`
  in single-process mode.
- **Multi-process = no dashboard** (by design for now). Aggregated multi-process
  dashboard is deferred.
- **Config-save has no hot-reload** — the file is written but not re-read until restart.
- **Shutdown UAF window** — the detached dashboard thread holds pointers to
  `gs.routes`/`log_buf`, destroyed when the gateway exits. A request racing shutdown
  could touch freed memory before the process ends. Low risk; a proper fix (stop the
  server / join on shutdown) is a future cleanup.
