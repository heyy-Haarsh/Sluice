# Sluice — IPv6 Test Cases

Tests for IPv6 support: a **dual-stack listener** (one socket per route accepts
both IPv6 and IPv4 clients), **IPv6 backends** (the gateway dials `::1`-style
hosts), and **IPv6 in access control** (exact + CIDR whitelist/blacklist, plus
IPv4-mapped normalization so existing IPv4 rules keep working). Run on
WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the access / config / gateway changes
make
```

### Test methodology
- **No background processes.** Each server runs in the foreground in its own
  terminal, labelled **T1, T2, T3, ...** — open a new one for each.
- **Stop a foreground server with Ctrl+C** when a test says so.
- Python backends are launched with **`python3`**.

### Confirm IPv6 loopback works
```bash
ping6 -c1 ::1        # should succeed; if not, IPv6 is disabled on this host
```
If `::1` is unreachable, the gateway still runs (it falls back to IPv4-only
listening) but these tests don't apply.

### Extra IPv6 source addresses (for the access-control section only)
Unlike IPv4 (the whole `127.0.0.0/8` is loopback), IPv6 has only `::1` by
default, so to simulate *distinct* IPv6 clients we add unique-local (ULA)
addresses to `lo`. **Section C** needs these; A and B don't.
```bash
sudo ip -6 addr add fd00::9/128       dev lo   # exact-blacklisted client
sudo ip -6 addr add fd00:beef::5/128  dev lo   # inside a blacklisted CIDR
sudo ip -6 addr add fd00:cafe::5/128  dev lo   # inside a whitelisted CIDR
sudo ip -6 addr add fd00:9999::5/128  dev lo   # outside every rule
```
(Removed again in the **Cleanup** section at the end.)

### Backends
- `tests/backends/echo_any.py <host> <port>` — echoes; binds IPv4 or IPv6 based
  on the host (a `:` means IPv6). Prints the connecting peer (the gateway).

### Config files used
- `config/ipv6_config.json` — dual-stack `:9000` → IPv4 backend `127.0.0.1:7001`.
- `config/ipv6_backend_config.json` — `:9000` → **IPv6** backend `::1:7001`, heartbeat on (3s).
- `config/ipv6_mixed_config.json` — `:9000` → `127.0.0.1:7001` + `::1:7002`.
- `config/ipv6_access_config.json` — global IPv6 blacklist (exact + CIDR);
  route `vip` (:9001) backend `7002` with an IPv6 whitelist CIDR.

---

## A. Dual-stack listener (IPv6 clients on the frontend)

**T1** — an IPv4 backend:
```bash
python3 tests/backends/echo_any.py 127.0.0.1 7001
```
**T2** — the gateway:
```bash
./build/sluice config/ipv6_config.json
```

### A1 — an IPv6 client reaches the gateway
**T3**:
```bash
echo hello | nc -N -6 ::1 9000
```
**Tests:** the single dual-stack listener accepts an IPv6 connection and relays
it to an IPv4 backend.
**Expect:** T3 gets `hello` echoed. T2's log shows
`accepted client fd=... from [::1]:<port> on route "app" -> 127.0.0.1:7001`
— note the **bracketed IPv6** address.

### A2 — an IPv4 client still works, normalized (regression)
**T3**:
```bash
echo hi | nc -N 127.0.0.1 9000
```
**Tests:** IPv4 clients arriving on the dual-stack (IPv6) socket appear as
IPv4-mapped `::ffff:127.0.0.1` at the kernel, but the gateway normalizes them.
**Expect:** T3 gets `hi` echoed. T2's log shows `from 127.0.0.1:<port>` —
**plain dotted-quad, NOT** `::ffff:127.0.0.1` and not bracketed. This is what
keeps every existing IPv4 access-control rule and test valid.

---

## B. IPv6 backends

### B1 — the gateway dials an IPv6 backend
**T1** — an IPv6 backend on `::1`:
```bash
python3 tests/backends/echo_any.py ::1 7001
```
**T2**:
```bash
./build/sluice config/ipv6_backend_config.json
```
**T3**:
```bash
echo hello | nc -N 127.0.0.1 9000
```
**Tests:** `open_nonblocking_connect` picks `AF_INET6` from the `::1` host literal.
**Expect:** T3 gets `hello` echoed. T1 prints `[conn] from gateway ::1:<port>`.
T2's log shows `-> ::1:7001`.

### B2 — IPv6 backend health via heartbeat
`ipv6_backend_config.json` has heartbeat on (3s). With T2 up and T1 (the `::1`
backend) **not yet started**, watch the log, then start T1.
**Tests:** heartbeat probes connect to an IPv6 backend.
**Expect:** before T1 is up, after a few seconds the log shows
`backend ::1:7001 is now unhealthy`; once T1 starts, within ~3s
`backend ::1:7001 is now healthy`.

### B3 — mixed IPv4 + IPv6 backends on one route (round-robin)
**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `python3 tests/backends/echo_any.py ::1 7002`   *(new terminal)*
**T3** `./build/sluice config/ipv6_mixed_config.json`   *(new terminal)*
**T4**:
```bash
for i in 1 2 3 4; do echo "hi-$i" | nc -N 127.0.0.1 9000; done
```
**Tests:** the two backends of different families are both selectable.
**Expect:** the connections alternate — the T1 (`127.0.0.1:7001`) and T2
(`::1:7002`) terminals each print `[conn]` lines for roughly half. T3's logs
alternate `-> 127.0.0.1:7001` and `-> ::1:7002`.

---

## C. IPv6 access control

First add the ULA source addresses (see Setup). Then:
**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`
**T2** `python3 tests/backends/echo_any.py 127.0.0.1 7002`   *(new terminal; for the vip route)*
**T3** `./build/sluice config/ipv6_access_config.json`   *(new terminal)*

