/*
 * journal.h -- Write-ahead log primitives.
 *
 * The journal is a ring of JOURNAL_SLOTS fixed-size slots inside the image.
 * Each slot holds at most one record.  A record is a jrec_header followed by
 * a payload (for UPDATE records the payload is a full metadata block).
 *
 * Write ordering enforced by the transaction manager:
 *
 *   1. BEGIN + UPDATE records are written to the ring.
 *   2. The ring is fsynced.
 *   3. The COMMIT record is written.
 *   4. The ring is fsynced again -- this is the commit point.
 *   5. Home blocks are applied and fsynced.
 *   6. The superblock (journal head/tail/checkpoint state) is updated as
 *      part of the same transaction, so checkpointing is atomic.
 */
#ifndef JFS_JOURNAL_H
#define JFS_JOURNAL_H

#include "journalfs.h"
#include "disk.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Journal state mirrored in memory; persisted in the superblock. */
struct journal_state {
    uint32_t head;            /* next slot to write                */
    uint32_t tail;            /* oldest slot still needed          */
    uint32_t last_txid;       /* highest txid assigned             */
    uint32_t last_committed;  /* highest committed txid            */
    uint32_t last_checkpointed; /* highest applied txid            */
    uint32_t generation;      /* anti-stale marker                 */
};

/* One decoded journal record. */
struct journal_record {
    uint32_t type;
    uint32_t txid;
    uint32_t seq;
    uint32_t generation;
    uint32_t block;           /* UPDATE: home block number         */
    uint32_t payload_len;
    const uint8_t *payload;   /* points into a scratch buffer      */
};

/* Initialise an in-memory journal state from a formatted superblock. */
void journal_state_init(struct journal_state *st);

/* Compute the byte offset of a slot inside the image. */
uint64_t journal_slot_offset(uint32_t slot);

/* Write a record into a slot.  Does not touch journal state. */
int journal_write_slot(struct disk *d, uint32_t slot,
                       uint32_t type, uint32_t txid, uint32_t seq,
                       uint32_t generation, uint32_t block,
                       const void *payload, uint32_t payload_len);

/* Read and validate one slot.  Returns 1 if a valid record was decoded,
 * 0 if the slot is empty/unused, -1 on a checksum or format error. */
int journal_read_slot(struct disk *d, uint32_t slot,
                      struct journal_record *out, uint8_t *scratch,
                      size_t scratch_len);

/* Persist the journal state into the superblock block. */
int journal_persist_state(struct disk *d, const struct journal_state *st);

/* Load the journal state from the superblock block. */
int journal_load_state(struct disk *d, struct journal_state *st);

/* Number of slots currently occupied. */
uint32_t journal_used_slots(const struct journal_state *st);

/* Number of free slots available for a new transaction. */
uint32_t journal_free_slots(const struct journal_state *st);

#ifdef __cplusplus
}
#endif

#endif /* JFS_JOURNAL_H */
