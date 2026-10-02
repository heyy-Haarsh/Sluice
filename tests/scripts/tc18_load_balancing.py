#!/usr/bin/env python3
"""
tc18_load_balancing.py — TC-18: Round-Robin Load Balancing

Makes N sequential connections through a gateway with 3 backends
(tagged_echo_backend A, B, C). Collects the tag from each reply
and verifies the gateway cycles through them in round-robin order.

Expected distribution for 9 connections: A,B,C,A,B,C,A,B,C

Prerequisites:
  python3 tests/backends/tagged_echo_backend.py 7201 A
  python3 tests/backends/tagged_echo_backend.py 7202 B
  python3 tests/backends/tagged_echo_backend.py 7203 C
  ./build/sluice tests/configs/tc_three_backends.json

Usage:
  python3 tests/scripts/tc18_load_balancing.py [num_requests] [gateway_port]
  Defaults: num_requests=9, gateway_port=9200
Exit: 0=PASS  1=FAIL
"""

import socket
import sys

NUM_REQUESTS = int(sys.argv[1]) if len(sys.argv) > 1 else 9
GATEWAY_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9200
GATEWAY_HOST = "127.0.0.1"
NUM_BACKENDS = 3
TAGS = ["A", "B", "C"]


def send_recv(msg):
    sock = socket.create_connection((GATEWAY_HOST, GATEWAY_PORT), timeout=10)
    sock.sendall(msg)
    sock.shutdown(socket.SHUT_WR)
    reply = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        reply += chunk
    sock.close()
    return reply.decode(errors="replace")


print(f"[TC-18] Load balancing test: {NUM_REQUESTS} requests across {NUM_BACKENDS} backends")

tags_seen = []
all_pass = True

for i in range(NUM_REQUESTS):
    msg = f"request-{i}".encode()
    try:
        reply = send_recv(msg)
        # tagged_echo_backend replies "[A] request-0" etc.
        tag = reply[1] if len(reply) >= 2 and reply[0] == '[' else "?"
        tags_seen.append(tag)
        print(f"  request-{i:02d}: backend [{tag}] replied: {reply.strip()!r}")
    except Exception as e:
        print(f"  request-{i:02d}: FAIL — {e}")
        all_pass = False
        tags_seen.append("?")

# Verify round-robin: tags should cycle A->B->C->A->...
print()
if all_pass:
    for i, tag in enumerate(tags_seen):
        expected = TAGS[i % NUM_BACKENDS]
        if tag != expected:
            print(f"  request-{i}: expected backend [{expected}], got [{tag}]")
            all_pass = False

if all_pass:
    print(f"[TC-18] PASS — round-robin order confirmed: {' '.join(tags_seen)}")
    sys.exit(0)
else:
    print(f"[TC-18] FAIL — distribution was: {' '.join(tags_seen)}")
    sys.exit(1)
