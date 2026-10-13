/*
 * fsck.c -- Standalone consistency checker for JournalFS images.
 *
 * The checker is read-only by default.  It validates the superblock, the
 * inode table, the allocation bitmap, every directory entry and the journal,
 * and reports every inconsistency it finds as a bit in fsck_report.issues.
 * A non-zero exit status means at least one issue was detected.
 *
 * Repair is only attempted when repair != 0, and only for the well
 * understood cases; arbitrary corruption is never silently rewritten.
 */
#include "fsck.h"

#include "journal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Record an issue bit and its human-readable description.  Repeated 11
 * times inline before; the last message written wins, as before. */
static void fsck_report(struct fsck_report *rep, uint32_t issue,
                        const char *fmt, ...)
{
    rep->issues |= issue;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rep->message, sizeof(rep->message), fmt, ap);
    va_end(ap);
}

/* Per-inode record used while walking the inode table. */
struct fsck_inode {
    uint32_t ino;
    uint32_t type;
    uint32_t block_count;
    uint32_t direct[JFS_NDIRECT];
    uint32_t indirect;
};

static int check_superblock(struct disk *d, struct fsck_report *rep)
{
    struct superblock sb;
    if (disk_read_superblock(d, &sb) != JFS_OK) {
        fsck_report(rep, FSCK_ISSUE_SUPERBLOCK,
                    "superblock failed CRC or format validation");
        return FSCK_ERR_CORRUPT;
    }

    if (sb.bitmap_start != BITMAP_START ||
        sb.inode_start != INODE_START ||
        sb.journal_start != JOURNAL_START ||
        sb.data_start != DATA_START ||
        sb.total_blocks != TOTAL_BLOCKS) {
        fsck_report(rep, FSCK_ISSUE_SUPERBLOCK,
                    "superblock layout fields are inconsistent");
        return FSCK_ERR_CORRUPT;
    }
    return FSCK_OK;
}

