/*
 * mkfs.c -- Image formatting logic, shared by the mkfs tool and the CLI.
 */
#include "journalfs.h"
#include "disk.h"

#include <string.h>
#include <time.h>

static void set_bit(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

static void build_superblock(struct superblock *sb)
{
    memset(sb, 0, sizeof(*sb));
    sb->magic = JFS_MAGIC;
    sb->version = JFS_VERSION;
    sb->block_size = BLOCK_SIZE;
    sb->total_blocks = TOTAL_BLOCKS;
    sb->data_block_count = DATA_BLOCK_COUNT;
    sb->inode_count = INODE_COUNT;
    sb->bitmap_start = BITMAP_START;
    sb->bitmap_blocks = BITMAP_BLOCKS;
    sb->inode_start = INODE_START;
    sb->inode_blocks = INODE_BLOCKS;
    sb->journal_start = JOURNAL_START;
    sb->journal_blocks = JOURNAL_BLOCKS;
    sb->data_start = DATA_START;
    sb->root_inode = JFS_ROOT_INODE;
    sb->free_blocks = DATA_BLOCK_COUNT - 1;
    sb->free_inodes = INODE_COUNT - 1;
    sb->journal_head = 0;
    sb->journal_tail = 0;
    sb->last_txid = 0;
    sb->last_committed_txid = 0;
    sb->last_checkpointed_txid = 0;
    sb->journal_generation = 1;
    sb->flags = 0;
}

static void build_bitmap(uint8_t *bitmap)
{
    memset(bitmap, 0, BLOCK_SIZE);
    set_bit(bitmap, 0);   /* data block 0 holds the root directory */
}

static void build_root_inode(struct inode *in)
{
    memset(in, 0, sizeof(*in));
    in->magic = JFS_INODE_MAGIC;
    in->ino = JFS_ROOT_INODE;
    in->type = JFS_TYPE_DIR;
    in->mode = 0755;
    in->uid = 0;
    in->gid = 0;
    in->nlink = 2;
    in->size = (uint64_t)(2 * sizeof(struct dirent));
    in->atime = in->mtime = in->ctime = (uint64_t)time(NULL);
    in->block_count = 1;
    in->direct[0] = 0;
    in->indirect = JFS_BLOCK_FREE;
    in->crc = jfs_crc32(in, offsetof(struct inode, crc));
}

static void build_root_dir(uint8_t *block)
{
    memset(block, 0, BLOCK_SIZE);

    struct dirent *dot = (struct dirent *)block;
    dot->ino = JFS_ROOT_INODE;
    dot->type = JFS_TYPE_DIR;
    dot->name_len = 1;
    memcpy(dot->name, ".", 2);

    struct dirent *dotdot = (struct dirent *)(block + sizeof(struct dirent));
    dotdot->ino = JFS_ROOT_INODE;
    dotdot->type = JFS_TYPE_DIR;
    dotdot->name_len = 2;
    memcpy(dotdot->name, "..", 3);
}

int mkfs_format(const char *path)
{
    if (!path)
        return JFS_ERR_INVAL;

    int rc = disk_create(path);
    if (rc == JFS_ERR_EXIST)
        return JFS_ERR_EXIST;
    if (rc != JFS_OK)
        return rc;

    struct disk *d = NULL;
    rc = disk_open(&d, path);
    if (rc != JFS_OK)
        return rc;

    struct superblock sb;
    build_superblock(&sb);
    rc = disk_write_superblock(d, &sb);
    if (rc != JFS_OK)
        goto fail;

    uint8_t bitmap[BLOCK_SIZE];
    build_bitmap(bitmap);
    rc = disk_write_block(d, BITMAP_START, bitmap);
    if (rc != JFS_OK)
        goto fail;

    uint8_t inode_block[BLOCK_SIZE];
    memset(inode_block, 0, sizeof(inode_block));
    struct inode root;
    build_root_inode(&root);
    memcpy(inode_block + (JFS_ROOT_INODE % INODES_PER_BLOCK) * INODE_SIZE,
           &root, INODE_SIZE);
    rc = disk_write_block(d, INODE_START, inode_block);
    if (rc != JFS_OK)
        goto fail;

    uint8_t dir_block[BLOCK_SIZE];
    build_root_dir(dir_block);
    rc = disk_write_block(d, DATA_START, dir_block);
    if (rc != JFS_OK)
        goto fail;

    rc = disk_fsync(d);
    if (rc != JFS_OK)
        goto fail;

    disk_close(d);
    return JFS_OK;

fail:
    disk_close(d);
    return rc;
}
