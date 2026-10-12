/*
 * checkpoint.c -- Journal space reclamation and utilisation monitoring.
 *
 * The journal is a bounded ring.  Space is only reclaimed once the
 * transactions that occupy it have been applied to their home blocks and are
 * recorded as checkpointed in the superblock.  Because every transaction in
 * JournalFS marks itself checkpointed in its own superblock update, the tail
 * can safely be advanced to the head at any time the filesystem is idle.
 */
#include "checkpoint.h"

#include <string.h>

int checkpoint_force(struct tx_manager *tm)
{
    return tx_checkpoint(tm);
}

uint32_t journal_utilization(const struct tx_manager *tm)
{
    if (!tm)
        return 0;
    return journal_used_slots(&tm->state);
}

uint32_t journal_capacity(const struct tx_manager *tm)
{
    (void)tm;
    return JOURNAL_SLOTS;
}
