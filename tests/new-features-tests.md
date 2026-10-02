# Sluice — Idle Timeout, CIDR, and Sticky-Routing Test Cases

Tests for three additions: per-connection **idle timeout** (global + per-route
override), **CIDR ranges** in whitelist/blacklist entries (global and
per-backend), and **affinity** (sticky) routing that pins a client IP/CIDR to a
specific backend within a route, falling back to normal load balancing when
that backend isn't eligible. Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the access.cpp / config.cpp / gateway.cpp changes
make
```
Note: `CMakeLists.txt`'s `setup` target now also links `src/access.cpp` (needed
because `config.cpp` calls `parse_cidr`/`parse_ipv4`, which live there) — if you
pulled this before that fix, re-run `cmake ..` to pick it up.

### Simulating different client IPs
All of `127.0.0.0/8` is loopback, so a client can connect *from* a chosen
source IP with `nc -s`:
```bash
nc -s 127.0.5.3 127.0.0.1 9003        # gateway sees client IP 127.0.5.3
nc -z -s 127.0.5.3 127.0.0.1 9003     # connect-then-close, for quick checks
```

### Backends
- Multi-client: `test_utils/echo_server <port>`
- Single-client: `test_utils/echo_backend.py <port>`

### Config files used below
- `config/idle_timeout_config.json` — global `idle_timeout_seconds: 6`; route
  `inherits_global` (:9000→:7001, no override), route `no_idle` (:9001→:7002,
  override `0`), route `custom_idle` (:9002→:7003, override `3`).
- `config/cidr_config.json` — global blacklist mixes an exact IP (`127.0.0.9`)
  and a CIDR (`127.0.9.0/24`); route `orders` (:9003) has backend `7003`
  (whitelist CIDR `127.0.5.0/28`) and backend `7004` (blacklist CIDR
  `127.0.6.0/24`).
- `config/affinity_config.json` — route `cache` (:9010→[:7010,:7011], heartbeat
  every 3s), affinity rule pins `127.0.20.0/24` → `127.0.0.1:7010`; `7010` also
  blacklists `127.0.20.5` specifically (to test the access-deny fallback case).

---

## A. Idle timeout

Start:
```bash
test_utils/echo_backend.py 7001 &
test_utils/echo_backend.py 7002 &
test_utils/echo_backend.py 7003 &
./sluice ../config/idle_timeout_config.json
```

### A1 — global timeout closes a silent connection
```bash
nc 127.0.0.1 9000    # inherits_global: idle_timeout_seconds = 6 (from global)
# don't type anything — just wait
```
**Tests:** a connection with zero bytes transferred in either direction gets
closed once it's been idle past the resolved timeout.
**Expect:** within ~6-7s (sweep runs every 1s, so up to 1s of slack past the
6s mark) the `nc` session is closed by the gateway. Log shows:
`route "inherits_global": idle timeout (6s) exceeded (fd=...), closing`.

### A2 — route override `0` disables idle timeout even though global is on
```bash
nc 127.0.0.1 9001    # no_idle: idle_timeout_seconds = 0 (explicit override)
# wait at least 15s — longer than global's 6s
```
**Tests:** an explicit `0` override beats the global value; it is not treated
as "unset".
**Expect:** the connection is still open after 15+s; no idle-timeout log line
for this route. Close it manually (Ctrl+C) when done.

### A3 — route override shorter than global
```bash
nc 127.0.0.1 9002    # custom_idle: idle_timeout_seconds = 3
```
**Tests:** the per-route override value is actually used, not just whether
it's zero/nonzero.
**Expect:** closes at ~3-4s (faster than A1's 6-7s), confirming the override
value — not the global — was applied.

### A4 — activity resets the idle clock
```bash
nc 127.0.0.1 9002    # custom_idle: 3s timeout
# every ~2s, type a character and press enter (keeps it under the 3s limit)
```
**Tests:** `last_activity` advances on traffic in either direction, so a
connection with regular (if sparse) traffic never idle-closes.
**Expect:** the connection stays open across several 2s gaps, closing only
once you stop sending input and let the gap exceed 3s.

### A5 — idle timeout only affects new connections after a hot reload
With A1's gateway still running (single-process, dashboard would normally be
used to trigger reload — here send `SIGUSR1` directly since the dashboard is
disabled in this config):
```bash
nc 127.0.0.1 9000 &     # opens BEFORE the reload
sleep 1
# edit idle_timeout_config.json: change global "idle_timeout_seconds" to 100
kill -USR1 $(pgrep -f 'sluice ../config/idle_timeout_config.json')
sleep 8
jobs    # is the nc from before the reload still running?
```
**Tests:** a Connection resolves and freezes its timeout at accept() time —
reload does not retroactively change already-open connections.
**Expect:** the `nc` opened before the reload still gets closed around the
*original* 6s deadline (i.e. it's gone by the time you check at ~9s), even
though the config now says 100s. A *new* `nc 127.0.0.1 9000` opened after the
reload uses the new 100s value.

---

## B. CIDR whitelist / blacklist

Start:
```bash
test_utils/echo_backend.py 7003 &
test_utils/echo_backend.py 7004 &
./sluice ../config/cidr_config.json
```

### B1 — global CIDR blacklist blocks the whole range
```bash
nc -z -s 127.0.9.50 127.0.0.1 9003 ; echo "rc=$?"     # in 127.0.9.0/24 -> blocked
nc -z -s 127.0.1.50 127.0.0.1 9003 ; echo "rc=$?"     # outside the range -> allowed
```
**Tests:** a CIDR entry in the blacklist matches every address in the range,
not just the network address.
**Expect:** `127.0.9.50` is refused with a
`blocked client 127.0.9.50 on route "orders" (global access/rate)` log; the
127.0.1.x attempt is accepted (subject to per-backend rules below).

### B2 — mixed exact-IP + CIDR list both work together
```bash
nc -z -s 127.0.0.9 127.0.0.1 9003 ; echo "rc=$?"      # exact-IP entry -> blocked
```
**Tests:** `parse_access` correctly classifies each list entry independently
(exact IP into the exact set, CIDR into the CIDR list) within the same array.
**Expect:** blocked, same as B1 — the exact-IP entry in the same `blacklist`
array as the CIDR entry still works.

### B3 — per-backend CIDR whitelist restricts to that range
```bash
# in the whitelisted /28 (127.0.5.0-127.0.5.15) -> should land on 7003
nc -z -s 127.0.5.3 127.0.0.1 9003
# just outside the /28 -> 7003's whitelist excludes it, falls to 7004
nc -z -s 127.0.5.20 127.0.0.1 9003
```
**Tests:** CIDR whitelist boundary is exact (a /28 is 16 addresses:
`.0`-`.15`).
**Expect (gateway logs):** `127.0.5.3` → `-> 127.0.0.1:7003`; `127.0.5.20` →
`-> 127.0.0.1:7004` (since it's outside 7003's whitelist range but 7004 has no
restriction on it).

### B4 — per-backend CIDR blacklist excludes that range
```bash
nc -z -s 127.0.6.9 127.0.0.1 9003
```
**Tests:** CIDR blacklist on a specific backend.
**Expect:** `127.0.6.9` is not in 7003's whitelisted /28, so round-robin tries
7004 next — but 7004 blacklists `127.0.6.0/24`, so **no** backend is eligible:
`no eligible backend for client 127.0.6.9 on route "orders" — dropping`.

### B5 — timed CIDR blacklist entry expires
```bash
UNTIL=$(date -d '+15 seconds' '+%Y-%m-%dT%H:%M:%S')
cat > /tmp/timed_cidr.json <<EOF
{ "dashboard":{"enabled":false},
  "blacklist":[ { "ip":"127.0.9.0/24", "until":"$UNTIL" } ],
  "routes":[{"name":"a","listen_port":9020,"backends":[{"host":"127.0.0.1","port":7001}]}] }
