#ifndef W98LINK_LINK_H
#define W98LINK_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/frame.h"
#include "w98link/status.h"

/*
 * Serial link state machine (docs/protocol.md, "Link layer: serial"): turns
 * the lossy frame channel into an ordered, lossless byte stream in each
 * direction, with session sync so either side can restart at any time.
 *
 * It does no I/O and reads no clock. The caller feeds it received bytes and
 * the current time, and drains the frames it wants sent. That keeps it
 * deterministic, which is what lets the tests drive it through a simulated
 * line with loss, corruption and delay in virtual time.
 *
 * Typical loop (see link_io.h for the one both programs use):
 *
 *     wl_link_rx(l, bytes_from_port, n, now);
 *     while ((n = wl_link_poll_tx(l, buf, sizeof(buf), now)) > 0)
 *         write(port, buf, n);
 *     wait for input, at most wl_link_next_timeout(l, now) ms
 */

#define WL_LINK_MAX_WINDOW 32U /* bounded by the 32-bit selective-ack field */
#define WL_LINK_DEFAULT_WINDOW 16U
#define WL_LINK_QUEUE 32768U /* bytes buffered each way above the window */

struct wl_link_cfg {
    uint16_t max_payload; /* offered; the session uses the smaller offer */
    uint8_t window;       /* offered, 1..WL_LINK_MAX_WINDOW */
    /* Line rate in bytes per second, for pacing and for timing frames from
     * when they actually leave rather than when they were queued. 0 turns
     * both off. */
    uint32_t bytes_per_sec;
};

struct wl_link_stats {
    uint32_t data_sent;    /* DATA frames, first transmissions */
    uint32_t retransmits;  /* DATA frames sent again */
    uint32_t timeouts;     /* retransmissions caused by the timer */
    uint32_t fast_retx;    /* retransmissions caused by a selective-ack gap */
    uint32_t resets;       /* sessions lost: peer restart or liveness */
    uint32_t rto_ms;       /* current retransmission timeout */
    struct wl_frame_rx_stats rx;
};

struct wl_link;

/* seed picks this side's first session id and seeds later ones. It must
 * differ between runs (the caller mixes in time and process id). */
struct wl_link *wl_link_new(const struct wl_link_cfg *cfg, uint32_t seed, uint32_t now);
void wl_link_free(struct wl_link *l);

/* Bytes from the transport, in any chunking. */
void wl_link_rx(struct wl_link *l, const uint8_t *data, size_t n, uint32_t now);

/* Writes at most one frame into out (which should hold WL_FRAME_MAX_WIRE
 * bytes) and returns its length, or 0 when nothing is due. Call until 0. */
size_t wl_link_poll_tx(struct wl_link *l, uint8_t *out, size_t cap, uint32_t now);

/* Milliseconds until the link next needs poll_tx called without new input. */
uint32_t wl_link_next_timeout(const struct wl_link *l, uint32_t now);

/* Queues stream bytes for the peer; returns how many fit (0 when the queue
 * is full or the link is not up). */
size_t wl_link_send(struct wl_link *l, const uint8_t *data, size_t n);

/* Takes up to cap bytes of the peer's stream, in order. */
size_t wl_link_recv(struct wl_link *l, uint8_t *buf, size_t cap);

/* Bytes queued or in flight that the peer has not acknowledged. */
size_t wl_link_unacked(const struct wl_link *l);

int wl_link_is_up(const struct wl_link *l);

/*
 * Incremented every time a session comes up. Everything above the link keeps
 * per-session state (compression streams, channels), so it compares this
 * with the value it last saw: a change means the old session is gone and the
 * streams must be reset, whether or not it noticed the link going down.
 */
uint32_t wl_link_generation(const struct wl_link *l);

void wl_link_get_stats(const struct wl_link *l, struct wl_link_stats *out);

#endif
