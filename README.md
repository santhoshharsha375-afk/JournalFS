# JournalFS — A Crash-Consistent Filesystem Using Write-Ahead Logging

A small, teachable filesystem that recovers correctly after a simulated power
failure, built on a write-ahead log (WAL), atomic transactions, checksummed
journal records, and mount-time recovery.

> **Status:** work in progress. The on-disk format, disk layer, CRC32 and the
> `mkfs` formatter are in place. The transaction manager, WAL, recovery,
> checkpointing, FUSE operations, crash-injection harness, consistency checker
> and benchmarks are being implemented.

## What it does

JournalFS stores all of its metadata (superblock, inode table, directory
entries, allocation bitmap) in a regular disk-image file. Every metadata change
goes through a transaction that is written to a write-ahead log **before** the
real metadata blocks are updated. If the process is killed at any point, the
next mount scans the journal and either replays a fully committed transaction
or discards an incomplete one.

## Layout

```
JournalFS/
├── include/    shared on-disk format definitions
├── src/        core C11 implementation
├── tools/      mkfs, fsck, journal_inspect
├── tests/      functional, crash-injection and corruption tests
├── benchmarks/ benchmark + graph scripts
├── scripts/    setup / demo shell scripts
├── docs/       design documents and viva guide
└── results/    generated CSV results and graphs
```

## Build

```
make
```

Requires a C11 compiler (`gcc`), `make`, and — for FUSE mounting — the FUSE 3
development headers (`libfuse3-dev` and `pkg-config` on Ubuntu).

## Documentation

See `docs/architecture.md` and `docs/on_disk_format.md` (in progress).
