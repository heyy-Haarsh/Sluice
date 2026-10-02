#!/usr/bin/env python3
"""
orchestrator.py — Traffic Gateway Full Benchmark Suite
=======================================================
Runs ALL benchmark tiers automatically:

  Tier 1 — Scalability (100 → 10k connections)
  Tier 2 — Sustained load (1000 connections for 60s)
  Tier 3 — Failover recovery (kills a backend, measures reconnection)

Output files (all relative to project root):
  benchmark/results/scalability.csv
  benchmark/results/sustained.csv
  benchmark/results/failover.csv
  benchmark/results/resources.csv       (raw per-second CPU/mem/fd data)

Run from project root:
  python3 tests/benchmark/orchestrator.py

Requires:
  - Gateway binary:  build/traffic_gateway
  - Echo backend:    test_utils/echo_backend.py
  - Load generator:  tests/benchmark/load_generator.py
  - Resource mon:    tests/benchmark/resource_monitor.py
"""

import subprocess
import time
import os
import json
import signal
import sys
import shutil

# ── Paths ─────────────────────────────────────────────────────────────────────
ROOT         = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GATEWAY_BIN  = os.path.join(ROOT, "build", "traffic_gateway")
# 3-backend config so no single Python backend is the bottleneck
CONFIG_PATH  = os.path.join(ROOT, "tests", "configs", "tc_benchmark.json")
# Use async backend — handles 10k+ concurrent connections properly
BACKEND_SCR  = os.path.join(ROOT, "test_utils", "async_echo_backend.py")
LOAD_GEN     = os.path.join(ROOT, "tests", "benchmark", "load_generator.py")
MONITOR_SCR  = os.path.join(ROOT, "tests", "benchmark", "resource_monitor.py")

OUT_DIR      = os.path.join(ROOT, "benchmark", "results")

# Backend ports (round-robin across 3 async echo backends)
BACKEND_PORTS = [7001, 7002, 7003]

# ── Benchmark parameters ───────────────────────────────────────────────────────
#   Tier 1: scalability sweep
SCALABILITY_LEVELS   = [100, 500, 1000, 5000, 10000]
SCALABILITY_DURATION = 15    # seconds per tier (long enough for stable averages)
WARMUP_DURATION      = 5     # seconds of throwaway traffic before each tier
COOLDOWN_BETWEEN     = 3     # seconds between tiers (let gateway drain)

#   Tier 2: sustained load
SUSTAINED_CONNECTIONS = 1000
SUSTAINED_DURATION    = 60   # seconds

#   Tier 3: failover — two backends, kill one mid-test
FAILOVER_CONNECTIONS  = 500
FAILOVER_DURATION     = 30   # total seconds
FAILOVER_KILL_AT      = 10   # kill backend B at T+10s

PAYLOAD_BYTES   = 1024   # 1 KB echo payload per round-trip
# Single asyncio event loop handles up to this many concurrent connections well.
# Above this, we spawn multiple parallel load generator processes and merge results.
MAX_PER_WORKER  = 500

# ── Helpers ───────────────────────────────────────────────────────────────────

def banner(msg):
    width = 56
    print("\n" + "═" * width)
    print(f"  {msg}")
    print("═" * width)

def step(n, total, msg):
    print(f"\n[{n}/{total}] {msg}", flush=True)

def ok(msg):
    print(f"    ✓ {msg}", flush=True)

def err(msg):
    print(f"    ✗ {msg}", flush=True)

def raise_fd_limit(needed):
    """Try to raise the open-file-descriptor limit if needed."""
    if sys.platform == "win32":
        return
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft < needed + 200:
            try:
                resource.setrlimit(resource.RLIMIT_NOFILE, (min(needed + 200, hard), hard))
                ok(f"Raised fd limit to {min(needed + 200, hard)}")
            except Exception:
                print(f"    ⚠  Could not raise fd limit (currently {soft}). "
                      f"Run:  ulimit -n 65535")
    except Exception:
        pass

def start_backend(port, suppress=True):
    kwargs = {}
    if suppress:
        kwargs = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    proc = subprocess.Popen([sys.executable, BACKEND_SCR, str(port)], **kwargs)
    return proc

