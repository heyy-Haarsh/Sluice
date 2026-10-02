# Sluice — Live Demo

Sluice is a Layer‑4 (TCP) reverse proxy / load balancer. This is the demo script.
Do the **common setup below once**, then walk the scenarios in order.

---

## Common setup (once)

**1. Install tools & build**
```bash
sudo apt install -y build-essential cmake jq curl
cmake -S . -B build && cmake --build build -j
```

**2. Snapshot the demo config** (so you can reset any time)
```bash
cp config/demo.json /tmp/demo.json.bak
```

**3. Terminal 1 — start three backends** (they stay running for the whole demo)
```bash
python3 tests/backends/demo_backend.py A 8001 &
python3 tests/backends/demo_backend.py B 8002 &
python3 tests/backends/demo_backend.py C 8003 &
```

**4. Terminal 2 — start Sluice** (leave it in the foreground to watch its logs)
```bash
./build/sluice config/demo.json
```

**5. Open in a browser:** the gateway `http://localhost:9000` and the dashboard `http://localhost:8080`.

Use **Terminal 3** for the `curl` commands in each scenario.

---

## Two commands you'll reuse

**Reload after a config edit** (zero downtime):
```bash
kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

**Reset the config** back to the starting point:
```bash
cp /tmp/demo.json.bak config/demo.json && kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

---

## Scenarios (in order)

1. [Gateway works](01-gateway.md)
2. [Admin dashboard](02-dashboard.md)
3. [Load balancing](03-load-balancing.md)
4. [Health checks & failover](04-failover.md)
5. [Zero‑downtime hot reload](05-hot-reload.md)
6. [IP access control](06-access-control.md)
7. [PROXY protocol](07-proxy-protocol.md)
8. [Rate limiting](08-rate-limiting.md)
9. [Scale & efficiency](09-scale.md)
10. [Production deployment](10-production.md)

---

## Teardown
```bash
# Ctrl+C the gateway (Terminal 2), then:
pkill -f demo_backend
```
