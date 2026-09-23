#ifndef DEVLINK_LINKTEST_H
#define DEVLINK_LINKTEST_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/session.h"
#include "devlink/transport.h"

/*
 * Link throughput and integrity test over a real port, through the full
 * session stack (link plus compression): the probe streams data, the server
 * echoes the plaintext back, and the probe checks every byte. Both directions
 * run at once, so this measures what the operations above will get.
 *
 * The data is either a caller buffer (a file, to measure real compression) or
 * a built-in pseudo-random sequence, which does not compress and so shows the
 * raw link rate.
 */
struct wl_linktest_result {
    uint64_t bytes;      /* verified bytes echoed back */
    uint64_t corrupt;    /* echoed bytes that did not match */
    uint32_t elapsed_ms; /* from link up to the last byte verified */
    uint32_t sessions;   /* link generations seen; more than 1 means a reset */
    struct wl_link_stats stats;
    struct wl_session_stats zstats;
};

/* Streams total bytes through the peer's echo, giving up after limit_ms.
 * data may be NULL for the pseudo-random sequence; otherwise it must hold
 * total bytes. */
wl_status wl_linktest_probe(struct wl_transport *t, const struct wl_session_cfg *cfg,
                            uint32_t seed, const uint8_t *data, uint64_t total,
                            uint32_t limit_ms, struct wl_linktest_result *out);

/* Echoes the stream until idle_ms pass without stream data (once some has
 * arrived) or wait_ms pass without the link coming up. */
wl_status wl_linktest_serve(struct wl_transport *t, const struct wl_session_cfg *cfg,
                            uint32_t seed, uint32_t wait_ms, uint32_t idle_ms,
                            struct wl_linktest_result *out);

#endif
