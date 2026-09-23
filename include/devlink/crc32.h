#ifndef DEVLINK_CRC32_H
#define DEVLINK_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), with the same
 * calling convention as zlib's crc32(): start from 0, feed the previous
 * result back in to continue. Kept separate from zlib so the link layer does
 * not depend on the compression library.
 */
uint32_t wl_crc32(uint32_t crc, const void *buf, size_t len);

#endif
