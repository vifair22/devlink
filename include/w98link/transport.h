#ifndef W98LINK_TRANSPORT_H
#define W98LINK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/status.h"

/*
 * A byte pipe to the peer: a serial port now, a TCP socket later. The core
 * only ever sees this interface, so the link layer and everything above it is
 * the same code on Linux and Win32.
 *
 * Reads take a timeout rather than blocking forever because the link layer
 * has timers to run (retransmission, acks, liveness) and the caller's loop
 * uses the timeout as its tick.
 */
struct wl_transport_ops {
    /* Reads up to cap bytes, waiting at most timeout_ms for the first one.
     * A timeout is not an error: it returns WL_OK with *got == 0. */
    wl_status (*read)(void *impl, uint8_t *buf, size_t cap, size_t *got,
                      uint32_t timeout_ms);
    /* Writes all len bytes or fails. */
    wl_status (*write)(void *impl, const uint8_t *buf, size_t len);
    void (*close)(void *impl);
};

struct wl_transport {
    const struct wl_transport_ops *ops;
    void *impl;
};

static inline wl_status wl_transport_read(struct wl_transport *t, uint8_t *buf,
                                          size_t cap, size_t *got,
                                          uint32_t timeout_ms)
{
    return t->ops->read(t->impl, buf, cap, got, timeout_ms);
}

static inline wl_status wl_transport_write(struct wl_transport *t,
                                           const uint8_t *buf, size_t len)
{
    return t->ops->write(t->impl, buf, len);
}

static inline void wl_transport_close(struct wl_transport *t)
{
    if (t->ops != NULL)
        t->ops->close(t->impl);
    t->ops = NULL;
    t->impl = NULL;
}

/* Opens a serial port at baud, 8N1, no flow control. Implemented per
 * platform: path is a device node on Linux and "COM1".."COM9" on Win32. */
wl_status wl_serial_open(const char *path, uint32_t baud, struct wl_transport *out);

#endif
