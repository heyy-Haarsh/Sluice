# Sluice — UDP Test Cases (Milestones A–E)

Tests for the UDP proxy: **config** (protocol field, per-`(protocol,port)`
uniqueness, session timeout/cap), **listen sockets**, the **core relay** (session
synthesis, LB, replies), the **session lifecycle** (idle reclamation, cap), and
**policy reuse** (affinity, access control, no health-probing of UDP backends).
Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the config / gateway UDP changes
make
```

### Test methodology
- **No background processes.** Each server runs in the foreground in its own
  terminal, labelled **T1, T2, T3, ...** — open a new one for each.
- **Stop a foreground server with Ctrl+C** when a test says so.
- Python backends are launched with **`python3`**.
- The UDP client is **`nc -u`** (OpenBSD netcat): `sudo apt install netcat-openbsd`.
  `-w1` = give up 1s after sending (prints the reply, then exits); no `-w` = stay
  interactive (type a line + Enter to send one datagram).

### Backends
- `tests/backends/udp_echo.py <host> <port> <tag>` — UDP echo; prefixes each
  reply with `[tag]` and prints the gateway peer it sees.
- `tests/backends/echo_any.py <host> <port>` — TCP echo (used only in C4).

### Config files (`tests/configs/`)
| File | Purpose |
|---|---|
| `tc_udp_single.json`  | 1 UDP route `:9401`→`7401`, dashboard on, heartbeat 3s, **timeout 5s** |
| `tc_udp_lb.json`      | UDP `:9401`→ `7401/7402/7403`, **affinity** `127.0.0.2`→`7403` |
| `tc_udp_coexist.json` | **tcp** `:9400`→`7400` **and udp** `:9400`→`7401` (same port) |
| `tc_udp_cap.json`     | UDP `:9401`→`7401`, **max_udp_sessions 2**, timeout 60s |
| `tc_udp_access.json`  | UDP `:9401`→`7401`, global **blacklist `127.0.0.2`** |
| `tc_udp_bad_proto.json` | invalid `"protocol":"sctp"` (must be rejected) |
| `tc_udp_dup_port.json`  | two UDP routes on `:9401` (must be rejected) |

Throughout, the gateway runs in **T-GW**; watch its log for the quoted lines.

---

## A. Config — parsing & validation

### A1 — a UDP route starts
**T-GW**
```bash
./sluice ../tests/configs/tc_udp_single.json
```
Expect a startup line: `listening on udp :9401 for route "udp-echo"`. Leave it
running for B. **PASS** = it starts (no error).

### A2 — tcp:9400 and udp:9400 coexist
```bash
./sluice ../tests/configs/tc_udp_coexist.json
```
**PASS** = starts and logs **both** `listening on :9400 for route "web-tcp"` and
`listening on udp :9400 for route "echo-udp"`. (Ctrl+C after.)

### A3 — bad protocol is rejected
```bash
./sluice ../tests/configs/tc_udp_bad_proto.json ; echo "exit=$?"
```
**PASS** = it exits non-zero with `"protocol" must be "tcp" or "udp", got "sctp"`.

### A4 — duplicate udp port is rejected
```bash
./sluice ../tests/configs/tc_udp_dup_port.json ; echo "exit=$?"
```
**PASS** = exits non-zero with `duplicate udp listen_port 9401`.
(Note: A2 proves the *cross-protocol* pair is allowed; only same-protocol
duplicates fail.)

> **Timeout floor** (checked behaviourally in **D1**): a `udp_session_timeout_seconds`
> below 5 is silently clamped up to 5 so a UDP session can never be un-expiring.

---

## B. UDP listen socket

### B1 — the port is bound as UDP
With `tc_udp_single.json` still running from A1, in **T2**:
```bash
ss -ulnp | grep :9401
```
**PASS** = a **UDP** (`ss -u`) listener on `:9401` is shown. (A stray `ss -tlnp`
would **not** list it — it's datagram, not stream.) Ctrl+C the gateway.

---

## C. Core relay (MVP)

### C1 — basic echo
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_single.json`
- **T2**:
  ```bash
  printf 'hello\n' | nc -u -w1 127.0.0.1 9401
  ```
**PASS** =
- T2 prints `[A] hello`.
- T-GW logs `udp session 127.0.0.1:<p> on route "udp-echo" -> 127.0.0.1:7401 (backend fd=<n>)`.
- T1 prints one `from gateway 127.0.0.1:<q>` line — note that peer port `<q>`
  is the gateway's, **not** the client's `<p>` (L4 proxy hides the client).

### C2 — two client ports → two sessions
Keep C1 running. **T2**:
```bash
printf 'one\n' | nc -u -w1 -p 41001 127.0.0.1 9401
printf 'two\n' | nc -u -w1 -p 41002 127.0.0.1 9401
```
**PASS** = T-GW logs **two** distinct `udp session ...` lines (ports 41001 and
41002, different backend fds), and T1 prints **two different** gateway peer
ports. Two client flows = two synthesized sessions. Ctrl+C all.

