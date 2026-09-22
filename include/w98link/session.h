#ifndef W98LINK_SESSION_H
#define W98LINK_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/link.h"
#include "w98link/transport.h"

/*
 * Compression layer (docs/protocol.md, "Compression layer"): one raw deflate
 * stream per direction, living exactly as long as a link session, between the
 * mux above and the link below.
 *
 * Like the link it does no I/O of its own. wl_session_work() moves data
 * through the streams; wl_session_pump() adds the transport for real use.
 *
 * Flushing: the compressor runs with Z_NO_FLUSH while it has plaintext and
 * issues Z_SYNC_FLUSH only when the plaintext queue is empty and the link has
 * room. During a bulk transfer the link is the bottleneck, the queue never
 * empties, and nothing is wasted on flushes; when traffic is interactive,
 * every message goes out at once.
 */

#define WL_SESSION_QUEUE 65536U /* plaintext buffered each way */
#define WL_SESSION_DEFAULT_LEVEL 9 /* CPU is free next to a 11 KB/s line */

struct wl_session_cfg {
    struct wl_link_cfg link;
    int level; /* zlib level 0-9; the peer needs no knowledge of it */
};

/* As wl_link_cfg_init, for the whole session configuration. */
void wl_session_cfg_init(struct wl_session_cfg *cfg);

struct wl_session_stats {
    uint64_t plain_out; /* plaintext bytes compressed and sent */
    uint64_t plain_in;  /* plaintext bytes received and decompressed */
    uint64_t z_out;     /* compressed bytes handed to the link */
    uint64_t z_in;      /* compressed bytes taken from the link */
    uint32_t flushes;
};

struct wl_session;

struct wl_session *wl_session_new(const struct wl_session_cfg *cfg, uint32_t seed,
                                  uint32_t now);
void wl_session_free(struct wl_session *s);

/* The link underneath, for feeding received bytes and draining frames when
 * the caller drives I/O itself (the tests' simulated line does). */
struct wl_link *wl_session_link(struct wl_session *s);

/* Moves plaintext -> deflate -> link and link -> inflate -> plaintext as far
 * as buffers allow. Fails only if the peer's stream does not decompress,
 * which on a reliable link means a protocol mismatch or a bug. */
wl_status wl_session_work(struct wl_session *s);

/* work, then one wl_link_pump, then work again. */
wl_status wl_session_pump(struct wl_session *s, struct wl_transport *t,
                          uint32_t max_wait_ms);

/* Queues plaintext; returns how much fit (0 when full or not up). */
size_t wl_session_send(struct wl_session *s, const uint8_t *data, size_t n);

size_t wl_session_recv(struct wl_session *s, uint8_t *buf, size_t cap);

/* Nonzero while anything sent has not yet been acknowledged by the peer:
 * queued plaintext, compressor state, or unacknowledged link frames. */
int wl_session_busy(const struct wl_session *s);

int wl_session_is_up(const struct wl_session *s);
uint32_t wl_session_generation(const struct wl_session *s);
void wl_session_get_stats(const struct wl_session *s, struct wl_session_stats *out);

#endif
