# PROXY Protocol — Deep Explanation

## The Problem: The Backend Sees the Wrong IP

An L4 proxy is transparent to the network — datagrams and streams travel
through it, but the backend's TCP peer or UDP source address is always the
**gateway**, not the real client. A backend that logs connections, enforces
geo-restrictions, or bills by client IP is completely blind.

```
Client 192.168.1.5  ──► Gateway :9010 ──► Backend :8010
                                             ↑
                          backend sees:  127.0.0.1 (gateway)
                          real client:   192.168.1.5  ← invisible
```

PROXY Protocol solves this by having the **gateway** tell the backend who the
real client is, in a header prepended to the traffic.

---

## TCP — PROXY Protocol v1 (text header)

v1 is a single CRLF-terminated ASCII line sent as the first bytes of the TCP
stream, before any application data:

```
PROXY TCP4 192.168.1.5 10.0.0.1 41001 9010\r\n
```

Fields: `PROXY <proto> <src-ip> <dst-ip> <src-port> <dst-port>`

**How it works in Sluice:**
- Config: `"proxy_protocol": true` on a backend.
- On every new TCP connection, `build_proxy_header()` is called once.
- The header is prepended to the `client_to_backend_buffer` and flushed the
  instant the backend connects — before any client bytes are read.
- Partial writes are handled by the normal EPOLLOUT/buffer machinery.
- The backend (`demo_backend.py`) parses the `PROXY` line, extracts the real
  client IP, and puts it in the HTML response body.

**Why v1 works for TCP but not UDP:**  
v1 is a stream prefix. TCP has a stream. UDP has no stream — each datagram is
standalone. You cannot prepend one header to a series of independent packets.
Also, v1 only defines `TCP4`/`TCP6` transport types — there is no `UDP4`.

---

## UDP — PROXY Protocol v2 (binary header, per-datagram)

v2 is a **16-byte binary header** (IPv4) or **52-byte header** (IPv6) prefixed
to each individual datagram. The spec defines a `DGRAM` transport type
(`0x12` for IPv4, `0x22` for IPv6) specifically for this purpose.

### Binary layout (IPv4, 16 bytes total)

```
Offset  Size  Value        Meaning
──────  ────  ───────────  ──────────────────────────────────────
 0      12    magic sig    \r\n\r\n\0\r\nQUIT\n  (spec-mandated)
12       1    0x21         version=2, command=PROXY
13       1    0x12         AF_INET(1) + DGRAM(2)
14       2    0x000C       length = 12 (big-endian)  ← bytes that follow
16       4    src IPv4     real client address
20       4    dst IPv4     gateway listen address
24       2    src port     real client port (big-endian)
26       2    dst port     gateway listen port (big-endian)
```

### How it works in Sluice

```cpp
// On session creation — inherit flag from backend config
s->proxy_protocol_v2 = backend->proxy_protocol;

// On every datagram forward — build header + sendmsg
uint8_t hdr[52];
size_t hlen = build_proxy_v2_udp_header(s->client_addr, s->client_listen_fd, hdr);

iovec iov[2] = { {hdr, hlen}, {buf, n} };  // header + payload
msghdr msg{ .msg_iov = iov, .msg_iovlen = 2 };
sendmsg(s->backend_fd, &msg, 0);           // one datagram, zero copies
```

- `build_proxy_v2_udp_header()` reads the raw `sockaddr_storage` stored at
  session creation (the true client address) and `getsockname()` on the
  listen fd for the destination.
- `sendmsg()` with two `iovec` entries assembles header + payload into a
  **single datagram** without any `memcpy`.
- Every datagram in the session carries its own header — there is no stream
  to write to once.
- The backend (`udp_proxy_v2_backend.py`) parses the 12-byte magic signature,
  reads the 4-byte source address from offset 16, normalises IPv4-mapped IPv6
  addresses, and embeds the real client IP in the echo reply so it appears
  directly in the `nc` terminal.

### Why getsockname() for the destination

On a wildcard-bound socket (`0.0.0.0`), `getsockname()` returns `0.0.0.0`
rather than the actual interface address. The PROXY v2 spec accepts this —
the destination field is informational and backends that need only the source
IP (the common case) are unaffected. Getting the true destination requires
`recvmsg()` + `IP_PKTINFO`, which is a known extension point deferred as it
adds complexity to the hottest loop in the codebase.

---

## Dual-Stack Note

The gateway listen socket is IPv6 with `IPV6_V6ONLY=0` (dual-stack). IPv4
clients arrive as IPv4-mapped IPv6 addresses (`::ffff:127.0.0.1`). Sluice's
`peer_endpoint()` normalises these to plain dotted-quad before storing them in
`UdpSession::client_ip`. The `sockaddr_storage client_addr` stores the raw
form, so when `build_proxy_v2_udp_header()` sees `AF_INET6` it writes a v2
IPv6 header — and the backend normalises `::ffff:127.0.0.1` back to
`127.0.0.1` when decoding so the demo output is clean.

---

## Summary Comparison

| Dimension         | TCP PROXY v1          | UDP PROXY v2                     |
|-------------------|-----------------------|----------------------------------|
| Format            | ASCII text line       | Binary struct                    |
| Transport tag     | `TCP4` / `TCP6`       | `0x12` (UDP/IPv4) `0x22` (IPv6)  |
| Sent              | Once per connection   | Once per datagram                |
| Mechanism         | Prepend to stream buf | `sendmsg()` + iovec              |
| Memory copies     | None (deque prepend)  | None (scatter iovec)             |
| Backend work      | Parse text line       | Parse 16-byte binary struct      |
| Spec version      | v1 (HAProxy 2010)     | v2 (HAProxy 2014+)               |
