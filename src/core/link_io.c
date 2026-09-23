#include "devlink/clock.h"
#include "devlink/link_io.h"

static wl_status flush_tx(struct wl_link *l, struct wl_transport *t)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    size_t n;

    while ((n = wl_link_poll_tx(l, wire, sizeof(wire), wl_clock_ms())) > 0) {
        wl_status s = wl_transport_write(t, wire, n);

        if (s != WL_OK)
            return s;
    }
    return WL_OK;
}

wl_status wl_link_pump(struct wl_link *l, struct wl_transport *t, uint32_t max_wait_ms)
{
    uint8_t buf[512];
    size_t got;
    uint32_t wait;
    wl_status s = flush_tx(l, t);

    if (s != WL_OK)
        return s;
    wait = wl_link_next_timeout(l, wl_clock_ms());
    if (wait > max_wait_ms)
        wait = max_wait_ms;
    s = wl_transport_read(t, buf, sizeof(buf), &got, wait);
    if (s != WL_OK)
        return s;
    if (got > 0)
        wl_link_rx(l, buf, got, wl_clock_ms());
    return flush_tx(l, t);
}
