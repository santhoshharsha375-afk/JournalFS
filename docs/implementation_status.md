# JournalFS Implementation Status

## Status: core complete and tested

The crash-consistent core — on-disk format, write-ahead log, transactions,
recovery, checkpointing, fsck, crash injection and benchmarks — is implemented
and passing tests.

## Implemented features

- [x] On-disk format: superblock, inode table, bitmap, directory entries, journal.
- [x] Format (`mkfs_journalfs`), mount-time open with recovery.
- [x] FUSE 3 operations: getattr, lookup, readdir, create, mkdir, unlink,
      rmdir, rename, truncate, open, read, write.
- [x] Non-FUSE CLI fallback for testing without mounting.
- [x] Write-ahead log with BEGIN / UPDATE / COMMIT records, CRC32 checksums,
      generation counter, ring buffer.
- [x] 8-step persistence protocol with a durable commit point.
- [x] Atomic multi-block transactions.
- [x] Mount-time recovery with idempotent replay.
- [x] Checkpointing and bounded journal.
- [x] Standalone `fsck` consistency checker.
- [x] Crash-injection harness at all 8 stages.
- [x] Benchmark suite with CSV output and graph generation.

## Test results

Run `make test`, `make crash-test`, `make benchmark`, `make graphs` for the
current results.  See `docs/benchmark_results.md`.

## Known limitations

- **Process-crash injection, not power failure.**  `abort()` is used to inject
  crashes deterministically; it does not emulate a device's volatile write
  cache.  The protocol is designed to be correct under both models, but the
  tests verify ordering, not every physical failure.
- **Single global transaction mutex.**  Correctness over concurrency; group
  commit is not implemented.
- **Data blocks are not journaled.**  Only metadata goes through the WAL; file
  payloads are written directly before the metadata that references them.  This
  keeps the design simple and is safe for metadata consistency.
- **Fixed image size** (about 64 MiB) and a fixed inode count.
