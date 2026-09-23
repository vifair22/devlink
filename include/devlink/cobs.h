#ifndef DEVLINK_COBS_H
#define DEVLINK_COBS_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/status.h"

/*
 * Consistent Overhead Byte Stuffing. Encoding removes every zero byte, so a
 * zero can delimit frames on the serial line (docs/protocol.md, "Frame
 * encoding"). Neither function writes the delimiter itself.
 */

/* Worst-case encoded size of n input bytes: one code byte per 254. */
#define WL_COBS_MAX_ENCODED(n) ((n) + ((n) / 254U) + 1U)

wl_status wl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                         size_t *out_len);

/* Fails with WL_ERR_CORRUPT on a zero byte or a truncated group, which is how
 * line noise shows up once it has got past the delimiter. */
wl_status wl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                         size_t *out_len);

#endif
