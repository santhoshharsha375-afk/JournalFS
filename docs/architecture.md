# JournalFS Architecture

## Overview

JournalFS is a small, teachable filesystem that recovers correctly after a
simulated power failure. It uses a **write-ahead log (WAL)** to make metadata
changes atomic and durable: a metadata block is never overwritten in place
until the transaction that produced the new value has been safely recorded in
the journal.

The filesystem is backed by a single regular file (the **disk image**).  All
persistent state — the superblock, inode table, directory entries, allocation
bitmap and the journal — lives in that one file.

```
            +--------------------------------------------------------------+
            |                     journalfs.img                           |
            |                                                              |
            |  +-----------+  +--------+  +-----------+  +-------------+  |
            |  | Superblock|  | Bitmap |  | Inode     |  | Journal     |  |
            |  | (block 0) |  |(block 1)|  | table     |  | region      |  |
            |  +-----------+  +--------+  |(2..65)    |  |(66..577)    |  |
            |                          +-----------+  +-------------+  |
            |                                         | Data blocks    |  |
            |                                         | (578..16961)   |  |
            |                                         +----------------+  |
            +--------------------------------------------------------------+
```

## Source layout

| File | Purpose |
|------|---------|
| `src/disk.c` | Raw image I/O: `pread`/`pwrite`, `fsync`, superblock read/write. |
| `src/crc32.c` | CRC32 (IEEE 802.3) used for journal records and the superblock. |
| `src/journal.c` | Journal slot read/write/validate; ring-offset arithmetic. |
| `src/transaction.c` | Transaction manager: the 8-step write-ahead protocol. |
| `src/filesystem.c` | Filesystem operations (create, read, write, delete, rename, ...). |
| `src/recovery.c` | Mount-time scan, validation and idempotent replay. |
| `src/checkpoint.c` | Journal space reclamation and utilisation reporting. |
| `src/fsck.c` | Standalone consistency checker. |
| `src/mkfs.c` | Image formatting (shared by the mkfs tool and the CLI). |
| `src/main.c` | CLI driver (non-FUSE fallback) and FUSE mount entry point. |
| `src/fuse_ops.c` | FUSE 3 callbacks. |
| `tools/mkfs_journalfs.c` | Formatting CLI. |
| `tools/fsck_journalfs.c` | Checker CLI. |
| `tools/journal_inspect.c` | Journal dump tool. |

## Design goals

1. **Correctness first.** Every metadata mutation goes through the journal.
2. **Teachability.** Small functions, explicit on-disk structures, no magic.
3. **Demonstrability.** Crash injection, recovery, checkpointing, fsck and
   benchmarks all work from the Makefile.

## The write-ahead rule

A metadata block is written to its home location **only after** the committed
transaction that produced it is durable in the journal.  The commit record is
the logical commit point: a transaction is committed if and only if its commit
record passes the CRC check after a crash.
