#include <stdlib.h>
#include <string.h>

#include "devlink/bytes.h"
#include "devlink/tcplink.h"

/*
 * One connection, one session. The link is up from the moment the connection
 * is usable (with a key: once the handshake is done) until it ends, and then
 * it has failed for good: failed() carries the reason, and the owner closes
 * the socket. There is nothing to resynchronise, unlike serial, because TCP
 * cannot lose or reorder a byte; a connection that misbehaves is a peer that
 * is broken, or someone else's.
 */

enum state { ST_UP, ST_FAILED };

/* A linear buffer: data lives in [head, head + len), compacted to the front
 * when the free space at the end runs short. */
struct lbuf {
    uint8_t *data;
    size_t cap;
    size_t head;
    size_t len;
};

struct tcplink {
    struct wl_tcplink_cfg cfg;
    enum state state;
    uint32_t generation;
    char why[128]; /* what ended the session, for failed() */

    struct lbuf in;   /* wire bytes received, not yet a whole message */
    struct lbuf rxq;  /* stream bytes for the session */
    struct lbuf txq;  /* stream bytes from the session, not yet framed */
    struct lbuf out;  /* one framed message, part-written to the transport */

    /* The timers start from the first time the link is driven, not from
     * its creation: callers create sessions with a clock of 0 and drive
     * them with the real one. */
    int started;
    uint32_t last_rx; /* when a valid message last arrived */
    uint32_t last_tx; /* when a message was last framed */
};

static void start(struct tcplink *l, uint32_t now)
{
    if (l->started)
        return;
    l->started = 1;
    l->last_rx = now;
    l->last_tx = now;
}

static int buf_init(struct lbuf *b, size_t cap)
{
    b->data = malloc(cap);
    b->cap = cap;
    b->head = 0;
    b->len = 0;
    return b->data != NULL;
}

static void consume(struct lbuf *b, size_t n)
{
    b->head += n;
    b->len -= n;
    if (b->len == 0)
        b->head = 0;
}

static size_t put(struct lbuf *b, const uint8_t *data, size_t n)
{
    size_t room = b->cap - b->head - b->len;

    if (room < n && b->head > 0) {
        memmove(b->data, b->data + b->head, b->len);
        b->head = 0;
        room = b->cap - b->len;
    }
    if (n > room)
        n = room;
    memcpy(b->data + b->head + b->len, data, n);
    b->len += n;
    return n;
}

static size_t take(struct lbuf *b, uint8_t *out, size_t cap)
{
    size_t n = b->len < cap ? b->len : cap;

    memcpy(out, b->data + b->head, n);
    consume(b, n);
    return n;
}

static void fail(struct tcplink *l, const char *why)
{
    if (l->state == ST_FAILED)
        return;
    l->state = ST_FAILED;
    strncpy(l->why, why, sizeof(l->why) - 1);
    l->why[sizeof(l->why) - 1] = '\0';
}

static uint32_t since(uint32_t now, uint32_t then)
{
    return now - then; /* wraps correctly: both are u32 milliseconds */
}

/* The longest payload one message can carry. */
static size_t max_payload(const struct tcplink *l)
{
    return WL_TCPLINK_MAX_MSG - (l->cfg.psk != NULL ? WL_TCPLINK_TAG : 0U);
}

/* Frames the next message from txq into out; an empty one is a keepalive. */
static void frame(struct tcplink *l, uint32_t now)
{
    size_t n = l->txq.len < max_payload(l) ? l->txq.len : max_payload(l);

    l->out.head = 0;
    wl_put_u16le(l->out.data, (uint16_t)n);
    memcpy(l->out.data + 2, l->txq.data + l->txq.head, n);
    consume(&l->txq, n);
    l->out.len = n + 2;
    l->last_tx = now;
}

/* ---------------------------------------------------------------------------
 * link interface
 * ------------------------------------------------------------------------- */

/* Nonzero while a whole message is waiting for the session to make room. */
static int blocked(const struct tcplink *l)
{
    size_t len;

    if (l->in.len < 2)
        return 0;
    len = wl_get_u16le(l->in.data + l->in.head);
    return l->in.len >= 2 + len && len > l->rxq.cap - l->rxq.len;
}

/* Moves every whole message that fits from the wire buffer to the stream. */
static void parse(struct tcplink *l, uint32_t now)
{
    while (l->in.len >= 2 && !blocked(l)) {
        size_t len = wl_get_u16le(l->in.data + l->in.head);

        if (l->in.len < 2 + len)
            return;
        consume(&l->in, 2);
        (void)put(&l->rxq, l->in.data + l->in.head, len);
        consume(&l->in, len);
        l->last_rx = now;
    }
}

static void l_rx(void *impl, const uint8_t *data, size_t n, uint32_t now)
{
    struct tcplink *l = impl;

    if (l->state == ST_FAILED)
        return;
    start(l, now);
    if (put(&l->in, data, n) != n) {
        /* rx_room() bounds what the pump reads, so this is a caller bug. */
        fail(l, "tcp link: received more than it had room for");
        return;
    }
    parse(l, now);
}

