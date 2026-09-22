#include <windows.h>

#include "w98link/clock.h"

/* GetTickCount exists on every Win32 and wraps at 49.7 days, which matches
 * the contract in clock.h. Its resolution on Win9x is about 55 ms (the PIT
 * tick); that is fine for link timers measured in hundreds of ms. */
uint32_t wl_clock_ms(void)
{
    return (uint32_t)GetTickCount();
}
