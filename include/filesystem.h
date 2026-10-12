/*
 * filesystem.h -- Internal filesystem operations.
 *
 * Every operation that modifies persistent metadata builds a set of block
 * updates in memory and commits them through the transaction manager, so the
 * change is atomic and recoverable.  File payloads are written directly to
 * their data blocks; only metadata goes through the journal.
 *
 * Write helpers take an in-progress transaction and stage their block
 * updates into it; the caller commits once all updates are staged.
 */
#ifndef JFS_FILESYSTEM_H
#define JFS_FILESYSTEM_H

#include "journalfs.h"
#include "disk.h"
#include "transaction.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct filesystem {
    struct tx_manager *tm;
};

/* Open an existing image and run recovery.  Never formats. */
int fs_open(struct filesystem **out, struct disk *d);
void fs_close(struct filesystem *fs);

/* ---- inode helpers ------------------------------------------------ */
int fs_read_inode(struct filesystem *fs, uint32_t ino, struct inode *out);
int fs_stage_inode(struct filesystem *fs, struct transaction *tx,
                   uint32_t ino, const struct inode *in);
int fs_alloc_inode(struct filesystem *fs, struct transaction *tx,
                   uint32_t type, uint32_t mode, uint32_t *out_ino);

/* ---- bitmap helpers ----------------------------------------------- */
int fs_read_bitmap(struct filesystem *fs, uint8_t *out);
int fs_stage_alloc_block(struct filesystem *fs, struct transaction *tx,
                          uint32_t *out);
int fs_stage_free_block(struct filesystem *fs, struct transaction *tx,
                         uint32_t block);

/* ---- directory helpers -------------------------------------------- */
int fs_lookup(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t *out_ino);
int fs_dir_add(struct filesystem *fs, struct transaction *tx,
               uint32_t dir_ino, const char *name, uint32_t ino,
               uint32_t type);
int fs_dir_remove(struct filesystem *fs, struct transaction *tx,
                  uint32_t dir_ino, const char *name);

/* ---- high-level operations (each is one transaction) --------------- */
int fs_create(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t type, uint32_t mode, uint32_t *out_ino);
int fs_unlink(struct filesystem *fs, uint32_t dir_ino, const char *name);
int fs_rmdir(struct filesystem *fs, uint32_t dir_ino, const char *name);
int fs_rename(struct filesystem *fs, uint32_t old_parent, uint32_t new_parent,
              const char *old_name, const char *new_name);
int fs_truncate(struct filesystem *fs, uint32_t ino, uint64_t size);

/* ---- data I/O (payload only; metadata changes are journaled) ------- */
int fs_read_data(struct filesystem *fs, uint32_t ino, void *buf,
                 size_t len, uint64_t offset, size_t *out_len);
int fs_write_data(struct filesystem *fs, uint32_t ino, const void *buf,
                  size_t len, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* JFS_FILESYSTEM_H */
