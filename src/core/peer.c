#include <stdlib.h>
#include <string.h>

#include "devlink/link_io.h"
#include "devlink/peer.h"
#include "devlink/proto.h"
#include "devlink/version.h"
#include "devlink/wire.h"

/* Serialized records waiting for the session, per channel. Big enough for
 * one maximum-size record plus a few bulk chunks behind it. */
#define CHAN_QUEUE (WL_REC_HEADER + WL_REC_MAX_PAYLOAD + (4U * WL_REC_CHUNK))
#define CTL_QUEUE 8192U
#define NO_SOURCE (-2)
#define CTL_SOURCE (-1)

struct queue {
    uint8_t *buf;
    size_t cap;
    size_t head;
    size_t len;
};

enum chan_state { CH_FREE, CH_OPENING, CH_INCOMING, CH_OPEN };

struct chan {
    enum chan_state state;
    uint16_t id;
    uint8_t kind;
    int we_closed;
    int peer_closed;
    uint32_t credit;  /* bytes the peer lets us send on this channel */
    uint32_t granted; /* bytes we have let the peer send, not yet received */
    uint32_t consumed; /* received since our last WINDOW */
    struct queue q;
};

struct wl_peer {
    struct wl_session *s;
    struct wl_peer_handlers h;
    int is_client;
    uint8_t platform;
    char build[WL_PEER_BUILD_MAX + 1]; /* empty: report wl_version() */
    uint32_t gen;
    int hello_sent;
    int ready;
    uint16_t failed;
    struct wl_hello peer_hello;
    uint16_t next_id;

    struct chan chans[WL_PEER_MAX_CHANNELS];
    struct queue ctl;
    unsigned rr; /* round-robin position among channels */
    int cur;     /* source of the record being pushed: channel index, CTL or none */
    size_t cur_left;

    uint8_t rec[WL_REC_HEADER + WL_REC_MAX_PAYLOAD];
    size_t rec_have;
};

/* ---------------------------------------------------------------------------
 * queues
 * ------------------------------------------------------------------------- */

static int queue_init(struct queue *q, size_t cap)
{
    q->buf = malloc(cap);
    q->cap = cap;
    q->head = q->len = 0;
    return q->buf != NULL;
}

static void queue_clear(struct queue *q)
{
    q->head = q->len = 0;
}

/* Appends one record whole, or nothing. */
static int queue_record(struct queue *q, uint16_t ch, uint8_t type, const uint8_t *p,
                        size_t n)
{
    uint8_t hdr[WL_REC_HEADER];

    if (n > WL_REC_MAX_PAYLOAD || q->cap - q->len < WL_REC_HEADER + n)
        return 0;
    if (q->cap - q->head - q->len < WL_REC_HEADER + n) {
        memmove(q->buf, q->buf + q->head, q->len);
        q->head = 0;
    }
    wl_put_u16le(hdr, ch);
    hdr[2] = type;
    wl_put_u16le(hdr + 3, (uint16_t)n);
    memcpy(q->buf + q->head + q->len, hdr, WL_REC_HEADER);
    if (n > 0)
        memcpy(q->buf + q->head + q->len + WL_REC_HEADER, p, n);
    q->len += WL_REC_HEADER + n;
    return 1;
}

static size_t queued_payload(const struct queue *q)
{
    return wl_get_u16le(q->buf + q->head + 3);
}

/* ---------------------------------------------------------------------------
 * channels
 * ------------------------------------------------------------------------- */

static struct chan *find_chan(struct wl_peer *p, uint16_t id)
{
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++) {
        if (p->chans[i].state != CH_FREE && p->chans[i].id == id)
            return &p->chans[i];
    }
    return NULL;
}

static struct chan *alloc_chan(struct wl_peer *p, uint16_t id, enum chan_state state,
                               uint8_t kind)
{
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++) {
        struct chan *c = &p->chans[i];

        if (c->state != CH_FREE)
            continue;
        c->state = state;
        c->id = id;
        c->kind = kind;
        c->we_closed = c->peer_closed = 0;
        c->credit = WL_CREDIT_INITIAL;
        c->granted = WL_CREDIT_INITIAL;
        c->consumed = 0;
        queue_clear(&c->q);
        return c;
    }
    return NULL;
}

