# JournalFS — A Crash-Consistent Filesystem Using Write-Ahead Logging

A small, teachable filesystem that recovers correctly after a simulated power
failure, built on a write-ahead log (WAL), atomic transactions, checksummed
journal records, and mount-time recovery.

**Platform:** Ubuntu Linux (22.04/24.04) or Ubuntu on WSL2. x86-64.
Everything — filesystem, tools, tests, benchmark and graph generator — is
plain **C11**. No Python or other runtimes are required.

## What it does

JournalFS stores all of its metadata (superblock, inode table, directory
entries, allocation bitmap) in a regular disk-image file. Every metadata change
goes through a transaction that is written to a write-ahead log **before** the
real metadata blocks are updated. If the process is killed at any point, the
next mount scans the journal and either replays a fully committed transaction
or discards an incomplete one.

## Quick start (fresh Ubuntu machine)

```bash
# 1. Install dependencies (only needed for FUSE mounting; the core builds
#    with nothing but gcc + make)
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libfuse3-dev

# 2. Build everything
make

# 3. Run the full test suite (unit + integration + crash injection +
#    corruption + checkpointing + multithreaded stress)
make test

# 4. Crash-injection suite only
make crash-test

# 5. Benchmark the journal vs no-journal, then plot the results
make benchmark
make graphs          # writes results/*.svg (open in any browser)

# 6. Try it through FUSE (on native Linux; see WSL2 note below)
mkdir -p mnt
./build/mkfs_journalfs journalfs.img
./build/journalfs journalfs.img mnt -f &    # -f = foreground, for debugging
echo "hello" > mnt/hello.txt
cat mnt/hello.txt
ls mnt
fusermount3 -u mnt
./build/fsck_journalfs journalfs.img        # must report OK
```

Or use the helper scripts: `scripts/format_image.sh`, `scripts/mount.sh`,
`scripts/unmount.sh`, `scripts/demo.sh`.

### WSL2 note

WSL2's DrvFS (`/mnt/c/...`) forbids FUSE mounts. Keep the image and mountpoint
on the Linux filesystem (e.g. `~/jfsmnt`), or use the non-FUSE CLI which
exercises every core path (journal, recovery, checkpointing, fsck) without
mounting:

```bash
./build/journalfs journalfs.img create /a.txt
./build/journalfs journalfs.img write /a.txt hello
./build/journalfs journalfs.img read /a.txt
./build/journalfs journalfs.img ls /
./build/journalfs journalfs.img fsck
```

## Binaries

| Binary | Purpose |
|--------|---------|
| `build/journalfs` | FUSE mount + CLI driver (mkfs, create, read, write, rm, mv, ls, stat, truncate, fsck, recover, checkpoint, journal) |
| `build/mkfs_journalfs` | Format an image (refuses to overwrite) |
| `build/fsck_journalfs` | Read-only consistency checker (`--repair` for safe fixes) |
| `build/journal_inspect` | Dump raw journal records |
| `build/jfs_test` | Full C test suite (`unit`, `basic`, `atomic`, `crash`, `random`, `corrupt`, `checkpoint`, `stress`) |
| `build/jfs_bench` | Benchmark harness (writes `results/benchmark_results.csv`) |
| `build/jfs_plot` | Renders `results/*.svg` graphs from the CSV |

## How the WAL protocol works

1. Build the transaction's metadata updates in memory.
2. Write BEGIN + UPDATE records to the journal ring; `fsync`.
3. Write the COMMIT record; `fsync`. **This is the durable commit point.**
4. Apply updates to their home blocks; `fsync`.
5. Reclaim the journal slots (checkpoint).

After a crash, recovery scans the journal: a transaction with a valid,
checksummed COMMIT record is replayed; anything else is discarded. Replay is
idempotent. See `docs/wal_protocol.md` and `docs/crash_recovery.md`.

## Crash injection

`build/jfs_test crash` forks a child per protocol stage (before-BEGIN, during
UPDATEs, after-persist, during COMMIT, after-commit, during apply,
after-apply), lets it `abort()` at that exact point, then reopens the image,
runs recovery and fsck, and asserts the transaction is either fully present or
fully absent. `build/jfs_test -s <seed> random` does the same with seeded
random stages and operations.

Note: process-crash injection (`abort()`) verifies the *ordering* of the
persistence protocol. It is not identical to a physical power failure (which
can also lose data sitting in a device write cache); see
`docs/crash_recovery.md`.

## Project layout

```
JournalFS/
├── include/    on-disk format + module headers
├── src/        core C11 implementation (disk, journal, tx, fs, recovery,
│               checkpoint, fsck, FUSE ops, CLI)
├── tools/      mkfs / fsck / journal_inspect front-ends
├── tests/      C test suite + shell smoke tests
├── benchmarks/ C benchmark harness + SVG graph generator
├── scripts/    setup / format / mount / unmount / demo
├── docs/       architecture, on-disk format, WAL protocol, recovery,
│               testing, benchmark results, viva questions
└── results/    generated CSV + SVG outputs
```

## Documentation

- `docs/architecture.md` — components and data flow
- `docs/on_disk_format.md` — every byte on disk
- `docs/wal_protocol.md` — commit ordering and durability rules
- `docs/crash_recovery.md` — mount-time recovery, idempotency, limits
- `docs/testing.md` — how to run every suite
- `docs/benchmark_results.md` — measured numbers and method
- `docs/viva_questions.md` — Week-12 viva prep
- `docs/implementation_status.md` — implemented features and limitations

## License

MIT — see `LICENSE`.
