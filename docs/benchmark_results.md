# JournalFS Benchmark Results

## Method

- **Workload:** 200 cycles of create + write + rm per mode (600 metadata
  operations), identical across modes.
- **Timing:** `clock_gettime(CLOCK_MONOTONIC)` around each cycle, measured
  in-process (no process-spawn overhead).
- **Modes:**
  - `nojournal` — WAL disabled; home blocks written directly (intentionally
    unsafe; used only as the comparison baseline).
  - `journal` — full WAL protocol (journal fsync, commit fsync, apply fsync).
  - `journal_ckpt` — WAL with a forced checkpoint every 20 cycles.
- **Environment:** Ubuntu 24.04 on WSL2, gcc 13.3, image on a regular file.
- **Reproduce:** `make benchmark && make graphs` (raw CSV in
  `results/benchmark_results.csv`, graphs in `results/*.svg`).

## Results (2026-10-13)

| Mode | ops/s | avg (ms) | median (ms) | p95 (ms) | total (s) |
|------|------:|---------:|------------:|---------:|----------:|
| nojournal | 821.84 | 3.65 | 3.66 | 5.51 | 0.73 |
| journal | 213.81 | 14.03 | 13.23 | 20.26 | 2.81 |
| journal_ckpt | 204.41 | 14.62 | 14.08 | 19.86 | 2.94 |

## Interpretation

- Journaling costs roughly **3.8x throughput** relative to the (unsafe)
  non-journaled baseline.  That is the price of three `fsync` barriers per
  transaction (journal persist, commit record, home-block apply) — the same
  cost class ext3/ext4 pay for metadata journaling.
- Explicit checkpointing adds only ~4% overhead on top of plain journaling,
  because in this design every transaction already reclaims its journal space
  after a successful apply; the forced checkpoint is nearly a no-op.
- Journal utilisation stays at 0 slots between operations: the ring is fully
  reclaimed after every applied transaction (`make test` → `checkpoint`
  suite verifies this over 400 cycles).

## Limitations of this comparison

- `nojournal` does not crash-safely persist metadata; it exists only to
  quantify what the WAL costs, not as a usable mode.
- Single filesystem process; no device-level write-cache emulation, so the
  numbers measure protocol overhead, not raw device latency.
- Group commit is not implemented; with multiple concurrent clients the
  journal mode could amortise fsyncs and close part of the gap.
