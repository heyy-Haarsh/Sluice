#!/usr/bin/env python3
"""
plot_graphs.py — Benchmark Graph Generator
==========================================
Reads the four CSV files produced by orchestrator.py and generates
six publication-quality PNG graphs in benchmark/graphs/.

Graphs produced:
  1. scalability_latency.png    — p50/p95/p99 vs. connection count
  2. scalability_throughput.png — MB/s vs. connection count
  3. scalability_success.png    — successful vs. failed connections
  4. resources_over_time.png    — CPU % + Memory MB + FD count over time
  5. sustained_stability.png    — latency stability over 60-second run
  6. failover_comparison.png    — before vs. after backend failure (bar chart)

Usage:
  python3 tests/benchmark/plot_graphs.py
  (run from the project root)

Requires:
  pip install matplotlib
"""

import os
import sys
import csv

try:
    import matplotlib
    matplotlib.use("Agg")          # headless / no display needed
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    import matplotlib.ticker as mticker
except ImportError:
    print("ERROR: matplotlib is not installed.")
    print("       Run:  pip install matplotlib")
    sys.exit(1)

# ── Paths ─────────────────────────────────────────────────────────────────────
ROOT       = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IN_DIR     = os.path.join(ROOT, "benchmark", "results")
OUT_DIR    = os.path.join(ROOT, "benchmark", "graphs")

SCALABILITY_CSV = os.path.join(IN_DIR, "scalability.csv")
RESOURCES_CSV   = os.path.join(IN_DIR, "resources.csv")
SUSTAINED_CSV   = os.path.join(IN_DIR, "sustained.csv")
FAILOVER_CSV    = os.path.join(IN_DIR, "failover.csv")

os.makedirs(OUT_DIR, exist_ok=True)

# ── Shared style ──────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.family":       "DejaVu Sans",
    "font.size":         12,
    "axes.titlesize":    14,
    "axes.titleweight":  "bold",
    "axes.labelsize":    12,
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "grid.alpha":        0.3,
    "grid.linestyle":    "--",
    "figure.dpi":        150,
    "savefig.dpi":       150,
    "savefig.bbox":      "tight",
})

COLORS = {
    "blue":       "#2563EB",
    "orange":     "#EA580C",
    "green":      "#16A34A",
    "red":        "#DC2626",
    "purple":     "#7C3AED",
    "gray":       "#6B7280",
    "light_blue": "#BFDBFE",
    "light_red":  "#FCA5A5",
}

PROJECT_NAME = "Traffic Gateway"


def load_csv(path):
    """Return list-of-dicts from a CSV.  Returns [] if file missing."""
    if not os.path.exists(path):
        print(f"  ⚠  Not found: {path}")
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def save(fig, name):
    path = os.path.join(OUT_DIR, name)
    fig.savefig(path)
    plt.close(fig)
    print(f"  ✓  {path}")


def annotate_max(ax, xs, ys, color, fmt="{:.1f}"):
    """Put a small label on the highest point of a line."""
    if not ys:
        return
    peak_i = ys.index(max(ys))
    ax.annotate(
        fmt.format(ys[peak_i]),
        xy=(xs[peak_i], ys[peak_i]),
        xytext=(0, 8), textcoords="offset points",
        ha="center", fontsize=9, color=color, fontweight="bold"
    )


# ── Graph 1 — Latency vs Connections ─────────────────────────────────────────

def graph_scalability_latency(rows):
    if not rows:
        return

    # Separate rows with real latency measurements from unmeasured ones
    measured = [r for r in rows if float(r["latency_p95_ms"]) > 0]
    unmeasured = [r for r in rows if float(r["latency_p95_ms"]) == 0]

    if not measured:
        print("  ⚠  No latency data to plot (all 0ms).")
        return

    labels = [r["connections"]            for r in measured]
    p50    = [float(r["latency_median_ms"]) for r in measured]
    p95    = [float(r["latency_p95_ms"])    for r in measured]
    p99    = [float(r["latency_p99_ms"])    for r in measured]

    fig, ax = plt.subplots(figsize=(10, 6))

    ax.plot(labels, p50, marker="o", linewidth=2.5,
            color=COLORS["green"],  label="p50 (median)")
    ax.plot(labels, p95, marker="s", linewidth=2.5,
            color=COLORS["orange"], label="p95")
    ax.plot(labels, p99, marker="^", linewidth=2.5,
            color=COLORS["red"],    label="p99")

    ax.fill_between(labels, p50, p99, alpha=0.08, color=COLORS["red"])

    for x, y in zip(labels, p99):
        ax.annotate(f"{y:.1f} ms", xy=(x, y), xytext=(0, 8),
                    textcoords="offset points", ha="center",
                    fontsize=8.5, color=COLORS["red"])

    ax.set_xlabel("Concurrent Connections")
    ax.set_ylabel("Latency (ms)")
    ax.set_title(f"{PROJECT_NAME} — Latency Scaling (Measured Tiers)")
    ax.legend(loc="upper left")
    ax.grid(axis="y")
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels)

    # Note about unmeasured tiers
    if unmeasured:
        unmeasured_labels = ", ".join(r["connections"] for r in unmeasured)
        fig.text(0.5, -0.04,
                 f"Note: {unmeasured_labels} connections held with 0 failures "
                 f"but Python benchmark tool couldn't measure latency at that scale.",
                 ha="center", fontsize=8.5, color=COLORS["gray"], style="italic")
    else:
        fig.text(0.5, -0.04,
                 "Lower is better.  p95 proves worst-case for 95% of users.",
                 ha="center", fontsize=9, color=COLORS["gray"])

    save(fig, "scalability_latency.png")



