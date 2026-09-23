#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink/agent.h"
#include "devlink/clock.h"
#include "devlink/crc32.h"
#include "devlink/fs.h"
#include "devlink/proc.h"
#include "devlink/proto.h"
#include "devlink/sha256.h"
#include "devlink/wire.h"

#define LOOP_MS 20U
#define OUT_CHUNK 4096U
#define STDIN_QUEUE 65536U

/* One running command. Output read from the process but not yet accepted by
 * the mux waits in pend_*; nothing more is read from that stream until it is
 * gone, so the process stalls instead of the agent buffering without bound. */
struct exec {
    int active;
    uint16_t ch;
    struct wl_proc *proc;
    uint8_t pend[2][OUT_CHUNK];
    size_t pend_len[2];
    int eof[2];
    int exited;
    int cancelled; /* the client gave up on it; it has been killed */
    uint32_t code;
    int exit_sent;
    int close_sent;
    uint64_t out_total; /* output bytes the client has taken, for a display */
    uint64_t reported;  /* out_total as last reported, so an idle command is quiet */
    uint8_t *stdin_q; /* client bytes the process has not taken yet */
    size_t stdin_len;
    int stdin_eof;
    /* An agent command such as @dir produces its output here instead of from
     * a process; proc is then NULL and the status is always 0. Everything
     * below this point in the exec path is shared with a real command. */
    char *out_buf;
    size_t out_len;
    size_t out_at;
};

/* One file transfer in progress. PUT writes to a temporary file beside the
 * destination and renames on success, so a failed transfer never leaves a
 * half-written file in place. */
struct xfer {
    int active;
    uint16_t ch;
    uint8_t kind;
    FILE *f;
    char path[WL_PATH_MAX];
    char tmp[WL_PATH_MAX + 8];
    uint64_t size;
    uint64_t mtime;
    uint64_t done;
    uint64_t reported; /* done as last reported, so an idle transfer is quiet */
    uint32_t crc;
    int failed;
    int finished; /* DONE or FAIL queued */
    uint8_t buf[OUT_CHUNK];
    size_t buf_len;
    int at_eof;
};

/* A process being released, one node per abandoned command. */
struct reaping {
    struct wl_proc *proc;
    struct reaping *next;
};

struct pending_hash {
    char rel[WL_PATH_MAX];
    uint8_t want[WL_HASH_BYTES];
    uint64_t mtime; /* the client's, to adopt if the contents match */
};

/* A sync in progress. Manifest paths are only remembered when the client
 * asked for deletions, which is the one case that needs the whole set. */
struct sync {
    int active;
    uint16_t ch;
    char root[WL_PATH_MAX];
    uint8_t flags;
    int manifest_done;
    uint32_t same;
    uint32_t written;
    uint32_t failed;
    uint32_t deleted;
    /* NEED paths are queued here until the mux has room for them. */
    char (*need)[WL_PATH_MAX];
    size_t n_need;
    size_t cap_need;
    size_t sent_need;
    /* The manifest's paths, for the deletion pass. */
    char *seen;
    size_t seen_len;
    size_t seen_cap;
    int seen_overflow;
    /* Files whose size matches but whose time does not: their contents
     * decide, and hashing them is far too slow to do while holding up the
     * link, so they are queued here and hashed a chunk at a time between
     * pumps (docs/bug-sync-hash-starves-link.md). */
    struct pending_hash *pend;
    size_t n_pend;
    size_t cap_pend;
    size_t pend_at;
    FILE *hash_f;          /* the file being hashed, if any */
    struct wl_sha256 hash; /* its state so far */

    /* The file being received. */
    FILE *f;
    char path[WL_PATH_MAX];
    char tmp[WL_PATH_MAX + 8];
    uint64_t left;
    uint64_t mtime;
    uint32_t crc;
    int file_bad;
};

struct agent {
    const struct wl_agent_cfg *cfg;
    struct wl_transport *t; /* kept for @caps, which asks the port about itself */
    struct wl_peer *peer;
    uint32_t reported_gen; /* session whose stream error was already logged */
    struct exec execs[WL_AGENT_MAX_EXEC];
    struct xfer xfers[WL_AGENT_MAX_XFER];
    struct sync sync;
    /* Processes being torn down. Abandoning a command must never hold up
     * the link (docs/bug-critical-error-hang.md, defect 2), so they are
     * killed here and collected a little at a time by the serving loop. A
     * list rather than an array because a wedged child can outlive many
     * commands, and running out of slots used to mean falling back to the
     * blocking teardown, which is the thing being avoided. */
    struct reaping *reaping;
    int stopping;  /* @stop received: finish the exchange, then return */
    int was_ready; /* a client was attached as of the last loop iteration */
};

/* One line to the console: a fixed prefix and a variable part. */
static void log_line(struct agent *a, const char *prefix, const char *detail)
{
    char buf[512];

    if (a->cfg->log == NULL)
        return;
    (void)snprintf(buf, sizeof(buf), "%s%s", prefix, detail);
    a->cfg->log(a->cfg->log_ctx, buf);
}

/*
 * The same moment, in the structured form a display needs. This sits beside
 * log_line because the two always report together: a moment that reaches only
 * one of them is a bug in whichever was forgotten.
 */
static void emit(struct agent *a, const struct wl_agent_event *e)
{
    if (a->cfg->event != NULL)
        a->cfg->event(a->cfg->event_ctx, e);
}

/* Session transitions, and the end of a piece of work. */
static void emit_at(struct agent *a, uint8_t type, uint16_t ch, uint32_t code)
{
    struct wl_agent_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.ch = ch;
    e.code = code;
    emit(a, &e);
}

static void emit_work(struct agent *a, uint16_t ch, uint8_t kind, const char *what,
                      uint64_t total)
{
    struct wl_agent_event e;

    memset(&e, 0, sizeof(e));
    e.type = WL_EV_CHANNEL;
    e.ch = ch;
    e.kind = kind;
    e.text = what;
    e.total = total;
    emit(a, &e);
}

static void emit_progress(struct agent *a, uint16_t ch, uint64_t done)
{
    struct wl_agent_event e;

    memset(&e, 0, sizeof(e));
    e.type = WL_EV_PROGRESS;
    e.ch = ch;
    e.done = done;
    emit(a, &e);
}

struct sync;
static void sync_end(struct agent *a, struct sync *y);

static struct xfer *find_xfer(struct agent *a, uint16_t ch)
{
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active && a->xfers[i].ch == ch)
            return &a->xfers[i];
    }
    return NULL;
}

static void end_xfer(struct agent *a, struct xfer *x, int keep_temp)
{
    /* Only work that was announced is reported as finished; a transfer
     * rejected before it went active never appeared anywhere. */
    if (x->active)
        emit_at(a, WL_EV_FINISHED, x->ch, 0);
    if (x->f != NULL)
        (void)fclose(x->f);
    if (!keep_temp && x->kind == WL_KIND_PUT && x->tmp[0] != '\0')
        (void)wl_fs_remove(x->tmp);
    memset(x, 0, sizeof(*x));
}

static struct exec *find_exec(struct agent *a, uint16_t ch)
{
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active && a->execs[i].ch == ch)
            return &a->execs[i];
    }
    return NULL;
}

