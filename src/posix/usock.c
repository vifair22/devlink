#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "w98link/posix_fd.h"
#include "w98link/usock.h"

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
    return wl_fd_transport(fd, out);
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
    return wl_fd_transport(fd, out);
}
