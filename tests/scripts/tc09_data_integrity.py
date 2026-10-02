#!/usr/bin/env python3
"""
tc09_data_integrity.py — TC-09: Data Integrity / Large Transfer

Connects through the gateway to large_response_backend, sends a small
trigger, receives the full payload, and verifies every byte matches
the expected repeating 0x00..0xFF pattern exactly.

Prerequisites:
  python3 tests/backends/large_response_backend.py 7001 2097152
  ./build/sluice tests/configs/tc_single_route.json

Usage:
  python3 tests/scripts/tc09_data_integrity.py [gateway_port] [size_bytes]
  Defaults: port=9000, size=2097152 (2 MB)
Exit: 0=PASS  1=FAIL
"""

import socket
import sys

GATEWAY_HOST = "127.0.0.1"
GATEWAY_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
SIZE         = int(sys.argv[2]) if len(sys.argv) > 2 else 2 * 1024 * 1024

EXPECTED = bytes(i % 256 for i in range(SIZE))

print(f"[TC-09] Connecting to gateway {GATEWAY_HOST}:{GATEWAY_PORT}")
print(f"[TC-09] Expecting {SIZE:,} bytes")

try:
    sock = socket.create_connection((GATEWAY_HOST, GATEWAY_PORT), timeout=30)
except Exception as e:
    print(f"[TC-09] FAIL — cannot connect: {e}")
    sys.exit(1)

sock.sendall(b"trigger\n")
sock.shutdown(socket.SHUT_WR)

received = b""
try:
    while len(received) < SIZE:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += chunk
except Exception as e:
    print(f"[TC-09] FAIL — recv error: {e}")
    sock.close()
    sys.exit(1)

sock.close()
print(f"[TC-09] Received {len(received):,} bytes")

if len(received) != SIZE:
    print(f"[TC-09] FAIL — size mismatch: got {len(received)}, expected {SIZE}")
    sys.exit(1)

if received != EXPECTED:
    for i, (r, e) in enumerate(zip(received, EXPECTED)):
        if r != e:
            print(f"[TC-09] FAIL — corruption at byte {i}: got {r:#04x}, expected {e:#04x}")
            sys.exit(1)
    print("[TC-09] FAIL — data mismatch (unknown position)")
    sys.exit(1)

print(f"[TC-09] PASS — all {SIZE:,} bytes received correctly, no corruption")
sys.exit(0)
