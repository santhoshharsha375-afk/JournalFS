# JournalFS Implementation Status

## Status: complete and tested (pure C11, no Python)

The crash-consistent core — on-disk format, write-ahead log, transactions,
recovery, checkpointing, fsck, crash injection, multithreaded stress and
benchmarks — is implemented and passing tests.  FUSE 3 mounting works on
native Linux (verified with `tests/fuse_smoke.sh`).

## Implemented features

- [x] On-disk format: superblock, inode table, bitmap, directory entries,
      journal ring (CRC32-protected records).
- [x] Format (`mkfs_journalfs`), mount-time open with recovery.
- [x] FUSE 3 operations: getattr, readdir, create, mkdir, unlink, rmdir,
      rename, truncate, open, read, write — with correct POSIX errno mapping.
- [x] Non-FUSE CLI fallback for testing without mounting.
- [x] Write-ahead log with BEGIN / UPDATE / COMMIT records, CRC32 checksums,
      generation counter, ring buffer.
- [x] 8-step persistence protocol with a durable commit point (fsync after
      the COMMIT record).
- [x] Atomic multi-block transactions (create, unlink, rename, truncate).
- [x] Mount-time recovery with idempotent replay; incomplete transactions
      discarded.
- [x] Checkpointing: journal space reclaimed after every applied transaction;
      ring stays bounded (verified over 400 cycles).
- [x] Standalone `fsck` consistency checker (read-only; `--repair` for safe
      fixes).
- [x] Deterministic crash injection at all 8 protocol stages + seeded random
      crash injection.
- [x] Pure-C benchmark suite (CSV) + pure-C SVG graph generator.
- [x] Multithreaded stress suite (8 threads, recursive transaction mutex).
- [x] AddressSanitizer + UndefinedBehaviorSanitizer clean.

## Test results (2026-10-13)

| Suite | Result |
|-------|--------|
| `make test` (shell + full C suite) | 72/72 passed |
| `make crash-test` (crash + random + atomic) | 22/22 passed |
| `make asan` + unit/basic/atomic/corrupt | 53/53 passed, no ASan reports |
| `tests/fuse_smoke.sh` (real FUSE mount) | passed |
| `make benchmark` + `make graphs` | CSV + 4 SVGs generated |

## Known limitations

- **Process-crash injection, not power failure.**  `abort()` is used to
  inject crashes deterministically; it does not emulate a device's volatile
  write cache.  The protocol is designed to be correct under both models, but
  the tests verify ordering, not every physical failure.
- **Single global (recursive) transaction mutex.**  Correctness over
  concurrency; group commit is not implemented (stretch goal).
- **Data blocks are not journaled.**  Only metadata goes through the WAL;
  file payloads are written directly before the metadata that references
  them.  This keeps the design simple and is safe for metadata consistency
  (data=ordered semantics).
- **Fixed image size** (~64 MiB) and a fixed inode count (1024).
- **WSL2:** FUSE mounts are forbidden over DrvFS (`/mnt/c/...`).  Keep the
  image and mountpoint on the Linux filesystem, or use the CLI fallback.
