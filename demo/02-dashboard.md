# 2. Admin dashboard

A built‑in web dashboard for live monitoring and control. It's bound to localhost
only, has a public health endpoint for external monitors, and is password‑protected
for everything else.

### Run
Open **http://localhost:8080**.

1. The landing view is the **public health page** — status, uptime, backend counts,
   total traffic. No login needed (safe to expose to a load balancer / uptime monitor).
   ```bash
   curl -s http://127.0.0.1:8080/api/health | jq
   ```
2. Note the **"default password" warning**, then **log in with password `root`**.
3. Now you see the full admin view: every route and backend, health, live connection
   counts and bytes, the live log tail, and the config editor.

### Expect
Public metrics without auth; full live state after login. Keep this tab open — the
next scenarios are visible here in real time.

← [previous](01-gateway.md) · next → [Load balancing](03-load-balancing.md)
