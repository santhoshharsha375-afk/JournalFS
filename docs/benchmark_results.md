# JournalFS Benchmark Results

Benchmarks are generated from real execution — `make benchmark` writes
`results/benchmark_results.csv`, and `make graphs` turns that into PNG plots.
No numbers are invented.

## Modes compared

- **nojournal** — metadata updates without a write-ahead log (comparison mode).
- **journal** — metadata updates through the WAL.
- **journal_ckpt** — metadata updates through the WAL with periodic checkpointing.

## What is measured

- Operations per second.
- Average, median and p95 operation latency.
- Total execution time.
- Journal utilisation (slots occupied).

## Interpretation

Journaling adds a forced `fsync` before home blocks are written, so the journal
mode is expected to be slower than the non-journaled comparison.  That cost is
what buys crash consistency.  Checkpointing reclaims journal space but does not
change the per-commit durability cost.

> The non-journaled mode exists to demonstrate what can go wrong without a
> WAL.  It is a teaching comparison, not a recommended configuration.

## Graphs

- `results/throughput.png` — operations/sec by mode.
- `results/latency.png` — avg/median/p95 latency by mode.
- `results/total_time.png` — total execution time by mode.

Run `make benchmark graphs` to regenerate everything from scratch.
