/*
 * mkfs_journalfs.c -- Format a new JournalFS image.
 *
 * Usage: mkfs_journalfs <image-path>
 *
 * Refuses to overwrite an existing image, so a real filesystem is never
 * silently reformatted.
 */
#include "journalfs.h"
#include "disk.h"

#include <stdio.h>

int mkfs_format(const char *path);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-path>\n", argv[0]);
        return 2;
    }

    int rc = mkfs_format(argv[1]);
    if (rc == JFS_ERR_EXIST) {
        fprintf(stderr, "mkfs_journalfs: '%s' already exists; refusing to "
                        "overwrite an existing image\n", argv[1]);
        return 1;
    }
    if (rc != JFS_OK) {
        fprintf(stderr, "mkfs_journalfs: format failed (%d)\n", rc);
        return 1;
    }

    printf("mkfs_journalfs: created %s (%u blocks, %u data blocks, "
           "%u inodes, %u journal slots)\n",
           argv[1], TOTAL_BLOCKS, DATA_BLOCK_COUNT, INODE_COUNT, JOURNAL_SLOTS);
    return 0;
}
