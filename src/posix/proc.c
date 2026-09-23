#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "devlink/clock.h"
#include "devlink/proc.h"

/* How long shutdown waits for the child to go before giving up on it. */
#define FREE_WAIT_MS 2000U

/* POSIX implementation: non-blocking pipe ends polled from the caller's
 * loop. The kernel's pipe buffer is the only queue on each stream, which is
 * the backpressure we want: a child writing faster than the link drains
 * simply blocks in write(). */
struct wl_proc {
    pid_t pid;
    int in_fd;  /* our end of the child's stdin, -1 once closed */
    int out_fd; /* -1 once EOF is seen */
    int err_fd; /* -1 once EOF is seen, or from the start when merged */
    int close_in;
    int exited;
    uint32_t code;
};

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);

    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

wl_status wl_proc_start(const char *cmd, const char *cwd, int merge_stderr,
                        struct wl_proc **out)
{
    int in[2] = {-1, -1};
    int o[2] = {-1, -1};
    int e[2] = {-1, -1};
    struct stat st;
    struct wl_proc *p;
    pid_t pid;

    /* Checked before forking so a bad directory is an OPEN_FAIL the client
     * can report, not a shell error mixed into the output. */
    if (cwd != NULL && cwd[0] != '\0' && (stat(cwd, &st) != 0 || !S_ISDIR(st.st_mode)))
        return wl_fail(WL_ERR_INVAL, "not a directory: %s", cwd);

    /* Writing to a child that has exited must be an error return, not a
     * signal that kills the agent. */
    (void)signal(SIGPIPE, SIG_IGN);

    if (pipe(in) != 0 || pipe(o) != 0 || (!merge_stderr && pipe(e) != 0)) {
        wl_status s = wl_fail(WL_ERR_IO, "pipe: %s", strerror(errno));
        close_fd(&in[0]);
        close_fd(&in[1]);
        close_fd(&o[0]);
        close_fd(&o[1]);
        return s;
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        close_fd(&in[0]);
        close_fd(&in[1]);
        close_fd(&o[0]);
        close_fd(&o[1]);
        close_fd(&e[0]);
        close_fd(&e[1]);
        return wl_fail(WL_ERR_NOMEM, "proc: out of memory");
    }

    pid = fork();
    if (pid < 0) {
        wl_status s = wl_fail(WL_ERR_IO, "fork: %s", strerror(errno));
        close_fd(&in[0]);
        close_fd(&in[1]);
        close_fd(&o[0]);
        close_fd(&o[1]);
        close_fd(&e[0]);
        close_fd(&e[1]);
        free(p);
        return s;
    }
    if (pid == 0) {
        (void)dup2(in[0], 0);
        (void)dup2(o[1], 1);
        (void)dup2(merge_stderr ? o[1] : e[1], 2);
        (void)close(in[0]);
        (void)close(in[1]);
        (void)close(o[0]);
        (void)close(o[1]);
        if (!merge_stderr) {
            (void)close(e[0]);
            (void)close(e[1]);
        }
        if (cwd != NULL && cwd[0] != '\0' && chdir(cwd) != 0)
            _exit(127);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close_fd(&in[0]);
    close_fd(&o[1]);
    close_fd(&e[1]);
    p->pid = pid;
    p->in_fd = in[1];
    p->out_fd = o[0];
    p->err_fd = e[0];
    (void)set_nonblock(p->in_fd);
    (void)set_nonblock(p->out_fd);
    if (p->err_fd >= 0)
        (void)set_nonblock(p->err_fd);
    (void)fcntl(p->in_fd, F_SETFD, FD_CLOEXEC);
    (void)fcntl(p->out_fd, F_SETFD, FD_CLOEXEC);
    if (p->err_fd >= 0)
        (void)fcntl(p->err_fd, F_SETFD, FD_CLOEXEC);
    *out = p;
    return WL_OK;
}

size_t wl_proc_read(struct wl_proc *p, int stream, uint8_t *buf, size_t cap, int *eof)
{
    int *fd = stream == WL_PROC_STDERR ? &p->err_fd : &p->out_fd;
    ssize_t n;

    *eof = 0;
    if (*fd < 0) {
        *eof = 1;
        return 0;
    }
    do {
        n = read(*fd, buf, cap);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        return (size_t)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    close_fd(fd);
    *eof = 1;
    return 0;
}

size_t wl_proc_write(struct wl_proc *p, const uint8_t *data, size_t n)
{
    ssize_t w;

    if (p->in_fd < 0 || p->close_in)
        return 0;
    do {
        w = write(p->in_fd, data, n);
    } while (w < 0 && errno == EINTR);
    if (w > 0)
        return (size_t)w;
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        close_fd(&p->in_fd); /* the child is gone or closed its stdin */
    return 0;
}

void wl_proc_close_stdin(struct wl_proc *p)
{
    /* Writes are never queued here (the pipe is the queue), so there is
     * nothing to drain first. */
    p->close_in = 1;
    close_fd(&p->in_fd);
}

int wl_proc_exited(struct wl_proc *p, uint32_t *code)
{
    int status;

    if (!p->exited) {
        pid_t r = waitpid(p->pid, &status, WNOHANG);

        if (r == p->pid) {
            p->exited = 1;
            if (WIFEXITED(status))
                p->code = (uint32_t)WEXITSTATUS(status);
            else
                p->code = 128U + (uint32_t)WTERMSIG(status);
        }
    }
    if (p->exited)
        *code = p->code;
    return p->exited;
}

void wl_proc_kill(struct wl_proc *p)
{
    if (!p->exited)
        (void)kill(p->pid, SIGKILL);
}

int wl_proc_reap(struct wl_proc *p)
{
    uint32_t code;

    if (p == NULL)
        return 1;
    if (!wl_proc_exited(p, &code)) {
        wl_proc_kill(p);
        return 0; /* wl_proc_exited reaps it on a later call */
    }
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    close_fd(&p->err_fd);
    free(p);
    return 1;
}

void wl_proc_free(struct wl_proc *p)
{
    uint32_t start = wl_clock_ms();

    if (p == NULL)
        return;
    /* The same shape as the Win32 side: the reaper, bounded, rather than a
     * blocking wait. Nothing here can wedge the way a Win32 reader thread
     * can, since there are no threads, but one shutdown path in two shapes
     * is one shape too many. */
    while (!wl_proc_reap(p)) {
        if (wl_clock_since(start) >= FREE_WAIT_MS)
            return;
        (void)poll(NULL, 0, 10);
    }
}
