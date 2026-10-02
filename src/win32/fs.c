#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "devlink/fs.h"
#include "devlink/proto.h"
#include "wintime.h"


wl_status wl_fs_stat(const char *path, uint64_t *size, uint64_t *mtime, int *is_dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);

    if (h == INVALID_HANDLE_VALUE)
        return wl_fail(WL_ERR_IO, "%s: not found (%lu)", path, GetLastError());
    FindClose(h);
    *size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    *mtime = wl_ft_to_unix(&fd.ftLastWriteTime);
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
    wl_unix_to_ft(mtime, &ft);
    /* The value handed over is UTC, which is what NTFS stores. On a FAT volume
     * Windows converts to local time on the way down and back on the way up,
     * so what sync compares is UTC either way; the two-second tolerance in
     * proto.h is for FAT's timestamp resolution, not for that conversion. */
    if (!SetFileTime(h, NULL, NULL, &ft)) {
        wl_status s = wl_fail(WL_ERR_IO, "%s: SetFileTime failed (%lu)", path, GetLastError());

        CloseHandle(h);
        return s;
    }
    CloseHandle(h);
    return WL_OK;
}

typedef BOOL(WINAPI *move_file_ex_fn)(LPCSTR, LPCSTR, DWORD);

/*
 * MoveFileEx with MOVEFILE_REPLACE_EXISTING replaces a file in one step. It
 * is resolved at run time because Win9x does not implement it, and there the
 * old file has to be deleted first, leaving a moment with nothing at the
 * destination: a put or sync interrupted in that gap takes the file the target
 * already had with it. On NT that gap is closed.
 *
 * Where MoveFileEx exists its failure is reported rather than retried with the
 * pair below. Falling back would mean answering "the destination is locked" by
 * deleting the destination.
 */
wl_status wl_fs_replace(const char *from, const char *to)
{
    static move_file_ex_fn move_ex;
    static int resolved;

    if (!resolved) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");

        if (k32 != NULL)
            move_ex = (move_file_ex_fn)GetProcAddress(k32, "MoveFileExA");
        resolved = 1;
    }
    if (move_ex != NULL) {
        if (move_ex(from, to, MOVEFILE_REPLACE_EXISTING))
            return WL_OK;
        /* Windows 98's kernel32 exports MoveFileExA as a stub that fails
         * with ERROR_CALL_NOT_IMPLEMENTED, so the export alone does not say
         * the call works: that failure takes the 9x path below. */
        if (GetLastError() != ERROR_CALL_NOT_IMPLEMENTED)
            return wl_fail(WL_ERR_IO, "cannot replace %s (%lu)", to, GetLastError());
    }
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

            if (!cb(ctx, rel, size, wl_ft_to_unix(&fd.ftLastWriteTime)))
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

int wl_fs_drive_ready(char letter)
{
    char root[4];
    UINT type;
    DWORD spc;
    DWORD bps;
    DWORD freec;
    DWORD totalc;

    /*
     * Only Windows 9x needs this. On NT the shell is cmd.exe, a Win32
     * process that inherits the agent's error mode, so an empty drive is an
     * ordinary error ("The device is not ready.", exit 1) and nothing hangs:
     * checked on Windows 2000 with dir and vol on an empty CD drive. There
     * the refusal only got in the way, of commands naming a drive that will
     * exist by the time it is used (a virtual disk being mounted) and of
     * colons that are not drives at all (shutdown /T:0).
     */
    if ((GetVersion() & 0x80000000UL) == 0)
        return 1;
    if (letter >= 'a' && letter <= 'z')
        letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z')
        return 1;
    root[0] = letter;
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    type = GetDriveTypeA(root);
    if (type == DRIVE_NO_ROOT_DIR)
        return 0;
    if (type != DRIVE_REMOVABLE && type != DRIVE_CDROM)
        return 1; /* fixed, network or unknown: not worth touching the media */
    /*
     * The cheapest call that reaches the media. It fails with ERROR_NOT_READY
     * on an empty drive rather than prompting, because the agent sets
     * SEM_FAILCRITICALERRORS at startup; a 32-bit caller honours that, unlike
     * command.com.
     */
    return GetDiskFreeSpaceA(root, &spc, &bps, &freec, &totalc) ? 1 : 0;
}

wl_status wl_fs_list(const char *path, wl_fs_list_cb cb, void *ctx)
{
    char pattern[WL_PATH_MAX + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t n = strlen(path);
    int stop = 0;

    if (n == 0 || n + 3 >= sizeof(pattern))
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", path);
    memcpy(pattern, path, n);
    while (n > 1 && (pattern[n - 1] == '\\' || pattern[n - 1] == '/'))
        n--;
    /* The separator goes back on unconditionally: a trailing one was just
     * stripped, and a bare drive root ("C:") needs one that was never there,
     * since "C:*" means the current directory of that drive. */
    pattern[n++] = '\\';
    pattern[n++] = '*';
    pattern[n] = '\0';
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return wl_fail(WL_ERR_IO, "cannot list %s (%lu)", path, GetLastError());
    do {
        uint64_t size;

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            size = 0;
        if (!cb(ctx, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                size, wl_ft_to_unix(&fd.ftLastWriteTime)))
            stop = 1;
    } while (!stop && FindNextFileA(h, &fd));
    FindClose(h);
    return WL_OK;
}
