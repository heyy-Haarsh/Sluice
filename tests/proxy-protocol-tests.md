# Sluice — PROXY Protocol (Client IP Preservation) Test Cases

Tests for **client IP preservation** via the **PROXY protocol v1**. When a
backend is configured `"proxy_protocol": true`, the gateway prepends a single
CRLF-terminated line to the backend stream, before any client bytes:

```
PROXY TCP4 <client_ip> <gateway_ip> <client_port> <gateway_port>\r\n
PROXY TCP6 <client_ip> <gateway_ip> <client_port> <gateway_port>\r\n
```

So the backend's TCP peer is still the **gateway** (the client never connects
to the backend), but the backend can now learn the **real client IP** from the
header — IPv4 clients yield `TCP4`, IPv6 clients yield `TCP6`. Default is
**off** per backend; a proxy-unaware backend must never be sent the header.

Run on WSL/Ubuntu.

---

## Setup

### Build
```bash
cd /mnt/c/Users/rvdan/Documents/Claude-Workspace/traffic-gateway/build
cmake ..     # picks up the config.cpp / gateway.cpp changes
make
```

### Test methodology
- **No background processes.** Each server runs in the foreground in its own
  terminal, labelled **T1, T2, T3, ...** — open a new one for each.
- **Stop a foreground server with Ctrl+C** when a test says so.
- Python backends are launched with **`python3`**.

### Simulating different client IPs
All of `127.0.0.0/8` is loopback, so an IPv4 client can connect *from* a chosen
source IP with `nc -s`:
```bash
nc -s 127.0.0.5 127.0.0.1 9000       # gateway sees client IP 127.0.0.5
```
For the IPv6 case (C1) `::1` is enough; no extra addresses needed.

### The proxy-aware backend
`tests/backends/proxy_protocol_backend.py <host> <port>` accepts a connection,
prints its TCP peer (the gateway) and — if a PROXY header is present — the
parsed real client address, then echoes the payload. Binds IPv4 or IPv6 based
on the host (a `:` means IPv6).

### Config files used
- `config/proxy_protocol_config.json` — one route `:9000 → 127.0.0.1:7001`,
  `proxy_protocol: true`, heartbeat + dashboard off.
- `config/proxy_protocol_mixed.json` — one route `:9000` with two backends:
  `7001` (`proxy_protocol: true`) and `7002` (`proxy_protocol: false`).

---

## A. Basic client-IP preservation (IPv4 → TCP4)

### A1 — backend learns the real client IP; TCP peer is still the gateway
**T1** — start the proxy-aware backend:
```bash
python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001
```
**T2** — start the gateway:
```bash
./build/sluice config/proxy_protocol_config.json
```
**T3** — connect from a chosen source IP and send a line:
```bash
echo hello | nc -N -s 127.0.0.5 127.0.0.1 9000
```
**Tests:** the header is emitted and correctly names the real client.
**Expect (T1 output):**
```
[conn] TCP peer (gateway) = 127.0.0.1:<ephemeral-port>
[conn] PROXY header       = PROXY TCP4 127.0.0.5 127.0.0.1 <cport> 9000
[conn] real client       -> 127.0.0.5:<cport> (TCP4)
[conn] client dialed gw  -> 127.0.0.1:9000
```
- **TCP peer is the gateway** (`127.0.0.1`) — NOT `127.0.0.5`.
- The **real client** line shows `127.0.0.5`.
- **T3** receives `hello` echoed back (payload relayed intact after the header).

---

## B. Header is opt-in per backend

