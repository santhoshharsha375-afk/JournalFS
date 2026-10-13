/*
 * jfs_test.c -- Unified C test suite for JournalFS.
 *
 * Replaces the former Python harnesses with a single C11 binary so the
 * whole project builds and tests with nothing but gcc and make.
 *
 * Suites:
 *   unit       -- CRC32 vectors, superblock round-trip, journal slot I/O
 *   basic      -- create/read/write/mkdir/rename/rm/truncate via the library
 *   atomic     -- multi-block ops are all-or-nothing after a mid-tx crash
 *   crash      -- deterministic crash injection at all 8 protocol stages
 *   random     -- seeded random crash injection (JFS_TEST_SEED / -s)
 *   corrupt    -- CRC detection of tampered journal and superblock
 *   checkpoint -- journal stays bounded over many transactions
 *   stress     -- multithreaded create/write/rm soak, then fsck
 *
 * Crash injection works by fork(); the child opens the image, performs one
 * mutating operation with a crash point armed, and abort()s at the exact
 * protocol stage.  The parent reaps it, runs recovery + fsck, and checks
 * that the transaction is either fully present or fully absent.
 *
 * Usage: jfs_test [suite ...]   (default: all suites)
 *        jfs_test -s <seed> random
 */
#include "journalfs.h"
#include "disk.h"
#include "journal.h"
#include "transaction.h"
#include "filesystem.h"
#include "fsck.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

int mkfs_format(const char *path);   /* src/mkfs.c */

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static char g_workdir[320];

static void check(const char *desc, int ok)
{
    if (ok) {
        printf("  PASS: %s\n", desc);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", desc);
        g_fail++;
    }
}

static void suite(const char *name)
{
    printf("=== %s ===\n", name);
}

static void img_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_workdir, name);
}

static int run_mkfs(const char *img)
{
    unlink(img);
    return mkfs_format(img);
}

/* Open image + run recovery, like mount does. */
static int open_fs(const char *img, struct disk **d, struct filesystem **fs)
{
    if (disk_open(d, img) != JFS_OK)
        return -1;
    if (fs_open(fs, *d) != JFS_OK) {
        disk_close(*d);
        *d = NULL;
        return -1;
    }
    return 0;
}

static void close_fs(struct disk *d, struct filesystem *fs)
{
    if (fs)
        fs_close(fs);
    if (d)
        disk_close(d);
}

static int fsck_ok(const char *img)
{
    struct disk *d = NULL;
    if (disk_open(&d, img) != JFS_OK)
        return 0;
    struct fsck_report rep;
    int rc = fsck_check(d, &rep, 0);
    disk_close(d);
    return rc == FSCK_OK;
}

/* Create a file at a root-level path via the library. */
static int lib_create(struct filesystem *fs, const char *name)
{
    uint32_t ino;
    return fs_create(fs, JFS_ROOT_INODE, name, JFS_TYPE_FILE, 0644, &ino);
}

static int lib_write(struct filesystem *fs, const char *name, const char *data)
{
    uint32_t ino;
    if (fs_lookup(fs, JFS_ROOT_INODE, name, &ino) != JFS_OK)
        return JFS_ERR_NOENT;
    return fs_write_data(fs, ino, data, strlen(data), 0);
}

static int lib_exists(struct filesystem *fs, const char *name)
{
    uint32_t ino;
    return fs_lookup(fs, JFS_ROOT_INODE, name, &ino) == JFS_OK;
}

/* ------------------------------------------------------------------ */
/* Crash helper: run one mutating op in a forked child with a crash
 * point armed.  Returns 1 if the child died from a signal (injected
 * crash), 0 if it exited normally, -1 on fork failure.            */
/* ------------------------------------------------------------------ */

typedef int (*crash_op_fn)(struct filesystem *fs);

static int crashed_child(const char *img, int stage, crash_op_fn op)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: open, arm crash point, mutate, (maybe) abort(). */
        struct disk *d = NULL;
        struct filesystem *fs = NULL;
        if (open_fs(img, &d, &fs) != JFS_OK)
            _exit(50);
        if (stage > 0)
            tx_set_crash_point(fs->tm, stage);
        int rc = op(fs);
        /* If we get here the crash point was not hit; still exit so the
         * parent sees a clean completion. */
        close_fs(d, fs);
        _exit(rc == JFS_OK ? 0 : 51);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFSIGNALED(status))
        return 1;                 /* injected crash */
    return 0;                     /* ran to completion */
}

