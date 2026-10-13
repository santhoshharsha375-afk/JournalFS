/*
 * transaction.c -- Transaction manager and write-ahead logging protocol.
 *
 * Implements the 8-step WAL protocol: persist journal records and the
 * commit record (fsync) before applying any home block, so the commit
 * record is the durable commit point.  Full protocol: docs/wal_protocol.md.
 * Concurrency is serialised by a single global mutex.
 */
#include "transaction.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

/* Records written by one transaction: BEGIN + metadata UPDATEs + superblock
 * UPDATE + COMMIT. */
#define TX_RECORD_OVERHEAD 3u

static void lock_manager(struct tx_manager *tm)
{
    if (tm && tm->mutex)
        pthread_mutex_lock((pthread_mutex_t *)tm->mutex);
}

static void unlock_manager(struct tx_manager *tm)
{
    if (tm && tm->mutex)
        pthread_mutex_unlock((pthread_mutex_t *)tm->mutex);
}

/* Public wrappers: filesystem-level operations take the manager lock for
 * their whole duration (recursive mutex, so the nested lock inside
 * tx_commit is a no-op).  This serialises every metadata mutation. */
void tx_manager_lock(struct tx_manager *tm)
{
    lock_manager(tm);
}

void tx_manager_unlock(struct tx_manager *tm)
{
    unlock_manager(tm);
}

/* Abort the process now.  Used by the crash-injection harness to simulate a
 * power failure at an exact, deterministic point in the protocol. */
static void crash_now(void)
{
    fflush(stdout);
    fflush(stderr);
    abort();
}

static void maybe_crash(struct tx_manager *tm, int stage)
{
    if (tm && tm->crash_point == stage)
        crash_now();
}

int tx_manager_open(struct tx_manager **out, struct disk *d)
{
    if (!out || !d)
        return JFS_ERR_INVAL;

    struct tx_manager *tm = (struct tx_manager *)calloc(1, sizeof(*tm));
    if (!tm)
        return JFS_ERR_NOMEM;

    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) {
        free(tm);
        return JFS_ERR_NOMEM;
    }
    /* Recursive: fs-level ops hold the lock for their whole duration and
     * tx_commit locks again internally. */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int mrc = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    if (mrc != 0) {
        free(m);
        free(tm);
        return JFS_ERR_IO;
    }

    tm->disk = d;
    tm->mutex = m;

    int rc = disk_read_superblock(d, &tm->sb);
    if (rc != JFS_OK) {
        pthread_mutex_destroy(m);
        free(m);
        free(tm);
        return rc;
    }

    journal_state_init(&tm->state);
    tm->state.head = tm->sb.journal_head;
    tm->state.tail = tm->sb.journal_tail;
    tm->state.last_txid = tm->sb.last_txid;
    tm->state.last_committed = tm->sb.last_committed_txid;
    tm->state.last_checkpointed = tm->sb.last_checkpointed_txid;
    tm->state.generation = tm->sb.journal_generation;

    *out = tm;
    return JFS_OK;
}

void tx_manager_close(struct tx_manager *tm)
{
    if (!tm)
        return;
    if (tm->mutex) {
        pthread_mutex_destroy((pthread_mutex_t *)tm->mutex);
        free(tm->mutex);
    }
    free(tm);
}

void tx_set_crash_point(struct tx_manager *tm, int stage)
{
    if (tm)
        tm->crash_point = stage;
}

int tx_begin(struct tx_manager *tm, struct transaction *tx)
{
    if (!tm || !tx)
        return JFS_ERR_INVAL;

    memset(tx, 0, sizeof(*tx));
    tx->mgr = tm;
    tx->txid = tm->state.last_txid + 1u;
    tx->seq = 0;
    tx->generation = tm->state.generation;
    tx->update_count = 0;
    tx->committed = 0;
    tx->applied = 0;
    return JFS_OK;
}

uint8_t *tx_get_staged(struct transaction *tx, uint32_t block)
{
    if (!tx)
        return NULL;
    for (uint32_t i = 0; i < tx->update_count; i++) {
        if (tx->updates[i].block == block)
            return tx->updates[i].data;
    }
    return NULL;
}

