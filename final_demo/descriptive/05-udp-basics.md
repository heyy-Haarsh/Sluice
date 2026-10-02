# 5. UDP Routing & Load Balancing

Live telemetry — sensor readings, game state, position updates — arrives many times per
second. If one packet is lost, nobody waits: a fresher one is already on the way. This
traffic doesn't need TCP's overhead of handshakes and retransmissions. Sluice routes UDP
datagrams to backends with the same load-balancing and access control as TCP.

The `telemetry` route listens on `:9400` (UDP) and has three backends (A, B, C).

### Basic relay — send a datagram, get an echo back
```bash
printf "temperature=22.5\n" | nc -u -w1 127.0.0.1 9400
```
**Expect:** `[A] temperature=22.5` (or B or C) echoed back. Watch the backend terminal — it prints the datagram it received.

### Load balancing — different clients hit different backends
```bash
for port in 5001 5002 5003 5004 5005 5006; do
  printf "ping\n" | nc -u -p $port -w1 127.0.0.1 9400
done
```
**Expect:** responses tagged `[A]`, `[B]`, `[C]`, `[A]`, `[B]`, `[C]` — round-robin across the three backends.

Each distinct source port is a separate client to the gateway. Watch the dashboard: the active session count and byte counters update in real time.

---

← [PROXY protocol](04-proxy-protocol.md) · next → [UDP sessions](06-udp-sessions.md)
