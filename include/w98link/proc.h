#ifndef W98LINK_PROC_H
#define W98LINK_PROC_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/status.h"

/*
 * A child process with piped standard streams, driven without blocking: the
 * agent's main loop polls it between link pumps. Implemented per platform
 * (src/posix/proc.c, src/win32/proc.c), so the agent logic above it runs
 * unchanged on Linux in the tests and on Win32 on the target.
 *
 * The command goes to the platform shell: /bin/sh -c on POSIX, %COMSPEC% /c
 * on Win32 (command.com on 9x, cmd.exe on NT).
 */

enum { WL_PROC_STDOUT = 1, WL_PROC_STDERR = 2 };

struct wl_proc;

/* cwd may be NULL or "" for the agent's own directory. Fails with
 * WL_ERR_INVAL when cwd is not a directory and WL_ERR_IO when the process
 * cannot be started; wl_error() says which. */
wl_status wl_proc_start(const char *cmd, const char *cwd, int merge_stderr,
                        struct wl_proc **out);

/* Copies up to cap bytes of buffered output from one stream. Sets *eof once
 * the stream has closed and everything from it has been returned. */
size_t wl_proc_read(struct wl_proc *p, int stream, uint8_t *buf, size_t cap, int *eof);

/* Queues bytes for the child's stdin; returns how many were taken. */
size_t wl_proc_write(struct wl_proc *p, const uint8_t *data, size_t n);

/* Closes stdin once everything queued has been written. */
void wl_proc_close_stdin(struct wl_proc *p);

/* Nonzero once the process has exited; *code is its exit status. */
int wl_proc_exited(struct wl_proc *p, uint32_t *code);

void wl_proc_kill(struct wl_proc *p);

/* Kills the process if it is still running, then releases everything. */
void wl_proc_free(struct wl_proc *p);

#endif
