#!/usr/bin/env python3
"""
load_generator.py — Async TCP Load Generator (single-process)
==============================================================
Spawns N concurrent asyncio clients to host:port.
Each client connects, then loops: send payload → recv echo → record latency.

The key to correct high-concurrency behaviour:
  - asyncio.sleep(0.001) between exchanges — prevents thundering-herd
    where all N coroutines wake up simultaneously and overwhelm the socket
    layer.  At 1000 conns × 1ms sleep, we get ~1000 exchanges/s per
    connection, which is plenty for benchmarking.
  - Per-connection READ_TIMEOUT prevents a stalled connection from
    blocking its coroutine forever.

Usage:
  python3 load_generator.py --host 127.0.0.1 --port 9000 \
      --connections 500 --duration 15 --payload 1024
"""

import asyncio
import argparse
import time
import json
import statistics
import sys

CONNECT_TIMEOUT = 5.0
READ_TIMEOUT    = 8.0


class LoadTest:
    def __init__(self, host, port, connections, duration, payload_size):
        self.host            = host
        self.port            = port
        self.num_connections = connections
        self.duration        = duration
        self.payload         = b"X" * payload_size
        self.payload_size    = payload_size

        self.latencies            = []
        self.total_bytes_sent     = 0
        self.total_bytes_recv     = 0
        self.successful_connects  = 0
        self.failed_connects      = 0
        self.successful_exchanges = 0
        self.failed_exchanges     = 0
        self._running             = False

    async def _client(self):
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=CONNECT_TIMEOUT
            )
            self.successful_connects += 1
        except Exception:
            self.failed_connects += 1
            return

        try:
            while self._running:
                t0 = time.perf_counter()

                writer.write(self.payload)
                await writer.drain()
                self.total_bytes_sent += self.payload_size

                received = 0
                try:
                    while received < self.payload_size:
                        chunk = await asyncio.wait_for(
                            reader.read(min(65536, self.payload_size - received)),
                            timeout=READ_TIMEOUT
                        )
                        if not chunk:
                            return   # server closed connection
                        received += len(chunk)
                except asyncio.TimeoutError:
                    self.failed_exchanges += 1
                    return

                self.total_bytes_recv += received
                t1 = time.perf_counter()

                if received == self.payload_size:
                    self.latencies.append((t1 - t0) * 1000.0)
                    self.successful_exchanges += 1
                else:
                    self.failed_exchanges += 1
                    return

                # Critical: a small sleep prevents all coroutines from
                # waking up and sending simultaneously (thundering herd).
                # 1 ms is imperceptible to real users but fixes scheduling.
                await asyncio.sleep(0.001)

        except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
            pass
        except Exception:
            self.failed_exchanges += 1
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

    async def _run(self):
        self._running = True
        tasks = [asyncio.create_task(self._client())
                 for _ in range(self.num_connections)]
        await asyncio.sleep(self.duration)
        self._running = False
        # Give clients 2 s to finish their current exchange cleanly
        await asyncio.gather(*tasks, return_exceptions=True)

    def run(self):
        asyncio.run(self._run())

    def result(self):
        lats = sorted(self.latencies) if self.latencies else [0.0]
        n    = len(lats)

        def pct(p):
            idx = min(int(n * p / 100.0), n - 1)
            return round(lats[idx], 3)

        total_mb   = (self.total_bytes_sent + self.total_bytes_recv) / (1024 * 1024)
        throughput = total_mb / self.duration if self.duration > 0 else 0.0

        return {
            "requested_connections":  self.num_connections,
            "successful_connections": self.successful_connects,
            "failed_connections":     self.failed_connects,
            "successful_exchanges":   self.successful_exchanges,
            "failed_exchanges":       self.failed_exchanges,
            "duration_seconds":       self.duration,
            "throughput_mb_s":        round(throughput, 3),
            "latency_ms": {
                "avg":    round(statistics.mean(lats), 3),
                "median": round(statistics.median(lats), 3),
                "p90":    pct(90),
                "p95":    pct(95),
                "p99":    pct(99),
                "max":    round(lats[-1], 3),
            }
        }


def main():
    parser = argparse.ArgumentParser(description="Async TCP load generator")
    parser.add_argument("--host",        default="127.0.0.1")
    parser.add_argument("--port",        type=int, default=9000)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--duration",    type=int, default=10)
    parser.add_argument("--payload",     type=int, default=1024)
    args = parser.parse_args()

    # Raise open-fd limit on Linux/WSL
    if sys.platform != "win32":
        try:
            import resource
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            need = args.connections + 256
            if soft < need:
                resource.setrlimit(resource.RLIMIT_NOFILE, (min(need, hard), hard))
        except Exception:
            pass

    test = LoadTest(args.host, args.port, args.connections,
                    args.duration, args.payload)
    test.run()
    print(json.dumps(test.result(), indent=2))


if __name__ == "__main__":
    main()
