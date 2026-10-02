#!/usr/bin/env python3
"""
Proxy-protocol-aware echo backend for Sluice tests.

Usage:  python3 proxy_protocol_backend.py <host> <port>

  python3 proxy_protocol_backend.py 127.0.0.1 7001    # IPv4 backend
  python3 proxy_protocol_backend.py ::1       7001    # IPv6 backend

Purpose:
  Demonstrates client-IP preservation. The TCP peer of every accepted
  connection is the GATEWAY (that's the whole point of a proxy). When the
  gateway is configured with "proxy_protocol": true for this backend, it
  prepends a PROXY protocol v1 header naming the *real* client. This script
  prints both so you can see the difference, then echoes the payload back.

  PROXY v1 header (single line, CRLF-terminated, <= 107 bytes):
      PROXY TCP4 <src_ip> <dst_ip> <src_port> <dst_port>\\r\\n
      PROXY TCP6 <src_ip> <dst_ip> <src_port> <dst_port>\\r\\n
      PROXY UNKNOWN\\r\\n
  where src = real client, dst = the gateway address the client connected to.

  Handles one connection at a time. Runs in the foreground; stop with Ctrl+C.
"""
import socket
import sys


def handle(conn, peer):
    # `peer` is the gateway's address — NOT the client's. That is expected.
    data = conn.recv(4096)
    if not data:
        conn.close()
        return

    if data.startswith(b"PROXY "):
        header, sep, rest = data.partition(b"\r\n")
        while sep == b"":  # header split across reads (rare)
            more = conn.recv(4096)
            if not more:
                break
            data += more
            header, sep, rest = data.partition(b"\r\n")
        fields = header.decode(errors="replace").split()
        proto = fields[1] if len(fields) >= 2 else "?"
        print(f"[conn] TCP peer (gateway) = {peer[0]}:{peer[1]}", flush=True)
        print(f"[conn] PROXY header       = {header.decode(errors='replace')}", flush=True)
        if proto in ("TCP4", "TCP6") and len(fields) >= 6:
            print(f"[conn] real client       -> {fields[2]}:{fields[4]} ({proto})", flush=True)
            print(f"[conn] client dialed gw  -> {fields[3]}:{fields[5]}", flush=True)
        else:
            print(f"[conn] PROXY {proto} (no address info)", flush=True)
        payload = rest
    else:
        print(f"[conn] TCP peer = {peer[0]}:{peer[1]}  (NO proxy header received)", flush=True)
        payload = data

    if payload:
        conn.sendall(payload)  # echo whatever came after the header
    while True:
        more = conn.recv(4096)
        if not more:
            break
        conn.sendall(more)
    conn.close()


def main():
    if len(sys.argv) != 3:
        print("usage: python3 proxy_protocol_backend.py <host> <port>", file=sys.stderr)
        sys.exit(1)
    host = sys.argv[1]
    port = int(sys.argv[2])
    family = socket.AF_INET6 if ":" in host else socket.AF_INET

    srv = socket.socket(family, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(16)
    fam = "IPv6" if family == socket.AF_INET6 else "IPv4"
    print(f"proxy-aware backend [{fam}] listening on {host}:{port}", flush=True)

    try:
        while True:
            conn, peer = srv.accept()
            try:
                handle(conn, peer)
            except ConnectionResetError:
                print("[conn] reset by peer", flush=True)
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        srv.close()


if __name__ == "__main__":
    main()
