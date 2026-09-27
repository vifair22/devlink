#include "devlink/clock.h"
#include "devlink/linkif.h"

static wl_status flush_tx(struct wl_linkif *l, struct wl_transport *t, uint8_t *wire)
{
    size_t n;

    while ((n = l->ops->poll_tx(l->impl, wire, WL_LINKIF_WIRE, wl_clock_ms())) > 0) {
        wl_status s = wl_transport_write(t, wire, n);

        if (s != WL_OK)
            return s;
    }
    return WL_OK;
}

wl_status wl_linkif_pump(struct wl_linkif *l, struct wl_transport *t, uint32_t max_wait_ms)
{
    uint8_t wire[WL_LINKIF_WIRE];
    uint8_t buf[4096];
    size_t got;
    uint32_t wait;
    wl_status s = flush_tx(l, t, wire);

    if (s != WL_OK)
        return s;
    wait = l->ops->next_timeout(l->impl, wl_clock_ms());
    if (wait > max_wait_ms)
        wait = max_wait_ms;
    s = wl_transport_read(t, buf, sizeof(buf), &got, wait);
    if (s != WL_OK)
        return s;
    if (got > 0)
        l->ops->rx(l->impl, buf, got, wl_clock_ms());
    s = flush_tx(l, t, wire);
    if (s != WL_OK)
        return s;
    return l->ops->failed(l->impl);
}
