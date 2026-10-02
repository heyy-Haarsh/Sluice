# 1. Gateway & Dashboard

Sluice is an L4 TCP/UDP reverse proxy and load balancer. It routes traffic from clients
to a pool of backends without touching application-layer data. A built-in web dashboard
provides live monitoring and full configuration control without restarting the process.

### Public health endpoint (no login required)
```bash
curl -s http://127.0.0.1:8080/api/health | jq
```

### Dashboard tour
Open **http://localhost:8080** and log in.

Point out:
- **Status tab** — every route, backend health, live connection and byte counters
- **Log tab** — live tail of gateway events
- **Config tab** — full editor; saving here triggers a live reload (covered in scenario 3)

---

← [index](00-index.md) · next → [Load balancing + Failover](02-lb-failover.md)
