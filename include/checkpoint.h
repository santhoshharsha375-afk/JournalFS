/*
 * checkpoint.h -- Journal checkpointing and utilisation monitoring.
 */
#ifndef JFS_CHECKPOINT_H
#define JFS_CHECKPOINT_H

#include "transaction.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Advance the journal tail to reclaim space used by applied transactions. */
int checkpoint_force(struct tx_manager *tm);

/* Number of journal slots currently occupied. */
uint32_t journal_utilization(const struct tx_manager *tm);

/* Total number of journal slots. */
uint32_t journal_capacity(const struct tx_manager *tm);

#ifdef __cplusplus
}
#endif

#endif /* JFS_CHECKPOINT_H */
