#ifndef DEVLINK_POSIX_FD_H
#define DEVLINK_POSIX_FD_H

#include "devlink/transport.h"

/* Wraps an already-open descriptor as a transport and takes ownership of it.
 * Used for pty pairs in tests, and later for sockets. */
wl_status wl_fd_transport(int fd, struct wl_transport *out);

#endif
