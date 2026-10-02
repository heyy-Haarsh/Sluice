# Sluice — File Logging Test Cases

Tests for the dedicated log-writer process (Design A): workers/master send log
lines over a pipe (non-blocking, drop-on-full, stderr fallback); a separate
writer process owns the single log file and handles time-based + SIGHUP rotation.
Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up src/log.cpp and src/logwriter.cpp
make
```

### Backends
- **Multiple clients:** `test_utils/echo_server <port>` (compiled; handles many
  concurrent clients).
- **Single client:** `test_utils/echo_backend.py <port>` (Python; one client at
  a time — fine where a test uses a single connection).

### Process model reminder
With file logging enabled and `worker_processes: N`, the process tree is:
```
master  ──┬── writer  (owns the log file)
          ├── worker 0
          ├── ...
          └── worker N-1
```
So `pgrep -fa sluice` shows **N + 2** processes (master + N workers +
writer). Every log line is `[pid] ...`. The **writer** is the process that logs
`log writer: writing to <file>` to **stderr** at startup — that line tells you
its pid.

### How logs are split
- **stdout:** the one-time startup summary from `main` (`loaded ... route(s)`,
  `workers:`, `log:`).
- **stderr:** only the writer's own announce line (`log writer: writing to ...`)
  and any fallback logs if the writer dies.
- **the log file:** everything else — all worker/master runtime logs.

---

## A. Basic file logging

### A1 — single process, logs land in the file
Make `config/logfile_single.json` = `logfile_config.json` but with
`"worker_processes": 1`. Then:
```bash
test_utils/echo_backend.py 7001 &
rm -f gateway.log
./sluice ../config/logfile_single.json
```
In another terminal:
```bash
echo hello | nc -N 127.0.0.1 9000
cat gateway.log
```
**Tests:** the writer process owns the file; runtime logs go there.
**Expect:**
- `gateway.log` exists and contains the `listening on ...`, `accepted client ...`,
  `backend connected ...`, `half-closed ...` lines — each `[pid]`-prefixed.
- The gateway's **stderr** shows only `[writerpid] log writer: writing to gateway.log`.
- The gateway's **stdout** shows the startup summary, including `log: gateway.log (rotation: none)`.

### A2 — file logging disabled ⇒ stderr (regression)
Use a config with **no** `log_file` (e.g. `config/config.json`).
```bash
./sluice ../config/config.json
```
**Tests:** the pre-file-logging behavior is unchanged when no file is configured.
**Expect:** `log: stderr` in the summary; **no writer process** (`pgrep` shows one
process for `worker_processes: 1`); logs on stderr as before.

---

## B. Multi-process file logging

### B1 — all workers write to one clean file
```bash
test_utils/echo_server 7001 &
test_utils/echo_server 7003 &
test_utils/echo_server 7004 &
rm -f gateway.log
./sluice ../config/logfile_config.json      # worker_processes: 4
```
Drive traffic:
```bash
for i in $(seq 1 60); do echo "hi-$i" | nc -N 127.0.0.1 9000 >/dev/null; done
```
**Tests:** one writer, one file, many worker processes — single owner of the fd.
**Expect:**
- `pgrep -fa sluice` shows **6** processes (master + 4 workers + writer).
- `gateway.log` contains accept lines from **multiple distinct worker pids**:
  ```bash
  grep "accepted client" gateway.log | grep -oE '^\[[0-9]+\]' | sort | uniq -c
  ```
  → more than one pid.

### B2 — no interleaving / no corruption under concurrent load
With B1 running, hammer concurrently (multi-client backend needed here):
```bash
for i in $(seq 1 300); do (echo "m-$i" | nc -N 127.0.0.1 9000 >/dev/null &); done
wait
```
**Tests:** atomic single-`write()` lines through the pipe + `O_APPEND` in the
writer keep every line whole.
**Expect:** every non-empty line in `gateway.log` begins with `[digits]` and is a
complete message — nothing spliced. Check:
```bash
grep -vE '^\[[0-9]+\] ' gateway.log | grep -v '^$'    # should print nothing
```

---

## C. Non-blocking hand-off (logging never stalls traffic)

### C1 — traffic keeps flowing under heavy logging
Run B1's setup. Generate a large burst so the writer has lots to write while
clients keep connecting:
```bash
time ( for i in $(seq 1 500); do echo "load-$i" | nc -N 127.0.0.1 9000 >/dev/null; done )
```
**Tests:** the event loop enqueues to the pipe non-blocking and never waits on
disk I/O.
**Expect:** the run completes promptly; the gateway stays responsive throughout.
If the writer ever falls behind, some log lines are **dropped** (by design) rather
than stalling a worker — so `gateway.log` may contain slightly fewer lines than
the number of events under extreme load. That trade-off (drop, don't block) is
correct behavior, not a bug.

---

## D. Rotation

### D1 — SIGHUP reopen (logrotate integration)
```bash
test_utils/echo_backend.py 7001 &
rm -f gateway.log gateway.log.1
./sluice ../config/logfile_single.json &   # single worker; $! = gateway pid
GW=$!
echo one | nc -N 127.0.0.1 9000 >/dev/null
mv gateway.log gateway.log.1     # simulate logrotate renaming the file away
kill -HUP $GW                    # gateway forwards HUP to the writer
sleep 0.3
echo two | nc -N 127.0.0.1 9000 >/dev/null
```
**Tests:** on SIGHUP the writer closes and reopens the configured path, so new
logs go to a fresh file (not the renamed-away one).
**Expect:**
- `gateway.log` (freshly recreated) contains the **`two`** connection's logs.
- `gateway.log.1` contains the **`one`** logs and has **stopped growing**.
- Without the reopen, all logs would have kept going into `gateway.log.1` — that's
  the bug SIGHUP fixes.

### D2 — time-based rotation (dated filenames)
Make a config with `"log_rotation": "hourly"` and `"log_file": "gateway.log"`.
```bash
rm -f gateway-*.log
./sluice ../config/logfile_hourly.json &
echo hi | nc -N 127.0.0.1 9000 >/dev/null
ls gateway-*.log
```
**Tests:** the writer derives a period-stamped filename.
**Expect:** a file named `gateway-YYYY-MM-DD-HH.log` is created (daily would be
`gateway-YYYY-MM-DD.log`). The writer's stderr announce shows
`writing to gateway-...log (hourly rotation)`.
> The actual **roll** to the next file happens when the clock crosses the
> hour/day boundary and the next line is written — that's time-dependent and not
> practical to force in a quick test. D2 verifies the naming; the roll logic is
> the same `target_filename` check on each batch.

---

## E. Writer failure handling

### E1 — writer dies ⇒ workers fall back to stderr, gateway survives
Run B1's setup. Find and kill the writer:
```bash
# the writer announced itself on stderr: "[<pid>] log writer: writing to ..."
kill -9 <writer_pid>
echo after-writer-died | nc -N 127.0.0.1 9000
```
**Tests:** a broken log pipe must never take down the gateway.
**Expect:**
- The gateway keeps serving (the `after-writer-died` request still echoes back).
- New runtime logs now appear on **stderr** again (the `EPIPE` fallback), no longer
  in the file.
- No worker crashes; `pgrep` still shows the master + workers (only the writer is
  gone — it is intentionally **not** respawned, since existing workers can't be
  reconnected to a new pipe).

---

## F. Graceful shutdown flushes the log

### F1 — final logs are written before exit
Run A1 (single process + file). Send a request, then stop with Ctrl+C:
```bash
echo lastreq | nc -N 127.0.0.1 9000 >/dev/null
# Ctrl+C the gateway
grep "shutting down" gateway.log
```
**Tests:** on shutdown the parent closes the pipe, the writer drains what's left,
then exits.
**Expect:** the final `Sluice: shutting down (...)` line is present in
`gateway.log` (it was drained by the writer before it exited). No truncated last
line.

---

## G. Config validation

Each should fail fast, non-zero exit, clear message.
```bash
# log_rotation invalid value
echo '{ "log_file":"x.log", "log_rotation":"weekly", "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/lr.json
./sluice /tmp/lr.json; echo "exit=$?"
# Expect: config error: config: "log_rotation" must be "none", "hourly", or "daily"   exit=1

# log_file wrong type
echo '{ "log_file": 123, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/lf.json
./sluice /tmp/lf.json; echo "exit=$?"
# Expect: config error: config: "log_file" must be a string   exit=1
```

---

## Helper configs to create for these tests

- `config/logfile_single.json` — copy of `logfile_config.json` with `"worker_processes": 1`.
- `config/logfile_hourly.json` — copy with `"log_rotation": "hourly"` (any worker count).

---

## Notes / known behavior

- **Drop-on-full is intentional.** Under extreme log volume the pipe fills and the
  worker drops lines rather than blocking the event loop. Logging is best-effort by
  design.
- **The writer is not respawned.** If it dies, everyone falls back to stderr for the
  rest of the run (a respawn couldn't reconnect the existing workers' pipe ends).
  Keep the writer simple so this stays rare.
- **Timestamps.** Lines carry a `[pid]` prefix but no timestamp yet. If you want
  time in the file, the natural place to add it is the writer (per line) or the
  `Log` prefix — a small future enhancement.
