#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "devlink/clock.h"

uint32_t wl_clock_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U));
}

wl_status wl_clock_utc(uint64_t *secs)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return wl_fail(WL_ERR_IO, "clock: CLOCK_REALTIME is unavailable");
    *secs = (uint64_t)ts.tv_sec;
    return WL_OK;
}

int wl_clock_local_offset_minutes(void)
{
    time_t now = time(NULL);
    struct tm local;

    if (localtime_r(&now, &local) == NULL)
        return 0;
    return (int)(local.tm_gmtoff / 60);
}

void wl_clock_zone_name(char *buf, size_t cap)
{
    time_t now = time(NULL);
    struct tm local;

    buf[0] = '\0';
    if (localtime_r(&now, &local) == NULL)
        return;
    if (strftime(buf, cap, "%Z", &local) == 0)
        buf[0] = '\0';
}

wl_status wl_clock_set_utc(uint64_t secs)
{
#ifdef DEVLINK_AGENT
    /*
     * The Linux agent runs a machine, often one whose CMOS battery is why the
     * clock is wrong, so here it is allowed. Only the system clock: the
     * hardware clock on a Windows machine keeps local time, which this side
     * cannot know, so writing it is left to an explicit hwclock --localtime
     * from a session that does.
     */
    struct timespec ts;

    if (secs > (uint64_t)INT32_MAX && sizeof(time_t) < 8)
        return wl_fail(WL_ERR_INVAL, "clock: %llu is past what time_t holds here",
                       (unsigned long long)secs);
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0)
        return wl_fail(WL_ERR_IO, "clock: %s", strerror(errno));
    return WL_OK;
#else
    /*
     * Refused on purpose, not for want of an implementation. This backend is
     * the client and the tested agent, neither of which has any business
     * moving this machine's clock, and a test that could do it by accident is
     * a test that will one day set a developer's clock to 1970.
     */
    (void)secs;
    return wl_fail(WL_ERR_INVAL, "setting the clock is not supported on this platform");
#endif
}
