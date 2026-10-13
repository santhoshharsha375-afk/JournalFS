/*
 * main.c -- JournalFS entry point.
 *
 * Two modes:
 *
 *   journalfs <image> <mountpoint> [fuse options]
 *       Mount the image through FUSE 3 (compiled in when libfuse3 is
 *       available).  Recovery runs automatically at mount time.
 *
 *   journalfs <image> <command> [args...]
 *       Run a single filesystem operation on the image and exit.  This is
 *       the non-FUSE fallback used for automated testing, crash injection
 *       and demonstration.  Commands: mkfs, create, mkdir, read, write,
 *       rm, rmdir, mv, ls, stat, truncate, fsck, recover, journal.
 *
 * Crash injection: set the JFS_CRASH_POINT environment variable to one of
 * the CRASH_* stage numbers to abort the process at that point during the
 * next mutating operation.
 */
#include "journalfs.h"
#include "disk.h"
#include "filesystem.h"
#include "fsck.h"
#include "transaction.h"
#include "checkpoint.h"
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

int mkfs_format(const char *path);

#ifdef HAVE_FUSE
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "fuse_ops.h"
#endif

/* ------------------------------------------------------------------ */
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

static int apply_crash_point(struct filesystem *fs)
{
    const char *cp = getenv("JFS_CRASH_POINT");
    if (cp && *cp) {
        int stage = atoi(cp);
        tx_set_crash_point(fs->tm, stage);
    }
    return JFS_OK;
}

/* Resolve a full path to its inode, and a path to its parent + final name.
 * Both are thin wrappers over the shared resolver in filesystem.c. */
static int path_to_inode(struct filesystem *fs, const char *path, uint32_t *ino)
{
    return fs_resolve_path(fs, path, ino);
}

static int resolve_parent(struct filesystem *fs, const char *path,
                          uint32_t *parent_ino, char *name, size_t name_len)
{
    return fs_resolve_parent(fs, path, parent_ino, name, name_len);
}

/* ------------------------------------------------------------------ */
/* CLI commands                                                       */
/* ------------------------------------------------------------------ */

static int cmd_mkfs(const char *path)
{
    return mkfs_format(path);
}

/* create and mkdir differ only in type + mode. */
static int cmd_mk(struct filesystem *fs, const char *path,
                  uint32_t type, uint32_t mode)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = resolve_parent(fs, path, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;

    uint32_t existing;
    if (fs_lookup(fs, parent, name, &existing) == JFS_OK)
        return JFS_ERR_EXIST;

    uint32_t ino;
    return fs_create(fs, parent, name, type, mode, &ino);
}

static int cmd_create(struct filesystem *fs, const char *path)
{
    return cmd_mk(fs, path, JFS_TYPE_FILE, 0644);
}

static int cmd_mkdir(struct filesystem *fs, const char *path)
{
    return cmd_mk(fs, path, JFS_TYPE_DIR, 0755);
}

static int cmd_write(struct filesystem *fs, const char *path,
                     const char *data, size_t len)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = resolve_parent(fs, path, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;

    uint32_t ino;
    if (fs_lookup(fs, parent, name, &ino) != JFS_OK) {
        rc = fs_create(fs, parent, name, JFS_TYPE_FILE, 0644, &ino);
        if (rc != JFS_OK)
            return rc;
    }

    return fs_write_data(fs, ino, data, len, 0);
}

