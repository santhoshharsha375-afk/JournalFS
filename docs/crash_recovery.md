# JournalFS Crash Recovery

## Mount-time recovery

When the filesystem is opened (`fs_open`) or the `recover` command is run, the
recovery procedure executes before the filesystem is exposed:

1. **Validate the superblock.**  Check magic, version and CRC.  If the CRC
   fails (a torn superblock write), the superblock is rebuilt from the latest
   journaled superblock update.
2. **Scan every journal slot.**  A slot is accepted only if its magic, CRC,
   record type, payload length and generation are all valid.
3. **Group records by transaction id.**  A transaction is considered only if
   it has a BEGIN, a contiguous run of UPDATE records (`seq = 0,1,2,...`) and
   a COMMIT.  Anything else is discarded.
4. **Identify committed transactions.**  A transaction with a valid COMMIT
   record is committed; the rest are ignored.
5. **Replay committed, uncheckpointed transactions** in transaction order,
   writing each UPDATE payload to its home block.
6. **Persist the recovered superblock** with the new journal head, tail and
   checkpoint counter.
7. **Validate and report.**

## What survives a crash

| Crash point | Result after recovery |
|-------------|----------------------|
| Before commit durable | Transaction absent (discarded) |
| After commit, before home apply | Transaction replayed (fully applied) |
| After home apply, before checkpoint | Transaction replayed (idempotent) |
| After checkpoint | Transaction absent (already applied) |

## Idempotency

Replay writes block payloads, which are pure functions of the journal record.
Applying the same record twice yields identical bytes, so repeating recovery
is safe.  The `last_checkpointed_txid` in the superblock prevents the same
transaction from being replayed twice across successive mounts.

## Crash injection

The crash-injection harness (`build/jfs_test crash` / `random`) forks a child
per stage, arms a crash point via `tx_set_crash_point`, and lets the transaction
code call `abort()` at that exact, deterministic point, simulating a power
failure.  After each crash the parent reopens the image, recovery runs, and the
result is checked for atomicity and consistency.  The CLI also honours the
`JFS_CRASH_POINT` environment variable for manual experiments.

### SIGKILL vs power failure

`abort()` (SIGABRT) terminates the process immediately.  This is **not**
identical to removing power: after a process crash the OS still guarantees
that any `write()` already returned is visible in the page cache, whereas a
power failure may lose data that was in a volatile write cache.  The tests
therefore verify the **ordering** of the persistence protocol, not every
physical storage failure mode.  The journal protocol is designed to be correct
under both models.
