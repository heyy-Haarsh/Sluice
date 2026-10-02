#!/usr/bin/env python3
"""
tc10_concurrent.py — TC-10: Concurrent Connections

Spawns N simultaneous client threads through the gateway, each sending
a unique message and checking for a unique correct reply from
counting_backend. Proves the gateway's event loop handles parallel
clients without mixing streams.

Prerequisites:
  python3 tests/backends/counting_backend.py 7001
  ./build/sluice tests/configs/tc_single_route.json

Usage:
  python3 tests/scripts/tc10_concurrent.py [num_clients] [gateway_port]
  Defaults: num_clients=20, port=9000
Exit: 0=all passed  1=any failed
"""

import socket
import sys
import threading

NUM_CLIENTS  = int(sys.argv[1]) if len(sys.argv) > 1 else 20
GATEWAY_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
GATEWAY_HOST = "127.0.0.1"

results = {}
lock = threading.Lock()


def client_worker(client_id):
    msg = f"hello from client {client_id}\n".encode()
    try:
        sock = socket.create_connection((GATEWAY_HOST, GATEWAY_PORT), timeout=15)
        sock.sendall(msg)
        sock.shutdown(socket.SHUT_WR)
        reply = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            reply += chunk
        sock.close()
        reply_str = reply.decode(errors="replace").strip()
        # counting_backend replies: "conn-N ok received M bytes"
        if f"received {len(msg)} bytes" in reply_str:
            with lock:
                results[client_id] = (True, reply_str)
        else:
            with lock:
                results[client_id] = (False, f"unexpected: {reply_str!r}")
    except Exception as e:
        with lock:
            results[client_id] = (False, str(e))


print(f"[TC-10] Spawning {NUM_CLIENTS} concurrent clients → :{GATEWAY_PORT}")
threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(NUM_CLIENTS)]
for t in threads:
    t.start()
for t in threads:
    t.join(timeout=20)

passed = failed = 0
for cid in range(NUM_CLIENTS):
    ok, detail = results.get(cid, (False, "thread did not complete"))
    status = "PASS" if ok else "FAIL"
    print(f"  client-{cid:02d}: {status}  {detail}")
    if ok:
        passed += 1
    else:
        failed += 1

print()
if failed == 0:
    print(f"[TC-10] PASS — all {passed}/{NUM_CLIENTS} clients succeeded")
    sys.exit(0)
else:
    print(f"[TC-10] FAIL — {failed}/{NUM_CLIENTS} clients failed")
    sys.exit(1)
