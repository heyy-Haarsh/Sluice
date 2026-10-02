# 8. Session affinity (sticky routing)

Session affinity pins specific client IPs — or whole CIDR ranges — to a fixed
backend within a route. Sluice checks affinity rules **before** load‑balancing,
so the pinned client always lands on its assigned backend as long as that backend
is healthy.

When the pinned backend is unhealthy, disabled, or denies the client by access
rule, Sluice **falls back to normal round‑robin** — there is no hard failure.

---

### Run

**Step 1 — add an affinity rule via the dashboard**

Open the dashboard CONFIG tab (`http://localhost:8080`, login with default
password `root`) and find the `main` route. Click **+ Add Affinity Rule**,
fill in:

| Field | Value |
|---|---|
| CLIENT (IP or CIDR) | `127.0.0.7` |
| BACKEND HOST | `127.0.0.1` |
| BACKEND PORT | `8001` |

Click **SAVE CONFIGURATION**. Sluice reloads automatically.

Or do it entirely from the terminal:
```bash
jq '.routes[0].affinity = [{"client":"127.0.0.7","host":"127.0.0.1","port":8001}]' \
   config/demo.json > /tmp/d && mv /tmp/d config/demo.json
kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

**Step 2 — verify the pinned client always hits backend A**
```bash
# Run four times — they should ALL land on backend A (:8001)
for i in 1 2 3 4; do
  curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'backend [A-C]'
done
```

**Step 3 — verify an unmatched client still round‑robins**
```bash
# A different source IP — NOT in the affinity rule
for i in 1 2 3 4; do
  curl --interface 127.0.0.20 -s http://127.0.0.1:9000/ | grep -o 'backend [A-C]'
done
```

**Step 4 — kill the pinned backend; the client gracefully falls back**
```bash
pkill -f 'demo_backend.py A 8001'
sleep 6       # wait one heartbeat cycle so Sluice marks it unhealthy

# Pinned client now falls to round‑robin (B or C), no error
curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'backend [A-C]'
```

---

### Expect

- **Step 2:** all four responses say `backend A`. Sluice logs:
  `route "main": client 127.0.0.7 pinned by affinity rule -> 127.0.0.1:8001`
- **Step 3:** responses cycle `A → B → C → A …` (round‑robin), no pin log line.
- **Step 4:** `backend 127.0.0.1:8001 is now unhealthy` appears in the logs,
  then the pinned client falls to `B` or `C` with `rc=0` — no dropped connection.

---

### Reset before moving on
```bash
# Restart backend A
python3 tests/backends/demo_backend.py A 8001 &
# Restore clean config
cp /tmp/demo.json.bak config/demo.json && kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

← [previous](07-proxy-protocol.md) · next → [Rate limiting](09-rate-limiting.md)
