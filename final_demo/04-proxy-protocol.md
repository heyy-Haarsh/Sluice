# 4. PROXY Protocol

```bash
# Before: backend sees the gateway address
curl --interface 127.0.0.7 -s http://127.0.0.1:9010/ | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+'

# Enable proxy_protocol in the Config tab for the echo route's backend, Save.

# After: backend sees the real client IP
curl --interface 127.0.0.7 -s http://127.0.0.1:9010/ | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+'
```

---

← [Hot reload](03-hotreload-access.md) · next → [UDP basics](05-udp-basics.md)