def start_gateway(config, suppress=True):
    kwargs = {}
    if suppress:
        kwargs = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    proc = subprocess.Popen([GATEWAY_BIN, config], **kwargs)
    return proc

def start_monitor(pid, out_csv):
    return subprocess.Popen(
        [sys.executable, MONITOR_SCR, "--pid", str(pid), "--output", out_csv],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

def kill(proc):
    if proc and proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            proc.kill()

def run_load_gen(connections, duration, host="127.0.0.1", port=9000):
    """Run the load generator and return the parsed JSON dict, or None on failure.
    
    For connections > MAX_PER_WORKER, automatically spawns multiple parallel
    processes (each handling MAX_PER_WORKER connections) and merges their results.
    This is how wrk, k6, and ab handle high concurrency internally.
    """
    if connections > MAX_PER_WORKER:
        return _run_load_gen_parallel(connections, duration, host, port)
    return _run_load_gen_single(connections, duration, host, port)


def _run_load_gen_single(connections, duration, host, port):
    """Run one load generator subprocess and return its JSON result."""
    cmd = [
        sys.executable, LOAD_GEN,
        "--host",        host,
        "--port",        str(port),
        "--connections", str(connections),
        "--duration",    str(duration),
        "--payload",     str(PAYLOAD_BYTES),
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=duration + 15)
        stdout = res.stdout.strip()
        if not stdout:
            err(f"Load generator produced no output (stderr: {res.stderr.strip()[:200]})")
            return None
        return json.loads(stdout)
    except subprocess.TimeoutExpired:
        err(f"Load generator timed out after {duration + 15}s")
        return None
    except json.JSONDecodeError as e:
        err(f"Load generator output not valid JSON: {e}")
        return None
    except Exception as e:
        err(f"Load generator error: {e}")
        return None


def _run_load_gen_parallel(connections, duration, host, port):
    """Split connections across multiple parallel load generator processes."""
    # Calculate how many workers we need
    n_workers = (connections + MAX_PER_WORKER - 1) // MAX_PER_WORKER
    base  = connections // n_workers
    extra = connections  % n_workers
    conn_per_worker = [base + (1 if i < extra else 0) for i in range(n_workers)]

    print(f"    (splitting {connections} conns across {n_workers} parallel workers: "
          f"{conn_per_worker})", flush=True)

    # Launch all workers simultaneously
    procs = []
    for n in conn_per_worker:
        cmd = [
            sys.executable, LOAD_GEN,
            "--host",        host,
            "--port",        str(port),
            "--connections", str(n),
            "--duration",    str(duration),
            "--payload",     str(PAYLOAD_BYTES),
        ]
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True))

    # Collect results with timeout
    results = []
    deadline = time.time() + duration + 20
    for p in procs:
        remaining = max(1, deadline - time.time())
        try:
            stdout, stderr = p.communicate(timeout=remaining)
            stdout = stdout.strip()
            if stdout:
                results.append(json.loads(stdout))
            elif stderr:
                err(f"Worker stderr: {stderr[:100]}")
        except subprocess.TimeoutExpired:
            p.kill()
            err("A worker process timed out and was killed.")
        except json.JSONDecodeError:
            err("A worker returned invalid JSON.")

    if not results:
        return None

    # Merge all worker results into one combined dict
    def _sum(key):  return sum(r.get(key, 0) for r in results)
    def _pct(p, lats):
        if not lats: return 0.0
        idx = min(int(len(lats) * p / 100.0), len(lats) - 1)
        return round(lats[idx], 3)

    total_req  = _sum("requested_connections")
    total_succ = _sum("successful_connections")
    total_fail = _sum("failed_connections")
    total_sexc = _sum("successful_exchanges")
    total_fexc = _sum("failed_exchanges")
    total_mb   = sum(r["throughput_mb_s"] * r["duration_seconds"] for r in results)
    throughput = total_mb / duration if duration > 0 else 0.0

    # Re-compute percentiles from the combined latency distribution
    # (approximate: average the reported percentiles weighted by exchange count)
    def wavg(key, sub):
        total_exc = sum(r.get("successful_exchanges", 0) for r in results)
        if total_exc == 0: return 0.0
        return sum(r["latency_ms"][key] * r.get("successful_exchanges", 0)
                   for r in results) / total_exc

    return {
        "requested_connections":  total_req,
        "successful_connections": total_succ,
        "failed_connections":     total_fail,
        "successful_exchanges":   total_sexc,
        "failed_exchanges":       total_fexc,
        "duration_seconds":       duration,
        "throughput_mb_s":        round(throughput, 3),
        "latency_ms": {
            "avg":    round(wavg("avg",    results), 3),
            "median": round(wavg("median", results), 3),
            "p90":    round(wavg("p90",    results), 3),
            "p95":    round(wavg("p95",    results), 3),
            "p99":    round(wavg("p99",    results), 3),
            "max":    round(max(r["latency_ms"]["max"] for r in results), 3),
        }
    }


