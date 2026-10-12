/*
 * journalfs.h -- Shared on-disk format definitions for JournalFS.
 *
 * This header is the single source of truth for every byte that JournalFS
 * persists.  All structures are laid out with explicit padding and are
 * protected by _Static_assert so that a layout mistake is a compile error
 * rather than a silent on-disk corruption.
 *
 * On-disk layout of the image (all offsets in blocks):
 *
 *   block 0                     Superblock
 *   block 1 .. 1+bitmap        Data-block allocation bitmap
 *   .. .. +inode_blocks        Inode table
 *   .. .. +journal_blocks      Journal region (ring of fixed-size slots)
 *   .. .. +data_blocks         File / directory data blocks
 *
 * Everything is little-endian (the project targets x86-64 Linux).
 */
#ifndef JOURNALFS_H
#define JOURNALFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Build-time configuration                                            */
/* ------------------------------------------------------------------ */

#define BLOCK_SIZE          4096u

#define JFS_MAGIC           0x4A465331u   /* 'J' 'F' 'S' '1' */
#define JFS_VERSION         1u

#define INODE_SIZE          256u
#define INODES_PER_BLOCK    (BLOCK_SIZE / INODE_SIZE)          /* 16 */
#define INODE_COUNT         1024u
#define INODE_BLOCKS        (INODE_COUNT / INODES_PER_BLOCK)   /* 64  */

#define DATA_BLOCK_COUNT    16384u
#define BITMAP_BLOCKS       1u   /* 16384 bits = 2048 bytes */

#define JOURNAL_SLOT_SIZE   8192u            /* 2 blocks, holds 1 record */
#define JOURNAL_SLOTS       256u
#define JOURNAL_BLOCKS      (JOURNAL_SLOTS * JOURNAL_SLOT_SIZE / BLOCK_SIZE) /* 512 */

#define JFS_NAME_MAX       244u
#define JFS_MAX_UPDATES     64u

/* Block numbers of the fixed regions. */
#define SUPERBLOCK_BLOCK    0u
#define BITMAP_START        1u
#define INODE_START         (BITMAP_START + BITMAP_BLOCKS)                 /* 2   */
#define JOURNAL_START       (INODE_START + INODE_BLOCKS)                   /* 66  */
#define DATA_START          (JOURNAL_START + JOURNAL_BLOCKS)               /* 578 */
#define TOTAL_BLOCKS        (DATA_START + DATA_BLOCK_COUNT)                /* 16962 */
#define IMAGE_SIZE          ((uint64_t)TOTAL_BLOCKS * BLOCK_SIZE)

/* ------------------------------------------------------------------ */
/* Superblock                                                          */
/* ------------------------------------------------------------------ */

#define JFS_ROOT_INODE      1u   /* inode 0 is reserved / invalid */

struct superblock {
    uint32_t magic;                  /* 0x00  JFS_MAGIC            */
    uint32_t version;                /* 0x04  JFS_VERSION          */
    uint32_t block_size;             /* 0x08  BLOCK_SIZE           */
    uint32_t total_blocks;           /* 0x0C  TOTAL_BLOCKS         */
    uint32_t data_block_count;       /* 0x10  DATA_BLOCK_COUNT     */
    uint32_t inode_count;            /* 0x14  INODE_COUNT          */
    uint32_t bitmap_start;             /* 0x18                       */
    uint32_t bitmap_blocks;            /* 0x1C                       */
    uint32_t inode_start;            /* 0x20                       */
    uint32_t inode_blocks;           /* 0x24                       */
    uint32_t journal_start;          /* 0x28                       */
    uint32_t journal_blocks;         /* 0x2C                       */
    uint32_t data_start;             /* 0x30                       */
    uint32_t root_inode;             /* 0x34                       */
    uint32_t free_blocks;            /* 0x38                       */
    uint32_t free_inodes;            /* 0x3C                       */
    uint32_t journal_head;           /* 0x40  next slot to write   */
    uint32_t journal_tail;           /* 0x44  oldest needed slot   */
    uint32_t last_txid;              /* 0x48  highest txid assigned */
    uint32_t last_committed_txid;    /* 0x4C  highest committed    */
    uint32_t last_checkpointed_txid; /* 0x50  highest applied      */
    uint32_t journal_generation;     /* 0x54  anti-stale marker    */
    uint32_t flags;                  /* 0x58                       */
    uint32_t crc;                    /* 0x5C  CRC32 of [0,0x5C)   */
    uint8_t  pad[BLOCK_SIZE - 0x60]; /* 0x60 .. BLOCK_SIZE         */
};

_Static_assert(sizeof(struct superblock) == BLOCK_SIZE, "superblock must be one block");

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

