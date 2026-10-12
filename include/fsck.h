/*
 * fsck.h -- Standalone consistency checker for JournalFS images.
 *
 * The checker is read-only by default: it reports inconsistencies and
 * returns a non-zero exit status.  A separate, explicitly requested repair
 * mode is provided for the well-understood cases.
 */
#ifndef JFS_FSCK_H
#define JFS_FSCK_H

#include "journalfs.h"
#include "disk.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSCK_OK            0
#define FSCK_ERR_CORRUPT  -1
#define FSCK_ERR_IO       -2

/* Bit flags describing what was found. */
#define FSCK_ISSUE_SUPERBLOCK     0x0001u
#define FSCK_ISSUE_BITMAP         0x0002u
#define FSCK_ISSUE_INODE          0x0004u
#define FSCK_ISSUE_DIRENT         0x0008u
#define FSCK_ISSUE_BLOCK_REF      0x0010u
#define FSCK_ISSUE_DUP_OWNER      0x0020u
#define FSCK_ISSUE_JOURNAL        0x0040u
#define FSCK_ISSUE_ORPHAN         0x0080u

struct fsck_report {
    uint32_t issues;              /* OR of FSCK_ISSUE_*            */
    uint32_t inodes_checked;
    uint32_t dirents_checked;
    uint32_t blocks_checked;
    uint32_t journal_records;
    uint32_t committed_tx;
    uint32_t uncheckpointed_tx;
    char     message[512];
};

/* Check an image.  If repair != 0, attempt safe repairs of the issues that
 * are well understood.  Returns FSCK_OK when no issues remain. */
int fsck_check(struct disk *d, struct fsck_report *rep, int repair);

#ifdef __cplusplus
}
#endif

#endif /* JFS_FSCK_H */