/* ------------------------------------------------------------------ */
/* Unit tests                                                          */
/* ------------------------------------------------------------------ */

static void test_crc32(void)
{
    suite("unit: CRC32");
    /* Standard IEEE 802.3 check vector. */
    check("crc32(\"123456789\") == 0xCBF43926",
          jfs_crc32("123456789", 9) == 0xCBF43926u);
    check("crc32(\"\") == 0", jfs_crc32("", 0) == 0u);
    /* Incremental update equals one-shot. */
    uint32_t part = jfs_crc32("12345", 5);
    part = jfs_crc32_update(part, "6789", 4);
    check("incremental == one-shot", part == 0xCBF43926u);
}

static void test_superblock(void)
{
    suite("unit: superblock");
    char img[400];
    img_path(img, sizeof(img), "sb.img");
    check("mkfs succeeds", run_mkfs(img) == JFS_OK);

    struct disk *d = NULL;
    check("disk opens", disk_open(&d, img) == JFS_OK);
    struct superblock sb;
    check("superblock CRC valid", disk_read_superblock(d, &sb) == JFS_OK);
    check("magic", sb.magic == JFS_MAGIC);
    check("version", sb.version == JFS_VERSION);
    check("block size", sb.block_size == BLOCK_SIZE);
    check("journal bounds inside image",
          sb.journal_start + sb.journal_blocks <= sb.total_blocks);
    check("root inode is directory-capable", sb.root_inode == JFS_ROOT_INODE);
    disk_close(d);
}

static void test_journal_slot(void)
{
    suite("unit: journal slot I/O");
    char img[400];
    img_path(img, sizeof(img), "jrnl.img");
    run_mkfs(img);

    struct disk *d = NULL;
    disk_open(&d, img);

    uint8_t payload[BLOCK_SIZE];
    memset(payload, 0xAB, sizeof(payload));
    check("write slot 0",
          journal_write_slot(d, 0, JREC_TYPE_BEGIN, 42, 0, 1, 0, NULL, 0)
              == JFS_OK);
    check("write slot 1 (update)",
          journal_write_slot(d, 1, JREC_TYPE_UPDATE, 42, 1, 1, 7,
                             payload, BLOCK_SIZE) == JFS_OK);

    uint8_t scratch[JREC_MAX_PAYLOAD];
    struct journal_record rec;
    check("read back BEGIN",
          journal_read_slot(d, 0, &rec, scratch, sizeof(scratch)) == 1);
    check("BEGIN type", rec.type == JREC_TYPE_BEGIN);
    check("BEGIN txid", rec.txid == 42);

    check("read back UPDATE",
          journal_read_slot(d, 1, &rec, scratch, sizeof(scratch)) == 1);
    check("UPDATE block", rec.block == 7);
    check("UPDATE payload matches",
          memcmp(scratch, payload, BLOCK_SIZE) == 0);

    /* Corrupt one payload byte: CRC must reject the record. */
    uint8_t byte;
    uint64_t off = journal_slot_offset(1) + JREC_HEADER_SIZE + 10;
    check("pread corruptible byte", disk_pread(d, &byte, 1, off) == JFS_OK);
    byte ^= 0xFF;
    check("pwrite flipped byte", disk_pwrite(d, &byte, 1, off) == JFS_OK);
    check("corrupt record rejected",
          journal_read_slot(d, 1, &rec, scratch, sizeof(scratch)) == -1);

    check("empty slot returns 0",
          journal_read_slot(d, 5, &rec, scratch, sizeof(scratch)) == 0);
    disk_close(d);
}

/* ------------------------------------------------------------------ */
/* Basic operations                                                    */
/* ------------------------------------------------------------------ */

