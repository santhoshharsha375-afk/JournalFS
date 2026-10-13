/*
 * filesystem.c -- Core filesystem operations.
 *
 * Metadata (superblock, inodes, directory blocks, bitmap) is only ever
 * modified by staging block updates into a transaction and committing it, so
 * every metadata change is atomic and recoverable.  File payloads are read
 * and written directly to their data blocks.
 */
#include "filesystem.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Low-level block staging helpers                                    */

/* Read a block for modification.  If the block is already staged in this
 * transaction, the staged copy is returned so that earlier modifications are
 * preserved; otherwise the block is read from disk. */
static int stage_block_read(struct filesystem *fs, struct transaction *tx,
                            uint32_t block, uint8_t *buf)
{
    uint8_t *staged = tx_get_staged(tx, block);
    if (staged) {
        memcpy(buf, staged, BLOCK_SIZE);
        return JFS_OK;
    }
    return disk_read_block(fs->tm->disk, block, buf);
}

/* Recompute an in-memory inode's CRC after it has been modified. */
static void inode_update_crc(struct inode *in)
{
    in->crc = jfs_crc32(in, offsetof(struct inode, crc));
}

/* Stamp mtime/ctime with now and refresh the CRC. */
static void inode_touch(struct inode *in)
{
    in->mtime = (uint64_t)time(NULL);
    in->ctime = in->mtime;
    inode_update_crc(in);
}

/* Free an inode: wipe its table slot and return it to the free pool. */
static int inode_free(struct filesystem *fs, struct transaction *tx,
                      uint32_t ino)
{
    struct inode free_in;
    memset(&free_in, 0, sizeof(free_in));
    int rc = fs_stage_inode(fs, tx, ino, &free_in);
    if (rc != JFS_OK)
        return rc;
    fs->tm->sb.free_inodes++;
    return JFS_OK;
}

/* Release every data block an inode still owns. */
static int free_data_blocks(struct filesystem *fs, struct transaction *tx,
                            const struct inode *in)
{
    for (uint32_t b = 0; b < in->block_count; b++) {
        int rc = fs_stage_free_block(fs, tx, in->direct[b]);
        if (rc != JFS_OK)
            return rc;
    }
    return JFS_OK;
}

int fs_read_inode(struct filesystem *fs, uint32_t ino, struct inode *out)
{
    if (!fs || !out || ino == 0 || ino >= INODE_COUNT)
        return JFS_ERR_INVAL;

    /* Lock so a concurrent transaction's multi-block apply cannot be
     * observed half-done.  The mutex is recursive: this is also called
     * from inside already-locked operations. */
    tx_manager_lock(fs->tm);
    uint32_t table_block = INODE_START + ino / INODES_PER_BLOCK;
    uint8_t buf[BLOCK_SIZE];
    int rc = disk_read_block(fs->tm->disk, table_block, buf);
    tx_manager_unlock(fs->tm);
    if (rc != JFS_OK)
        return rc;

    memcpy(out, buf + (ino % INODES_PER_BLOCK) * INODE_SIZE, INODE_SIZE);
    if (out->magic != JFS_INODE_MAGIC)
        return JFS_ERR_NOENT;
    return JFS_OK;
}

int fs_stage_inode(struct filesystem *fs, struct transaction *tx,
                   uint32_t ino, const struct inode *in)
{
    if (!fs || !tx || !in || ino == 0 || ino >= INODE_COUNT)
        return JFS_ERR_INVAL;

    uint32_t table_block = INODE_START + ino / INODES_PER_BLOCK;
    uint8_t buf[BLOCK_SIZE];
    int rc = stage_block_read(fs, tx, table_block, buf);
    if (rc != JFS_OK)
        return rc;

    memcpy(buf + (ino % INODES_PER_BLOCK) * INODE_SIZE, in, INODE_SIZE);
    return tx_stage_update(tx, table_block, buf);
}

int fs_read_bitmap(struct filesystem *fs, uint8_t *out)
{
    if (!fs || !out)
        return JFS_ERR_INVAL;
    return disk_read_block(fs->tm->disk, BITMAP_START, out);
}

