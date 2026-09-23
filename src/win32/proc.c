#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "devlink/clock.h"
#include "devlink/proc.h"

/*
 * Win32 implementation.
 *
 * Win9x cannot wait on anonymous pipes or do overlapped I/O on them, so each
 * pipe gets a thread that does blocking I/O and meets the main loop at a
 * ring buffer under a critical section. The same code runs on NT.
 *
 * Handle inheritance: the child's pipe ends are created inheritable and ours
 * are duplicated as non-inheritable. SetHandleInformation would be simpler
 * but does not exist on Win9x.
 */

#define RING 65536U
/* How long shutdown waits for a wedged reader before abandoning it. */
#define FREE_WAIT_MS 2000U

struct ring {
    uint8_t buf[RING];
    size_t head;
    size_t len;
};

struct stream {
    HANDLE pipe; /* our end */
    HANDLE thread;
    struct ring r;
    volatile LONG done; /* the thread has finished */
};

struct wl_proc {
    HANDLE process;
    CRITICAL_SECTION lock;
    struct stream out;
    struct stream err;
    struct stream in;
    HANDLE in_wake;  /* auto-reset event: stdin data queued or close asked */
    int close_in;    /* guarded by lock */
    int has_err;
    volatile LONG stopping;
    int exited;
    uint32_t code;
};

static size_t ring_put(struct ring *r, const uint8_t *p, size_t n)
{
    size_t tail = (r->head + r->len) % RING;
    size_t first;

    if (n > RING - r->len)
        n = RING - r->len;
    first = n < RING - tail ? n : RING - tail;
    memcpy(r->buf + tail, p, first);
    memcpy(r->buf, p + first, n - first);
    r->len += n;
    return n;
}

static size_t ring_get(struct ring *r, uint8_t *p, size_t n)
{
    size_t first;

    if (n > r->len)
        n = r->len;
    first = n < RING - r->head ? n : RING - r->head;
    memcpy(p, r->buf + r->head, first);
    memcpy(p + first, r->buf, n - first);
    r->head = (r->head + n) % RING;
    r->len -= n;
    return n;
}

struct reader_arg {
    struct wl_proc *p;
    struct stream *s;
};

/* Reads the child's output until the pipe breaks, which is how Win32 reports
 * that every writer (the child and anything it spawned) has exited. When the
 * ring is full it waits, which stalls the child: backpressure. */
static DWORD WINAPI reader(LPVOID arg)
{
    struct reader_arg *a = arg;
    struct wl_proc *p = a->p;
    struct stream *s = a->s;
    uint8_t buf[4096];
    DWORD n;

    free(a);
    while (!p->stopping && ReadFile(s->pipe, buf, sizeof(buf), &n, NULL) && n > 0) {
        size_t off = 0;

        while (off < n && !p->stopping) {
            size_t put;

            EnterCriticalSection(&p->lock);
            put = ring_put(&s->r, buf + off, n - off);
            LeaveCriticalSection(&p->lock);
            off += put;
            if (off < n)
                Sleep(10);
        }
    }
    InterlockedExchange((LONG *)&s->done, 1);
    return 0;
}

static DWORD WINAPI writer(LPVOID arg)
{
    struct wl_proc *p = arg;
    uint8_t buf[4096];

    for (;;) {
        size_t n;
        int close_now;
        DWORD w;

        (void)WaitForSingleObject(p->in_wake, 100);
        if (p->stopping)
            break;
        for (;;) {
            EnterCriticalSection(&p->lock);
            n = ring_get(&p->in.r, buf, sizeof(buf));
            close_now = p->close_in && p->in.r.len == 0;
            LeaveCriticalSection(&p->lock);
            if (n > 0 && (!WriteFile(p->in.pipe, buf, (DWORD)n, &w, NULL) || w != n))
                close_now = 1; /* the child stopped reading or exited */
            if (close_now || n == 0)
                break;
        }
        if (close_now)
            break;
    }
    CloseHandle(p->in.pipe);
    p->in.pipe = NULL;
    InterlockedExchange((LONG *)&p->in.done, 1);
    return 0;
}

