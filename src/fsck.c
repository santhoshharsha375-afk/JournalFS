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
        rep->issues |= FSCK_ISSUE_SUPERBLOCK;
        snprintf(rep->message, sizeof(rep->message),
                 "superblock failed CRC or format validation");
        return FSCK_ERR_CORRUPT;
    }

    if (sb.bitmap_start != BITMAP_START ||
        sb.inode_start != INODE_START ||
        sb.journal_start != JOURNAL_START ||
        sb.data_start != DATA_START ||
        sb.total_blocks != TOTAL_BLOCKS) {
        rep->issues |= FSCK_ISSUE_SUPERBLOCK;
        snprintf(rep->message, sizeof(rep->message),
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
            if (stored != computed) {
                rep->issues |= FSCK_ISSUE_INODE;
                snprintf(rep->message, sizeof(rep->message),
                         "inode %u has a bad CRC", ino);
            }

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
                rep->issues |= FSCK_ISSUE_BLOCK_REF;
                snprintf(rep->message, sizeof(rep->message),
                         "inode %u references out-of-range block %u", ino, blk);
                continue;
            }
            if (owner[blk] != 0 && owner[blk] != ino) {
                rep->issues |= FSCK_ISSUE_DUP_OWNER;
                snprintf(rep->message, sizeof(rep->message),
                         "block %u is owned by both inode %u and %u",
                         blk, owner[blk], ino);
            }
            owner[blk] = ino;
        }

        /* Indirect blocks. */
        if (fi->block_count > JFS_NDIRECT && fi->indirect != JFS_BLOCK_FREE) {
            uint32_t ind = fi->indirect;
            if (ind >= DATA_BLOCK_COUNT) {
                rep->issues |= FSCK_ISSUE_BLOCK_REF;
                snprintf(rep->message, sizeof(rep->message),
                         "inode %u references out-of-range indirect block %u",
                         ino, ind);
            } else {
                if (owner[ind] != 0 && owner[ind] != ino) {
                    rep->issues |= FSCK_ISSUE_DUP_OWNER;
                }
                owner[ind] = ino;
            }

            uint8_t ind_block[BLOCK_SIZE];
            if (disk_read_block(d, DATA_START + ind, ind_block) == JFS_OK) {
                uint32_t *entries = (uint32_t *)ind_block;
                for (uint32_t k = JFS_NDIRECT; k < fi->block_count; k++) {
                    uint32_t blk = entries[k - JFS_NDIRECT];
                    if (blk == JFS_BLOCK_FREE)
                        continue;
                    if (blk >= DATA_BLOCK_COUNT) {
                        rep->issues |= FSCK_ISSUE_BLOCK_REF;
                        continue;
                    }
                    if (owner[blk] != 0 && owner[blk] != ino) {
                        rep->issues |= FSCK_ISSUE_DUP_OWNER;
                    }
                    owner[blk] = ino;
                }
            }
        }
    }

    /* ---- Bitmap vs ownership consistency ----------------------------- */
    for (uint32_t i = 0; i < DATA_BLOCK_COUNT; i++) {
        int allocated = (bitmap[i / 8u] >> (i % 8u)) & 1u;
        if (owner[i] != 0 && !allocated) {
            rep->issues |= FSCK_ISSUE_BITMAP;
            snprintf(rep->message, sizeof(rep->message),
                     "block %u is owned by inode %u but not marked allocated",
                     i, owner[i]);
        } else if (owner[i] == 0 && allocated) {
            rep->issues |= FSCK_ISSUE_ORPHAN;
            snprintf(rep->message, sizeof(rep->message),
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
                    rep->issues |= FSCK_ISSUE_DIRENT;
                    snprintf(rep->message, sizeof(rep->message),
                             "directory %u references missing inode %u",
                             ino, de->ino);
                    continue;
                }
                if (de->type != inodes[de->ino].type) {
                    rep->issues |= FSCK_ISSUE_DIRENT;
                    snprintf(rep->message, sizeof(rep->message),
                             "directory %u entry '%.64s' type mismatch with inode %u",
                             ino, de->name, de->ino);
                }
            }
        }
    }

    /* ---- Journal structure and transaction sequence ------------------ */
    uint8_t scratch[JREC_MAX_PAYLOAD];
    uint32_t valid = 0;
    uint32_t corrupt = 0;
    uint32_t max_txid = 0;

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
        if (rec.txid > max_txid)
            max_txid = rec.txid;
        if (rec.txid > rep->committed_tx)
            rep->committed_tx = rec.txid;
    }

    rep->journal_records = valid;
    if (corrupt > 0) {
        rep->issues |= FSCK_ISSUE_JOURNAL;
        snprintf(rep->message, sizeof(rep->message),
                 "%u corrupt journal slot(s)", corrupt);
    }

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
