# JournalFS Viva Guide (Week 12)

## What is JournalFS?

A small filesystem backed by a regular file that recovers correctly after a
simulated power failure.  It uses a write-ahead log so that metadata changes
are atomic and durable.

## Likely questions and answers

**Q: What is a write-ahead log (WAL)?**
A: A log where every metadata change is recorded *before* the real metadata
block is overwritten.  The log record must be durable (fsynced) before the
home block is updated.

**Q: What is the write-ahead rule?**
A: Never write a metadata block to its home location before the committed
transaction that produced it is safely in the journal.  The commit record is
the commit point.

**Q: What happens if we crash mid-transaction?**
A: If the commit record is not durable, the transaction is discarded.  If the
commit is durable but the home blocks were not applied, the transaction is
replayed.  Either way the filesystem ends up consistent.

**Q: How does recovery decide what is committed?**
A: A transaction is committed iff its COMMIT record is present and passes the
CRC check.  The commit record is the single point of decision.

**Q: What is a checksum for? Why CRC32?**
A: CRC32 detects accidental corruption (bit flips, torn writes).  It is not
cryptographic — an attacker can recompute it — but it is fast and catches the
common failure modes.

**Q: What does fsync actually guarantee?**
A: It flushes the OS page cache to storage.  On a real power failure the
device's own volatile write cache may still lose data, so we document fsync as
"durable" and note this assumption.

**Q: What is a transaction here?**
A: A set of metadata block updates that must all be visible or all invisible.
Creating a file touches the inode, the directory block and the bitmap — one
transaction covers all three.

**Q: Is the journal bounded?**
A: Yes, it is a ring buffer.  Checkpointing advances the tail to reclaim space
once the transactions using those slots have been applied.

**Q: What is checkpointing?**
A: Applying committed transactions' home blocks and recording that fact in
the superblock, so their journal slots can be reused.

**Q: Why is your replay idempotent?**
A: Replay writes block payloads that are pure functions of the journal record.
Applying the same record twice gives the same bytes, so repeated recovery is
safe.

**Q: What are the limits of the crash tests?**
A: `abort()` (SIGABRT) is a process crash, not a power failure.  The OS still
guarantees completed writes are visible, so the tests verify protocol ordering,
not every physical storage failure.

**Q: What is the atomicity guarantee?**
A: All metadata updates of a transaction are atomic after recovery.  Data
(payload) blocks are written before the metadata that references them, so a
crash never leaves an inode pointing at unwritten data.

**Q: How do you handle disk-full?**
A: The bitmap tracks free blocks.  Allocating when the bitmap is full returns
`ENOSPC`.  The journal capacity is also checked before committing a transaction.

**Q: What POSIX errors do you use?**
A: `ENOENT` (no such file), `EEXIST` (exists), `ENOTDIR`, `EISDIR`,
`ENOTEMPTY`, `ENOSPC` (no space), `EINVAL`, `EIO`, among others.
