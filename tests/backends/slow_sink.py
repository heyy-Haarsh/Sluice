#!/usr/bin/env python3
"""
slow_sink.py — accepts connections and NEVER reads from them.

Usage:  python3 slow_sink.py <host> <port>

Used to test gateway backpressure: because this backend never recv()s, the
gateway's client->backend buffer fills to the high-water mark and then the
gateway must stop reading the client (bounding memory) instead of buffering
without limit. Runs in the foreground; stop with Ctrl+C.
"""
import socket
import sys


def main():
    if len(sys.argv) != 3:
        print("usage: python3 slow_sink.py <host> <port>", file=sys.stderr)
        sys.exit(1)
    host, port = sys.argv[1], int(sys.argv[2])
    family = socket.AF_INET6 if ":" in host else socket.AF_INET

    srv = socket.socket(family, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(16)
    print(f"slow_sink listening on {host}:{port} — accepts but never reads", flush=True)

    held = []  # keep refs so the kernel doesn't RST them
    try:
        while True:
            conn, peer = srv.accept()
            held.append(conn)
            print(f"accepted from {peer[0]}:{peer[1]} (holding {len(held)}, not reading)", flush=True)
    except KeyboardInterrupt:
        print("\nshutting down", flush=True)
    finally:
        srv.close()


if __name__ == "__main__":
    main()