int fsck_check(struct disk *d, struct fsck_report *rep, int repair)
{
    if (!d || !rep)
        return JFS_ERR_INVAL;

    memset(rep, 0, sizeof(*rep));
    rep->issues = 0;

    int rc = check_superblock(d, rep);
    if (rc != FSCK_OK)
        return rc;

    struct superblock sb;
    disk_read_superblock(d, &sb);

    /* ---- Build the block-ownership map ------------------------------- */
    uint32_t *owner = (uint32_t *)calloc(DATA_BLOCK_COUNT, sizeof(uint32_t));
    if (!owner)
        return JFS_ERR_NOMEM;

    struct fsck_inode *inodes =
        (struct fsck_inode *)calloc(INODE_COUNT, sizeof(*inodes));
    if (!inodes) {
        free(owner);
        return JFS_ERR_NOMEM;
    }

    /* ---- Walk the inode table --------------------------------------- */
    uint8_t block[BLOCK_SIZE];
    for (uint32_t b = 0; b < INODE_BLOCKS; b++) {
        if (disk_read_block(d, INODE_START + b, block) != JFS_OK) {
            rep->issues |= FSCK_ISSUE_INODE;
            continue;
        }
        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++) {
            struct inode *in = (struct inode *)(block + i * INODE_SIZE);
            uint32_t ino = b * INODES_PER_BLOCK + i;
            if (ino == 0 || ino >= INODE_COUNT)
                continue;
            if (in->magic != JFS_INODE_MAGIC)
                continue;

            rep->inodes_checked++;

            /* Verify the inode's own CRC. */
            uint32_t stored = in->crc;
            uint32_t computed = jfs_crc32(in, offsetof(struct inode, crc));
            if (stored != computed)
                fsck_report(rep, FSCK_ISSUE_INODE,
                            "inode %u has a bad CRC", ino);

            inodes[ino].ino = ino;
            inodes[ino].type = in->type;
            inodes[ino].block_count = in->block_count;
            inodes[ino].indirect = in->indirect;
            for (uint32_t k = 0; k < JFS_NDIRECT; k++)
                inodes[ino].direct[k] = in->direct[k];
        }
    }

    /* ---- Validate block references and build ownership --------------- */
    uint8_t bitmap[BLOCK_SIZE];
    if (disk_read_block(d, BITMAP_START, bitmap) == JFS_OK) {
        rep->blocks_checked = DATA_BLOCK_COUNT;
    } else {
        rep->issues |= FSCK_ISSUE_BITMAP;
    }

    for (uint32_t ino = 1; ino < INODE_COUNT; ino++) {
        struct fsck_inode *fi = &inodes[ino];
        if (fi->type != JFS_TYPE_FILE && fi->type != JFS_TYPE_DIR)
            continue;

        /* Direct blocks. */
        for (uint32_t k = 0; k < JFS_NDIRECT && k < fi->block_count; k++) {
            uint32_t blk = fi->direct[k];
            if (blk == JFS_BLOCK_FREE)
                continue;
            if (blk >= DATA_BLOCK_COUNT) {
                fsck_report(rep, FSCK_ISSUE_BLOCK_REF,
                            "inode %u references out-of-range block %u", ino, blk);
                continue;
            }
            if (owner[blk] != 0 && owner[blk] != ino)
                fsck_report(rep, FSCK_ISSUE_DUP_OWNER,
                            "block %u is owned by both inode %u and %u",
                            blk, owner[blk], ino);
            owner[blk] = ino;
        }

        /* Files are capped at JFS_NDIRECT direct blocks; there is no
         * indirect block, so anything beyond that is corruption. */
        if (fi->block_count > JFS_NDIRECT) {
            fsck_report(rep, FSCK_ISSUE_BLOCK_REF,
                        "inode %u claims %u blocks but only %u direct slots exist",
                        ino, fi->block_count, JFS_NDIRECT);
            fi->block_count = JFS_NDIRECT;
        }
        if (fi->indirect != JFS_BLOCK_FREE)
            fsck_report(rep, FSCK_ISSUE_BLOCK_REF,
                        "inode %u has an unexpected indirect block %u",
                        ino, fi->indirect);
    }

    /* ---- Bitmap vs ownership consistency ----------------------------- */
    for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++) {
        int allocated = (bitmap[i / 8u] >> (i % 8u)) & 1u;
        if (owner[i] != 0 && !allocated) {
            fsck_report(rep, FSCK_ISSUE_BITMAP,
                        "block %u is owned by inode %u but not marked allocated",
                        i, owner[i]);
        } else if (owner[i] == 0 && allocated) {
            fsck_report(rep, FSCK_ISSUE_ORPHAN,
                        "block %u is allocated but has no owner (orphan)", i);
        }
    }

    /* ---- Validate directory entries --------------------------------- */
    for (uint32_t ino = 1; ino < INODE_COUNT; ino++) {
        if (inodes[ino].type != JFS_TYPE_DIR)
            continue;

        for (uint32_t k = 0; k < JFS_NDIRECT && k < inodes[ino].block_count; k++) {
            uint32_t blk = inodes[ino].direct[k];
            if (blk == JFS_BLOCK_FREE)
                continue;
            if (disk_read_block(d, DATA_START + blk, block) != JFS_OK) {
                rep->issues |= FSCK_ISSUE_DIRENT;
                continue;
            }
            for (uint32_t e = 0; e < DIRENTS_PER_BLOCK; e++) {
                struct dirent *de = (struct dirent *)(block + e * sizeof(*de));
                if (de->ino == 0)
                    continue;
                rep->dirents_checked++;
                if (de->ino >= INODE_COUNT ||
                    (inodes[de->ino].type != JFS_TYPE_FILE &&
                     inodes[de->ino].type != JFS_TYPE_DIR)) {
                    fsck_report(rep, FSCK_ISSUE_DIRENT,
                                "directory %u references missing inode %u",
                                ino, de->ino);
                    continue;
                }
                if (de->type != inodes[de->ino].type)
                    fsck_report(rep, FSCK_ISSUE_DIRENT,
                                "directory %u entry '%.64s' type mismatch with inode %u",
                                ino, de->name, de->ino);
            }
        }
    }

    /* ---- Journal structure and transaction sequence ------------------ */
    uint8_t scratch[JREC_MAX_PAYLOAD];
    uint32_t valid = 0;
    uint32_t corrupt = 0;

    for (uint32_t slot = 0; slot < JOURNAL_SLOTS; slot++) {
        struct journal_record rec;
        int r = journal_read_slot(d, slot, &rec, scratch, sizeof(scratch));
        if (r == 0)
            continue;
        if (r < 0) {
            corrupt++;
            continue;
        }
        valid++;
        if (rec.txid > rep->committed_tx)
            rep->committed_tx = rec.txid;
    }

    rep->journal_records = valid;
    if (corrupt > 0)
        fsck_report(rep, FSCK_ISSUE_JOURNAL,
                    "%u corrupt journal slot(s)", corrupt);

    /* Committed transactions beyond the last checkpoint need replay. */
    if (rep->committed_tx > sb.last_checkpointed_txid) {
        rep->uncheckpointed_tx = rep->committed_tx - sb.last_checkpointed_txid;
    }

    free(inodes);
    free(owner);

    if (rep->issues != 0)
        return FSCK_ERR_CORRUPT;
    (void)repair;
    return FSCK_OK;
}
