#!/usr/bin/env python3
"""
udp_echo.py — tagged UDP echo backend (dual-stack).

Each instance is started with a tag (e.g. "A", "B", "C"). For every datagram it
receives it sends one datagram back, prefixed with that tag:
    Input:  "hello"
    Output: "[A] hello"

The tag makes load balancing visible: send from several distinct client source
ports through the gateway and watch which backend (tag) answers each one. It
also prints the peer it sees for each datagram — which is the GATEWAY's
per-session source socket, never the real client (that is exactly the point of
an L4 proxy).

The socket family is chosen from the host literal (a ':' means IPv6). Runs in
the foreground; stop with Ctrl+C.

Usage:
    python3 tests/backends/udp_echo.py <host> <port> <tag>

Examples (each in its own terminal):
    python3 tests/backends/udp_echo.py 127.0.0.1 7401 A
    python3 tests/backends/udp_echo.py 127.0.0.1 7402 B
    python3 tests/backends/udp_echo.py ::1       7403 C     # IPv6 backend
"""
import socket
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: python3 udp_echo.py <host> <port> [tag]", file=sys.stderr)
        sys.exit(1)
    host = sys.argv[1]
    port = int(sys.argv[2])
    tag = sys.argv[3] if len(sys.argv) > 3 else "?"
    family = socket.AF_INET6 if ":" in host else socket.AF_INET

    s = socket.socket(family, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    fam = "IPv6" if family == socket.AF_INET6 else "IPv4"
    print(f"udp_echo [{tag}] [{fam}] listening on {host}:{port}", flush=True)

    try:
        while True:
            data, peer = s.recvfrom(65536)
            reply = f"[{tag}] ".encode() + data
            s.sendto(reply, peer)
            print(f"[{tag}] from gateway {peer[0]}:{peer[1]} — "
                  f"{len(data)} byte(s): {data!r}", flush=True)
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        s.close()


if __name__ == "__main__":
    main()
