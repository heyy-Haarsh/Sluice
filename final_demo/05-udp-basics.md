# 5. UDP Routing & Load Balancing

`telemetry` route, `:9400` UDP, backends A/B/C.

### Basic relay
```bash
printf "temperature=22.5\n" | nc -u -w1 127.0.0.1 9400
```

### Load balancing
```bash
for port in 5001 5002 5003 5004 5005 5006; do printf "ping\n" | nc -u -p $port -w1 127.0.0.1 9400; done
```

---

← [PROXY protocol](04-proxy-protocol.md) · next → [UDP sessions](06-udp-sessions.md)