EOF
test_utils/echo_backend.py 7001 &
./sluice /tmp/timed_cidr.json &
sleep 1
echo hi | nc -N -s 127.0.9.5 127.0.0.1 9020      # before UNTIL: blocked, no echo
sleep 16
echo hi | nc -N -s 127.0.9.5 127.0.0.1 9020      # after UNTIL: allowed, echoes "hi"
```
**Tests:** the absolute-datetime expiry logic (already tested for exact IPs in
`access-control-tests.md` A2) applies equally to CIDR blacklist entries.
**Expect:** blocked before `UNTIL`, allowed after.

### B6 — malformed CIDR rejected at config load
```bash
echo '{ "blacklist":["10.0.0.0/33"], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/badcidr1.json
./sluice /tmp/badcidr1.json; echo "exit=$?"
# -> config blacklist: invalid CIDR "10.0.0.0/33" (expected e.g. "10.0.0.0/24", prefix 0-32)   exit=1

echo '{ "blacklist":["10.0.0/24"], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/badcidr2.json
./sluice /tmp/badcidr2.json; echo "exit=$?"
# -> config blacklist: invalid CIDR "10.0.0/24" (expected e.g. "10.0.0.0/24", prefix 0-32)   exit=1
```
**Tests:** config validation catches bad CIDR syntax at startup instead of the
entry silently never matching.
**Expect:** both fail fast with the CIDR error and a non-zero exit code.

### B7 — dashboard round-trip: removing a CIDR blacklist entry un-blocks on reload (no restart)
Enable the dashboard on `cidr_config.json` (`"dashboard": {"enabled": true, "port": 8080}`),
start the gateway, confirm B1 still blocks `127.0.9.50`, then:
```bash
curl -s http://localhost:8080/api/config | python3 -c \
  "import json,sys; c=json.load(sys.stdin); c['blacklist']=['127.0.0.9']; print(json.dumps(c))" \
  > /tmp/cidr_no_range.json
