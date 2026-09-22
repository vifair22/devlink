#ifndef W98LINK_FS_H
#define W98LINK_FS_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/status.h"

/*
 * The few file operations that are not portable through stdio: metadata,
 * timestamps and replacing a file. Reading and writing uses stdio directly.
 *
 * Times are seconds since 1970-01-01 UTC (docs/protocol.md, "Paths and
 * times"); the Win32 side converts to and from FILETIME.
 */

wl_status wl_fs_stat(const char *path, uint64_t *size, uint64_t *mtime, int *is_dir);

wl_status wl_fs_set_mtime(const char *path, uint64_t mtime);

/* Replaces the destination if it exists. On Win9x this is remove-then-rename
 * rather than one atomic step: nothing there offers an atomic replace. */
wl_status wl_fs_replace(const char *from, const char *to);

wl_status wl_fs_remove(const char *path);

/* Builds a temporary name beside path, so the rename afterwards stays on
 * the same file system. */
wl_status wl_fs_temp_name(const char *path, char *buf, size_t cap);

#endif
