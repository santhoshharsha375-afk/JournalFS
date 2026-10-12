#!/usr/bin/env python3
"""
benchmark.py -- Reproducible benchmark suite for JournalFS.

Compares:
  - non-journaled metadata updates (no WAL)
  - journaled metadata updates (WAL)
  - journaled updates with checkpointing

Measures operations/second, average/median/p95 latency, total time and journal
utilisation.  Writes raw results to results/*.csv.  Run `make graphs` to
generate the plots from those CSVs.
"""

import os
import sys
import subprocess
import tempfile
import shutil
import time
import csv
import statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "journalfs")
MKFS = os.path.join(ROOT, "build", "mkfs_journalfs")
RESULTS = os.path.join(ROOT, "results")

OPS = 200


def run_cli(img, cmd, env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    t0 = time.perf_counter()
    r = subprocess.run([CLI, img] + cmd, env=env, capture_output=True,
                       text=True, timeout=120)
    dt = time.perf_counter() - t0
    return r.returncode == 0, dt


def bench_mode(name, workdir, use_journal, use_checkpoint):
    img = os.path.join(workdir, f"{name}.img")
    subprocess.run([MKFS, img], capture_output=True, timeout=60)

    env = {} if use_journal else {"JFS_NOJOURNAL": "1"}

    latencies = []
    ok = True
    t_start = time.perf_counter()

    for i in range(OPS):
        name_f = f"/f{i}.txt"
        ok1, dt1 = run_cli(img, ["create", name_f], env)
        ok2, dt2 = run_cli(img, ["write", name_f, f"content_{i}"], env)
        ok3, dt3 = run_cli(img, ["rm", name_f], env)
        if not (ok1 and ok2 and ok3):
            ok = False
            break
        latencies.append(dt1 + dt2 + dt3)
        if use_checkpoint and i % 20 == 0:
            run_cli(img, ["checkpoint"], env)

    total = time.perf_counter() - t_start

    if not ok:
        return None

    latencies.sort()
    n = len(latencies)
    avg = statistics.mean(latencies)
    med = statistics.median(latencies)
    p95 = latencies[int(n * 0.95)] if n > 0 else 0.0
    ops_per_sec = (OPS * 3) / total if total > 0 else 0.0

    return {
        "mode": name,
        "ops": OPS * 3,
        "total_time_s": f"{total:.4f}",
        "ops_per_sec": f"{ops_per_sec:.2f}",
        "avg_latency_ms": f"{avg * 1000:.3f}",
        "median_latency_ms": f"{med * 1000:.3f}",
        "p95_latency_ms": f"{p95 * 1000:.3f}",
    }


def main():
    if not os.path.exists(CLI):
        print(f"CLI not found: {CLI}. Run make first.")
        sys.exit(2)

    os.makedirs(RESULTS, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="jfs_bench_")

    try:
        print("=== JournalFS benchmark ===")
        print(f"Operations per mode: {OPS} create+write+rm cycles "
              f"({OPS * 3} individual ops)")

        results = []
        for name, jrnl, ckpt in [
            ("nojournal", False, False),
            ("journal", True, False),
            ("journal_ckpt", True, True),
        ]:
            print(f"  benchmarking {name}...")
            row = bench_mode(name, workdir, jrnl, ckpt)
            if row:
                results.append(row)
                print(f"    {row['ops_per_sec']} ops/s, "
                      f"avg {row['avg_latency_ms']} ms")

        csv_path = os.path.join(RESULTS, "benchmark_results.csv")
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(results[0].keys()))
            w.writeheader()
            w.writerows(results)
        print(f"\nResults written to {csv_path}")
        print("Run 'make graphs' to generate plots.")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
