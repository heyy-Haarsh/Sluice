# 4. Health checks & failover

Sluice probes each backend every few seconds. A backend that stops responding is
taken out of rotation automatically — clients never hit a dead server. It rejoins
on its own when it recovers.

### Run
```bash
# Kill backend A and watch the dashboard mark it DOWN (within ~3s)
pkill -f 'demo_backend.py A'

# Traffic now avoids A entirely — still zero failures:
for i in $(seq 1 9); do curl -s http://127.0.0.1:9000/ | grep -o 'Backend [ABC]'; done

# Bring A back:
python3 tests/backends/demo_backend.py A 8001 &
```

### Expect
After the kill: only **B and C** answer, no errors. In the dashboard, backend A
turns red. After restart, A turns green within ~3s and returns to rotation.

← [previous](03-load-balancing.md) · next → [Zero‑downtime hot reload](05-hot-reload.md)
