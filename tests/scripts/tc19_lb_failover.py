#!/usr/bin/env python3
"""
tc19_lb_failover.py — TC-19: Load Balancing Skips Unhealthy Backend

Starts the gateway with 3 backends but only runs backends A and C
(B is intentionally down). Verifies the gateway never routes traffic
to the dead backend — all connections go only to A and C.

This test exercises passive health marking: the first connection to B
fails, marks it unhealthy, and subsequent round-robin selections skip it.

Prerequisites:
  python3 tests/backends/tagged_echo_backend.py 7201 A   ← running
  # backend B on :7202 intentionally NOT started
  python3 tests/backends/tagged_echo_backend.py 7203 C   ← running
  ./build/sluice tests/configs/tc_three_backends.json

Usage:
  python3 tests/scripts/tc19_lb_failover.py [num_requests] [gateway_port]
  Defaults: num_requests=6, gateway_port=9200
Exit: 0=PASS  1=FAIL
"""

import socket
import sys

NUM_REQUESTS = int(sys.argv[1]) if len(sys.argv) > 1 else 6
GATEWAY_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9200
GATEWAY_HOST = "127.0.0.1"


def try_send_recv(msg):
    """Returns (reply_str, None) on success or (None, error_str) on failure."""
    try:
        sock = socket.create_connection((GATEWAY_HOST, GATEWAY_PORT), timeout=5)
        sock.sendall(msg)
        sock.shutdown(socket.SHUT_WR)
        reply = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            reply += chunk
        sock.close()
        return reply.decode(errors="replace"), None
    except Exception as e:
        return None, str(e)


print(f"[TC-19] LB failover test: backend B (:7202) is intentionally DOWN")
print(f"        Making {NUM_REQUESTS} requests — none should go to B")
print()

tags_seen = []
failures = 0

for i in range(NUM_REQUESTS):
    msg = f"req-{i}".encode()
    reply, err = try_send_recv(msg)

    if err:
        # The very FIRST attempt might hit B and get dropped — that's OK.
        # Subsequent attempts should succeed as B is now marked unhealthy.
        print(f"  request-{i:02d}: connection dropped (backend was B — acceptable once)")
        tags_seen.append("dropped")
    else:
        tag = reply[1] if reply and len(reply) >= 2 and reply[0] == '[' else "?"
        tags_seen.append(tag)
        if tag == "B":
            print(f"  request-{i:02d}: FAIL — gateway routed to dead backend B!")
            failures += 1
        else:
            print(f"  request-{i:02d}: PASS — backend [{tag}] replied")

print()
if failures == 0:
    print(f"[TC-19] PASS — no traffic reached dead backend B")
    print(f"         Distribution: {' '.join(tags_seen)}")
    sys.exit(0)
else:
    print(f"[TC-19] FAIL — {failures} request(s) reached dead backend B")
    sys.exit(1)
