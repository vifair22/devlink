#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98link/agent.h"
#include "w98link/proc.h"
#include "w98link/proto.h"
#include "w98link/wire.h"

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
    uint32_t code;
    int exit_sent;
    int close_sent;
    uint8_t *stdin_q; /* client bytes the process has not taken yet */
    size_t stdin_len;
    int stdin_eof;
};

struct agent {
    const struct wl_agent_cfg *cfg;
    struct wl_peer *peer;
    uint32_t reported_gen; /* session whose stream error was already logged */
    struct exec execs[WL_AGENT_MAX_EXEC];
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

static struct exec *find_exec(struct agent *a, uint16_t ch)
{
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active && a->execs[i].ch == ch)
            return &a->execs[i];
    }
    return NULL;
}

static void end_exec(struct exec *e)
{
    wl_proc_free(e->proc);
    free(e->stdin_q);
    memset(e, 0, sizeof(*e));
}

/* ---------------------------------------------------------------------------
 * peer handlers
 * ------------------------------------------------------------------------- */

static void on_session(void *ctx)
{
    struct agent *a = ctx;

    /* The previous client is gone; nobody will read its commands' output. */
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active)
            end_exec(&a->execs[i]);
    }
    log_line(a, "session: new client", "");
}

static void on_ready(void *ctx, const struct wl_hello *h)
{
    struct agent *a = ctx;

    log_line(a, "session: client ", h->build);
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

    st = wl_proc_start(cmd_z, cwd_z, (flags & WL_EXEC_MERGE_STDERR) != 0, &e->proc);
    if (st != WL_OK) {
        (void)wl_peer_reject(a->peer, ch, st == WL_ERR_INVAL ? WL_E_PATH : WL_E_PROCESS,
                             wl_error());
        log_line(a, "exec failed: ", wl_error());
        free(e->stdin_q);
        e->stdin_q = NULL;
    } else if (wl_peer_accept(a->peer, ch) != WL_OK) {
        wl_proc_free(e->proc);
        free(e->stdin_q);
        memset(e, 0, sizeof(*e));
    } else {
        e->active = 1;
        e->ch = ch;
        log_line(a, "exec: ", cmd_z);
    }
    free(cmd_z);
    free(cwd_z);
}

static void on_open(void *ctx, uint16_t ch, uint8_t kind, const uint8_t *args, size_t n)
{
    struct agent *a = ctx;

    if (kind == WL_KIND_EXEC)
        open_exec(a, ch, args, n);
    else
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, "operation not supported");
}

static void on_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct agent *a = ctx;
    struct exec *e = find_exec(a, ch);

    if (e == NULL)
        return;
    if (type == WL_EXEC_STDIN) {
        /* The credit window (64 KiB) is the most the client can have in
         * flight, and the queue is that size, so it cannot overflow unless
         * the client breaks the protocol; if it does, the excess is lost. */
        size_t room = STDIN_QUEUE - e->stdin_len;
        size_t k = n < room ? n : room;

        memcpy(e->stdin_q + e->stdin_len, p, k);
        e->stdin_len += k;
    } else if (type == WL_EXEC_STDIN_EOF) {
        e->stdin_eof = 1;
    }
}

static void on_closed(void *ctx, uint16_t ch)
{
    struct agent *a = ctx;
    struct exec *e = find_exec(a, ch);

    /* The client hung up on a running command (Ctrl-C on its side). */
    if (e != NULL && !e->close_sent) {
        (void)wl_peer_close(a->peer, ch);
        end_exec(e);
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

static void service_exec(struct agent *a, struct exec *e)
{
    static const uint8_t types[2] = {WL_EXEC_STDOUT, WL_EXEC_STDERR};
    static const int streams[2] = {WL_PROC_STDOUT, WL_PROC_STDERR};

    if (e->stdin_len > 0) {
        size_t n = wl_proc_write(e->proc, e->stdin_q, e->stdin_len);

        memmove(e->stdin_q, e->stdin_q + n, e->stdin_len - n);
        e->stdin_len -= n;
    }
    if (e->stdin_eof && e->stdin_len == 0) {
        wl_proc_close_stdin(e->proc);
        e->stdin_eof = 0;
    }

    for (int s = 0; s < 2; s++) {
        while (!e->eof[s] || e->pend_len[s] > 0) {
            if (e->pend_len[s] > 0) {
                size_t n = wl_peer_write(a->peer, e->ch, types[s], e->pend[s], e->pend_len[s]);

                memmove(e->pend[s], e->pend[s] + n, e->pend_len[s] - n);
                e->pend_len[s] -= n;
                if (e->pend_len[s] > 0)
                    break; /* mux queue full; the process waits */
            }
            if (e->eof[s])
                break;
            e->pend_len[s] = wl_proc_read(e->proc, streams[s], e->pend[s], OUT_CHUNK, &e->eof[s]);
            if (e->pend_len[s] == 0)
                break;
        }
    }

    if (!e->exited)
        e->exited = wl_proc_exited(e->proc, &e->code);
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
            end_exec(e);
        }
    }
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
    while (cfg->stop == NULL || !cfg->stop(cfg->stop_ctx)) {
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
        for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
            if (a->execs[i].active)
                service_exec(a, &a->execs[i]);
        }
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active)
            end_exec(&a->execs[i]);
    }
    wl_peer_free(a->peer);
    free(a);
    return st;
}
