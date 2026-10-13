/*
 * jfs_bench.c -- Reproducible benchmark suite for JournalFS (pure C).
 *
 * Compares three modes over an identical create+write+rm workload:
 *   nojournal   -- WAL disabled (direct home-block updates)
 *   journal     -- WAL enabled, checkpoint only at the end
 *   journal_ckpt-- WAL enabled with periodic forced checkpoints
 *
 * For every mode we record operations/second, average / median / p95
 * latency, total time and peak journal utilisation, then write the raw
 * rows to results/benchmark_results.csv.  `make graphs` renders them.
 *
 * Latency is measured in-process around each fs_* call, so the numbers
 * reflect the filesystem protocol itself, not process-spawn overhead.
 */
#include "journalfs.h"
#include "disk.h"
#include "transaction.h"
#include "filesystem.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

extern int mkfs_format(const char *path);

#define DEFAULT_OPS 200

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

struct mode_result {
    char name[32];
    int ops;
    double total_s;
    double ops_per_sec;
    double avg_ms, median_ms, p95_ms;
    uint32_t max_journal_used;
    uint32_t journal_cap;
};

static int bench_mode(const char *name, const char *img, int use_journal,
                      int use_checkpoint, int cycles, struct mode_result *out)
{
    unlink(img);
    if (mkfs_format(img) != JFS_OK) {
        fprintf(stderr, "mkfs failed for %s\n", img);
        return -1;
    }

    struct disk *d = NULL;
    struct filesystem *fs = NULL;
    if (disk_open(&d, img) != JFS_OK || fs_open(&fs, d) != JFS_OK) {
        fprintf(stderr, "open failed for %s\n", img);
        return -1;
    }
    if (!use_journal)
        fs->tm->no_journal = 1;

    double *lat = (double *)malloc((size_t)cycles * sizeof(double));
    if (!lat) {
        fs_close(fs);
        disk_close(d);
        return -1;
    }

    uint32_t max_used = 0;
    char payload[64];
    int ok = 1;
    double t0 = now_s();

    for (int i = 0; i < cycles; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "/f%d.txt", i);
        snprintf(payload, sizeof(payload), "content_%d", i);

        double o0 = now_s();

        uint32_t ino;
        if (fs_create(fs, JFS_ROOT_INODE, nm + 1, JFS_TYPE_FILE, 0644, &ino)
            != JFS_OK) { ok = 0; break; }
        if (fs_write_data(fs, ino, payload, strlen(payload), 0) != JFS_OK) {
            ok = 0; break;
        }
        if (fs_unlink(fs, JFS_ROOT_INODE, nm + 1) != JFS_OK) {
            ok = 0; break;
        }

        lat[i] = now_s() - o0;

        if (use_checkpoint && i % 20 == 0)
            checkpoint_force(fs->tm);

        uint32_t used = journal_utilization(fs->tm);
        if (used > max_used)
            max_used = used;
    }

    double total = now_s() - t0;
    fs_close(fs);
    disk_close(d);

    if (!ok) {
        fprintf(stderr, "benchmark workload failed in mode %s\n", name);
        free(lat);
        return -1;
    }

    qsort(lat, (size_t)cycles, sizeof(double), cmp_double);
    double sum = 0;
    for (int i = 0; i < cycles; i++)
        sum += lat[i];

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->ops = cycles * 3;
    out->total_s = total;
    out->ops_per_sec = total > 0 ? (cycles * 3) / total : 0;
    out->avg_ms = (sum / cycles) * 1000.0;
    out->median_ms = lat[cycles / 2] * 1000.0;
    out->p95_ms = lat[(int)(cycles * 0.95)] * 1000.0;
    out->max_journal_used = max_used;
    out->journal_cap = JOURNAL_SLOTS;

    free(lat);
    return 0;
}

int main(int argc, char **argv)
{
    int cycles = DEFAULT_OPS;
    if (argc > 1)
        cycles = atoi(argv[1]);
    if (cycles <= 0 || cycles > 100000) {
        fprintf(stderr, "usage: jfs_bench [cycles-per-mode]\n");
        return 2;
    }

    char workdir[128];
    snprintf(workdir, sizeof(workdir), "/tmp/jfs_bench_%d", (int)getpid());
    if (mkdir(workdir, 0755) != 0) {
        perror("mkdir");
        return 2;
    }

    printf("=== JournalFS benchmark ===\n");
    printf("Operations per mode: %d create+write+rm cycles (%d individual ops)\n",
           cycles, cycles * 3);

    struct mode_result results[3];
    const struct { const char *name; int journal; int ckpt; } modes[] = {
        { "nojournal",    0, 0 },
        { "journal",      1, 0 },
        { "journal_ckpt", 1, 1 },
    };

    char img[160];
    int n = 0;
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        printf("  benchmarking %s...\n", modes[m].name);
        snprintf(img, sizeof(img), "%s/%s.img", workdir, modes[m].name);
        if (bench_mode(modes[m].name, img, modes[m].journal, modes[m].ckpt,
                       cycles, &results[n]) == 0) {
            printf("    %.2f ops/s, avg %.3f ms, median %.3f ms, "
                   "p95 %.3f ms, journal %u/%u\n",
                   results[n].ops_per_sec, results[n].avg_ms,
                   results[n].median_ms, results[n].p95_ms,
                   results[n].max_journal_used, results[n].journal_cap);
            n++;
        }
    }

    /* Write CSV into the project results/ directory. */
    const char *csv = "results/benchmark_results.csv";
    FILE *f = fopen(csv, "w");
    if (!f) {
        perror("fopen csv");
        return 1;
    }
    fprintf(f, "mode,ops,total_time_s,ops_per_sec,avg_latency_ms,"
               "median_latency_ms,p95_latency_ms,journal_used,journal_cap\n");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s,%d,%.4f,%.2f,%.3f,%.3f,%.3f,%u,%u\n",
                results[i].name, results[i].ops, results[i].total_s,
                results[i].ops_per_sec, results[i].avg_ms,
                results[i].median_ms, results[i].p95_ms,
                results[i].max_journal_used, results[i].journal_cap);
    }
    fclose(f);
    printf("\nResults written to %s\n", csv);
    printf("Run 'make graphs' to generate plots.\n");

    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", workdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: workdir cleanup failed\n");
    return 0;
}
