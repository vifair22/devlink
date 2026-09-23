#ifndef DEVLINK_WIRE_H
#define DEVLINK_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "devlink/bytes.h"

/*
 * Bounds-checked field writers and readers for record payloads. Both are
 * sticky: once a write overflows or a read runs past the end, every later
 * call is a no-op and the flag stays set, so a message is built or parsed
 * straight through and checked once at the end.
 */

struct wl_wbuf {
    uint8_t *p;
    size_t cap;
    size_t len;
    int overflow;
};

struct wl_rbuf {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int bad;
};

static inline void wl_wbuf_init(struct wl_wbuf *w, uint8_t *p, size_t cap)
{
    w->p = p;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

static inline uint8_t *wl_w_reserve(struct wl_wbuf *w, size_t n)
{
    uint8_t *q;

    if (w->overflow || w->cap - w->len < n) {
        w->overflow = 1;
        return NULL;
    }
    q = w->p + w->len;
    w->len += n;
    return q;
}

static inline void wl_w_u8(struct wl_wbuf *w, uint8_t v)
{
    uint8_t *q = wl_w_reserve(w, 1);

    if (q != NULL)
        q[0] = v;
}

static inline void wl_w_u16(struct wl_wbuf *w, uint16_t v)
{
    uint8_t *q = wl_w_reserve(w, 2);

    if (q != NULL)
        wl_put_u16le(q, v);
}

static inline void wl_w_u32(struct wl_wbuf *w, uint32_t v)
{
    uint8_t *q = wl_w_reserve(w, 4);

    if (q != NULL)
        wl_put_u32le(q, v);
}

static inline void wl_w_u64(struct wl_wbuf *w, uint64_t v)
{
    wl_w_u32(w, (uint32_t)(v & 0xFFFFFFFFU));
    wl_w_u32(w, (uint32_t)(v >> 32));
}

static inline void wl_w_bytes(struct wl_wbuf *w, const void *src, size_t n)
{
    uint8_t *q = wl_w_reserve(w, n);

    if (q != NULL && n > 0)
        memcpy(q, src, n);
}

/* str: u16 length, then the bytes. */
static inline void wl_w_str(struct wl_wbuf *w, const char *s, size_t n)
{
    if (n > 0xFFFFU) {
        w->overflow = 1;
        return;
    }
    wl_w_u16(w, (uint16_t)n);
    wl_w_bytes(w, s, n);
}

static inline void wl_rbuf_init(struct wl_rbuf *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->bad = 0;
}

static inline const uint8_t *wl_r_take(struct wl_rbuf *r, size_t n)
{
    const uint8_t *q;

    if (r->bad || r->len - r->pos < n) {
        r->bad = 1;
        return NULL;
    }
    q = r->p + r->pos;
    r->pos += n;
    return q;
}

static inline uint8_t wl_r_u8(struct wl_rbuf *r)
{
    const uint8_t *q = wl_r_take(r, 1);

    return q != NULL ? q[0] : 0;
}

static inline uint16_t wl_r_u16(struct wl_rbuf *r)
{
    const uint8_t *q = wl_r_take(r, 2);

    return q != NULL ? wl_get_u16le(q) : 0;
}

static inline uint32_t wl_r_u32(struct wl_rbuf *r)
{
    const uint8_t *q = wl_r_take(r, 4);

    return q != NULL ? wl_get_u32le(q) : 0;
}

static inline uint64_t wl_r_u64(struct wl_rbuf *r)
{
    uint64_t lo = wl_r_u32(r);

    return lo | ((uint64_t)wl_r_u32(r) << 32);
}

/* Returns a pointer into the buffer (not NUL-terminated) and its length. */
static inline const char *wl_r_str(struct wl_rbuf *r, size_t *n)
{
    size_t len = wl_r_u16(r);
    const uint8_t *q = wl_r_take(r, len);

    *n = q != NULL ? len : 0;
    return (const char *)q;
}

/* True if the payload was consumed exactly, with nothing left over. */
static inline int wl_r_done(const struct wl_rbuf *r)
{
    return !r->bad && r->pos == r->len;
}

#endif
