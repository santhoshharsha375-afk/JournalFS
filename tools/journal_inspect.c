/*
 * journal_inspect.c -- Dump the journal region of a JournalFS image.
 *
 * Usage: journal_inspect <image-path>
 *
 * Prints every valid journal slot and flags corrupt or stale slots, so the
 * write-ahead log can be inspected without mounting the filesystem.
 */
#include "journalfs.h"
#include "disk.h"
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-path>\n", argv[0]);
        return 2;
    }

    struct disk *d = NULL;
    if (disk_open(&d, argv[1]) != JFS_OK) {
        fprintf(stderr, "journal_inspect: cannot open '%s'\n", argv[1]);
        return 1;
    }

    uint8_t scratch[JREC_MAX_PAYLOAD];
    uint32_t valid = 0, corrupt = 0, empty = 0;

    for (uint32_t slot = 0; slot < JOURNAL_SLOTS; slot++) {
        struct journal_record rec;
        int rc = journal_read_slot(d, slot, &rec, scratch, sizeof(scratch));
        if (rc == 0) {
            empty++;
            continue;
        }
        if (rc < 0) {
            corrupt++;
            printf("slot %3u: CORRUPT\n", slot);
            continue;
        }
        valid++;
        const char *type = "?";
        switch (rec.type) {
        case JREC_TYPE_BEGIN:  type = "BEGIN "; break;
        case JREC_TYPE_UPDATE: type = "UPDATE"; break;
        case JREC_TYPE_COMMIT: type = "COMMIT"; break;
        default: break;
        }
        printf("slot %3u: %s txid=%u seq=%u gen=%u block=%u len=%u\n",
               slot, type, rec.txid, rec.seq, rec.generation, rec.block,
               rec.payload_len);
    }

    printf("\nsummary: %u valid, %u corrupt, %u empty (of %u slots)\n",
           valid, corrupt, empty, JOURNAL_SLOTS);
    disk_close(d);
    return 0;
}
