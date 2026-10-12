#!/usr/bin/env python3
"""
plot_results.py -- Generate graphs from benchmark CSV results.

Reads results/benchmark_results.csv and produces clearly labelled PNG graphs:
  - operations per second by mode
  - average / median / p95 latency by mode
  - total execution time by mode

Requires matplotlib.  Run after `make benchmark`.
"""

import os
import sys
import csv

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
CSV_PATH = os.path.join(RESULTS, "benchmark_results.csv")


def main():
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is required. Install it with: pip install matplotlib")
        sys.exit(2)

    if not os.path.exists(CSV_PATH):
        print(f"CSV not found: {CSV_PATH}. Run 'make benchmark' first.")
        sys.exit(2)

    rows = []
    with open(CSV_PATH, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)

    if not rows:
        print("No data in CSV.")
        sys.exit(2)

    modes = [r["mode"] for r in rows]
    x = range(len(modes))

    def col(key):
        return [float(r[key]) for r in rows]

    os.makedirs(RESULTS, exist_ok=True)

    # Operations per second
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.bar(x, col("ops_per_sec"), color=["#c0392b", "#2980b9", "#27ae60"])
    ax.set_xticks(list(x))
    ax.set_xticklabels(modes)
    ax.set_ylabel("Operations per second")
    ax.set_xlabel("Mode")
    ax.set_title("JournalFS: Throughput by mode")
    ax.grid(axis="y", alpha=0.3)
    for i, v in enumerate(col("ops_per_sec")):
        ax.text(i, v, f"{v:.1f}", ha="center", va="bottom")
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "throughput.png"), dpi=150)
    plt.close(fig)

    # Latency percentiles
    fig, ax = plt.subplots(figsize=(8, 5))
    width = 0.25
    ax.bar([i - width for i in x], col("avg_latency_ms"), width,
           label="Average", color="#2980b9")
    ax.bar(list(x), col("median_latency_ms"), width,
           label="Median", color="#27ae60")
    ax.bar([i + width for i in x], col("p95_latency_ms"), width,
           label="P95", color="#e67e22")
    ax.set_xticks(list(x))
    ax.set_xticklabels(modes)
    ax.set_ylabel("Latency (ms)")
    ax.set_xlabel("Mode")
    ax.set_title("JournalFS: Operation latency by mode")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "latency.png"), dpi=150)
    plt.close(fig)

    # Total time
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.bar(x, col("total_time_s"), color=["#c0392b", "#2980b9", "#27ae60"])
    ax.set_xticks(list(x))
    ax.set_xticklabels(modes)
    ax.set_ylabel("Total time (seconds)")
    ax.set_xlabel("Mode")
    ax.set_title("JournalFS: Total execution time by mode")
    ax.grid(axis="y", alpha=0.3)
    for i, v in enumerate(col("total_time_s")):
        ax.text(i, v, f"{v:.2f}", ha="center", va="bottom")
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS, "total_time.png"), dpi=150)
    plt.close(fig)

    print(f"Graphs written to {RESULTS}/")
    print("  throughput.png")
    print("  latency.png")
    print("  total_time.png")


if __name__ == "__main__":
    main()
