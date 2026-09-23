#ifndef W98LINK_WIN32_WINTIME_H
#define W98LINK_WIN32_WINTIME_H

#include <stdint.h>
#include <windows.h>

/*
 * FILETIME to and from the protocol's seconds since 1970-01-01 UTC, shared by
 * the file and clock backends so the epoch constant exists once. FILETIME
 * counts 100 ns units from 1601-01-01; this is the gap between the two epochs
 * in those units.
 */
#define WL_EPOCH_DELTA 116444736000000000ULL
#define WL_FT_PER_SECOND 10000000ULL

static uint64_t wl_ft_to_unix(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;

    return t < WL_EPOCH_DELTA ? 0 : (t - WL_EPOCH_DELTA) / WL_FT_PER_SECOND;
}

static void wl_unix_to_ft(uint64_t secs, FILETIME *ft)
{
    uint64_t t = (secs * WL_FT_PER_SECOND) + WL_EPOCH_DELTA;

    ft->dwLowDateTime = (DWORD)(t & 0xFFFFFFFFU);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

#endif
