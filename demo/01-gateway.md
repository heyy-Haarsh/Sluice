# 1. Gateway works

Clients connect to Sluice; Sluice relays the traffic to a backend server. It works
at the TCP layer, so it proxies any protocol — here, plain HTTP — without
understanding it.

### Run
```bash
curl -s http://127.0.0.1:9000/
```
Or open **http://localhost:9000** in the browser.

### Expect
An HTML page: **"Backend A"** (or B/C). The client talked to Sluice on `:9000`;
Sluice fetched the response from a backend on `:8001‑8003` and piped it back.

← [index](00-index.md) · next → [Admin dashboard](02-dashboard.md)
