#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "w98link/fs.h"
#include "w98link/proto.h"

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

wl_status wl_fs_mkdir_p(const char *path)
{
    char buf[WL_PATH_MAX];
    size_t n = strlen(path);

    if (n >= sizeof(buf))
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", path);
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        char c = buf[i];

        if (c != '/' && c != '\\' && c != '\0')
            continue;
        /* "C:" is a drive, not a directory to create. */
        if (i == 2 && buf[1] == ':')
            continue;
        buf[i] = '\0';
        if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return wl_fail(WL_ERR_IO, "cannot create %s (%lu)", buf, GetLastError());
        if (i < n)
            buf[i] = c;
    }
    return WL_OK;
}

static wl_status walk_dir(char *full, size_t full_len, char *rel, size_t rel_len,
                          int include_hidden, wl_fs_walk_cb cb, void *ctx, int *stop)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    wl_status st = WL_OK;

    if (full_len + 3 >= WL_PATH_MAX)
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", full);
    full[full_len] = '\\';
    full[full_len + 1] = '*';
    full[full_len + 2] = '\0';
    h = FindFirstFileA(full, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        full[full_len] = '\0';
        /* An empty directory still reports "." and "..", so a failure here
         * is a real one. */
        return wl_fail(WL_ERR_IO, "cannot list %s (%lu)", full, GetLastError());
    }
    do {
        size_t n = strlen(fd.cFileName);
        size_t new_rel;

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (!include_hidden && fd.cFileName[0] == '.')
            continue;
        if (full_len + n + 2 >= WL_PATH_MAX || rel_len + n + 2 >= WL_PATH_MAX) {
            st = wl_fail(WL_ERR_NOSPACE, "path too long under %s", full);
            break;
        }
        full[full_len] = '\\';
        memcpy(full + full_len + 1, fd.cFileName, n + 1);
        if (rel_len > 0)
            rel[rel_len] = '/';
        memcpy(rel + rel_len + (rel_len > 0 ? 1 : 0), fd.cFileName, n + 1);
        new_rel = rel_len + n + (rel_len > 0 ? 1 : 0);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            st = walk_dir(full, full_len + n + 1, rel, new_rel, include_hidden, cb, ctx, stop);
        } else {
            uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;

            if (!cb(ctx, rel, size, ft_to_unix(&fd.ftLastWriteTime)))
                *stop = 1;
        }
    } while (!*stop && st == WL_OK && FindNextFileA(h, &fd));
    FindClose(h);
    full[full_len] = '\0';
    return st;
}

wl_status wl_fs_walk(const char *root, int include_hidden, wl_fs_walk_cb cb, void *ctx)
{
    char full[WL_PATH_MAX];
    char rel[WL_PATH_MAX];
    size_t n = strlen(root);
    int stop = 0;

    if (n == 0 || n >= sizeof(full) - 4)
        return wl_fail(WL_ERR_INVAL, "bad root: %s", root);
    memcpy(full, root, n + 1);
    while (n > 1 && (full[n - 1] == '\\' || full[n - 1] == '/'))
        full[--n] = '\0';
    rel[0] = '\0';
    return walk_dir(full, n, rel, 0, include_hidden, cb, ctx, &stop);
}

int wl_fs_join(char *out, size_t cap, const char *root, const char *rel)
{
    size_t rn = strlen(root);
    size_t sn = strlen(rel);

    if (rn + 1 + sn + 1 > cap)
        return 0;
    memcpy(out, root, rn);
    out[rn] = '/';
    memcpy(out + rn + 1, rel, sn);
    out[rn + 1 + sn] = '\0';
    return 1;
}