static size_t l_rx_room(const void *impl)
{
    const struct tcplink *l = impl;

    return l->in.cap - l->in.len;
}

static size_t l_poll_tx(void *impl, uint8_t *out, size_t cap, uint32_t now)
{
    struct tcplink *l = impl;

    if (l->state == ST_FAILED)
        return 0;
    start(l, now);
    /* While the session is behind, the pump stops reading and the peer's
     * keepalives wait unread; that is our silence, not the peer's. */
    if (blocked(l))
        l->last_rx = now;
    if (since(now, l->last_rx) >= l->cfg.dead_ms) {
        fail(l, "tcp link: nothing from the peer for too long; it has gone");
        return 0;
    }
    if (l->out.len == 0 &&
        (l->txq.len > 0 || since(now, l->last_tx) >= l->cfg.keepalive_ms))
        frame(l, now);
    return take(&l->out, out, cap);
}

static uint32_t l_next_timeout(const void *impl, uint32_t now)
{
    const struct tcplink *l = impl;
    uint32_t dead, keep;

    if (l->state == ST_FAILED)
        return UINT32_MAX;
    if (!l->started || l->out.len > 0 || l->txq.len > 0)
        return 0;
    dead = l->cfg.dead_ms - since(now, l->last_rx);
    keep = since(now, l->last_tx) >= l->cfg.keepalive_ms
               ? 0
               : l->cfg.keepalive_ms - since(now, l->last_tx);
    if (since(now, l->last_rx) >= l->cfg.dead_ms)
        dead = 0;
    return dead < keep ? dead : keep;
}

static size_t l_send(void *impl, const uint8_t *data, size_t n)
{
    struct tcplink *l = impl;

    if (l->state != ST_UP)
        return 0;
    return put(&l->txq, data, n);
}

static size_t l_recv(void *impl, uint8_t *buf, size_t cap)
{
    struct tcplink *l = impl;
    size_t n = take(&l->rxq, buf, cap);

    /* Room just made may let a waiting message through. Its arrival time is
     * the last one seen, which is all the dead timer needs. */
    if (n > 0)
        parse(l, l->last_rx);
    return n;
}

static size_t l_unacked(const void *impl)
{
    const struct tcplink *l = impl;

    return l->txq.len + l->out.len;
}

static int l_is_up(const void *impl)
{
    return ((const struct tcplink *)impl)->state == ST_UP;
}

static uint32_t l_generation(const void *impl)
{
    return ((const struct tcplink *)impl)->generation;
}

static wl_status l_failed(const void *impl)
{
    const struct tcplink *l = impl;

    if (l->state != ST_FAILED)
        return WL_OK;
    return wl_fail(WL_ERR_IO, "%s", l->why);
}

static void l_free(void *impl)
{
    struct tcplink *l = impl;

    if (l == NULL)
        return;
    free(l->in.data);
    free(l->rxq.data);
    free(l->txq.data);
    free(l->out.data);
    free(l);
}

static const struct wl_link_ops tcp_ops = {
    l_rx,    l_poll_tx,    l_next_timeout, l_send, l_recv,   l_unacked,
    l_is_up, l_generation, l_failed,       l_free, l_rx_room,
};

void wl_tcplink_cfg_init(struct wl_tcplink_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->dead_ms = WL_TCPLINK_DEFAULT_DEAD_MS;
    cfg->keepalive_ms = WL_TCPLINK_DEFAULT_KEEPALIVE_MS;
}

wl_status wl_tcplink_make(void *arg, uint32_t seed, uint32_t now, struct wl_linkif *out)
{
    const struct wl_tcplink_cfg *cfg = arg;
    struct tcplink *l;

    (void)seed;
    if (cfg->psk != NULL)
        return wl_fail(WL_ERR_INVAL, "tcp link: encryption is not built yet");
    l = calloc(1, sizeof(*l));
    if (l == NULL)
        return wl_fail(WL_ERR_NOMEM, "tcp link: out of memory");
    l->cfg = *cfg;
    if (l->cfg.dead_ms == 0)
        l->cfg.dead_ms = WL_TCPLINK_DEFAULT_DEAD_MS;
    if (l->cfg.keepalive_ms == 0)
        l->cfg.keepalive_ms = WL_TCPLINK_DEFAULT_KEEPALIVE_MS;
    if (!buf_init(&l->in, WL_TCPLINK_MAX_MSG + 2U) ||
        !buf_init(&l->rxq, WL_TCPLINK_QUEUE + WL_TCPLINK_MAX_MSG) ||
        !buf_init(&l->txq, WL_TCPLINK_QUEUE) || !buf_init(&l->out, WL_TCPLINK_MAX_MSG + 2U)) {
        l_free(l);
        return wl_fail(WL_ERR_NOMEM, "tcp link: out of memory");
    }
    (void)now;
    l->state = ST_UP;
    l->generation = 1;
    out->ops = &tcp_ops;
    out->impl = l;
    return WL_OK;
}
