#ifndef TEST_NOISY_LINE_H
#define TEST_NOISY_LINE_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/transport.h"

/*
 * A transport that damages the bytes passing through it, wrapped around
 * another one.
 *
 * The pty the other tests use is perfect, and a real cable is not: it drops
 * bytes and flips bits, and the link layer answers with a retransmission. That
 * path carries data nothing else tests, so anything above it that mishandles a
 * frame arriving twice, or out of order, only shows up on the cable.
 *
 * The damage is regular rather than random (every Nth byte, alternately
 * dropped and flipped) so that a failure is the same failure on the next run.
 */
struct noisy_line {
    struct wl_transport inner;
    uint32_t every; /* damage one byte in every this many; 0 is a clean line */
    uint64_t seen;
    uint64_t dropped;
    uint64_t flipped;
};

static wl_status noisy_write(void *impl, const uint8_t *buf, size_t len)
{
    struct noisy_line *n = impl;
    uint8_t out[256];
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        n->seen++;
        if (n->every != 0 && n->seen % n->every == 0) {
            if ((n->seen / n->every) % 2U == 0U) {
                n->dropped++;
                continue;
            }
            n->flipped++;
            out[o++] = (uint8_t)(buf[i] ^ 0x40U);
        } else {
            out[o++] = buf[i];
        }
        if (o == sizeof(out)) {
            wl_status s = wl_transport_write(&n->inner, out, o);

            if (s != WL_OK)
                return s;
            o = 0;
        }
    }
    return o > 0 ? wl_transport_write(&n->inner, out, o) : WL_OK;
}

static wl_status noisy_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                            uint32_t timeout_ms)
{
    struct noisy_line *n = impl;

    return wl_transport_read(&n->inner, buf, cap, got, timeout_ms);
}

static void noisy_close(void *impl)
{
    struct noisy_line *n = impl;

    wl_transport_close(&n->inner);
}

static const struct wl_transport_ops noisy_ops = {noisy_read, noisy_write, noisy_close, NULL,
                                                  NULL};

/* Takes over `inner`; close the returned transport, not the original. */
static inline void noisy_line(struct noisy_line *n, struct wl_transport *inner, uint32_t every,
                              struct wl_transport *out)
{
    n->inner = *inner;
    n->every = every;
    n->seen = 0;
    n->dropped = 0;
    n->flipped = 0;
    out->ops = &noisy_ops;
    out->impl = n;
}

#endif
