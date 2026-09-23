#ifndef DEVLINK_ECHO_H
#define DEVLINK_ECHO_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/frame.h"
#include "devlink/transport.h"

/*
 * Frame echo test: the first thing run over a new cable, port or build. One
 * side serves (echoes every DATA frame back unchanged), the other probes
 * (sends numbered frames stop-and-wait and checks each echo). It exercises
 * the transport and the framing without the link state machine, so a failure
 * here is the wire or the port, not the protocol.
 *
 * The probe ends a run with an ACK frame, which tells the server to return at
 * once instead of waiting out its idle timeout.
 */
struct wl_echo_stats {
    uint32_t sent;       /* probe: DATA frames sent */
    uint32_t echoed;     /* probe: correct echoes; serve: frames echoed */
    uint32_t mismatched; /* probe: echo with the right seq but wrong payload */
    uint32_t timeouts;   /* probe: no echo within the timeout */
    uint32_t elapsed_ms;
    struct wl_frame_rx_stats rx;
};

/* Echoes DATA frames until an ACK frame arrives or idle_ms pass with no valid
 * frame. The idle clock starts at the call, so the probe has idle_ms to begin. */
wl_status wl_echo_serve(struct wl_transport *t, uint32_t idle_ms,
                        struct wl_echo_stats *st);

/* Sends count frames of payload_len bytes, waiting up to timeout_ms for each
 * echo. Lost or damaged echoes are counted, not treated as errors; only a
 * transport failure makes this return non-OK. */
wl_status wl_echo_probe(struct wl_transport *t, uint32_t count, size_t payload_len,
                        uint32_t timeout_ms, struct wl_echo_stats *st);

#endif