static void free_chan(struct wl_peer *p, struct chan *c)
{
    /* A record half pushed into the session must still be finished, or the
     * stream would desynchronise; only an untouched queue can be dropped. */
    if (p->cur == (int)(c - p->chans))
        return;
    c->state = CH_FREE;
    queue_clear(&c->q);
}

static int our_parity(const struct wl_peer *p, uint16_t id)
{
    return (id & 1U) == (p->is_client ? 1U : 0U);
}

/* ---------------------------------------------------------------------------
 * control records
 * ------------------------------------------------------------------------- */

static int send_ctl(struct wl_peer *p, uint8_t type, const uint8_t *payload, size_t n)
{
    return queue_record(&p->ctl, WL_CONTROL_CHANNEL, type, payload, n);
}

static void fail(struct wl_peer *p, uint16_t code, const char *msg, int tell_peer)
{
    uint8_t buf[256];
    struct wl_wbuf w;
    size_t n = strlen(msg);

    if (p->failed)
        return;
    p->failed = code;
    if (tell_peer) {
        wl_wbuf_init(&w, buf, sizeof(buf));
        wl_w_u16(&w, code);
        wl_w_str(&w, msg, n < 200 ? n : 200);
        (void)send_ctl(p, WL_CTL_ERROR, buf, w.len);
    }
    if (p->h.error != NULL)
        p->h.error(p->h.ctx, code, msg, n);
}

static void send_hello(struct wl_peer *p)
{
    uint8_t buf[WL_PEER_BUILD_MAX + 16];
    struct wl_wbuf w;
    /* Normally this program's own identity. The local multiplexer overrides it
     * so that a client talking to it sees the agent on the far end of the line
     * rather than the relay in between, which is what the client asked about. */
    const char *v = p->build[0] != '\0' ? p->build : wl_version();
    size_t n = strlen(v);

    wl_wbuf_init(&w, buf, sizeof(buf));
    wl_w_u16(&w, WL_PROTO_VERSION);
    wl_w_str(&w, v, n < WL_PEER_BUILD_MAX ? n : WL_PEER_BUILD_MAX);
    wl_w_u8(&w, p->platform);
    wl_w_u32(&w, 0); /* capabilities: bit 0 (auth) reserved, 0 in v1 */
    (void)send_ctl(p, WL_CTL_HELLO, buf, w.len);
    p->hello_sent = 1;
}

static void send_window(struct wl_peer *p, struct chan *c)
{
    uint8_t buf[6];

    wl_put_u16le(buf, c->id);
    wl_put_u32le(buf + 2, c->consumed);
    if (send_ctl(p, WL_CTL_WINDOW, buf, sizeof(buf))) {
        c->granted += c->consumed;
        c->consumed = 0;
    }
}

static void on_hello(struct wl_peer *p, struct wl_rbuf *r)
{
    struct wl_hello *h = &p->peer_hello;
    const char *build;
    size_t n;

    h->proto = wl_r_u16(r);
    build = wl_r_str(r, &n);
    h->platform = wl_r_u8(r);
    h->caps = wl_r_u32(r);
    /* Trailing bytes are allowed: a later version may append fields. */
    if (r->bad) {
        fail(p, WL_E_MALFORMED, "malformed HELLO", 1);
        return;
    }
    if (n > WL_PEER_BUILD_MAX)
        n = WL_PEER_BUILD_MAX;
    memcpy(h->build, build, n);
    h->build[n] = '\0';
    if (h->proto != WL_PROTO_VERSION) {
        /* The higher version reports it (docs/protocol.md, "Handshake"). */
        fail(p, WL_E_VERSION, "protocol version mismatch", h->proto < WL_PROTO_VERSION);
        return;
    }
    p->ready = 1;
    if (p->h.ready != NULL)
        p->h.ready(p->h.ctx, h);
}

