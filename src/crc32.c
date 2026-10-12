/*
 * crc32.c -- CRC32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * Used to checksum journal records and the superblock.  A CRC32 detects
 * accidental corruption (bit flips, torn writes, stale data) with high
 * probability, but it is NOT a cryptographic hash: an attacker who can
 * rewrite a block can also recompute the CRC.  It also cannot detect a
 * block that was replaced by an older, equally valid copy of itself --
 * that is what the journal generation counter and txid ordering are for.
 */
#include "journalfs.h"

#include <stddef.h>

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            if (c & 1u)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

uint32_t jfs_crc32_update(uint32_t crc, const void *data, size_t len)
{
    if (!crc_table_ready)
        crc32_build_table();

    const uint8_t *p = (const uint8_t *)data;
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t jfs_crc32(const void *data, size_t len)
{
    return jfs_crc32_update(0u, data, len);
}
