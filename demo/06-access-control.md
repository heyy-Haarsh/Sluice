# 6. IP access control

Sluice can allow/deny clients by IP before they ever reach a backend. Supports
blacklists, whitelists, CIDR ranges, and time‑limited bans — global or per‑backend.

We simulate a specific client using a loopback source IP (`--interface`).

### Run
```bash
# Baseline: this client works (HTTP 200)
curl --interface 127.0.0.9 -s -o /dev/null -w "127.0.0.9 -> %{http_code}\n" http://127.0.0.1:9000/

# Blacklist it, then reload:
jq '.access.blacklist += [{"ip":"127.0.0.9"}]' config/demo.json > /tmp/d && mv /tmp/d config/demo.json
kill -USR1 $(pgrep -f 'sluice config/demo.json')

# Blocked at the gateway now — but everyone else is unaffected:
curl --interface 127.0.0.9 -s -o /dev/null -w "127.0.0.9    -> %{http_code}\n" http://127.0.0.1:9000/
curl                        -s -o /dev/null -w "other client -> %{http_code}\n" http://127.0.0.1:9000/
```

### Expect
`127.0.0.9` goes **200 → 000** (connection refused), Sluice logs
`blocked client 127.0.0.9 ...`, and other clients still get **200**.

**Also supports** (mention, no need to run): CIDR ranges like `"10.0.0.0/8"`,
timed bans like `{"ip":"1.2.3.4","until":"2026-12-31T23:59:59"}`, whitelist mode
(only listed IPs allowed), and the same rules scoped to a single backend.

### Reset before moving on
```bash
cp /tmp/demo.json.bak config/demo.json && kill -USR1 $(pgrep -f 'sluice config/demo.json')
```

← [previous](05-hot-reload.md) · next → [PROXY protocol](07-proxy-protocol.md)
