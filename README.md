# Sluice

A Layer‑4 (TCP & UDP) reverse proxy and load balancer, written from scratch in C++17 on `epoll`.

> **Sluice** (n.): a gate that controls a stream of water — here, TCP streams.

Sluice sits in front of a pool of backend servers: it load‑balances connections,
health‑checks backends, enforces access rules, and is operated live from a web
dashboard. Because it works at Layer 4, it proxies **any** TCP protocol (HTTP, gRPC,
databases, SSH, …) without understanding it. A single event‑loop thread handles
thousands of connections in a few MB of RAM.

---

## Features

- **TCP & UDP** — proxy either transport; one route is one protocol (`tcp`/`udp`)
- **Load balancing** — round‑robin across healthy backends
- **Health checks & failover** — TCP probes; failed backends are dropped automatically and rejoin on recovery
- **Zero‑downtime hot reload** — change config with no dropped connections
- **Access control** — IP whitelist / blacklist, CIDR ranges, timed bans; global or per‑backend
- **Rate limiting** — per‑IP sliding window with exponential‑backoff blocking
- **PROXY protocol** — pass the real client IP through to backends (opt‑in per backend)
- **Session affinity** — pin a client IP/CIDR to a backend (soft stickiness)
- **IPv6 / dual‑stack** — one listener serves both families
- **Backpressure** — bounded per‑connection memory under load, plus a hard connection cap
- **Admin dashboard** — live stats, logs, and config editing; password‑protected, with a public health endpoint
- **Multi‑process** — N workers sharing one port via `SO_REUSEPORT`
- **Production‑ready** — `.deb` package + hardened systemd service; file logging with rotation

---

## Requirements

- **Linux** (uses `epoll`/`timerfd`) — native or WSL
- CMake ≥ 3.14 and a C++17 compiler
- Network access at build time (fetches `nlohmann/json` and `cpp-httplib`)

## Build

```bash
sudo apt install -y build-essential cmake
cmake -S . -B build && cmake --build build -j
```

This produces the `build/sluice` binary.

## Run

Create a `config.json` — minimal example:

```json
{
  "routes": [
    { "name": "web", "listen_port": 9000,
      "backends": [ { "host": "127.0.0.1", "port": 8001 } ] }
  ],
  "dashboard": { "enabled": true, "port": 8080 }
}
```

```bash
./build/sluice config.json
```

Clients connecting to `:9000` are now proxied to the backend on `:8001`.

## Dashboard

Open **http://localhost:8080** for live routes, backend health, traffic, and logs.
It's bound to localhost by default; log in with the default password `root` (change
it in the UI). A public `GET /api/health` endpoint is available for uptime monitors.

## Hot reload

Edit the config and apply it with zero downtime:

```bash
kill -USR1 $(pgrep -x sluice)     # or: systemctl reload sluice, or the dashboard button
```

An invalid config is rejected and the running config is kept — a typo can't take you down.

## UDP routing

Add `"protocol": "udp"` to a route and Sluice proxies datagrams instead of streams:

```json
{ "name": "game", "listen_port": 9001, "protocol": "udp",
  "backends": [ { "host": "127.0.0.1", "port": 7401 } ] }
```

Because UDP is connectionless, Sluice **synthesizes a session** per client source
address, pins it to one backend (load‑balancing, affinity, access control and
rate limiting all apply on the first datagram), and reclaims it after an idle
timeout (`udp_session_timeout_seconds`, default 30 — there is no FIN to close it).
`max_udp_sessions` caps concurrent sessions. TCP and UDP can share a port
(`tcp:9000` + `udp:9000` are two routes). Replies always leave from the gateway,
so the client only ever sees Sluice's address.

Caveats: UDP source IPs are spoofable, so IP allow/deny and rate limits are
**advisory**; UDP backends are **not** health‑probed (a TCP connect says nothing
about a datagram service) — a failed backend is detected passively via
`ECONNREFUSED`. A sample config with a TCP and a UDP route side by side is in
[`config/udp_demo.json`](config/udp_demo.json).

---

## Production install (`.deb` + systemd)

```bash
cd build && cpack -G DEB
sudo apt install ./sluice_*.deb
systemctl status sluice
```

Installs as a hardened, unprivileged systemd service (dedicated user,
`CAP_NET_BIND_SERVICE`, sandboxing, auto‑restart). See [PACKAGING.md](PACKAGING.md).

## Benchmarks

Held **10,000 concurrent connections with zero failures** in ~5–6 MB of RAM, with
seamless failover under load. Graphs in `benchmark/graphs/`; suite in `tests/benchmark/`.

## Documentation

- **Guided demo:** [`demo/00-index.md`](demo/00-index.md)
- **Full reference** (architecture, feature deep‑dives, complete config format): [`sluice_project_reference.md`](sluice_project_reference.md)
- **Packaging & deployment:** [`PACKAGING.md`](PACKAGING.md)
