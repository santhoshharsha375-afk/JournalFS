/*
 * fuse_ops.c -- FUSE 3 operations for JournalFS.
 *
 * Each callback resolves its path through the filesystem layer and performs
 * the corresponding operation.  Metadata-modifying callbacks go through the
 * transaction manager, so they are journaled and recoverable; the write
 * callback writes payload first and commits the metadata afterwards.
 *
 * This file is only compiled when libfuse3 is available (HAVE_FUSE).
 */
#include "fuse_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

static struct filesystem *g_fs;

/* Map internal JFS_ERR_* codes to POSIX errno values. */
static int jfs_errno(int rc)
{
    switch (rc) {
    case JFS_OK:           return 0;
    case JFS_ERR_IO:       return -EIO;
    case JFS_ERR_NOSPC:    return -ENOSPC;
    case JFS_ERR_NOENT:    return -ENOENT;
    case JFS_ERR_EXIST:    return -EEXIST;
    case JFS_ERR_NOTEMPTY: return -ENOTEMPTY;
    case JFS_ERR_NOTDIR:   return -ENOTDIR;
    case JFS_ERR_ISDIR:    return -EISDIR;
    case JFS_ERR_INVAL:    return -EINVAL;
    case JFS_ERR_CORRUPT:  return -EIO;
    case JFS_ERR_FORMAT:   return -EIO;
    case JFS_ERR_BUSY:     return -EBUSY;
    case JFS_ERR_NOMEM:    return -ENOMEM;
    case JFS_ERR_TOOLARGE: return -EFBIG;
    case JFS_ERR_BADF:     return -EBADF;
    case JFS_ERR_FAULT:    return -EFAULT;
    default:               return -EIO;
    }
}

/* Resolve a full path ("/" or "/a/b") to its inode number. */
static int path_to_ino(const char *path, uint32_t *ino)
{
    if (!g_fs || !path || !ino)
        return -EINVAL;
    return jfs_errno(fs_resolve_path(g_fs, path, ino));
}

/* Split "/a/b" into the parent directory inode and the final name. */
static int split_parent(const char *path, uint32_t *parent, char *name)
{
    if (!g_fs)
        return -EINVAL;
    return jfs_errno(fs_resolve_parent(g_fs, path, parent, name,
                                       JFS_NAME_MAX + 1));
}

static void inode_to_stat(const struct inode *in, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = in->ino;
    st->st_mode = in->mode;
    if (in->type == JFS_TYPE_DIR)
        st->st_mode |= S_IFDIR;
    else
        st->st_mode |= S_IFREG;
    st->st_nlink = in->nlink;
    st->st_size = (off_t)in->size;
    st->st_mtime = (time_t)in->mtime;
    st->st_atime = (time_t)in->atime;
    st->st_ctime = (time_t)in->ctime;
}

static int jfs_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;

    struct inode in;
    if (fs_read_inode(g_fs, ino, &in) != JFS_OK)
        return -EIO;
    inode_to_stat(&in, st);
    return 0;
}

static int jfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)flags;
    (void)fi;

    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;

    struct inode in;
    if (fs_read_inode(g_fs, ino, &in) != JFS_OK)
        return -EIO;
    if (in.type != JFS_TYPE_DIR)
        return -ENOTDIR;

    if (filler(buf, ".", NULL, 0, 0) != 0)
        return -ENOMEM;
    if (filler(buf, "..", NULL, 0, 0) != 0)
        return -ENOMEM;

    uint8_t block[BLOCK_SIZE];
    for (uint32_t b = 0; b < in.block_count; b++) {
        if (disk_read_block(g_fs->tm->disk, DATA_START + in.direct[b],
                            block) != JFS_OK)
            return -EIO;
        for (uint32_t e = 0; e < DIRENTS_PER_BLOCK; e++) {
            struct dirent *de = (struct dirent *)(block + e * sizeof(*de));
            if (de->ino == 0)
                continue;
            if (filler(buf, de->name, NULL, 0, 0) != 0)
                return -ENOMEM;
        }
    }
    return 0;
}

