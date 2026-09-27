#include <winsock.h>

#include "devlink/wait.h"

/*
 * select() with Winsock 1.1, which Windows 98 and 2000 both have. It takes
 * only sockets and fails outright on empty sets, so with nothing to select
 * on it is a plain Sleep. Winsock must already be started: a caller with
 * sockets to wait on has one only because the TCP transport started it.
 */
void wl_wait_any(const int *fds, size_t n, int unwaitable, uint32_t timeout_ms)
{
    fd_set set;
    struct timeval tv;

    if (unwaitable && timeout_ms > WL_WAIT_POLL_MS)
        timeout_ms = WL_WAIT_POLL_MS;
    if (n == 0) {
        Sleep(timeout_ms);
        return;
    }
    FD_ZERO(&set);
    for (size_t i = 0; i < n && i < FD_SETSIZE; i++)
        FD_SET((SOCKET)fds[i], &set);
    tv.tv_sec = (long)(timeout_ms / 1000U);
    tv.tv_usec = (long)(timeout_ms % 1000U) * 1000L;
    (void)select(0, &set, NULL, NULL, &tv);
}
