/*
 * disk.h -- Raw disk-image I/O layer.
 *
 * The "disk" is a single regular file.  All access goes through pread /
 * pwrite so that a partial write never tears a block.  Every public
 * function checks for short reads/writes and reports JFS_ERR_IO instead
 * of silently continuing.
 */
#ifndef JFS_DISK_H
#define JFS_DISK_H

#include "journalfs.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A handle to an open disk image. */
struct disk {
    int      fd;          /* file descriptor                   */
    uint64_t size;        /* size of the image in bytes        */
    char    *path;        /* owned copy of the path            */
    int      read_only;   /* 1 => refuse writes                */
};

/* Open an existing image for read/write.  Returns JFS_OK or JFS_ERR_IO. */
int disk_open(struct disk **out, const char *path);

/* Open without creating; fails if the image does not exist. */
int disk_open_existing(struct disk **out, const char *path);

/* Create a new image of IMAGE_SIZE bytes, failing if it already exists. */
int disk_create(const char *path);

void disk_close(struct disk *d);

/* Read exactly one block.  *out is filled with BLOCK_SIZE bytes. */
int disk_read_block(struct disk *d, uint32_t block, void *out);

/* Write exactly one block. */
int disk_write_block(struct disk *d, uint32_t block, const void *in);

/* Read/write an arbitrary byte range (used for the journal ring). */
int disk_pread(struct disk *d, void *buf, size_t len, uint64_t offset);
int disk_pwrite(struct disk *d, const void *buf, size_t len, uint64_t offset);

/* Read the superblock, verifying magic, version and CRC. */
int disk_read_superblock(struct disk *d, struct superblock *sb);

/* Write the superblock, computing and storing its CRC. */
int disk_write_superblock(struct disk *d, const struct superblock *sb);

/* Force the image to stable storage. */
int disk_fsync(struct disk *d);

uint64_t disk_size(const struct disk *d);

#ifdef __cplusplus
}
#endif

#endif /* JFS_DISK_H */
