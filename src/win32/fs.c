#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "w98link/fs.h"

/* FILETIME counts 100 ns units from 1601-01-01; this is the gap to the Unix
 * epoch in those units. */
#define EPOCH_DELTA 116444736000000000ULL
#define PER_SECOND 10000000ULL

static uint64_t ft_to_unix(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;

    return t < EPOCH_DELTA ? 0 : (t - EPOCH_DELTA) / PER_SECOND;
}

static void unix_to_ft(uint64_t secs, FILETIME *ft)
{
    uint64_t t = (secs * PER_SECOND) + EPOCH_DELTA;

    ft->dwLowDateTime = (DWORD)(t & 0xFFFFFFFFU);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

wl_status wl_fs_stat(const char *path, uint64_t *size, uint64_t *mtime, int *is_dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);

    if (h == INVALID_HANDLE_VALUE)
        return wl_fail(WL_ERR_IO, "%s: not found (%lu)", path, GetLastError());
    FindClose(h);
    *size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    *mtime = ft_to_unix(&fd.ftLastWriteTime);
    *is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return WL_OK;
}

wl_status wl_fs_set_mtime(const char *path, uint64_t mtime)
{
    FILETIME ft;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);

    if (h == INVALID_HANDLE_VALUE)
        return wl_fail(WL_ERR_IO, "%s: cannot open to set its time (%lu)", path,
                       GetLastError());
    unix_to_ft(mtime, &ft);
    /* Win9x keeps FAT times in local time and converts here, so the value
     * seen by DOS matches the machine's clock, which is what sync needs. */
    if (!SetFileTime(h, NULL, NULL, &ft)) {
        wl_status s = wl_fail(WL_ERR_IO, "%s: SetFileTime failed (%lu)", path, GetLastError());

        CloseHandle(h);
        return s;
    }
    CloseHandle(h);
    return WL_OK;
}

wl_status wl_fs_replace(const char *from, const char *to)
{
    /* MoveFileEx with MOVEFILE_REPLACE_EXISTING is NT-only, and MoveFile
     * refuses an existing destination, so the old file goes first. The gap
     * is unavoidable on Win9x. */
    (void)DeleteFileA(to);
    if (!MoveFileA(from, to))
        return wl_fail(WL_ERR_IO, "cannot rename to %s (%lu)", to, GetLastError());
    return WL_OK;
}

wl_status wl_fs_remove(const char *path)
{
    if (!DeleteFileA(path))
        return wl_fail(WL_ERR_IO, "cannot delete %s (%lu)", path, GetLastError());
    return WL_OK;
}

wl_status wl_fs_temp_name(const char *path, char *buf, size_t cap)
{
    if (strlen(path) + 5 >= cap)
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", path);
    sprintf(buf, "%s.wlt", path);
    return WL_OK;
}