curl -s -X POST http://localhost:8080/api/config/save \
  -H 'Content-Type: application/json' --data @/tmp/cidr_no_range.json
curl -s -X POST http://localhost:8080/api/reload
sleep 1
nc -z -s 127.0.9.50 127.0.0.1 9003 ; echo "rc=$?"    # should now be ALLOWED
```
**Tests:** CIDR blacklist entries are cleared-and-rebuilt each reload (no
dynamic violation state to preserve), unlike single-IP blacklist entries —
so removing one and reloading actually un-blocks the range, without a restart.
**Expect:** `127.0.9.50` connects successfully after the reload.

---

## C. Sticky routes (affinity)

Start:
```bash
test_utils/echo_backend.py 7010 &
test_utils/echo_backend.py 7011 &
./sluice ../config/affinity_config.json
```

### C1 — matched client always lands on the pinned backend
```bash
for i in 1 2 3 4; do nc -z -s 127.0.20.10 127.0.0.1 9010; done
```
**Tests:** an affinity match bypasses round-robin entirely — every connection
from the matching client goes to the same backend.
**Expect (gateway logs):** all four show
`route "cache": client 127.0.20.10 pinned by affinity rule -> 127.0.0.1:7010`
and `-> 127.0.0.1:7010` in the accepted-client line — never `:7011`.

### C2 — unmatched client uses normal round-robin
```bash
for i in 1 2 3 4; do nc -z -s 127.0.30.10 127.0.0.1 9010; done
```
**Tests:** clients outside any affinity rule's CIDR are unaffected — no
regression to the existing load-balancing behavior.
**Expect:** connections alternate between `7010` and `7011` (round-robin), no
"pinned by affinity" log line.

### C3 — pinned backend goes unhealthy → falls back to round-robin
```bash
pkill -f 'echo_backend.py 7010'
sleep 4     # let a heartbeat interval (3s) pass so 7010 is marked unhealthy
nc -z -s 127.0.20.10 127.0.0.1 9010 ; echo "rc=$?"
```
**Tests:** the "if unhealthy, follow normal routing rules" fallback.
**Expect:** `backend 127.0.0.1:7010 is now unhealthy` appears in the logs
first; the affinity-matched client's connection now goes to `7011` (no
"pinned by affinity" line this time, since `select_affinity_backend` returned
nullptr and normal `select_backend` took over), and it succeeds (`rc=0`).

### C4 — pinned backend recovers → resumes being used
```bash
test_utils/echo_backend.py 7010 &
sleep 4     # let it be probed healthy again
nc -z -s 127.0.20.10 127.0.0.1 9010
```
**Tests:** the fallback is dynamic, re-evaluated on every connection — not
"sticky to the fallback" once it happens once.
**Expect:** `backend 127.0.0.1:7010 is now healthy` in the logs, then the next
connection from `127.0.20.10` is pinned back to `7010` (log line reappears).

### C5 — access-denied on the pinned backend also falls back
`127.0.20.5` is inside the affinity CIDR (`127.0.20.0/24`) but is specifically
blacklisted on the pinned backend `7010` in `affinity_config.json`:
```bash
nc -z -s 127.0.20.5 127.0.0.1 9010 ; echo "rc=$?"
```
**Tests:** the fallback isn't limited to health — any reason the pinned
backend is ineligible for this client (disabled, retiring, unhealthy, or
access-denied) triggers the same fallback to normal routing.
**Expect:** no "pinned by affinity" log line (the rule matched, but its
target wasn't eligible); the connection instead lands on `7011` and succeeds.
Meanwhile `127.0.20.10` (not blacklisted) still pins to `7010` as in C1 —
confirming this is a per-client outcome, not a route-wide one.

---

## D. Config validation

Each should fail fast, non-zero exit, clear message.
```bash
# global idle_timeout_seconds negative
echo '{ "idle_timeout_seconds": -1, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/it1.json
./sluice /tmp/it1.json; echo "exit=$?"
# -> config: "idle_timeout_seconds" must be >= 0   exit=1

