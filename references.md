# References 

All sources referenced or shared during planning and development

---

## Tally-provided materials

- **The Wizard's Spellbook (PDF)** 
- **The Wizard of System Programming (PDF)**

### Tally's reading list 
- **[dev.to — I/O Multiplexing](https://dev.to/min38/io-multiplexing-3lho)** — Code-first walkthrough of why thread-per-connection breaks down and the select→poll→epoll progression, plus an epoll/IOCP/kqueue comparison table.
- **[opensource.com — Systems Programming](https://opensource.com/article/22/4/systems-programming)** — Design style philosophy on fault isolation and "let it crash" design;
- **[IBM Docs — Creating a connection-oriented socket](https://www.ibm.com/docs/en/i/7.6.0?topic=design-creating-connection-oriented-socket)** — Canonical socket()/bind()/listen()/accept() sequence checklist; good as a step-order reference
---

## Core systems programming prep

- **[OSTEP — Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/)** 
- **[Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)**
- **[Beej's Guide to Unix IPC](https://beej.us/guide/bgipc/)**
- **[The Linux Programming Interface (man7.org)](https://www.man7.org/tlpi/)** 

---

## L4 relays / traffic gateways

- **[HAProxy — Layer 4 vs Layer 7 Proxy Mode](https://www.haproxy.com/blog/layer-4-and-layer-7-proxy-mode)** — Explanation of TCP-mode vs HTTP-mode proxying; 
- **[HAProxy — TCP Mode config tutorial (official)](https://www.haproxy.com/documentation/haproxy-configuration-tutorials/protocol-support/tcp/)** 
- **[NGINX — TCP/UDP Load Balancing (stream module, admin guide)](https://docs.nginx.com/nginx/admin-guide/load-balancer/tcp-udp-load-balancer/)**
- **[NGINX — ngx_stream_proxy_module (official)](https://nginx.org/en/docs/stream/ngx_stream_proxy_module.html)** — Official raw TCP/UDP proxying module reference.
- **[NGINX — ngx_stream_core_module (official)](https://nginx.org/en/docs/stream/ngx_stream_core_module.html)** — Core stream-layer directives, including PROXY protocol variables.
- **[Serverlab — Layer 4 Load Balancing with HAProxy](https://www.serverlab.ca/tutorials/linux/network-services/layer-4-load-balancing-with-haproxy/)** — Hands-on config walkthrough.


---

## IP whitelisting/blacklisting and rate limiting

- **[HAProxy docs (DeepWiki) — Rate Limiting and DDoS Protection](https://deepwiki.com/haproxy/docs/5.4-rate-limiting-and-ddos-protection)** 
- **[Secure Machinery — Traffic limits with HAProxy stick-tables](https://securemachinery.com/2018/04/14/traffic-limits-with-haproxy/)** — Concrete whitelist/blacklist ACL examples.
- **[NGINX — Rate Limiting with NGINX (official blog)](https://blog.nginx.org/blog/rate-limiting-nginx)** 
- **[oneuptime — HAProxy Rate Limiting by IPv4 Address](https://oneuptime.com/blog/post/2026-03-20-haproxy-rate-limiting-ipv4/view)**

---

## Hot reloading of config files

- **[HAProxy docs (DeepWiki) — Starting, Stopping, and Reloading](https://deepwiki.com/haproxy/docs/6.1-starting-stopping-and-reloading)** 
- **[HAProxy — Truly Seamless Reloads (official blog)](https://www.haproxy.com/blog/truly-seamless-reloads-with-haproxy-no-more-hacks)** 
- **[dev.to — HAProxy's Zero-Downtime Reload](https://dev.to/joshuabvarghese/haproxys-zero-downtime-reload-312j)** — Config validation and health-check state preservation across reload.
- **[NGINX — Control NGINX Processes at Runtime (official docs)](https://docs.nginx.com/nginx/admin-guide/basic-functionality/runtime-control/)** — Signal-based reload (SIGHUP) reference.
- **[luasoftware — Nginx Hot Reload Without Downtime](https://code.luasoftware.com/tutorials/nginx/nginx-hot-reload-without-downtime/)** — Validate-then-swap-then-drain sequence walkthrough.


## Proxy concept — backend sees client IP, client never sees backend

- **[The PROXY Protocol — official specification (haproxy.org)](http://www.haproxy.org/download/2.4/doc/proxy-protocol.txt)** — Primary source; exact wire format for v1 (text) and v2 (binary).
- **[AWS — Preserving client IP with Proxy protocol v2 and NLB (official)](https://aws.amazon.com/blogs/networking-and-content-delivery/preserving-client-ip-address-with-proxy-protocol-v2-and-network-load-balancer/)**
- **[oneuptime — How to Configure Proxy Protocol](https://oneuptime.com/blog/post/2026-01-25-proxy-protocol/view)** 
---

## Multi-process capabilities

- **[NGINX — Inside NGINX: How We Designed for Performance & Scale (official blog)](https://blog.nginx.org/blog/inside-nginx-how-we-designed-for-performance-scale)**
- **[Medium — Nginx Master-Worker Architecture: From Zero to Production](https://medium.com/@nomannayeem/nginx-master-worker-architecture-from-zero-to-production-c451ee8e44ca)** — Practical walkthrough of actual process trees and privilege separation.


## Logging functionality

- **[Dash0 — Mastering Log Rotation in Linux with Logrotate](https://www.dash0.com/guides/log-rotation-linux-logrotate)**
- **[Ducxinh — Understanding Log Rotation in Nginx](https://www.ducxinh.com/en/techblog/understanding-log-rotation-in-nginx)**
- **[simplified.guide — How to configure log rotation for Nginx](https://www.simplified.guide/nginx/configure-log-rotation)** 

## AI tools used

- **Claude Code for research and to validate desgin desicions at production level as well as for coding purposes also used for creating testing markdowns and scripts**
- **Google Gemini for research about similar products and to validate desgin desicions at production level**
- **Google Anitgravity for coding purposes also used for creating testing markdowns and scripts** 

