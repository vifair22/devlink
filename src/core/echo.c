#include <stdlib.h>
#include <string.h>

#include "devlink/clock.h"
#include "devlink/echo.h"

/* Serial reads are small; a few hundred bytes per call keeps the loop
 * responsive without many syscalls per frame. */
#define READ_CHUNK 512U
#define SERVE_TICK_MS 250U

/* Heap-allocated: the receiver and the frame buffers are several KiB, and
 * the agent's thread stacks are not somewhere to put them. */
struct echo_ctx {
    struct wl_transport *t;
    struct wl_frame_rx rx;
    uint8_t wire[WL_FRAME_MAX_WIRE];
    uint8_t in[READ_CHUNK];
    uint8_t payload[WL_SERIAL_MAX_PAYLOAD];
    struct wl_echo_stats *st;
    wl_status io; /* first transport failure seen inside a callback */
    int done;     /* serve: ACK seen; probe: current frame resolved */
    uint16_t want_seq;
    size_t want_len;
};

static struct echo_ctx *ctx_new(struct wl_transport *t, struct wl_echo_stats *st)
{
    struct echo_ctx *c = calloc(1, sizeof(*c));

    if (c == NULL)
        return NULL;
    c->t = t;
    c->st = st;
    memset(st, 0, sizeof(*st));
    wl_frame_rx_init(&c->rx, WL_SERIAL_MAX_PAYLOAD);
    return c;
}

static wl_status send_frame(struct echo_ctx *c, uint8_t type, uint16_t seq,
                            const uint8_t *payload, size_t len)
{
    struct wl_frame f;
    size_t n;
    wl_status s;

    f.type = type;
    f.seq = seq;
    f.ack = 0;
    f.sack = 0;
    f.payload = payload;
    f.payload_len = len;
    s = wl_frame_encode(&f, c->wire, sizeof(c->wire), &n);
    if (s != WL_OK)
        return s;
    return wl_transport_write(c->t, c->wire, n);
}

/* Reads once and feeds the receiver, which calls cb for each good frame. */
static wl_status pump(struct echo_ctx *c, uint32_t timeout_ms, wl_frame_cb cb)
{
    size_t got;
    wl_status s = wl_transport_read(c->t, c->in, sizeof(c->in), &got, timeout_ms);

    if (s != WL_OK)
        return s;
    wl_frame_rx_feed(&c->rx, c->in, got, cb, c);
    return c->io;
}

static void serve_cb(void *ctx, const struct wl_frame *f)
{
    struct echo_ctx *c = ctx;

    if (f->type == WL_FRAME_ACK) {
        c->done = 1;
        return;
    }
    if (f->type != WL_FRAME_DATA || c->io != WL_OK)
        return;
    c->io = send_frame(c, WL_FRAME_DATA, f->seq, f->payload, f->payload_len);
    if (c->io == WL_OK)
        c->st->echoed++;
}

wl_status wl_echo_serve(struct wl_transport *t, uint32_t idle_ms,
                        struct wl_echo_stats *st)
{
    struct echo_ctx *c = ctx_new(t, st);
    uint32_t start = wl_clock_ms();
    uint32_t last = start;
    wl_status s = WL_OK;

    if (c == NULL)
        return wl_fail(WL_ERR_NOMEM, "echo: out of memory");
    while (!c->done && wl_clock_since(last) < idle_ms) {
        uint32_t before = c->rx.stats.frames;

        s = pump(c, SERVE_TICK_MS, serve_cb);
        if (s != WL_OK)
            break;
        if (c->rx.stats.frames != before)
            last = wl_clock_ms();
    }
    st->rx = c->rx.stats;
    st->elapsed_ms = wl_clock_since(start);
    free(c);
    return s;
}

/* Deterministic per-frame payload, so the probe can check an echo without
 * keeping copies. Every eighth byte is zero to keep COBS honest. */
static void fill_payload(uint8_t *p, size_t len, uint16_t seq)
{
    uint32_t x = 0x9E3779B9U ^ ((uint32_t)seq * 2654435761U);

    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (i % 8U == 7U) ? 0 : (uint8_t)x;
    }
}

static void probe_cb(void *ctx, const struct wl_frame *f)
{
    struct echo_ctx *c = ctx;

    /* Late echoes of frames that already timed out are ignored. */
    if (f->type != WL_FRAME_DATA || f->seq != c->want_seq || c->done)
        return;
    c->done = 1;
    if (f->payload_len == c->want_len &&
        memcmp(f->payload, c->payload, c->want_len) == 0)
        c->st->echoed++;
    else
        c->st->mismatched++;
}

wl_status wl_echo_probe(struct wl_transport *t, uint32_t count, size_t payload_len,
                        uint32_t timeout_ms, struct wl_echo_stats *st)
{
    struct echo_ctx *c;
    uint32_t start;
    wl_status s = WL_OK;

    if (payload_len > WL_SERIAL_MAX_PAYLOAD)
        return wl_fail(WL_ERR_INVAL, "echo: payload %zu exceeds %u", payload_len,
                       WL_SERIAL_MAX_PAYLOAD);
    c = ctx_new(t, st);
    if (c == NULL)
        return wl_fail(WL_ERR_NOMEM, "echo: out of memory");
    start = wl_clock_ms();
    for (uint32_t i = 0; i < count && s == WL_OK; i++) {
        uint32_t sent_at;

        c->want_seq = (uint16_t)i;
        c->want_len = payload_len;
        c->done = 0;
        fill_payload(c->payload, payload_len, c->want_seq);
        s = send_frame(c, WL_FRAME_DATA, c->want_seq, c->payload, payload_len);
        if (s != WL_OK)
            break;
        st->sent++;
        sent_at = wl_clock_ms();
        while (!c->done && s == WL_OK) {
            uint32_t waited = wl_clock_since(sent_at);

            if (waited >= timeout_ms) {
                st->timeouts++;
                break;
            }
            s = pump(c, timeout_ms - waited, probe_cb);
        }
    }
    if (s == WL_OK)
        s = send_frame(c, WL_FRAME_ACK, 0, NULL, 0);
    st->rx = c->rx.stats;
    st->elapsed_ms = wl_clock_since(start);
    free(c);
    return s;
}
