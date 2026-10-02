#!/usr/bin/env python3
"""
udp_proxy_v2_backend.py — PROXY Protocol v2 aware UDP echo backend.

Usage:
    python3 tests/backends/udp_proxy_v2_backend.py <host> <port> <tag>

    python3 tests/backends/udp_proxy_v2_backend.py 127.0.0.1 7410 P

Purpose:
    Receives UDP datagrams that may be prefixed with a PROXY Protocol v2
    binary header. When the header is present it strips it, reads the real
    client IP/port from it, prints the comparison, then echoes the payload
    back (without the header) so the client sees a normal reply.

    This makes the "before vs after" demo easy:
      • Without proxy_protocol → UDP peer is the gateway (hidden client)
      • With    proxy_protocol → backend logs the *real* client IP from header

PROXY v2 binary header (fixed 16-byte prefix for IPv4):
    Bytes  0-11  Magic signature  \\r\\n\\r\\n\\0\\r\\nQUIT\\n
    Byte   12    Version+Command  0x21 = version 2, command PROXY
    Byte   13    Family+Protocol  0x12 = AF_INET + DGRAM
    Bytes  14-15 Length of rest   (big-endian uint16)
    Bytes  16-19 Source IPv4 address  (real client)
    Bytes  20-23 Destination IPv4 address  (gateway)
    Bytes  24-25 Source port (big-endian)
    Bytes  26-27 Destination port (big-endian)

IPv6 is 0x22 for family+protocol and has 36 address bytes (52 total).
"""
import socket
import struct
import sys

# The 12-byte magic signature that starts every PROXY v2 header.
PROXY_V2_SIG = b"\r\n\r\n\x00\r\nQUIT\n"


def parse_proxy_v2(data: bytes):
    """
    Try to parse a PROXY v2 header from the start of `data`.

    Returns (src_ip, src_port, payload_bytes) on success.
    Returns (None, None, data) if there is no v2 header (pass-through).
    """
    # Minimum header is 16 bytes (sig + cmd + fam + 2-byte length)
    if len(data) < 16 or data[:12] != PROXY_V2_SIG:
        return None, None, data   # no header — raw datagram

    version_cmd = data[12]
    family_proto = data[13]
    length = struct.unpack_from("!H", data, 14)[0]   # big-endian uint16

    # version must be 2, command must be PROXY (not LOCAL)
    if (version_cmd >> 4) != 2 or (version_cmd & 0x0F) != 1:
        return None, None, data   # local command or unknown — pass through

    addr_block = data[16 : 16 + length]
    payload = data[16 + length :]

    af   = (family_proto >> 4) & 0x0F   # address family
    proto = family_proto & 0x0F          # transport: 1=stream 2=dgram

    if af == 1:  # AF_INET (IPv4)
        if len(addr_block) < 12:
            return None, None, data
        src_addr = socket.inet_ntop(socket.AF_INET, addr_block[0:4])
        src_port = struct.unpack_from("!H", addr_block, 8)[0]
        return src_addr, src_port, payload

    if af == 2:  # AF_INET6
        if len(addr_block) < 36:
            return None, None, data
        raw_addr = addr_block[0:16]
        src_port = struct.unpack_from("!H", addr_block, 32)[0]
        # Normalize IPv4-mapped IPv6 addresses (::ffff:a.b.c.d) to plain IPv4
        # so the demo output shows "127.0.0.1" instead of "::ffff:127.0.0.1".
        IPV4_MAPPED_PREFIX = b"\x00" * 10 + b"\xff\xff"
        if raw_addr[:12] == IPV4_MAPPED_PREFIX:
            src_addr = socket.inet_ntop(socket.AF_INET, raw_addr[12:16])
        else:
            src_addr = socket.inet_ntop(socket.AF_INET6, raw_addr)
        return src_addr, src_port, payload

    return None, None, data  # unknown family


def main():
    if len(sys.argv) < 3:
        print("usage: python3 udp_proxy_v2_backend.py <host> <port> [tag]",
              file=sys.stderr)
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])
    tag  = sys.argv[3] if len(sys.argv) > 3 else "P"
    family = socket.AF_INET6 if ":" in host else socket.AF_INET

    s = socket.socket(family, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))

    fam_label = "IPv6" if family == socket.AF_INET6 else "IPv4"
    print(f"[{tag}] udp_proxy_v2_backend [{fam_label}] listening on {host}:{port}",
          flush=True)
    print(f"[{tag}] PROXY v2 aware — will decode client IP from binary header",
          flush=True)

    try:
        while True:
            data, peer = s.recvfrom(65536)

            # peer[0]:peer[1] is the GATEWAY's address (that is the whole
            # point of an L4 proxy — the backend cannot tell who the real
            # client is without the PROXY header).
            real_ip, real_port, payload = parse_proxy_v2(data)

            if real_ip is not None:
                # PROXY v2 header found — embed the real client in the reply
                # so the nc terminal shows it directly (no need to watch backend logs).
                print(f"[{tag}] PROXY v2 ✓  real client → {real_ip}:{real_port}  "
                      f"(gateway peer was {peer[0]}:{peer[1]})", flush=True)
                reply = f"[{tag}] real-client={real_ip}:{real_port} | ".encode() + payload
            else:
                # No header — client IP is hidden; only the gateway peer is known.
                print(f"[{tag}] no PROXY header — UDP peer = {peer[0]}:{peer[1]}", flush=True)
                reply = f"[{tag}] real-client=hidden (no PROXY header) | ".encode() + payload
            s.sendto(reply, peer)

    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        s.close()


if __name__ == "__main__":
    main()