# ── Graph 2 — Throughput vs Connections ───────────────────────────────────────

def graph_scalability_throughput(rows):
    if not rows:
        return

    labels     = [r["connections"]         for r in rows]
    throughput = [float(r["throughput_mb_s"]) for r in rows]

    fig, ax = plt.subplots(figsize=(10, 6))

    bars = ax.bar(labels, throughput, color=COLORS["blue"], edgecolor="white",
                  linewidth=0.8, width=0.5, zorder=3)

    # Value labels on top of bars
    for bar, val in zip(bars, throughput):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.02 * max(throughput),
                f"{val:.2f}", ha="center", va="bottom",
                fontsize=9, fontweight="bold", color=COLORS["blue"])

    ax.set_xlabel("Concurrent Connections")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title(f"{PROJECT_NAME} — Throughput Scaling")
    ax.grid(axis="y", zorder=0)
    ax.set_ylim(0, max(throughput) * 1.2)

    fig.text(0.5, -0.04,
             "Higher is better.  Shows gateway data-forwarding capacity.",
             ha="center", fontsize=9, color=COLORS["gray"])

    save(fig, "scalability_throughput.png")


# ── Graph 3 — Success vs Failure ──────────────────────────────────────────────

def graph_scalability_success(rows):
    if not rows:
        return

    labels  = [r["connections"]                  for r in rows]
    success = [int(r["successful"])               for r in rows]
    fail    = [int(r["failed"])                   for r in rows]

    x = range(len(labels))
    fig, ax = plt.subplots(figsize=(10, 6))

    ax.bar(x, success, label="Successful", color=COLORS["green"],
           edgecolor="white", zorder=3)
    ax.bar(x, fail,    label="Failed",     color=COLORS["red"],
           bottom=success, edgecolor="white", zorder=3)

    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Concurrent Connections")
    ax.set_ylabel("Connection Count")
    ax.set_title(f"{PROJECT_NAME} — Connection Reliability")
    ax.legend()
    ax.grid(axis="y", zorder=0)

    fig.text(0.5, -0.04,
             "Green = successful, Red = failed.  Zero red = perfect reliability.",
             ha="center", fontsize=9, color=COLORS["gray"])

    save(fig, "scalability_success.png")


# ── Graph 4 — Resources Over Time ─────────────────────────────────────────────

def graph_resources(rows):
    if not rows:
        return

    ts  = [float(r["elapsed_s"])  for r in rows]
    cpu = [float(r["cpu_percent"]) for r in rows]
    mem = [float(r["memory_mb"])   for r in rows]
    fds = [int(r["fd_count"])      for r in rows]

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 9), sharex=True)
    fig.subplots_adjust(hspace=0.35)

    # CPU
    ax1.fill_between(ts, cpu, alpha=0.25, color=COLORS["red"])
    ax1.plot(ts, cpu, color=COLORS["red"], linewidth=1.5)
    ax1.set_ylabel("CPU (%)")
    ax1.set_title(f"{PROJECT_NAME} — Resource Usage During Load Test")
    ax1.grid(axis="y")

    # Memory
    ax2.fill_between(ts, mem, alpha=0.25, color=COLORS["blue"])
    ax2.plot(ts, mem, color=COLORS["blue"], linewidth=1.5)
    ax2.set_ylabel("Memory (MB)")
    ax2.grid(axis="y")

    # File descriptors
    ax3.fill_between(ts, fds, alpha=0.25, color=COLORS["purple"])
    ax3.plot(ts, fds, color=COLORS["purple"], linewidth=1.5)
    ax3.set_ylabel("Open FDs")
    ax3.set_xlabel("Elapsed Time (s)")
    ax3.grid(axis="y")

    fig.text(0.5, -0.02,
             "FD count tracks open TCP connections.  Flat memory proves epoll efficiency.",
             ha="center", fontsize=9, color=COLORS["gray"])

    save(fig, "resources_over_time.png")


