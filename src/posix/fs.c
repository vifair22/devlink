#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "w98link/fs.h"

wl_status wl_fs_stat(const char *path, uint64_t *size, uint64_t *mtime, int *is_dir)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return wl_fail(WL_ERR_IO, "%s: %s", path, strerror(errno));
    *size = (uint64_t)st.st_size;
    *mtime = (uint64_t)st.st_mtime;
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return WL_OK;
}

wl_status wl_fs_set_mtime(const char *path, uint64_t mtime)
{
    struct timeval tv[2];

    tv[0].tv_sec = (time_t)mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    if (utimes(path, tv) != 0)
        return wl_fail(WL_ERR_IO, "%s: %s", path, strerror(errno));
    return WL_OK;
}

wl_status wl_fs_replace(const char *from, const char *to)
{
    if (rename(from, to) != 0)
        return wl_fail(WL_ERR_IO, "%s: %s", to, strerror(errno));
    return WL_OK;
}

wl_status wl_fs_remove(const char *path)
{
    if (remove(path) != 0)
        return wl_fail(WL_ERR_IO, "%s: %s", path, strerror(errno));
    return WL_OK;
}

wl_status wl_fs_temp_name(const char *path, char *buf, size_t cap)
{
    if (strlen(path) + 5 >= cap)
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", path);
    (void)snprintf(buf, cap, "%s.wlt", path);
    return WL_OK;
}
