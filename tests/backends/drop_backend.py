"""
drop_backend.py — TC-14: Backend Crashes Mid-Connection

Accepts a connection, reads exactly `read_bytes` bytes, then abruptly
closes the socket (simulating a backend crash). The gateway must:
  - Detect the RST/EOF from the backend
  - Tear down the paired client connection cleanly
  - Not crash or enter a busy-loop
  - Continue accepting new connections normally

Usage:
    python3 tests/backends/drop_backend.py <port> [read_bytes]
    read_bytes defaults to 16
"""

import socket
import sys

port       = int(sys.argv[1])
read_bytes = int(sys.argv[2]) if len(sys.argv) > 2 else 16

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(5)
print(f"drop_backend on :{port} — will drop after reading {read_bytes} bytes",
      flush=True)

while True:
    c, addr = s.accept()
    print(f"[drop_backend] accepted {addr}", flush=True)
    received = 0
    while received < read_bytes:
        d = c.recv(read_bytes - received)
        if not d:
            break
        received += len(d)
    print(f"[drop_backend] dropping after {received} bytes — simulating crash",
          flush=True)
    c.close()   # abrupt close → RST to gateway