### C3 — round-robin load balancing
- **T1/T2/T3**: `udp_echo.py 127.0.0.1 7401 A` / `7402 B` / `7403 C`
- **T-GW**: `./sluice ../tests/configs/tc_udp_lb.json`
- **T4** (each iteration is a fresh source port = a fresh session):
  ```bash
  for i in 1 2 3 4 5 6; do printf "req$i\n" | nc -u -w1 127.0.0.1 9401; done
  ```
**PASS** = replies cycle across backends: `[A] req1  [B] req2  [C] req3  [A] req4
[B] req5  [C] req6`. Leave running for E1.

### C4 — TCP and UDP on the same port, side by side
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 U`
- **T2**: `python3 ../tests/backends/echo_any.py 127.0.0.1 7400`   *(TCP)*
- **T-GW**: `./sluice ../tests/configs/tc_udp_coexist.json`
- **T3**:
  ```bash
  printf 'tcp-hi'      | nc    -w1 127.0.0.1 9400   # -> TCP backend, echoes "tcp-hi"
  printf 'udp-hi\n'    | nc -u -w1 127.0.0.1 9400   # -> UDP backend, echoes "[U] udp-hi"
  ```
**PASS** = the TCP send echoes `tcp-hi`; the UDP send echoes `[U] udp-hi`. Same
port `:9400`, two protocols, two backends. Ctrl+C all.

---

## D. Lifecycle

### D1 — idle-timeout reclamation
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_single.json`  *(timeout 5s)*
- **T2** — start an **interactive** client, keep it open:
  ```bash
  nc -u 127.0.0.1 9401
  ```
  Type `hi` + Enter → get `[A] hi`. Note the backend fd in T-GW and the gateway
  peer port in T1. **Now wait ~7 seconds without typing.**

**PASS** =
- T-GW logs `udp session 127.0.0.1:<p> ... idle timeout (5s) exceeded, closing`.
- Back in T2, type `hi again` + Enter → still `[A] hi again`, **but** T-GW logs a
  **new** `udp session ...` with a **different backend fd**, and T1 prints a
  **new** gateway peer port. The old session was reclaimed; the next datagram
  built a fresh one. Ctrl+C all.

*(Floor spot-check, optional: edit `tc_udp_single.json` to
`"udp_session_timeout_seconds": 2`, repeat — the session still survives ~4s of
silence, proving 2 was clamped to 5.)*

### D2 — session cap
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_cap.json`  *(max_udp_sessions 2, timeout 60s)*
- **T2** — three distinct source ports; the 60s timeout keeps the first two
  sessions alive while the third arrives:
  ```bash
  printf 's1\n' | nc -u -w1 -p 42001 127.0.0.1 9401
  printf 's2\n' | nc -u -w1 -p 42002 127.0.0.1 9401
  printf 's3\n' | nc -u -w1 -p 42003 127.0.0.1 9401
  ```
**PASS** =
- `s1` → `[A] s1`, `s2` → `[A] s2` (two sessions created).
- `s3` → **no reply**; T-GW logs `udp session limit (2) reached — dropping
  datagram from 127.0.0.1`.
- Re-send from `-p 42001` → still `[A] ...`: existing sessions keep working while
  new ones are refused. Ctrl+C all.

---

## E. Policy reuse + health

### E1 — session affinity (sticky pin)
Using the **C3** setup (`tc_udp_lb.json`, backends A/B/C still up), **T4**:
```bash
for i in 1 2 3; do printf "pin$i\n" | nc -u -w1 -s 127.0.0.2 127.0.0.1 9401; done
```
**PASS** = **all three** replies are `[C] ...` (source `127.0.0.2` is pinned to
`7403`), and T-GW logs each session line ending in `[affinity]`. Meanwhile
un-pinned clients from `127.0.0.1` still round-robin (re-run C3's loop to confirm).

### E2 — access control blocks a UDP client
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_access.json`  *(blacklist 127.0.0.2)*
- **T2**:
  ```bash
  printf 'ok\n'      | nc -u -w1 -s 127.0.0.1 127.0.0.1 9401   # allowed
  printf 'blocked\n' | nc -u -w1 -s 127.0.0.2 127.0.0.1 9401   # denied
  ```
**PASS** = the `127.0.0.1` send echoes `[A] ok`; the `127.0.0.2` send gets **no
reply** and T-GW logs `blocked udp client 127.0.0.2 on route "udp-guarded"
(global access/rate)`. Same whitelist/blacklist/CIDR engine as TCP. Ctrl+C all.

