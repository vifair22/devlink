#include <errno.h>
#include <poll.h>

#include "devlink/wait.h"

void wl_wait_any(const int *fds, size_t n, int unwaitable, uint32_t timeout_ms)
{
    struct pollfd p[16];
    size_t k = n < sizeof(p) / sizeof(p[0]) ? n : sizeof(p) / sizeof(p[0]);

    if (unwaitable && timeout_ms > WL_WAIT_POLL_MS)
        timeout_ms = WL_WAIT_POLL_MS;
    for (size_t i = 0; i < k; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[i].revents = 0;
    }
    /* An interrupted wait just returns early: the caller loops anyway. */
    (void)poll(p, (nfds_t)k, timeout_ms > 60000U ? 60000 : (int)timeout_ms);
}
