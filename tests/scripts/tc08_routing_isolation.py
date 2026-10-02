#!/usr/bin/env python3
"""
tc08_routing_isolation.py — TC-08: Routing Isolation

Connects to two different gateway ports and verifies each reaches
a different backend (each echoes a distinct message back).

Prerequisites (run in separate terminals first):
  python3 test_utils/echo_backend.py 7101
  python3 test_utils/echo_backend.py 7102
  ./build/sluice tests/configs/tc_two_routes.json

Usage:
  python3 tests/scripts/tc08_routing_isolation.py
Exit: 0=PASS  1=FAIL
"""

import socket
import sys

GATEWAY_HOST = "127.0.0.1"
ROUTES = [
    {"port": 9100, "msg": b"secret-for-service-A"},
    {"port": 9101, "msg": b"secret-for-service-B"},
]


def send_recv(port, msg):
    sock = socket.create_connection((GATEWAY_HOST, port), timeout=10)
    sock.sendall(msg)
    sock.shutdown(socket.SHUT_WR)
    reply = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        reply += chunk
    sock.close()
    return reply


print("[TC-08] Routing isolation test")
all_pass = True

for route in ROUTES:
    port, msg = route["port"], route["msg"]
    try:
        reply = send_recv(port, msg)
        if reply == msg:
            print(f"  port {port}: PASS — echoed {msg!r}")
        else:
            print(f"  port {port}: FAIL — sent {msg!r}, got {reply!r}")
            all_pass = False
    except Exception as e:
        print(f"  port {port}: FAIL — {e}")
        all_pass = False

print()
if all_pass:
    print("[TC-08] PASS")
    sys.exit(0)
else:
    print("[TC-08] FAIL")
    sys.exit(1)
