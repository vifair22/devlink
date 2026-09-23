#include <windows.h>

#include "w98link/clock.h"
#include "wintime.h"

/* GetTickCount exists on every Win32 and wraps at 49.7 days, which matches
 * the contract in clock.h. Its resolution on Win9x is about 55 ms (the PIT
 * tick); that is fine for link timers measured in hundreds of ms. */
uint32_t wl_clock_ms(void)
{
    return (uint32_t)GetTickCount();
}

wl_status wl_clock_utc(uint64_t *secs)
{
    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    *secs = wl_ft_to_unix(&ft);
    return WL_OK;
}

int wl_clock_local_offset_minutes(void)
{
    TIME_ZONE_INFORMATION tz;
    DWORD id = GetTimeZoneInformation(&tz);

    if (id == TIME_ZONE_ID_INVALID)
        return 0;
    /* Windows states the offset as minutes to add to local time to reach UTC,
     * and keeps the daylight part separate; this reports the other direction,
     * which is how people write an offset. */
    if (id == TIME_ZONE_ID_DAYLIGHT)
        return -(int)(tz.Bias + tz.DaylightBias);
    return -(int)(tz.Bias + tz.StandardBias);
}

void wl_clock_zone_name(char *buf, size_t cap)
{
    TIME_ZONE_INFORMATION tz;
    DWORD id = GetTimeZoneInformation(&tz);
    const WCHAR *name;

    buf[0] = '\0';
    if (id == TIME_ZONE_ID_INVALID)
        return;
    name = id == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightName : tz.StandardName;
    /* The only wide string in the agent, and it comes from the API rather
     * than from us; a zone name is ASCII in practice, and a lone conversion
     * is better than reporting nothing. */
    if (WideCharToMultiByte(CP_ACP, 0, name, -1, buf, (int)cap, NULL, NULL) == 0)
        buf[0] = '\0';
}

/*
 * On NT the privilege to set the clock is held by administrators but disabled
 * in the token until asked for, so SetSystemTime fails with ERROR_PRIVILEGE_
 * NOT_HELD unless this runs first. On Win9x there are no privileges and these
 * calls simply fail, which is why the result is ignored.
 */
static void ask_for_the_privilege(void)
{
    TOKEN_PRIVILEGES tp;
    HANDLE token;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
        return;
    memset(&tp, 0, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(NULL, "SeSystemtimePrivilege", &tp.Privileges[0].Luid))
        (void)AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(token);
}

wl_status wl_clock_set_utc(uint64_t secs)
{
    SYSTEMTIME st;
    FILETIME ft;

    wl_unix_to_ft(secs, &ft);
    if (!FileTimeToSystemTime(&ft, &st))
        return wl_fail(WL_ERR_INVAL, "clock: %llu is not a time this system can hold",
                       (unsigned long long)secs);
    ask_for_the_privilege();
    /* The UTC clock is set, not the local one: the protocol's file times are
     * UTC, so that is what must be right even if this machine's time zone is
     * not. */
    if (!SetSystemTime(&st))
        return wl_fail(WL_ERR_IO, "clock: SetSystemTime failed (%lu)", GetLastError());
    return WL_OK;
}
