# JournalFS Write-Ahead Log Protocol

## The journal

The journal is a **ring buffer** of 256 fixed-size slots (8192 bytes each) in a
dedicated region of the image.  Each slot holds at most one record.  The
superblock tracks:

- `journal_head` — the next slot to write.
- `journal_tail` — the oldest slot that may still be needed.
- `journal_generation` — incremented on format; used to reject stale records
  left over after the ring wraps.

## Record types

- **BEGIN** — opens a transaction (`txid`, `seq = 0`).
- **UPDATE** — a metadata block update (`txid`, `seq`, `block`, 4096-byte payload).
- **COMMIT** — closes a transaction durably (`txid`, final `seq`).

## The 8-step protocol

Every transaction follows this exact sequence:

1. **Construct** the metadata updates in memory.
2. **Write** the BEGIN and UPDATE records to the journal ring.
3. **Persist** the journal records (`fsync`).
4. **Write** the COMMIT record.
5. **Persist** the commit record (`fsync`).  ← **the durable commit point**
6. **Apply** the updates to their home blocks.
7. **Persist** the home blocks (`fsync`).
8. **Checkpoint** — the superblock update (staged in step 1) records the new
   journal head and marks the transaction checkpointed, so its journal space
   can be reclaimed.

The superblock is staged as an ordinary journaled block in step 2, so the
checkpoint state is committed atomically with everything else.

## Write-ahead rule

> A home metadata block is never persisted before the committed transaction
> that produced it is durable in the journal.

This is enforced by ordering: the commit record (step 5) is fsynced **before**
any home block is written (step 6).

## What the commit record means

After a crash, a transaction is **committed** if and only if its COMMIT record
is present and passes the CRC check.  The commit record is the single point of
decision:

- Crash **before** a valid commit is durable → the transaction is discarded.
- Crash **after** the commit is durable but before home blocks are applied →
  the transaction is replayed.

## Idempotent replay

Replay writes each UPDATE record's payload to its home block.  Applying the
same update twice produces the same bytes, so running recovery repeatedly
cannot corrupt the filesystem.  After replay, the superblock records
`last_checkpointed_txid`, so a second recovery will not replay the same
transaction again.

## CRC32: what it detects and what it does not

CRC32 (IEEE 802.3) detects accidental corruption — bit flips, torn writes and
stale data — with high probability.  It is **not** a cryptographic hash: an
attacker who can rewrite a block can also recompute the CRC.  It also cannot
detect a block replaced by an older, equally valid copy of itself; that is what
the journal generation counter and transaction-id ordering are for.

## fsync and power failure

`fsync` flushes the OS page cache to the underlying storage.  On a real power
failure, the storage device's own write cache may or may not be preserved,
depending on the device.  JournalFS treats `fsync` as "durable" and documents
this assumption.  A **process crash** (our crash injection) is not the same as
a power failure: the OS still guarantees that completed `write()` calls are
visible, so our tests verify the *ordering* of the protocol rather than
emulating every physical storage failure.
