#ifndef DEVLINK_TCP_H
#define DEVLINK_TCP_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/transport.h"

/*
 * TCP sockets as byte pipes behind struct wl_transport, one implementation per
 * platform: BSD sockets with IPv4 and IPv6 in src/posix/tcp.c, Winsock 1.1 and
 * IPv4 only in src/win32/tcp.c, because wsock32 is what Windows 98 and 2000
 * both have. What travels over them (the length-prefixed, Noise-encrypted
 * messages of docs/protocol.md "Link layer: TCP") is the link layer's
 * business, not this one's.
 *
 * Every socket handed out has TCP_NODELAY set. The link layer writes a whole
 * message at a time and then waits for an answer, which is the exact pattern
 * Nagle's algorithm turns into a 200 ms stall per round trip.
 *
 * A peer that closes or resets the connection is reported by read() and
 * write() as WL_ERR_IO with a message saying so, never as a stream of
 * zero-byte reads, which the transport contract reserves for a timeout.
 */

#define WL_TCP_DEFAULT_PORT 2098U

/*
 * Parses a line spec "tcp:HOST[:PORT]". HOST is a name, a dotted IPv4 address
 * or an IPv6 address in brackets ("tcp:[::1]:2098"); PORT defaults to
 * WL_TCP_DEFAULT_PORT.
 *
 * WL_ERR_INVAL means the spec does not start with "tcp:", and the caller then
 * treats it as a serial path. A spec that does start with it but is malformed
 * (no host, a bad port, an unclosed bracket, a bare IPv6 address) is
 * WL_ERR_CORRUPT, and a host longer than host_cap allows is WL_ERR_NOSPACE, so
 * neither can be mistaken for a serial port and opened as a file.
 */
wl_status wl_tcp_parse(const char *spec, char *host, size_t host_cap, uint16_t *port);

/* Connects, giving up after timeout_ms. Name lookup comes first and is not
 * covered by the timeout: neither getaddrinfo nor gethostbyname takes one. */
wl_status wl_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                         struct wl_transport *out);

struct wl_tcp_listener;

/*
 * Binds and listens. bind_addr NULL or "" is every interface (on POSIX both
 * IPv4 and IPv6 where the system allows one socket to take both). Port 0 asks
 * the system for a free port; wl_tcp_listener_port says which.
 *
 * POSIX sets SO_REUSEADDR so a restarted agent can bind while the previous
 * one's connections sit in TIME_WAIT. Win32 does not: there the same option
 * lets a second process bind the port while the first still holds it.
 */
wl_status wl_tcp_listen(const char *bind_addr, uint16_t port, struct wl_tcp_listener **out);

/*
 * Waits up to timeout_ms for a connection. A timeout is not an error: it
 * returns WL_OK with out->ops == NULL. Otherwise out is the new connection
 * and peer (when not NULL) its address, "a.b.c.d:port" or "[v6]:port".
 */
wl_status wl_tcp_accept(struct wl_tcp_listener *l, uint32_t timeout_ms, struct wl_transport *out,
                        char *peer, size_t peer_cap);

/* The listening socket to poll, on POSIX. -1 on Win32, where a socket is not
 * a descriptor. */
int wl_tcp_listener_fd(const struct wl_tcp_listener *l);

/* The port actually bound, which is the point of asking for port 0. */
uint16_t wl_tcp_listener_port(const struct wl_tcp_listener *l);

void wl_tcp_listener_close(struct wl_tcp_listener *l);

#endif
