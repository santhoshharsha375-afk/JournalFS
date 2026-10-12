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

/* ------------------------------------------------------------------ */
/* Low-level block staging helpers                                    */
/* ------------------------------------------------------------------ */

static int stage_whole_block(struct filesystem *fs, struct transaction *tx,
                             uint32_t block, const void *data)
{
    (void)fs;
    return tx_stage_update(tx, block, data);
}

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

int fs_read_inode(struct filesystem *fs, uint32_t ino, struct inode *out)
{
    if (!fs || !out || ino == 0 || ino >= INODE_COUNT)
        return JFS_ERR_INVAL;

    uint32_t table_block = INODE_START + ino / INODES_PER_BLOCK;
    uint8_t buf[BLOCK_SIZE];
    int rc = disk_read_block(fs->tm->disk, table_block, buf);
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
    return stage_whole_block(fs, tx, table_block, buf);
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

int fs_stage_alloc_block(struct filesystem *fs, struct transaction *tx,
                          uint32_t *out)
{
    if (!fs || !tx || !out)
        return JFS_ERR_INVAL;

    uint8_t bitmap[BLOCK_SIZE];
    int rc = stage_block_read(fs, tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    uint32_t free_index = 0;
    int found = 0;
    for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++) {
        if (!bitmap_test(bitmap, i)) {
            free_index = i;
            found = 1;
            break;
        }
    }
    if (!found)
        return JFS_ERR_NOSPC;

    bitmap_set(bitmap, free_index);
    rc = stage_whole_block(fs, tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_blocks--;
    *out = free_index;
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
    rc = stage_whole_block(fs, tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_blocks++;
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* Inode allocation                                                   */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Directory helpers                                                  */
/* ------------------------------------------------------------------ */

int fs_lookup(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t *out_ino)
{
    if (!fs || !name || !out_ino)
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
        rc = disk_read_block(fs->tm->disk, img_block, block);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0)
                continue;
            if (de->name_len == strlen(name) &&
                memcmp(de->name, name, strlen(name)) == 0) {
                *out_ino = de->ino;
                return JFS_OK;
            }
        }
    }
    return JFS_ERR_NOENT;
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

    /* Look for a free slot in the existing directory blocks. */
    for (uint32_t b = 0; b < dir.block_count && !found; b++) {
        uint32_t img_block = DATA_START + dir.direct[b];
        rc = disk_read_block(fs->tm->disk, img_block, block);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0) {
                de->ino = ino;
                de->type = type;
                de->name_len = (uint16_t)name_len;
                de->flags = 0;
                memset(de->name, 0, JFS_NAME_MAX);
                memcpy(de->name, name, name_len);
                found = 1;
                break;
            }
        }
        if (found) {
            rc = stage_whole_block(fs, tx, DATA_START + dir.direct[b], block);
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
        struct dirent *de = (struct dirent *)block;
        de->ino = ino;
        de->type = type;
        de->name_len = (uint16_t)name_len;
        de->flags = 0;
        memcpy(de->name, name, name_len);

        rc = stage_whole_block(fs, tx, DATA_START + new_block, block);
        if (rc != JFS_OK)
            return rc;

        /* Link the new block into the directory inode. */
        if (dir.block_count < JFS_NDIRECT) {
            dir.direct[dir.block_count] = new_block;
        } else {
            /* Indirect block handling. */
            if (dir.block_count == JFS_NDIRECT) {
                uint32_t ind;
                rc = fs_stage_alloc_block(fs, tx, &ind);
                if (rc != JFS_OK)
                    return rc;
                uint8_t ind_block[BLOCK_SIZE];
                memset(ind_block, 0, sizeof(ind_block));
                uint32_t *entries = (uint32_t *)ind_block;
                for (uint32_t k = 0; k < JFS_NDIRECT; k++)
                    entries[k] = dir.direct[k];
                entries[JFS_NDIRECT] = new_block;
                rc = stage_whole_block(fs, tx, DATA_START + ind, ind_block);
                if (rc != JFS_OK)
                    return rc;
                dir.indirect = ind;
            } else {
                uint8_t ind_block[BLOCK_SIZE];
                uint32_t ind_img = DATA_START + dir.indirect;
                rc = disk_read_block(fs->tm->disk, ind_img, ind_block);
                if (rc != JFS_OK)
                    return rc;
                uint32_t *entries = (uint32_t *)ind_block;
                entries[dir.block_count - JFS_NDIRECT] = new_block;
                rc = stage_whole_block(fs, tx, ind_img, ind_block);
                if (rc != JFS_OK)
                    return rc;
            }
        }
        dir.block_count++;
    }

    dir.size += (uint64_t)sizeof(struct dirent);
    dir.mtime = (uint64_t)time(NULL);
    dir.ctime = dir.mtime;
    inode_update_crc(&dir);
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
        rc = disk_read_block(fs->tm->disk, img_block, block);
        if (rc != JFS_OK)
            return rc;

        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; i++) {
            struct dirent *de = (struct dirent *)(block + i * sizeof(*de));
            if (de->ino == 0)
                continue;
            if (de->name_len == strlen(name) &&
                memcmp(de->name, name, strlen(name)) == 0) {
                memset(de, 0, sizeof(*de));
                rc = stage_whole_block(fs, tx, img_block, block);
                if (rc != JFS_OK)
                    return rc;

                dir.size -= (uint64_t)sizeof(struct dirent);
                dir.mtime = (uint64_t)time(NULL);
                dir.ctime = dir.mtime;
                inode_update_crc(&dir);
                rc = fs_stage_inode(fs, tx, dir_ino, &dir);
                if (rc != JFS_OK)
                    return rc;
                return JFS_OK;
            }
        }
    }
    return JFS_ERR_NOENT;
}

