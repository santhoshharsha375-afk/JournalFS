/*
 * recovery.c -- Mount-time crash recovery.
 *
 * Scans the journal, groups records by transaction id, and replays only
 * fully committed transactions in ascending order onto their home blocks.
 * Replay is idempotent and never formats or silently rewrites an image.
 * Full procedure: docs/crash_recovery.md.
 */
#include "transaction.h"
#include "recovery.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct scan_record {
    uint32_t type;
    uint32_t txid;
    uint32_t seq;
    uint32_t slot;
    uint32_t block;
    uint32_t generation;
};

static int cmp_by_txid_seq(const void *a, const void *b)
{
    const struct scan_record *ra = (const struct scan_record *)a;
    const struct scan_record *rb = (const struct scan_record *)b;
    if (ra->txid != rb->txid)
        return (ra->txid < rb->txid) ? -1 : 1;
    if (ra->seq != rb->seq)
        return (ra->seq < rb->seq) ? -1 : 1;
    return 0;
}

int tx_recover(struct tx_manager *tm, char *report, size_t report_len)
{
    if (!tm || !tm->disk)
        return JFS_ERR_INVAL;

    struct disk *d = tm->disk;
    uint8_t scratch[JREC_MAX_PAYLOAD];

    /* --- Step 1: validate the superblock ------------------------------- */
    struct superblock sb;
    int sb_valid = (disk_read_superblock(d, &sb) == JFS_OK);

    uint32_t generation = 1;
    if (sb_valid)
        generation = sb.journal_generation;

    /* --- Step 2: scan the journal -------------------------------------- */
    struct scan_record *recs = (struct scan_record *)calloc(
        JOURNAL_SLOTS, sizeof(*recs));
    if (!recs)
        return JFS_ERR_NOMEM;

    size_t nrec = 0;
    uint32_t max_generation = generation;
    uint32_t corrupt_slots = 0;

    for (uint32_t slot = 0; slot < JOURNAL_SLOTS; slot++) {
        struct journal_record rec;
        int rc = journal_read_slot(d, slot, &rec, scratch, sizeof(scratch));
        if (rc == 0)
            continue;                    /* empty slot                */
        if (rc < 0) {
            corrupt_slots++;             /* torn / foreign / bad CRC  */
            continue;
        }

        /* Validate the record thoroughly before trusting it. */
        if (rec.type != JREC_TYPE_BEGIN &&
            rec.type != JREC_TYPE_UPDATE &&
            rec.type != JREC_TYPE_COMMIT)
            continue;
        if (rec.type == JREC_TYPE_UPDATE && rec.payload_len != BLOCK_SIZE)
            continue;
        if (rec.txid == 0)
            continue;

        if (rec.generation > max_generation)
            max_generation = rec.generation;

        recs[nrec].type = rec.type;
        recs[nrec].txid = rec.txid;
        recs[nrec].seq = rec.seq;
        recs[nrec].slot = slot;
        recs[nrec].block = rec.block;
        recs[nrec].generation = rec.generation;
        nrec++;
    }

    /* If the superblock was torn, trust the generation seen in the records. */
    if (!sb_valid)
        generation = max_generation ? max_generation : 1u;

    /* Drop records from stale generations (left over from a ring wrap). */
    size_t kept = 0;
    for (size_t i = 0; i < nrec; i++) {
        if (recs[i].generation != generation)
            continue;
        recs[kept++] = recs[i];
    }
    nrec = kept;

    /* Order by (txid, seq) so every transaction forms one contiguous group
     * whose UPDATE records are already in sequence order. */
    qsort(recs, nrec, sizeof(*recs), cmp_by_txid_seq);

    /* --- Step 3: replay committed transactions, in one pass ------------- */
    /*
     * The sort above orders records by (txid, seq), so each transaction is
     * now a contiguous group already in sequence order.  A transaction is
     * committed iff its records are contiguous, begin with BEGIN and end with
     * COMMIT.  We validate and replay each group inline, walking txid order so
     * the newest update of a block is applied last.
     *
     * If the superblock is torn its checkpoint counter is untrustworthy, so
     * last_checkpointed stays 0 and every committed transaction is replayed.
     * Replay is idempotent, so that is safe.
     */
    uint32_t last_checkpointed = sb_valid ? sb.last_checkpointed_txid : 0;

    uint32_t max_committed = 0;
    size_t ncommitted = 0;
    uint32_t replayed = 0;
    uint32_t applied_blocks = 0;

    size_t i = 0;
    while (i < nrec) {
        uint32_t txid = recs[i].txid;
        size_t j = i;
        while (j < nrec && recs[j].txid == txid)
            j++;

        int contiguous = 1;
        int has_commit = 0;
        for (size_t k = i; k < j; k++) {
            if (recs[k].seq != (uint32_t)(k - i)) {
                contiguous = 0;
                break;
            }
            if (recs[k].type == JREC_TYPE_COMMIT && k == j - 1)
                has_commit = 1;
        }

        if (contiguous && has_commit && recs[i].type == JREC_TYPE_BEGIN) {
            ncommitted++;
            if (txid > max_committed)
                max_committed = txid;

            if (txid > last_checkpointed) {
                for (size_t k = i; k < j; k++) {
                    if (recs[k].type != JREC_TYPE_UPDATE)
                        continue;
                    struct journal_record rec;
                    if (journal_read_slot(d, recs[k].slot, &rec,
                                          scratch, sizeof(scratch)) != 1)
                        continue;            /* skip unreadable record */
                    if (disk_write_block(d, recs[k].block, rec.payload) == JFS_OK)
                        applied_blocks++;
                }
                replayed++;
            }
        }

        i = j;
    }

    int rc = disk_fsync(d);

    /* --- Step 4: persist the recovered journal state ------------------- */
    tm->state.last_committed = max_committed;
    tm->state.last_txid = (max_committed > 0) ? max_committed
                                              : tm->state.last_txid;
    tm->state.last_checkpointed = max_committed;
    tm->state.generation = generation;
    tm->state.head = (uint32_t)(tm->state.head % JOURNAL_SLOTS);
    tm->state.tail = tm->state.head;     /* everything applied: reclaim */

    struct superblock nsb;
    if (sb_valid)
        nsb = sb;
    else
        memset(&nsb, 0, sizeof(nsb));

    nsb.magic = JFS_MAGIC;
    nsb.version = JFS_VERSION;
    nsb.block_size = BLOCK_SIZE;
    nsb.total_blocks = TOTAL_BLOCKS;
    nsb.data_block_count = DATA_BLOCK_COUNT;
    nsb.inode_count = INODE_COUNT;
    nsb.bitmap_start = BITMAP_START;
    nsb.bitmap_blocks = BITMAP_BLOCKS;
    nsb.inode_start = INODE_START;
    nsb.inode_blocks = INODE_BLOCKS;
    nsb.journal_start = JOURNAL_START;
    nsb.journal_blocks = JOURNAL_BLOCKS;
    nsb.data_start = DATA_START;
    nsb.root_inode = JFS_ROOT_INODE;
    nsb.journal_head = tm->state.head;
    nsb.journal_tail = tm->state.tail;
    nsb.last_txid = tm->state.last_txid;
    nsb.last_committed_txid = tm->state.last_committed;
    nsb.last_checkpointed_txid = tm->state.last_checkpointed;
    nsb.journal_generation = generation;
    nsb.flags = 0;

    int wrc = disk_write_superblock(d, &nsb);
    if (wrc == JFS_OK) {
        tm->sb = nsb;
        wrc = disk_fsync(d);
    }
    if (rc == JFS_OK)
        rc = wrc;

    if (report && report_len > 0) {
        snprintf(report, report_len,
                 "recovery: %u record(s), %u corrupt slot(s), "
                 "%u committed tx, %u replayed, %u block(s) applied",
                 (unsigned)nrec, (unsigned)corrupt_slots,
                 (unsigned)ncommitted, (unsigned)replayed,
                 (unsigned)applied_blocks);
    }

    free(recs);
    return rc;
}