int tx_stage_update(struct transaction *tx, uint32_t block, const void *data)
{
    if (!tx || !data)
        return JFS_ERR_INVAL;
    if (tx->update_count >= JFS_MAX_UPDATES)
        return JFS_ERR_TOOLARGE;

    /* If this block is already staged, replace it in place so multiple
     * modifications to the same block within one transaction are merged. */
    for (uint32_t i = 0; i < tx->update_count; i++) {
        if (tx->updates[i].block == block) {
            memcpy(tx->updates[i].data, data, BLOCK_SIZE);
            return JFS_OK;
        }
    }

    struct tx_update *u = &tx->updates[tx->update_count];
    u->block = block;
    memcpy(u->data, data, BLOCK_SIZE);
    tx->update_count++;
    return JFS_OK;
}

/* Stage the superblock as a journaled update.  The new journal head must
 * account for every record this transaction will write: BEGIN, the metadata
 * updates already staged, this superblock update, and the COMMIT record. */
static int stage_superblock(struct tx_manager *tm, struct transaction *tx)
{
    uint32_t m = tx->update_count;               /* metadata updates staged */
    uint32_t total = m + TX_RECORD_OVERHEAD;     /* + BEGIN + superblk + COMMIT */
    uint32_t new_head = (tm->state.head + total) % JOURNAL_SLOTS;

    struct superblock sb = tm->sb;
    sb.journal_head = new_head;
    sb.journal_tail = tm->state.tail;            /* reclaimed at checkpoint */
    sb.last_txid = tx->txid;
    sb.last_committed_txid = tx->txid;
    sb.last_checkpointed_txid = tx->txid;        /* applied atomically */
    sb.journal_generation = tm->state.generation;
    sb.crc = jfs_crc32(&sb, offsetof(struct superblock, crc));

    return tx_stage_update(tx, SUPERBLOCK_BLOCK, &sb);
}

int tx_persist(struct transaction *tx)
{
    if (!tx || !tx->mgr)
        return JFS_ERR_INVAL;
    struct tx_manager *tm = tx->mgr;

    if (tx->update_count > JFS_MAX_UPDATES - 1)
        return JFS_ERR_TOOLARGE;                 /* need room for superblock */

    /* Capacity check: the transaction must fit in the free journal space. */
    uint32_t need = tx->update_count + TX_RECORD_OVERHEAD;
    if (journal_free_slots(&tm->state) < need)
        return JFS_ERR_NOSPC;

    int rc = stage_superblock(tm, tx);
    if (rc != JFS_OK)
        return rc;

    uint32_t head = tm->state.head;
    tx->begin_slot = head;
    tx->commit_slot = (head + 1u + tx->update_count) % JOURNAL_SLOTS;

    /* Record 1: BEGIN. */
    rc = journal_write_slot(tm->disk, head, JREC_TYPE_BEGIN, tx->txid, 0,
                            tx->generation, 0, NULL, 0);
    if (rc != JFS_OK)
        return rc;
    maybe_crash(tm, CRASH_AFTER_BEGIN);

    /* Records 2..M+1: metadata UPDATEs.  The superblock update was staged
     * last, so it is written as the final update record. */
    for (uint32_t i = 0; i < tx->update_count; i++) {
        uint32_t slot = (head + 1u + i) % JOURNAL_SLOTS;
        rc = journal_write_slot(tm->disk, slot, JREC_TYPE_UPDATE, tx->txid,
                                tx->seq + 1u + i, tx->generation,
                                tx->updates[i].block,
                                tx->updates[i].data, BLOCK_SIZE);
        if (rc != JFS_OK)
            return rc;
        maybe_crash(tm, CRASH_DURING_UPDATES);
    }

    /* Advance in-memory journal state. */
    tm->state.head = (tx->commit_slot + 1u) % JOURNAL_SLOTS;
    tm->state.last_txid = tx->txid;
    tm->state.last_committed = tx->txid;

    rc = disk_fsync(tm->disk);
    if (rc != JFS_OK)
        return rc;

    maybe_crash(tm, CRASH_AFTER_PERSIST);
    return JFS_OK;
}

