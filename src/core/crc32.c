#include "w98link/crc32.h"

/* Byte-at-a-time table lookup. At 115200 bps the link moves about 11 KB/s,
 * so slicing-by-8 would be speed nobody can observe; the table is built on
 * first use to keep 1 KiB of constants out of the source. */
static uint32_t table[256];
static int table_ready;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1U) ? (c >> 1) ^ 0xEDB88320U : c >> 1;
        table[i] = c;
    }
    table_ready = 1;
}

uint32_t wl_crc32(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    if (!table_ready)
        build_table();
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}
