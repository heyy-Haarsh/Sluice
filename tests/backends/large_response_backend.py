"""
large_response_backend.py — TC-09: Data Integrity

Accepts connections, drains all incoming bytes (waits for EOF from
client), then sends back a large deterministic payload and closes.

The payload is a repeating 0x00..0xFF byte pattern, so tc09_data_integrity.py
can reconstruct the expected bytes and compare exactly.

Usage:
    python3 tests/backends/large_response_backend.py <port> [size_bytes]
    size_bytes defaults to 2097152 (2 MB)
"""

import socket
import sys

port      = int(sys.argv[1])
size      = int(sys.argv[2]) if len(sys.argv) > 2 else 2 * 1024 * 1024

# Deterministic payload: repeating 0x00..0xFF
payload = bytes(i % 256 for i in range(size))

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(5)
print(f"large_response_backend on :{port} — will send {size:,} bytes per connection",
      flush=True)

while True:
    c, addr = s.accept()
    print(f"[backend] connection from {addr}", flush=True)
    # Drain the client's request (wait for FIN)
    while True:
        d = c.recv(65536)
        if not d:
            break
    # Reply with the full payload
    c.sendall(payload)
    c.close()
    print(f"[backend] sent {size:,} bytes, connection closed", flush=True)