### C1 — exact IPv6 blacklist + canonicalization
The config blacklists `fd00:0:0:0:0:0:0:9` — a **non-canonical** spelling of
`fd00::9`. The client's IP is reported canonically (`fd00::9`), so this only
blocks if the gateway canonicalizes the config entry too.
**T4**:
```bash
echo x | nc -N -s fd00::9    ::1 9000 ; echo "rc=$?"    # blocked
echo x | nc -N -s fd00:9999::5 ::1 9000 ; echo "rc=$?"  # allowed
```
**Expect:** the `fd00::9` client is blocked —
`blocked client fd00::9 on route "gate" (global access/rate)` — proving the
config's expanded form was canonicalized to match. `fd00:9999::5` is allowed
and echoes `x`.

### C2 — IPv6 CIDR blacklist
`fd00:beef::/32` is blacklisted.
**T4**:
```bash
echo x | nc -N -s fd00:beef::5 ::1 9000 ; echo "rc=$?"   # in range -> blocked
echo x | nc -N -s fd00:9999::5 ::1 9000 ; echo "rc=$?"   # out of range -> allowed
```
**Expect:** `fd00:beef::5` blocked (`blocked client fd00:beef::5 ...`);
`fd00:9999::5` allowed.

### C3 — IPv6 CIDR whitelist (per-backend, on the vip route)
Route `vip` (:9001) backend `7002` whitelists `fd00:cafe::/32` — only that
range may use it.
**T4**:
```bash
echo x | nc -N -s fd00:cafe::5 ::1 9001 ; echo "rc=$?"   # whitelisted -> allowed
echo x | nc -N -s fd00:9999::5 ::1 9001 ; echo "rc=$?"   # not whitelisted -> dropped
```
**Expect:** `fd00:cafe::5` reaches `7002` and echoes `x`. `fd00:9999::5` finds
no eligible backend (`no eligible backend for client fd00:9999::5 ... — dropping`).

### C4 — IPv4 rules unaffected by the v6 work (mapping regression)
Add a quick IPv4 blacklist check to prove the mapped-normalization path still
feeds IPv4 strings into access control. Create `/tmp/v4rule.json`:
```bash
cat > /tmp/v4rule.json <<'EOF'
{ "dashboard":{"enabled":false}, "heartbeat":{"enabled":false},
  "blacklist":["127.0.0.9"],
  "routes":[{"name":"a","listen_port":9002,"backends":[{"host":"127.0.0.1","port":7001}]}] }
EOF
```
Restart T3 against it (`./build/sluice /tmp/v4rule.json`), then **T4**:
```bash
echo x | nc -N -s 127.0.0.9 127.0.0.1 9002 ; echo "rc=$?"   # blocked
echo x | nc -N -s 127.0.0.5 127.0.0.1 9002 ; echo "rc=$?"   # allowed
```
**Tests:** an IPv4 client arriving on the dual-stack socket (as `::ffff:127.0.0.9`)
is normalized to `127.0.0.9` before the ACL check, so a plain IPv4 blacklist
still matches.
**Expect:** `127.0.0.9` blocked, `127.0.0.5` allowed — identical to the
pre-IPv6 behavior.

