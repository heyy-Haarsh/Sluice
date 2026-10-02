# 2. Load Balancing, Affinity & Failover

### Round-robin
```bash
for i in $(seq 1 9); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
```

### Session affinity
Config tab → `web` route → **Affinity [None]** → **+ Add Affinity Rule**
CLIENT `127.0.0.7`, HOST `127.0.0.1`, PORT `8002` → **Save Configuration**.
```bash
for i in 1 2 3 4; do curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
```

### Failover
```bash
pkill -f 'demo_backend.py B 8002'
```
```bash
for i in $(seq 1 6); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done
curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'
```

### Recovery
```bash
python3 tests/backends/demo_backend.py B 8002 &
```

---

← [Gateway](01-gateway-dashboard.md) · next → [Hot reload + Access control](03-hotreload-access.md)