#define JFS_INODE_MAGIC     0x494E4F44u   /* 'I' 'N' 'O' 'D' */

#define JFS_TYPE_FILE       1u
#define JFS_TYPE_DIR        2u

#define JFS_NDIRECT         12u
#define JFS_INDIRECT_BLOCKS (BLOCK_SIZE / 4u)   /* 1024 */

struct inode {
    uint32_t magic;                  /* 0x00                    */
    uint32_t ino;                    /* 0x04  inode number      */
    uint32_t type;                   /* 0x08  JFS_TYPE_*        */
    uint32_t mode;                   /* 0x0C  permission bits   */
    uint32_t uid;                    /* 0x10                    */
    uint32_t gid;                    /* 0x14                    */
    uint32_t nlink;                  /* 0x18                    */
    uint32_t flags;                  /* 0x1C                    */
    uint64_t size;                   /* 0x20                    */
    uint64_t atime;                  /* 0x28  seconds           */
    uint64_t mtime;                  /* 0x30                    */
    uint64_t ctime;                  /* 0x38                    */
    uint32_t block_count;            /* 0x40  allocated blocks  */
    uint32_t direct[JFS_NDIRECT];    /* 0x44  12 * 4 = 48 bytes */
    uint32_t indirect;               /* 0x5C  indirect blk num  */
    uint32_t pad;                    /* 0x60                    */
    uint32_t crc;                    /* 0x64                    */
    uint8_t  tail[INODE_SIZE - 0x68];/* 0x68 .. 256             */
};

_Static_assert(sizeof(struct inode) == INODE_SIZE, "inode size mismatch");

/* ------------------------------------------------------------------ */
/* Directory entries                                                   */
/* ------------------------------------------------------------------ */

struct dirent {
    uint32_t ino;                    /* 0x00                    */
    uint32_t type;                   /* 0x04  JFS_TYPE_*        */
    uint16_t name_len;               /* 0x08                    */
    uint16_t flags;                  /* 0x0A                    */
    char     name[JFS_NAME_MAX];     /* 0x0C .. 256             */
};

_Static_assert(sizeof(struct dirent) == 256u, "dirent must be 256 bytes");
#define DIRENTS_PER_BLOCK   (BLOCK_SIZE / sizeof(struct dirent))   /* 16 */

/* ------------------------------------------------------------------ */
/* Journal records                                                     */
/* ------------------------------------------------------------------ */

#define JREC_MAGIC          0x4A524543u   /* 'J' 'R' 'E' 'C' */

#define JREC_TYPE_BEGIN     1u
#define JREC_TYPE_UPDATE    2u
#define JREC_TYPE_COMMIT    3u

struct jrec_header {
    uint32_t magic;                  /* 0x00  JREC_MAGIC         */
    uint32_t type;                   /* 0x04  JREC_TYPE_*        */
    uint32_t txid;                   /* 0x08  transaction id     */
    uint32_t seq;                    /* 0x0C  seq within tx      */
    uint32_t generation;             /* 0x10  journal generation */
    uint32_t payload_len;            /* 0x14  payload bytes      */
    uint32_t block;                  /* 0x18  UPDATE: home block */
    uint32_t crc;                    /* 0x1C  CRC32              */
};

#define JREC_HEADER_SIZE    sizeof(struct jrec_header)   /* 32 */
#define JREC_MAX_PAYLOAD    (JOURNAL_SLOT_SIZE - JREC_HEADER_SIZE)

/* ------------------------------------------------------------------ */
/* Error codes (POSIX-ish, kept small and explicit)                    */
/* ------------------------------------------------------------------ */

#define JFS_OK              0
#define JFS_ERR_IO          (-1)
#define JFS_ERR_NOSPC       (-2)
#define JFS_ERR_NOENT       (-3)
#define JFS_ERR_EXIST       (-4)
#define JFS_ERR_NOTEMPTY    (-5)
#define JFS_ERR_NOTDIR      (-6)
#define JFS_ERR_ISDIR       (-7)
#define JFS_ERR_INVAL       (-8)
#define JFS_ERR_CORRUPT     (-9)
#define JFS_ERR_FORMAT      (-10)
#define JFS_ERR_BUSY        (-11)
#define JFS_ERR_NOMEM       (-12)
#define JFS_ERR_TOOLARGE    (-13)
#define JFS_ERR_BADF        (-14)
#define JFS_ERR_FAULT       (-15)

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3, reflected polynomial 0xEDB88320)                 */
/* ------------------------------------------------------------------ */

uint32_t jfs_crc32(const void *data, size_t len);
uint32_t jfs_crc32_update(uint32_t crc, const void *data, size_t len);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t jfs_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline uint64_t jfs_min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

#ifdef __cplusplus
}
#endif

#endif /* JOURNALFS_H */
