# 6. UDP Sessions, Stickiness, Failover, Rate Limiting & Datagram Guard

### Stickiness
```bash
for i in 1 2 3 4; do printf "update $i\n" | nc -u -p 44001 -w1 127.0.0.1 9400; done
```

### Idle timeout
Wait ~10s. Dashboard active session count → `0`.

### Passive failover
```bash
printf "alive\n" | nc -u -p 44002 -w1 127.0.0.1 9400
pkill -f 'udp_echo.py 127.0.0.1 7401 A'
printf "retry\n"  | nc -u -p 44002 -w1 127.0.0.1 9400
printf "retry2\n" | nc -u -p 44002 -w1 127.0.0.1 9400
```

Restart A:
```bash
python3 tests/backends/udp_echo.py 127.0.0.1 7401 A &
```

### Rate limiting
```bash
for i in $(seq 1 15); do printf "burst $i\n" | nc -u -p 44003 -w1 127.0.0.1 9400; done | wc -l
```

### Datagram counters
```bash
for i in 1 2 3; do printf "metric $i\n" | nc -u -p 44004 -w1 127.0.0.1 9400; done
```

### Max datagram size (`max_datagram_bytes: 512`)
```bash
# Normal payload — passes through
printf "temperature=22.5\n" | nc -u -w1 127.0.0.1 9400

# Oversized — dropped before any backend sees it
python3 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.sendto(b'X'*600, ('127.0.0.1',9400))"
```

---

← [UDP basics](05-udp-basics.md) · next → [UDP PROXY Protocol v2](07-udp-proxy-protocol.md)
