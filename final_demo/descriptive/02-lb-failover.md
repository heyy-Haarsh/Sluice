# 2. Load Balancing & Failover

Incoming connections are distributed across healthy backends using round-robin. Sluice
probes each backend every 3 seconds; a backend that stops responding is removed from
rotation automatically and re-added when it recovers — with no configuration change needed.

### Round-robin in action
```bash
for i in $(seq 1 9); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
```
**Expect:** responses cycle `A → B → C → A → B → C …`

Watch the dashboard — all three backends' byte counters climb evenly.

### Session affinity — pin a client to one backend
Round-robin is the default, but a specific client (or CIDR range) can be pinned to a
fixed backend — useful for session-sticky workloads. Sluice checks affinity **before**
load balancing.

In the dashboard **Config tab**, open the `web` route, click **Affinity [None]** to
expand the panel, then **+ Add Affinity Rule** and set CLIENT=`127.0.0.7`,
BACKEND HOST=`127.0.0.1`, BACKEND PORT=`8002` (backend B). **Save Configuration**.
```bash
# Always lands on B, regardless of the round-robin cursor
for i in 1 2 3 4; do curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
```
**Expect:** all four say `Backend B`. The gateway log shows
`client 127.0.0.7 pinned by affinity rule -> 127.0.0.1:8002`.

### Failover — kill backend B
```bash
pkill -f 'demo_backend.py B 8002'
```
Within ~3 seconds the dashboard marks B as down. Then:
```bash
for i in $(seq 1 6); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [AC]'; done

# The pinned client (127.0.0.7) falls back gracefully too — no hard failure
curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'Backend [AC]'
```
**Expect:** only A and C respond — zero failures, B is bypassed. The pinned client now
lands on A or C as well: when a pin's target becomes unhealthy, Sluice falls back to
normal round-robin instead of hard-failing that client.

### Recovery
```bash
python3 tests/backends/demo_backend.py B 8002 &
```
**Expect:** B turns green on the dashboard within one heartbeat interval, rejoins
rotation, and the pinned client (127.0.0.7) goes back to landing on it every time.

---

← [Gateway](01-gateway-dashboard.md) · next → [Hot reload + Access control](03-hotreload-access.md)
