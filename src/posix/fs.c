#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "w98link/fs.h"
#include "w98link/proto.h"

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

wl_status wl_fs_mkdir_p(const char *path)
{
    char buf[WL_PATH_MAX];
    size_t n = strlen(path);

    if (n >= sizeof(buf))
        return wl_fail(WL_ERR_NOSPACE, "path too long: %s", path);
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST)
            return wl_fail(WL_ERR_IO, "%s: %s", buf, strerror(errno));
        if (i < n)
            buf[i] = '/';
    }
    return WL_OK;
}

/* Recursive helper; rel_len is how much of rel is the prefix built so far. */
static wl_status walk_dir(char *full, size_t full_len, char *rel, size_t rel_len,
                          int include_hidden, wl_fs_walk_cb cb, void *ctx, int *stop)
{
    DIR *d = opendir(full);
    struct dirent *e;
    wl_status st = WL_OK;

    if (d == NULL)
        return wl_fail(WL_ERR_IO, "%s: %s", full, strerror(errno));
    while (!*stop && (e = readdir(d)) != NULL) {
        struct stat sb;
        size_t n = strlen(e->d_name);

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!include_hidden && e->d_name[0] == '.')
            continue;
        if (full_len + n + 2 >= WL_PATH_MAX || rel_len + n + 2 >= WL_PATH_MAX) {
            st = wl_fail(WL_ERR_NOSPACE, "path too long under %s", full);
            break;
        }
        full[full_len] = '/';
        memcpy(full + full_len + 1, e->d_name, n + 1);
        if (rel_len > 0)
            rel[rel_len] = '/';
        memcpy(rel + rel_len + (rel_len > 0 ? 1 : 0), e->d_name, n + 1);
        {
            size_t new_rel = rel_len + n + (rel_len > 0 ? 1 : 0);

            if (stat(full, &sb) != 0)
                continue; /* vanished between readdir and stat */
            if (S_ISDIR(sb.st_mode))
                st = walk_dir(full, full_len + n + 1, rel, new_rel, include_hidden, cb, ctx,
                              stop);
            else if (S_ISREG(sb.st_mode) &&
                     !cb(ctx, rel, (uint64_t)sb.st_size, (uint64_t)sb.st_mtime))
                *stop = 1;
        }
        if (st != WL_OK)
            break;
    }
    (void)closedir(d);
    return st;
}

wl_status wl_fs_walk(const char *root, int include_hidden, wl_fs_walk_cb cb, void *ctx)
{
    char full[WL_PATH_MAX];
    char rel[WL_PATH_MAX];
    size_t n = strlen(root);
    int stop = 0;

    if (n == 0 || n >= sizeof(full) - 2)
        return wl_fail(WL_ERR_INVAL, "bad root: %s", root);
    memcpy(full, root, n + 1);
    while (n > 1 && full[n - 1] == '/')
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
