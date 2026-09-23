#ifndef W98LINK_MUX_H
#define W98LINK_MUX_H

#include <stdint.h>

#include "w98link/peer.h"
#include "w98link/session.h"
#include "w98link/transport.h"

/*
 * The local multiplexer (docs/plan.md, iteration 11).
 *
 * One process owns the line and keeps a single session to the agent. Client
 * invocations connect to a Unix socket and get a channel of their own, so a
 * long build in one terminal and a directory listing in another no longer
 * fight over the line. Before this, every invocation opened the port, ran one
 * command and exited, and because the agent serves one session a second
 * invocation displaced the first and killed whatever it was running.
 *
 * It is a relay, not a second protocol: a client speaks the ordinary wire
 * protocol to it, and every channel the client opens is opened again on the
 * line and the records passed through. That is why nothing in the client
 * needed changing, and why the agent needed nothing at all.
 *
 * Two things fall out of the single session. The handshake happens once rather
 * than per command, and the compression streams live as long as the multiplexer
 * does, so the deflate dictionary keeps what it learned from earlier commands
 * instead of starting from nothing every time.
 *
 * Concurrency only, no detach: a channel belongs to the client that opened it
 * and dies with it, so Ctrl-C or a closed terminal stops the remote command
 * exactly as it did before.
 */

struct wl_mux_cfg {
    struct wl_session_cfg session; /* for the line; the local sessions copy it */
    const char *socket_path;
    /* Exit after this long with no client connected. 0 never exits. */
    uint32_t idle_exit_ms;
    /* Called once per loop; nonzero stops the multiplexer. NULL runs until
     * idle_exit_ms or a signal. */
    int (*stop)(void *ctx);
    void *stop_ctx;
    void (*log)(void *ctx, const char *msg);
    void *log_ctx;
};

/*
 * Runs until stopped, the idle timer expires, or the line fails. The transport
 * is the line to the agent and is not closed here.
 */
wl_status wl_mux_serve(struct wl_transport *line, const struct wl_mux_cfg *cfg,
                       uint32_t seed);

/* The socket a client should look for, built from the line's name so two lines
 * get two multiplexers. Returns 0 if it does not fit. */
int wl_mux_socket_path(const char *line, char *out, size_t cap);

#endif