def write_csv_header(path, header):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(header + "\n")

def append_csv_row(path, row):
    with open(path, "a") as f:
        f.write(row + "\n")

def fmt_row_scalability(d):
    return (f"{d['requested_connections']},"
            f"{d['successful_connections']},"
            f"{d['failed_connections']},"
            f"{d['throughput_mb_s']},"
            f"{d['latency_ms']['avg']},"
            f"{d['latency_ms']['median']},"
            f"{d['latency_ms']['p95']},"
            f"{d['latency_ms']['p99']}")

def print_tier_result(d):
    ok(f"Connections:  {d['successful_connections']}/{d['requested_connections']} "
       f"(failed: {d['failed_connections']})")
    ok(f"Throughput:   {d['throughput_mb_s']} MB/s")
    ok(f"Latency avg:  {d['latency_ms']['avg']} ms   "
       f"p50: {d['latency_ms']['median']} ms   "
       f"p95: {d['latency_ms']['p95']} ms   "
       f"p99: {d['latency_ms']['p99']} ms")

# ── Tier 1: Scalability ────────────────────────────────────────────────────────

def run_scalability(monitor):
    banner("TIER 1 — Scalability Sweep (100 → 10 000 connections)")

    csv_path = os.path.join(OUT_DIR, "scalability.csv")
    write_csv_header(csv_path,
        "connections,successful,failed,"
        "throughput_mb_s,latency_avg_ms,latency_median_ms,latency_p95_ms,latency_p99_ms")

    for i, conns in enumerate(SCALABILITY_LEVELS, 1):
        raise_fd_limit(conns)
        print(f"\n  ▶ Step {i}/{len(SCALABILITY_LEVELS)}: {conns} concurrent connections")

        # Warm-up: run traffic for WARMUP_DURATION without recording
        print(f"    warming up ({WARMUP_DURATION}s)...", end="", flush=True)
        run_load_gen(min(conns, 200), WARMUP_DURATION)
        print(" done", flush=True)

        # Actual measurement
        print(f"    measuring ({SCALABILITY_DURATION}s)...", end="", flush=True)
        d = run_load_gen(conns, SCALABILITY_DURATION)
        print(" done", flush=True)

        if d:
            print_tier_result(d)
            append_csv_row(csv_path, fmt_row_scalability(d))
        else:
            err("No data recorded for this tier.")

        if i < len(SCALABILITY_LEVELS):
            print(f"    cooling down ({COOLDOWN_BETWEEN}s)...", flush=True)
            time.sleep(COOLDOWN_BETWEEN)

    ok(f"Scalability results → {csv_path}")

# ── Tier 2: Sustained Load ────────────────────────────────────────────────────

