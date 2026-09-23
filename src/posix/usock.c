#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "devlink/posix_fd.h"
#include "devlink/usock.h"

static wl_status fill_address(const char *path, struct sockaddr_un *addr, socklen_t *len)
{
    size_t n = strlen(path);

    if (n + 1 > sizeof(addr->sun_path))
        return wl_fail(WL_ERR_NOSPACE, "socket path too long: %s", path);
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, n + 1);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    return WL_OK;
}

/*
 * Sockets get their own transport rather than sharing the descriptor one,
 * for a single reason: write() to a socket whose peer has gone raises SIGPIPE,
 * whose default action is to kill the process. That is not theoretical. The
 * multiplexer's own autostart probe connects and closes at once, and the first
 * thing the multiplexer did was write a HELLO into it and die, leaving a stale
 * socket for the next attempt to trip over.
 *
 * send() with MSG_NOSIGNAL returns EPIPE instead, which the layers above
 * already handle as a peer that has gone. Ignoring SIGPIPE in main() would
 * also work, but it would put the safety of this code in the caller's hands,
 * and a library that dies when its peer hangs up is broken however carefully
 * it is called.
 */
struct sock_impl {
    int fd;
};

static wl_status sock_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                           uint32_t timeout_ms)
{
    struct sock_impl *s = impl;
    struct pollfd p = {s->fd, POLLIN, 0};
    ssize_t n;
    int r;

    *got = 0;
    do {
        r = poll(&p, 1, timeout_ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return wl_fail(WL_ERR_IO, "poll: %s", strerror(errno));
    if (r == 0)
        return WL_OK;
    do {
        n = recv(s->fd, buf, cap, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return wl_fail(WL_ERR_IO, "recv: %s", strerror(errno));
    if (n == 0)
        return wl_fail(WL_ERR_IO, "the peer closed the socket");
    *got = (size_t)n;
    return WL_OK;
}

static wl_status sock_write(void *impl, const uint8_t *buf, size_t len)
{
    struct sock_impl *s = impl;

    while (len > 0) {
        ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return wl_fail(WL_ERR_IO, "send: %s", strerror(errno));
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return WL_OK;
}

static void sock_close(void *impl)
{
    struct sock_impl *s = impl;

    (void)close(s->fd);
    free(s);
}

static int sock_fd(void *impl)
{
    return ((struct sock_impl *)impl)->fd;
}

static const struct wl_transport_ops sock_ops = {sock_read, sock_write, sock_close, NULL,
                                                 sock_fd};

static wl_status sock_transport(int fd, struct wl_transport *out)
{
    struct sock_impl *s = malloc(sizeof(*s));

    if (s == NULL) {
        (void)close(fd);
        return wl_fail(WL_ERR_NOMEM, "socket transport: out of memory");
    }
    s->fd = fd;
    out->ops = &sock_ops;
    out->impl = s;
    return WL_OK;
}

wl_status wl_unix_connect(const char *path, struct wl_transport *out)
{
    struct sockaddr_un addr;
    socklen_t len = 0;
    wl_status st = fill_address(path, &addr, &len);
    int fd;

    if (st != WL_OK)
        return st;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return wl_fail(WL_ERR_IO, "socket: %s", strerror(errno));
    if (connect(fd, (const struct sockaddr *)&addr, len) != 0) {
        st = wl_fail(WL_ERR_IO, "connect %s: %s", path, strerror(errno));
        (void)close(fd);
        return st;
    }
    return sock_transport(fd, out);
}

/* Nonzero if something is listening on the path right now. Used to tell a
 * socket left behind by a process that died from one with an owner. */
static int somebody_is_listening(const struct sockaddr_un *addr, socklen_t len)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int answered;

    if (fd < 0)
        return 0;
    answered = connect(fd, (const struct sockaddr *)addr, len) == 0;
    (void)close(fd);
    return answered;
}

wl_status wl_unix_listen(const char *path, int *listen_fd)
{
    struct sockaddr_un addr;
    socklen_t len = 0;
    wl_status st = fill_address(path, &addr, &len);
    int fd;

    if (st != WL_OK)
        return st;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return wl_fail(WL_ERR_IO, "socket: %s", strerror(errno));
    if (bind(fd, (const struct sockaddr *)&addr, len) != 0) {
        if (errno != EADDRINUSE) {
            st = wl_fail(WL_ERR_IO, "bind %s: %s", path, strerror(errno));
            (void)close(fd);
            return st;
        }
        if (somebody_is_listening(&addr, len)) {
            (void)close(fd);
            return wl_fail(WL_ERR_IO, "another multiplexer is already on %s", path);
        }
        /* Nobody answers it, so it outlived its owner. */
        (void)unlink(path);
        if (bind(fd, (const struct sockaddr *)&addr, len) != 0) {
            st = wl_fail(WL_ERR_IO, "bind %s: %s", path, strerror(errno));
            (void)close(fd);
            return st;
        }
    }
    if (listen(fd, 8) != 0) {
        st = wl_fail(WL_ERR_IO, "listen %s: %s", path, strerror(errno));
        (void)close(fd);
        (void)unlink(path);
        return st;
    }
    *listen_fd = fd;
    return WL_OK;
}

wl_status wl_unix_accept(int listen_fd, struct wl_transport *out)
{
    int fd;

    do {
        fd = accept(listen_fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return wl_fail(WL_ERR_IO, "accept: %s", strerror(errno));
    return sock_transport(fd, out);
}