static void bitmap_set(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

static void bitmap_clear(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8u] &= (uint8_t)~(1u << (index % 8u));
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index / 8u] >> (index % 8u)) & 1u;
}

static int bitmap_find_free(const uint8_t *bitmap, uint32_t *out_index)
{
    for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++)
        if (!bitmap_test(bitmap, i))
            return *out_index = i, JFS_OK;
    return JFS_ERR_NOSPC;
}

int fs_stage_alloc_block(struct filesystem *fs, struct transaction *tx,
                          uint32_t *out)
{
    if (!fs || !tx || !out)
        return JFS_ERR_INVAL;

    uint8_t bitmap[BLOCK_SIZE];
    int rc = stage_block_read(fs, tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    uint32_t free_index;
    rc = bitmap_find_free(bitmap, &free_index);
    if (rc != JFS_OK)
        return rc;

    bitmap_set(bitmap, free_index);
    rc = tx_stage_update(tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_blocks--;
    *out = free_index;
    return JFS_OK;
}

/* Append a newly allocated data block to an inode.  Files are capped at
 * JFS_NDIRECT direct blocks (48 KB); there is no indirect block, so the
 * 13th allocation reports out of space.  Shared by fs_dir_add and
 * fs_truncate. */
static int link_block(struct filesystem *fs, struct transaction *tx,
                      struct inode *in, uint32_t blk)
{
    if (in->block_count >= JFS_NDIRECT)
        return JFS_ERR_NOSPC;

    in->direct[in->block_count] = blk;
    in->block_count++;
    return JFS_OK;
}

int fs_stage_free_block(struct filesystem *fs, struct transaction *tx,
                         uint32_t block)
{
    if (!fs || !tx || block >= DATA_BLOCK_COUNT)
        return JFS_ERR_INVAL;

    uint8_t bitmap[BLOCK_SIZE];
    int rc = stage_block_read(fs, tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    if (!bitmap_test(bitmap, block))
        return JFS_ERR_INVAL;          /* double free */

    bitmap_clear(bitmap, block);
    rc = tx_stage_update(tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_blocks++;
    return JFS_OK;
}

/* Inode allocation                                                   */

int fs_alloc_inode(struct filesystem *fs, struct transaction *tx,
                   uint32_t type, uint32_t mode, uint32_t *out_ino)
{
    if (!fs || !tx || !out_ino)
        return JFS_ERR_INVAL;

    /* Scan the inode table for a free inode (magic == 0 means unused). */
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < INODE_BLOCKS; b++) {
        int rc = disk_read_block(fs->tm->disk, INODE_START + b, buf);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            struct inode *in = (struct inode *)(buf + i * INODE_SIZE);
            uint32_t ino = b * INODES_PER_BLOCK + i;
            if (ino == 0 || ino >= INODE_COUNT)
                continue;
            if (in->magic == JFS_INODE_MAGIC)
                continue;

            /* Found a free inode: initialise it. */
            struct inode ni;
            memset(&ni, 0, sizeof(ni));
            ni.magic = JFS_INODE_MAGIC;
            ni.ino = ino;
            ni.type = type;
            ni.mode = mode;
            ni.uid = 0;
            ni.gid = 0;
            ni.nlink = (type == JFS_TYPE_DIR) ? 2 : 1;
            ni.size = 0;
            ni.atime = ni.mtime = ni.ctime = (uint64_t)time(NULL);
            ni.block_count = 0;
            for (uint32_t k = 0; k < JFS_NDIRECT; k++)
                ni.direct[k] = JFS_BLOCK_FREE;
            ni.indirect = JFS_BLOCK_FREE;
            ni.crc = jfs_crc32(&ni, offsetof(struct inode, crc));

            rc = fs_stage_inode(fs, tx, ino, &ni);
            if (rc != JFS_OK)
                return rc;

            fs->tm->sb.free_inodes--;
            *out_ino = ino;
            return JFS_OK;
        }
    }
    return JFS_ERR_NOSPC;
}

/* Directory helpers                                                  */

int fs_lookup(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t *out_ino)
{
    if (!fs || !name || !out_ino)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct inode dir;
    int rc = fs_read_inode(fs, dir_ino, &dir);
    if (rc != JFS_OK)
        goto out;
    if (dir.type != JFS_TYPE_DIR) {
        rc = JFS_ERR_NOTDIR;
        goto out;
    }

    uint8_t block[BLOCK_SIZE];
    rc = JFS_ERR_NOENT;
    for (uint32_t b = 0; b < dir.block_count; b++) {
        uint32_t img_block = DATA_START + dir.direct[b];
        if (disk_read_block(fs->tm->disk, img_block, block) != JFS_OK)
            goto out;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0)
                continue;
            if (de->name_len == strlen(name) &&
                memcmp(de->name, name, strlen(name)) == 0) {
                *out_ino = de->ino;
                rc = JFS_OK;
                goto out;
            }
        }
    }
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

/* Populate a dirent slot.  Used both when reusing a free slot and when
 * starting a freshly allocated directory block. */
static void dirent_fill(struct dirent *de, uint32_t ino, uint32_t type,
                        const char *name, size_t name_len)
{
    memset(de, 0, sizeof(*de));
    de->ino = ino;
    de->type = type;
    de->name_len = (uint16_t)name_len;
    memcpy(de->name, name, name_len);
}

/* Add a dirent to a directory, allocating a new directory block if needed.
 * Stages every modified block into tx. */
int fs_dir_add(struct filesystem *fs, struct transaction *tx,
               uint32_t dir_ino, const char *name, uint32_t ino,
               uint32_t type)
{
    if (!fs || !tx || !name || ino == 0)
        return JFS_ERR_INVAL;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > JFS_NAME_MAX)
        return JFS_ERR_INVAL;

    struct inode dir;
    int rc = fs_read_inode(fs, dir_ino, &dir);
    if (rc != JFS_OK)
        return rc;
    if (dir.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;

    uint8_t block[BLOCK_SIZE];
    int found = 0;

    /* Look for a free slot in the existing directory blocks.  Read through
     * the transaction so an earlier staged update (e.g. a rename's remove)
     * is visible; reading the raw disk block would clobber it. */
    for (uint32_t b = 0; b < dir.block_count && !found; b++) {
        uint32_t img_block = DATA_START + dir.direct[b];
        rc = stage_block_read(fs, tx, img_block, block);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0) {
                dirent_fill(de, ino, type, name, name_len);
                found = 1;
                break;
            }
        }
        if (found) {
            rc = tx_stage_update(tx, DATA_START + dir.direct[b], block);
            if (rc != JFS_OK)
                return rc;
        }
    }

    if (!found) {
        /* Allocate a new directory block. */
        uint32_t new_block;
        rc = fs_stage_alloc_block(fs, tx, &new_block);
        if (rc != JFS_OK)
            return rc;

        memset(block, 0, BLOCK_SIZE);
        dirent_fill((struct dirent *)block, ino, type, name, name_len);

        rc = tx_stage_update(tx, DATA_START + new_block, block);
        if (rc != JFS_OK)
            return rc;

        rc = link_block(fs, tx, &dir, new_block);
        if (rc != JFS_OK)
            return rc;
    }

    dir.size += (uint64_t)sizeof(struct dirent);
    inode_touch(&dir);
    return fs_stage_inode(fs, tx, dir_ino, &dir);
}

