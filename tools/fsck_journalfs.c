/*
 * fsck_journalfs.c -- Consistency checker CLI.
 *
 * Usage: fsck_journalfs [--repair] <image-path>
 *
 * Exits 0 when the image is consistent, 1 when issues are found (or on
 * error).  Read-only unless --repair is given.
 */
#include "fsck.h"
#include "disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--repair] <image-path>\n", argv[0]);
        return 2;
    }

    int repair = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0)
            repair = 1;
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: fsck_journalfs [--repair] <image-path>\n");
        return 2;
    }

    struct disk *d = NULL;
    int rc = disk_open(&d, path);
    if (rc != JFS_OK) {
        fprintf(stderr, "fsck_journalfs: cannot open '%s' (%d)\n", path, rc);
        return 2;
    }

    struct fsck_report rep;
    rc = fsck_check(d, &rep, repair);
    disk_close(d);

    if (rc == FSCK_OK) {
        printf("fsck_journalfs: OK -- %u inodes, %u dirents, "
               "%u journal records, %u committed\n",
               rep.inodes_checked, rep.dirents_checked,
               rep.journal_records, rep.committed_tx);
        return 0;
    }

    printf("fsck_journalfs: ISSUES FOUND (%08x)\n", rep.issues);
    if (rep.message[0])
        printf("  %s\n", rep.message);
    printf("  inodes=%u dirents=%u journal_records=%u committed_tx=%u "
           "uncheckpointed=%u\n",
           rep.inodes_checked, rep.dirents_checked, rep.journal_records,
           rep.committed_tx, rep.uncheckpointed_tx);
    return 1;
}