/* Ends a command without blocking: the process goes to the reaper. */
static void end_exec(struct agent *a, struct exec *e)
{
    if (e->active)
        emit_at(a, WL_EV_FINISHED, e->ch, e->code);
    if (e->proc != NULL) {
        struct reaping *r = malloc(sizeof(*r));

        if (r != NULL) {
            r->proc = e->proc;
            r->next = a->reaping;
            a->reaping = r;
        } else {
            /* Out of memory for one pointer, so the bounded teardown it is;
             * it no longer forces anything down, it only takes a moment. */
            wl_proc_free(e->proc);
        }
    }
    free(e->stdin_q);
    free(e->out_buf);
    memset(e, 0, sizeof(*e));
}

/* One pass over the list, releasing whatever has finally let go. */
static void reap(struct agent *a)
{
    struct reaping **link = &a->reaping;

    while (*link != NULL) {
        struct reaping *r = *link;

        if (wl_proc_reap(r->proc)) {
            *link = r->next;
            free(r);
        } else {
            link = &r->next;
        }
    }
}

/* ---------------------------------------------------------------------------
 * peer handlers
 * ------------------------------------------------------------------------- */

static void on_session(void *ctx)
{
    struct agent *a = ctx;

    /* The previous client is gone; nobody will read its commands' output,
     * and its half-written uploads are worthless. */
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active)
            end_exec(a, &a->execs[i]);
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active)
            end_xfer(a, &a->xfers[i], 0);
    }
    if (a->sync.active)
        sync_end(a, &a->sync);
    log_line(a, "session: new client", "");
    emit_at(a, WL_EV_SESSION, 0, 0);
}

static void on_ready(void *ctx, const struct wl_hello *h)
{
    struct agent *a = ctx;

    log_line(a, "session: client ", h->build);
    {
        struct wl_agent_event e;

        memset(&e, 0, sizeof(e));
        e.type = WL_EV_READY;
        e.text = h->build;
        emit(a, &e);
    }
}

/*
 * Refuses a command that names a drive with nothing in it, and says which.
 *
 * A Win9x shell builtin that touches empty removable media stops on a DOS
 * critical-error prompt, and while that prompt is up Windows freezes the
 * agent itself: the client is not answered, a cancel is not read, and only a
 * keypress at the target ends it (docs/bug-critical-error-hang.md). There is
 * no recovering from it from this side, so the command must not be started in
 * the first place. The check is cheap and 32-bit, so it fails cleanly where
 * DOS would have prompted.
 *
 * Letters are taken from anywhere in the command line, since a drive can be
 * named in an argument as readily as in the command itself. The false
 * positive this allows, a letter that happens to be followed by a colon
 * without meaning a drive, only bites when that drive is also empty, and then
 * the refusal is a good deal more useful than the freeze.
 */
static int drives_are_ready(struct agent *a, const char *cmd, char *bad)
{
    int (*ready)(char) = a->cfg->drive_ready != NULL ? a->cfg->drive_ready : wl_fs_drive_ready;
    uint32_t seen = 0;

    for (size_t i = 0; cmd[i] != '\0'; i++) {
        char c = cmd[i];
        unsigned bit;

        if (cmd[i + 1] != ':')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z')
            continue;
        /* A letter run together with what precedes it is part of a word, not
         * a drive: the g of "prog:" is not drive G. */
        if (i > 0 && ((cmd[i - 1] >= 'A' && cmd[i - 1] <= 'Z') ||
                      (cmd[i - 1] >= 'a' && cmd[i - 1] <= 'z') ||
                      (cmd[i - 1] >= '0' && cmd[i - 1] <= '9')))
            continue;
        bit = 1U << (unsigned)(c - 'A');
        if ((seen & bit) != 0)
            continue;
        seen |= bit;
        if (!ready(c)) {
            *bad = c;
            return 0;
        }
    }
    return 1;
}

/* Watcom's runtime is not asked to format 64-bit integers: this is smaller
 * than pulling in that part of printf, and there is no doubt about whether
 * the target's C library handles the conversion. */
static size_t u64_dec(uint64_t v, char *out)
{
    char tmp[20];
    size_t n = 0;
    size_t i;

    do {
        tmp[n++] = (char)('0' + (unsigned)(v % 10U));
        v /= 10U;
    } while (v != 0);
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Appends a literal to a buffer kept by length rather than by a terminator,
 * the same convention as u64_dec above. */
static size_t put_lit(char *out, const char *lit)
{
    size_t i;

    for (i = 0; lit[i] != '\0'; i++)
        out[i] = lit[i];
    return i;
}

/* One @dir listing, grown a line at a time. */
struct listing {
    char *buf;
    size_t len;
    size_t cap;
    int oom;
};

/*
 * One entry per line: type, size, mtime, name, separated by single spaces,
 * with the name last because it may contain them. Sizes are exact and names
 * are as stored, which is the point of doing this here rather than in
 * command.com: DOS reports 8.3 aliases and a locale-dependent date.
 */
static int listing_line(void *ctx, const char *name, int is_dir, uint64_t size,
                        uint64_t mtime)
{
    struct listing *l = ctx;
    char line[WL_PATH_MAX + 48];
    size_t n = 0;
    size_t name_n = strlen(name);

    line[n++] = is_dir ? 'd' : 'f';
    line[n++] = ' ';
    n += u64_dec(size, line + n);
    line[n++] = ' ';
    n += u64_dec(mtime, line + n);
    line[n++] = ' ';
    if (name_n > WL_PATH_MAX)
        name_n = WL_PATH_MAX;
    /* Copied a character at a time rather than with memcpy because the result
     * is deliberately not a string: the line is kept by length and ends with
     * the newline below. */
    for (size_t i = 0; i < name_n; i++)
        line[n + i] = name[i];
    n += name_n;
    line[n++] = '\n';

    if (l->len + n > l->cap) {
        size_t want = l->cap == 0 ? 4096U : l->cap * 2U;
        char *bigger;

        while (want < l->len + n)
            want *= 2U;
        bigger = realloc(l->buf, want);
        if (bigger == NULL) {
            l->oom = 1;
            return 0;
        }
        l->buf = bigger;
        l->cap = want;
    }
    memcpy(l->buf + l->len, line, n);
    l->len += n;
    return 1;
}

/* Returns nonzero if the slot is now serving the listing. */
static int agent_dir(struct agent *a, uint16_t ch, const char *path, struct exec *e)
{
    struct listing l;
    char bad = '\0';
    wl_status st;

    if (path[0] == '\0') {
        (void)wl_peer_reject(a->peer, ch, WL_E_PATH, "@dir needs a path");
        return 0;
    }
    /* The same guard as a shell command gets: a listing of an empty drive
     * must fail here rather than anywhere near DOS. */
    if (!drives_are_ready(a, path, &bad)) {
        char msg[64];

        sprintf(msg, "drive %c: is not ready", bad);
        (void)wl_peer_reject(a->peer, ch, WL_E_NOT_FOUND, msg);
        return 0;
    }
    memset(&l, 0, sizeof(l));
    st = wl_fs_list(path, listing_line, &l);
    if (st != WL_OK || l.oom) {
        free(l.buf);
        (void)wl_peer_reject(a->peer, ch, l.oom ? WL_E_PROCESS : WL_E_NOT_FOUND,
                             l.oom ? "agent out of memory" : wl_error());
        return 0;
    }
    if (wl_peer_accept(a->peer, ch) != WL_OK) {
        free(l.buf);
        return 0;
    }
    /* Handed to the exec path, which already knows how to push output out a
     * chunk at a time and finish with EXIT and CLOSE. */
    e->active = 1;
    e->ch = ch;
    e->out_buf = l.buf;
    e->out_len = l.len;
    log_line(a, "dir: ", path);
    return 1;
}

/*
 * What the port underneath says about itself, so a decision to drive it faster
 * rests on the driver's answer rather than a guess about the hardware. Served
 * through the same buffer path as @dir.
 *
 * Returns nonzero if the slot is now serving the answer.
 */
static int agent_caps(struct agent *a, uint16_t ch, struct exec *e)
{
    char buf[1024];
    wl_status st = wl_transport_caps(a->t, buf, sizeof(buf));
    size_t n;

    if (st != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, wl_error());
        return 0;
    }
    n = strlen(buf);
    if (wl_peer_accept(a->peer, ch) != WL_OK)
        return 0;
    e->out_buf = malloc(n);
    if (e->out_buf == NULL) {
        (void)wl_peer_close(a->peer, ch);
        return 0;
    }
    memcpy(e->out_buf, buf, n);
    e->out_len = n;
    e->active = 1;
    e->ch = ch;
    log_line(a, "caps: reported", "");
    return 1;
}

