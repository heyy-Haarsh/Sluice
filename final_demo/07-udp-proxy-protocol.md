# 7. UDP PROXY Protocol v2 (Binary)

```bash
# No header — reply shows the gateway address
printf 'hello\n' | nc -u -w1 127.0.0.1 7410

# Through gateway — reply reveals real client IP
printf 'hello\n' | nc -u -w1 127.0.0.1 9401

# Distinct source address — identity revealed end-to-end
printf 'hello\n' | nc -u -w1 -s 127.0.0.2 127.0.0.1 9401
```

---

← [UDP Sessions](06-udp-sessions.md) · next → [Scale & Production](08-scale-production.md)
