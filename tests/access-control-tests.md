# Sluice — Access Control & Rate Limiting Test Cases

Tests for IP whitelist/blacklist (global + per-backend), backend `disabled`,
blacklist timeouts, and per-IP rate limiting with exponential-backoff blocks.
Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up src/access.cpp
make
```

### Simulating different client IPs (important)
All local traffic is normally from `127.0.0.1`, but the **entire `127.0.0.0/8`
range is loopback**, so you can make a client connect *from* a chosen source IP
with `nc -s`:
```bash
nc -s 127.0.0.5 127.0.0.1 9000      # gateway sees client IP 127.0.0.5
nc      127.0.0.1 9000              # gateway sees client IP 127.0.0.1
```
For quick connect-then-close (rate tests), use `nc -z`:
```bash
nc -z -s 127.0.0.7 127.0.0.1 9000
```

### Backends
- Multi-client: `test_utils/echo_server <port>`
- Single-client: `test_utils/echo_backend.py <port>`

### Where the decisions show up
Blocks and rate events are logged (stderr / log file). The routed backend for
each accepted client appears in the `accepted client ... -> host:port` line.

---

## A. Global blacklist

Start backends + gateway:
```bash
test_utils/echo_server 7003 &
test_utils/echo_server 7004 &
test_utils/echo_backend.py 7001 &
./sluice ../config/access_config.json
```
(`access_config.json` blacklists `127.0.0.9` permanently and `127.0.0.8` for 15s;
global rate limit 5/min.)

### A1 — permanent blacklist blocks
```bash
nc -z -s 127.0.0.9 127.0.0.1 9000 ; echo "rc=$?"     # blocked
nc -z            127.0.0.1 9000 ; echo "rc=$?"       # from 127.0.0.1 -> allowed
```
**Expect:** the `127.0.0.9` attempt is refused/closed with a gateway log
`blocked client 127.0.0.9 on route "payments" (global access/rate)`. The
`127.0.0.1` attempt is accepted and relayed normally.

### A2 — timed blacklist expires at an absolute time (survives restart)
The static timeout is an **absolute datetime** (`"until": "YYYY-MM-DDTHH:MM:SS"`,
local time), so it ends at the same instant regardless of restarts. To watch one
expire, generate a config whose `until` is ~20s in the future:
```bash
UNTIL=$(date -d '+20 seconds' '+%Y-%m-%dT%H:%M:%S')
cat > /tmp/timed.json <<EOF
{ "dashboard":{"enabled":false},
  "blacklist":[ { "ip":"127.0.0.8", "until":"$UNTIL" } ],
  "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }
EOF
test_utils/echo_backend.py 7001 &
./sluice /tmp/timed.json
```
Then, in another terminal:
```bash
echo hi | nc -N -s 127.0.0.8 127.0.0.1 9000    # before UNTIL: blocked -> no echo
sleep 21
echo hi | nc -N -s 127.0.0.8 127.0.0.1 9000    # after UNTIL: allowed -> "hi"
```
**Tests:** absolute-time expiry.
**Expect:** blocked (no echo + `blocked client 127.0.0.8 ...` log) until `UNTIL`,
allowed afterward.
**Restart check:** stop and restart the gateway a few seconds before `UNTIL` — the
block still lifts at `UNTIL`, not `restart + 20s` (that's the point of using an
absolute time).

---

## B. Global whitelist

```bash
test_utils/echo_backend.py 7001 &
./sluice ../config/access_whitelist.json     # whitelist: ["127.0.0.5"]
```
### B1 — whitelisting message + enforcement
**Expect at startup:** a log line `whitelisting enabled: only whitelisted clients
can connect`.
```bash
nc -z -s 127.0.0.5 127.0.0.1 9000 ; echo "rc=$?"      # whitelisted -> allowed
nc -z            127.0.0.1 9000 ; echo "rc=$?"        # not whitelisted -> blocked
```
**Expect:** `127.0.0.5` connects; `127.0.0.1` is blocked (`global access/rate`
log line).

### B2 — both whitelist and blacklist configured => warning
```bash
./sluice ../config/access_both.json
```
**Expect at startup:** `whitelisting enabled ...` **and** `warning: both whitelist
and blacklist set globally; blacklist (deny) takes precedence`. A blacklisted IP
is still denied even though whitelisting is on (deny wins).

---

## C. Per-backend whitelist / blacklist / disabled

Using `access_config.json` (orders route on 9003):
- backend `7003` whitelists `127.0.0.5`
- backend `7004` blacklists `127.0.0.5`
- backend `7005` is `disabled`

Backends up on 7003 and 7004 (7005 needs none — it's disabled).

### C1 — per-backend routing respects the lists
```bash
# client 127.0.0.5: 7004 blacklists it, 7005 disabled -> must land on 7003
nc -z -s 127.0.0.5 127.0.0.1 9003
# client 127.0.0.1: 7003 whitelist excludes it, 7005 disabled -> must land on 7004
nc -z            127.0.0.1 9003
```
**Tests:** per-backend ACL filters the selection; disabled backend never chosen.
**Expect (gateway logs):**
- `127.0.0.5` → `-> 127.0.0.1:7003`
- `127.0.0.1` → `-> 127.0.0.1:7004`
- **Never** `-> 127.0.0.1:7005`.

### C2 — disabled backend is never health-probed
Leave the gateway idle for a couple of heartbeat intervals. `7005` has no server
running, yet:
**Expect:** **no** `backend 127.0.0.1:7005 is now unhealthy` log — a disabled
backend is skipped by heartbeats entirely (no probe attempted).

### C3 — all backends block the client => dropped
Blacklist a client on the only eligible backends (or point it at a route where it
matches none). E.g. from `127.0.0.5` to a route where every non-disabled backend
blacklists it → **Expect:** `no eligible backend for client 127.0.0.5 ... —
dropping`.

---

## D. Rate limiting + exponential backoff

`access_config.json`: global 5/min, `backoff_base_seconds: 10`, cap 60s.

### D1 — global per-IP limit trips and blocks
```bash
for i in $(seq 1 8); do nc -z -s 127.0.0.7 127.0.0.1 9000; done
```
**Tests:** the 6th connection crosses the limit; further ones are blocked.
**Expect (logs):**
- connections 1–6 accepted;
- on the 6th: `rate limit exceeded by 127.0.0.7 (global): blocked for 10s (violation 1)`;
- connections 7–8: `blocked client 127.0.0.7 ... (global access/rate)`.

### D2 — backoff doubles on repeat violation
```bash
sleep 11                                   # let the 10s block expire
for i in $(seq 1 6); do nc -z -s 127.0.0.7 127.0.0.1 9000; done
```
**Expect:** a second violation logs `blocked for 20s (violation 2)` (then 40s,
capped at 60s on further violations).

### D3 — per-backend rate limit
Backend `7004` has `rate_limit.max_per_minute: 3`. From an IP that routes to 7004
(e.g. `127.0.0.1` on the orders route):
```bash
for i in $(seq 1 5); do nc -z 127.0.0.1 9003; done
```
**Expect:** after 3 connections routed to 7004, a
`rate limit exceeded by 127.0.0.1 (backend 127.0.0.1:7004): blocked for ...` log;
subsequent connections from that IP find 7004 ineligible (and, since 7003 excludes
`127.0.0.1` and 7005 is disabled, get **dropped** with `no eligible backend`).

---

## E. Multi-process (limits divided by N)

Make `access_config_mp.json` = `access_config.json` with `"worker_processes": 4`.
```bash
./sluice ../config/access_config_mp.json
```
**Expect:**
- Summary shows `rate limit: 5/min per IP (global), ~1/min per worker`.
- Each worker enforces ~limit/N; the aggregate approximates the configured limit.
- **Caveat (by design):** static ACLs are identical across workers (correct), but
  rate counters + dynamic blocks are **per worker** — an IP may need to trip
  several workers before it's blocked everywhere, and a block on one worker doesn't
  apply to the others. Documented behavior.

---

## F. Config validation

Each should fail fast, non-zero exit, clear message.
```bash
# blacklist entry wrong type
echo '{ "blacklist":[123], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/bl.json
./sluice /tmp/bl.json; echo "exit=$?"
# -> config: blacklist entries must be a string or an object   exit=1

# blacklist object without ip
echo '{ "blacklist":[{"until":"2030-01-01T00:00:00"}], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/bl2.json
./sluice /tmp/bl2.json; echo "exit=$?"
# -> config: blacklist object needs a string "ip"   exit=1

# blacklist "until" bad datetime
echo '{ "blacklist":[{"ip":"1.2.3.4","until":"soon"}], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/bl3.json
./sluice /tmp/bl3.json; echo "exit=$?"
# -> config: blacklist "until" must be "YYYY-MM-DDTHH:MM:SS" (local time), got: soon   exit=1

# rate_limit max_per_minute < 1
echo '{ "rate_limit":{"max_per_minute":0}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/rl.json
./sluice /tmp/rl.json; echo "exit=$?"
# -> config: rate_limit "max_per_minute" must be >= 1   exit=1

# backend rate_limit not an object
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001,"rate_limit":500}]}] }' > /tmp/brl.json
./sluice /tmp/brl.json; echo "exit=$?"
# -> route[0]: backend "rate_limit" must be an object with integer "max_per_minute"   exit=1

