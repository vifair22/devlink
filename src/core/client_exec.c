#include <string.h>

#include "devlink/client_exec.h"
#include "devlink/clock.h"
#include "devlink/proto.h"
#include "devlink/wire.h"

#define PUMP_MS 50U
/* After EXIT, how long to keep pumping so our CLOSE and acks reach the
 * agent before the process exits and the port closes. */
#define LINGER_MS 2000U
/* How long to wait for the agent to acknowledge a cancel before giving up on
 * it. Long enough for a round trip on a slow line and a wedged agent's loop,
 * short enough that a second Ctrl-C is not what ends the client. */
#define CANCEL_MS 3000U

enum phase { PH_CONNECT, PH_OPENING, PH_RUNNING, PH_DONE, PH_FAILED };

struct client {
    struct wl_peer *peer;
    const struct wl_exec_io *io;
    struct wl_exec_result *out;
    enum phase phase;
    uint16_t ch;
    int sessions;
    int cancel_sent;
    uint32_t cancel_at;
};

static void set_message(struct wl_exec_result *out, const char *msg, size_t n)
{
    if (n >= sizeof(out->message))
        n = sizeof(out->message) - 1;
    memcpy(out->message, msg, n);
    out->message[n] = '\0';
}

static void on_session(void *ctx)
{
    struct client *c = ctx;

    /* A second session means the link reset under a running command; its
     * output is incomplete, so the command has failed from our side. */
    if (++c->sessions > 1 && c->phase != PH_DONE) {
        c->phase = PH_FAILED;
        set_message(c->out, "link reset during the command", 29);
    }
}

static void on_ready(void *ctx, const struct wl_hello *h)
{
    struct client *c = ctx;

    c->out->agent = *h;
}

static void on_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n)
{
    struct client *c = ctx;

    if (ch != c->ch || c->phase != PH_OPENING)
        return;
    if (!ok) {
        c->out->fail_code = code;
        set_message(c->out, msg, n);
        c->phase = PH_FAILED;
        return;
    }
    c->out->ran = 1;
    c->phase = PH_RUNNING;
    /* v1 is non-interactive: the command gets an empty stdin. */
    (void)wl_peer_send(c->peer, ch, WL_EXEC_STDIN_EOF, NULL, 0);
}

static void on_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct client *c = ctx;

    if (ch != c->ch)
        return;
    if ((type == WL_EXEC_STDOUT || type == WL_EXEC_STDERR) && c->io->output != NULL) {
        c->io->output(c->io->ctx, type, p, n);
    } else if (type == WL_EXEC_EXIT && n == 4) {
        c->out->exit_code = wl_get_u32le(p);
        if (c->cancel_sent)
            c->out->cancel_acked = 1;
        c->phase = PH_DONE;
    }
}

static void on_closed(void *ctx, uint16_t ch)
{
    struct client *c = ctx;

    if (ch == c->ch && c->phase == PH_RUNNING) {
        c->phase = PH_FAILED;
        set_message(c->out, "agent closed the command without an exit status", 48);
    }
}

static void on_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct client *c = ctx;

    c->out->fail_code = code;
    set_message(c->out, msg, n);
    c->phase = PH_FAILED;
}

struct info_ctx {
    struct wl_hello *out;
    int ready;
    int failed;
};

static void info_ready(void *ctx, const struct wl_hello *h)
{
    struct info_ctx *c = ctx;

    *c->out = *h;
    c->ready = 1;
}

static void info_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct info_ctx *c = ctx;

    (void)code;
    (void)msg;
    (void)n;
    c->failed = 1;
}

