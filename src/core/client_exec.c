#include <string.h>

#include "w98link/client_exec.h"
#include "w98link/clock.h"
#include "w98link/proto.h"
#include "w98link/wire.h"

#define PUMP_MS 50U
/* After EXIT, how long to keep pumping so our CLOSE and acks reach the
 * agent before the process exits and the port closes. */
#define LINGER_MS 2000U

enum phase { PH_CONNECT, PH_OPENING, PH_RUNNING, PH_DONE, PH_FAILED };

struct client {
    struct wl_peer *peer;
    const struct wl_exec_io *io;
    struct wl_exec_result *out;
    enum phase phase;
    uint16_t ch;
    int sessions;
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
