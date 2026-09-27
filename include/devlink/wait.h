#ifndef DEVLINK_WAIT_H
#define DEVLINK_WAIT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Waits until any of the descriptors is readable, or timeout_ms passes,
 * whichever comes first. What a descriptor is belongs to the platform: a file
 * descriptor on POSIX, a socket on Win32, where select() takes nothing else.
 *
 * unwaitable says that some other input cannot be waited on this way (a
 * Win32 serial port), so it has to be polled: the wait is then kept short
 * enough for polling it not to add noticeable delay.
 */
#define WL_WAIT_POLL_MS 10U

void wl_wait_any(const int *fds, size_t n, int unwaitable, uint32_t timeout_ms);

#endif