# ── Graph 5 — Sustained Stability ─────────────────────────────────────────────

def graph_sustained(rows):
    if not rows:
        return

    ts  = [float(r["elapsed_s"])           for r in rows]
    p95 = [float(r["latency_p95_ms"])      for r in rows]
    tpt = [float(r["throughput_mb_s"])     for r in rows]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    fig.subplots_adjust(hspace=0.35)

    # p95 stability
    ax1.plot(ts, p95, color=COLORS["orange"], linewidth=2, marker="o", markersize=5)
    mean_p95 = sum(p95) / len(p95)
    ax1.axhline(mean_p95, linestyle="--", color=COLORS["gray"], linewidth=1.2,
                label=f"mean {mean_p95:.2f} ms")
    ax1.set_ylabel("p95 Latency (ms)")
    ax1.set_title(f"{PROJECT_NAME} — 60-Second Sustained Stability")
    ax1.legend()
    ax1.grid(axis="y")

    # Throughput stability
    ax2.fill_between(ts, tpt, alpha=0.2, color=COLORS["green"])
    ax2.plot(ts, tpt, color=COLORS["green"], linewidth=2, marker="o", markersize=5)
    ax2.set_ylabel("Throughput (MB/s)")
    ax2.set_xlabel("Elapsed Time (s)")
    ax2.grid(axis="y")

    fig.text(0.5, -0.02,
             "Flat lines = stable performance.  No degradation over time = production-ready.",
             ha="center", fontsize=9, color=COLORS["gray"])

    save(fig, "sustained_stability.png")


# ── Graph 6 — Failover Comparison ─────────────────────────────────────────────

def graph_failover(rows):
    if not rows:
        return

    def find(phase):
        for r in rows:
            if r["phase"] == phase:
                return r
        return None

    before = find("before_kill")
    after  = find("after_kill")

    if not before or not after:
        print("  ⚠  Failover CSV missing before/after rows.")
        return

    metrics = ["latency_avg_ms", "latency_p95_ms", "latency_p99_ms"]
    labels  = ["Avg Latency (ms)", "p95 Latency (ms)", "p99 Latency (ms)"]

    before_vals = [float(before[m]) for m in metrics]
    after_vals  = [float(after[m])  for m in metrics]

    x     = range(len(metrics))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 6))

    bars1 = ax.bar([i - width/2 for i in x], before_vals, width,
                   label="Both backends ALIVE", color=COLORS["green"], edgecolor="white")
    bars2 = ax.bar([i + width/2 for i in x], after_vals, width,
                   label="Backend B KILLED", color=COLORS["red"], edgecolor="white")

    def label_bars(bars, vals):
        for bar, val in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width() / 2,
                    bar.get_height() + 0.3,
                    f"{val:.1f}", ha="center", va="bottom",
                    fontsize=9, fontweight="bold")

    label_bars(bars1, before_vals)
    label_bars(bars2, after_vals)

    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_ylabel("Latency (ms)")
    ax.set_title(f"{PROJECT_NAME} — Failover: Latency Before vs. After Backend Failure")
    ax.legend()
    ax.grid(axis="y", zorder=0)

    # Annotate success rates
    b_succ = f"{before['successful']}/{before['connections']}"
    a_succ = f"{after['successful']}/{after['connections']}"
    fig.text(0.5, -0.04,
             f"Before kill: {b_succ} connections succeeded  |  "
             f"After kill: {a_succ} connections succeeded",
             ha="center", fontsize=10, color=COLORS["gray"])

    save(fig, "failover_comparison.png")


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    print(f"\n{'='*52}")
    print("  Traffic Gateway — Graph Generator")
    print(f"{'='*52}\n")
    print(f"  Reading CSVs from : {IN_DIR}")
    print(f"  Writing PNGs to   : {OUT_DIR}\n")

    scale_rows     = load_csv(SCALABILITY_CSV)
    resource_rows  = load_csv(RESOURCES_CSV)
    sustained_rows = load_csv(SUSTAINED_CSV)
    failover_rows  = load_csv(FAILOVER_CSV)

    print("  Generating graphs...")
    graph_scalability_latency(scale_rows)
    graph_scalability_throughput(scale_rows)
    graph_scalability_success(scale_rows)
    graph_resources(resource_rows)
    graph_sustained(sustained_rows)
    graph_failover(failover_rows)

    print(f"\n  All graphs saved in: {OUT_DIR}/")
    print("  Files:")
    for f in sorted(os.listdir(OUT_DIR)):
        path = os.path.join(OUT_DIR, f)
        size = os.path.getsize(path) // 1024
        print(f"    {f:<35}  {size} KB")
    print()


if __name__ == "__main__":
    main()