# disabled wrong type
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001,"disabled":"yes"}]}] }' > /tmp/dis.json
./sluice /tmp/dis.json; echo "exit=$?"
# -> route[0]: backend "disabled" must be a boolean   exit=1
```

---

## G. Backward compatibility

### G1 — config with no access/rate fields still works
```bash
test_utils/echo_backend.py 7001 &
./sluice ../config/config.json
```
**Expect:** starts normally; summary shows `rate limit: 1000/min per IP (global)`
(the default); no ACL restrictions; all clients allowed (subject only to the
default 1000/min). Relay behaves exactly as before.

---

## Helper configs to create
- `config/access_config_mp.json` — copy of `access_config.json` + `"worker_processes": 4`.

---

## Decisions baked in (for reference)
- **Deny beats allow:** blacklist (incl. dynamic rate blocks) is checked before the
  whitelist, so a rate-limited IP is blocked even in whitelist mode.
- **Order:** global gate first (reject early), then per-backend during selection.
- **Static blacklist `until`** is an **absolute local datetime** (so the block ends
  at the same instant across restarts); a plain-string entry is permanent.
- **Rate limit** is **per-IP**, enforced by dynamically blacklisting the offender
  with exponential backoff (`base × 2^(violations−1)`, capped).
- **Multi-process:** per-worker limits = `max_per_minute / worker_processes`;
  dynamic state is per-worker (aggregate is approximate).
- **IP matching is exact** (no CIDR ranges yet — a possible future enhancement).
