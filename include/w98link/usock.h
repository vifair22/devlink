#ifndef W98LINK_USOCK_H
#define W98LINK_USOCK_H

#include "w98link/transport.h"

/*
 * Unix-domain sockets, for the local multiplexer: client invocations reach the
 * process that owns the line through one of these instead of opening the port
 * themselves (docs/plan.md, iteration 11).
 *
 * The link layer runs over the socket unchanged. Framing and a window are
 * unnecessary on a reliable local pipe, and the protocol defines a leaner form
 * for TCP, but reusing the serial link here costs a few per cent of local CPU
 * and no new code at all, and it keeps one implementation under test.
 */

/* Connects to a listening socket. Fails with WL_ERR_IO if nothing is there,
 * which is how a client discovers there is no multiplexer running. */
wl_status wl_unix_connect(const char *path, struct wl_transport *out);

/* Binds and listens. A socket left behind by a process that is gone is
 * removed first; one that is still answering is left alone and reported as
 * WL_ERR_IO, so two multiplexers cannot share a line. */
wl_status wl_unix_listen(const char *path, int *listen_fd);

/* Accepts one waiting connection as a transport. */
wl_status wl_unix_accept(int listen_fd, struct wl_transport *out);

#endif
