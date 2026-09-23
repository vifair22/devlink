#ifndef W98LINK_CLOCK_H
#define W98LINK_CLOCK_H

#include <stdint.h>

#include "w98link/status.h"

/*
 * Monotonic milliseconds from an arbitrary origin, implemented per platform.
 * It wraps after about 49 days (Win32's GetTickCount does), so callers only
 * ever compare differences computed in uint32_t arithmetic.
 */
uint32_t wl_clock_ms(void);

static inline uint32_t wl_clock_since(uint32_t start)
{
    return wl_clock_ms() - start;
}

/*
 * The wall clock: seconds since 1970-01-01 UTC, the same scale the protocol
 * uses for file times. Deliberately separate from wl_clock_ms, which is
 * monotonic and unaffected by any of this; that separation is what makes it
 * safe to set the clock under a running session, where the link's timers
 * would otherwise jump.
 */
wl_status wl_clock_utc(uint64_t *secs);

/* Minutes to add to UTC to get the machine's local time, daylight saving
 * included: -300 for US Eastern in winter. Zero if the platform will not say,
 * which is only a display detail. */
int wl_clock_local_offset_minutes(void);

/*
 * Sets the wall clock. Fails with WL_ERR_INVAL where it is not supported,
 * which includes every POSIX build: the client has no business setting its
 * own clock, and the POSIX agent exists to be tested rather than to run a
 * machine, so a test can never move the developer's clock.
 */
wl_status wl_clock_set_utc(uint64_t secs);

#endif
