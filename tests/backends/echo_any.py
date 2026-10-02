#!/usr/bin/env python3
"""
echo_any.py — echo backend that binds to an IPv4 OR IPv6 address.

Usage:  python3 echo_any.py <host> <port>

  python3 echo_any.py 127.0.0.1 7001     # IPv4 backend
  python3 echo_any.py ::1        7002     # IPv6 backend

The family is chosen from the host literal (a ':' means IPv6). Each accepted
connection's peer (which is the GATEWAY, not the client) is printed, then the
payload is echoed back. Handles one connection at a time; runs in the
foreground — stop with Ctrl+C.
"""
import socket
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: python3 echo_any.py <host> <port>", file=sys.stderr)
        sys.exit(1)
    host = sys.argv[1]
    port = int(sys.argv[2])
    family = socket.AF_INET6 if ":" in host else socket.AF_INET

    srv = socket.socket(family, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(16)
    fam = "IPv6" if family == socket.AF_INET6 else "IPv4"
    print(f"echo_any [{fam}] listening on {host}:{port}", flush=True)

    try:
        while True:
            conn, peer = srv.accept()
            print(f"[conn] from gateway {peer[0]}:{peer[1]}", flush=True)
            data = b""
            while True:
                d = conn.recv(65536)
                if not d:
                    break
                conn.sendall(d)
                data += d
            conn.close()
            print(f"[conn] echoed {len(data)} byte(s)", flush=True)
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        srv.close()


if __name__ == "__main__":
    main()