/* ------------------------------------------------------------------ */
/* High-level operations                                              */
/* ------------------------------------------------------------------ */

int fs_create(struct filesystem *fs, uint32_t dir_ino, const char *name,
              uint32_t type, uint32_t mode, uint32_t *out_ino)
{
    if (!fs || !name || !out_ino)
        return JFS_ERR_INVAL;

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    uint32_t ino;
    rc = fs_alloc_inode(fs, &tx, type, mode, &ino);
    if (rc != JFS_OK)
        return rc;

    rc = fs_dir_add(fs, &tx, dir_ino, name, ino, type);
    if (rc != JFS_OK)
        return rc;

    rc = tx_commit(&tx);
    if (rc != JFS_OK)
        return rc;

    *out_ino = ino;
    return JFS_OK;
}

int fs_unlink(struct filesystem *fs, uint32_t dir_ino, const char *name)
{
    if (!fs || !name)
        return JFS_ERR_INVAL;

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    uint32_t ino;
    rc = fs_lookup(fs, dir_ino, name, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;
    if (in.type != JFS_TYPE_FILE)
        return JFS_ERR_ISDIR;

    rc = fs_dir_remove(fs, &tx, dir_ino, name);
    if (rc != JFS_OK)
        return rc;

    /* Free the inode's data blocks. */
    for (uint32_t b = 0; b < in.block_count; b++) {
        rc = fs_stage_free_block(fs, &tx, in.direct[b]);
        if (rc != JFS_OK)
            return rc;
    }
    if (in.indirect != JFS_BLOCK_FREE) {
        uint8_t ind_block[BLOCK_SIZE];
        rc = disk_read_block(fs->tm->disk, DATA_START + in.indirect, ind_block);
        if (rc != JFS_OK)
            return rc;
        uint32_t *entries = (uint32_t *)ind_block;
        for (uint32_t k = 0; k < JFS_INDIRECT_BLOCKS; k++) {
            if (entries[k] == JFS_BLOCK_FREE)
                break;
            rc = fs_stage_free_block(fs, &tx, entries[k]);
            if (rc != JFS_OK)
                return rc;
        }
        rc = fs_stage_free_block(fs, &tx, in.indirect);
        if (rc != JFS_OK)
            return rc;
    }

    /* Mark the inode free. */
    struct inode free_in;
    memset(&free_in, 0, sizeof(free_in));
    rc = fs_stage_inode(fs, &tx, ino, &free_in);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_inodes++;
    return tx_commit(&tx);
}

int fs_rmdir(struct filesystem *fs, uint32_t dir_ino, const char *name)
{
    if (!fs || !name)
        return JFS_ERR_INVAL;

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    uint32_t ino;
    rc = fs_lookup(fs, dir_ino, name, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;
    if (in.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;

    /* Refuse to remove a non-empty directory. */
    if (in.size > (uint64_t)(2 * sizeof(struct dirent)))
        return JFS_ERR_NOTEMPTY;

    rc = fs_dir_remove(fs, &tx, dir_ino, name);
    if (rc != JFS_OK)
        return rc;

    /* Free the directory's data blocks. */
    for (uint32_t b = 0; b < in.block_count; b++) {
        rc = fs_stage_free_block(fs, &tx, in.direct[b]);
        if (rc != JFS_OK)
            return rc;
    }
    if (in.indirect != JFS_BLOCK_FREE) {
        rc = fs_stage_free_block(fs, &tx, in.indirect);
        if (rc != JFS_OK)
            return rc;
    }

    struct inode free_in;
    memset(&free_in, 0, sizeof(free_in));
    rc = fs_stage_inode(fs, &tx, ino, &free_in);
    if (rc != JFS_OK)
        return rc;

    fs->tm->sb.free_inodes++;
    return tx_commit(&tx);
}

int fs_rename(struct filesystem *fs, uint32_t old_parent, uint32_t new_parent,
              const char *old_name, const char *new_name)
{
    if (!fs || !old_name || !new_name)
        return JFS_ERR_INVAL;

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    uint32_t ino;
    rc = fs_lookup(fs, old_parent, old_name, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;

    rc = fs_dir_remove(fs, &tx, old_parent, old_name);
    if (rc != JFS_OK)
        return rc;

    rc = fs_dir_add(fs, &tx, new_parent, new_name, ino, in.type);
    if (rc != JFS_OK)
        return rc;

    return tx_commit(&tx);
}

int fs_truncate(struct filesystem *fs, uint32_t ino, uint64_t size)
{
    if (!fs)
        return JFS_ERR_INVAL;

    struct transaction tx;
    int rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;

    uint32_t new_blocks = (uint32_t)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (new_blocks > in.block_count) {
        /* Allocate additional blocks. */
        for (uint32_t b = in.block_count; b < new_blocks; b++) {
            uint32_t blk;
            rc = fs_stage_alloc_block(fs, &tx, &blk);
            if (rc != JFS_OK)
                return rc;
            if (b < JFS_NDIRECT) {
                in.direct[b] = blk;
            } else {
                /* Indirect block handling. */
                if (b == JFS_NDIRECT) {
                    uint32_t ind;
                    rc = fs_stage_alloc_block(fs, &tx, &ind);
                    if (rc != JFS_OK)
                        return rc;
                    uint8_t ind_block[BLOCK_SIZE];
                    memset(ind_block, 0, sizeof(ind_block));
                    uint32_t *entries = (uint32_t *)ind_block;
                    for (uint32_t k = 0; k < JFS_NDIRECT; k++)
                        entries[k] = in.direct[k];
                    entries[JFS_NDIRECT] = blk;
                    rc = stage_whole_block(fs, &tx, DATA_START + ind, ind_block);
                    if (rc != JFS_OK)
                        return rc;
                    in.indirect = ind;
                } else {
                    uint8_t ind_block[BLOCK_SIZE];
                    uint32_t ind_img = DATA_START + in.indirect;
                    rc = disk_read_block(fs->tm->disk, ind_img, ind_block);
                    if (rc != JFS_OK)
                        return rc;
                    uint32_t *entries = (uint32_t *)ind_block;
                    entries[b - JFS_NDIRECT] = blk;
                    rc = stage_whole_block(fs, &tx, ind_img, ind_block);
                    if (rc != JFS_OK)
                        return rc;
                }
            }
        }
    } else if (new_blocks < in.block_count) {
        /* Free surplus blocks. */
        for (uint32_t b = new_blocks; b < in.block_count; b++) {
            uint32_t blk;
            if (b < JFS_NDIRECT) {
                blk = in.direct[b];
                in.direct[b] = JFS_BLOCK_FREE;
            } else {
                uint8_t ind_block[BLOCK_SIZE];
                uint32_t ind_img = DATA_START + in.indirect;
                rc = stage_block_read(fs, &tx, ind_img, ind_block);
                if (rc != JFS_OK)
                    return rc;
                uint32_t *entries = (uint32_t *)ind_block;
                blk = entries[b - JFS_NDIRECT];
                entries[b - JFS_NDIRECT] = JFS_BLOCK_FREE;
                rc = stage_whole_block(fs, &tx, ind_img, ind_block);
                if (rc != JFS_OK)
                    return rc;
            }
            rc = fs_stage_free_block(fs, &tx, blk);
            if (rc != JFS_OK)
                return rc;
        }
    }

    in.block_count = new_blocks;
    in.size = size;
    in.mtime = (uint64_t)time(NULL);
    in.ctime = in.mtime;
    inode_update_crc(&in);
    rc = fs_stage_inode(fs, &tx, ino, &in);
    if (rc != JFS_OK)
        return rc;

    return tx_commit(&tx);
}

/* ------------------------------------------------------------------ */
/* Data I/O                                                           */
/* ------------------------------------------------------------------ */

/* Resolve a file block index to an image block number. */
static int resolve_file_block(struct filesystem *fs, struct inode *in,
                              uint32_t bi, uint32_t *img_block)
{
    if (bi < JFS_NDIRECT) {
        if (in->direct[bi] == JFS_BLOCK_FREE)
            return JFS_ERR_INVAL;
        *img_block = DATA_START + in->direct[bi];
        return JFS_OK;
    }
    uint32_t idx = bi - JFS_NDIRECT;
    if (idx >= JFS_INDIRECT_BLOCKS)
        return JFS_ERR_TOOLARGE;
    if (in->indirect == JFS_BLOCK_FREE)
        return JFS_ERR_INVAL;

    uint8_t ind_block[BLOCK_SIZE];
    int rc = disk_read_block(fs->tm->disk, DATA_START + in->indirect, ind_block);
    if (rc != JFS_OK)
        return rc;
    uint32_t *entries = (uint32_t *)ind_block;
    if (entries[idx] == JFS_BLOCK_FREE)
        return JFS_ERR_INVAL;
    *img_block = DATA_START + entries[idx];
    return JFS_OK;
}

int fs_read_data(struct filesystem *fs, uint32_t ino, void *buf,
                 size_t len, uint64_t offset, size_t *out_len)
{
    if (!fs || !buf || !out_len)
        return JFS_ERR_INVAL;

    struct inode in;
    int rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;
    if (in.type != JFS_TYPE_FILE)
        return JFS_ERR_ISDIR;

    if (offset >= in.size) {
        *out_len = 0;
        return JFS_OK;
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
    return JFS_OK;
}

int fs_write_data(struct filesystem *fs, uint32_t ino, const void *buf,
                  size_t len, uint64_t offset)
{
    if (!fs || !buf)
        return JFS_ERR_INVAL;

    struct inode in;
    int rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;
    if (in.type != JFS_TYPE_FILE)
        return JFS_ERR_ISDIR;

    /* One transaction covers the block allocations, the payload writes and
     * the inode update.  The payload is written to its data blocks before
     * the metadata is committed, so a crash can never leave the inode
     * referencing data that was not yet written. */
    struct transaction tx;
    rc = tx_begin(fs->tm, &tx);
    if (rc != JFS_OK)
        return rc;

    uint8_t bitmap[BLOCK_SIZE];
    rc = fs_read_bitmap(fs, bitmap);
    if (rc != JFS_OK)
        return rc;

    uint8_t ind_block[BLOCK_SIZE];
    memset(ind_block, 0, sizeof(ind_block));
    int have_ind = 0;
    if (in.indirect != JFS_BLOCK_FREE) {
        rc = disk_read_block(fs->tm->disk, DATA_START + in.indirect, ind_block);
        if (rc != JFS_OK)
            return rc;
        have_ind = 1;
    }

    const uint8_t *in_buf = (const uint8_t *)buf;
    size_t done = 0;
    uint64_t pos = offset;

    while (done < len) {
        uint32_t bi = (uint32_t)(pos / BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % BLOCK_SIZE);
        uint32_t chunk = BLOCK_SIZE - boff;
        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        /* Determine whether this file block is already allocated. */
        uint32_t blk = 0;
        int need_alloc = 0;
        if (bi < JFS_NDIRECT) {
            blk = in.direct[bi];
            need_alloc = (blk == JFS_BLOCK_FREE);
        } else {
            uint32_t idx = bi - JFS_NDIRECT;
            if (idx >= JFS_INDIRECT_BLOCKS)
                return JFS_ERR_TOOLARGE;
            if (in.indirect == JFS_BLOCK_FREE) {
                need_alloc = 1;
            } else {
                blk = ((uint32_t *)ind_block)[idx];
                need_alloc = (blk == JFS_BLOCK_FREE);
            }
        }

        if (need_alloc) {
            uint32_t free_index = 0;
            int found = 0;
            for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++) {
                if (!bitmap_test(bitmap, i)) {
                    free_index = i;
                    found = 1;
                    break;
                }
            }
            if (!found)
                return JFS_ERR_NOSPC;
            bitmap_set(bitmap, free_index);
            blk = free_index;
            fs->tm->sb.free_blocks--;

                if (bi < JFS_NDIRECT) {
                    in.direct[bi] = blk;
                } else {
                    if (in.indirect == JFS_BLOCK_FREE) {
                        /* Allocate the indirect block itself. */
                    uint32_t ind = 0;
                    int ifound = 0;
                    for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++) {
                        if (!bitmap_test(bitmap, i)) {
                            ind = i;
                            ifound = 1;
                            break;
                        }
                    }
                    if (!ifound)
                        return JFS_ERR_NOSPC;
                    bitmap_set(bitmap, ind);
                    in.indirect = ind;
                    memset(ind_block, 0, sizeof(ind_block));
                    have_ind = 1;
                    fs->tm->sb.free_blocks--;
                }
                ((uint32_t *)ind_block)[bi - JFS_NDIRECT] = blk;
            }
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
                return rc;
            memcpy(block + boff, in_buf + done, chunk);
            rc = disk_write_block(fs->tm->disk, img_block, block);
        }
        if (rc != JFS_OK)
            return rc;

        done += chunk;
        pos += chunk;
    }

    rc = stage_whole_block(fs, &tx, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        return rc;
    if (have_ind) {
        rc = stage_whole_block(fs, &tx, DATA_START + in.indirect, ind_block);
        if (rc != JFS_OK)
            return rc;
    }

    uint64_t new_size = offset + len;
    if (new_size > in.size)
        in.size = new_size;
    in.mtime = (uint64_t)time(NULL);
    in.ctime = in.mtime;
    inode_update_crc(&in);
    rc = fs_stage_inode(fs, &tx, ino, &in);
    if (rc != JFS_OK)
        return rc;

    return tx_commit(&tx);
}

/* ------------------------------------------------------------------ */
/* Open / close                                                       */
/* ------------------------------------------------------------------ */

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
