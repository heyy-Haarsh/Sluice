# 3. Hot Reload & Access Control

Config changes — adding or removing backends, updating access rules — apply instantly
via the dashboard with no process restart and without dropping any live connections.
IP access rules (exact IPs, CIDR ranges, timed bans) block clients at the gateway
before they ever reach a backend.

### Part A — Block a client via the dashboard

Open the **Config tab** in the dashboard. Find the `access` → `blacklist` field and add:
```json
[{ "ip": "127.0.0.9" }]
```
Click **Save Configuration**. Sluice reloads in place.

Verify in Terminal C:
```bash
curl --interface 127.0.0.9 -s -o /dev/null -w "127.0.0.9   -> %{http_code}\n" http://127.0.0.1:9000/
curl                        -s -o /dev/null -w "other client -> %{http_code}\n" http://127.0.0.1:9000/
```
**Expect:** `127.0.0.9 → 000` (blocked), `other client → 200` (unaffected).

The gateway log shows: `blocked client 127.0.0.9 …`

### Part B — Add a backend live

Backend D is already running (started with the rest in setup) — it just isn't in the
config yet, so it's getting no traffic.

In the dashboard **Config tab**, add to the `web` route's `backends` array:
```json
{ "host": "127.0.0.1", "port": 8004 }
```
Save. Then:
```bash
for i in $(seq 1 8); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABCD]'; done
```
**Expect:** D joins the round-robin. The gateway process never stopped.

D and the blacklist rule above stay live for the rest of the demo — no need to revert.

---

← [Load balancing](02-lb-failover.md) · next → [PROXY protocol](04-proxy-protocol.md)