/*
 * @time reports the agent's wall clock, and with "set <seconds>" adopts the
 * client's. The target keeps time badly (an A21m's RTC drifts minutes a day,
 * and a dead CMOS battery loses it altogether), and everything the protocol
 * decides about files rests on it: sync compares modification times, so a
 * clock that is wrong makes it either resend a whole tree or miss changes.
 *
 * Setting the clock under a running session is safe because the link's timers
 * are monotonic (wl_clock_ms, GetTickCount on the target), not wall clock.
 * This is exactly what could not be done over Kermit, whose timers moved with
 * the clock and whose session died when it changed.
 *
 * Returns nonzero if the slot is now serving the answer.
 */
static int agent_time(struct agent *a, uint16_t ch, const char *arg, struct exec *e)
{
    char buf[256];
    char zone[64];
    uint64_t now = 0;
    size_t n = 0;
    int off;

    if (strncmp(arg, "set ", 4) == 0) {
        uint64_t want = 0;
        const char *p = arg + 4;

        while (*p == ' ')
            p++;
        if (*p < '0' || *p > '9') {
            (void)wl_peer_reject(a->peer, ch, WL_E_MALFORMED, "@time set needs a number");
            return 0;
        }
        for (; *p >= '0' && *p <= '9'; p++)
            want = (want * 10U) + (uint64_t)(*p - '0');
        if (wl_clock_set_utc(want) != WL_OK) {
            (void)wl_peer_reject(a->peer, ch, WL_E_ACCESS, wl_error());
            return 0;
        }
        log_line(a, "clock: set from the client", "");
    } else if (arg[0] != '\0') {
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, "usage: @time [set SECONDS]");
        return 0;
    }
    if (wl_clock_utc(&now) != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, wl_error());
        return 0;
    }
    off = wl_clock_local_offset_minutes();
    n += put_lit(buf + n, "utc ");
    n += u64_dec(now, buf + n);
    n += put_lit(buf + n, "\nlocaloffset ");
    if (off < 0) {
        buf[n++] = '-';
        off = -off;
    }
    n += u64_dec((uint64_t)off, buf + n);
    buf[n++] = '\n';
    wl_clock_zone_name(zone, sizeof(zone));
    if (zone[0] != '\0') {
        n += put_lit(buf + n, "zone ");
        n += put_lit(buf + n, zone);
        buf[n++] = '\n';
    }

    if (wl_peer_accept(a->peer, ch) != WL_OK)
        return 0;
    e->out_buf = malloc(n);
    if (e->out_buf == NULL) {
        (void)wl_peer_close(a->peer, ch);
        return 0;
    }
    memcpy(e->out_buf, buf, n);
    e->out_len = n;
    e->active = 1;
    e->ch = ch;
    return 1;
}

/*
 * Commands beginning with '@' address the agent itself rather than the shell
 * (docs/protocol.md, "EXEC"). @stop exists so the agent can be replaced: the
 * launcher copies a new binary in and starts it again. @dir lists a directory
 * without going near command.com, which is what makes it report long names,
 * exact sizes and a real exit status. @caps reports what the port it is
 * serving on can be set to, and @time reads or sets the clock.
 *
 * Returns nonzero if the exec slot is now in use.
 */
static int agent_command(struct agent *a, uint16_t ch, const char *cmd, struct exec *e)
{
    uint8_t code[4];

    if (strncmp(cmd, "@dir", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char *path = cmd + 4;

        while (*path == ' ')
            path++;
        return agent_dir(a, ch, path, e);
    }
    if (strcmp(cmd, "@caps") == 0)
        return agent_caps(a, ch, e);
    if (strncmp(cmd, "@time", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\0')) {
        const char *arg = cmd + 5;

        while (*arg == ' ')
            arg++;
        return agent_time(a, ch, arg, e);
    }
    if (strcmp(cmd, "@stop") != 0) {
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, "unknown agent command");
        return 0;
    }
    if (wl_peer_accept(a->peer, ch) != WL_OK)
        return 0;
    wl_put_u32le(code, 0);
    (void)wl_peer_send(a->peer, ch, WL_EXEC_EXIT, code, sizeof(code));
    (void)wl_peer_close(a->peer, ch);
    a->stopping = 1;
    log_line(a, "stopping at the client's request", "");
    emit_at(a, WL_EV_STOPPING, ch, 0);
    return 0;
}

static void open_exec(struct agent *a, uint16_t ch, const uint8_t *args, size_t n)
{
    struct wl_rbuf r;
    const char *cmd;
    const char *cwd;
    size_t cmd_n;
    size_t cwd_n;
    uint8_t flags;
    char *cmd_z;
    char *cwd_z;
    struct exec *e = NULL;
    wl_status st;

    wl_rbuf_init(&r, args, n);
    cmd = wl_r_str(&r, &cmd_n);
    cwd = wl_r_str(&r, &cwd_n);
    flags = wl_r_u8(&r);
    if (r.bad || cmd_n == 0) {
        (void)wl_peer_reject(a->peer, ch, WL_E_MALFORMED, "malformed EXEC arguments");
        return;
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC && e == NULL; i++) {
        if (!a->execs[i].active)
            e = &a->execs[i];
    }
    if (e == NULL) {
        (void)wl_peer_reject(a->peer, ch, WL_E_PROCESS, "too many commands running");
        return;
    }
    cmd_z = malloc(cmd_n + 1);
    cwd_z = malloc(cwd_n + 1);
    e->stdin_q = malloc(STDIN_QUEUE);
    if (cmd_z == NULL || cwd_z == NULL || e->stdin_q == NULL) {
        free(cmd_z);
        free(cwd_z);
        free(e->stdin_q);
        e->stdin_q = NULL;
        (void)wl_peer_reject(a->peer, ch, WL_E_PROCESS, "agent out of memory");
        return;
    }
    memcpy(cmd_z, cmd, cmd_n);
    cmd_z[cmd_n] = '\0';
    memcpy(cwd_z, cwd, cwd_n);
    cwd_z[cwd_n] = '\0';

    if (cmd_z[0] == '@') {
        if (!agent_command(a, ch, cmd_z, e)) {
            free(e->stdin_q);
            e->stdin_q = NULL;
        }
        free(cmd_z);
        free(cwd_z);
        return;
    }
    {
        char bad = '\0';

        if (!drives_are_ready(a, cmd_z, &bad)) {
            char msg[64];

            sprintf(msg, "drive %c: is not ready", bad);
            (void)wl_peer_reject(a->peer, ch, WL_E_NOT_FOUND, msg);
            log_line(a, "exec refused: ", msg);
            free(cmd_z);
            free(cwd_z);
            free(e->stdin_q);
            e->stdin_q = NULL;
            return;
        }
    }
    st = wl_proc_start(cmd_z, cwd_z, (flags & WL_EXEC_MERGE_STDERR) != 0, &e->proc);
    if (st != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, st == WL_ERR_INVAL ? WL_E_PATH : WL_E_PROCESS,
                             wl_error());
        log_line(a, "exec failed: ", wl_error());
        free(e->stdin_q);
        e->stdin_q = NULL;
    } else if (wl_peer_accept(a->peer, ch) != WL_OK) {
        end_exec(a, e);
    } else {
        e->active = 1;
        e->ch = ch;
        /* Input is credited as the process takes it, not as it arrives: the
         * queue below is exactly one credit window, and that only holds if
         * the client is told to refill at the rate the command reads. */
        (void)wl_peer_hold_credit(a->peer, ch);
        log_line(a, "exec: ", cmd_z);
        emit_work(a, ch, WL_KIND_EXEC, cmd_z, 0);
    }
    free(cmd_z);
    free(cwd_z);
}

