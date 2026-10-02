# Sluice — Final Demo

## Setup

Run all of this **once** before the demo. Keep these terminals open throughout.

> All commands run from the project root: `~/Systematic_Chaos/`

### Terminal G — Gateway
```bash
cp final_demo/demo.json final_demo/demo.json.bak
./build/sluice final_demo/demo.json
```

### Terminal B — Backends
```bash
# TCP backends for load balancing (web route, :9000)
python3 tests/backends/demo_backend.py A 8001 &
python3 tests/backends/demo_backend.py B 8002 &
python3 tests/backends/demo_backend.py C 8003 &

# 4th TCP backend — NOT in the initial config; added live in scenario 3
python3 tests/backends/demo_backend.py D 8004 &

# Same self-identifying backend, used for the PROXY protocol demo (echo route, :9010)
python3 tests/backends/demo_backend.py P 8010 &

# UDP backends for telemetry route (:9400)
python3 tests/backends/udp_echo.py 127.0.0.1 7401 A &
python3 tests/backends/udp_echo.py 127.0.0.1 7402 B &
python3 tests/backends/udp_echo.py 127.0.0.1 7403 C &
```

### Browser — Dashboard
Open **http://localhost:8080**

Default password: **`root`**

---

## Scenarios

| # | Scenario | File |
|---|---|---|
| 1 | Gateway + Dashboard | [01-gateway-dashboard.md](01-gateway-dashboard.md) |
| 2 | Load Balancing, Affinity & Failover | [02-lb-failover.md](02-lb-failover.md) |
| 3 | Hot Reload + Access Control | [03-hotreload-access.md](03-hotreload-access.md) |
| 4 | PROXY Protocol | [04-proxy-protocol.md](04-proxy-protocol.md) |
| 5 | UDP Routing + Load Balancing | [05-udp-basics.md](05-udp-basics.md) |
| 6 | UDP Sessions, Failover & Rate Limiting | [06-udp-sessions.md](06-udp-sessions.md) |
| 7 | Scale + Production *(if time)* | [07-scale-production.md](07-scale-production.md) |

---

### Resetting the config (rarely needed)
Scenarios 2 and 3 add routing config live (affinity, backend D, a blacklist rule) and
deliberately **leave it in place** — it doesn't interfere with anything later. Only reset
if you want a clean slate for a re-run:
```bash
cp final_demo/demo.json.bak final_demo/demo.json
kill -USR1 $(pgrep -x sluice)
```
