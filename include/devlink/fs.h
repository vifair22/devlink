#ifndef DEVLINK_FS_H
#define DEVLINK_FS_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/status.h"

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

/* Joins a root and a relative path with '/', refusing to truncate.
 * Returns 0 if the result would not fit. */
int wl_fs_join(char *out, size_t cap, const char *root, const char *rel);

/* Creates a directory and any missing parents. Succeeds if it exists. */
wl_status wl_fs_mkdir_p(const char *path);

/*
 * Walks a directory tree, calling back for each regular file with its path
 * relative to root, using '/' separators. Directories whose name starts with
 * '.' are skipped unless include_hidden is set, which is what keeps .git out
 * of a sync by default.
 *
 * The callback returns 0 to stop the walk.
 */
typedef int (*wl_fs_walk_cb)(void *ctx, const char *rel, uint64_t size, uint64_t mtime);

wl_status wl_fs_walk(const char *root, int include_hidden, wl_fs_walk_cb cb, void *ctx);

/*
 * Nonzero if a command may name a drive letter. On Windows 9x: the drive
 * exists, and if it is removable there is media in it. Everywhere else,
 * always: on NT an empty drive is an ordinary error, and a letter that means
 * nothing on the platform (every letter, on POSIX) needs no check, so
 * callers need no platform test.
 *
 * This exists to keep a command naming an empty removable drive away from
 * command.com. The Win32 call fails cleanly under the error mode the agent
 * sets; DOS instead stops on a critical-error prompt that freezes the whole
 * agent until somebody presses a key at the target
 * (docs/bug-critical-error-hang.md). NT's cmd.exe is a Win32 process and
 * honours that error mode, so there is nothing to keep away.
 */
int wl_fs_drive_ready(char letter);

/*
 * Lists one directory, without recursing, calling back for each entry other
 * than "." and "..". Directories are reported too, with size 0. Names are as
 * stored: on Win32 that is the long name, not the 8.3 alias that a DOS `dir`
 * prints.
 *
 * The callback returns 0 to stop the listing.
 */
typedef int (*wl_fs_list_cb)(void *ctx, const char *name, int is_dir, uint64_t size,
                             uint64_t mtime);

wl_status wl_fs_list(const char *path, wl_fs_list_cb cb, void *ctx);

#endif
