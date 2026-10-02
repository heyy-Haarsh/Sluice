"""
counting_backend.py — TC-10: Concurrent Connections

Accepts connections in parallel (threads), assigns each a unique ID,
reads all bytes the client sends, then replies:
    "conn-<id> ok received <N> bytes\n"

The automated test (tc10_concurrent.py) checks that:
  - Every client got a reply
  - The byte count in the reply matches the message size sent
  - No stream mixing occurred between connections

Usage:
    python3 tests/backends/counting_backend.py <port>
"""

import socket
import sys
import threading

port = int(sys.argv[1])

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(64)
print(f"counting_backend on :{port}", flush=True)

counter_lock = threading.Lock()
connection_counter = 0


def handle(c, conn_id):
    total = 0
    while True:
        d = c.recv(65536)
        if not d:
            break
        total += len(d)
    reply = f"conn-{conn_id} ok received {total} bytes\n".encode()
    c.sendall(reply)
    c.close()
    print(f"[backend] conn-{conn_id}: {total} bytes received, replied", flush=True)


while True:
    c, _ = s.accept()
    with counter_lock:
        connection_counter += 1
        cid = connection_counter
    threading.Thread(target=handle, args=(c, cid), daemon=True).start()
