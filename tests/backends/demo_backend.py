#!/usr/bin/env python3
"""
demo_backend.py — a tiny self-identifying HTTP backend for the Sluice demo.

Usage:  python3 demo_backend.py <name> <port>
  python3 demo_backend.py A 8001

Serves a small HTML page showing (a) which backend instance answered, and
(b) the client address the BACKEND sees. If Sluice sends a PROXY-protocol v1
header (proxy_protocol: true), the backend parses it and reports the REAL
client; otherwise it reports its TCP peer — which is the gateway. Prints one
line per request so a terminal can be watched too.
"""
import datetime
import socket
import sys


def handle(conn, peer, name):
    data = conn.recv(4096)
    if not data:
        return

    seen = f"{peer[0]}:{peer[1]}"          # default: the gateway's own socket
    via = "direct connection (the gateway)"
    if data.startswith(b"PROXY "):
        line, _, rest = data.partition(b"\r\n")
        f = line.decode(errors="replace").split()
        if len(f) >= 6:
            seen = f"{f[2]}:{f[4]}"          # real client from the PROXY header
            via = "PROXY protocol header"
        data = rest

    path = "/"
    try:
        path = data.split(b" ", 2)[1].decode(errors="replace")
    except Exception:
        pass
    if path == "/favicon.ico":
        conn.sendall(b"HTTP/1.0 204 No Content\r\nConnection: close\r\n\r\n")
        return

    if path == "/slow":
        conn.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
        for i in range(10):
            import time; time.sleep(1)
            chunk = f"chunk {i}\n"
            conn.sendall(f"{len(chunk):x}\r\n{chunk}\r\n".encode())
        conn.sendall(b"0\r\n\r\n")
        return

    ts = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"[backend {name}] {ts}  client seen = {seen}  ({via})", flush=True)

    body = (
        "<!doctype html><meta charset=utf-8>"
        f"<title>Sluice demo — backend {name}</title>"
        "<body style='font-family:system-ui,sans-serif;text-align:center;margin-top:12vh'>"
        f"<div style='font-size:72px;font-weight:700'>Backend {name}</div>"
        "<p style='font-size:20px;color:#555;margin-top:24px'>The backend sees your address as</p>"
        f"<div style='font-size:30px;color:#0969da;font-family:monospace'>{seen}</div>"
        f"<p style='color:#888'>via {via}</p>"
        "</body>"
    )
    resp = (
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        f"Content-Length: {len(body.encode())}\r\n"
        "Connection: close\r\n\r\n" + body
    )
    conn.sendall(resp.encode())


def main():
    if len(sys.argv) != 3:
        print("usage: python3 demo_backend.py <name> <port>", file=sys.stderr)
        sys.exit(1)
    name, port = sys.argv[1], int(sys.argv[2])

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(64)
    print(f"[backend {name}] listening on 127.0.0.1:{port}", flush=True)

    try:
        while True:
            conn, peer = srv.accept()
            try:
                handle(conn, peer, name)
            except Exception as e:
                print(f"[backend {name}] error: {e}", flush=True)
            finally:
                conn.close()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