wl_status wl_client_info(struct wl_transport *t, const struct wl_session_cfg *cfg,
                         uint32_t seed, uint32_t timeout_ms, struct wl_hello *out)
{
    struct info_ctx c;
    struct wl_peer_handlers h;
    struct wl_peer *p;
    uint32_t start = wl_clock_ms();
    wl_status st = WL_OK;

    memset(out, 0, sizeof(*out));
    memset(&c, 0, sizeof(c));
    c.out = out;
    memset(&h, 0, sizeof(h));
    h.ctx = &c;
    h.ready = info_ready;
    h.error = info_error;
    p = wl_peer_new(cfg, seed, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    if (p == NULL)
        return WL_ERR_NOMEM;
    while (!c.ready && !c.failed && st == WL_OK) {
        st = wl_peer_pump(p, t, PUMP_MS);
        if (wl_clock_since(start) >= timeout_ms) {
            st = wl_fail(WL_ERR_IO, "no answer from the agent within %u s",
                         (unsigned)(timeout_ms / 1000U));
            break;
        }
    }
    if (st == WL_OK && c.failed)
        st = wl_fail(WL_ERR_IO, "the agent refused the connection");
    wl_peer_free(p);
    return st;
}

wl_status wl_client_exec(struct wl_transport *t, const struct wl_session_cfg *cfg,
                         uint32_t seed, const char *cmd, const char *cwd, uint8_t flags,
                         const struct wl_exec_io *io, uint32_t connect_timeout_ms,
                         struct wl_exec_result *out)
{
    struct client c;
    struct wl_peer_handlers h;
    uint8_t args[WL_REC_MAX_PAYLOAD];
    struct wl_wbuf w;
    uint32_t start = wl_clock_ms();
    wl_status st = WL_OK;

    memset(out, 0, sizeof(*out));
    memset(&c, 0, sizeof(c));
    c.io = io;
    c.out = out;
    c.phase = PH_CONNECT;

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, cmd, strlen(cmd));
    wl_w_str(&w, cwd != NULL ? cwd : "", cwd != NULL ? strlen(cwd) : 0);
    wl_w_u8(&w, flags);
    if (w.overflow)
        return wl_fail(WL_ERR_INVAL, "exec: command too long");

    memset(&h, 0, sizeof(h));
    h.ctx = &c;
    h.session = on_session;
    h.ready = on_ready;
    h.opened = on_opened;
    h.record = on_record;
    h.closed = on_closed;
    h.error = on_error;
    c.peer = wl_peer_new(cfg, seed, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    if (c.peer == NULL)
        return WL_ERR_NOMEM;

    while (c.phase != PH_DONE && c.phase != PH_FAILED) {
        st = wl_peer_pump(c.peer, t, PUMP_MS);
        if (st != WL_OK)
            break;
        if (io->cancel != NULL && *io->cancel != 0) {
            /*
             * Before the command is running there is nothing over there to
             * kill, so the caller is obeyed immediately. Once it is running,
             * CANCEL goes out once and the agent's answer is waited for:
             * leaving without it would abandon a process on the target.
             */
            if (c.phase != PH_RUNNING) {
                out->cancelled = 1;
                break;
            }
            if (!c.cancel_sent &&
                wl_peer_send(c.peer, c.ch, WL_EXEC_CANCEL, NULL, 0) == WL_OK) {
                c.cancel_sent = 1;
                c.cancel_at = wl_clock_ms();
                out->cancelled = 1;
            }
            if (c.cancel_sent && wl_clock_since(c.cancel_at) >= CANCEL_MS)
                break;
        }
        if (c.phase == PH_CONNECT) {
            if (wl_peer_ready(c.peer)) {
                st = wl_peer_open(c.peer, WL_KIND_EXEC, args, w.len, &c.ch);
                if (st != WL_OK)
                    break;
                c.phase = PH_OPENING;
            } else if (wl_clock_since(start) >= connect_timeout_ms) {
                st = wl_fail(WL_ERR_IO, "no answer from the agent within %u s",
                             (unsigned)(connect_timeout_ms / 1000U));
                break;
            }
        }
    }
    if (st == WL_OK && c.phase == PH_DONE) {
        uint32_t linger = wl_clock_ms();

        while (wl_peer_close(c.peer, c.ch) == WL_ERR_NOSPACE &&
               wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(c.peer, t, PUMP_MS);
        while (!wl_peer_idle(c.peer) && wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(c.peer, t, PUMP_MS);
    }
    wl_peer_free(c.peer);
    return st;
}