# per-route idle_timeout_seconds wrong type
echo '{ "routes":[{"name":"a","listen_port":9000,"idle_timeout_seconds":"soon","backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/it2.json
./sluice /tmp/it2.json; echo "exit=$?"
# -> route[0]: "idle_timeout_seconds" must be an integer   exit=1

# affinity entry missing "host"
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}],"affinity":[{"client":"10.0.0.0/24","port":7001}]}] }' > /tmp/aff1.json
./sluice /tmp/aff1.json; echo "exit=$?"
# -> route[0]: missing required field "host"   exit=1

# affinity "client" not a valid IP or CIDR
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}],"affinity":[{"client":"not-an-ip","host":"127.0.0.1","port":7001}]}] }' > /tmp/aff2.json
./sluice /tmp/aff2.json; echo "exit=$?"
# -> route[0]: affinity "client" "not-an-ip" must be a valid IPv4 address or CIDR range   exit=1

# affinity "host" not a valid IPv4 (hostnames aren't supported, same as backends)
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}],"affinity":[{"client":"10.0.0.0/24","host":"example.com","port":7001}]}] }' > /tmp/aff3.json
./sluice /tmp/aff3.json; echo "exit=$?"
# -> route[0]: affinity "host" "example.com" must be a valid IPv4 address   exit=1

# affinity "port" out of range
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}],"affinity":[{"client":"10.0.0.0/24","host":"127.0.0.1","port":70000}]}] }' > /tmp/aff4.json
./sluice /tmp/aff4.json; echo "exit=$?"
# -> route[0]: affinity "port" 70000 out of range (1-65535)   exit=1
```

---

## E. Dashboard — CONFIG tab

Manual UI checks (start any config with the dashboard enabled, open
`http://localhost:8080`, CONFIG tab):

### E1 — global idle timeout field round-trips
Set the **IDLE TIMEOUT** block's seconds field to `45`, SAVE TO DISK.
**Expect:** `curl -s http://localhost:8080/api/config` shows
`"idle_timeout_seconds": 45` at the root. Reloading the CONFIG tab shows `45`
still in the field (not reset to `0`).

### E2 — per-route idle timeout: blank means inherit, not zero
Leave a route's **IDLE TIMEOUT (s)** field blank, SAVE TO DISK.
**Expect:** the saved JSON for that route has **no** `idle_timeout_seconds`
key at all (distinct from `0`). Typing `0` and saving instead produces
`"idle_timeout_seconds": 0` in that route's object.

### E3 — sticky routes editor add/remove
Under a route's **STICKY ROUTES** section, click **+ ADD STICKY ROUTE**, fill
in client `10.0.0.0/24`, host `127.0.0.1`, port `7001`, SAVE TO DISK.
**Expect:** the route's JSON gains
`"affinity":[{"client":"10.0.0.0/24","host":"127.0.0.1","port":7001}]`.
Removing the row (✕) and saving again drops the `affinity` key entirely (empty
array is omitted, matching how empty whitelist/blacklist are handled).

### E4 — whitelist/blacklist fields accept and validate CIDR
In any backend's ACCESS panel, enter `10.0.0.0/24` in the whitelist textarea
and `10.0.0.0/33` (invalid) as a blacklist IP. Click SAVE TO DISK.
**Expect:** the whitelist entry is accepted; the invalid blacklist CIDR shows
an inline `Invalid IPv4 or CIDR` error under that row and the save is blocked
until it's fixed or removed.

---

## Notes / known behavior

- **Idle timeout resolves once, at accept time.** A hot reload changing the
  global or a route's `idle_timeout_seconds` only affects connections accepted
  *after* the reload (A5). This mirrors how per-backend rate limits already
  behave for already-selected backends.
- **Sweep granularity is 1 second.** Idle timeouts can fire up to ~1s later
  than the configured value; don't rely on sub-second precision in assertions.
- **CIDR blacklist entries are NOT merge-only.** Unlike single-IP blacklist
  entries (which persist their `violations` count across reloads by design —
  see `access-control-tests.md`), CIDR entries have no dynamic state, so a
  reload fully rebuilds them — removals actually take effect (B7).
- **Affinity only considers the FIRST matching rule.** If that rule's target
  backend isn't eligible, the client falls straight to normal round-robin —
  it does not try a second affinity rule even if one would also match.
- **IPv4 only**, consistent with the rest of the gateway (backends, CIDR,
  affinity `client`/`host` — no IPv6, no hostnames).
