#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "devlink/random.h"

/* /dev/urandom, for a kernel older than getrandom(2) (3.17), which a rescue
 * image booting an old PC may well carry. */
static wl_status from_urandom(uint8_t *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t done = 0;

    if (fd < 0)
        return wl_fail(WL_ERR_IO, "random: /dev/urandom: %s", strerror(errno));
    while (done < n) {
        ssize_t r = read(fd, buf + done, n - done);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            int err = r < 0 ? errno : EIO;

            (void)close(fd);
            return wl_fail(WL_ERR_IO, "random: /dev/urandom: %s", strerror(err));
        }
        done += (size_t)r;
    }
    (void)close(fd);
    return WL_OK;
}

wl_status wl_random_bytes(uint8_t *buf, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t r = getrandom(buf + done, n - done, 0);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ENOSYS)
                return from_urandom(buf + done, n - done);
            return wl_fail(WL_ERR_IO, "random: getrandom: %s", strerror(errno));
        }
        done += (size_t)r;
    }
    return WL_OK;
}
