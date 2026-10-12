/*
 * journal.c -- Write-ahead log primitives.
 *
 * The journal is a ring of fixed-size slots.  Each slot holds one record:
 * a 32-byte header followed by a payload.  UPDATE records carry a full
 * metadata block as their payload.
 *
 * A record is considered present only when its magic matches AND its CRC32
 * matches.  The CRC covers every header field except the crc field itself,
 * followed by the payload.  This lets recovery distinguish a valid record
 * from a torn write or from stale bytes left over from a previous ring
 * generation.
 */
#include "journal.h"

#include <string.h>
#include <stdlib.h>

void journal_state_init(struct journal_state *st)
{
    if (!st)
        return;
    st->head = 0;
    st->tail = 0;
    st->last_txid = 0;
    st->last_committed = 0;
    st->last_checkpointed = 0;
    st->generation = 1;
}

uint64_t journal_slot_offset(uint32_t slot)
{
    return (uint64_t)JOURNAL_START * BLOCK_SIZE + (uint64_t)slot * JOURNAL_SLOT_SIZE;
}

/* Compute the CRC of a record: header (crc field zeroed) + payload. */
static uint32_t jrec_crc(const struct jrec_header *h, const void *payload)
{
    struct jrec_header tmp = *h;
    tmp.crc = 0;
    uint32_t crc = jfs_crc32(&tmp, sizeof(tmp));
    if (payload && h->payload_len > 0)
        crc = jfs_crc32_update(crc, payload, h->payload_len);
    return crc;
}

int journal_write_slot(struct disk *d, uint32_t slot,
                       uint32_t type, uint32_t txid, uint32_t seq,
                       uint32_t generation, uint32_t block,
                       const void *payload, uint32_t payload_len)
{
    if (!d)
        return JFS_ERR_INVAL;
    if (slot >= JOURNAL_SLOTS)
        return JFS_ERR_INVAL;
    if (payload_len > JREC_MAX_PAYLOAD)
        return JFS_ERR_TOOLARGE;

    uint8_t slot_buf[JOURNAL_SLOT_SIZE];
    memset(slot_buf, 0, sizeof(slot_buf));

    struct jrec_header h;
    memset(&h, 0, sizeof(h));
    h.magic = JREC_MAGIC;
    h.type = type;
    h.txid = txid;
    h.seq = seq;
    h.generation = generation;
    h.payload_len = payload_len;
    h.block = block;
    h.crc = jrec_crc(&h, payload);

    memcpy(slot_buf, &h, sizeof(h));
    if (payload && payload_len > 0)
        memcpy(slot_buf + sizeof(h), payload, payload_len);

    return disk_pwrite(d, slot_buf, sizeof(slot_buf), journal_slot_offset(slot));
}

int journal_read_slot(struct disk *d, uint32_t slot,
                      struct journal_record *out, uint8_t *scratch,
                      size_t scratch_len)
{
    if (!d || !out)
        return JFS_ERR_INVAL;
    if (slot >= JOURNAL_SLOTS)
        return JFS_ERR_INVAL;

    uint8_t slot_buf[JOURNAL_SLOT_SIZE];
    int rc = disk_pread(d, slot_buf, sizeof(slot_buf), journal_slot_offset(slot));
    if (rc != JFS_OK)
        return rc;

    /* An untouched slot is all zeros. */
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(slot_buf); i++) {
        if (slot_buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero)
        return 0;

    struct jrec_header h;
    memcpy(&h, slot_buf, sizeof(h));

    if (h.magic != JREC_MAGIC)
        return -1;   /* corrupt / foreign */
    if (h.payload_len > JREC_MAX_PAYLOAD)
        return -1;

    uint32_t computed = jrec_crc(&h, slot_buf + sizeof(h));
    if (computed != h.crc)
        return -1;   /* checksum mismatch */

    if (scratch && h.payload_len > 0) {
        if (scratch_len < h.payload_len)
            return JFS_ERR_NOMEM;
        memcpy(scratch, slot_buf + sizeof(h), h.payload_len);
    }

    out->type = h.type;
    out->txid = h.txid;
    out->seq = h.seq;
    out->generation = h.generation;
    out->block = h.block;
    out->payload_len = h.payload_len;
    out->payload = scratch;
    return 1;
}

int journal_persist_state(struct disk *d, const struct journal_state *st)
{
    if (!d || !st)
        return JFS_ERR_INVAL;

    struct superblock sb;
    int rc = disk_read_superblock(d, &sb);
    if (rc != JFS_OK)
        return rc;

    sb.journal_head = st->head;
    sb.journal_tail = st->tail;
    sb.last_txid = st->last_txid;
    sb.last_committed_txid = st->last_committed;
    sb.last_checkpointed_txid = st->last_checkpointed;
    sb.journal_generation = st->generation;
    return disk_write_superblock(d, &sb);
}

int journal_load_state(struct disk *d, struct journal_state *st)
{
    if (!d || !st)
        return JFS_ERR_INVAL;

    struct superblock sb;
    int rc = disk_read_superblock(d, &sb);
    if (rc != JFS_OK)
        return rc;

    st->head = sb.journal_head;
    st->tail = sb.journal_tail;
    st->last_txid = sb.last_txid;
    st->last_committed = sb.last_committed_txid;
    st->last_checkpointed = sb.last_checkpointed_txid;
    st->generation = sb.journal_generation;
    return JFS_OK;
}

uint32_t journal_used_slots(const struct journal_state *st)
{
    if (!st)
        return 0;
    return (st->head - st->tail) % JOURNAL_SLOTS;
}

uint32_t journal_free_slots(const struct journal_state *st)
{
    if (!st)
        return 0;
    /* Keep one slot of slack so head == tail means "empty", not "full". */
    return JOURNAL_SLOTS - journal_used_slots(st) - 1;
}
