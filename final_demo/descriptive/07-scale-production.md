# 7. Scale & Production

A single epoll event loop handles all connections without allocating a thread per client,
keeping memory flat regardless of connection count. For multi-core deployments, N worker
processes share the same port via `SO_REUSEPORT`; the kernel distributes connections across
them. Sluice ships as a Debian package with a hardened systemd service for production use.

### Benchmark results
Benchmarked with a custom multi-process load generator:

| Concurrent connections | Success rate | Memory (gateway) |
|---|---|---|
| 100 | 100% | ~5 MB |
| 500 | 100% | ~5 MB |
| 1,000 | 100% | ~5 MB |
| 5,000 | 100% | ~6 MB |
| 10,000 | **100%** | **~6 MB** |

RAM is flat across all tiers — proof of the epoll design, not thread-per-connection.

![Resources over time](../benchmark/graphs/resources_over_time.png)
![Scalability — connections](../benchmark/graphs/scalability_success.png)

### Multi-process mode
```bash
# Stop the current gateway (Ctrl+C in Terminal G), then:
./build/sluice config/demo_mp.json

# Confirm: 1 master + N workers, all on the same port
pgrep -af sluice

# Fire a burst of concurrent requests — SO_REUSEPORT spreads them across all 4 workers
for i in $(seq 1 20); do curl -s -o /dev/null -w "%{http_code} " http://127.0.0.1:9000/ & done; wait; echo
```
**Expect:** twenty `200`s come back together — the whole worker pool serving traffic
concurrently on one port, each worker with its own independent event loop.

### Production deployment
```bash
# Installed as a systemd service (run on the host, not WSL):
systemctl status sluice
ps -o user= -p "$(systemctl show -p MainPID --value sluice)"   # runs as 'sluice', not root
systemctl reload sluice   # zero-downtime config reload
```

---

← [UDP sessions](06-udp-sessions.md) · [back to index](00-index.md)