int tx_commit_record(struct transaction *tx)
{
    if (!tx || !tx->mgr)
        return JFS_ERR_INVAL;
    struct tx_manager *tm = tx->mgr;

    maybe_crash(tm, CRASH_DURING_COMMIT);

    int rc = journal_write_slot(tm->disk, tx->commit_slot, JREC_TYPE_COMMIT,
                                tx->txid, tx->seq + 1u + tx->update_count,
                                tx->generation, 0, NULL, 0);
    if (rc != JFS_OK)
        return rc;

    rc = disk_fsync(tm->disk);
    if (rc != JFS_OK)
        return rc;

    tx->committed = 1;
    tm->state.last_committed = tx->txid;

    maybe_crash(tm, CRASH_AFTER_COMMIT);
    return JFS_OK;
}

int tx_apply(struct transaction *tx)
{
    if (!tx || !tx->mgr)
        return JFS_ERR_INVAL;
    struct tx_manager *tm = tx->mgr;

    for (uint32_t i = 0; i < tx->update_count; i++) {
        int rc = disk_write_block(tm->disk, tx->updates[i].block,
                                  tx->updates[i].data);
        if (rc != JFS_OK)
            return rc;
        maybe_crash(tm, CRASH_DURING_APPLY);
    }

    int rc = disk_fsync(tm->disk);
    if (rc != JFS_OK)
        return rc;

    tx->applied = 1;
    tm->sb.journal_head = tm->state.head;
    tm->sb.journal_tail = tm->state.tail;
    tm->sb.last_txid = tm->state.last_txid;
    tm->sb.last_committed_txid = tm->state.last_committed;
    tm->sb.last_checkpointed_txid = tx->txid;

    /* The home blocks (including the superblock that records this
     * transaction as checkpointed) are now durable, so the journal slots
     * this transaction used can be reclaimed immediately.  Advancing the
     * tail here keeps the ring bounded without a separate checkpoint
     * pass; the new tail is persisted by the next transaction's
     * superblock update. */
    tm->state.tail = tm->state.head;

    maybe_crash(tm, CRASH_AFTER_APPLY);
    return JFS_OK;
}

int tx_commit(struct transaction *tx)
{
    if (!tx || !tx->mgr)
        return JFS_ERR_INVAL;
    struct tx_manager *tm = tx->mgr;

    lock_manager(tm);
    maybe_crash(tm, CRASH_BEFORE_PERSIST);

    int rc = JFS_OK;
    if (tm->no_journal) {
        /* Comparison mode: no write-ahead log.  Home blocks are written
         * directly, so a crash mid-transaction can leave the metadata
         * inconsistent.  This is intentional and is what the
         * non-journaled comparison is meant to demonstrate. */
        for (uint32_t i = 0; i < tx->update_count; i++) {
            rc = disk_write_block(tm->disk, tx->updates[i].block,
                                  tx->updates[i].data);
            if (rc != JFS_OK)
                goto out;
        }
        rc = disk_fsync(tm->disk);
        tx->committed = 1;
        tx->applied = 1;
        goto out;
    }

    rc = tx_persist(tx);
    if (rc == JFS_OK)
        rc = tx_commit_record(tx);
    if (rc == JFS_OK)
        rc = tx_apply(tx);

out:
    unlock_manager(tm);
    return rc;
}

void tx_abort(struct transaction *tx)
{
    (void)tx;
    /* Nothing to roll back: updates are only in memory until tx_apply. */
}

int tx_checkpoint(struct tx_manager *tm)
{
    if (!tm)
        return JFS_ERR_INVAL;

    lock_manager(tm);

    /* Only reclaim space for transactions that are already applied.  Because
     * every transaction marks itself checkpointed in its superblock update,
     * advancing the tail to the head is safe here. */
    struct journal_state *st = &tm->state;
    st->tail = st->head;

    struct superblock sb = tm->sb;
    sb.journal_tail = st->tail;
    sb.journal_head = st->head;
    sb.last_checkpointed_txid = st->last_committed;

    int rc = disk_write_superblock(tm->disk, &sb);
    if (rc == JFS_OK) {
        tm->sb = sb;
        rc = disk_fsync(tm->disk);
    }

    unlock_manager(tm);
    return rc;
}
