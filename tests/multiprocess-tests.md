# Sluice — Multi-Process Test Cases

Tests for the multi-process feature (fork master + N workers, `SO_REUSEPORT`,
crash respawn, graceful shutdown) plus regression checks that the existing
features still work under multiple workers. Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the new src/master.cpp
make
```
Expect a clean build (warnings-as-info only). If `make` fails, fix before testing.

### Helper backends

`echo_backend.py` (echoes everything back):
```python
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(64)
print(f"echo backend on {port}", flush=True)
while True:
    c, _ = s.accept()
    while True:
        d = c.recv(65536)
        if not d: break
        c.sendall(d)
    c.close()
```

`slow_backend.py` (reads slowly — for the backpressure regression test):
```python
import socket, sys, time
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(64)
print(f"slow backend on {port}", flush=True)
c, _ = s.accept()
total = 0
while True:
    d = c.recv(1024)
    if not d: break
    total += len(d); time.sleep(0.02)
print("slow backend received total bytes:", total, flush=True)
```

### Handy commands
- **List all gateway processes (master + workers):** `pgrep -fa sluice`
- Every log line is prefixed with the emitting process's pid: `[12345] accepted client ...`. That prefix is how you tell which worker handled a connection.

---

## A. Startup & process model

### A1 — N workers start under a master
```bash
python3 echo_backend.py 7001 &
python3 echo_backend.py 7003 &
python3 echo_backend.py 7004 &
./sluice ../config/mp_config.json
```
**Tests:** the master forks the configured number of workers.
**Expect:**
- Startup summary prints `workers: 4`.
- One `[master-pid] master: started 4 worker(s)` line.
- Each worker logs its own `listening on :9000 ...` / `:9003 ...` and `event loop started` (so you see the listening lines **4×**, once per worker — confirms each bound the ports via `SO_REUSEPORT`).
- In another terminal, `pgrep -fa sluice` shows **5 processes** (1 master + 4 workers).

### A2 — `worker_processes: 1` is the old single-process path
Make a copy of `mp_config.json` with `"worker_processes": 1` (or use `config.json`, which has no worker field → defaults to 1).
```bash
./sluice ../config/config.json
```
**Tests:** default/1 path does not fork and behaves exactly as before.
**Expect:** summary prints `workers: 1`; **no** `master:` lines; `pgrep -fa sluice` shows a **single** process. Behavior identical to the pre-multiprocess build.

### A3 — `worker_processes: "auto"`
Make a config with `"worker_processes": "auto"`.
```bash
./sluice ../config/auto_config.json
```
**Tests:** `auto` resolves to the core count.
**Expect:** `workers: N` where N == `nproc`, and N workers start.

---

## B. Connection distribution (SO_REUSEPORT)

### B1 — connections spread across workers
Gateway running from A1 (4 workers, echo backend on 7001). Fire many short connections at one route and collect which worker handled each:
```bash
for i in $(seq 1 40); do echo "hi-$i" | nc -N 127.0.0.1 9000 >/dev/null; done
```
Watch the gateway output (or capture it). Extract the pids that logged an accept:
```bash
# if you redirected the gateway's stderr to gw.log:
grep "accepted client" gw.log | grep -oE '^\[[0-9]+\]' | sort | uniq -c
```
**Tests:** the kernel load-balances accepted connections across the workers.
**Expect:** **more than one** distinct worker pid appears among the `accepted client` lines (distribution won't be perfectly even for a small sample, but it must not all land on one worker).

### B2 — a single worker still serves a full connection end-to-end
```bash
echo "roundtrip" | nc -N 127.0.0.1 9000
```
**Tests:** whichever worker accepts also completes the relay (connections are pinned to one worker).
**Expect:** `roundtrip` is echoed back; the accept, `backend connected`, and half-close logs for that connection all carry the **same** `[pid]` prefix.

---

## C. Crash resilience (respawn)

### C1 — killing a worker respawns it
Gateway running (4 workers). Pick a worker pid and hard-kill it:
```bash
pgrep -fa sluice            # note the master (first) and a worker pid
kill -9 <a_worker_pid>
```
**Tests:** the master reaps the dead worker and forks a replacement.
**Expect:**
- Master logs `worker pid=<dead> (slot k) exited` then `respawned slot k as pid=<new>`.
- `pgrep -fa sluice` again shows **5 processes** (a new worker replaced the killed one).
- The other workers were unaffected — connections through them never dropped.

### C2 — surviving workers keep serving during a crash
Start a steady trickle in one terminal:
```bash
while true; do echo ping | nc -N 127.0.0.1 9000 >/dev/null; sleep 0.2; done
```
In another, `kill -9` a worker.
**Tests:** fault isolation — one worker dying doesn't interrupt the others.
**Expect:** the trickle keeps succeeding throughout (at most the in-flight connections on the killed worker fail); the gateway self-heals via respawn.

### C3 — crash-loop backoff (optional, destructive)
Only if you want to see the guard: a worker that dies immediately and repeatedly triggers a backoff. You can simulate by pointing a config at an un-bindable port so `run_gateway` throws on startup (e.g. `listen_port: 80` without privileges), `worker_processes: 2`.
**Tests:** the master doesn't fork-bomb on a deterministic startup failure.
**Expect:** `worker: fatal: ...` from each worker, and master lines showing `died in <ms>ms, backing off before respawn` roughly once per second rather than spinning.

---

## D. Graceful shutdown

### D1 — Ctrl+C stops master and all workers
Gateway running in the foreground. Press **Ctrl+C**.
**Tests:** group shutdown with no orphans.
**Expect:** master logs `signal received, stopping workers`; each worker logs `Sluice: shutting down (...)`; master ends with `all workers stopped`; process returns to the shell. `pgrep -fa sluice` shows **nothing**.

### D2 — `kill -TERM <master>` stops everything
```bash
./sluice ../config/mp_config.json &   # background
kill -TERM $(pgrep -f sluice | head -1)   # the master pid
```
**Tests:** signaling only the master tears down the whole tree.
**Expect:** all workers exit; no leftover processes; no zombies (`ps -el | grep defunct` is empty).

---

## E. Feature regression under multiple workers

These confirm the multi-process change didn't break existing behavior.

### E1 — relay / echo
Covered by B2 — a message echoes back correctly. ✔ if B2 passes.

### E2 — round-robin load balancing
Backends up on 7003 and 7004 (route `orders`, port 9003). Because each worker
keeps its **own** round-robin cursor, look at accepts handled by a **single**
worker:
```bash
for i in $(seq 1 20); do echo "o-$i" | nc -N 127.0.0.1 9003 >/dev/null; done
grep "on route \"orders\"" gw.log | grep '^\[<one_worker_pid>\]' \
  | grep -oE '127.0.0.1:(7003|7004)' | sort | uniq -c
