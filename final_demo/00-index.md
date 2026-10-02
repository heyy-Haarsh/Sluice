# Sluice — Final Demo

## Setup

Run once, keep terminals open. Commands run from the project root.

### Terminal G — Gateway
```bash
cp final_demo/demo.json final_demo/demo.json.bak
./build/sluice final_demo/demo.json.bak
```

### Terminal B — Backends
```bash
python3 tests/backends/demo_backend.py A 8001 &
python3 tests/backends/demo_backend.py B 8002 &
python3 tests/backends/demo_backend.py C 8003 &
python3 tests/backends/demo_backend.py D 8004 &
python3 tests/backends/demo_backend.py E 8020 &
python3 tests/backends/demo_backend.py P 8010 &
python3 tests/backends/udp_echo.py 127.0.0.1 7401 A &
python3 tests/backends/udp_echo.py 127.0.0.1 7402 B &
python3 tests/backends/udp_echo.py 127.0.0.1 7403 C &
python3 tests/backends/udp_proxy_v2_backend.py 127.0.0.1 7410 P &
```

### Browser — Dashboard
**http://localhost:8080** — password `root`

---

## Scenarios

| # | Scenario | File |
|---|---|---|
| 1 | Gateway + Dashboard | [01-gateway-dashboard.md](01-gateway-dashboard.md) |
| 2 | Load Balancing, Affinity & Failover | [02-lb-failover.md](02-lb-failover.md) |
| 3 | Security, DDoS Protection & Graceful Reload | [03-hotreload-access.md](03-hotreload-access.md) |
| 4 | PROXY Protocol | [04-proxy-protocol.md](04-proxy-protocol.md) |
| 5 | UDP Routing + Load Balancing | [05-udp-basics.md](05-udp-basics.md) |
| 6 | UDP Sessions, Failover, Rate Limiting & Datagram Guard | [06-udp-sessions.md](06-udp-sessions.md) |
| 7 | UDP PROXY Protocol | [07-udp-proxy-protocol.md](07-udp-proxy-protocol.md) |
| 8 | Scale + Production *(if time)* | [08-scale-production.md](08-scale-production.md) |

---

### Reset (only if needed)
```bash
cp final_demo/demo.json.bak final_demo/demo.json
kill -USR1 $(pgrep -x sluice)
```

### Kill background processes from current terminal
```
kill $(jobs -p)
```
