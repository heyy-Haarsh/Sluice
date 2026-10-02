#!/usr/bin/env python3
"""
resource_monitor.py — Gateway Resource Profiler
================================================
Samples CPU %, RSS memory (MB), and open file-descriptor count
from /proc/<pid>/ once per second and appends rows to a CSV file.

This process is launched as a background subprocess by orchestrator.py
and terminated when the benchmark ends.

Usage:
  python3 resource_monitor.py --pid 1234 --output resources.csv
"""

import argparse
import time
import os
import sys


def read_proc(pid):
    """Return (cpu_ticks_total, rss_mb) or (None, None) if process gone."""
    try:
        with open(f"/proc/{pid}/statm") as f:
            parts = f.read().split()
            rss_mb = int(parts[1]) * 4096 / (1024 * 1024)   # pages → MB
        with open(f"/proc/{pid}/stat") as f:
            parts = f.read().split()
            cpu_ticks = int(parts[13]) + int(parts[14])       # utime + stime
        return cpu_ticks, rss_mb
    except Exception:
        return None, None


def count_fds(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except Exception:
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid",      type=int, required=True)
    parser.add_argument("--output",   default="resources.csv")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="Sampling interval in seconds")
    args = parser.parse_args()

    if sys.platform == "win32":
        print("ERROR: resource_monitor requires /proc (Linux/WSL only).")
        sys.exit(1)

    clk_tck = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100

    print(f"[monitor] PID={args.pid}  interval={args.interval}s  → {args.output}",
          flush=True)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    with open(args.output, "w") as f:
        f.write("elapsed_s,cpu_percent,memory_mb,fd_count\n")
        f.flush()

        prev_cpu  = None
        prev_time = None
        start     = time.monotonic()

        try:
            while True:
                now  = time.monotonic()
                cpu, mem = read_proc(args.pid)
                fds      = count_fds(args.pid)

                if cpu is None:
                    print("[monitor] Process exited — stopping.", flush=True)
                    break

                cpu_pct = 0.0
                if prev_cpu is not None and prev_time is not None:
                    dt      = now - prev_time
                    d_ticks = cpu - prev_cpu
                    # (ticks / ticks_per_second) / wall_seconds = fraction of one CPU core
                    cpu_pct = (d_ticks / clk_tck) / dt * 100.0 if dt > 0 else 0.0

                prev_cpu  = cpu
                prev_time = now

                if prev_cpu is not None:   # skip the very first sample (no delta)
                    elapsed = now - start
                    f.write(f"{elapsed:.1f},{cpu_pct:.2f},{mem:.2f},{fds or 0}\n")
                    f.flush()

                time.sleep(args.interval)

        except KeyboardInterrupt:
            print("[monitor] Interrupted.", flush=True)


if __name__ == "__main__":
    main()