/* Makes a non-inheritable copy of h in place. */
static int uninherit(HANDLE *h)
{
    HANDLE self = GetCurrentProcess();
    HANDLE dup;

    if (!DuplicateHandle(self, *h, self, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    CloseHandle(*h);
    *h = dup;
    return 1;
}

static void close_handle(HANDLE *h)
{
    if (*h != NULL && *h != INVALID_HANDLE_VALUE)
        CloseHandle(*h);
    *h = NULL;
}

static int start_thread(struct stream *s, LPTHREAD_START_ROUTINE fn, void *arg)
{
    DWORD tid;

    s->thread = CreateThread(NULL, 16384, fn, arg, 0, &tid);
    return s->thread != NULL;
}

static int is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);

    return a != 0xFFFFFFFFU && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* Win9x's command.com always exits 0, whatever the program it ran returned,
 * so a command that is just a program and its arguments is started directly
 * and keeps its exit code. Anything using the shell (redirection, pipes,
 * multiple commands, environment variables) still goes through it, and on
 * Win9x those commands report 0. Internal commands such as dir fail to start
 * directly and fall back to the shell. */
static int needs_shell(const char *cmd)
{
    return strpbrk(cmd, "|&<>^()%\"") != NULL;
}

wl_status wl_proc_start(const char *cmd, const char *cwd, int merge_stderr,
                        struct wl_proc **out)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE child_in = NULL;
    HANDLE child_out = NULL;
    HANDLE child_err = NULL;
    char shell[MAX_PATH];
    const char *direct;
    char *line;
    size_t len;
    struct wl_proc *p;
    struct reader_arg *a;
    OSVERSIONINFOA vi;

    if (cwd != NULL && cwd[0] != '\0' && !is_dir(cwd))
        return wl_fail(WL_ERR_INVAL, "not a directory: %s", cwd);

    vi.dwOSVersionInfoSize = sizeof(vi);
    (void)GetVersionExA(&vi);
    if (GetEnvironmentVariableA("COMSPEC", shell, sizeof(shell)) == 0) {
        /* No COMSPEC: name the shell the platform ships. Copied by length
         * because the source is a literal and the destination is MAX_PATH;
         * strcpy here is safe but says so to no reader and no analyser. */
        const char *fallback =
            vi.dwPlatformId == VER_PLATFORM_WIN32_NT ? "cmd.exe" : "command.com";

        memcpy(shell, fallback, strlen(fallback) + 1);
    }
    len = strlen(shell) + strlen(cmd) + 8;
    line = malloc(len);
    p = calloc(1, sizeof(*p));
    if (line == NULL || p == NULL) {
        free(line);
        free(p);
        return wl_fail(WL_ERR_NOMEM, "proc: out of memory");
    }
    sprintf(line, "%s /c %s", shell, cmd);
    direct = needs_shell(cmd) ? NULL : cmd;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&child_in, &p->in.pipe, &sa, 0) ||
        !CreatePipe(&p->out.pipe, &child_out, &sa, 0) ||
        (!merge_stderr && !CreatePipe(&p->err.pipe, &child_err, &sa, 0)) ||
        !uninherit(&p->in.pipe) || !uninherit(&p->out.pipe) ||
        (!merge_stderr && !uninherit(&p->err.pipe))) {
        wl_status s = wl_fail(WL_ERR_IO, "proc: cannot create pipes (%lu)", GetLastError());

        close_handle(&child_in);
        close_handle(&child_out);
        close_handle(&child_err);
        close_handle(&p->in.pipe);
        close_handle(&p->out.pipe);
        close_handle(&p->err.pipe);
        free(line);
        free(p);
        return s;
    }

    /*
     * No console for the child, by both of the means the two platforms offer.
     *
     * With no flags at all a child inherits its parent's console, which is
     * invisible on the console agent and free. The windowed agent has no
     * console to inherit, so Windows would give each child a fresh one and a
     * black box would flash on the target every time anything ran. NT answers
     * that with CREATE_NO_WINDOW, which 9x ignores; 9x answers it with a
     * hidden show command, which NT does not need. Setting both makes the two
     * front ends behave alike, which is worth more than the one thing this
     * gives up: a Ctrl-C at the console agent's window no longer reaches the
     * children, because they are no longer in its console group. Nothing
     * depended on that. Commands are ended through wl_proc_free, deliberately
     * and with a bounded wait, and always were.
     */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = child_in;
    si.hStdOutput = child_out;
    si.hStdError = merge_stderr ? child_out : child_err;
    /* Direct first when the command is just a program, then through the
     * shell. CreateProcess writes to the command line it is given, so each
     * attempt works on its own copy. */
    {
        const char *tries[2];
        unsigned n_tries = 0;
        int started = 0;

        if (direct != NULL)
            tries[n_tries++] = direct;
        tries[n_tries++] = line;
        for (unsigned i = 0; i < n_tries && !started; i++) {
            /* CreateProcessA may write to the command line it is given, so
             * each attempt gets its own copy. */
            size_t try_len = strlen(tries[i]) + 1;
            char *copy = malloc(try_len);

            if (copy == NULL)
                continue;
            memcpy(copy, tries[i], try_len);
            started = CreateProcessA(NULL, copy, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                                     cwd != NULL && cwd[0] != '\0' ? cwd : NULL, &si,
                                     &pi) != 0;
            free(copy);
        }
        if (!started) {
            wl_status s = wl_fail(WL_ERR_IO, "cannot start '%s' (%lu)", cmd, GetLastError());

            close_handle(&child_in);
            close_handle(&child_out);
            close_handle(&child_err);
            close_handle(&p->in.pipe);
            close_handle(&p->out.pipe);
            close_handle(&p->err.pipe);
            free(line);
            free(p);
            return s;
        }
    }
    free(line);
    /* Our copies of the child's ends must go, or the pipes never break and
     * the readers never see end of file. */
    close_handle(&child_in);
    close_handle(&child_out);
    close_handle(&child_err);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->has_err = !merge_stderr;
    InitializeCriticalSection(&p->lock);
    p->in_wake = CreateEventA(NULL, FALSE, FALSE, NULL);

    a = malloc(sizeof(*a));
    if (a != NULL) {
        a->p = p;
        a->s = &p->out;
        if (!start_thread(&p->out, reader, a))
            free(a);
    }
    if (p->has_err) {
        a = malloc(sizeof(*a));
        if (a != NULL) {
            a->p = p;
            a->s = &p->err;
            if (!start_thread(&p->err, reader, a))
                free(a);
        }
    } else {
        p->err.done = 1;
    }
    if (p->in_wake == NULL || !start_thread(&p->in, writer, p)) {
        close_handle(&p->in.pipe);
        p->in.done = 1;
    }
    *out = p;
    return WL_OK;
}

