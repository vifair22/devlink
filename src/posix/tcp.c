#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "devlink/clock.h"
#include "devlink/tcp.h"

/*
 * TCP over BSD sockets, for the client (connect) and the Linux agent
 * (listen). The transport is usock.c's with two differences that matter on a
 * network: a reset is reported as the peer going away, like a close, and the
 * caps output names both ends. send() carries MSG_NOSIGNAL for usock.c's
 * reason: a write to a connection the peer has closed would otherwise raise
 * SIGPIPE and kill the process.
 */

struct tcp_impl {
    int fd;
};

struct wl_tcp_listener {
    int fd;
    uint16_t port;
};

static int poll_ms(uint32_t ms)
{
    return ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)ms;
}

static wl_status peer_gone(int err, const char *op)
{
    if (err == ECONNRESET || err == EPIPE)
        return wl_fail(WL_ERR_IO, "the peer closed the connection (%s: %s)", op,
                       strerror(err));
    return wl_fail(WL_ERR_IO, "%s: %s", op, strerror(err));
}

static wl_status tcp_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                          uint32_t timeout_ms)
{
    struct tcp_impl *s = impl;
    struct pollfd p = {s->fd, POLLIN, 0};
    ssize_t n;
    int r;

    *got = 0;
    do {
        r = poll(&p, 1, poll_ms(timeout_ms));
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return wl_fail(WL_ERR_IO, "poll: %s", strerror(errno));
    if (r == 0)
        return WL_OK;
    do {
        n = recv(s->fd, buf, cap, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return peer_gone(errno, "recv");
    if (n == 0)
        return wl_fail(WL_ERR_IO, "the peer closed the connection");
    *got = (size_t)n;
    return WL_OK;
}

static wl_status tcp_write(void *impl, const uint8_t *buf, size_t len)
{
    struct tcp_impl *s = impl;

    while (len > 0) {
        ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return peer_gone(errno, "send");
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return WL_OK;
}

static void tcp_close(void *impl)
{
    struct tcp_impl *s = impl;

    (void)close(s->fd);
    free(s);
}

static int tcp_fd(void *impl)
{
    return ((struct tcp_impl *)impl)->fd;
}

/* "a.b.c.d:port" or "[v6]:port". An IPv4 peer reaching a dual-stack listener
 * arrives as ::ffff:a.b.c.d, and is written the way a person would type it. */
static void format_addr(const struct sockaddr_storage *ss, char *out, size_t cap)
{
    /* Numeric only (NI_NUMERICHOST), so the longest is an IPv6 address, not
     * NI_MAXHOST's hostname; sizing it so is what lets the compiler see the
     * formatted result fits the callers' buffers. */
    char host[INET6_ADDRSTRLEN];
    char serv[NI_MAXSERV];
    struct sockaddr_in v4;
    const struct sockaddr *sa = (const struct sockaddr *)ss;
    socklen_t len = ss->ss_family == AF_INET6 ? (socklen_t)sizeof(struct sockaddr_in6)
                                              : (socklen_t)sizeof(struct sockaddr_in);

    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)ss;

        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            memset(&v4, 0, sizeof(v4));
            v4.sin_family = AF_INET;
            v4.sin_port = v6->sin6_port;
            memcpy(&v4.sin_addr, &v6->sin6_addr.s6_addr[12], 4);
            sa = (const struct sockaddr *)&v4;
            len = (socklen_t)sizeof(v4);
        }
    }
    if (getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        (void)snprintf(out, cap, "?");
        return;
    }
    if (sa->sa_family == AF_INET6)
        (void)snprintf(out, cap, "[%s]:%s", host, serv);
    else
        (void)snprintf(out, cap, "%s:%s", host, serv);
}

static wl_status tcp_caps(void *impl, char *out, size_t cap)
{
    struct tcp_impl *s = impl;
    struct sockaddr_storage peer;
    struct sockaddr_storage local;
    socklen_t plen = sizeof(peer);
    socklen_t llen = sizeof(local);
    char p[80];
    char l[80];
    int n;

    memset(&peer, 0, sizeof(peer));
    memset(&local, 0, sizeof(local));
    if (getpeername(s->fd, (struct sockaddr *)&peer, &plen) != 0)
        return wl_fail(WL_ERR_IO, "caps: getpeername: %s", strerror(errno));
    if (getsockname(s->fd, (struct sockaddr *)&local, &llen) != 0)
        return wl_fail(WL_ERR_IO, "caps: getsockname: %s", strerror(errno));
    format_addr(&peer, p, sizeof(p));
    format_addr(&local, l, sizeof(l));
    n = snprintf(out, cap, "transport tcp\npeer %s\nlocal %s\n", p, l);
    if (n < 0 || (size_t)n >= cap)
        return wl_fail(WL_ERR_NOSPACE, "caps: buffer too small");
    return WL_OK;
}

static const struct wl_transport_ops tcp_ops = {tcp_read, tcp_write, tcp_close, tcp_caps,
                                                tcp_fd};

/* Takes ownership of fd: on failure it is closed. */
static wl_status tcp_transport(int fd, struct wl_transport *out)
{
    struct tcp_impl *s;
    int one = 1;

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        wl_status st = wl_fail(WL_ERR_IO, "setsockopt TCP_NODELAY: %s", strerror(errno));

        (void)close(fd);
        return st;
    }
    s = malloc(sizeof(*s));
    if (s == NULL) {
        (void)close(fd);
        return wl_fail(WL_ERR_NOMEM, "tcp transport: out of memory");
    }
    s->fd = fd;
    out->ops = &tcp_ops;
    out->impl = s;
    return WL_OK;
}

