#ifndef DEVLINK_PEER_H
#define DEVLINK_PEER_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/session.h"
#include "devlink/transport.h"

/*
 * Mux and control channel (docs/protocol.md, "Mux layer" and "Control
 * channel"): multiplexes numbered channels over one session, with the HELLO
 * handshake, per-channel credit, and round-robin fairness so a bulk channel
 * cannot starve an interactive one. Operations (exec, put, get) sit on top
 * and see only channels and records.
 *
 * Events arrive through handler callbacks, called from inside wl_peer_work()
 * or wl_peer_pump(). Handlers may call any wl_peer_* function.
 */

#define WL_PEER_MAX_CHANNELS 16U
#define WL_PEER_BUILD_MAX 96U

struct wl_hello {
    uint16_t proto;
    uint8_t platform;
    uint32_t caps;
    char build[WL_PEER_BUILD_MAX + 1]; /* NUL-terminated, truncated if longer */
};

struct wl_peer_handlers {
    void *ctx;
    /* A new session started. Every channel of the previous one is gone and
     * its handles are invalid; nothing may be opened until ready(). */
    void (*session)(void *ctx);
    /* The peer's HELLO arrived and is compatible. */
    void (*ready)(void *ctx, const struct wl_hello *peer);
    /* The peer opened channel ch. Answer with wl_peer_accept or
     * wl_peer_reject, now or on a later call. */
    void (*open)(void *ctx, uint16_t ch, uint8_t kind, const uint8_t *args, size_t n);
    /* The answer to our wl_peer_open. On refusal the channel is gone. */
    void (*opened)(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n);
    /* A record on an open channel. */
    void (*record)(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n);
    /* The peer closed ch. Once we have closed it too, the number is free. */
    void (*closed)(void *ctx, uint16_t ch);
    /* The peer sent ERROR, or broke the protocol; the session is unusable. */
    void (*error)(void *ctx, uint16_t code, const char *msg, size_t n);
};

struct wl_peer;

/* is_client picks the channel numbering (client odd, agent even) so both
 * sides can open channels without racing for numbers. platform goes into
 * our HELLO. */
struct wl_peer *wl_peer_new(const struct wl_session_cfg *cfg, uint32_t seed, uint32_t now,
                            int is_client, uint8_t platform,
                            const struct wl_peer_handlers *handlers);
void wl_peer_free(struct wl_peer *p);

/*
 * Presents somebody else's identity in our HELLO. Only the local multiplexer
 * uses it: a client connecting to it must see the agent across the line, not
 * the relay in between. Must be called before the first work or pump, because
 * HELLO goes out with the first of those.
 */
void wl_peer_set_identity(struct wl_peer *p, uint8_t platform, const char *build);

struct wl_session *wl_peer_session(struct wl_peer *p);

/* Runs the mux over whatever the session has: parses and dispatches
 * received records, and feeds queued records into the session. */
wl_status wl_peer_work(struct wl_peer *p);

/* work, one link pump on the transport, work again. */
wl_status wl_peer_pump(struct wl_peer *p, struct wl_transport *t, uint32_t max_wait_ms);

int wl_peer_ready(const struct wl_peer *p);

/* Nonzero once ERROR was sent or received; the code that ended the session. */
uint16_t wl_peer_failed(const struct wl_peer *p);

wl_status wl_peer_open(struct wl_peer *p, uint8_t kind, const uint8_t *args, size_t n,
                       uint16_t *ch_out);
wl_status wl_peer_accept(struct wl_peer *p, uint16_t ch);
wl_status wl_peer_reject(struct wl_peer *p, uint16_t ch, uint16_t code, const char *msg);

/* Queues one whole record, or fails with WL_ERR_NOSPACE if the channel's
 * queue cannot take it right now (try again after the next work call). */
wl_status wl_peer_send(struct wl_peer *p, uint16_t ch, uint8_t type, const uint8_t *data,
                       size_t n);

/* Queues bulk data as records of up to WL_REC_CHUNK bytes; returns how many
 * bytes were taken. */
size_t wl_peer_write(struct wl_peer *p, uint16_t ch, uint8_t type, const uint8_t *data,
                     size_t n);

wl_status wl_peer_close(struct wl_peer *p, uint16_t ch);

/*
 * Counts every record the application has queued. A loop that feeds the peer
 * compares it before and after its turn: if anything was taken, there may be
 * room for more at once, and the next pump should not wait. Over serial the
 * line paces everything and the wait costs nothing; over TCP the round trip
 * is the limit, and a pump that waited 50 ms after each queue-full of data
 * held an upload to a quarter of what the connection could carry.
 */
uint32_t wl_peer_queued(const struct wl_peer *p);

/*
 * Holds back this channel's flow-control credit until the application says
 * the bytes have been used, rather than granting more as soon as a record is
 * handed to the handler.
 *
 * It is for a channel whose records land in a buffer of their own that
 * something else drains: a command's stdin, waiting for the process to read
 * it. Crediting on delivery lets the sender refill faster than that buffer
 * empties, and what does not fit has nowhere to go but the floor. Held, the
 * window and the buffer mean the same thing again.
 *
 * A channel in this mode must report everything it takes, or the sender runs
 * out of credit and stops.
 */
wl_status wl_peer_hold_credit(struct wl_peer *p, uint16_t ch);

/* Reports n bytes of a held channel as used, which may grant the sender more. */
void wl_peer_consumed(struct wl_peer *p, uint16_t ch, size_t n);

/* Nothing queued in the mux and nothing unacknowledged below it. */
int wl_peer_idle(const struct wl_peer *p);

/*
 * How many channel numbers are in use. A channel is released only once both
 * sides have closed it, so this creeping upwards on a long-lived peer means
 * somebody is not closing what they opened. That was invisible while every
 * command owned its own session and the session's death cleaned up after it;
 * the multiplexer keeps one session for days, so it needs the number.
 */
unsigned wl_peer_channels_open(const struct wl_peer *p);

#endif
