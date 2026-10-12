/*
 * mkfs_journalfs.c -- Format a new JournalFS image.
 *
 * Usage: mkfs_journalfs <image-path>
 *
 * Creates the image file if it does not exist, then writes a valid
 * superblock, an empty data-block bitmap, an inode table containing only
 * the root directory, and a root directory data block holding "." and "..".
 *
 * This tool never touches an existing image: it refuses to run if the file
 * is already present, so a real filesystem is never silently reformatted.
 */
#include "journalfs.h"
#include "disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void set_bit(uint8_t *bitmap, uint32_t index)
{
    bitmap[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

static int build_superblock(struct superblock *sb)
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
    sb->free_blocks = DATA_BLOCK_COUNT - 1;   /* root dir uses one block */
    sb->free_inodes = INODE_COUNT - 1;       /* root inode is used */
    sb->journal_head = 0;
    sb->journal_tail = 0;
    sb->last_txid = 0;
    sb->last_committed_txid = 0;
    sb->last_checkpointed_txid = 0;
    sb->journal_generation = 1;
    sb->flags = 0;
    return JFS_OK;
}

static int build_bitmap(uint8_t *bitmap)
{
    memset(bitmap, 0, BLOCK_SIZE);
    /* Data block 0 (image block DATA_START) holds the root directory. */
    set_bit(bitmap, 0);
    return JFS_OK;
}

static int build_root_inode(struct inode *in)
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
    in->direct[0] = 0;      /* data block index 0 */
    in->indirect = 0;
    in->crc = jfs_crc32(in, offsetof(struct inode, crc));
    return JFS_OK;
}

static int build_root_dir(uint8_t *block)
{
    memset(block, 0, BLOCK_SIZE);

    struct dirent *dot = (struct dirent *)(block + 0);
    dot->ino = JFS_ROOT_INODE;
    dot->type = JFS_TYPE_DIR;
    dot->name_len = 1;
    dot->flags = 0;
    memcpy(dot->name, ".", 2);

    struct dirent *dotdot = (struct dirent *)(block + sizeof(struct dirent));
    dotdot->ino = JFS_ROOT_INODE;
    dotdot->type = JFS_TYPE_DIR;
    dotdot->name_len = 2;
    dotdot->flags = 0;
    memcpy(dotdot->name, "..", 3);

    return JFS_OK;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-path>\n", argv[0]);
        return 2;
    }

    const char *path = argv[1];

    int rc = disk_create(path);
    if (rc == JFS_ERR_EXIST) {
        fprintf(stderr, "mkfs_journalfs: '%s' already exists; refusing to "
                        "overwrite an existing image\n", path);
        return 1;
    }
    if (rc != JFS_OK) {
        fprintf(stderr, "mkfs_journalfs: cannot create '%s' (%d)\n", path, rc);
        return 1;
    }

    struct disk *d = NULL;
    rc = disk_open(&d, path);
    if (rc != JFS_OK) {
        fprintf(stderr, "mkfs_journalfs: cannot open '%s' (%d)\n", path, rc);
        return 1;
    }

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
    printf("mkfs_journalfs: created %s (%u blocks, %u data blocks, "
           "%u inodes, %u journal slots)\n",
           path, TOTAL_BLOCKS, DATA_BLOCK_COUNT, INODE_COUNT, JOURNAL_SLOTS);
    return 0;

fail:
    fprintf(stderr, "mkfs_journalfs: format failed (%d)\n", rc);
    disk_close(d);
    return 1;
}