static void on_open(struct wl_peer *p, struct wl_rbuf *r)
{
    uint16_t id = wl_r_u16(r);
    uint8_t kind = wl_r_u8(r);
    uint8_t buf[64];
    struct wl_wbuf w;
    static const char full[] = "no free channel";

    if (r->bad || id == WL_CONTROL_CHANNEL || our_parity(p, id) || find_chan(p, id)) {
        fail(p, WL_E_MALFORMED, "bad OPEN", 1);
        return;
    }
    if (alloc_chan(p, id, CH_INCOMING, kind) == NULL) {
        wl_wbuf_init(&w, buf, sizeof(buf));
        wl_w_u16(&w, id);
        wl_w_u16(&w, WL_E_UNKNOWN);
        wl_w_str(&w, full, sizeof(full) - 1);
        (void)send_ctl(p, WL_CTL_OPEN_FAIL, buf, w.len);
        return;
    }
    if (p->h.open != NULL)
        p->h.open(p->h.ctx, id, kind, r->p + r->pos, r->len - r->pos);
}

static void on_control(struct wl_peer *p, uint8_t type, const uint8_t *pl, size_t n)
{
    struct wl_rbuf r;
    struct chan *c;
    uint16_t id;

    wl_rbuf_init(&r, pl, n);
    if (type == WL_CTL_HELLO) {
        on_hello(p, &r);
        return;
    }
    if (type == WL_CTL_ERROR) {
        uint16_t code = wl_r_u16(&r);
        size_t len;
        const char *msg = wl_r_str(&r, &len);

        if (!p->failed) {
            p->failed = code != 0 ? code : WL_E_MALFORMED;
            if (p->h.error != NULL)
                p->h.error(p->h.ctx, p->failed, msg != NULL ? msg : "", len);
        }
        return;
    }
    if (!p->ready) {
        fail(p, WL_E_MALFORMED, "record before HELLO", 1);
        return;
    }
    switch (type) {
    case WL_CTL_OPEN:
        on_open(p, &r);
        return;
    case WL_CTL_OPEN_OK:
        id = wl_r_u16(&r);
        c = find_chan(p, id);
        if (!wl_r_done(&r))
            break;
        if (c == NULL || c->state != CH_OPENING)
            return; /* stale: a channel from before a session reset */
        c->state = CH_OPEN;
        if (p->h.opened != NULL)
            p->h.opened(p->h.ctx, id, 1, 0, "", 0);
        return;
    case WL_CTL_OPEN_FAIL: {
        uint16_t code;
        size_t len;
        const char *msg;

        id = wl_r_u16(&r);
        code = wl_r_u16(&r);
        msg = wl_r_str(&r, &len);
        c = find_chan(p, id);
        if (r.bad)
            break;
        if (c == NULL || c->state != CH_OPENING)
            return;
        free_chan(p, c);
        if (p->h.opened != NULL)
            p->h.opened(p->h.ctx, id, 0, code, msg, len);
        return;
    }
    case WL_CTL_CLOSE:
        id = wl_r_u16(&r);
        c = find_chan(p, id);
        if (!wl_r_done(&r))
            break;
        if (c == NULL)
            return; /* both sides closed at once; the number is already free */
        c->peer_closed = 1;
        if (p->h.closed != NULL)
            p->h.closed(p->h.ctx, id);
        if (c->we_closed)
            free_chan(p, c);
        return;
    case WL_CTL_WINDOW:
        id = wl_r_u16(&r);
        {
            uint32_t add = wl_r_u32(&r);

            c = find_chan(p, id);
            if (!wl_r_done(&r))
                break;
            if (c != NULL)
                c->credit += add;
        }
        return;
    case WL_CTL_PING:
        if (n == 4)
            (void)send_ctl(p, WL_CTL_PONG, pl, n);
        return;
    case WL_CTL_PONG:
        return;
    default:
        break;
    }
    fail(p, WL_E_MALFORMED, "malformed control record", 1);
}

static void on_channel_record(struct wl_peer *p, uint16_t id, uint8_t type,
                              const uint8_t *pl, size_t n)
{
    struct chan *c = find_chan(p, id);

    /* Records for a channel we have already closed and freed can still be
     * in flight; they are dropped, not an error. */
    if (c == NULL || c->state == CH_OPENING)
        return;
    if (n > c->granted) {
        fail(p, WL_E_CREDIT, "flow-control credit exceeded", 1);
        return;
    }
    c->granted -= (uint32_t)n;
    c->consumed += (uint32_t)n;
    if (c->consumed >= WL_CREDIT_TOPUP)
        send_window(p, c);
    if (p->h.record != NULL && !c->we_closed)
        p->h.record(p->h.ctx, id, type, pl, n);
}