```
**Tests:** round-robin still alternates backends.
**Expect:** for that one worker, connections split between `:7003` and `:7004` (roughly alternating). Across all workers combined, both backends are used.

### E3 — heartbeats run per worker
Leave the gateway idle (no client traffic) with backends up, `interval_seconds: 5`.
Kill the `orders` backend on 7004.
**Tests:** each worker independently health-checks idle backends.
**Expect:** within ~5s you see `backend 127.0.0.1:7004 is now unhealthy` — **once per worker** (i.e. up to N times, each line with a different `[pid]`). This N× probing is expected and by design (workers don't share health state). Restart 7004 → each worker logs `... is now healthy` again.

### E4 — backpressure (no data loss through a slow backend)
Point a single-worker config at `slow_backend.py` to keep it simple, OR run
mp and target the slow backend; then:
```bash
python3 slow_backend.py 7003 &
python3 -c "import sys; sys.stdout.write('x'*2000000)" | nc -N 127.0.0.1 9003
```
**Tests:** buffering/backpressure still correct under a worker.
**Expect:** when the transfer completes, the slow backend prints `received total bytes: 2000000` exactly — no loss, no partial-send warnings.

### E5 — half-close still delivers the late reply
Use the half-close backend from earlier feature tests on a route, connect with
`nc -N`, send data, Ctrl+D.
**Expect:** the backend's post-EOF reply still comes back before the connection
closes (same behavior as single-process).

---

## F. Config validation

Each should fail fast with a clear message and non-zero exit.

```bash
# worker_processes = 0
echo '{ "worker_processes": 0, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/w0.json
./sluice /tmp/w0.json; echo "exit=$?"
# Expect: config error: config: "worker_processes" must be >= 1   exit=1

# worker_processes = "banana"
echo '{ "worker_processes": "banana", "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/wb.json
./sluice /tmp/wb.json; echo "exit=$?"
# Expect: config error: config: "worker_processes" string must be "auto"   exit=1

# worker_processes = 3.5 (non-integer)
echo '{ "worker_processes": 3.5, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/wf.json
./sluice /tmp/wf.json; echo "exit=$?"
# Expect: config error: config: "worker_processes" must be an integer or "auto"   exit=1
```

---

## G. Logging (no interleaving)

### G1 — lines are pid-tagged and never interleave
Run 4 workers, drive heavy concurrent traffic while capturing stderr:
```bash
./sluice ../config/mp_config.json 2> gw.log &
for i in $(seq 1 200); do (echo "m-$i" | nc -N 127.0.0.1 9000 >/dev/null &); done
wait
```
**Tests:** the atomic single-`write()` log path prevents mid-line interleaving
across processes.
**Expect:** every line in `gw.log` starts with a `[pid]` prefix and is a
complete, well-formed message — no line has another line's text spliced into it.
Quick sanity check (every non-empty line begins with `[digits]`):
```bash
grep -vE '^\[[0-9]+\] ' gw.log | grep -v '^$'   # should print nothing
```

---

## Known limitations to be aware of while testing

- **Heartbeat probes scale with workers.** With N workers, each idle backend is
  probed by every worker → up to N× probe connections per interval (see E3). By
  design (shared-nothing workers); harmless at these scales.
- **fd-exhaustion busy-loop (pre-existing, not addressed here).** Under an
  `EMFILE`/`ENFILE` condition (fd table full), a listener can spin logging accept
  errors. This belongs with the DoS-hardening work (bounded resources), not the
  multi-process change.
- **Global limits are per-worker.** There is no cross-worker connection or
  rate cap; any such limit would currently apply per worker, summing to N× the
  intended total.
