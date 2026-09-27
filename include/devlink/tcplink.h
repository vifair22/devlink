#ifndef DEVLINK_TCPLINK_H
#define DEVLINK_TCPLINK_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/linkif.h"
#include "devlink/status.h"

/*
 * TCP link (docs/protocol.md, "Link layer: TCP"): a connection carries one
 * session, as a sequence of messages, each a u16 length and that many bytes.
 * TCP already makes the stream ordered and lossless, so this layer adds only
 * what a network needs and a cable does not: noticing a peer that has gone
 * without closing the connection, and (with a key) the Noise handshake and
 * encryption.
 *
 * Like the serial link it does no I/O and reads no clock; the session drives
 * it through the link interface (linkif.h).
 */

#define WL_TCPLINK_MAX_MSG 65535U  /* the u16 length's limit, tag included */
#define WL_TCPLINK_TAG 16U         /* what encryption adds to each message */
#define WL_TCPLINK_QUEUE 65536U    /* stream bytes buffered each way */
#define WL_TCPLINK_DEFAULT_DEAD_MS 10000U
#define WL_TCPLINK_DEFAULT_KEEPALIVE_MS 2000U
#define WL_TCPLINK_HANDSHAKE_MS 10000U

struct wl_tcplink_cfg {
    int initiator; /* the client starts the handshake; the agent answers */
    /* The 32-byte key from the passphrase. NULL runs the link in the clear,
     * which only the tests may do: no program ever speaks TCP without it. */
    const uint8_t *psk;
    /* As in wl_link_cfg: 0 uses the defaults above. */
    uint32_t dead_ms;
    uint32_t keepalive_ms;
};

/* Fills cfg with the defaults; as with wl_link_cfg_init, start here. */
void wl_tcplink_cfg_init(struct wl_tcplink_cfg *cfg);

/*
 * A wl_make_link_fn (session.h) building a TCP link from the
 * struct wl_tcplink_cfg that arg points to. Put it in a session's config,
 * with the tcplink config as its argument, to run that session over TCP.
 */
wl_status wl_tcplink_make(void *arg, uint32_t seed, uint32_t now, struct wl_linkif *out);

#endif