static int jfs_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = split_parent(path, &parent, name);
    if (rc != 0)
        return rc;
    return jfs_errno(fs_create(g_fs, parent, name, JFS_TYPE_FILE,
                               mode & 07777, &(uint32_t){0}));
}

static int jfs_mkdir(const char *path, mode_t mode)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = split_parent(path, &parent, name);
    if (rc != 0)
        return rc;
    return jfs_errno(fs_create(g_fs, parent, name, JFS_TYPE_DIR,
                               mode & 07777, &(uint32_t){0}));
}

static int jfs_unlink(const char *path)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = split_parent(path, &parent, name);
    if (rc != 0)
        return rc;
    return jfs_errno(fs_unlink(g_fs, parent, name));
}

static int jfs_rmdir(const char *path)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = split_parent(path, &parent, name);
    if (rc != 0)
        return rc;
    return jfs_errno(fs_rmdir(g_fs, parent, name));
}

static int jfs_rename(const char *from, const char *to, unsigned int flags)
{
    (void)flags;
    uint32_t from_parent, to_parent;
    char from_name[JFS_NAME_MAX + 1], to_name[JFS_NAME_MAX + 1];

    int rc = split_parent(from, &from_parent, from_name);
    if (rc != 0)
        return rc;
    rc = split_parent(to, &to_parent, to_name);
    if (rc != 0)
        return rc;

    return jfs_errno(fs_rename(g_fs, from_parent, to_parent,
                               from_name, to_name));
}

static int jfs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;
    return jfs_errno(fs_truncate(g_fs, ino, (uint64_t)size));
}

static int jfs_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    return path_to_ino(path, &ino);
}

static int jfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;

    size_t got = 0;
    rc = fs_read_data(g_fs, ino, buf, size, (uint64_t)offset, &got);
    if (rc != JFS_OK)
        return jfs_errno(rc);
    return (int)got;
}

static int jfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;

    rc = fs_write_data(g_fs, ino, buf, size, (uint64_t)offset);
    if (rc != JFS_OK)
        return jfs_errno(rc);
    return (int)size;
}

static struct fuse_operations jfs_ops = {
    .getattr  = jfs_getattr,
    .readdir  = jfs_readdir,
    .create   = jfs_create,
    .mkdir    = jfs_mkdir,
    .unlink   = jfs_unlink,
    .rmdir    = jfs_rmdir,
    .rename   = jfs_rename,
    .truncate = jfs_truncate,
    .open     = jfs_open,
    .read     = jfs_read,
    .write    = jfs_write,
};

int fuse_main_entry(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: journalfs <image> <mountpoint> [fuse opts]\n");
        return 2;
    }

    const char *image = argv[1];

    struct disk *d = NULL;
    if (disk_open(&d, image) != JFS_OK) {
        fprintf(stderr, "journalfs: cannot open '%s'\n", image);
        return 1;
    }

    if (fs_open(&g_fs, d) != JFS_OK) {
        fprintf(stderr, "journalfs: recovery failed on '%s'\n", image);
        disk_close(d);
        return 1;
    }

    /* Build the argv for fuse_main: program name, then everything after the
     * image path (mountpoint followed by any extra FUSE options). */
    int fuse_argc = argc - 1;   /* prog + mountpoint + opts */
    char **fuse_argv = (char **)calloc((size_t)fuse_argc + 1, sizeof(char *));
    if (!fuse_argv) {
        fs_close(g_fs);
        disk_close(d);
        return 1;
    }
    fuse_argv[0] = (char *)"journalfs";
    for (int i = 1; i < fuse_argc; i++)
        fuse_argv[i] = argv[1 + i];   /* argv[2..] = mountpoint + opts */

    int ret = fuse_main(fuse_argc, fuse_argv, &jfs_ops, NULL);
    free(fuse_argv);
    return ret;
}
