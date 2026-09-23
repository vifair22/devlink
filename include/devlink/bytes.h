#ifndef DEVLINK_BYTES_H
#define DEVLINK_BYTES_H

#include <stdint.h>

/* Little-endian field access for wire formats (docs/protocol.md,
 * "Conventions"). Byte-wise, so alignment and host order never matter. */

static inline void wl_put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static inline void wl_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)(v >> 24);
}

static inline void wl_put_u64le(uint8_t *p, uint64_t v)
{
    wl_put_u32le(p, (uint32_t)(v & 0xFFFFFFFFU));
    wl_put_u32le(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t wl_get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static inline uint32_t wl_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t wl_get_u64le(const uint8_t *p)
{
    return (uint64_t)wl_get_u32le(p) | ((uint64_t)wl_get_u32le(p + 4) << 32);
}

#endif