size_t wl_proc_read(struct wl_proc *p, int stream, uint8_t *buf, size_t cap, int *eof)
{
    struct stream *s = stream == WL_PROC_STDERR ? &p->err : &p->out;
    size_t n;
    int done = s->done; /* read before draining, so no data is missed */

    EnterCriticalSection(&p->lock);
    n = ring_get(&s->r, buf, cap);
    *eof = done && s->r.len == 0;
    LeaveCriticalSection(&p->lock);
    return n;
}

size_t wl_proc_write(struct wl_proc *p, const uint8_t *data, size_t n)
{
    size_t put;

    if (p->in.done)
        return 0;
    EnterCriticalSection(&p->lock);
    put = p->close_in ? 0 : ring_put(&p->in.r, data, n);
    LeaveCriticalSection(&p->lock);
    if (put > 0)
        SetEvent(p->in_wake);
    return put;
}

void wl_proc_close_stdin(struct wl_proc *p)
{
    EnterCriticalSection(&p->lock);
    p->close_in = 1;
    LeaveCriticalSection(&p->lock);
    if (p->in_wake != NULL)
        SetEvent(p->in_wake);
}

int wl_proc_exited(struct wl_proc *p, uint32_t *code)
{
    DWORD c;

    if (!p->exited && WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(p->process, &c)) {
        p->exited = 1;
        p->code = (uint32_t)c;
    }
    if (p->exited)
        *code = p->code;
    return p->exited;
}

void wl_proc_kill(struct wl_proc *p)
{
    if (!p->exited)
        (void)TerminateProcess(p->process, 1);
}

int wl_proc_reap(struct wl_proc *p)
{
    uint32_t code;

    if (p == NULL)
        return 1;
    if (!p->stopping) {
        /* First call: ask everything to stop, then leave. */
        if (!wl_proc_exited(p, &code))
            wl_proc_kill(p);
        InterlockedExchange((LONG *)&p->stopping, 1);
        if (p->in_wake != NULL)
            SetEvent(p->in_wake);
        /* Closing our ends breaks the pipes, which is what wakes a reader
         * blocked in ReadFile. */
        close_handle(&p->out.pipe);
        close_handle(&p->err.pipe);
        return 0;
    }
    if (!wl_proc_exited(p, &code))
        return 0;
    /* Threads are checked, never waited for: a reader still blocked because
     * a grandchild holds the pipe open must not stall the agent. It is
     * checked again next time round. TerminateThread is not used at all;
     * killing a thread holding the C runtime lock breaks the whole agent. */
    if ((p->out.thread != NULL && WaitForSingleObject(p->out.thread, 0) != WAIT_OBJECT_0) ||
        (p->err.thread != NULL && WaitForSingleObject(p->err.thread, 0) != WAIT_OBJECT_0) ||
        (p->in.thread != NULL && WaitForSingleObject(p->in.thread, 0) != WAIT_OBJECT_0))
        return 0;
    close_handle(&p->out.thread);
    close_handle(&p->err.thread);
    close_handle(&p->in.thread);
    close_handle(&p->in.pipe);
    close_handle(&p->in_wake);
    CloseHandle(p->process);
    DeleteCriticalSection(&p->lock);
    free(p);
    return 1;
}

void wl_proc_free(struct wl_proc *p)
{
    uint32_t start = wl_clock_ms();

    if (p == NULL)
        return;
    /*
     * Shutdown goes through the same reaper the serving loop uses, for a
     * bounded time, and gives up rather than forcing anything: a reader
     * thread blocked in ReadFile holds a pointer to p and may hold the C
     * runtime's lock, so TerminateThread on it risks a broken heap in the
     * rest of the process, and freeing p under it is a use-after-free. This
     * used to terminate the thread after two seconds and free p anyway.
     *
     * The cost of giving up is one abandoned struct, about 200 KB of rings
     * and handles, which is paid at most once per wedged child; the thread
     * exits on its own whenever the grandchild holding the pipe finally goes.
     */
    while (!wl_proc_reap(p)) {
        if (wl_clock_since(start) >= FREE_WAIT_MS)
            return;
        Sleep(10);
    }
}