int fs_dir_remove(struct filesystem *fs, struct transaction *tx,
                  uint32_t dir_ino, const char *name)
{
    if (!fs || !tx || !name)
        return JFS_ERR_INVAL;

    struct inode dir;
    int rc = fs_read_inode(fs, dir_ino, &dir);
    if (rc != JFS_OK)
        return rc;
    if (dir.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;

    uint8_t block[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; b++) {
        uint32_t img_block = DATA_START + dir.direct[b];
        /* Stage-aware read so an earlier staged change (e.g. a rename's
         * dir_add into the same block) is not clobbered. */
        rc = stage_block_read(fs, tx, img_block, block);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0)
                continue;
            if (de->name_len == strlen(name) &&
                memcmp(de->name, name, strlen(name)) == 0) {
                memset(de, 0, sizeof(*de));
                rc = tx_stage_update(tx, img_block, block);
                if (rc != JFS_OK)
                    return rc;

                dir.size -= (uint64_t)sizeof(struct dirent);
                inode_touch(&dir);
                rc = fs_stage_inode(fs, tx, dir_ino, &dir);
                if (rc != JFS_OK)
                    return rc;
                return JFS_OK;
            }
        }
    }
    return JFS_ERR_NOENT;
}

/* Path resolution (shared by the FUSE and CLI front-ends)             */