/* ---------------------------------------------------------------------------
 * pumping records in and out of the session
 * ------------------------------------------------------------------------- */

static void work_rx(struct wl_peer *p)
{
    while (!p->failed) {
        size_t need = WL_REC_HEADER;
        size_t len = 0;

        if (p->rec_have >= WL_REC_HEADER) {
            len = wl_get_u16le(p->rec + 3);
            if (len > WL_REC_MAX_PAYLOAD) {
                fail(p, WL_E_MALFORMED, "record too long", 1);
                return;
            }
            need = WL_REC_HEADER + len;
        }
        if (p->rec_have < need) {
            size_t n = wl_session_recv(p->s, p->rec + p->rec_have, need - p->rec_have);

            if (n == 0)
                return;
            p->rec_have += n;
            continue;
        }
        p->rec_have = 0;
        {
            uint16_t id = wl_get_u16le(p->rec);
            uint8_t type = p->rec[2];

            if (id == WL_CONTROL_CHANNEL)
                on_control(p, type, p->rec + WL_REC_HEADER, len);
            else if (!p->ready)
                fail(p, WL_E_MALFORMED, "record before HELLO", 1);
            else
                on_channel_record(p, id, type, p->rec + WL_REC_HEADER, len);
        }
    }
}

static struct queue *source_queue(struct wl_peer *p, int src)
{
    return src == CTL_SOURCE ? &p->ctl : &p->chans[src].q;
}

/* Picks the next record to push: control first, then channels round-robin,
 * skipping any whose next record exceeds its credit. */
static int pick_source(struct wl_peer *p)
{
    if (p->ctl.len > 0)
        return CTL_SOURCE;
    for (unsigned k = 0; k < WL_PEER_MAX_CHANNELS; k++) {
        unsigned i = (p->rr + k) % WL_PEER_MAX_CHANNELS;
        struct chan *c = &p->chans[i];
        size_t n;

        if (c->state == CH_FREE || c->q.len == 0)
            continue;
        n = queued_payload(&c->q);
        if (n > c->credit)
            continue;
        c->credit -= (uint32_t)n;
        p->rr = i + 1;
        return (int)i;
    }
    return NO_SOURCE;
}

static void work_tx(struct wl_peer *p)
{
    for (;;) {
        struct queue *q;
        size_t n;

        if (p->cur == NO_SOURCE) {
            p->cur = pick_source(p);
            if (p->cur == NO_SOURCE)
                return;
            q = source_queue(p, p->cur);
            p->cur_left = WL_REC_HEADER + queued_payload(q);
        }
        q = source_queue(p, p->cur);
        n = wl_session_send(p->s, q->buf + q->head, p->cur_left);
        q->head += n;
        q->len -= n;
        p->cur_left -= n;
        if (p->cur_left > 0)
            return; /* session full */
        if (p->cur >= 0) {
            struct chan *c = &p->chans[p->cur];

            p->cur = NO_SOURCE;
            /* A channel closed by both sides waits for its last record. */
            if (c->we_closed && c->peer_closed && c->q.len == 0)
                free_chan(p, c);
        } else {
            p->cur = NO_SOURCE;
        }
    }
}

static void new_session(struct wl_peer *p)
{
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++) {
        p->chans[i].state = CH_FREE;
        queue_clear(&p->chans[i].q);
    }
    queue_clear(&p->ctl);
    p->cur = NO_SOURCE;
    p->cur_left = 0;
    p->rec_have = 0;
    p->ready = 0;
    p->failed = 0;
    p->hello_sent = 0;
    p->next_id = p->is_client ? 1U : 2U;
    if (p->h.session != NULL)
        p->h.session(p->h.ctx);
    send_hello(p);
}

wl_status wl_peer_work(struct wl_peer *p)
{
    wl_status st = wl_session_work(p->s);

    if (st != WL_OK)
        return st;
    if (!wl_session_is_up(p->s))
        return WL_OK;
    if (wl_session_generation(p->s) != p->gen) {
        p->gen = wl_session_generation(p->s);
        new_session(p);
    }
    work_tx(p);
    work_rx(p);
    work_tx(p);
    return wl_session_work(p->s);
}

