#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "devlink/posix_fd.h"
#include "devlink/transport.h"

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

/* The kernel will set whatever rate the hardware supports, so the useful
 * fact here is what the port is running at now; the FTDI at this end goes far
 * beyond anything the other end will agree to. */
static wl_status fd_caps(void *impl, char *out, size_t cap)
{
    struct fd_impl *s = impl;
    struct termios tio;
    speed_t sp;
    int n;

    if (tcgetattr(s->fd, &tio) != 0)
        return wl_fail(WL_ERR_IO, "caps: not a terminal: %s", strerror(errno));
    sp = cfgetospeed(&tio);
    n = snprintf(out, cap,
                 "transport serial (posix)\n"
                 "current %lu\n"
                 "settable any rate the kernel and the hardware agree on\n",
                 (unsigned long)sp);
    if (n < 0 || (size_t)n >= cap)
        return wl_fail(WL_ERR_NOSPACE, "caps: buffer too small");
    return WL_OK;
}

static int fd_fd(void *impl)
{
    return ((struct fd_impl *)impl)->fd;
}

static const struct wl_transport_ops fd_ops = {fd_read, fd_write, fd_close, fd_caps, fd_fd};

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

/*
 * Names whoever else has the port open, for the message. Best effort: it walks
 * /proc looking for a descriptor on the same character device, and says nothing
 * if it cannot tell. Comparing device numbers rather than paths is what makes
 * it work through /dev/serial/by-id symlinks.
 */
static void name_the_holder(const char *path, char *out, size_t cap)
{
    struct stat want;
    DIR *proc;
    struct dirent *e;

    out[0] = '\0';
    if (stat(path, &want) != 0 || !S_ISCHR(want.st_mode))
        return;
    proc = opendir("/proc");
    if (proc == NULL)
        return;
    while ((e = readdir(proc)) != NULL && out[0] == '\0') {
        char dir[286]; /* /proc/<pid>/fd, with room for any pid */
        DIR *fds;
        struct dirent *f;

        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        if (snprintf(dir, sizeof(dir), "/proc/%s/fd", e->d_name) >= (int)sizeof(dir))
            continue;
        fds = opendir(dir);
        if (fds == NULL)
            continue; /* not ours to look at, which is fine */
        while ((f = readdir(fds)) != NULL) {
            char full[556];
            struct stat got;

            if (f->d_name[0] == '.')
                continue;
            if (snprintf(full, sizeof(full), "%s/%s", dir, f->d_name) >= (int)sizeof(full))
                continue;
            if (stat(full, &got) != 0 || !S_ISCHR(got.st_mode) || got.st_rdev != want.st_rdev)
                continue;
            if ((int)strtol(e->d_name, NULL, 10) == getpid())
                continue;
            {
                char comm[64] = "";
                char commpath[286];
                FILE *cf;

                (void)snprintf(commpath, sizeof(commpath), "/proc/%s/comm", e->d_name);
                cf = fopen(commpath, "r");
                if (cf != NULL) {
                    if (fgets(comm, (int)sizeof(comm), cf) != NULL) {
                        size_t n = strlen(comm);

                        while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r'))
                            comm[--n] = '\0';
                    }
                    (void)fclose(cf);
                }
                char pid[16];

                size_t k;

                /* Copied by hand rather than with snprintf: this is a pid, but
                 * the compiler only knows it is a directory name, and any
                 * formatted copy of one into a small buffer is a truncation
                 * warning the strict flags refuse. */
                for (k = 0; k + 1 < sizeof(pid) && e->d_name[k] != '\0'; k++)
                    pid[k] = e->d_name[k];
                pid[k] = '\0';
                (void)snprintf(out, cap, " (held by pid %s%s%s)", pid,
                               comm[0] != '\0' ? ", " : "", comm);
            }
            break;
        }
        (void)closedir(fds);
    }
    (void)closedir(proc);
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

    /*
     * One user of the line at a time. Nothing in the operating system enforces
     * this for a serial port, and the consequence is not two programs politely
     * interleaving: the agent serves one client and a second client displaces
     * the first, so a command already running on the target is killed. That
     * has happened, to somebody's install.
     *
     * flock on the descriptor rather than a file in /var/lock, which is
     * root-only here, and which would need stale locks cleaned up. The kernel
     * drops this one when the process exits, however it exits.
     */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char who[160];
        wl_status st;

        name_the_holder(path, who, sizeof(who));
        st = wl_fail(WL_ERR_IO, "serial: %s is already in use%s; wait for it to finish",
                     path, who);
        (void)close(fd);
        return st;
    }

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