static struct xfer *new_xfer(struct agent *a, uint16_t ch, uint8_t kind,
                             const char *path, size_t path_n)
{
    struct xfer *x = NULL;

    for (unsigned i = 0; i < WL_AGENT_MAX_XFER && x == NULL; i++) {
        if (!a->xfers[i].active)
            x = &a->xfers[i];
    }
    if (x == NULL || path_n == 0 || path_n >= sizeof(x->path)) {
        (void)wl_peer_reject(a->peer, ch, x == NULL ? WL_E_PROCESS : WL_E_PATH,
                             x == NULL ? "too many transfers" : "path too long");
        return NULL;
    }
    memset(x, 0, sizeof(*x));
    memcpy(x->path, path, path_n);
    x->path[path_n] = '\0';
    x->ch = ch;
    x->kind = kind;
    return x;
}

static void open_put(struct agent *a, uint16_t ch, const uint8_t *args, size_t n)
{
    struct wl_rbuf r;
    const char *path;
    size_t path_n;
    struct xfer *x;
    uint64_t size;
    uint64_t mtime;
    uint8_t flags;
    uint64_t have_size;
    uint64_t have_mtime;
    int is_dir;

    wl_rbuf_init(&r, args, n);
    path = wl_r_str(&r, &path_n);
    size = wl_r_u64(&r);
    mtime = wl_r_u64(&r);
    flags = wl_r_u8(&r);
    if (r.bad) {
        (void)wl_peer_reject(a->peer, ch, WL_E_MALFORMED, "malformed PUT arguments");
        return;
    }
    x = new_xfer(a, ch, WL_KIND_PUT, path, path_n);
    if (x == NULL)
        return;
    if (wl_fs_stat(x->path, &have_size, &have_mtime, &is_dir) == WL_OK) {
        if (is_dir) {
            (void)wl_peer_reject(a->peer, ch, WL_E_PATH, "destination is a directory");
            memset(x, 0, sizeof(*x));
            return;
        }
        if ((flags & WL_PUT_OVERWRITE) == 0) {
            (void)wl_peer_reject(a->peer, ch, WL_E_EXISTS, "file exists");
            memset(x, 0, sizeof(*x));
            return;
        }
    }
    if (wl_fs_temp_name(x->path, x->tmp, sizeof(x->tmp)) == WL_OK)
        x->f = fopen(x->tmp, "wb");
    if (x->f == NULL) {
        (void)wl_peer_reject(a->peer, ch, WL_E_ACCESS, "cannot create the file");
        memset(x, 0, sizeof(*x));
        return;
    }
    x->size = size;
    x->mtime = mtime;
    if (wl_peer_accept(a->peer, ch) != WL_OK) {
        end_xfer(a, x, 0);
        return;
    }
    x->active = 1;
    log_line(a, "put: ", x->path);
    emit_work(a, ch, WL_KIND_PUT, x->path, x->size);
}

static void open_get(struct agent *a, uint16_t ch, const uint8_t *args, size_t n)
{
    struct wl_rbuf r;
    const char *path;
    size_t path_n;
    struct xfer *x;
    uint8_t meta[16];
    int is_dir;

    wl_rbuf_init(&r, args, n);
    path = wl_r_str(&r, &path_n);
    if (r.bad) {
        (void)wl_peer_reject(a->peer, ch, WL_E_MALFORMED, "malformed GET arguments");
        return;
    }
    x = new_xfer(a, ch, WL_KIND_GET, path, path_n);
    if (x == NULL)
        return;
    if (wl_fs_stat(x->path, &x->size, &x->mtime, &is_dir) != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, WL_E_NOT_FOUND, wl_error());
        memset(x, 0, sizeof(*x));
        return;
    }
    if (is_dir) {
        (void)wl_peer_reject(a->peer, ch, WL_E_PATH, "that is a directory");
        memset(x, 0, sizeof(*x));
        return;
    }
    x->f = fopen(x->path, "rb");
    if (x->f == NULL) {
        (void)wl_peer_reject(a->peer, ch, WL_E_ACCESS, "cannot read the file");
        memset(x, 0, sizeof(*x));
        return;
    }
    if (wl_peer_accept(a->peer, ch) != WL_OK) {
        end_xfer(a, x, 0);
        return;
    }
    wl_put_u64le(meta, x->size);
    wl_put_u64le(meta + 8, x->mtime);
    x->active = 1;
    if (wl_peer_send(a->peer, ch, WL_FILE_META, meta, sizeof(meta)) != WL_OK) {
        end_xfer(a, x, 0);
        return;
    }
    log_line(a, "get: ", x->path);
    emit_work(a, ch, WL_KIND_GET, x->path, x->size);
}

/* ---------------------------------------------------------------------------
 * sync
 * ------------------------------------------------------------------------- */

static void sync_end(struct agent *a, struct sync *y)
{
    if (y->active)
        emit_at(a, WL_EV_FINISHED, y->ch, 0);
    if (y->hash_f != NULL)
        (void)fclose(y->hash_f);
    if (y->f != NULL)
        (void)fclose(y->f);
    if (y->tmp[0] != '\0')
        (void)wl_fs_remove(y->tmp);
    free(y->need);
    free(y->seen);
    free(y->pend);
    memset(y, 0, sizeof(*y));
}

static int sync_queue_need(struct sync *y, const char *rel)
{
    if (y->n_need == y->cap_need) {
        size_t cap = y->cap_need == 0 ? 64 : y->cap_need * 2;
        char (*grown)[WL_PATH_MAX] = realloc(y->need, cap * WL_PATH_MAX);

        if (grown == NULL)
            return 0;
        y->need = grown;
        y->cap_need = cap;
    }
    (void)snprintf(y->need[y->n_need], WL_PATH_MAX, "%s", rel);
    y->n_need++;
    return 1;
}

