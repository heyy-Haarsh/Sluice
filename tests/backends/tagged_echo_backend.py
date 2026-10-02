"""
tagged_echo_backend.py — TC-18: Load Balancing Round-Robin

Each instance of this backend is started with a tag (e.g. "A", "B", "C").
It echoes back the client's data prefixed with that tag:
    Input:  "hello"
    Output: "[A] hello"

This lets tc18_load_balancing.py confirm which backend served each
request — proving that the gateway is distributing connections across
all backends in round-robin order.

Usage:
    python3 tests/backends/tagged_echo_backend.py <port> <tag>

Example (start three instances in separate terminals):
    python3 tests/backends/tagged_echo_backend.py 7201 A
    python3 tests/backends/tagged_echo_backend.py 7202 B
    python3 tests/backends/tagged_echo_backend.py 7203 C
"""

import socket
import sys

port = int(sys.argv[1])
tag  = sys.argv[2] if len(sys.argv) > 2 else "?"

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(5)
print(f"tagged_echo_backend [{tag}] on :{port}", flush=True)

while True:
    c, _ = s.accept()
    data = b''
    while True:
        d = c.recv(65536)
        if not d:
            break
        data += d
    reply = f"[{tag}] ".encode() + data
    c.sendall(reply)
    c.close()
    print(f"[{tag}] served: {data!r}", flush=True)
