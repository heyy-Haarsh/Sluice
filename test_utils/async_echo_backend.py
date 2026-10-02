#!/usr/bin/env python3
"""
async_echo_backend.py — High-concurrency async TCP echo server
==============================================================
Handles thousands of concurrent connections using asyncio.

Usage:
  python3 async_echo_backend.py <port>
  python3 async_echo_backend.py 7001
"""

import asyncio
import sys


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
        pass
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def main():
    if len(sys.argv) < 2:
        print("Usage: python3 async_echo_backend.py <port>")
        sys.exit(1)

    port = int(sys.argv[1])

    # Raise fd limit so we can handle 10k+ connections
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft < 15000:
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(65535, hard), hard))
    except Exception:
        pass

    server = await asyncio.start_server(
        handle_client, "127.0.0.1", port,
        limit=2**20,        # 1 MB read buffer per connection
        backlog=4096,       # OS accept queue depth
        reuse_address=True,
        reuse_port=True,
    )

    addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
    print(f"[async-echo] listening on {addrs}", flush=True)

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
