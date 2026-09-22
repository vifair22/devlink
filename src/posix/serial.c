#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "w98link/posix_fd.h"
#include "w98link/transport.h"

struct fd_impl {
    int fd;
};

static wl_status fd_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                         uint32_t timeout_ms)
{
    struct fd_impl *s = impl;
    struct pollfd p = {s->fd, POLLIN, 0};
    int timeout = timeout_ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)timeout_ms;
    ssize_t n;
    int r;

    *got = 0;
    do {
        r = poll(&p, 1, timeout);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return wl_fail(WL_ERR_IO, "poll: %s", strerror(errno));
    if (r == 0)
        return WL_OK;

    do {
        n = read(s->fd, buf, cap);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return wl_fail(WL_ERR_IO, "read: %s", strerror(errno));
    /* POLLHUP with nothing to read: the other end is gone (a pty whose peer
     * closed, later a socket). A serial port never reports this. */
    if (n == 0)
        return wl_fail(WL_ERR_IO, "read: peer closed");
    *got = (size_t)n;
    return WL_OK;
}

static wl_status fd_write(void *impl, const uint8_t *buf, size_t len)
{
    struct fd_impl *s = impl;

    while (len > 0) {
        ssize_t n = write(s->fd, buf, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return wl_fail(WL_ERR_IO, "write: %s", strerror(errno));
        buf += n;
        len -= (size_t)n;
    }
    return WL_OK;
}

static void fd_close(void *impl)
{
    struct fd_impl *s = impl;

    (void)close(s->fd);
    free(s);
}

static const struct wl_transport_ops fd_ops = {fd_read, fd_write, fd_close};

wl_status wl_fd_transport(int fd, struct wl_transport *out)
{
    struct fd_impl *s = malloc(sizeof(*s));

    if (s == NULL) {
        (void)close(fd);
        return wl_fail(WL_ERR_NOMEM, "fd transport: out of memory");
    }
    s->fd = fd;
    out->ops = &fd_ops;
    out->impl = s;
    return WL_OK;
}

static int baud_to_speed(uint32_t baud, speed_t *out)
{
    static const struct {
        uint32_t baud;
        speed_t speed;
    } table[] = {
        {9600, B9600},   {19200, B19200},   {38400, B38400},
        {57600, B57600}, {115200, B115200}, {230400, B230400},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].baud == baud) {
            *out = table[i].speed;
            return 1;
        }
    }
    return 0;
}

wl_status wl_serial_open(const char *path, uint32_t baud, struct wl_transport *out)
{
    struct termios tio;
    speed_t speed;
    int fd;

    if (!baud_to_speed(baud, &speed))
        return wl_fail(WL_ERR_INVAL, "serial: unsupported baud rate %u", (unsigned)baud);

    /* O_NOCTTY matters: without it a session leader that opens the port
     * acquires it as a controlling terminal, and job-control signals start
     * stopping the process on tcsetattr (seen with C-Kermit on this link). */
    fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return wl_fail(WL_ERR_IO, "serial: open %s: %s", path, strerror(errno));

    if (tcgetattr(fd, &tio) != 0) {
        wl_status st = wl_fail(WL_ERR_IO, "serial: tcgetattr %s: %s", path, strerror(errno));
        (void)close(fd);
        return st;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(tcflag_t)(CRTSCTS | CSTOPB | PARENB);
    tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    /* Reads are driven by poll(), so the driver should return whatever it
     * has instead of waiting for a minimum count. */
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0 ||
        tcsetattr(fd, TCSANOW, &tio) != 0) {
        wl_status st = wl_fail(WL_ERR_IO, "serial: configure %s: %s", path, strerror(errno));
        (void)close(fd);
        return st;
    }
    /* Discard anything that arrived before we were listening; it cannot be
     * part of a session that has not started. */
    (void)tcflush(fd, TCIOFLUSH);
    return wl_fd_transport(fd, out);
}
