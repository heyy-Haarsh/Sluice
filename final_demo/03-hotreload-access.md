# 3. Security, DDoS Protection & Graceful Reload

### Part A — CIDR Access Control
Config tab → `blacklist` (top-level JSON) → change to:
```json
  "blacklist": [
    "127.0.0.8/31"
  ],
```
**Save Configuration.**

```bash
curl --interface 127.0.0.8 -s -o /dev/null -w ".8 (blocked) -> %{http_code}\n" http://127.0.0.1:9000/
curl --interface 127.0.0.9 -s -o /dev/null -w ".9 (blocked) -> %{http_code}\n" http://127.0.0.1:9000/
curl --interface 127.0.0.10 -s -o /dev/null -w ".10 (allowed) -> %{http_code}\n" http://127.0.0.1:9000/
```

### Part B — Anti-DDoS Penalty Box
```bash
for i in $(seq 1 12); do
  curl --interface 127.0.0.30 -s -o /dev/null -w "%{http_code} " http://127.0.0.1:9000/
done; echo
```

### Part C — Graceful Draining (Zero Downtime)
```bash
curl -s http://127.0.0.1:9020/slow 
```
Config tab → delete the entire `drain_demo` route → **Save Configuration**.

Dashboard Status tab → watch **DRAINING ROUTES** section. Wait 10s for download to finish.

---

← [Load balancing](02-lb-failover.md) · next → [PROXY protocol](04-proxy-protocol.md)