/* Remembers a manifest path for the deletion pass, as NUL-separated text. */
static void sync_remember(struct sync *y, const char *rel)
{
    size_t n = strlen(rel) + 1;

    if ((y->flags & WL_SYNC_DELETE) == 0 || y->seen_overflow)
        return;
    if (y->seen_len + n > y->seen_cap) {
        size_t cap = y->seen_cap == 0 ? 8192 : y->seen_cap * 2;
        char *grown;

        while (cap < y->seen_len + n)
            cap *= 2;
        /* A tree far bigger than the target's memory stops the deletion
         * pass rather than the sync: files are still updated, nothing is
         * removed, and the client is told. */
        if (cap > (1U << 20)) {
            y->seen_overflow = 1;
            return;
        }
        grown = realloc(y->seen, cap);
        if (grown == NULL) {
            y->seen_overflow = 1;
            return;
        }
        y->seen = grown;
        y->seen_cap = cap;
    }
    memcpy(y->seen + y->seen_len, rel, n);
    y->seen_len += n;
}

static int sync_was_listed(const struct sync *y, const char *rel)
{
    for (size_t at = 0; at < y->seen_len; at += strlen(y->seen + at) + 1) {
        if (strcmp(y->seen + at, rel) == 0)
            return 1;
    }
    return 0;
}

/* Compares one manifest entry with what is on disk here. */
static void sync_entry(struct agent *a, struct sync *y, const char *rel, uint64_t size,
                       uint64_t mtime, const uint8_t *hash)
{
    char full[WL_PATH_MAX];
    uint64_t have_size;
    uint64_t have_mtime;
    int is_dir;

    sync_remember(y, rel);
    if (!wl_fs_join(full, sizeof(full), y->root, rel)) {
        y->failed++;
        return;
    }
    if (wl_fs_stat(full, &have_size, &have_mtime, &is_dir) != WL_OK || is_dir ||
        have_size != size) {
        if (!sync_queue_need(y, rel))
            y->failed++;
        return;
    }
    if ((have_mtime > mtime ? have_mtime - mtime : mtime - have_mtime) <= WL_MTIME_SLACK) {
        y->same++;
        return;
    }
    /* Same size, different time: the contents decide, later and in slices. */
    if (y->n_pend == y->cap_pend) {
        size_t cap = y->cap_pend == 0 ? 32 : y->cap_pend * 2;
        struct pending_hash *grown = realloc(y->pend, cap * sizeof(*grown));

        if (grown == NULL) {
            /* Cannot check it, so send it: wasteful, never wrong. */
            if (!sync_queue_need(y, rel))
                y->failed++;
            return;
        }
        y->pend = grown;
        y->cap_pend = cap;
    }
    {
        struct pending_hash *ph = &y->pend[y->n_pend++];

        (void)snprintf(ph->rel, sizeof(ph->rel), "%s", rel);
        memcpy(ph->want, hash, WL_HASH_BYTES);
        ph->mtime = mtime;
    }
    (void)a;
}

/* Hashes at most this much per call, so the link is serviced every few
 * tens of milliseconds even on the target's disk. */
#define HASH_SLICE 262144U

/* Advances the hashing cursor by one slice. Returns 1 while work remains. */
static int sync_hash_step(struct sync *y)
{
    struct pending_hash *ph;
    char full[WL_PATH_MAX];
    uint8_t buf[16384];
    size_t done = 0;

    if (y->pend_at >= y->n_pend)
        return 0;
    ph = &y->pend[y->pend_at];
    if (y->hash_f == NULL) {
        if (!wl_fs_join(full, sizeof(full), y->root, ph->rel)) {
            y->pend_at++;
            return 1;
        }
        y->hash_f = fopen(full, "rb");
        wl_sha256_init(&y->hash);
        if (y->hash_f == NULL) {
            /* Unreadable now, though it was there a moment ago: ask for it. */
            if (!sync_queue_need(y, ph->rel))
                y->failed++;
            y->pend_at++;
            return 1;
        }
    }
    while (done < HASH_SLICE) {
        size_t n = fread(buf, 1, sizeof(buf), y->hash_f);

        if (n > 0) {
            wl_sha256_update(&y->hash, buf, n);
            done += n;
        }
        if (n < sizeof(buf))
            break; /* end of file, or a read error */
    }
    if (done >= HASH_SLICE)
        return 1; /* more of this file next time round */

    {
        uint8_t digest[32];
        int bad = ferror(y->hash_f) != 0;

        (void)fclose(y->hash_f);
        y->hash_f = NULL;
        wl_sha256_final(&y->hash, digest);
        if (bad || memcmp(digest, ph->want, WL_HASH_BYTES) != 0) {
            if (!sync_queue_need(y, ph->rel))
                y->failed++;
        } else {
            y->same++;
            /* Only the timestamp differed: line it up so the next sync
             * takes the cheap path and never hashes this file again. */
            if ((y->flags & WL_SYNC_DRY_RUN) == 0 &&
                wl_fs_join(full, sizeof(full), y->root, ph->rel))
                (void)wl_fs_set_mtime(full, ph->mtime);
        }
    }
    y->pend_at++;
    return 1;
}

struct delete_ctx {
    struct agent *a;
    struct sync *y;
};

static int delete_extra(void *ctx, const char *rel, uint64_t size, uint64_t mtime)
{
    struct delete_ctx *d = ctx;
    char full[WL_PATH_MAX];

    (void)size;
    (void)mtime;
    if (sync_was_listed(d->y, rel))
        return 1;
    if (!wl_fs_join(full, sizeof(full), d->y->root, rel)) {
        d->y->failed++;
        return 1;
    }
    if ((d->y->flags & WL_SYNC_DRY_RUN) != 0 || wl_fs_remove(full) == WL_OK)
        d->y->deleted++;
    else
        d->y->failed++;
    return 1;
}

static void sync_manifest_done(struct agent *a, struct sync *y)
{
    y->manifest_done = 1;
    if ((y->flags & WL_SYNC_DELETE) == 0)
        return;
    if (y->seen_overflow) {
        log_line(a, "sync: too many files to delete safely; nothing removed", "");
        return;
    }
    {
        struct delete_ctx d;

        d.a = a;
        d.y = y;
        /* Hidden files are included: the client decides what it sends, and
         * a file it never mentions is extraneous either way. */
        (void)wl_fs_walk(y->root, 1, delete_extra, &d);
    }
}

static void sync_file_begin(struct agent *a, struct sync *y, struct wl_rbuf *r)
{
    size_t len;
    const char *rel = wl_r_str(r, &len);
    uint64_t size = wl_r_u64(r);
    uint64_t mtime = wl_r_u64(r);
    char dir[WL_PATH_MAX];
    char *slash;

    (void)size;
    y->file_bad = 0;
    y->crc = 0;
    y->left = 0;
    y->mtime = mtime;
    {
        char rel_z[WL_PATH_MAX];

        if (r->bad || len == 0 || len >= sizeof(rel_z)) {
            y->file_bad = 1;
            y->failed++;
            return;
        }
        memcpy(rel_z, rel, len);
        rel_z[len] = '\0';
        if (!wl_fs_join(y->path, sizeof(y->path), y->root, rel_z)) {
            y->file_bad = 1;
            y->failed++;
            return;
        }
    }
    (void)snprintf(dir, sizeof(dir), "%s", y->path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (wl_fs_mkdir_p(dir) != WL_OK) {
            y->file_bad = 1;
            y->failed++;
            log_line(a, "sync: ", wl_error());
            return;
        }
    }
    if (wl_fs_temp_name(y->path, y->tmp, sizeof(y->tmp)) == WL_OK)
        y->f = fopen(y->tmp, "wb");
    if (y->f == NULL) {
        y->file_bad = 1;
        y->failed++;
        y->tmp[0] = '\0';
    }
}

