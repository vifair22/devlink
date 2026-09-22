#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "w98link/link_io.h"
#include "w98link/session.h"

/* Compressed bytes between deflate and the link, and between the link and
 * inflate. Small: they only smooth the hand-off, the queues above are the
 * real buffers. */
#define ZBUF 8192U

/* A linear buffer: data lives in [head, head + len). Compacted to the front
 * when the free space at the end runs short, which with these sizes is a
 * cheap memmove, and keeps every zlib call on one contiguous region. */
struct lbuf {
    size_t head;
    size_t len;
};

struct wl_session {
    struct wl_link *link;
    uint32_t gen; /* link generation the streams belong to */
    int level;
    int streams_ready;
    z_stream def;
    z_stream inf;
    int dirty; /* deflate has taken input it has not flushed */

    uint8_t out_plain[WL_SESSION_QUEUE];
    struct lbuf out_plain_b;
    uint8_t out_z[ZBUF];
    struct lbuf out_z_b;
    uint8_t in_z[ZBUF];
    struct lbuf in_z_b;
    uint8_t in_plain[WL_SESSION_QUEUE];
    struct lbuf in_plain_b;

    struct wl_session_stats stats;
};

static size_t tail_room(const struct lbuf *b, size_t cap)
{
    return cap - b->head - b->len;
}

static void compact(uint8_t *buf, struct lbuf *b)
{
    if (b->head == 0)
        return;
    memmove(buf, buf + b->head, b->len);
    b->head = 0;
}

static void consume(struct lbuf *b, size_t n)
{
    b->head += n;
    b->len -= n;
    if (b->len == 0)
        b->head = 0;
}

/* The streams are created lazily and reset, not recreated, per session:
 * deflateReset keeps the 256 KiB of state allocated, which matters on a
 * machine where every restart would otherwise churn the heap. */