def run_sustained():
    banner(f"TIER 2 — Sustained Load ({SUSTAINED_CONNECTIONS} conns × {SUSTAINED_DURATION}s)")

    csv_path = os.path.join(OUT_DIR, "sustained.csv")
    write_csv_header(csv_path,
        "elapsed_s,connections,successful,failed,"
        "throughput_mb_s,latency_avg_ms,latency_median_ms,latency_p95_ms,latency_p99_ms")

    raise_fd_limit(SUSTAINED_CONNECTIONS)

    # Sample every 10 seconds
    segment_duration = 10
    segments = SUSTAINED_DURATION // segment_duration
    elapsed = 0

    print(f"\n  Running {segments} × {segment_duration}s segments...", flush=True)
    for seg in range(1, segments + 1):
        print(f"    segment {seg}/{segments}...", end="", flush=True)
        d = run_load_gen(SUSTAINED_CONNECTIONS, segment_duration)
        elapsed += segment_duration
        if d:
            row = (f"{elapsed},"
                   f"{d['requested_connections']},{d['successful_connections']},"
                   f"{d['failed_connections']},{d['throughput_mb_s']},"
                   f"{d['latency_ms']['avg']},{d['latency_ms']['median']},"
                   f"{d['latency_ms']['p95']},{d['latency_ms']['p99']}")
            append_csv_row(csv_path, row)
            print(f" p95={d['latency_ms']['p95']}ms  tput={d['throughput_mb_s']}MB/s", flush=True)
        else:
            print(" ERROR", flush=True)

    ok(f"Sustained results → {csv_path}")

# ── Tier 3: Failover Recovery ─────────────────────────────────────────────────

def run_failover():
    """
    Start a two-backend config.  Kill one backend at FAILOVER_KILL_AT seconds.
    Record connection success and latency before and after the kill.
    Write two rows: 'before_kill' and 'after_kill'.
    """
    banner("TIER 3 — Failover Recovery Test")

    csv_path = os.path.join(OUT_DIR, "failover.csv")
    write_csv_header(csv_path,
        "phase,connections,successful,failed,"
        "throughput_mb_s,latency_avg_ms,latency_p95_ms,latency_p99_ms")

    # Start TWO backends
    print("\n  Starting backend A (port 7001)...", flush=True)
    ba = start_backend(7001)
    print("  Starting backend B (port 7002)...", flush=True)
    bb = start_backend(7002)
    time.sleep(0.5)

    # Start gateway with two-backend config
    two_backend_cfg = os.path.join(ROOT, "tests", "configs", "tc_two_routes.json")
    # Fallback to single-route if two-route config doesn't have two backends
    # We will create a temp config
    tmp_cfg = os.path.join(OUT_DIR, "tmp_failover.json")
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(tmp_cfg, "w") as f:
        f.write('{\n'
                '  "heartbeat": { "enabled": true, "interval_seconds": 3 },\n'
                '  "routes": [\n'
                '    {\n'
                '      "name": "failover-service",\n'
                '      "listen_port": 9010,\n'
                '      "backends": [\n'
                '        { "host": "127.0.0.1", "port": 7001 },\n'
                '        { "host": "127.0.0.1", "port": 7002 }\n'
                '      ]\n'
                '    }\n'
                '  ]\n'
                '}\n')

    print("  Starting gateway (dual-backend, port 9010)...", flush=True)
    gw = start_gateway(tmp_cfg)
    time.sleep(1)

    if gw.poll() is not None:
        err("Gateway failed to start for failover test.")
        kill(ba); kill(bb)
        return

    raise_fd_limit(FAILOVER_CONNECTIONS)

    # Phase 1: before kill
    print(f"\n  Phase 1: measuring {FAILOVER_KILL_AT}s with BOTH backends alive...", flush=True)
    d1 = run_load_gen(FAILOVER_CONNECTIONS, FAILOVER_KILL_AT, port=9010)
    if d1:
        print_tier_result(d1)
        append_csv_row(csv_path,
            f"before_kill,{d1['requested_connections']},{d1['successful_connections']},"
            f"{d1['failed_connections']},{d1['throughput_mb_s']},"
            f"{d1['latency_ms']['avg']},{d1['latency_ms']['p95']},{d1['latency_ms']['p99']}")

    # Kill backend B
    print(f"\n  ⚡ Killing backend B (port 7002)...", flush=True)
    kill(bb)
    time.sleep(1)  # give heartbeat a moment to detect it

    # Phase 2: after kill
    remaining = FAILOVER_DURATION - FAILOVER_KILL_AT
    print(f"  Phase 2: measuring {remaining}s with backend B DEAD (traffic on A only)...", flush=True)
    d2 = run_load_gen(FAILOVER_CONNECTIONS, remaining, port=9010)
    if d2:
        print_tier_result(d2)
        append_csv_row(csv_path,
            f"after_kill,{d2['requested_connections']},{d2['successful_connections']},"
            f"{d2['failed_connections']},{d2['throughput_mb_s']},"
            f"{d2['latency_ms']['avg']},{d2['latency_ms']['p95']},{d2['latency_ms']['p99']}")

    # Cleanup
    kill(gw); kill(ba); kill(bb)
    try:
        os.remove(tmp_cfg)
    except Exception:
        pass

    ok(f"Failover results → {csv_path}")