### C5 — v4 and v6 CIDRs don't cross-match
Using `ipv6_access_config.json` (restart T3 on it): the IPv6 CIDR
`fd00:beef::/32` must not match any IPv4 client, and the IPv4 world must not
match IPv6 ranges. Send an IPv4 client to the `gate` route:
```bash
echo x | nc -N 127.0.0.1 9000 ; echo "rc=$?"   # IPv4 client, only v6 rules exist -> allowed
```
**Expect:** allowed — the IPv6 blacklist entries never match an IPv4 client
(the mapped IPv4 address can't fall inside `fd00:beef::/32`).

---

## D. Config validation

Each should fail fast, non-zero exit, clear message.
```bash
# IPv6 CIDR prefix out of range
echo '{ "blacklist":["fd00::/129"], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/v6c1.json
./build/sluice /tmp/v6c1.json; echo "exit=$?"
# -> config blacklist: invalid CIDR "fd00::/129" (expected e.g. "10.0.0.0/24", prefix 0-32)   exit=1
#    (message names the v4 example, but the /129 IPv6 prefix is correctly rejected)

# malformed IPv6 CIDR address
echo '{ "whitelist":["fd00:::1/64"], "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001}]}] }' > /tmp/v6c2.json
./build/sluice /tmp/v6c2.json; echo "exit=$?"
# -> config whitelist: invalid CIDR "fd00:::1/64" ...   exit=1

# affinity host that is neither valid IPv4 nor IPv6
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"::1","port":7001}],"affinity":[{"client":"fd00::/32","host":"nope","port":7001}]}] }' > /tmp/v6c3.json
./build/sluice /tmp/v6c3.json; echo "exit=$?"
# -> route[0]: affinity "host" "nope" must be a valid IP address   exit=1
```
A **valid** IPv6 backend + IPv6 affinity should load cleanly:
```bash
echo '{ "dashboard":{"enabled":false}, "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"::1","port":7001}],"affinity":[{"client":"fd00::/32","host":"::1","port":7001}]}] }' > /tmp/v6ok.json
./build/sluice /tmp/v6ok.json; echo "exit=$?"
# -> starts normally (Ctrl+C to stop); exit code on clean shutdown = 0
```

---

## E. Dashboard

Manual check (start any config with the dashboard enabled, open the CONFIG tab):

### E1 — IPv6 backend host accepted
Enter `::1` as a backend HOST (IP) and a valid port; SAVE TO DISK.
**Expect:** no validation error on the host field; the saved JSON has
`"host": "::1"`.

### E2 — IPv6 CIDR accepted in whitelist/blacklist
In a backend's ACCESS panel, enter `fd00:beef::/32` in the whitelist and add a
blacklist row `fd00::9`; SAVE TO DISK.
**Expect:** both validate; the saved JSON contains them. An invalid entry like
`fd00::/129` shows the inline `Invalid IP/CIDR` / `Invalid IP or CIDR` error and
blocks the save.

---

## Cleanup

Remove the ULA test addresses added in Setup:
```bash
sudo ip -6 addr del fd00::9/128       dev lo
sudo ip -6 addr del fd00:beef::5/128  dev lo
sudo ip -6 addr del fd00:cafe::5/128  dev lo
sudo ip -6 addr del fd00:9999::5/128  dev lo
```

---

## Notes / known behavior

- **Dual-stack, single socket.** Each route listens on one `AF_INET6` socket
  with `IPV6_V6ONLY=0`; it serves both families. If the host has no IPv6, the
  gateway falls back to an IPv4-only listener automatically.
- **IPv4-mapped normalization.** IPv4 clients on the dual-stack socket are
  reported as `::ffff:a.b.c.d` by the kernel; the gateway normalizes them to
  plain `a.b.c.d` so logs and every IPv4 access rule behave exactly as before.
- **IPv6 is bracketed in logs** (`[::1]:port`) to disambiguate the port from the
  address colons.
- **Unified 128-bit matching.** Internally all addresses are 16 bytes (IPv4
  stored IPv4-mapped); an IPv4 `/n` CIDR is stored as `/(96+n)`. This is why v4
  and v6 never cross-match (C5) yet share one code path.
- **Canonicalization.** Static whitelist/blacklist entries are canonicalized at
  load, so any valid spelling of an IPv6 address in the config matches the
  client's reported (canonical) form (C1).
- **The CIDR validation error message** names an IPv4 example (`10.0.0.0/24`)
  for both families — cosmetic; the validation itself is family-correct.
- **DNS is still not supported** — backend hosts must be IP literals (IPv4 or
  IPv6), consistent with the rest of the gateway.