static void test_basic(void)
{
    suite("basic: file and directory operations");
    char img[400];
    img_path(img, sizeof(img), "basic.img");
    run_mkfs(img);

    struct disk *d = NULL;
    struct filesystem *fs = NULL;
    check("open with recovery", open_fs(img, &d, &fs) == 0);

    check("create /a.txt", lib_create(fs, "a.txt") == JFS_OK);
    check("write /a.txt", lib_write(fs, "a.txt", "hello_journalfs") == JFS_OK);

    uint32_t ino;
    check("lookup /a.txt", fs_lookup(fs, JFS_ROOT_INODE, "a.txt", &ino) == JFS_OK);

    char buf[64];
    size_t got = 0;
    check("read /a.txt",
          fs_read_data(fs, ino, buf, sizeof(buf) - 1, 0, &got) == JFS_OK);
    buf[got] = '\0';
    check("read content matches", strcmp(buf, "hello_journalfs") == 0);

    uint32_t dir_ino;
    check("mkdir /d",
          fs_create(fs, JFS_ROOT_INODE, "d", JFS_TYPE_DIR, 0755, &dir_ino)
              == JFS_OK);

    uint32_t nested;
    check("create /d/b.txt",
          fs_create(fs, dir_ino, "b.txt", JFS_TYPE_FILE, 0644, &nested)
              == JFS_OK);
    check("write /d/b.txt",
          fs_write_data(fs, nested, "nested", 6, 0) == JFS_OK);

    check("rename /a.txt -> /d/a.txt",
          fs_rename(fs, JFS_ROOT_INODE, dir_ino, "a.txt", "a.txt") == JFS_OK);
    check("/a.txt gone", !lib_exists(fs, "a.txt"));
    check("/d/a.txt present",
          fs_lookup(fs, dir_ino, "a.txt", &ino) == JFS_OK);

    check("truncate /d/a.txt to 4", fs_truncate(fs, ino, 4) == JFS_OK);
    struct inode in;
    check("truncated size", fs_read_inode(fs, ino, &in) == JFS_OK && in.size == 4);

    check("unlink /d/a.txt", fs_unlink(fs, dir_ino, "a.txt") == JFS_OK);
    check("rmdir /d", fs_rmdir(fs, JFS_ROOT_INODE, "d") == JFS_OK);
    close_fs(d, fs);

    check("fsck clean after basic ops", fsck_ok(img));
}

/* ------------------------------------------------------------------ */
/* Transaction atomicity under crash                                   */
/* ------------------------------------------------------------------ */

static int op_create(struct filesystem *fs) { return lib_create(fs, "atomic.txt"); }
static int op_unlink(struct filesystem *fs)
{
    return fs_unlink(fs, JFS_ROOT_INODE, "atomic.txt");
}
static int op_rename(struct filesystem *fs)
{
    return fs_rename(fs, JFS_ROOT_INODE, JFS_ROOT_INODE, "atomic.txt", "moved.txt");
}
static int op_truncate(struct filesystem *fs)
{
    uint32_t ino;
    if (fs_lookup(fs, JFS_ROOT_INODE, "atomic.txt", &ino) != JFS_OK)
        return JFS_ERR_NOENT;
    return fs_truncate(fs, ino, 4);
}

static void test_atomic(void)
{
    suite("atomic: multi-block ops all-or-nothing at commit");

    /* Crash after the commit record is durable: the op must survive. */
    char img[400];
    img_path(img, sizeof(img), "at_create.img");
    run_mkfs(img);
    crashed_child(img, CRASH_AFTER_COMMIT, op_create);
    check("create@after_commit: fsck clean", fsck_ok(img));
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        check("create@after_commit: file exists",
              open_fs(img, &d, &fs) == 0 && lib_exists(fs, "atomic.txt"));
        close_fs(d, fs);
    }

    /* Crash before the commit record: the op must vanish. */
    img_path(img, sizeof(img), "at_pre.img");
    run_mkfs(img);
    crashed_child(img, CRASH_AFTER_PERSIST, op_create);
    check("create@before_commit: fsck clean", fsck_ok(img));
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        check("create@before_commit: file absent",
              open_fs(img, &d, &fs) == 0 && !lib_exists(fs, "atomic.txt"));
        close_fs(d, fs);
    }

    /* Unlink after durable commit: deletion must be visible (freed). */
    img_path(img, sizeof(img), "at_unl.img");
    run_mkfs(img);
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        open_fs(img, &d, &fs);
        lib_create(fs, "atomic.txt");
        close_fs(d, fs);
    }
    crashed_child(img, CRASH_AFTER_COMMIT, op_unlink);
    check("unlink@after_commit: fsck clean", fsck_ok(img));
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        check("unlink@after_commit: file gone",
              open_fs(img, &d, &fs) == 0 && !lib_exists(fs, "atomic.txt"));
        close_fs(d, fs);
    }

    /* Rename crash-committed: old name gone, new name present. */
    img_path(img, sizeof(img), "at_ren.img");
    run_mkfs(img);
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        open_fs(img, &d, &fs);
        lib_create(fs, "atomic.txt");
        close_fs(d, fs);
    }
    crashed_child(img, CRASH_AFTER_COMMIT, op_rename);
    check("rename@after_commit: fsck clean", fsck_ok(img));
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        check("rename@after_commit: fully renamed",
              open_fs(img, &d, &fs) == 0 &&
                  !lib_exists(fs, "atomic.txt") && lib_exists(fs, "moved.txt"));
        close_fs(d, fs);
    }

    /* Truncate crash-committed: size is the new one. */
    img_path(img, sizeof(img), "at_trc.img");
    run_mkfs(img);
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        open_fs(img, &d, &fs);
        lib_create(fs, "atomic.txt");
        lib_write(fs, "atomic.txt", "some_data_here");
        close_fs(d, fs);
    }
    crashed_child(img, CRASH_AFTER_COMMIT, op_truncate);
    check("truncate@after_commit: fsck clean", fsck_ok(img));
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        struct inode in; uint32_t ino;
        check("truncate@after_commit: size == 4",
              open_fs(img, &d, &fs) == 0 &&
                  fs_lookup(fs, JFS_ROOT_INODE, "atomic.txt", &ino) == JFS_OK &&
                  fs_read_inode(fs, ino, &in) == JFS_OK && in.size == 4);
        close_fs(d, fs);
    }
}