static void sync_file_end(struct agent *a, struct sync *y, uint32_t want_crc)
{
    if (y->file_bad || y->f == NULL) {
        y->file_bad = 0;
        return;
    }
    if (fclose(y->f) != 0 || y->crc != want_crc) {
        y->f = NULL;
        (void)wl_fs_remove(y->tmp);
        y->tmp[0] = '\0';
        y->failed++;
        log_line(a, "sync: bad copy of ", y->path);
        return;
    }
    y->f = NULL;
    if (wl_fs_replace(y->tmp, y->path) != WL_OK) {
        (void)wl_fs_remove(y->tmp);
        y->tmp[0] = '\0';
        y->failed++;
        log_line(a, "sync: ", wl_error());
        return;
    }
    y->tmp[0] = '\0';
    if (y->mtime != 0)
        (void)wl_fs_set_mtime(y->path, y->mtime);
    y->written++;
    (void)a;
}

static void on_sync_record(struct agent *a, struct sync *y, uint8_t type, const uint8_t *p,
                           size_t n)
{
    struct wl_rbuf r;

    wl_rbuf_init(&r, p, n);
    switch (type) {
    case WL_SYNC_MANIFEST: {
        uint16_t count = wl_r_u16(&r);

        for (uint16_t i = 0; i < count && !r.bad; i++) {
            size_t len;
            const char *rel = wl_r_str(&r, &len);
            uint64_t size = wl_r_u64(&r);
            uint64_t mtime = wl_r_u64(&r);
            const uint8_t *hash = wl_r_take(&r, WL_HASH_BYTES);
            char rel_z[WL_PATH_MAX];

            if (r.bad || len == 0 || len >= sizeof(rel_z))
                break;
            memcpy(rel_z, rel, len);
            rel_z[len] = '\0';
            sync_entry(a, y, rel_z, size, mtime, hash);
        }
        return;
    }
    case WL_SYNC_MANIFEST_END:
        sync_manifest_done(a, y);
        return;
    case WL_SYNC_FILE:
        sync_file_begin(a, y, &r);
        return;
    case WL_SYNC_DATA:
        if (y->file_bad || y->f == NULL)
            return;
        if (n > 0 && fwrite(p, 1, n, y->f) != n) {
            y->file_bad = 1;
            y->failed++;
            log_line(a, "sync: cannot write ", y->path);
            return;
        }
        y->crc = wl_crc32(y->crc, p, n);
        return;
    case WL_SYNC_FILE_END:
        if (n == 4)
            sync_file_end(a, y, wl_get_u32le(p));
        return;
    case WL_SYNC_DONE: {
        uint8_t buf[8];

        wl_put_u32le(buf, y->written);
        wl_put_u32le(buf + 4, y->failed);
        (void)wl_peer_send(a->peer, y->ch, WL_SYNC_SUMMARY, buf, sizeof(buf));
        log_line(a, "sync: done", "");
        return;
    }
    default:
        return;
    }
}

/* Hands the agent's NEED list to the client as the mux accepts it. */
static void service_sync(struct agent *a, struct sync *y)
{
    uint8_t buf[WL_REC_MAX_PAYLOAD];

    /* Hashing runs whether or not the whole manifest has arrived, one
     * slice per loop iteration, so the link is always serviced in between. */
    if (sync_hash_step(y))
        return;
    /* 1 means the manifest arrived and the answer is still to go out;
     * 2 means it has gone. */
    if (y->manifest_done != 1)
        return;
    while (y->sent_need < y->n_need) {
        struct wl_wbuf w;
        size_t count = 0;

        wl_wbuf_init(&w, buf, 4096);
        wl_w_u16(&w, 0);
        while (y->sent_need + count < y->n_need) {
            const char *rel = y->need[y->sent_need + count];
            size_t before = w.len;

            wl_w_str(&w, rel, strlen(rel));
            if (w.overflow) {
                w.overflow = 0;
                w.len = before;
                break;
            }
            count++;
        }
        if (count == 0)
            break;
        wl_put_u16le(buf, (uint16_t)count);
        if (wl_peer_send(a->peer, y->ch, WL_SYNC_NEED, buf, w.len) != WL_OK)
            return;
        y->sent_need += count;
    }
    if (y->sent_need == y->n_need && !y->file_bad) {
        uint8_t end[8];

        wl_put_u32le(end, y->same);
        wl_put_u32le(end + 4, y->deleted);
        if (wl_peer_send(a->peer, y->ch, WL_SYNC_NEED_END, end, sizeof(end)) == WL_OK)
            y->manifest_done = 2; /* sent */
    }
}

static void open_sync(struct agent *a, uint16_t ch, const uint8_t *args, size_t n)
{
    struct wl_rbuf r;
    const char *root;
    size_t root_n;
    struct sync *y = &a->sync;

    wl_rbuf_init(&r, args, n);
    root = wl_r_str(&r, &root_n);
    if (r.bad || root_n == 0 || root_n >= WL_PATH_MAX - 64) {
        (void)wl_peer_reject(a->peer, ch, WL_E_PATH, "bad sync root");
        return;
    }
    if (y->active) {
        (void)wl_peer_reject(a->peer, ch, WL_E_PROCESS, "a sync is already running");
        return;
    }
    memset(y, 0, sizeof(*y));
    memcpy(y->root, root, root_n);
    y->root[root_n] = '\0';
    y->flags = wl_r_u8(&r);
    if ((y->flags & WL_SYNC_DRY_RUN) == 0 && wl_fs_mkdir_p(y->root) != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, WL_E_ACCESS, wl_error());
        memset(y, 0, sizeof(*y));
        return;
    }
    if (wl_peer_accept(a->peer, ch) != WL_OK) {
        memset(y, 0, sizeof(*y));
        return;
    }
    y->active = 1;
    y->ch = ch;
    log_line(a, "sync: ", y->root);
    emit_work(a, ch, WL_KIND_SYNC, y->root, 0);
}

static void on_open(void *ctx, uint16_t ch, uint8_t kind, const uint8_t *args, size_t n)
{
    struct agent *a = ctx;

    switch (kind) {
    case WL_KIND_EXEC:
        open_exec(a, ch, args, n);
        break;
    case WL_KIND_PUT:
        open_put(a, ch, args, n);
        break;
    case WL_KIND_GET:
        open_get(a, ch, args, n);
        break;
    case WL_KIND_SYNC:
        open_sync(a, ch, args, n);
        break;
    default:
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, "operation not supported");
        break;
    }
}

