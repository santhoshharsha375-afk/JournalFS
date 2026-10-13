# JournalFS On-Disk Format

All structures are little-endian and use fixed-size blocks of **4096 bytes**.
Every structure is protected by `_Static_assert` so a layout change is a
compile error.

## Layout

| Region | Blocks | Count | Purpose |
|--------|--------|-------|---------|
| Superblock | 0 | 1 | Magic, version, layout, counters, journal state |
| Bitmap | 1 | 1 | Data-block allocation (16384 bits = 2048 bytes) |
| Inode table | 2–65 | 64 | 1024 inodes, 16 per block |
| Journal | 66–577 | 512 | 256 slots × 8192 bytes |
| Data | 578–16961 | 16384 | File and directory payloads |

`TOTAL_BLOCKS = 16962`, `IMAGE_SIZE = 16962 × 4096 = 69 476 352` bytes.

## Superblock (block 0)

```
offset  size  field
0x00    4     magic = 0x4A465331 ("JFS1")
0x04    4     version = 1
0x08    4     block_size = 4096
0x0C    4     total_blocks
0x10    4     data_block_count
0x14    4     inode_count
0x18    4     bitmap_start / bitmap_blocks
0x20    4     inode_start / inode_blocks
0x28    4     journal_start / journal_blocks
0x30    4     data_start
0x34    4     root_inode = 1
0x38    4     free_blocks
0x3C    4     free_inodes
0x40    4     journal_head  (next slot to write)
0x44    4     journal_tail  (oldest slot still needed)
0x48    4     last_txid
0x4C    4     last_committed_txid
0x50    4     last_checkpointed_txid
0x54    4     journal_generation
0x58    4     flags
0x5C    4     crc  (CRC32 of bytes [0, 0x5C))
0x60    ...   zero padding to 4096
```

The superblock is itself a **journaled block** (block 0), so a torn superblock
write is repaired by replaying the journal.

## Inode (256 bytes)

```
offset  size  field
0x00    4     magic = 0x494E4F44 ("INOD")
0x04    4     ino
0x08    4     type (1 = file, 2 = dir)
0x0C    4     mode
0x10    4     uid / gid
0x18    4     nlink
0x1C    4     flags
0x20    8     size
0x28    8     atime / mtime / ctime
0x40    4     block_count
0x44    48    direct[12]   (data-block indices; JFS_BLOCK_FREE = free)
0x5C    4     indirect     (reserved, always JFS_BLOCK_FREE; kept for layout)
0x60    4     pad
0x64    4     crc
0x68    ...   zero padding to 256
```

`JFS_BLOCK_FREE = 0xFFFFFFFF` marks an unused slot.  Zero is a valid data-block
index, so it cannot mean "free".

**File size limit: 12 direct blocks x 4096 = 48 KB.**  JournalFS has no
indirect block: the 13th block allocation reports `JFS_ERR_NOSPC` and writes
past 48 KB report `JFS_ERR_TOOLARGE`.  The `indirect` field stays on disk so
the inode layout (and therefore `sizeof(struct inode)`) is unchanged, but it
is always `JFS_BLOCK_FREE` and `fsck` flags any other value as corruption.

## Directory entry (256 bytes)

```
offset  size  field
0x00    4     ino
0x04    4     type
0x08    2     name_len
0x0A    2     flags
0x0C    244   name
```

16 entries per block.  A directory is a sequence of data blocks filled with
entries; `ino == 0` marks a free slot.

## Journal record (header 32 bytes + payload)

```
offset  size  field
0x00    4     magic = 0x4A524543 ("JREC")
0x04    4     type (1 = BEGIN, 2 = UPDATE, 3 = COMMIT)
0x08    4     txid
0x0C    4     seq
0x10    4     generation
0x14    4     payload_len
0x18    4     block   (UPDATE: home block number)
0x1C    4     crc     (CRC32 of header[0,0x1C) + payload)
```

An UPDATE record's payload is a full 4096-byte metadata block.  A slot is 8192
bytes, so one record fits with room to spare.  A slot is valid only when its
magic **and** CRC both match.
