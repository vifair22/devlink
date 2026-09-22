#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98link/agent.h"
#include "w98link/clock.h"
#include "w98link/crc32.h"
#include "w98link/fs.h"
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
    uint32_t crc;
    int failed;
    int finished; /* DONE or FAIL queued */
    uint8_t buf[OUT_CHUNK];
    size_t buf_len;
    int at_eof;
};

struct agent {
    const struct wl_agent_cfg *cfg;
    struct wl_peer *peer;
    uint32_t reported_gen; /* session whose stream error was already logged */
    struct exec execs[WL_AGENT_MAX_EXEC];
    struct xfer xfers[WL_AGENT_MAX_XFER];
    int stopping; /* @stop received: finish the exchange, then return */
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

static struct xfer *find_xfer(struct agent *a, uint16_t ch)
{
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active && a->xfers[i].ch == ch)
            return &a->xfers[i];
    }
    return NULL;
}

static void end_xfer(struct xfer *x, int keep_temp)
{
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

    /* The previous client is gone; nobody will read its commands' output,
     * and its half-written uploads are worthless. */
    for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
        if (a->execs[i].active)
            end_exec(&a->execs[i]);
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active)
            end_xfer(&a->xfers[i], 0);
    }
    log_line(a, "session: new client", "");
}

static void on_ready(void *ctx, const struct wl_hello *h)
{
    struct agent *a = ctx;

    log_line(a, "session: client ", h->build);
}

/* Commands beginning with '@' address the agent itself rather than the
 * shell (docs/protocol.md, "EXEC"). @stop exists so the agent can be
 * replaced: the launcher copies a new binary in and starts it again. */
static void agent_command(struct agent *a, uint16_t ch, const char *cmd)
{
    uint8_t code[4];

    if (strcmp(cmd, "@stop") != 0) {
        (void)wl_peer_reject(a->peer, ch, WL_E_UNKNOWN, "unknown agent command");
        return;
    }
    if (wl_peer_accept(a->peer, ch) != WL_OK)
        return;
    wl_put_u32le(code, 0);
    (void)wl_peer_send(a->peer, ch, WL_EXEC_EXIT, code, sizeof(code));
    (void)wl_peer_close(a->peer, ch);
    a->stopping = 1;
    log_line(a, "stopping at the client's request", "");
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
        agent_command(a, ch, cmd_z);
        free(cmd_z);
        free(cwd_z);
        free(e->stdin_q);
        e->stdin_q = NULL;
        return;
    }
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
        end_xfer(x, 0);
        return;
    }
    x->active = 1;
    log_line(a, "put: ", x->path);
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
        end_xfer(x, 0);
        return;
    }
    wl_put_u64le(meta, x->size);
    wl_put_u64le(meta + 8, x->mtime);
    x->active = 1;
    if (wl_peer_send(a->peer, ch, WL_FILE_META, meta, sizeof(meta)) != WL_OK) {
        end_xfer(x, 0);
        return;
    }
    log_line(a, "get: ", x->path);
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
    if (x->done != x->size) {
        xfer_fail(a, x, WL_E_CRC, "wrong number of bytes received");
        return;
    }
    if (x->crc != want_crc) {
        xfer_fail(a, x, WL_E_CRC, "checksum mismatch");
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

    if (x != NULL) {
        on_xfer_record(a, x, type, p, n);
        return;
    }
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
    struct xfer *x = find_xfer(a, ch);

    /* The client hung up on a running command (Ctrl-C on its side). */
    if (e != NULL && !e->close_sent) {
        (void)wl_peer_close(a->peer, ch);
        end_exec(e);
    }
    if (x != NULL) {
        (void)wl_peer_close(a->peer, ch);
        end_xfer(x, 0);
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

/* Streams a GET out, and closes either kind of transfer once it is over. */
static void service_xfer(struct agent *a, struct xfer *x)
{
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
        end_xfer(x, 1);
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
        for (unsigned i = 0; i < WL_AGENT_MAX_EXEC; i++) {
            if (a->execs[i].active)
                service_exec(a, &a->execs[i]);
        }
        for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
            if (a->xfers[i].active)
                service_xfer(a, &a->xfers[i]);
        }
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
            end_exec(&a->execs[i]);
    }
    for (unsigned i = 0; i < WL_AGENT_MAX_XFER; i++) {
        if (a->xfers[i].active)
            end_xfer(&a->xfers[i], 0);
    }
    wl_peer_free(a->peer);
    free(a);
    return st;
}