### E3 — UDP backends are not health-probed
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_single.json`  *(heartbeat every 3s)*
- Watch T-GW for ~10s, and in **T2**: `ss -tnp | grep 7401`
**PASS** =
- **No** probe churn in the log (no `backend 127.0.0.1:7401 is now healthy/…`
  lines appearing on the heartbeat interval) and **no** TCP connection to `:7401`
  in `ss -tnp` — a TCP connect probe would be meaningless for a UDP service.
- Now **Ctrl+C T1** (kill the backend) and send `printf 'x\n' | nc -u -w1 127.0.0.1 9401`
  once, then again. T-GW logs `udp backend 127.0.0.1:7401 refused ... — ending
  session` (passive `ECONNREFUSED`, not an active probe). Restart T1 and it works
  again on the next datagram.

### E4 — stats reflect UDP sessions
With `tc_udp_single.json` running (dashboard on), open
**http://localhost:8080** (log in, default password `root`). Send a few datagrams
(C1) and confirm the `7401` backend's **active** count rises by one per live
session and its **bytes** climb; after the 5s idle timeout the active count falls
back to 0. The `udp_echo` terminal (T1) printing each served datagram is the
ground-truth cross-check. *(A dedicated UDP badge / "sessions" label is Milestone
G; here the existing counters already populate.)*

---

## F. Hot reload, shutdown, multi-process

Reload re-reads the **same** config path on `SIGUSR1`, so these tests edit a live
copy in place. Prep a working config and backends:
```bash
cp ../tests/configs/tc_udp_single.json /tmp/live.json   # udp :9401 -> 7401
```
- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T2**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7402 B`
- **T-GW**: `./sluice /tmp/live.json`

### F1 — hot-add a UDP route
Edit `/tmp/live.json` to add a second UDP route, then signal:
```bash
# add to "routes": { "name":"udp2","listen_port":9402,"protocol":"udp",
#                     "backends":[{"host":"127.0.0.1","port":7402}] }
kill -USR1 $(pgrep -x sluice)
```
**PASS** = T-GW logs `hot-reload: added udp route "udp2" on :9402`, and
`printf 'x\n' | nc -u -w1 127.0.0.1 9402` echoes `[B] x`. The original `:9401`
still echoes `[A] ...`.

### F2 — hot-remove a UDP route
Remove the `udp2` route from `/tmp/live.json`, then `kill -USR1 $(pgrep -x sluice)`.
**PASS** = T-GW logs `hot-reload: udp route "udp2" removed — closed 1 listener(s),
dropped N session(s)`; `nc -u -w1 127.0.0.1 9402` now gets **no reply** (listener
gone); `:9401` keeps working. (`ss -ulnp | grep 9402` shows nothing.)

### F3 — clean shutdown
`Ctrl+C` T-GW. **PASS** = it exits promptly with no error; `ss -ulnp | grep 9401`
is empty afterward (all UDP listeners closed).

### F4 — multi-process flow coherence *(optional)*
Add `"worker_processes": 2` to `/tmp/live.json` and restart. Send several
datagrams from the **same** source port
(`for i in 1 2 3; do printf "$i\n" | nc -u -w1 -p 43001 127.0.0.1 9401; done`).
**PASS** = all three are served by the **same** worker/backend — `SO_REUSEPORT`
keeps one client flow on one worker (the session lives in that worker only).

---

## G. Dashboard / UI

- **T1**: `python3 ../tests/backends/udp_echo.py 127.0.0.1 7401 A`
- **T-GW**: `./sluice ../tests/configs/tc_udp_single.json`  *(dashboard on :8080)*
- Open **http://localhost:8080** and log in (default password `root`).

### G1 — status table
Send a couple of datagrams (`printf 'hi\n' | nc -u -w1 127.0.0.1 9401`) and view
the **STATUS** tab. **PASS** =
- the `udp-echo` route shows a **UDP** badge;
- its active count carries a small **`sess`** label (sessions, not connections),
  and rises while a session is live, returning to 0 after the 5s idle timeout;
- the backend's health cell is a neutral **`—`** (dashed), not UP/DOWN, because
  UDP backends aren't probed.

### G2 — config editor round-trip
Open the **CONFIG** tab. **PASS** =
- the `udp-echo` route shows **PROTOCOL = UDP**, a **SESSION TIMEOUT (s)** field
  (not "Idle Timeout"), the advisory UDP note, and its backend row has **no HB /
  PROXY v1** toggles;
- set SESSION TIMEOUT to `10`, click **SAVE CONFIGURATION** → "Saved and reloaded
  successfully"; reopen CONFIG and the value is still `10` (it round-tripped
  through `config.json` as `udp_session_timeout_seconds`). Switching a route's
  PROTOCOL dropdown to UDP and back to TCP swaps the fields live.

---

## Cleanup
- Ctrl+C every foreground server (gateways and backends).
- Remove the scratch config if you made one: `rm -f /tmp/live.json`.
- No other system state is added (no `ip addr`) — nothing else to undo.

---

### Coverage map
| Milestone | Cases |
|---|---|
| **A** Config | A1–A4 (+ D1 floor note) |
| **B** Listen socket | B1 |
| **C** Core relay | C1–C4 |
| **D** Lifecycle | D1 (idle), D2 (cap) |
| **E** Policy/health | E1 (affinity), E2 (access), E3 (no-probe), E4 (stats) |
| **F** Reload/shutdown/MP | F1 (add), F2 (remove), F3 (shutdown), F4 (MP) |
| **G** Dashboard/UI | G1 (status), G2 (config editor) |
