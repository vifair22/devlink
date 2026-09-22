#include <string.h>

#include "w98link/clock.h"
#include "w98link/link_io.h"
#include "w98link/linktest.h"

#define PUMP_MS 50U

static uint8_t pattern(uint64_t k)
{
    uint32_t x = (uint32_t)k * 2654435761U;

    return (uint8_t)((x >> 24) ^ (uint32_t)(k >> 8));
}

wl_status wl_linktest_probe(struct wl_transport *t, const struct wl_link_cfg *cfg,
                            uint32_t seed, uint64_t total, uint32_t limit_ms,
                            struct wl_linktest_result *out)
{
    struct wl_link *l = wl_link_new(cfg, seed, wl_clock_ms());
    uint8_t buf[1024];
    uint64_t sent = 0;
    uint32_t start = wl_clock_ms();
    uint32_t up_at = 0;
    uint32_t gen = 0;
    wl_status s = WL_OK;

    memset(out, 0, sizeof(*out));
    if (l == NULL)
        return WL_ERR_NOMEM;
    while (out->bytes < total && wl_clock_since(start) < limit_ms) {
        s = wl_link_pump(l, t, PUMP_MS);
        if (s != WL_OK)
            break;
        if (wl_link_generation(l) != gen) {
            /* A new session restarts the stream from byte 0 on both ends. */
            gen = wl_link_generation(l);
            out->sessions++;
            sent = 0;
            out->bytes = 0;
            up_at = wl_clock_ms();
        }
        while (sent < total) {
            size_t want = total - sent < sizeof(buf) ? (size_t)(total - sent) : sizeof(buf);
            size_t n;

            for (size_t i = 0; i < want; i++)
                buf[i] = pattern(sent + i);
            n = wl_link_send(l, buf, want);
            sent += n;
            if (n < want)
                break;
        }
        for (;;) {
            size_t n = wl_link_recv(l, buf, sizeof(buf));

            if (n == 0)
                break;
            for (size_t i = 0; i < n; i++) {
                if (buf[i] != pattern(out->bytes + i))
                    out->corrupt++;
            }
            out->bytes += n;
        }
    }
    out->elapsed_ms = up_at != 0 ? wl_clock_since(up_at) : 0;
    wl_link_get_stats(l, &out->stats);
    wl_link_free(l);
    return s;
}

wl_status wl_linktest_serve(struct wl_transport *t, const struct wl_link_cfg *cfg,
                            uint32_t seed, uint32_t wait_ms, uint32_t idle_ms,
                            struct wl_linktest_result *out)
{
    struct wl_link *l = wl_link_new(cfg, seed, wl_clock_ms());
    uint8_t buf[1024];
    size_t pending = 0; /* received but not yet accepted by wl_link_send */
    uint32_t start = wl_clock_ms();
    uint32_t last_data = 0;
    uint32_t gen = 0;
    wl_status s = WL_OK;

    memset(out, 0, sizeof(*out));
    if (l == NULL)
        return WL_ERR_NOMEM;
    for (;;) {
        if (out->bytes == 0 && wl_clock_since(start) >= wait_ms)
            break;
        if (out->bytes > 0 && wl_clock_since(last_data) >= idle_ms)
            break;
        s = wl_link_pump(l, t, PUMP_MS);
        if (s != WL_OK)
            break;
        if (wl_link_generation(l) != gen) {
            gen = wl_link_generation(l);
            out->sessions++;
            pending = 0;
        }
        for (;;) {
            size_t n;

            if (pending > 0) {
                n = wl_link_send(l, buf, pending);
                memmove(buf, buf + n, pending - n);
                pending -= n;
                if (pending > 0)
                    break; /* peer is not draining; let the window move */
            }
            n = wl_link_recv(l, buf, sizeof(buf));
            if (n == 0)
                break;
            out->bytes += n;
            last_data = wl_clock_ms();
            pending = n;
        }
    }
    out->elapsed_ms = wl_clock_since(start);
    wl_link_get_stats(l, &out->stats);
    wl_link_free(l);
    return s;
}