/* ------------------------------------------------------------------ */
/* Deterministic crash stages                                          */
/* ------------------------------------------------------------------ */

static const struct { int stage; const char *name; int committed; } stages[] = {
    { CRASH_BEFORE_PERSIST,  "before_persist",  0 },
    { CRASH_AFTER_BEGIN,     "after_begin",     0 },
    { CRASH_DURING_UPDATES,  "during_updates",  0 },
    { CRASH_AFTER_PERSIST,   "after_persist",   0 },
    { CRASH_DURING_COMMIT,   "during_commit",   0 },
    { CRASH_AFTER_COMMIT,    "after_commit",    1 },
    { CRASH_DURING_APPLY,    "during_apply",    1 },
    { CRASH_AFTER_APPLY,     "after_apply",     1 },
};

static void test_crash_stages(void)
{
    suite("crash: deterministic injection at every stage");
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        char img[400], name[64];
        snprintf(name, sizeof(name), "crash_%d.img", stages[i].stage);
        img_path(img, sizeof(img), name);
        run_mkfs(img);

        int crashed = crashed_child(img, stages[i].stage, op_create);
        int clean = fsck_ok(img);

        struct disk *d = NULL; struct filesystem *fs = NULL;
        int exists = 0;
        if (open_fs(img, &d, &fs) == 0) {
            exists = lib_exists(fs, "atomic.txt");
            close_fs(d, fs);
        }
        int ok = crashed == 1 && clean && (exists == stages[i].committed);
        printf("  stage %d (%s): crashed=%d exists=%d expected=%d fsck=%d\n",
               stages[i].stage, stages[i].name, crashed, exists,
               stages[i].committed, clean);
        check(stages[i].name, ok);
    }

    /* Idempotent recovery: run recovery twice, fsck stays clean. */
    char img[400];
    img_path(img, sizeof(img), "idem.img");
    run_mkfs(img);
    crashed_child(img, CRASH_AFTER_COMMIT, op_create);
    check("recovery run 1 fsck clean", fsck_ok(img));
    check("recovery run 2 fsck clean", fsck_ok(img));
}

/* ------------------------------------------------------------------ */
/* Randomised crash injection                                          */
/* ------------------------------------------------------------------ */

static void test_random_crash(unsigned seed, int iterations)
{
    suite("random: seeded crash injection");
    printf("  seed=%u iterations=%d\n", seed, iterations);
    srand(seed);
    int inconsistent = 0, committed_seen = 0, discarded_seen = 0;

    for (int i = 0; i < iterations; i++) {
        char img[400], name[64];
        snprintf(name, sizeof(name), "rnd_%d.img", i);
        img_path(img, sizeof(img), name);
        run_mkfs(img);

        int stage = 1 + (rand() % 8);
        int op_pick = rand() % 4;
        crash_op_fn ops[4] = { op_create, op_unlink, op_rename, op_truncate };

        /* For ops that need a pre-existing file, create it first. */
        if (op_pick != 0) {
            struct disk *d = NULL; struct filesystem *fs = NULL;
            if (open_fs(img, &d, &fs) == 0) {
                lib_create(fs, "atomic.txt");
                lib_write(fs, "atomic.txt", "payload");
                close_fs(d, fs);
            }
        }

        crashed_child(img, stage, ops[op_pick]);
        if (!fsck_ok(img))
            inconsistent++;

        struct disk *d = NULL; struct filesystem *fs = NULL;
        if (open_fs(img, &d, &fs) == 0) {
            if (lib_exists(fs, "atomic.txt"))
                committed_seen++;
            else
                discarded_seen++;
            close_fs(d, fs);
        }
        unlink(img);
    }

    printf("  committed=%d discarded=%d inconsistent=%d\n",
           committed_seen, discarded_seen, inconsistent);
    check("every random crash left a consistent fs", inconsistent == 0);
    check("both outcomes exercised", committed_seen > 0 && discarded_seen > 0);
}

