#include <stdio.h>

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
    size_t room = sizeof(buf);
    uint32_t wait;
    wl_status s = flush_tx(l, t, wire);

    if (s != WL_OK)
        return s;
    if (l->ops->rx_room != NULL) {
        room = l->ops->rx_room(l->impl);
        if (room > sizeof(buf))
            room = sizeof(buf);
        /* Nowhere to put input: the session above has to drain first, and it
         * runs as soon as this returns. */
        if (room == 0)
            return l->ops->failed(l->impl);
    }
    wait = l->ops->next_timeout(l->impl, wl_clock_ms());
    if (wait > max_wait_ms)
        wait = max_wait_ms;
    s = wl_transport_read(t, buf, room, &got, wait);
    if (s != WL_OK) {
        /* A link that never came up and never will (TCP) whose connection
         * closes has almost always been refused by the agent's handshake,
         * which says nothing on the wire by design. Say it here, where the
         * client can see it. */
        if (l->ops->rx_room != NULL && l->ops->generation(l->impl) == 0) {
            char why[256]; /* the size of the error buffer it copies */

            /* Copied first: wl_fail formats into the buffer wl_error reads. */
            (void)snprintf(why, sizeof(why), "%s", wl_error());
            return wl_fail(s, "%s before the handshake finished: the agent refused the "
                              "passphrase, or it is not a devlink agent", why);
        }
        return s;
    }
    if (got > 0)
        l->ops->rx(l->impl, buf, got, wl_clock_ms());
    s = flush_tx(l, t, wire);
    if (s != WL_OK)
        return s;
    return l->ops->failed(l->impl);
}
