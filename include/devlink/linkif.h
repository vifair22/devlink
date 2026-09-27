#ifndef DEVLINK_LINKIF_H
#define DEVLINK_LINKIF_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/status.h"
#include "devlink/transport.h"

/*
 * What the session needs from a link, whichever one it is: the serial link
 * (link.h) or the TCP link (tcplink.h). Both are I/O-free state machines of
 * the same shape: bytes from the transport go in through rx, bytes for the
 * transport come out of poll_tx, and a timer says when poll_tx must be called
 * again without new input. Above that, each offers an ordered, lossless byte
 * stream in each direction. Keeping the shape identical is what lets one
 * session, one pump and the simulated-line tests serve both.
 *
 * The one real difference is what happens when a session ends. The serial
 * link resynchronises by itself and a new session follows on the same line.
 * A TCP session is its connection, so once it has ended, or failed to start,
 * the link can never come up again: failed() says so, and the owner closes
 * the connection.
 */
struct wl_link_ops {
    void (*rx)(void *impl, const uint8_t *data, size_t n, uint32_t now);
    /* Writes at most cap bytes of wire output into out; 0 when nothing is due.
     * cap is at least WL_LINKIF_WIRE. Call until it returns 0. */
    size_t (*poll_tx)(void *impl, uint8_t *out, size_t cap, uint32_t now);
    uint32_t (*next_timeout)(const void *impl, uint32_t now);
    size_t (*send)(void *impl, const uint8_t *data, size_t n);
    size_t (*recv)(void *impl, uint8_t *buf, size_t cap);
    size_t (*unacked)(const void *impl);
    int (*is_up)(const void *impl);
    uint32_t (*generation)(const void *impl);
    /* WL_OK while the link can still carry a session; otherwise the reason
     * it never will, with the message recorded by wl_fail(). */
    wl_status (*failed)(const void *impl);
    void (*free)(void *impl);
    /* Optional: how many more bytes rx will take now. A link with its own
     * flow control (serial) leaves it NULL and takes anything, dropping what
     * it cannot hold for the peer to resend. One that relies on TCP's (the
     * TCP link) stops the pump reading while its session is behind, so the
     * backlog waits in the kernel and the peer's sender blocks. */
    size_t (*rx_room)(const void *impl);
};

struct wl_linkif {
    const struct wl_link_ops *ops;
    void *impl;
};

/* The buffer a pump hands poll_tx: enough for one whole serial frame. */
#define WL_LINKIF_WIRE 8192U

/*
 * Glue between a link and a real transport, as both programs' main loops use
 * it: sends whatever is due, waits for input no longer than the link's next
 * timer or max_wait_ms, feeds what arrived, and sends again. Fails when the
 * transport does, or when the link reports it has failed for good.
 */
wl_status wl_linkif_pump(struct wl_linkif *l, struct wl_transport *t, uint32_t max_wait_ms);

#endif
