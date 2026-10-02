# 8. Rate limiting

Per‑IP connection rate limiting protects backends from floods and abusive clients.
Offenders are temporarily blocked, with the block time doubling on repeat offence
(exponential backoff).

We drop the limit to 5/min for the demo and flood from one source IP.

### Run
```bash
# Lower the global per-IP limit to 5/min and reload:
jq '.rate_limit.max_per_minute = 5' config/demo.json > /tmp/d && mv /tmp/d config/demo.json
kill -USR1 $(pgrep -f 'sluice config/demo.json')

# Fire 12 quick requests from one client — watch it get cut off:
for i in $(seq 1 12); do
  curl --interface 127.0.0.30 -s -o /dev/null -w "%{http_code} " http://127.0.0.1:9000/
done; echo
```

### Expect
The first few return **200**, then the client is blocked → **000** (connection
refused) for the rest. Sluice logs `rate limit exceeded by 127.0.0.30 ... blocked
for 15s`. Other client IPs are unaffected.

### Reset before moving on
```bash
cp /tmp/demo.json.bak config/demo.json && kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

← [previous](07-proxy-protocol.md) · next → [Scale & efficiency](09-scale.md)
