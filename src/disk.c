/*
 * disk.c -- Raw disk-image I/O.
 *
 * A short read or short write is always an error: we never let a partially
 * transferred block reach the rest of the system, because a torn metadata
 * block is exactly the corruption the journal is supposed to protect against.
 */
#include "disk.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int read_all(int fd, void *buf, size_t len, uint64_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return JFS_ERR_IO;
        }
        if (n == 0)
            return JFS_ERR_IO;   /* unexpected EOF */
        p += n;
        len -= (size_t)n;
        offset += (uint64_t)n;
    }
    return JFS_OK;
}

static int write_all(int fd, const void *buf, size_t len, uint64_t offset)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return JFS_ERR_IO;
        }
        if (n == 0)
            return JFS_ERR_IO;
        p += n;
        len -= (size_t)n;
        offset += (uint64_t)n;
    }
    return JFS_OK;
}

int disk_open(struct disk **out, const char *path)
{
    if (!out || !path)
        return JFS_ERR_INVAL;

    struct disk *d = (struct disk *)calloc(1, sizeof(*d));
    if (!d)
        return JFS_ERR_NOMEM;

    d->fd = -1;
    d->path = strdup(path);
    if (!d->path) {
        free(d);
        return JFS_ERR_NOMEM;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        free(d->path);
        free(d);
        return JFS_ERR_IO;
    }
    d->fd = fd;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        free(d->path);
        free(d);
        return JFS_ERR_IO;
    }
    d->size = (uint64_t)st.st_size;

    *out = d;
    return JFS_OK;
}

int disk_open_existing(struct disk **out, const char *path)
{
    struct disk *d = NULL;
    int rc = disk_open(&d, path);
    if (rc != JFS_OK)
        return rc;
    if (d->size != IMAGE_SIZE) {
        disk_close(d);
        return JFS_ERR_FORMAT;
    }
    *out = d;
    return JFS_OK;
}

int disk_create(const char *path)
{
    if (!path)
        return JFS_ERR_INVAL;

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            return JFS_ERR_EXIST;
        return JFS_ERR_IO;
    }

    /* Write the full image size so the file is the right length. */
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));

    /* Write the last byte via ftruncate is cheaper and safe here because the
     * image is created empty and immediately formatted by mkfs. */
    if (ftruncate(fd, (off_t)IMAGE_SIZE) != 0) {
        close(fd);
        return JFS_ERR_IO;
    }
    (void)zero;
    close(fd);
    return JFS_OK;
}

void disk_close(struct disk *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d->path);
    free(d);
}

int disk_read_block(struct disk *d, uint32_t block, void *out)
{
    if (!d || !out)
        return JFS_ERR_INVAL;
    if (block >= TOTAL_BLOCKS)
        return JFS_ERR_INVAL;
    return read_all(d->fd, out, BLOCK_SIZE, (uint64_t)block * BLOCK_SIZE);
}

int disk_write_block(struct disk *d, uint32_t block, const void *in)
{
    if (!d || !in)
        return JFS_ERR_INVAL;
    if (d->read_only)
        return JFS_ERR_FAULT;
    if (block >= TOTAL_BLOCKS)
        return JFS_ERR_INVAL;
    return write_all(d->fd, in, BLOCK_SIZE, (uint64_t)block * BLOCK_SIZE);
}

int disk_pread(struct disk *d, void *buf, size_t len, uint64_t offset)
{
    if (!d || !buf)
        return JFS_ERR_INVAL;
    return read_all(d->fd, buf, len, offset);
}

int disk_pwrite(struct disk *d, const void *buf, size_t len, uint64_t offset)
{
    if (!d || !buf)
        return JFS_ERR_INVAL;
    if (d->read_only)
        return JFS_ERR_FAULT;
    return write_all(d->fd, buf, len, offset);
}

int disk_read_superblock(struct disk *d, struct superblock *sb)
{
    if (!d || !sb)
        return JFS_ERR_INVAL;
    int rc = disk_read_block(d, SUPERBLOCK_BLOCK, sb);
    if (rc != JFS_OK)
        return rc;
    if (sb->magic != JFS_MAGIC)
        return JFS_ERR_FORMAT;
    if (sb->version != JFS_VERSION)
        return JFS_ERR_FORMAT;
    if (sb->block_size != BLOCK_SIZE)
        return JFS_ERR_FORMAT;

    uint32_t stored = sb->crc;
    uint32_t computed = jfs_crc32(sb, offsetof(struct superblock, crc));
    if (stored != computed)
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}

int disk_write_superblock(struct disk *d, const struct superblock *sb)
{
    if (!d || !sb)
        return JFS_ERR_INVAL;

    struct superblock tmp = *sb;
    tmp.crc = jfs_crc32(&tmp, offsetof(struct superblock, crc));
    return disk_write_block(d, SUPERBLOCK_BLOCK, &tmp);
}

int disk_fsync(struct disk *d)
{
    if (!d)
        return JFS_ERR_INVAL;
    if (fsync(d->fd) != 0)
        return JFS_ERR_IO;
    return JFS_OK;
}

uint64_t disk_size(const struct disk *d)
{
    return d ? d->size : 0;
}
