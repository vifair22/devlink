#include <stdlib.h>
#include <string.h>

#include "w98link/bytes.h"
#include "w98link/link.h"

/* Timer constants from docs/protocol.md, "Session start", "Reliability" and
 * "Liveness". */
#define SYNC_INTERVAL_MS 500U
#define ACK_DELAY_MS 20U
#define KEEPALIVE_MS 2000U
#define DEAD_MS 10000U
#define RTO_INIT_MS 500U
#define RTO_MIN_MS 200U
#define RTO_MAX_MS 5000U
#define TIMEOUT_CAP_MS 1000U

#define SYNC_PAYLOAD 7U      /* u32 id, u16 max_payload, u8 window */
#define SYNC_ACK_PAYLOAD 11U /* u32 echoed id, then a SYNC payload */
#define MIN_PAYLOAD_OFFER 16U

enum state { ST_SYNCING, ST_UP };

struct txslot {
    uint8_t in_use;
    uint8_t acked;         /* selectively acked, waiting for the base to pass */
    uint8_t retransmitted; /* Karn: never take an RTT sample from these */
    uint8_t fast_pending;  /* a selective-ack gap asked for an early resend */
    uint8_t fast_done;     /* only one early resend per frame */
    uint16_t len;
    uint32_t left_at;  /* estimated time the last byte leaves the port */
    uint32_t deadline; /* resend if not acked by then */
    uint8_t data[WL_SERIAL_MAX_PAYLOAD];
};

struct rxslot {
    uint8_t present;
    uint16_t len;
    uint8_t data[WL_SERIAL_MAX_PAYLOAD];
};

struct ring {
    uint8_t buf[WL_LINK_QUEUE];
    size_t head;
    size_t len;
};

struct wl_link {
    struct wl_link_cfg cfg;
    enum state state;
    uint32_t rng;
    uint32_t my_id;
    uint32_t peer_id;
    int have_peer;
    int my_sync_acked;
    int sync_ack_due;
    uint32_t next_sync_at;
    uint16_t peer_max_payload;
    uint8_t peer_window;
    uint16_t max_payload; /* negotiated */
    uint8_t window;       /* negotiated */
    uint32_t generation;

    /* send side */
    uint16_t base;     /* oldest unacknowledged seq */
    uint16_t next_seq; /* seq of the next new frame */
    struct txslot tx[WL_LINK_MAX_WINDOW];
    struct ring txq;
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto;
    int have_rtt;
    uint32_t line_free_at; /* when the port will have sent everything queued */
    uint32_t last_tx;

    /* receive side */
    uint16_t expected; /* next seq to deliver */
    struct rxslot rx[WL_LINK_MAX_WINDOW];
    struct ring rxq;
    unsigned ack_pending; /* frames delivered since the last ack we sent */
    int ack_now;
    int ack_timer;
    uint32_t ack_due_at;
    uint32_t last_rx;

    struct wl_frame_rx frx;
    uint32_t now; /* time of the wl_link_rx call, for the frame callback */
    struct wl_link_stats stats;
};

/* ---------------------------------------------------------------------------
 * small helpers
 * ------------------------------------------------------------------------- */

/* Wrap-safe comparisons without signed casts: a is "before" b when the
 * forward distance from b to a is more than half the number space. */
static int time_before(uint32_t a, uint32_t b)
{
    return a - b >= 0x80000000U;
}

static uint16_t seq_diff(uint16_t a, uint16_t b)
{
    return (uint16_t)(a - b);
}

static int seq_before(uint16_t a, uint16_t b)
{
    return seq_diff(a, b) >= 0x8000U;
}