wl_status wl_peer_pump(struct wl_peer *p, struct wl_transport *t, uint32_t max_wait_ms)
{
    wl_status before = wl_peer_work(p);
    wl_status io;
    wl_status after;

    /* The link is pumped even when the streams above it have failed: a
     * broken session is only cleared by the link seeing the peer's next
     * SYNC, and that needs the link to keep running. */
    io = wl_link_pump(wl_session_link(p->s), t, max_wait_ms);
    if (io != WL_OK)
        return io;
    after = wl_peer_work(p);
    return before != WL_OK ? before : after;
}

/* ---------------------------------------------------------------------------
 * public
 * ------------------------------------------------------------------------- */

struct wl_peer *wl_peer_new(const struct wl_session_cfg *cfg, uint32_t seed, uint32_t now,
                            int is_client, uint8_t platform,
                            const struct wl_peer_handlers *handlers)
{
    struct wl_peer *p = calloc(1, sizeof(*p));
    int ok;

    if (p == NULL) {
        (void)wl_fail(WL_ERR_NOMEM, "peer: out of memory");
        return NULL;
    }
    ok = queue_init(&p->ctl, CTL_QUEUE);
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS && ok; i++)
        ok = queue_init(&p->chans[i].q, CHAN_QUEUE);
    p->s = ok ? wl_session_new(cfg, seed, now) : NULL;
    if (p->s == NULL) {
        wl_peer_free(p);
        (void)wl_fail(WL_ERR_NOMEM, "peer: out of memory");
        return NULL;
    }
    p->h = *handlers;
    p->is_client = is_client;
    p->platform = platform;
    p->cur = NO_SOURCE;
    return p;
}

void wl_peer_free(struct wl_peer *p)
{
    if (p == NULL)
        return;
    wl_session_free(p->s);
    free(p->ctl.buf);
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++)
        free(p->chans[i].q.buf);
    free(p);
}

void wl_peer_set_identity(struct wl_peer *p, uint8_t platform, const char *build)
{
    size_t n = strlen(build);

    p->platform = platform;
    if (n > WL_PEER_BUILD_MAX)
        n = WL_PEER_BUILD_MAX;
    memcpy(p->build, build, n);
    p->build[n] = '\0';
}

struct wl_session *wl_peer_session(struct wl_peer *p)
{
    return p->s;
}

int wl_peer_ready(const struct wl_peer *p)
{
    return p->ready && !p->failed && wl_session_is_up(p->s) &&
           wl_session_generation(p->s) == p->gen;
}

uint16_t wl_peer_failed(const struct wl_peer *p)
{
    return p->failed;
}

wl_status wl_peer_open(struct wl_peer *p, uint8_t kind, const uint8_t *args, size_t n,
                       uint16_t *ch_out)
{
    uint8_t buf[WL_REC_MAX_PAYLOAD];
    struct wl_wbuf w;
    struct chan *c;
    uint16_t id;

    if (!wl_peer_ready(p))
        return wl_fail(WL_ERR_INVAL, "peer: not ready");
    /* Skip numbers still in use after a wrap; the table is small, so a
     * free one turns up quickly if any exists. */
    id = p->next_id;
    for (unsigned tries = 0; find_chan(p, id) != NULL && tries < 64; tries++)
        id = (uint16_t)(id + 2U) != 0 ? (uint16_t)(id + 2U) : (uint16_t)(p->is_client ? 1U : 2U);
    wl_wbuf_init(&w, buf, sizeof(buf));
    wl_w_u16(&w, id);
    wl_w_u8(&w, kind);
    wl_w_bytes(&w, args, n);
    if (w.overflow)
        return wl_fail(WL_ERR_INVAL, "peer: OPEN arguments too long");
    c = alloc_chan(p, id, CH_OPENING, kind);
    if (c == NULL)
        return wl_fail(WL_ERR_NOSPACE, "peer: no free channel");
    if (!send_ctl(p, WL_CTL_OPEN, buf, w.len)) {
        free_chan(p, c);
        return wl_fail(WL_ERR_NOSPACE, "peer: control queue full");
    }
    p->next_id = (uint16_t)(id + 2U);
    *ch_out = id;
    return WL_OK;
}

