# JournalFS Testing

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
| `make test` | Basic operations (create, read, write, mkdir, rename, delete, fsck) |
| `make crash-test` | Crash injection at all 8 stages + idempotency + data integrity |
| `python3 tests/test_transactions.py` | Multi-block operation atomicity |
| `python3 tests/test_corruption.py` | CRC detection of tampered journal/superblock |
| `python3 tests/test_checkpointing.py` | Bounded journal over many transactions |
| `make benchmark` | Throughput / latency comparison (writes CSV) |
| `make graphs` | Plots from the CSV results |

## Crash injection

Set `JFS_CRASH_POINT` to a stage number (1–8) and run a mutating command.  The
process aborts at that point.  Reopen the image to run recovery, then check the
result.

```
JFS_CRASH_POINT=6 build/journalfs img create /file.txt   # crashes after commit
build/journalfs img recover                                 # replays
build/fsck_journalfs img                                   # must be clean
```

## Consistency checker

`build/fsck_journalfs [--repair] <image>` validates the superblock, inode
table, bitmap, directory entries and journal, and reports every inconsistency.
It exits non-zero when issues are found.  It is read-only by default.

## Sanitizer build

```
make asan
```

builds with AddressSanitizer and UndefinedBehaviorSanitizer.  Use the
`build/journalfs` and `build/fsck_journalfs` binaries from that build to catch
memory errors and UB.