/* ------------------------------------------------------------------ */
/* Corruption detection                                                */
/* ------------------------------------------------------------------ */

static void test_corrupt(void)
{
    suite("corrupt: checksums reject tampered records");
    char img[400];

    /* Tamper with a journal record header byte -> fsck must complain. */
    img_path(img, sizeof(img), "corr_j.img");
    run_mkfs(img);
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        open_fs(img, &d, &fs);
        lib_create(fs, "file.txt");
        lib_write(fs, "file.txt", "data");
        close_fs(d, fs);
    }
    {
        struct disk *d = NULL;
        disk_open(&d, img);
        uint8_t b;
        /* txid field of slot 0 header, always covered by the CRC. */
        uint64_t off = journal_slot_offset(0) + 8;
        disk_pread(d, &b, 1, off);
        b ^= 0xFF;
        disk_pwrite(d, &b, 1, off);
        disk_close(d);
    }
    check("corrupted journal byte detected", !fsck_ok(img));

    /* Tamper with the superblock -> fsck must fail. */
    img_path(img, sizeof(img), "corr_sb.img");
    run_mkfs(img);
    {
        struct disk *d = NULL;
        disk_open(&d, img);
        uint8_t b;
        disk_pread(d, &b, 1, 8);   /* block_size field */
        b ^= 0xFF;
        disk_pwrite(d, &b, 1, 8);
        disk_close(d);
    }
    check("corrupted superblock detected", !fsck_ok(img));

    /* Corrupt a journal payload; recovery must complete and not replay
     * the bad record as metadata. */
    img_path(img, sizeof(img), "corr_rec.img");
    run_mkfs(img);
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        open_fs(img, &d, &fs);
        lib_create(fs, "file.txt");
        close_fs(d, fs);
    }
    {
        struct disk *d = NULL;
        disk_open(&d, img);
        uint8_t b;
        uint64_t off = journal_slot_offset(0) + JREC_HEADER_SIZE + 100;
        disk_pread(d, &b, 1, off);
        b ^= 0xFF;
        disk_pwrite(d, &b, 1, off);
        disk_close(d);
    }
    {
        struct disk *d = NULL; struct filesystem *fs = NULL;
        check("recovery completes despite corrupt record",
              open_fs(img, &d, &fs) == 0);
        close_fs(d, fs);
    }
}

/* ------------------------------------------------------------------ */
/* Checkpointing / bounded journal                                     */
/* ------------------------------------------------------------------ */

static void test_checkpoint(int cycles)
{
    suite("checkpoint: journal bounded over many transactions");
    char img[400];
    img_path(img, sizeof(img), "ckpt.img");
    run_mkfs(img);

    struct disk *d = NULL;
    struct filesystem *fs = NULL;
    check("open", open_fs(img, &d, &fs) == 0);

    uint32_t cap = journal_capacity(fs->tm);
    uint32_t max_used = 0;
    int ok = 1;
    char payload[64];

    for (int i = 0; i < cycles && ok; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "f%d.txt", i);
        snprintf(payload, sizeof(payload), "content_%d", i);
        if (lib_create(fs, nm) != JFS_OK ||
            lib_write(fs, nm, payload) != JFS_OK ||
            fs_unlink(fs, JFS_ROOT_INODE, nm) != JFS_OK) {
            ok = 0;
            break;
        }
        if (i % 50 == 0 && checkpoint_force(fs->tm) != JFS_OK)
            ok = 0;
        uint32_t used = journal_utilization(fs->tm);
        if (used > max_used)
            max_used = used;
    }

    printf("  cycles=%d max_journal_used=%u/%u\n", cycles, max_used, cap);
    check("all cycles succeeded", ok);
    check("journal usage stayed within capacity", max_used <= cap);
    close_fs(d, fs);
    check("fsck clean after checkpointing", fsck_ok(img));
}