static uint32_t time_until(uint32_t deadline, uint32_t now)
{
    return time_before(now, deadline) ? deadline - now : 0;
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static uint32_t next_random(struct wl_link *l)
{
    uint32_t x = l->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    l->rng = x;
    return x;
}

static size_t ring_put(struct ring *r, const uint8_t *p, size_t n)
{
    size_t tail;
    size_t first;

    n = min_size(n, WL_LINK_QUEUE - r->len);
    tail = (r->head + r->len) % WL_LINK_QUEUE;
    first = min_size(n, WL_LINK_QUEUE - tail);
    memcpy(r->buf + tail, p, first);
    memcpy(r->buf, p + first, n - first);
    r->len += n;
    return n;
}

static size_t ring_get(struct ring *r, uint8_t *p, size_t n)
{
    size_t first;

    n = min_size(n, r->len);
    first = min_size(n, WL_LINK_QUEUE - r->head);
    memcpy(p, r->buf + r->head, first);
    memcpy(p + first, r->buf, n - first);
    r->head = (r->head + n) % WL_LINK_QUEUE;
    r->len -= n;
    return n;
}

static uint32_t wire_time_ms(const struct wl_link *l, size_t bytes)
{
    if (l->cfg.bytes_per_sec == 0)
        return 0;
    return (uint32_t)(((bytes * 1000U) + l->cfg.bytes_per_sec - 1U) / l->cfg.bytes_per_sec);
}

static uint16_t in_flight(const struct wl_link *l)
{
    return seq_diff(l->next_seq, l->base);
}

/* ---------------------------------------------------------------------------
 * session state
 * ------------------------------------------------------------------------- */

/* Drops everything that belongs to the current session. Stream bytes queued
 * or buffered here were produced by the old compression streams and mean
 * nothing to a new session, so they go too. */
static void reset_session(struct wl_link *l, uint32_t now, int new_id)
{
    l->state = ST_SYNCING;
    l->my_sync_acked = 0;
    l->sync_ack_due = 0;
    l->next_sync_at = now;
    if (new_id) {
        l->my_id = next_random(l);
        l->have_peer = 0;
    }
    l->base = 0;
    l->next_seq = 0;
    l->expected = 0;
    memset(l->tx, 0, sizeof(l->tx));
    memset(l->rx, 0, sizeof(l->rx));
    l->txq.head = l->txq.len = 0;
    l->rxq.head = l->rxq.len = 0;
    l->have_rtt = 0;
    l->rto = RTO_INIT_MS;
    l->ack_pending = 0;
    l->ack_now = 0;
    l->ack_timer = 0;
    l->line_free_at = now;
}

static void check_up(struct wl_link *l, uint32_t now)
{
    uint16_t mp;
    uint8_t win;

    if (l->state != ST_SYNCING || !l->my_sync_acked || !l->have_peer)
        return;
    mp = l->cfg.max_payload < l->peer_max_payload ? l->cfg.max_payload
                                                  : l->peer_max_payload;
    win = l->cfg.window < l->peer_window ? l->cfg.window : l->peer_window;
    l->max_payload = mp > 0 ? mp : 1;
    l->window = win > 0 ? win : 1;
    l->state = ST_UP;
    l->generation++;
    l->last_rx = now;
    l->last_tx = now;
}

/* Common handling of the peer's id and offers, from SYNC or SYNC_ACK. */
static void learn_peer(struct wl_link *l, const uint8_t *p, uint32_t now)
{
    uint32_t id = wl_get_u32le(p);

    /* A different id from a known peer means it restarted: its streams are
     * new, so ours must be too. */
    if (l->have_peer && id != l->peer_id && (l->state == ST_UP || l->my_sync_acked)) {
        reset_session(l, now, 0);
        l->stats.resets++;
    }
    l->peer_id = id;
    l->have_peer = 1;
    l->peer_max_payload = wl_get_u16le(p + 4);
    l->peer_window = p[6] < WL_LINK_MAX_WINDOW ? p[6] : (uint8_t)WL_LINK_MAX_WINDOW;
}

/* ---------------------------------------------------------------------------
 * receive path
 * ------------------------------------------------------------------------- */

static void rtt_sample(struct wl_link *l, const struct txslot *s, uint32_t now)
{
    uint32_t r;
    uint32_t dev;

    if (s->retransmitted || time_before(now, s->left_at))
        return;
    r = now - s->left_at;
    if (!l->have_rtt) {
        l->srtt = r;
        l->rttvar = r / 2U;
        l->have_rtt = 1;
    } else {
        dev = r > l->srtt ? r - l->srtt : l->srtt - r;
        l->rttvar = ((3U * l->rttvar) + dev) / 4U;
        l->srtt = ((7U * l->srtt) + r) / 8U;
    }
    /* The 50 ms floor on the variance term keeps a run of identical samples
     * from collapsing the timeout onto the mean. */
    l->rto = l->srtt + (4U * l->rttvar > 50U ? 4U * l->rttvar : 50U);
    if (l->rto < RTO_MIN_MS)
        l->rto = RTO_MIN_MS;
    if (l->rto > RTO_MAX_MS)
        l->rto = RTO_MAX_MS;
}

static void process_ack(struct wl_link *l, uint16_t ack, uint32_t sack, uint32_t now)
{
    struct txslot *s;

    /* An ack for a frame never sent is a peer bug or a stale frame. */
    if (seq_before(l->next_seq, ack))
        return;
    while (seq_before(l->base, ack)) {
        s = &l->tx[l->base % WL_LINK_MAX_WINDOW];
        if (s->in_use && !s->acked)
            rtt_sample(l, s, now);
        s->in_use = 0;
        l->base++;
    }
    for (unsigned i = 0; i < 32U; i++) {
        uint16_t seq = (uint16_t)(ack + 1U + i);

        if ((sack & (1UL << i)) == 0 || !seq_before(seq, l->next_seq))
            continue;
        s = &l->tx[seq % WL_LINK_MAX_WINDOW];
        if (s->in_use && !s->acked) {
            rtt_sample(l, s, now);
            s->acked = 1;
        }
    }
    /* Later frames got through but the base did not: resend it now rather
     * than waiting out its timer. Once per frame, or every ack in a burst
     * would trigger another copy. */
    s = &l->tx[l->base % WL_LINK_MAX_WINDOW];
    if (sack != 0 && in_flight(l) > 0 && s->in_use && !s->acked && !s->fast_done)
        s->fast_pending = 1;
}

/* Moves in-order frames into the stream buffer while it has room. If it
 * fills, frames wait in their slots and the ack stops advancing, which is
 * what eventually stalls the sender: backpressure without extra messages. */
static int deliver(struct wl_link *l)
{
    int moved = 0;

    for (;;) {
        struct rxslot *s = &l->rx[l->expected % WL_LINK_MAX_WINDOW];

        if (!s->present || WL_LINK_QUEUE - l->rxq.len < s->len)
            break;
        (void)ring_put(&l->rxq, s->data, s->len);
        s->present = 0;
        l->expected++;
        moved++;
    }
    return moved;
}

static void rx_data(struct wl_link *l, const struct wl_frame *f, uint32_t now)
{
    uint16_t d = seq_diff(f->seq, l->expected);
    struct rxslot *s;

    if (d >= 0x8000U || d >= l->window || f->payload_len > l->max_payload) {
        /* A duplicate means our ack was lost; out of range means the peer
         * is confused about the window. Either way, tell it where we are. */
        l->ack_now = 1;
        return;
    }
    s = &l->rx[f->seq % WL_LINK_MAX_WINDOW];
    if (!s->present) {
        memcpy(s->data, f->payload, f->payload_len);
        s->len = (uint16_t)f->payload_len;
        s->present = 1;
    }
    if (d != 0)
        l->ack_now = 1; /* a gap: the sender should hear about it at once */
    if (deliver(l) == 0)
        return;
    if (++l->ack_pending >= 2U) {
        l->ack_now = 1;
    } else if (!l->ack_timer) {
        l->ack_timer = 1;
        l->ack_due_at = now + ACK_DELAY_MS;
    }
}

static void on_frame(void *ctx, const struct wl_frame *f)
{
    struct wl_link *l = ctx;
    uint32_t now = l->now;

    switch (f->type) {
    case WL_FRAME_SYNC:
        if (f->payload_len != SYNC_PAYLOAD)
            return;
        learn_peer(l, f->payload, now);
        l->sync_ack_due = 1;
        break;
    case WL_FRAME_SYNC_ACK:
        if (f->payload_len != SYNC_ACK_PAYLOAD || wl_get_u32le(f->payload) != l->my_id)
            return; /* an answer to a SYNC from an earlier life of ours */
        learn_peer(l, f->payload + 4, now);
        l->my_sync_acked = 1;
        break;
    case WL_FRAME_DATA:
    case WL_FRAME_ACK:
        if (l->state != ST_UP)
            return;
        process_ack(l, f->ack, f->sack, now);
        if (f->type == WL_FRAME_DATA)
            rx_data(l, f, now);
        break;
    default:
        return;
    }
    l->last_rx = now;
    check_up(l, now);
}

void wl_link_rx(struct wl_link *l, const uint8_t *data, size_t n, uint32_t now)
{
    l->now = now;
    wl_frame_rx_feed(&l->frx, data, n, on_frame, l);
}

/* ---------------------------------------------------------------------------
 * send path
 * ------------------------------------------------------------------------- */

static uint32_t selective_ack_bits(const struct wl_link *l)
{
    uint32_t bits = 0;

    for (unsigned i = 0; i + 1U < l->window && i < 32U; i++) {
        uint16_t seq = (uint16_t)(l->expected + 1U + i);

        if (l->rx[seq % WL_LINK_MAX_WINDOW].present)
            bits |= 1UL << i;
    }
    return bits;
}

/* Pacing: keep at most about one frame queued in the port beyond what is
 * already on the wire. A deep OS queue would make acks wait behind bulk
 * data and inflate the peer's RTT estimate. */
static uint32_t pace_wait(const struct wl_link *l, uint32_t now)
{
    uint32_t lead;

    if (l->cfg.bytes_per_sec == 0)
        return 0;
    lead = wire_time_ms(l, WL_FRAME_MAX_WIRE);
    return time_until(l->line_free_at - lead, now);
}

static size_t emit(struct wl_link *l, struct wl_frame *f, uint8_t *out, size_t cap,
                   uint32_t now)
{
    size_t n;
    uint32_t start;

    if (f->type == WL_FRAME_DATA || f->type == WL_FRAME_ACK) {
        f->ack = l->expected;
        f->sack = selective_ack_bits(l);
        l->ack_pending = 0;
        l->ack_now = 0;
        l->ack_timer = 0;
    }
    if (wl_frame_encode(f, out, cap, &n) != WL_OK)
        return 0; /* only possible with a caller buffer below WL_FRAME_MAX_WIRE */
    start = time_before(l->line_free_at, now) ? now : l->line_free_at;
    l->line_free_at = start + wire_time_ms(l, n);
    l->last_tx = now;
    return n;
}

static size_t send_sync(struct wl_link *l, uint8_t type, uint8_t *out, size_t cap,
                        uint32_t now)
{
    uint8_t p[SYNC_ACK_PAYLOAD];
    uint8_t *q = p;
    struct wl_frame f;

    memset(&f, 0, sizeof(f));
    f.type = type;
    if (type == WL_FRAME_SYNC_ACK) {
        wl_put_u32le(q, l->peer_id);
        q += 4;
    }
    wl_put_u32le(q, l->my_id);
    wl_put_u16le(q + 4, l->cfg.max_payload);
    q[6] = l->cfg.window;
    f.payload = p;
    f.payload_len = (size_t)(q + SYNC_PAYLOAD - p);
    return emit(l, &f, out, cap, now);
}

static size_t send_slot(struct wl_link *l, uint16_t seq, uint8_t *out, size_t cap,
                        uint32_t now)
{
    struct txslot *s = &l->tx[seq % WL_LINK_MAX_WINDOW];
    struct wl_frame f;
    size_t n;

    memset(&f, 0, sizeof(f));
    f.type = WL_FRAME_DATA;
    f.seq = seq;
    f.payload = s->data;
    f.payload_len = s->len;
    n = emit(l, &f, out, cap, now);
    s->left_at = l->line_free_at;
    s->deadline = s->left_at + l->rto;
    return n;
}

static int due_resend(const struct wl_link *l, uint32_t now, uint16_t *seq_out)
{
    for (uint16_t i = 0; i < in_flight(l); i++) {
        uint16_t seq = (uint16_t)(l->base + i);
        const struct txslot *s = &l->tx[seq % WL_LINK_MAX_WINDOW];

        if (s->in_use && !s->acked && (s->fast_pending || !time_before(now, s->deadline))) {
            *seq_out = seq;
            return 1;
        }
    }
    return 0;
}

static int ack_due(const struct wl_link *l, uint32_t now)
{
    return l->ack_now || (l->ack_timer && !time_before(now, l->ack_due_at)) ||
           !time_before(now, l->last_tx + KEEPALIVE_MS);
}

size_t wl_link_poll_tx(struct wl_link *l, uint8_t *out, size_t cap, uint32_t now)
{
    uint16_t seq;

    if (cap < WL_FRAME_MAX_WIRE)
        return 0;
    if (l->state == ST_UP && !time_before(now, l->last_rx + DEAD_MS)) {
        reset_session(l, now, 1);
        l->stats.resets++;
    }
    if (pace_wait(l, now) > 0)
        return 0;

    if (l->sync_ack_due && l->have_peer) {
        l->sync_ack_due = 0;
        return send_sync(l, WL_FRAME_SYNC_ACK, out, cap, now);
    }
    if (l->state == ST_SYNCING) {
        if (l->my_sync_acked || time_before(now, l->next_sync_at))
            return 0;
        l->next_sync_at = now + SYNC_INTERVAL_MS;
        return send_sync(l, WL_FRAME_SYNC, out, cap, now);
    }

    if (due_resend(l, now, &seq)) {
        struct txslot *s = &l->tx[seq % WL_LINK_MAX_WINDOW];

        if (s->fast_pending) {
            s->fast_pending = 0;
            s->fast_done = 1;
            l->stats.fast_retx++;
        } else {
            l->stats.timeouts++;
            /* Back off: the line or the peer is slower than we thought.
             * The next clean sample recomputes the timeout. */
            l->rto = l->rto * 2U > RTO_MAX_MS ? RTO_MAX_MS : l->rto * 2U;
        }
        s->retransmitted = 1;
        l->stats.retransmits++;
        return send_slot(l, seq, out, cap, now);
    }

    if (in_flight(l) < l->window && l->txq.len > 0) {
        struct txslot *s = &l->tx[l->next_seq % WL_LINK_MAX_WINDOW];

        memset(s, 0, offsetof(struct txslot, data));
        s->len = (uint16_t)ring_get(&l->txq, s->data, l->max_payload);
        s->in_use = 1;
        seq = l->next_seq++;
        l->stats.data_sent++;
        return send_slot(l, seq, out, cap, now);
    }

    if (ack_due(l, now)) {
        struct wl_frame f;

        memset(&f, 0, sizeof(f));
        f.type = WL_FRAME_ACK;
        return emit(l, &f, out, cap, now);
    }
    return 0;
}

uint32_t wl_link_next_timeout(const struct wl_link *l, uint32_t now)
{
    uint32_t t = TIMEOUT_CAP_MS;
    uint32_t pace = pace_wait(l, now);

#define SOONER(x)                                                                        \
    do {                                                                                 \
        uint32_t v_ = (x);                                                               \
        if (v_ < t)                                                                      \
            t = v_;                                                                      \
    } while (0)

    if (l->sync_ack_due && l->have_peer)
        SOONER(0);
    if (l->state == ST_SYNCING) {
        if (!l->my_sync_acked)
            SOONER(time_until(l->next_sync_at, now));
    } else {
        for (uint16_t i = 0; i < in_flight(l); i++) {
            const struct txslot *s = &l->tx[(uint16_t)(l->base + i) % WL_LINK_MAX_WINDOW];

            if (s->in_use && !s->acked)
                SOONER(s->fast_pending ? 0 : time_until(s->deadline, now));
        }
        if (in_flight(l) < l->window && l->txq.len > 0)
            SOONER(0);
        if (l->ack_now)
            SOONER(0);
        if (l->ack_timer)
            SOONER(time_until(l->ack_due_at, now));
        SOONER(time_until(l->last_tx + KEEPALIVE_MS, now));
        SOONER(time_until(l->last_rx + DEAD_MS, now));
    }
#undef SOONER
    /* Nothing can be sent before the port drains, so waking earlier than
     * that would only spin. */
    return t > pace ? t : pace;
}

/* ---------------------------------------------------------------------------
 * public
 * ------------------------------------------------------------------------- */

struct wl_link *wl_link_new(const struct wl_link_cfg *cfg, uint32_t seed, uint32_t now)
{
    struct wl_link *l = calloc(1, sizeof(*l));

    if (l == NULL) {
        (void)wl_fail(WL_ERR_NOMEM, "link: out of memory");
        return NULL;
    }
    l->cfg = *cfg;
    if (l->cfg.max_payload > WL_SERIAL_MAX_PAYLOAD)
        l->cfg.max_payload = WL_SERIAL_MAX_PAYLOAD;
    /* SYNC_ACK carries 11 bytes of payload, so an offer below that could not
     * even complete the handshake. */
    if (l->cfg.max_payload < MIN_PAYLOAD_OFFER)
        l->cfg.max_payload = MIN_PAYLOAD_OFFER;
    if (l->cfg.window == 0 || l->cfg.window > WL_LINK_MAX_WINDOW)
        l->cfg.window = WL_LINK_DEFAULT_WINDOW;
    l->rng = seed != 0 ? seed : 0x6D2B79F5U;
    (void)next_random(l);
    wl_frame_rx_init(&l->frx, l->cfg.max_payload);
    reset_session(l, now, 1);
    return l;
}

void wl_link_free(struct wl_link *l)
{
    free(l);
}

size_t wl_link_send(struct wl_link *l, const uint8_t *data, size_t n)
{
    if (l->state != ST_UP)
        return 0;
    return ring_put(&l->txq, data, n);
}

size_t wl_link_recv(struct wl_link *l, uint8_t *buf, size_t cap)
{
    size_t n = ring_get(&l->rxq, buf, cap);

    /* Room freed in the stream buffer may let waiting frames through; the
     * peer only learns the window moved from an ack. */
    if (n > 0 && deliver(l) > 0)
        l->ack_now = 1;
    return n;
}

size_t wl_link_unacked(const struct wl_link *l)
{
    size_t n = l->txq.len;

    for (uint16_t i = 0; i < in_flight(l); i++) {
        const struct txslot *s = &l->tx[(uint16_t)(l->base + i) % WL_LINK_MAX_WINDOW];

        if (s->in_use && !s->acked)
            n += s->len;
    }
    return n;
}

int wl_link_is_up(const struct wl_link *l)
{
    return l->state == ST_UP;
}

uint32_t wl_link_generation(const struct wl_link *l)
{
    return l->generation;
}

void wl_link_get_stats(const struct wl_link *l, struct wl_link_stats *out)
{
    *out = l->stats;
    out->rto_ms = l->rto;
    out->rx = l->frx.stats;
}