static wl_status reset_streams(struct wl_session *s)
{
    if (!s->streams_ready) {
        memset(&s->def, 0, sizeof(s->def));
        memset(&s->inf, 0, sizeof(s->inf));
        /* windowBits -15: raw deflate, no zlib header, 32 KiB window. */
        if (deflateInit2(&s->def, s->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return wl_fail(WL_ERR_NOMEM, "session: deflateInit2 failed");
        if (inflateInit2(&s->inf, -15) != Z_OK) {
            (void)deflateEnd(&s->def);
            return wl_fail(WL_ERR_NOMEM, "session: inflateInit2 failed");
        }
        s->streams_ready = 1;
    } else {
        (void)deflateReset(&s->def);
        (void)inflateReset(&s->inf);
    }
    s->dirty = 0;
    memset(&s->out_plain_b, 0, sizeof(struct lbuf));
    memset(&s->out_z_b, 0, sizeof(struct lbuf));
    memset(&s->in_z_b, 0, sizeof(struct lbuf));
    memset(&s->in_plain_b, 0, sizeof(struct lbuf));
    return WL_OK;
}

/* Runs deflate over the queued plaintext (or an empty input when flushing)
 * into the free end of out_z. Returns nonzero if it made progress. */
static int run_deflate(struct wl_session *s, int flush)
{
    size_t in_before = s->out_plain_b.len;
    size_t room;

    compact(s->out_z, &s->out_z_b);
    room = tail_room(&s->out_z_b, ZBUF);
    s->def.next_in = s->out_plain + s->out_plain_b.head;
    s->def.avail_in = (uInt)s->out_plain_b.len;
    s->def.next_out = s->out_z + s->out_z_b.head + s->out_z_b.len;
    s->def.avail_out = (uInt)room;
    /* Only Z_STREAM_ERROR is possible here, and only with a corrupted
     * stream struct; Z_BUF_ERROR just means no progress was possible. */
    (void)deflate(&s->def, flush);

    consume(&s->out_plain_b, in_before - s->def.avail_in);
    s->out_z_b.len += room - s->def.avail_out;
    s->stats.plain_out += in_before - s->def.avail_in;
    if (flush == Z_NO_FLUSH && in_before != s->def.avail_in)
        s->dirty = 1;
    if (flush == Z_SYNC_FLUSH && s->def.avail_out != 0) {
        /* Output space left over means the flush completed. */
        s->dirty = 0;
        s->stats.flushes++;
    }
    return in_before != s->def.avail_in || room != s->def.avail_out;
}

static void work_tx(struct wl_session *s)
{
    for (;;) {
        if (s->out_z_b.len > 0) {
            size_t n = wl_link_send(s->link, s->out_z + s->out_z_b.head, s->out_z_b.len);

            s->stats.z_out += n;
            consume(&s->out_z_b, n);
            if (s->out_z_b.len > 0)
                return; /* link queue full; the link is the bottleneck */
            continue;
        }
        if (s->out_plain_b.len > 0) {
            if (!run_deflate(s, Z_NO_FLUSH))
                return;
            continue;
        }
        if (s->dirty) {
            (void)run_deflate(s, Z_SYNC_FLUSH);
            if (s->out_z_b.len == 0)
                return;
            continue;
        }
        return;
    }
}

static wl_status work_rx(struct wl_session *s)
{
    for (;;) {
        size_t room;
        size_t in_before;
        int ret;

        if (s->in_z_b.len == 0) {
            s->in_z_b.head = 0;
            s->in_z_b.len = wl_link_recv(s->link, s->in_z, ZBUF);
            s->stats.z_in += s->in_z_b.len;
            if (s->in_z_b.len == 0)
                return WL_OK;
        }
        compact(s->in_plain, &s->in_plain_b);
        room = tail_room(&s->in_plain_b, WL_SESSION_QUEUE);
        if (room == 0)
            return WL_OK; /* the reader is behind; the link backs up for us */

        in_before = s->in_z_b.len;
        s->inf.next_in = s->in_z + s->in_z_b.head;
        s->inf.avail_in = (uInt)s->in_z_b.len;
        s->inf.next_out = s->in_plain + s->in_plain_b.len;
        s->inf.avail_out = (uInt)room;
        ret = inflate(&s->inf, Z_SYNC_FLUSH);
        consume(&s->in_z_b, in_before - s->inf.avail_in);
        s->in_plain_b.len += room - s->inf.avail_out;
        s->stats.plain_in += room - s->inf.avail_out;

        /* A session's stream never ends, so Z_STREAM_END is as wrong as a
         * data error: the peer is not speaking this protocol. */
        if (ret != Z_OK && ret != Z_BUF_ERROR)
            return wl_fail(WL_ERR_CORRUPT, "session: peer stream does not decompress (%d: %s)",
                           ret, s->inf.msg != NULL ? s->inf.msg : "no detail");
        if (in_before == s->inf.avail_in && room == s->inf.avail_out)
            return WL_OK;
    }
}

wl_status wl_session_work(struct wl_session *s)
{
    uint32_t gen = wl_link_generation(s->link);

    if (gen != s->gen) {
        wl_status st = reset_streams(s);

        if (st != WL_OK)
            return st;
        s->gen = gen;
    }
    if (!wl_link_is_up(s->link) || !s->streams_ready)
        return WL_OK;
    work_tx(s);
    return work_rx(s);
}

wl_status wl_session_pump(struct wl_session *s, struct wl_transport *t,
                          uint32_t max_wait_ms)
{
    wl_status st = wl_session_work(s);

    if (st != WL_OK)
        return st;
    st = wl_link_pump(s->link, t, max_wait_ms);
    if (st != WL_OK)
        return st;
    return wl_session_work(s);
}

struct wl_session *wl_session_new(const struct wl_session_cfg *cfg, uint32_t seed,
                                  uint32_t now)
{
    struct wl_session *s = calloc(1, sizeof(*s));

    if (s == NULL) {
        (void)wl_fail(WL_ERR_NOMEM, "session: out of memory");
        return NULL;
    }
    s->level = cfg->level >= 0 && cfg->level <= 9 ? cfg->level : WL_SESSION_DEFAULT_LEVEL;
    s->link = wl_link_new(&cfg->link, seed, now);
    if (s->link == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

void wl_session_free(struct wl_session *s)
{
    if (s == NULL)
        return;
    if (s->streams_ready) {
        (void)deflateEnd(&s->def);
        (void)inflateEnd(&s->inf);
    }
    wl_link_free(s->link);
    free(s);
}

struct wl_link *wl_session_link(struct wl_session *s)
{
    return s->link;
}

size_t wl_session_send(struct wl_session *s, const uint8_t *data, size_t n)
{
    size_t room;

    /* Refused until the streams belong to the current session, so bytes
     * never straddle two sessions' compressors. */
    if (!wl_link_is_up(s->link) || s->gen != wl_link_generation(s->link))
        return 0;
    compact(s->out_plain, &s->out_plain_b);
    room = tail_room(&s->out_plain_b, WL_SESSION_QUEUE);
    if (n > room)
        n = room;
    memcpy(s->out_plain + s->out_plain_b.len, data, n);
    s->out_plain_b.len += n;
    return n;
}

size_t wl_session_recv(struct wl_session *s, uint8_t *buf, size_t cap)
{
    size_t n = s->in_plain_b.len < cap ? s->in_plain_b.len : cap;

    memcpy(buf, s->in_plain + s->in_plain_b.head, n);
    consume(&s->in_plain_b, n);
    return n;
}

int wl_session_busy(const struct wl_session *s)
{
    return s->out_plain_b.len > 0 || s->out_z_b.len > 0 || s->dirty ||
           wl_link_unacked(s->link) > 0;
}

int wl_session_is_up(const struct wl_session *s)
{
    return wl_link_is_up(s->link) && s->gen == wl_link_generation(s->link);
}

uint32_t wl_session_generation(const struct wl_session *s)
{
    return s->gen;
}

void wl_session_get_stats(const struct wl_session *s, struct wl_session_stats *out)
{
    *out = s->stats;
}
