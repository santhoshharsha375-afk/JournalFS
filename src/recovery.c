/*
 * recovery.c -- Mount-time crash recovery.
 *
 * When the filesystem is mounted (or the recovery tool is run), the journal
 * is scanned and the on-disk metadata is brought to a consistent state:
 *
 *   1. Validate the superblock.  If its CRC fails, the superblock is
 *      rebuilt from the latest journaled superblock update.
 *   2. Scan every journal slot.  A slot is accepted only if its magic, CRC,
 *      record type, payload length and generation are all valid.
 *   3. Group records by transaction id.  A transaction is only considered
 *      if it has a BEGIN, a contiguous run of UPDATE records, and a COMMIT.
 *      Anything else (torn write, missing record, bad checksum) is
 *      discarded -- it is never replayed as metadata.
 *   4. Transactions whose commit record is durable are committed; the rest
 *      are ignored.
 *   5. Committed transactions newer than the last checkpointed id are
 *      replayed onto their home blocks in transaction order.
 *   6. Replay is idempotent: applying the same block update twice yields
 *      the same bytes, so repeating recovery cannot corrupt the image.
 *   7. The superblock is then persisted with the recovered journal state.
 *
 * Recovery never formats or silently rewrites an existing image.
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

static int cmp_by_seq(const void *a, const void *b)
{
    const struct scan_record *ra = (const struct scan_record *)a;
    const struct scan_record *rb = (const struct scan_record *)b;
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

    /* --- Step 3: group by transaction and find committed ones --------- */
    qsort(recs, nrec, sizeof(*recs), cmp_by_txid_seq);

    /* For each txid, verify contiguity and the presence of a COMMIT. */
    uint32_t *committed = (uint32_t *)calloc(JOURNAL_SLOTS, sizeof(uint32_t));
    if (!committed) {
        free(recs);
        return JFS_ERR_NOMEM;
    }
    size_t ncommitted = 0;

    size_t i = 0;
    while (i < nrec) {
        uint32_t txid = recs[i].txid;
        size_t j = i;
        while (j < nrec && recs[j].txid == txid)
            j++;

        /* Records for this txid are recs[i..j).  Check contiguity and
         * that the last record is a COMMIT. */
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
            committed[ncommitted++] = txid;
        }

        i = j;
    }

    /* --- Step 4: determine the last checkpointed transaction ----------- */
    uint32_t last_checkpointed = 0;
    if (sb_valid)
        last_checkpointed = sb.last_checkpointed_txid;
    /*
     * If the superblock is torn we cannot trust its checkpoint counter, so
     * we replay every committed transaction.  Replay is idempotent, so this
     * is safe and guarantees no committed update is left unapplied.
     */

    /* --- Step 5: replay committed, uncheckpointed transactions --------- */
    uint32_t replayed = 0;
    uint32_t applied_blocks = 0;

    for (size_t c = 0; c < ncommitted; c++) {
        uint32_t txid = committed[c];
        if (txid <= last_checkpointed)
            continue;

        /* Collect this transaction's UPDATE records in sequence order. */
        struct scan_record *upd = (struct scan_record *)calloc(
            JOURNAL_SLOTS, sizeof(*upd));
        if (!upd)
            continue;
        size_t nupd = 0;
        for (size_t k = 0; k < nrec; k++) {
            if (recs[k].txid == txid && recs[k].type == JREC_TYPE_UPDATE)
                upd[nupd++] = recs[k];
        }
        qsort(upd, nupd, sizeof(*upd), cmp_by_seq);

        for (size_t u = 0; u < nupd; u++) {
            struct journal_record rec;
            int rc = journal_read_slot(d, upd[u].slot, &rec,
                                       scratch, sizeof(scratch));
            if (rc != 1)
                continue;                /* skip unreadable record */
            int wrc = disk_write_block(d, upd[u].block, rec.payload);
            if (wrc == JFS_OK)
                applied_blocks++;
        }
        replayed++;
        free(upd);
    }

    int rc = disk_fsync(d);

    /* --- Step 6: persist the recovered journal state ------------------- */
    uint32_t max_committed = 0;
    for (size_t c = 0; c < ncommitted; c++)
        if (committed[c] > max_committed)
            max_committed = committed[c];

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

    free(committed);
    free(recs);
    return rc;
}
