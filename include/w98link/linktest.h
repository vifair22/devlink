#ifndef W98LINK_LINKTEST_H
#define W98LINK_LINKTEST_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/link.h"
#include "w98link/transport.h"

/*
 * Link throughput and integrity test over a real port: the probe streams a
 * deterministic byte sequence, the server echoes the stream back, and the
 * probe checks every byte. Both directions run at once, so this measures the
 * full-duplex rate the upper layers will get.
 */
struct wl_linktest_result {
    uint64_t bytes;      /* verified bytes echoed back */
    uint64_t corrupt;    /* echoed bytes that did not match */
    uint32_t elapsed_ms; /* from link up to the last byte verified */
    uint32_t sessions;   /* link generations seen; more than 1 means a reset */
    struct wl_link_stats stats;
};

/* Streams total bytes through the peer's echo, giving up after limit_ms. */
wl_status wl_linktest_probe(struct wl_transport *t, const struct wl_link_cfg *cfg,
                            uint32_t seed, uint64_t total, uint32_t limit_ms,
                            struct wl_linktest_result *out);

/* Echoes the stream until idle_ms pass without stream data (once some has
 * arrived) or wait_ms pass without the link coming up. */
wl_status wl_linktest_serve(struct wl_transport *t, const struct wl_link_cfg *cfg,
                            uint32_t seed, uint32_t wait_ms, uint32_t idle_ms,
                            struct wl_linktest_result *out);

#endif
