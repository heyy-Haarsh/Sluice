# 6. UDP Sessions, Stickiness, Failover & Rate Limiting

UDP has no handshake or connection state. Sluice synthesizes a session keyed on the
client's source address, so datagrams from the same client always reach the same backend
for the lifetime of that session. Sessions expire automatically after 10 seconds of
silence, freeing the backend socket and reclaiming resources.

### Stickiness — same client always hits the same backend
```bash
# All four datagrams come from source port 44001 — one session, one backend
for i in 1 2 3 4; do
  printf "update $i\n" | nc -u -p 44001 -w1 127.0.0.1 9400
done
```
**Expect:** all four replies carry the same tag (e.g. all `[B]`). The gateway log shows one session-creation line and no new sessions for the repeat sends.

On the dashboard, the backend's active session count is `1` while the session is alive.

### Idle timeout — sessions expire automatically
Stop sending and wait ~10 seconds. Watch the dashboard: the active session count drops back to `0`.

No explicit close is needed — the gateway reclaims the session and its backend socket automatically.

### Passive failover — backend goes down mid-session
UDP backends aren't actively probed (a TCP-style connect test means nothing for a
datagram service) — Sluice detects a dead one passively, from the kernel's own
"port unreachable" signal, the moment traffic actually hits it.
```bash
# Start a pinned session on source port 44002
printf "alive\n" | nc -u -p 44002 -w1 127.0.0.1 9400

# Kill whichever backend that session landed on, e.g. A:
pkill -f 'udp_echo.py 127.0.0.1 7401 A'

# First send after the kill ends the dying session (its own datagram may get no
# reply); the second establishes a fresh session on a healthy backend
printf "retry\n"  | nc -u -p 44002 -w1 127.0.0.1 9400
printf "retry2\n" | nc -u -p 44002 -w1 127.0.0.1 9400
```
**Expect:** the gateway log shows `udp backend … refused … — ending session`, and on the
**dashboard** that backend's health cell flips to `—`/DOWN immediately — no probe interval
to wait out. `retry2`'s reply comes from a different backend tag. (It rejoins rotation on
its own ~10s after being restarted — no action needed.)

```bash
# Restart backend A before moving on
python3 tests/backends/udp_echo.py 127.0.0.1 7401 A &
```

### Per-session rate limiting — one client can't flood a backend
Each UDP session is capped independently — 5 datagrams/sec on this route
(`udp_datagram_rate_limit`). A client sending faster than that is silently
throttled; the backend and every other session are unaffected.
```bash
for i in $(seq 1 15); do printf "burst $i\n" | nc -u -p 44003 -w1 127.0.0.1 9400; done | wc -l
```
**Expect:** watch **Terminal G** while this runs — `... rate limit (5/s) exceeded —
throttling` appears almost immediately (logged once per session, not per datagram). The
final count is well under `15`: the excess datagrams never reached the backend.

---

← [UDP basics](05-udp-basics.md) · next → [Scale + Production](07-scale-production.md)
