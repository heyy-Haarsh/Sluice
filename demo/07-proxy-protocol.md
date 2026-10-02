# 7. PROXY protocol

Because Sluice opens its own connection to the backend, the backend normally sees
**Sluice's** IP, not the client's — which breaks backend‑side access logs, rate
limits and geo‑rules. PROXY protocol passes the real client IP through to the
backend. Opt‑in per backend.

We use a loopback source IP (`--interface 127.0.0.7`) so the difference is visible.

### Run
```bash
# Before: the backend sees the gateway
curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+'

# Turn on proxy_protocol for every backend, reload:
jq '.routes[0].backends |= map(. + {"proxy_protocol": true})' config/demo.json > /tmp/d && mv /tmp/d config/demo.json
kill -USR1 $(pgrep -f 'sluice config/demo.json')

# After: the backend now sees the REAL client
curl --interface 127.0.0.7 -s http://127.0.0.1:9000/ | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+'
```

### Expect
Before: backend sees **127.0.0.1** (the gateway). After: backend sees **127.0.0.7**
(the real client), delivered "via PROXY protocol header".

### Reset before moving on
```bash
cp /tmp/demo.json.bak config/demo.json && kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

← [previous](06-access-control.md) · next → [Rate limiting](08-rate-limiting.md)