/* Queues FAIL and stops the transfer; the temporary file goes too. */
static void xfer_fail(struct agent *a, struct xfer *x, uint16_t code, const char *msg)
{
    uint8_t buf[256];
    struct wl_wbuf w;
    size_t n = strlen(msg);

    wl_wbuf_init(&w, buf, sizeof(buf));
    wl_w_u16(&w, code);
    wl_w_str(&w, msg, n < 200 ? n : 200);
    (void)wl_peer_send(a->peer, x->ch, WL_FILE_FAIL, buf, w.len);
    log_line(a, "transfer failed: ", msg);
    x->failed = 1;
    x->finished = 1;
    if (x->f != NULL) {
        (void)fclose(x->f);
        x->f = NULL;
    }
    if (x->kind == WL_KIND_PUT && x->tmp[0] != '\0')
        (void)wl_fs_remove(x->tmp);
}

static void put_end(struct agent *a, struct xfer *x, uint32_t want_crc)
{
    if (fclose(x->f) != 0) {
        x->f = NULL;
        xfer_fail(a, x, WL_E_DISK_FULL, "cannot finish writing the file");
        return;
    }
    x->f = NULL;
    /* Both say the file did not arrive, but not for the same reason: a wrong
     * count is the transport, a wrong sum over the right count is the bytes.
     * The numbers go with it so the answer does not need a second attempt. */
    if (x->done != x->size) {
        char msg[96];

        (void)snprintf(msg, sizeof(msg), "wrong number of bytes: expected %llu, received %llu",
                       (unsigned long long)x->size, (unsigned long long)x->done);
        xfer_fail(a, x, WL_E_CRC, msg);
        return;
    }
    if (x->crc != want_crc) {
        char msg[96];

        (void)snprintf(msg, sizeof(msg),
                       "checksum mismatch over %llu bytes: sent %08lx, received %08lx",
                       (unsigned long long)x->done, (unsigned long)want_crc,
                       (unsigned long)x->crc);
        xfer_fail(a, x, WL_E_CRC, msg);
        return;
    }
    if (wl_fs_replace(x->tmp, x->path) != WL_OK) {
        xfer_fail(a, x, WL_E_ACCESS, wl_error());
        return;
    }
    x->tmp[0] = '\0';
    /* A failure to set the time does not invalidate the contents, but sync
     * would then copy the file again, so it is worth reporting. */
    if (x->mtime != 0 && wl_fs_set_mtime(x->path, x->mtime) != WL_OK)
        log_line(a, "warning: ", wl_error());
    (void)wl_peer_send(a->peer, x->ch, WL_FILE_DONE, NULL, 0);
    x->finished = 1;
}

static void on_xfer_record(struct agent *a, struct xfer *x, uint8_t type,
                           const uint8_t *p, size_t n)
{
    if (x->failed || x->kind != WL_KIND_PUT)
        return;
    if (type == WL_FILE_DATA) {
        if (x->done + n > x->size) {
            xfer_fail(a, x, WL_E_CRC, "more data than the file announced");
            return;
        }
        if (n > 0 && fwrite(p, 1, n, x->f) != n) {
            xfer_fail(a, x, WL_E_DISK_FULL, "cannot write the file");
            return;
        }
        x->crc = wl_crc32(x->crc, p, n);
        x->done += n;
    } else if (type == WL_FILE_END && n == 4) {
        put_end(a, x, wl_get_u32le(p));
    }
}

static void on_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct agent *a = ctx;
    struct exec *e = find_exec(a, ch);
    struct xfer *x = find_xfer(a, ch);

    if (a->sync.active && a->sync.ch == ch) {
        on_sync_record(a, &a->sync, type, p, n);
        return;
    }
    if (x != NULL) {
        on_xfer_record(a, x, type, p, n);
        return;
    }
    if (e == NULL)
        return;
    if (type == WL_EXEC_STDIN) {
        /* The credit window (64 KiB) is the most the client can have in
         * flight and the queue is that size, so this cannot overflow: credit
         * is held until the process reads (wl_peer_hold_credit at open), so
         * the window empties at the pace the command does. A client that
         * breaks the protocol loses the excess rather than this buffer. */
        size_t room = STDIN_QUEUE - e->stdin_len;
        size_t k = n < room ? n : room;

        memcpy(e->stdin_q + e->stdin_len, p, k);
        e->stdin_len += k;
    } else if (type == WL_EXEC_STDIN_EOF) {
        e->stdin_eof = 1;
    } else if (type == WL_EXEC_CANCEL) {
        /* Killed here, answered in service_exec, which can retry a full mux
         * queue; a handler cannot. */
        if (!e->cancelled) {
            e->cancelled = 1;
            wl_proc_kill(e->proc);
            log_line(a, "exec: cancelled by the client", "");
        }
    }
}

static void on_closed(void *ctx, uint16_t ch)
{
    struct agent *a = ctx;
    struct exec *e = find_exec(a, ch);
    struct xfer *x = find_xfer(a, ch);

    /* The client hung up on a running command (Ctrl-C on its side). */
    if (e != NULL && !e->close_sent) {
        (void)wl_peer_close(a->peer, ch);
        end_exec(a, e);
    }
    if (x != NULL) {
        (void)wl_peer_close(a->peer, ch);
        end_xfer(a, x, 0);
    }
    if (a->sync.active && a->sync.ch == ch) {
        (void)wl_peer_close(a->peer, ch);
        sync_end(a, &a->sync);
    }
}

static void on_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct agent *a = ctx;
    char buf[256];

    (void)code;
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, msg, n);
    buf[n] = '\0';
    log_line(a, "session error: ", buf);
}

/* ---------------------------------------------------------------------------
 * servicing running commands
 * ------------------------------------------------------------------------- */

/*
 * The next chunk of one output stream, from the process or, for an agent
 * command such as @dir, from the buffer it left behind. Keeping both behind
 * one call is what lets an agent command reuse the whole exec path: flow
 * control, chunking, EXIT and CLOSE.
 */
static size_t exec_output(struct exec *e, int s, int stream, int *eof)
{
    size_t n;

    if (e->proc != NULL)
        return wl_proc_read(e->proc, stream, e->pend[s], OUT_CHUNK, eof);
    if (s != 0) {
        *eof = 1; /* an agent command has no stderr */
        return 0;
    }
    n = e->out_len - e->out_at;
    if (n > OUT_CHUNK)
        n = OUT_CHUNK;
    memcpy(e->pend[s], e->out_buf + e->out_at, n);
    e->out_at += n;
    if (e->out_at == e->out_len)
        *eof = 1;
    return n;
}

