#!/usr/bin/env python3
"""
hold_conns.py — open <count> connections to a host:port and hold them open.

Usage:  python3 hold_conns.py <host> <port> <count>

Sends nothing; just keeps the sockets established so the gateway counts them as
live client<->backend pairs. Used to test the max_connections cap. Runs in the
foreground; Ctrl+C releases all connections.
"""
import socket
import sys
import time


def main():
    if len(sys.argv) != 4:
        print("usage: python3 hold_conns.py <host> <port> <count>", file=sys.stderr)
        sys.exit(1)
    host, port, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

    socks = []
    for i in range(count):
        try:
            s = socket.create_connection((host, port))
            socks.append(s)
            print(f"opened {i + 1}/{count}", flush=True)
        except OSError as e:
            print(f"connection {i + 1} failed: {e}", flush=True)
        time.sleep(0.1)

    print(f"holding {len(socks)} connection(s) — Ctrl+C to release", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nreleasing", flush=True)
    finally:
        for s in socks:
            s.close()


if __name__ == "__main__":
    main()