static wl_status set_blocking(int fd, int blocking)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0)
        return wl_fail(WL_ERR_IO, "fcntl: %s", strerror(errno));
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (fcntl(fd, F_SETFL, flags) != 0)
        return wl_fail(WL_ERR_IO, "fcntl: %s", strerror(errno));
    return WL_OK;
}

/*
 * One address, within what is left of the caller's timeout. connect() on a
 * blocking socket waits for the kernel's own timeout (minutes, for an address
 * that drops SYNs), so the socket is non-blocking for the attempt and poll()
 * holds the deadline; it is made blocking again after, since the transport's
 * write is a blocking send.
 */
static wl_status try_address(const struct addrinfo *ai, const char *what, uint32_t left_ms,
                             int *fd_out)
{
    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    ai->ai_protocol);
    wl_status st;
    int err = 0;
    socklen_t elen = sizeof(err);

    if (fd < 0)
        return wl_fail(WL_ERR_IO, "socket: %s", strerror(errno));
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        uint32_t start = wl_clock_ms();
        int r;

        if (errno != EINPROGRESS) {
            st = wl_fail(WL_ERR_IO, "connect %s: %s", what, strerror(errno));
            (void)close(fd);
            return st;
        }
        for (;;) {
            uint32_t spent = wl_clock_since(start);

            r = poll(&p, 1, poll_ms(spent >= left_ms ? 0 : left_ms - spent));
            if (r >= 0 || errno != EINTR)
                break;
        }
        if (r == 0) {
            (void)close(fd);
            return wl_fail(WL_ERR_IO, "connect %s: no answer within %lu ms", what,
                           (unsigned long)left_ms);
        }
        if (r < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
            err = errno;
        if (err != 0) {
            (void)close(fd);
            return wl_fail(WL_ERR_IO, "connect %s: %s", what, strerror(err));
        }
    }
    st = set_blocking(fd, 1);
    if (st != WL_OK) {
        (void)close(fd);
        return st;
    }
    *fd_out = fd;
    return WL_OK;
}

wl_status wl_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                         struct wl_transport *out)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    const struct addrinfo *ai;
    char serv[8];
    char what[300];
    wl_status st = WL_OK;
    uint32_t start;
    int fd = -1;
    int r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;
    (void)snprintf(serv, sizeof(serv), "%u", (unsigned)port);
    (void)snprintf(what, sizeof(what), "%s:%u", host, (unsigned)port);
    r = getaddrinfo(host, serv, &hints, &list);
    if (r != 0)
        return wl_fail(WL_ERR_IO, "cannot resolve %s: %s", host,
                       r == EAI_SYSTEM ? strerror(errno) : gai_strerror(r));
    /* A name may have several addresses (IPv6 and IPv4 for localhost); the
     * first that answers wins, and the error kept is the last one's. */
    start = wl_clock_ms();
    for (ai = list; ai != NULL; ai = ai->ai_next) {
        uint32_t spent = wl_clock_since(start);

        if (ai != list && spent >= timeout_ms)
            break;
        st = try_address(ai, what, spent >= timeout_ms ? 0 : timeout_ms - spent, &fd);
        if (st == WL_OK)
            break;
    }
    freeaddrinfo(list);
    if (fd < 0)
        return st;
    return tcp_transport(fd, out);
}

/*
 * Every interface means one IPv6 socket with IPV6_V6ONLY off, which takes IPv4
 * as well, so a client can reach the agent by either. A system without IPv6
 * fails that socket() and gets an IPv4 one instead.
 *
 * This and open_at return the bound socket, or -1 with the message recorded;
 * every failure is WL_ERR_IO.
 */
