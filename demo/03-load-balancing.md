# 3. Load balancing

New connections are spread across all healthy backends using round‑robin, so no
single server is overloaded.

### Run
```bash
for i in $(seq 1 9); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
```

### Expect
The responses cycle **A → B → C → A → B → C …**. In the dashboard, the three
backends' connection/byte counters all climb roughly evenly.

← [previous](02-dashboard.md) · next → [Health checks & failover](04-failover.md)