int fs_resolve_path(struct filesystem *fs, const char *path, uint32_t *ino);

/* Split "/a/b/c" into the parent directory inode and the final name. */
int fs_resolve_parent(struct filesystem *fs, const char *path,
                      uint32_t *parent_ino, char *name, size_t name_len)
{
    if (!fs || !path || path[0] != '/' || !parent_ino || !name)
        return JFS_ERR_INVAL;

    const char *slash = strrchr(path, '/');
    if (!slash)
        return JFS_ERR_INVAL;

    /* Copy the directory part ("/a/b") and NUL-terminate it. */
    size_t plen = (size_t)(slash - path);
    char parent_path[plen + 1];
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    /* An empty directory part means we are at the root ("/name"). */
    int rc = (plen == 0) ? JFS_OK
                         : fs_resolve_path(fs, parent_path, parent_ino);
    if (rc != JFS_OK)
        return rc;
    if (plen == 0)
        *parent_ino = JFS_ROOT_INODE;

    size_t nlen = strlen(slash + 1);
    if (nlen == 0 || nlen > JFS_NAME_MAX || nlen >= name_len)
        return JFS_ERR_INVAL;
    memcpy(name, slash + 1, nlen);
    name[nlen] = '\0';
    return JFS_OK;
}

/* Resolve a full path ("/" or "/a/b") to its inode number. */
int fs_resolve_path(struct filesystem *fs, const char *path, uint32_t *ino)
{
    if (!fs || !path || path[0] != '/' || !ino)
        return JFS_ERR_INVAL;
    if (path[1] == '\0') {
        *ino = JFS_ROOT_INODE;
        return JFS_OK;
    }

    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = fs_resolve_parent(fs, path, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;
    return fs_lookup(fs, parent, name, ino);
}

/* High-level operations                                              */

int fs_create(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t type, uint32_t mode, uint32_t *out_ino)
{
    if (!fs || !name || !out_ino)
        return JFS_ERR_INVAL;

    /* Hold the manager lock for the whole operation so concurrent creators
     * cannot race on the inode allocator, bitmap or directory blocks. */
    tx_manager_lock(fs->tm);

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        goto out;

    uint32_t ino;
    rc = fs_alloc_inode(fs, &tx, type, mode, &ino);
    if (rc != JFS_OK)
        goto out;

    rc = fs_dir_add(fs, &tx, dir_ino, name, ino, type);
    if (rc != JFS_OK)
        goto out;

    rc = tx_commit(&tx);
    if (rc != JFS_OK)
        goto out;

    *out_ino = ino;
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

/* Shared body of fs_unlink and fs_rmdir.  The two differ only in the type
 * they expect, the error reported on a type mismatch, and rmdir's extra
 * non-empty check. */
static int remove_entry(struct filesystem *fs, uint32_t dir_ino,
                        const char *name, uint32_t want_type,
                        int type_err, int require_empty)
{
    if (!fs || !name)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        goto out;

    uint32_t ino;
    rc = fs_lookup(fs, dir_ino, name, &ino);
    if (rc != JFS_OK)
        goto out;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        goto out;
    if (in.type != want_type) {
        rc = type_err;
        goto out;
    }

    if (require_empty && in.size > (uint64_t)(2 * sizeof(struct dirent))) {
        rc = JFS_ERR_NOTEMPTY;
        goto out;
    }

    rc = fs_dir_remove(fs, &tx, dir_ino, name);
    if (rc != JFS_OK)
        goto out;

    rc = free_data_blocks(fs, &tx, &in);
    if (rc != JFS_OK)
        goto out;

    rc = inode_free(fs, &tx, ino);
    if (rc != JFS_OK)
        goto out;

    rc = tx_commit(&tx);
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

int fs_unlink(struct filesystem *fs, uint32_t dir_ino, const char *name)
{
    return remove_entry(fs, dir_ino, name, JFS_TYPE_FILE, JFS_ERR_ISDIR, 0);
}

int fs_rmdir(struct filesystem *fs, uint32_t dir_ino, const char *name)
{
    return remove_entry(fs, dir_ino, name, JFS_TYPE_DIR, JFS_ERR_NOTDIR, 1);
}

int fs_rename(struct filesystem *fs, uint32_t old_parent, uint32_t new_parent,
              const char *old_name, const char *new_name)
{
    if (!fs || !old_name || !new_name)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        goto out;

    uint32_t ino;
    rc = fs_lookup(fs, old_parent, old_name, &ino);
    if (rc != JFS_OK)
        goto out;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        goto out;

    rc = fs_dir_remove(fs, &tx, old_parent, old_name);
    if (rc != JFS_OK)
        goto out;

    rc = fs_dir_add(fs, &tx, new_parent, new_name, ino, in.type);
    if (rc != JFS_OK)
        goto out;

    rc = tx_commit(&tx);
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

int fs_truncate(struct filesystem *fs, uint32_t ino, uint64_t size)
{
    if (!fs)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        goto out;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        goto out;

    uint32_t new_blocks = (uint32_t)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (new_blocks > in.block_count) {
        /* Grow: allocate and link blocks until the count is reached. */
        while (in.block_count < new_blocks) {
            uint32_t blk;
            rc = fs_stage_alloc_block(fs, &tx, &blk);
            if (rc != JFS_OK)
                goto out;
            rc = link_block(fs, &tx, &in, blk);
            if (rc != JFS_OK)
                goto out;
        }
    } else if (new_blocks < in.block_count) {
        /* Shrink: free surplus blocks. */
        while (in.block_count > new_blocks) {
            uint32_t b = in.block_count - 1;
            uint32_t blk = in.direct[b];
            in.direct[b] = JFS_BLOCK_FREE;
            rc = fs_stage_free_block(fs, &tx, blk);
            if (rc != JFS_OK)
                goto out;
            in.block_count--;
        }
    }

    in.size = size;
    inode_touch(&in);
    rc = fs_stage_inode(fs, &tx, ino, &in);
    if (rc != JFS_OK)
        goto out;

    rc = tx_commit(&tx);
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

/* Data I/O                                                           */

/* Resolve a file block index to an image block number. */
static int resolve_file_block(struct filesystem *fs, struct inode *in,
                              uint32_t bi, uint32_t *img_block)
{
    if (bi >= JFS_NDIRECT)
        return JFS_ERR_TOOLARGE;        /* 48 KB file size limit */
    if (in->direct[bi] == JFS_BLOCK_FREE)
        return JFS_ERR_INVAL;

    *img_block = DATA_START + in->direct[bi];
    return JFS_OK;
}

int fs_read_data(struct filesystem *fs, uint32_t ino, void *buf,
                 size_t len, uint64_t offset, size_t *out_len)
{
    if (!fs || !buf || !out_len)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct inode in;
    int rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        goto out;
    if (in.type != JFS_TYPE_FILE) {
        rc = JFS_ERR_ISDIR;
        goto out;
    }

    if (offset >= in.size) {
        *out_len = 0;
        rc = JFS_OK;
        goto out;
    }
    if (offset + len > in.size)
        len = (size_t)(in.size - offset);

    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    uint64_t pos = offset;

    while (done < len) {
        uint32_t bi = (uint32_t)(pos / BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % BLOCK_SIZE);
        uint32_t chunk = BLOCK_SIZE - boff;
        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        uint32_t img_block;
        rc = resolve_file_block(fs, &in, bi, &img_block);
        if (rc != JFS_OK)
            break;

        uint8_t block[BLOCK_SIZE];
        rc = disk_read_block(fs->tm->disk, img_block, block);
        if (rc != JFS_OK)
            break;

        memcpy(out + done, block + boff, chunk);
        done += chunk;
        pos += chunk;
    }

    *out_len = done;
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

int fs_write_data(struct filesystem *fs, uint32_t ino, const void *buf,
                  size_t len, uint64_t offset)
{
    if (!fs || !buf)
        return JFS_ERR_INVAL;

    tx_manager_lock(fs->tm);

    struct inode in;
    int rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        goto out;
    if (in.type != JFS_TYPE_FILE) {
        rc = JFS_ERR_ISDIR;
        goto out;
    }

    /* One transaction covers the block allocations, the payload writes and
     * the inode update.  The payload is written to its data blocks before
     * the metadata is committed, so a crash can never leave the inode
     * referencing data that was not yet written. */
    struct transaction tx;
    rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        goto out;

    uint8_t bitmap[BLOCK_SIZE];
    rc = fs_read_bitmap(fs, bitmap);
    if (rc != JFS_OK)
        goto out;

    const uint8_t *in_buf = (const uint8_t *)buf;
    size_t done = 0;
    uint64_t pos = offset;

    while (done < len) {
        uint32_t bi = (uint32_t)(pos / BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % BLOCK_SIZE);
        uint32_t chunk = BLOCK_SIZE - boff;
        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        if (bi >= JFS_NDIRECT) {
            rc = JFS_ERR_TOOLARGE;      /* 48 KB file size limit */
            goto out;
        }

        /* Determine whether this file block is already allocated. */
        uint32_t blk = in.direct[bi];
        if (blk == JFS_BLOCK_FREE) {
            rc = bitmap_find_free(bitmap, &blk);
            if (rc != JFS_OK)
                goto out;
            bitmap_set(bitmap, blk);
            fs->tm->sb.free_blocks--;
            in.direct[bi] = blk;
            if (bi >= in.block_count)
                in.block_count = bi + 1;
        }

        uint32_t img_block = DATA_START + blk;
        if (boff == 0 && chunk == BLOCK_SIZE) {
            rc = disk_write_block(fs->tm->disk, img_block, in_buf + done);
        } else {
            uint8_t block[BLOCK_SIZE];
            rc = disk_read_block(fs->tm->disk, img_block, block);
            if (rc != JFS_OK)
                goto out;
            memcpy(block + boff, in_buf + done, chunk);
            rc = disk_write_block(fs->tm->disk, img_block, block);
        }
        if (rc != JFS_OK)
            goto out;

        done += chunk;
        pos += chunk;
    }

    rc = tx_stage_update(&tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        goto out;

    uint64_t new_size = offset + len;
    if (new_size > in.size)
        in.size = new_size;
    inode_touch(&in);
    rc = fs_stage_inode(fs, &tx, ino, &in);
    if (rc != JFS_OK)
        goto out;

    rc = tx_commit(&tx);
out:
    tx_manager_unlock(fs->tm);
    return rc;
}

/* Open / close                                                       */

int fs_open(struct filesystem **out, struct disk *d)
{
    if (!out || !d)
        return JFS_ERR_INVAL;

    struct filesystem *fs = (struct filesystem *)calloc(1, sizeof(*fs));
    if (!fs)
        return JFS_ERR_NOMEM;

    int rc = tx_manager_open(&fs->tm, d);
    if (rc != JFS_OK) {
        free(fs);
        return rc;
    }

    /* Run recovery before exposing the filesystem. */
    char report[256];
    rc = tx_recover(fs->tm, report, sizeof(report));
    if (rc != JFS_OK) {
        tx_manager_close(fs->tm);
        free(fs);
        return rc;
    }

    *out = fs;
    return JFS_OK;
}

void fs_close(struct filesystem *fs)
{
    if (!fs)
        return;
    tx_manager_close(fs->tm);
    free(fs);
}
