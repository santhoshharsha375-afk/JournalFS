/*
 * recovery.h -- Mount-time crash recovery interface.
 */
#ifndef JFS_RECOVERY_H
#define JFS_RECOVERY_H

#include "transaction.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scan the journal, discard incomplete or corrupt transactions, replay the
 * committed ones that have not yet been checkpointed, and persist the
 * recovered superblock.  Fills `report` with a human-readable summary when
 * report is non-NULL.  Returns JFS_OK on success.
 */
int tx_recover(struct tx_manager *tm, char *report, size_t report_len);

#ifdef __cplusplus
}
#endif

#endif /* JFS_RECOVERY_H */
