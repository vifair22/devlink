#ifndef TEST_SIM_LINE_H
#define TEST_SIM_LINE_H

/*
 * A simulated serial line between two link endpoints, in virtual time. The
 * line serialises bytes at a fixed rate, adds latency, and can drop bytes,
 * flip bits and inject garbage. Shared by the link and session tests so both
 * run over exactly the same model.
 */

/* cmocka's assertions, and the headers cmocka.h expects before it. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "devlink/link.h"

#define SIM_CHAN_CAP (1U << 18)

struct sim_line_cfg {
    uint32_t bytes_per_sec; /* 0: infinitely fast */
    uint32_t latency_us;
    uint32_t drop_ppm;    /* per byte */
    uint32_t flip_ppm;    /* per byte, one random bit */
    uint32_t garbage_ppm; /* per byte, inserts one random byte after it */
};

struct sim_chan {
    uint8_t byte[SIM_CHAN_CAP];
    uint64_t at_us[SIM_CHAN_CAP];
    size_t head;
    size_t len;
    uint64_t busy_until_us;
};

struct sim_world {
    struct sim_line_cfg line;
    struct sim_chan ab; /* A -> B */
    struct sim_chan ba; /* B -> A */
    uint64_t now_us;
    uint64_t rng;
};

static inline void sim_init_world(struct sim_world *w, const struct sim_line_cfg *line)
{
    w->line = *line;
    w->ab.head = w->ab.len = 0;
    w->ba.head = w->ba.len = 0;
    w->ab.busy_until_us = w->ba.busy_until_us = 0;
    w->rng = 0x2545F4914F6CDD1DULL;
    w->now_us = 1000000U;
}

static inline uint32_t sim_rnd(struct sim_world *w)
{
    uint64_t x = w->rng;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    w->rng = x;
    return (uint32_t)(x >> 32);
}

static inline uint32_t sim_now_ms(const struct sim_world *w)
{
    return (uint32_t)(w->now_us / 1000U);
}

static inline void sim_push_byte(struct sim_world *w, struct sim_chan *c, uint8_t b)
{
    uint64_t start = c->busy_until_us > w->now_us ? c->busy_until_us : w->now_us;
    uint64_t byte_us = w->line.bytes_per_sec ? 1000000U / w->line.bytes_per_sec : 0;
    size_t tail;

    assert_true(c->len < SIM_CHAN_CAP);
    c->busy_until_us = start + byte_us;
    tail = (c->head + c->len) % SIM_CHAN_CAP;
    c->byte[tail] = b;
    c->at_us[tail] = c->busy_until_us + w->line.latency_us;
    c->len++;
}

/* Bytes sent towards a side that is not running still occupy the wire. */
static inline void sim_send(struct sim_world *w, struct sim_chan *c, const uint8_t *p,
                            size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];

        if (w->line.drop_ppm && sim_rnd(w) % 1000000U < w->line.drop_ppm)
            continue;
        if (w->line.flip_ppm && sim_rnd(w) % 1000000U < w->line.flip_ppm)
            b ^= (uint8_t)(1U << (sim_rnd(w) % 8U));
        sim_push_byte(w, c, b);
        if (w->line.garbage_ppm && sim_rnd(w) % 1000000U < w->line.garbage_ppm)
            sim_push_byte(w, c, (uint8_t)sim_rnd(w));
    }
}

/* Delivers everything due by now; a side that is not running loses it. */
static inline void sim_deliver(struct sim_world *w, struct sim_chan *c, struct wl_link *to,
                               int running)
{
    uint8_t buf[4096];
    size_t n = 0;

    for (;;) {
        while (c->len > 0 && c->at_us[c->head] <= w->now_us && n < sizeof(buf)) {
            buf[n++] = c->byte[c->head];
            c->head = (c->head + 1) % SIM_CHAN_CAP;
            c->len--;
        }
        if (n == 0)
            return;
        if (running)
            wl_link_rx(to, buf, n, sim_now_ms(w));
        n = 0;
    }
}

static inline void sim_tx(struct sim_world *w, struct wl_link *from, struct sim_chan *c)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    size_t n;

    while ((n = wl_link_poll_tx(from, wire, sizeof(wire), sim_now_ms(w))) > 0)
        sim_send(w, c, wire, n);
}

static inline uint64_t sim_chan_next_us(const struct sim_chan *c)
{
    return c->len > 0 ? c->at_us[c->head] : UINT64_MAX;
}

/* Moves virtual time to the next event: a byte arriving or either link's
 * timer, whichever is first; always forward, even if a timer is due now. */
static inline void sim_advance(struct sim_world *w, const struct wl_link *a, int a_running,
                               const struct wl_link *b, int b_running)
{
    uint32_t to = 1000;
    uint64_t next;

    if (a_running) {
        uint32_t t = wl_link_next_timeout(a, sim_now_ms(w));
        to = t < to ? t : to;
    }
    if (b_running) {
        uint32_t t = wl_link_next_timeout(b, sim_now_ms(w));
        to = t < to ? t : to;
    }
    next = (uint64_t)(sim_now_ms(w) + to) * 1000U;
    if (sim_chan_next_us(&w->ab) < next)
        next = sim_chan_next_us(&w->ab);
    if (sim_chan_next_us(&w->ba) < next)
        next = sim_chan_next_us(&w->ba);
    w->now_us = next > w->now_us ? next : w->now_us + 100U;
}

#endif
