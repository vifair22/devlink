#ifndef W98LINK_CLOCK_H
#define W98LINK_CLOCK_H

#include <stdint.h>

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

#endif