### B1 — default off: no header sent (baseline)
Make a copy with the flag off → `/tmp/pp_off.json`:
```bash
sed 's/"proxy_protocol": true/"proxy_protocol": false/' \
  config/proxy_protocol_config.json > /tmp/pp_off.json
```
**T1** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001`
**T2** `./build/sluice /tmp/pp_off.json`
**T3** `echo hello | nc -N -s 127.0.0.5 127.0.0.1 9000`
**Tests:** with the flag off, the stream is byte-for-byte the old behavior.
**Expect (T1):** `[conn] TCP peer = 127.0.0.1:<port>  (NO proxy header received)`,
and T3 still gets `hello`. No PROXY line anywhere.

### B2 — per-backend granularity within one route
**T1** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001`
**T2** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7002`   *(new terminal)*
**T3** `./build/sluice config/proxy_protocol_mixed.json`   *(new terminal)*
**T4** — send several connections so round-robin hits both backends:
```bash
for i in 1 2 3 4; do echo "hi-$i" | nc -N -s 127.0.0.5 127.0.0.1 9000; done
```
**Tests:** the header is decided per backend, not per route.
**Expect:** the **7001** terminal prints `PROXY header ...` / `real client` for
its connections; the **7002** terminal prints `(NO proxy header received)`. Both
echo correctly.

---

## C. IPv6 client → TCP6 header

### C1 — an IPv6 client yields a TCP6 header
**T1** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001`
**T2** `./build/sluice config/proxy_protocol_config.json`
**T3**:
```bash
echo hello | nc -N -6 ::1 9000
```
**Tests:** the header family follows the client family (the gateway listens
dual-stack; a native IPv6 client is reported as IPv6, not IPv4-mapped).
**Expect (T1):**
```
[conn] PROXY header       = PROXY TCP6 ::1 ::1 <cport> 9000
[conn] real client       -> ::1:<cport> (TCP6)
```
T3 gets `hello` echoed. (The backend here is IPv4 — that's fine; the header
describes the *client*↔*gateway* leg, independent of the backend's family.)

---

## D. Correctness details

### D1 — header precedes client data even if the client sends immediately
**T1** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001`
**T2** `./build/sluice config/proxy_protocol_config.json`
**T3**:
```bash
printf 'PAYLOAD-FIRST-LINE\n' | nc -N -s 127.0.0.7 127.0.0.1 9000
```
**Tests:** the gateway writes the PROXY header as the very first bytes on the
backend socket, ahead of any relayed client data.
**Expect (T1):** the `PROXY header` line parses cleanly (src `127.0.0.7`), and
the echoed payload is exactly `PAYLOAD-FIRST-LINE` — the header is not spliced
into or prepended onto the application data.

### D2 — different source IPs produce different headers
With D1's servers up, in **T3**:
```bash
echo a | nc -N -s 127.0.0.5 127.0.0.1 9000
echo b | nc -N -s 127.0.0.7 127.0.0.1 9000
```
**Expect (T1):** two connections, one header with `127.0.0.5`, the next with
`127.0.0.7`.

---

## E. Why it must be opt-in (the caveat, demonstrated)

### E1 — proxy-unaware backend + flag on = header leaks into the app stream
Point a `proxy_protocol: true` backend at a plain echo backend that knows
nothing about the PROXY protocol:
**T1** `python3 tests/backends/echo_any.py 127.0.0.1 7001`   *(plain echo)*
**T2** `./build/sluice config/proxy_protocol_config.json`
**T3**:
```bash
echo hi | nc -N -s 127.0.0.5 127.0.0.1 9000
```
**Tests:** confirms the header is in-band application bytes to a backend that
doesn't parse it — which is exactly why the flag defaults off and is per-backend.
**Expect (T3):** the client receives back **the PROXY header line followed by
`hi`** (the plain backend echoed everything, header included):
```
PROXY TCP4 127.0.0.5 127.0.0.1 <cport> 9000
hi
```
This is the failure mode you must avoid by only enabling `proxy_protocol` on
backends that actually understand it. Not a gateway bug — a config contract.

---

## F. Hot reload

### F1 — toggling proxy_protocol affects only new connections
**T1** `python3 tests/backends/proxy_protocol_backend.py 127.0.0.1 7001`
**T2** start with the flag **off**:
```bash
sed 's/"proxy_protocol": true/"proxy_protocol": false/' \
  config/proxy_protocol_config.json > /tmp/pp_toggle.json
./build/sluice /tmp/pp_toggle.json
```
**T3**:
```bash
echo before | nc -N -s 127.0.0.5 127.0.0.1 9000        # T1: NO proxy header
sed -i 's/"proxy_protocol": false/"proxy_protocol": true/' /tmp/pp_toggle.json
kill -USR1 $(pgrep -f 'sluice /tmp/pp_toggle.json')
sleep 1
echo after | nc -N -s 127.0.0.5 127.0.0.1 9000         # T1: header present
```
**Tests:** the per-backend `proxy_protocol` field is refreshed on reload (same
persist-refresh path as whitelist/blacklist/rate_limit).
**Expect (T1):** the `before` connection shows `(NO proxy header received)`; the
`after` connection shows a full `PROXY TCP4 ... 127.0.0.5 ...` block.

---

## G. Config validation
```bash
echo '{ "routes":[{"name":"a","listen_port":9000,"backends":[{"host":"127.0.0.1","port":7001,"proxy_protocol":"yes"}]}] }' > /tmp/pp_bad.json
./build/sluice /tmp/pp_bad.json; echo "exit=$?"
# -> route[0]: backend "proxy_protocol" must be a boolean   exit=1
```

---

## H. Dashboard

Manual check (start a config with the dashboard enabled, open the CONFIG tab):

### H1 — PROXY checkbox round-trips
Toggle a backend's **PROXY** checkbox on, SAVE TO DISK.
**Expect:** `curl -s http://localhost:8080/api/config` shows
`"proxy_protocol": true` on that backend. Reopening the CONFIG tab shows the box
still checked. Unchecking + saving drops the key (default false is omitted,
like `disabled`).

---

## Notes / known behavior

- **v1 (text), not v2 (binary).** We emit the human-readable v1 header, capped
  at 107 bytes; an IPv6 `TCP6` line still fits. v2 is a future drop-in swap.
- **Family follows the client.** `TCP4` for IPv4 (incl. IPv4-mapped, normalized
  to dotted-quad), `TCP6` for native IPv6. If an endpoint address can't be
  determined, the gateway emits the spec's `PROXY UNKNOWN` line rather than a
  malformed one.
- **TCP peer stays the gateway.** The client only ever talks to the gateway;
  the backend's TCP peer is the gateway too. Only the *header* carries the real
  client identity.
- **Opt-in per backend, default off.** Sending the header to a backend that
  doesn't parse it corrupts its application stream (E1). Enable only on backends
  configured to expect it (e.g. an nginx `proxy_protocol` listener).
- **Resolved once, at accept time**, like idle timeout: a reload changing
  `proxy_protocol` only affects connections accepted after the reload (F1).