static void service_exec(struct agent *a, struct exec *e)
{
    static const uint8_t types[2] = {WL_EXEC_STDOUT, WL_EXEC_STDERR};
    static const int streams[2] = {WL_PROC_STDOUT, WL_PROC_STDERR};

    /*
     * A cancelled command is answered at once rather than by waiting for the
     * child to die and reporting its real status. Its remaining output and
     * exit code are of no interest to a client that has given up, and the
     * child may never die at all: a Win9x DOS VM sitting on a critical-error
     * prompt ignores TerminateProcess until someone dismisses it. The client
     * gets its acknowledgement either way and the process goes to the
     * reaper, which is what keeps the agent serving.
     */
    if (e->cancelled) {
        if (!e->exit_sent) {
            uint8_t buf[4];

            wl_put_u32le(buf, WL_EXEC_EXIT_CANCELLED);
            if (wl_peer_send(a->peer, e->ch, WL_EXEC_EXIT, buf, sizeof(buf)) == WL_OK)
                e->exit_sent = 1;
        }
        if (e->exit_sent && wl_peer_close(a->peer, e->ch) == WL_OK)
            end_exec(a, e);
        return;
    }

    if (e->proc != NULL) {
        if (e->stdin_len > 0) {
            size_t n = wl_proc_write(e->proc, e->stdin_q, e->stdin_len);

            memmove(e->stdin_q, e->stdin_q + n, e->stdin_len - n);
            e->stdin_len -= n;
            wl_peer_consumed(a->peer, e->ch, n);
        }
        if (e->stdin_eof && e->stdin_len == 0) {
            wl_proc_close_stdin(e->proc);
            e->stdin_eof = 0;
        }
    }

    for (int s = 0; s < 2; s++) {
        while (!e->eof[s] || e->pend_len[s] > 0) {
            if (e->pend_len[s] > 0) {
                size_t n = wl_peer_write(a->peer, e->ch, types[s], e->pend[s], e->pend_len[s]);

                memmove(e->pend[s], e->pend[s] + n, e->pend_len[s] - n);
                e->pend_len[s] -= n;
                e->out_total += n;
                if (e->pend_len[s] > 0)
                    break; /* mux queue full; the process waits */
            }
            if (e->eof[s])
                break;
            e->pend_len[s] = exec_output(e, s, streams[s], &e->eof[s]);
            if (e->pend_len[s] == 0)
                break;
        }
    }

    if (e->out_total != e->reported) {
        e->reported = e->out_total;
        emit_progress(a, e->ch, e->out_total);
    }
    if (!e->exited)
        e->exited = e->proc != NULL ? wl_proc_exited(e->proc, &e->code) : 1;
    /* EXIT only after both streams are drained, so the client sees every
     * byte of output before the exit status. */
    if (e->exited && e->eof[0] && e->eof[1] && e->pend_len[0] == 0 && e->pend_len[1] == 0) {
        if (!e->exit_sent) {
            uint8_t buf[4];

            wl_put_u32le(buf, e->code);
            if (wl_peer_send(a->peer, e->ch, WL_EXEC_EXIT, buf, sizeof(buf)) == WL_OK)
                e->exit_sent = 1;
        }
        if (e->exit_sent && !e->close_sent && wl_peer_close(a->peer, e->ch) == WL_OK) {
            e->close_sent = 1;
            end_exec(a, e);
        }
    }
}

/* Streams a GET out, and closes either kind of transfer once it is over. */
static void service_xfer(struct agent *a, struct xfer *x)
{
    /* Both directions land in x->done, so one place reports either kind.
     * A sync has no byte count of its own to report: its channel carries a
     * manifest as well as file contents, and the files it writes are counted
     * in its own summary. */
    if (x->done != x->reported) {
        x->reported = x->done;
        emit_progress(a, x->ch, x->done);
    }
    if (x->kind == WL_KIND_GET && !x->finished && !x->failed) {
        while (!x->at_eof || x->buf_len > 0) {
            if (x->buf_len > 0) {
                size_t n = wl_peer_write(a->peer, x->ch, WL_FILE_DATA, x->buf, x->buf_len);

                memmove(x->buf, x->buf + n, x->buf_len - n);
                x->buf_len -= n;
                if (x->buf_len > 0)
                    return; /* the mux queue is full; try again next loop */
            }
            if (x->at_eof)
                break;
            x->buf_len = fread(x->buf, 1, sizeof(x->buf), x->f);
            x->crc = wl_crc32(x->crc, x->buf, x->buf_len);
            x->done += x->buf_len;
            if (x->buf_len == 0) {
                if (ferror(x->f)) {
                    xfer_fail(a, x, WL_E_ACCESS, "cannot read the file");
                    return;
                }
                x->at_eof = 1;
            }
        }
        if (x->at_eof && x->buf_len == 0) {
            uint8_t crc[4];

            wl_put_u32le(crc, x->crc);
            if (wl_peer_send(a->peer, x->ch, WL_FILE_END, crc, sizeof(crc)) == WL_OK)
                x->finished = 1;
        }
    }
    if (x->finished && wl_peer_close(a->peer, x->ch) == WL_OK)
        end_xfer(a, x, 1);
}

wl_status wl_agent_serve(struct wl_transport *t, const struct wl_agent_cfg *cfg,
                         uint32_t seed)
{
    struct agent *a = calloc(1, sizeof(*a));
    struct wl_peer_handlers h;
    wl_status st = WL_OK;

    if (a == NULL)
        return wl_fail(WL_ERR_NOMEM, "agent: out of memory");
    a->cfg = cfg;
    a->t = t;
    memset(&h, 0, sizeof(h));
    h.ctx = a;
    h.session = on_session;
    h.ready = on_ready;
    h.open = on_open;
    h.record = on_record;
    h.closed = on_closed;
    h.error = on_error;
    a->peer = wl_peer_new(&cfg->session, seed, 0, 0, cfg->platform, &h);
    if (a->peer == NULL) {
        free(a);
        return WL_ERR_NOMEM;
    }
    while ((cfg->stop == NULL || !cfg->stop(cfg->stop_ctx)) && !a->stopping) {
        st = wl_peer_pump(a->peer, t, LOOP_MS);
        if (st == WL_ERR_CORRUPT) {
            /* The client's stream did not decompress: a mismatched build.
             * Keep serving; the next client starts a fresh session. Logged
             * once, since every pump of this session will say the same. */
            uint32_t gen = wl_session_generation(wl_peer_session(a->peer));

            if (gen != a->reported_gen) {
                a->reported_gen = gen;
                log_line(a, "session error: ", wl_error());
            }
            st = WL_OK;
        }
        if (st != WL_OK)
            break;
        /*
         * A client that goes away is noticed by the link rather than by a
         * handler, so the departure is a transition to watch for here. Without
         * it a display would show the last client as attached forever.
         */
        if (wl_peer_ready(a->peer) != a->was_ready) {
            a->was_ready = !a->was_ready;
            if (!a->was_ready) {
                log_line(a, "session: the client is gone", "");
                emit_at(a, WL_EV_GONE, 0, 0);
            }
        }
        for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
            if (a->execs[i].active)
                service_exec(a, &a->execs[i]);
        }
        for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
            if (a->xfers[i].active)
                service_xfer(a, &a->xfers[i]);
        }
        if (a->sync.active)
            service_sync(a, &a->sync);
        reap(a);
    }
    if (a->stopping && st == WL_OK) {
        /* Give the acknowledgement of @stop time to reach the client, so it
         * sees the command succeed rather than the link dying. */
        uint32_t linger = wl_clock_ms();

        while (!wl_peer_idle(a->peer) && wl_clock_since(linger) < 3000U) {
            if (wl_peer_pump(a->peer, t, LOOP_MS) != WL_OK)
                break;
        }
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active)
            end_exec(a, &a->execs[i]);
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active)
            end_xfer(a, &a->xfers[i], 0);
    }
    if (a->sync.active)
        sync_end(a, &a->sync);
    /* Shutting down is the one place that waits for a child at all, and even
     * there the wait is bounded: a child that will not go is abandoned rather
     * than forced, so the agent always reaches its exit and its launcher can
     * start the replacement. */
    while (a->reaping != NULL) {
        struct reaping *r = a->reaping;

        a->reaping = r->next;
        wl_proc_free(r->proc);
        free(r);
    }
    wl_peer_free(a->peer);
    free(a);
    return st;
}