static int cmd_read(struct filesystem *fs, const char *path, FILE *out)
{
    uint32_t ino;
    int rc = path_to_inode(fs, path, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;

    uint8_t *buf = (uint8_t *)malloc(in.size ? (size_t)in.size : 1);
    if (!buf)
        return JFS_ERR_NOMEM;

    size_t got = 0;
    rc = fs_read_data(fs, ino, buf, (size_t)in.size, 0, &got);
    if (rc == JFS_OK)
        fwrite(buf, 1, got, out);
    free(buf);
    return rc;
}

/* rm and rmdir differ only in which filesystem call they make. */
static int cmd_rm_dir(struct filesystem *fs, const char *path, int is_dir)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = resolve_parent(fs, path, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;
    if (name[0] == '\0')
        return JFS_ERR_INVAL;

    return is_dir ? fs_rmdir(fs, parent, name)
                  : fs_unlink(fs, parent, name);
}

static int cmd_rm(struct filesystem *fs, const char *path)
{
    return cmd_rm_dir(fs, path, 0);
}

static int cmd_rmdir(struct filesystem *fs, const char *path)
{
    return cmd_rm_dir(fs, path, 1);
}

static int cmd_mv(struct filesystem *fs, const char *oldp, const char *newp)
{
    uint32_t old_parent, new_parent;
    char old_name[JFS_NAME_MAX + 1], new_name[JFS_NAME_MAX + 1];

    int rc = resolve_parent(fs, oldp, &old_parent, old_name, sizeof(old_name));
    if (rc != JFS_OK)
        return rc;
    rc = resolve_parent(fs, newp, &new_parent, new_name, sizeof(new_name));
    if (rc != JFS_OK)
        return rc;

    return fs_rename(fs, old_parent, new_parent, old_name, new_name);
}

static int cmd_ls(struct filesystem *fs, const char *path)
{
    uint32_t ino;
    int rc = path_to_inode(fs, path, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;
    if (in.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;

    uint8_t block[BLOCK_SIZE];
    for (uint32_t b = 0; b < in.block_count; b++) {
        rc = disk_read_block(fs->tm->disk, DATA_START + in.direct[b], block);
        if (rc != JFS_OK)
            return rc;
        for (uint32_t e = 0; e < DIRENTS_PER_BLOCK; e++) {
            struct dirent *de = (struct dirent *)(block + e * sizeof(*de));
            if (de->ino == 0)
                continue;
            printf("%s\n", de->name);
        }
    }
    return JFS_OK;
}

static int cmd_stat(struct filesystem *fs, const char *path)
{
    uint32_t ino;
    int rc = path_to_inode(fs, path, &ino);
    if (rc != JFS_OK)
        return rc;

    struct inode in;
    rc = fs_read_inode(fs, ino, &in);
    if (rc != JFS_OK)
        return rc;

    printf("ino=%u type=%s mode=%04o nlink=%u size=%llu blocks=%u\n",
           in.ino, in.type == JFS_TYPE_DIR ? "dir" : "file",
           in.mode, in.nlink, (unsigned long long)in.size, in.block_count);
    return JFS_OK;
}

static int cmd_truncate(struct filesystem *fs, const char *path, const char *sizep)
{
    uint32_t parent;
    char name[JFS_NAME_MAX + 1];
    int rc = resolve_parent(fs, path, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;
    if (name[0] == '\0')
        return JFS_ERR_ISDIR;

    uint32_t ino;
    rc = fs_lookup(fs, parent, name, &ino);
    if (rc != JFS_OK)
        return rc;

    uint64_t size = strtoull(sizep, NULL, 0);
    return fs_truncate(fs, ino, size);
}

static int cmd_journal(struct filesystem *fs)
{
    uint8_t scratch[JREC_MAX_PAYLOAD];
    for (uint32_t slot = 0; slot < JOURNAL_SLOTS; slot++) {
        struct journal_record rec;
        int rc = journal_read_slot(fs->tm->disk, slot, &rec,
                                   scratch, sizeof(scratch));
        if (rc == 0)
            continue;
        if (rc < 0) {
            printf("slot %u: CORRUPT\n", slot);
            continue;
        }
        printf("slot %u: type=%u txid=%u seq=%u gen=%u block=%u len=%u\n",
               slot, rec.type, rec.txid, rec.seq, rec.generation,
               rec.block, rec.payload_len);
    }
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* CLI driver                                                         */
/* ------------------------------------------------------------------ */

static int run_cli(const char *image, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "no command given\n");
        return 2;
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "mkfs") == 0)
        return cmd_mkfs(image);

    struct disk *d = NULL;
    int rc = disk_open(&d, image);
    if (rc != JFS_OK) {
        fprintf(stderr, "journalfs: cannot open '%s' (%d)\n", image, rc);
        return 2;
    }

    struct filesystem *fs = NULL;
    rc = fs_open(&fs, d);
    if (rc != JFS_OK) {
        fprintf(stderr, "journalfs: open/recovery failed (%d)\n", rc);
        disk_close(d);
        return 1;
    }

    if (getenv("JFS_NOJOURNAL"))
        fs->tm->no_journal = 1;

    apply_crash_point(fs);

    int ret = JFS_OK;

    if (strcmp(cmd, "create") == 0 && argc >= 2)
        ret = cmd_create(fs, argv[1]);
    else if (strcmp(cmd, "mkdir") == 0 && argc >= 2)
        ret = cmd_mkdir(fs, argv[1]);
    else if (strcmp(cmd, "write") == 0 && argc >= 3)
        ret = cmd_write(fs, argv[1], argv[2], strlen(argv[2]));
    else if (strcmp(cmd, "read") == 0 && argc >= 2)
        ret = cmd_read(fs, argv[1], stdout);
    else if (strcmp(cmd, "rm") == 0 && argc >= 2)
        ret = cmd_rm(fs, argv[1]);
    else if (strcmp(cmd, "rmdir") == 0 && argc >= 2)
        ret = cmd_rmdir(fs, argv[1]);
    else if (strcmp(cmd, "mv") == 0 && argc >= 3)
        ret = cmd_mv(fs, argv[1], argv[2]);
    else if (strcmp(cmd, "ls") == 0 && argc >= 2)
        ret = cmd_ls(fs, argv[1]);
    else if (strcmp(cmd, "stat") == 0 && argc >= 2)
        ret = cmd_stat(fs, argv[1]);
    else if (strcmp(cmd, "truncate") == 0 && argc >= 3)
        ret = cmd_truncate(fs, argv[1], argv[2]);
    else if (strcmp(cmd, "journal") == 0)
        ret = cmd_journal(fs);
    else if (strcmp(cmd, "recover") == 0) {
        char report[256];
        ret = tx_recover(fs->tm, report, sizeof(report));
        printf("%s\n", report);
    } else if (strcmp(cmd, "checkpoint") == 0)
        ret = checkpoint_force(fs->tm);
    else if (strcmp(cmd, "fsck") == 0) {
        struct fsck_report rep;
        ret = fsck_check(d, &rep, 0);
        printf("%s: %s (issues=%08x)\n", ret == FSCK_OK ? "OK" : "ISSUES",
               rep.message, rep.issues);
    } else {
        fprintf(stderr, "journalfs: unknown command '%s'\n", cmd);
        ret = 2;
    }

    if (ret != JFS_OK)
        fprintf(stderr, "journalfs: %s failed (%d)\n", cmd, ret);

    fs_close(fs);
    disk_close(d);
    return ret == JFS_OK ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* FUSE mount (when libfuse3 is available)                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_FUSE

int fuse_main_entry(int argc, char **argv);

/* argv layout expected by fuse_main_entry: [prog, image, mountpoint, ...] */
static int run_fuse(int argc, char **argv)
{
    return fuse_main_entry(argc, argv);
}

#endif

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <image> <mountpoint> [fuse opts]  (mount mode)\n"
            "       %s <image> <command> [args...]        (cli mode)\n",
            argv[0], argv[0]);
        return 2;
    }

    const char *image = argv[1];

#ifdef HAVE_FUSE
    /* If the second argument is an existing directory, treat it as a FUSE
     * mountpoint; otherwise treat it as a CLI command. */
    struct stat st;
    if (stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode))
        return run_fuse(argc, argv);
    return run_cli(image, argc - 2, argv + 2);
#else
    return run_cli(image, argc - 2, argv + 2);
#endif
}
