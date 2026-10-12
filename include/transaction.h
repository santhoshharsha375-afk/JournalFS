/*
 * transaction.h -- Transaction manager.
 *
 * A transaction collects a set of metadata block updates in memory, writes
 * them to the write-ahead log, commits durably, then applies them to their
 * home blocks.  Either every update of a transaction is visible after a
 * crash, or none of them are.
 *
 * Concurrency: a single global mutex serialises transactions.  This is
 * deliberately simple -- correctness before optimisation.
 */
#ifndef JFS_TRANSACTION_H
#define JFS_TRANSACTION_H

#include "journalfs.h"
#include "disk.h"
#include "journal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-memory description of one metadata block update. */
struct tx_update {
    uint32_t block;                 /* home block number          */
    uint8_t  data[BLOCK_SIZE];      /* full replacement block    */
};

struct transaction {
    uint32_t txid;
    uint32_t seq;                   /* next sequence number to use */
    uint32_t generation;
    uint32_t update_count;
    struct tx_update updates[JFS_MAX_UPDATES];
    int      committed;
    int      applied;
};

/* The transaction manager binds a disk image to a journal state. */
struct tx_manager {
    struct disk *disk;
    struct journal_state state;
    void *mutex;                    /* platform mutex handle      */
    int   no_journal;               /* comparison mode: skip WAL  */
    int   crash_point;              /* >=0 => abort at this stage */
};

/* Open the manager on an existing image.  No formatting is performed. */
int tx_manager_open(struct tx_manager **out, struct disk *d);

void tx_manager_close(struct tx_manager *tm);

/* Begin a new transaction.  Fails if one is already active. */
int tx_begin(struct tx_manager *tm, struct transaction *tx);

/* Stage an update to a metadata block.  The block is copied into the
 * transaction; the caller may reuse its buffer afterwards. */
int tx_stage_update(struct transaction *tx, uint32_t block, const void *data);

/* Persist all staged records to the journal and fsync. */
int tx_persist(struct transaction *tx);

/* Write the commit record and fsync -- the durable commit point. */
int tx_commit_record(struct transaction *tx);

/* Apply staged updates to their home blocks and fsync. */
int tx_apply(struct transaction *tx);

/* Run the full protocol: persist, commit, apply. */
int tx_commit(struct transaction *tx);

/* Abort the transaction and release its journal slots. */
void tx_abort(struct transaction *tx);

/* Recovery: replay committed-but-uncheckpointed transactions. */
int tx_recover(struct tx_manager *tm, char *report, size_t report_len);

/* Set a deterministic crash point (used by the crash-injection harness). */
void tx_set_crash_point(struct tx_manager *tm, int stage);

#ifdef __cplusplus
}
#endif

#endif /* JFS_TRANSACTION_H */