static int open_any(uint16_t port)
{
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int zero = 0;
    int one = 1;
    int err;

    if (fd >= 0) {
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons(port);
        a6.sin6_addr = in6addr_any;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (const struct sockaddr *)&a6, sizeof(a6)) == 0)
            return fd;
        err = errno;
        (void)close(fd);
        if (err == EADDRINUSE) {
            (void)wl_fail(WL_ERR_IO, "bind port %u: %s", (unsigned)port, strerror(EADDRINUSE));
            return -1;
        }
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        (void)wl_fail(WL_ERR_IO, "socket: %s", strerror(errno));
        return -1;
    }
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_port = htons(port);
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (const struct sockaddr *)&a4, sizeof(a4)) != 0) {
        (void)wl_fail(WL_ERR_IO, "bind port %u: %s", (unsigned)port, strerror(errno));
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int open_at(const char *addr, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    char serv[8];
    int one = 1;
    int fd;
    int r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;
    (void)snprintf(serv, sizeof(serv), "%u", (unsigned)port);
    r = getaddrinfo(addr, serv, &hints, &list);
    if (r != 0) {
        (void)wl_fail(WL_ERR_IO, "cannot resolve %s: %s", addr,
                      r == EAI_SYSTEM ? strerror(errno) : gai_strerror(r));
        return -1;
    }
    fd = socket(list->ai_family, list->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                list->ai_protocol);
    if (fd < 0) {
        (void)wl_fail(WL_ERR_IO, "socket: %s", strerror(errno));
        freeaddrinfo(list);
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, list->ai_addr, list->ai_addrlen) != 0) {
        (void)wl_fail(WL_ERR_IO, "bind %s:%u: %s", addr, (unsigned)port, strerror(errno));
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

wl_status wl_tcp_listen(const char *bind_addr, uint16_t port, struct wl_tcp_listener **out)
{
    struct wl_tcp_listener *l;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    wl_status st;
    int fd;

    /* The listener is non-blocking so that accept() after poll() cannot hang
     * on a connection that was reset while it waited in the queue. */
    if (bind_addr == NULL || bind_addr[0] == '\0')
        fd = open_any(port);
    else
        fd = open_at(bind_addr, port);
    if (fd < 0)
        return WL_ERR_IO;
    memset(&ss, 0, sizeof(ss));
    if (listen(fd, 8) != 0) {
        st = wl_fail(WL_ERR_IO, "listen: %s", strerror(errno));
        (void)close(fd);
        return st;
    }
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
        st = wl_fail(WL_ERR_IO, "getsockname: %s", strerror(errno));
        (void)close(fd);
        return st;
    }
    l = malloc(sizeof(*l));
    if (l == NULL) {
        (void)close(fd);
        return wl_fail(WL_ERR_NOMEM, "tcp listener: out of memory");
    }
    l->fd = fd;
    l->port = ss.ss_family == AF_INET6 ? ntohs(((const struct sockaddr_in6 *)&ss)->sin6_port)
                                       : ntohs(((const struct sockaddr_in *)&ss)->sin_port);
    *out = l;
    return WL_OK;
}

wl_status wl_tcp_accept(struct wl_tcp_listener *l, uint32_t timeout_ms, struct wl_transport *out,
                        char *peer, size_t peer_cap)
{
    struct pollfd p = {l->fd, POLLIN, 0};
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd;
    int r;

    memset(&ss, 0, sizeof(ss));
    out->ops = NULL;
    out->impl = NULL;
    do {
        r = poll(&p, 1, poll_ms(timeout_ms));
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return wl_fail(WL_ERR_IO, "poll: %s", strerror(errno));
    if (r == 0)
        return WL_OK;
    fd = accept4(l->fd, (struct sockaddr *)&ss, &len, SOCK_CLOEXEC);
    if (fd < 0) {
        /* The connection went away between poll and accept, or a signal
         * came in: as far as the caller is concerned, nobody arrived. */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED ||
            errno == EINTR || errno == EPROTO)
            return WL_OK;
        return wl_fail(WL_ERR_IO, "accept: %s", strerror(errno));
    }
    if (peer != NULL && peer_cap > 0)
        format_addr(&ss, peer, peer_cap);
    return tcp_transport(fd, out);
}

int wl_tcp_listener_fd(const struct wl_tcp_listener *l)
{
    return l->fd;
}

uint16_t wl_tcp_listener_port(const struct wl_tcp_listener *l)
{
    return l->port;
}

void wl_tcp_listener_close(struct wl_tcp_listener *l)
{
    if (l == NULL)
        return;
    (void)close(l->fd);
    free(l);
}