/* ------------------------------------------------------------------ */
/* Multithreaded stress / soak                                         */
/* ------------------------------------------------------------------ */

struct stress_arg {
    struct filesystem *fs;
    int tid;
    int ops;
    int errors;
};

static void *stress_thread(void *p)
{
    struct stress_arg *a = (struct stress_arg *)p;
    char nm[32], payload[64];
    for (int i = 0; i < a->ops; i++) {
        snprintf(nm, sizeof(nm), "t%d_%d.txt", a->tid, i);
        snprintf(payload, sizeof(payload), "stress_%d_%d", a->tid, i);
        if (lib_create(a->fs, nm) != JFS_OK) {
            a->errors++;
            continue;
        }
        if (lib_write(a->fs, nm, payload) != JFS_OK)
            a->errors++;
        /* Verify our own data reads back. */
        uint32_t ino;
        char buf[64];
        size_t got = 0;
        if (fs_lookup(a->fs, JFS_ROOT_INODE, nm, &ino) == JFS_OK &&
            fs_read_data(a->fs, ino, buf, sizeof(buf) - 1, 0, &got) == JFS_OK) {
            buf[got] = '\0';
            if (strcmp(buf, payload) != 0)
                a->errors++;
        } else {
            a->errors++;
        }
        if (fs_unlink(a->fs, JFS_ROOT_INODE, nm) != JFS_OK)
            a->errors++;
    }
    return NULL;
}

static void test_stress(int nthreads, int ops)
{
    suite("stress: multithreaded create/write/read/unlink soak");
    char img[400];
    img_path(img, sizeof(img), "stress.img");
    run_mkfs(img);

    struct disk *d = NULL;
    struct filesystem *fs = NULL;
    check("open", open_fs(img, &d, &fs) == 0);

    struct stress_arg args[16];
    pthread_t tids[16];
    if (nthreads > 16)
        nthreads = 16;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < nthreads; i++) {
        args[i].fs = fs;
        args[i].tid = i;
        args[i].ops = ops;
        args[i].errors = 0;
        pthread_create(&tids[i], NULL, stress_thread, &args[i]);
    }
    int total_errors = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tids[i], NULL);
        total_errors += args[i].errors;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    int total_ops = nthreads * ops * 4;   /* create+write+read+unlink */
    printf("  %d threads x %d cycles = %d ops in %.2fs (%.0f ops/s), "
           "errors=%d\n",
           nthreads, ops, total_ops, secs, total_ops / secs, total_errors);

    check("no operation errors under concurrency", total_errors == 0);
    close_fs(d, fs);
    check("fsck clean after stress", fsck_ok(img));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: jfs_test [-s seed] [unit|basic|atomic|crash|random|"
        "corrupt|checkpoint|stress] ...\n"
        "  default: run every suite\n");
}

int main(int argc, char **argv)
{
    /* Line-buffer stdout so fork()'d crash-injection children never flush a
     * duplicate copy of the parent's pending banner on exit. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    unsigned seed = (unsigned)time(NULL);
    int argi = 1;
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        seed = (unsigned)strtoul(argv[2], NULL, 0);
        argi = 3;
    }

    snprintf(g_workdir, sizeof(g_workdir), "/tmp/jfs_test_%d", (int)getpid());
    if (mkdir(g_workdir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir workdir");
        return 2;
    }

    int ran_any = 0;
    for (int i = argi; i < argc; i++) {
        const char *s = argv[i];
        ran_any = 1;
        if (strcmp(s, "unit") == 0) { test_crc32(); test_superblock(); test_journal_slot(); }
        else if (strcmp(s, "basic") == 0) test_basic();
        else if (strcmp(s, "atomic") == 0) test_atomic();
        else if (strcmp(s, "crash") == 0) test_crash_stages();
        else if (strcmp(s, "random") == 0) test_random_crash(seed, 24);
        else if (strcmp(s, "corrupt") == 0) test_corrupt();
        else if (strcmp(s, "checkpoint") == 0) test_checkpoint(400);
        else if (strcmp(s, "stress") == 0) test_stress(8, 60);
        else { usage(); return 2; }
    }
    if (!ran_any) {
        test_crc32();
        test_superblock();
        test_journal_slot();
        test_basic();
        test_atomic();
        test_crash_stages();
        test_random_crash(seed, 24);
        test_corrupt();
        test_checkpoint(400);
        test_stress(8, 60);
    }

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);

    /* Cleanup workdir. */
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_workdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: workdir cleanup failed\n");

    if (g_fail) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