wl_status wl_peer_accept(struct wl_peer *p, uint16_t ch)
{
    struct chan *c = find_chan(p, ch);
    uint8_t buf[2];

    if (c == NULL || c->state != CH_INCOMING)
        return wl_fail(WL_ERR_INVAL, "peer: no pending OPEN on channel %u", (unsigned)ch);
    wl_put_u16le(buf, ch);
    if (!send_ctl(p, WL_CTL_OPEN_OK, buf, sizeof(buf)))
        return wl_fail(WL_ERR_NOSPACE, "peer: control queue full");
    c->state = CH_OPEN;
    return WL_OK;
}

wl_status wl_peer_reject(struct wl_peer *p, uint16_t ch, uint16_t code, const char *msg)
{
    struct chan *c = find_chan(p, ch);
    uint8_t buf[512];
    struct wl_wbuf w;
    size_t n = strlen(msg);

    if (c == NULL || c->state != CH_INCOMING)
        return wl_fail(WL_ERR_INVAL, "peer: no pending OPEN on channel %u", (unsigned)ch);
    wl_wbuf_init(&w, buf, sizeof(buf));
    wl_w_u16(&w, ch);
    wl_w_u16(&w, code);
    wl_w_str(&w, msg, n < 400 ? n : 400);
    if (!send_ctl(p, WL_CTL_OPEN_FAIL, buf, w.len))
        return wl_fail(WL_ERR_NOSPACE, "peer: control queue full");
    free_chan(p, c);
    return WL_OK;
}

wl_status wl_peer_send(struct wl_peer *p, uint16_t ch, uint8_t type, const uint8_t *data,
                       size_t n)
{
    struct chan *c = find_chan(p, ch);

    if (c == NULL || c->state != CH_OPEN || c->we_closed)
        return wl_fail(WL_ERR_INVAL, "peer: channel %u is not open", (unsigned)ch);
    if (!queue_record(&c->q, ch, type, data, n))
        return WL_ERR_NOSPACE;
    return WL_OK;
}

size_t wl_peer_write(struct wl_peer *p, uint16_t ch, uint8_t type, const uint8_t *data,
                     size_t n)
{
    size_t done = 0;

    while (done < n) {
        size_t k = n - done < WL_REC_CHUNK ? n - done : WL_REC_CHUNK;

        if (wl_peer_send(p, ch, type, data + done, k) != WL_OK)
            break;
        done += k;
    }
    return done;
}

wl_status wl_peer_close(struct wl_peer *p, uint16_t ch)
{
    struct chan *c = find_chan(p, ch);
    uint8_t buf[2];

    if (c == NULL || c->we_closed)
        return WL_OK;
    if (c->state == CH_OPENING)
        return wl_fail(WL_ERR_INVAL, "peer: channel %u has no answer to its OPEN yet",
                       (unsigned)ch);
    wl_put_u16le(buf, ch);
    /* CLOSE travels on the control channel, which has priority, so it must
     * not overtake this channel's queued records: those go first. */
    if (c->q.len > 0 || p->cur == (int)(c - p->chans))
        return WL_ERR_NOSPACE;
    if (!send_ctl(p, WL_CTL_CLOSE, buf, sizeof(buf)))
        return WL_ERR_NOSPACE;
    c->we_closed = 1;
    if (c->peer_closed || c->state != CH_OPEN)
        free_chan(p, c);
    return WL_OK;
}

unsigned wl_peer_channels_open(const struct wl_peer *p)
{
    unsigned n = 0;

    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++) {
        if (p->chans[i].state != CH_FREE)
            n++;
    }
    return n;
}

int wl_peer_idle(const struct wl_peer *p)
{
    if (p->ctl.len > 0 || p->cur != NO_SOURCE)
        return 0;
    for (unsigned i = 0; i < WL_PEER_MAX_CHANNELS; i++) {
        if (p->chans[i].state != CH_FREE && p->chans[i].q.len > 0)
            return 0;
    }
    return !wl_session_busy(p->s);
}
