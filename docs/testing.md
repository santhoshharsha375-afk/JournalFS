# JournalFS Testing

Everything — filesystem, tools, tests, benchmark and graph generator — is
plain C11.  No Python or other runtimes are required.

## Non-FUSE fallback

The core filesystem, journal, recovery, checkpointing and fsck all work
**without FUSE** through the CLI driver (`build/journalfs`).  This is the
primary test path and needs no special permissions:

```
build/journalfs <image> <command> [args]
```

Commands: `mkfs create mkdir read write rm rmdir mv ls stat truncate fsck
recover checkpoint journal`.

## Test suites

| Command | What it tests |
|---------|---------------|
| `make test` | Shell smoke test + the full C suite below |
| `build/jfs_test unit` | CRC32 vectors, superblock, journal slot I/O |
| `build/jfs_test basic` | create, read, write, mkdir, rename, truncate, delete |
| `build/jfs_test atomic` | Multi-block ops all-or-nothing after a crash |
| `make crash-test` | Deterministic + random crash injection (all 8 stages) |
| `build/jfs_test corrupt` | CRC detection of tampered journal/superblock |
| `build/jfs_test checkpoint` | Bounded journal over many transactions |
| `build/jfs_test stress` | Multithreaded create/write/read/unlink soak + fsck |
| `make benchmark` | Throughput / latency comparison (writes CSV) |
| `make graphs` | SVG plots from the CSV results |

Run a subset: `build/jfs_test unit basic`.  Seed the random crash suite:
`build/jfs_test -s 12345 random`.

## Crash injection

Two mechanisms:

1. **In-process (CLI):** set `JFS_CRASH_POINT` to a stage number (1–8) and run
   a mutating command.  The process aborts at that point.  Reopen the image to
   run recovery, then check the result.

   ```
   JFS_CRASH_POINT=6 build/journalfs img create /file.txt   # crashes after commit
   build/journalfs img recover                                 # replays
   build/fsck_journalfs img                                   # must be clean
   ```

2. **Fork-based (test suite):** `build/jfs_test crash` forks a child per stage,
   arms the crash point, performs one operation, and lets the child abort().
   The parent reaps it, runs recovery and fsck, and asserts the transaction is
   either fully present or fully absent.

## Consistency checker

`build/fsck_journalfs [--repair] <image>` validates the superblock, inode
table, bitmap, directory entries and journal, and reports every inconsistency.
It exits non-zero when issues are found.  It is read-only by default.

## Sanitizer build

```
make asan
```

builds with AddressSanitizer and UndefinedBehaviorSanitizer.  Use the
binaries from that build to catch memory errors and UB.