# ── Main ───────────────────────────────────────────────────────────────────────

def preflight_checks():
    if not os.path.isfile(GATEWAY_BIN):
        print(f"ERROR: Gateway binary not found at {GATEWAY_BIN}")
        print("       Run:  cd build && cmake --build .")
        sys.exit(1)
    if not os.path.isfile(CONFIG_PATH):
        print(f"ERROR: Config not found at {CONFIG_PATH}")
        sys.exit(1)
    if not os.path.isfile(BACKEND_SCR):
        print(f"ERROR: echo_backend.py not found at {BACKEND_SCR}")
        sys.exit(1)
    if sys.platform == "win32":
        print("ERROR: This benchmark suite must be run inside WSL (Linux).")
        sys.exit(1)

def main():
    preflight_checks()
    os.makedirs(OUT_DIR, exist_ok=True)

    banner("Traffic Gateway — Complete Benchmark Suite")
    print(f"  Project root : {ROOT}")
    print(f"  Gateway      : {GATEWAY_BIN}")
    print(f"  Config       : {CONFIG_PATH}")
    print(f"  Output dir   : {OUT_DIR}")
    print(f"\n  Test plan:")
    print(f"    Tier 1 — Scalability  : {SCALABILITY_LEVELS} connections × {SCALABILITY_DURATION}s each")
    print(f"    Tier 2 — Sustained    : {SUSTAINED_CONNECTIONS} connections × {SUSTAINED_DURATION}s")
    print(f"    Tier 3 — Failover     : {FAILOVER_CONNECTIONS} connections, kill backend at T+{FAILOVER_KILL_AT}s")

    # ── Start shared infrastructure for Tier 1 + 2
    step(1, 5, f"Starting {len(BACKEND_PORTS)} async echo backends...")
    backends = []
    for p in BACKEND_PORTS:
        backends.append(start_backend(p))
        print(f"    starting backend on port {p}...", flush=True)
    time.sleep(1.0)   # give all backends time to bind
    ok(f"{len(BACKEND_PORTS)} backends running on ports {BACKEND_PORTS}")

    step(2, 5, "Starting gateway...")
    gateway = start_gateway(CONFIG_PATH)
    time.sleep(1)
    if gateway.poll() is not None:
        err("Gateway exited immediately. Check config and binary.")
        kill(backend)
        sys.exit(1)
    ok(f"Gateway running (PID {gateway.pid})")

    step(3, 5, "Starting resource monitor...")
    res_csv = os.path.join(OUT_DIR, "resources.csv")
    monitor = start_monitor(gateway.pid, res_csv)
    ok(f"Monitor running → {res_csv}")

    step(4, 5, "Running benchmark tiers...")
    try:
        run_scalability(monitor)
        time.sleep(COOLDOWN_BETWEEN)
        run_sustained()
    finally:
        # Always clean up shared infra
        step(5, 5, "Cleaning up shared infrastructure...")
        kill(monitor)
        kill(gateway)
        for b in backends:
            kill(b)
        ok("Stopped monitor, gateway, all backends")

    # ── Tier 3 manages its own processes
    run_failover()

    # ── Final summary
    banner("Benchmark Complete!")
    print(f"\n  Results written to: {OUT_DIR}/")
    print(f"    scalability.csv   — Latency + throughput across load levels")
    print(f"    sustained.csv     — 60-second stability over time")
    print(f"    failover.csv      — Before/after backend kill comparison")
    print(f"    resources.csv     — Per-second CPU, memory, FD count")
    print(f"\n  Next step → generate graphs:")
    print(f"    python3 tests/benchmark/plot_graphs.py\n")

if __name__ == "__main__":
    main()
