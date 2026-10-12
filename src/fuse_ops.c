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
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static struct filesystem *g_fs;

/* Resolve a full path ("/" or "/a/b") to its inode number. */
static int path_to_ino(const char *path, uint32_t *ino)
{
    if (!g_fs || !path || !ino)
        return -EINVAL;

    if (strcmp(path, "/") == 0) {
        *ino = JFS_ROOT_INODE;
        return 0;
    }

    uint32_t parent;
    char name[JFS_NAME_MAX + 1];

    /* Split into parent directory and final component. */
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -EINVAL;

    size_t plen = (size_t)(slash - path);
    if (plen == 0)
        plen = 1;                    /* path is "/name" */

    /* Resolve the parent directory. */
    if (plen == 1 && path[0] == '/') {
        parent = JFS_ROOT_INODE;
    } else {
        char parent_path[plen + 1];
        memcpy(parent_path, path, plen);
        parent_path[plen] = '\0';
        int rc = path_to_ino(parent_path, &parent);
        if (rc != 0)
            return rc;
    }

    const char *nm = slash + 1;
    if (*nm == '\0')
        return -EINVAL;

    uint32_t child;
    int rc = fs_lookup(g_fs, parent, nm, &child);
    if (rc != JFS_OK)
        return -ENOENT;
    *ino = child;
    return 0;
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

static int jfs_lookup(const char *path, struct fuse_entry_param *e)
{
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;

    struct inode in;
    if (fs_read_inode(g_fs, ino, &in) != JFS_OK)
        return -EIO;

    memset(e, 0, sizeof(*e));
    e->ino = ino;
    e->attr_timeout = 1.0;
    e->entry_timeout = 1.0;
    inode_to_stat(&in, &e->attr);
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

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return -EINVAL;
    size_t plen = (size_t)(slash - path);
    char parent_path[plen + 1];
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    int rc = path_to_ino(parent_path[0] ? parent_path : "/", &parent);
    if (rc != 0)
        return rc;

    uint32_t nm_len = strlen(slash + 1);
    if (nm_len == 0 || nm_len > JFS_NAME_MAX)
        return -EINVAL;
    memcpy(name, slash + 1, nm_len);
    name[nm_len] = '\0';

    uint32_t ino;
    rc = fs_create(g_fs, parent, name, JFS_TYPE_FILE, mode & 07777, &ino);
    if (rc != JFS_OK)
        return -EIO;
    return 0;
}

static int jfs_mkdir(const char *path, mode_t mode)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return -EINVAL;
    size_t plen = (size_t)(slash - path);
    char parent_path[plen + 1];
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    int rc = path_to_ino(parent_path[0] ? parent_path : "/", &parent);
    if (rc != 0)
        return rc;

    uint32_t nm_len = strlen(slash + 1);
    if (nm_len == 0 || nm_len > JFS_NAME_MAX)
        return -EINVAL;
    memcpy(name, slash + 1, nm_len);
    name[nm_len] = '\0';

    uint32_t ino;
    rc = fs_create(g_fs, parent, name, JFS_TYPE_DIR, mode & 07777, &ino);
    if (rc != JFS_OK)
        return -EIO;
    return 0;
}

static int jfs_unlink(const char *path)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return -EINVAL;
    size_t plen = (size_t)(slash - path);
    char parent_path[plen + 1];
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    int rc = path_to_ino(parent_path[0] ? parent_path : "/", &parent);
    if (rc != 0)
        return rc;

    uint32_t nm_len = strlen(slash + 1);
    if (nm_len == 0 || nm_len > JFS_NAME_MAX)
        return -EINVAL;
    memcpy(name, slash + 1, nm_len);
    name[nm_len] = '\0';

    return fs_unlink(g_fs, parent, name) == JFS_OK ? 0 : -EIO;
}

static int jfs_rmdir(const char *path)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return -EINVAL;
    size_t plen = (size_t)(slash - path);
    char parent_path[plen + 1];
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    int rc = path_to_ino(parent_path[0] ? parent_path : "/", &parent);
    if (rc != 0)
        return rc;

    uint32_t nm_len = strlen(slash + 1);
    if (nm_len == 0 || nm_len > JFS_NAME_MAX)
        return -EINVAL;
    memcpy(name, slash + 1, nm_len);
    name[nm_len] = '\0';

    return fs_rmdir(g_fs, parent, name) == JFS_OK ? 0 : -EIO;
}

static int jfs_rename(const char *from, const char *to, unsigned int flags)
{
    (void)flags;

    uint32_t from_parent, to_parent;
    char from_name[JFS_NAME_MAX + 1], to_name[JFS_NAME_MAX + 1];

    const char *fslash = strrchr(from, '/');
    const char *tslash = strrchr(to, '/');
    if (!fslash || !tslash || fslash == from || tslash == to)
        return -EINVAL;

    size_t fplen = (size_t)(fslash - from);
    char fpath[fplen + 1];
    memcpy(fpath, from, fplen);
    fpath[fplen] = '\0';
    int rc = path_to_ino(fpath[0] ? fpath : "/", &from_parent);
    if (rc != 0)
        return rc;

    size_t tplen = (size_t)(tslash - to);
    char tpath[tplen + 1];
    memcpy(tpath, to, tplen);
    tpath[tplen] = '\0';
    rc = path_to_ino(tpath[0] ? tpath : "/", &to_parent);
    if (rc != 0)
        return rc;

    uint32_t fnlen = strlen(fslash + 1);
    uint32_t tnlen = strlen(tslash + 1);
    if (fnlen == 0 || fnlen > JFS_NAME_MAX || tnlen == 0 || tnlen > JFS_NAME_MAX)
        return -EINVAL;
    memcpy(from_name, fslash + 1, fnlen);
    from_name[fnlen] = '\0';
    memcpy(to_name, tslash + 1, tnlen);
    to_name[tnlen] = '\0';

    return fs_rename(g_fs, from_parent, from_name, to_name) == JFS_OK ? 0 : -EIO;
}

static int jfs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    uint32_t ino;
    int rc = path_to_ino(path, &ino);
    if (rc != 0)
        return rc;
    return fs_truncate(g_fs, ino, (uint64_t)size) == JFS_OK ? 0 : -EIO;
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
        return -EIO;
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
        return -EIO;
    return (int)size;
}

static struct fuse_operations jfs_ops = {
    .getattr  = jfs_getattr,
    .lookup   = jfs_lookup,
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

    /* Build the argv for fuse_main from everything after the image path. */
    int fuse_argc = argc - 2;
    char **fuse_argv = (char **)calloc((size_t)fuse_argc + 1, sizeof(char *));
    if (!fuse_argv) {
        fs_close(g_fs);
        disk_close(d);
        return 1;
    }
    for (int i = 0; i < fuse_argc; i++)
        fuse_argv[i] = argv[2 + i];
    fuse_argv[0] = (char *)"journalfs";

    int ret = fuse_main(fuse_argc, fuse_argv, &jfs_ops, NULL);
    free(fuse_argv);
    return ret;
}
