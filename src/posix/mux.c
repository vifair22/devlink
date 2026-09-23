#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "devlink/clock.h"
#include "devlink/mux.h"
#include "devlink/proto.h"
#include "devlink/usock.h"
#include "devlink/wire.h"

#define MUX_CLIENTS 8U
#define MUX_PAIRS 12U
#define LOOP_MS 20U

/*
 * Records that could not be handed on because the other side's queue was full,
 * kept in arrival order until it takes them.
 *
 * One would be enough if the source could be stopped the moment it refused,
 * and the loop does stop pumping a peer whose records are not being taken. But
 * records are dispatched from inside a pump, several per call, so by the time
 * the first one is held here the rest of that pump's burst is already on its
 * way. Keeping one and overwriting it lost a 4 KiB chunk out of the middle of
 * a file, and said nothing: a large, highly compressible transfer failed its
 * checksum because the line delivers such a file far faster than the far side
 * drains it.
 *
 * What one pump can deliver is bounded. wl_peer_work drains the session's
 * plaintext queue and never refills it part way through, so a single pump
 * dispatches less than WL_SESSION_QUEUE, and no further pump happens while
 * anything is held here. That bound is the cap below. Reaching it would mean
 * the bound had stopped being true, which is a defect and not a busy line, so
 * it ends the channel with a message rather than quietly losing a record.
 */
#define STASH_CAP WL_SESSION_QUEUE
#define STASH_FIRST 8192U
#define STASH_HEADER 3U /* type, then the payload length */

struct stash {
    uint8_t *buf;
    size_t cap;
    size_t head; /* first byte not yet handed on */
    size_t len;  /* bytes held from head */
};

/* One channel of one client, and the channel it was relayed onto. */
struct pair {
    int used;
    unsigned client;
    unsigned client_serial;
    uint16_t local;
    uint16_t up;
    int up_open;  /* the agent accepted it */
    int pending;  /* opened upstream, answer not seen yet */
    /*
     * Opened by the client while the line was down, so it is held here until
     * the session comes back rather than refused. Refusing was wrong twice
     * over: a client that connects the instant the multiplexer starts has done
     * nothing wrong, and neither has one whose line happens to be
     * resynchronising at that moment.
     */
    int waiting;
    int up_closed;   /* the agent closed its end */
    int down_closed; /* the client closed its end */
    /*
     * A close can be refused (WL_ERR_NOSPACE) while the channel still has
     * records queued, so what matters is not that it was attempted but that it
     * was sent. Freeing a pair whose close never went out holds the channel
     * number for the life of the multiplexer, which is exactly the leak this
     * file had.
     */
    int up_close_sent;
    int down_close_sent;
    uint8_t kind;
    uint8_t *args;
    size_t args_len;
    uint32_t waiting_since;
    struct stash to_up;
    struct stash to_down;
};

/* How long a held open waits for the line before the client is told. The
 * client has no timeout of its own once its handshake is done, so this is what
 * stops it waiting for ever. */
#define WAIT_FOR_LINE_MS 15000U

struct client {
    int used;
    struct wl_transport t;
    struct wl_peer *peer;
    struct mux *m;
    unsigned index;
    /* Slots are reused, so a pair that outlives its client would otherwise
     * find a stranger in the same slot and close one of its channels. */
    unsigned serial;
};

struct mux {
    const struct wl_mux_cfg *cfg;
    struct wl_transport *line;
    struct wl_peer *up;
    struct wl_hello agent;
    int up_ready;
    struct client clients[MUX_CLIENTS];
    struct pair pairs[MUX_PAIRS];
    uint32_t seed;
    unsigned next_serial;
    uint32_t alone_since; /* when the last client left, for the idle timer */
};

static void log_line(struct mux *m, const char *prefix, const char *detail)
{
    char buf[512];

    if (m->cfg->log == NULL)
        return;
    (void)snprintf(buf, sizeof(buf), "%s%s", prefix, detail);
    m->cfg->log(m->cfg->log_ctx, buf);
}

/* ---------------------------------------------------------------------------
 * pairs
 * ------------------------------------------------------------------------- */

static struct pair *pair_by_local(struct mux *m, unsigned client, uint16_t ch)
{
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        if (m->pairs[i].used && m->pairs[i].client == client && m->pairs[i].local == ch)
            return &m->pairs[i];
    }
    return NULL;
}

static struct pair *pair_by_up(struct mux *m, uint16_t ch)
{
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        if (m->pairs[i].used && (m->pairs[i].up_open || m->pairs[i].pending) &&
            m->pairs[i].up == ch)
            return &m->pairs[i];
    }
    return NULL;
}

/* Defined below: the teardown is used by the handlers above it. */
static void try_finish(struct mux *m, struct pair *p);

static struct client *pair_client(struct mux *m, const struct pair *p)
{
    struct client *c = &m->clients[p->client];

    if (!c->used || c->serial != p->client_serial)
        return NULL;
    return c;
}

static int stash_waiting(const struct stash *s)
{
    return s->len > 0;
}

static void stash_clear(struct stash *s)
{
    s->head = 0;
    s->len = 0;
}

static void stash_free(struct stash *s)
{
    free(s->buf);
    s->buf = NULL;
    s->cap = 0;
    stash_clear(s);
}

/* Keeps a record for the direction that refused it. Zero means it would not
 * fit, which the caller must treat as a failure and never as a discard. */
static int stash_put(struct stash *s, uint8_t type, const uint8_t *data, size_t n)
{
    size_t need = STASH_HEADER + n;

    if (need > STASH_CAP - s->len)
        return 0;
    if (s->cap - s->head - s->len < need) {
        /* Reclaim what has been handed on before asking for more memory. */
        if (s->head > 0) {
            memmove(s->buf, s->buf + s->head, s->len);
            s->head = 0;
        }
        while (s->cap - s->len < need) {
            size_t want = s->cap == 0 ? STASH_FIRST : s->cap * 2U;
            uint8_t *bigger;

            if (want > STASH_CAP)
                want = STASH_CAP;
            bigger = realloc(s->buf, want);
            if (bigger == NULL)
                return 0;
            s->buf = bigger;
            s->cap = want;
        }
    }
    s->buf[s->head + s->len] = type;
    wl_put_u16le(s->buf + s->head + s->len + 1, (uint16_t)n);
    if (n > 0)
        memcpy(s->buf + s->head + s->len + STASH_HEADER, data, n);
    s->len += need;
    return 1;
}

/* The record at the front, which stays there until stash_drop takes it. */
static int stash_front(const struct stash *s, uint8_t *type, const uint8_t **data, size_t *n)
{
    if (s->len == 0)
        return 0;
    *type = s->buf[s->head];
    *n = wl_get_u16le(s->buf + s->head + 1);
    *data = s->buf + s->head + STASH_HEADER;
    return 1;
}

static void stash_drop(struct stash *s)
{
    size_t n = wl_get_u16le(s->buf + s->head + 1);

    s->head += STASH_HEADER + n;
    s->len -= STASH_HEADER + n;
    if (s->len == 0)
        s->head = 0;
}

static void pair_free(struct pair *p)
{
    free(p->args);
    stash_free(&p->to_up);
    stash_free(&p->to_down);
    memset(p, 0, sizeof(*p));
}


/* ---------------------------------------------------------------------------
 * handlers on the line
 * ------------------------------------------------------------------------- */

static void up_session(void *ctx)
{
    struct mux *m = ctx;

    /*
     * The line resynchronised, so every channel on it is gone. The clients'
     * own sessions are untouched, but their commands are not coming back, so
     * each is told its channel has closed and left to report that as it sees
     * fit.
     */
    m->up_ready = 0;
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        struct pair *p = &m->pairs[i];

        if (!p->used)
            continue;
        /* The upstream channels went with the session, so there is nothing
         * left to close over there and no number to hold. The client's side
         * still has to be closed, and try_finish will keep trying. */
        p->up_open = 0;
        p->pending = 0;
        p->up_closed = 1;
        p->up_close_sent = 1;
        try_finish(m, p);
    }
    log_line(m, "line: session restarted", "");
}

static void up_ready(void *ctx, const struct wl_hello *h)
{
    struct mux *m = ctx;

    m->agent = *h;
    m->up_ready = 1;
    log_line(m, "line: agent ", h->build);
}

static void up_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n)
{
    struct mux *m = ctx;
    struct pair *p = pair_by_up(m, ch);
    char text[256];

    if (p == NULL || !p->pending)
        return;
    p->pending = 0;
    p->up_open = ok ? 1 : 0;
    if (pair_client(m, p) == NULL) {
        /* Its client left while the agent was deciding; the channel it just
         * granted has to be handed back. */
        p->down_closed = 1;
        p->up_closed = !ok;
        try_finish(m, p);
        return;
    }
    if (ok) {
        if (wl_peer_accept(m->clients[p->client].peer, p->local) != WL_OK) {
            p->down_closed = 1;
            try_finish(m, p);
        }
        return;
    }
    if (n >= sizeof(text))
        n = sizeof(text) - 1;
    memcpy(text, msg, n);
    text[n] = '\0';
    (void)wl_peer_reject(m->clients[p->client].peer, p->local, code, text);
    pair_free(p);
}

/*
 * The backlog would not take a record. That means more arrived in one pump
 * than the bound above allows for, so the buffering that keeps this honest has
 * stopped being enough. Truncating the transfer silently is the one thing that
 * must not happen, so the channel ends here and says why; everything else on
 * the line carries on.
 */
static void overflowed(struct mux *m, struct pair *p, const char *where)
{
    char note[96];

    (void)snprintf(note, sizeof(note), "%s backlog full on channel %u; ending it", where,
                   (unsigned)p->local);
    log_line(m, "mux: ", note);
    stash_clear(&p->to_up);
    stash_clear(&p->to_down);
    p->up_closed = 1;
    p->down_closed = 1;
    try_finish(m, p);
}

static void up_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *data, size_t n)
{
    struct mux *m = ctx;
    struct pair *p = pair_by_up(m, ch);

    if (p == NULL || !m->clients[p->client].used)
        return;
    /* Anything already waiting goes first, or this record would overtake it. */
    if (stash_waiting(&p->to_down)
        || wl_peer_send(m->clients[p->client].peer, p->local, type, data, n) != WL_OK) {
        if (!stash_put(&p->to_down, type, data, n))
            overflowed(m, p, "downward");
    }
}

/*
 * Finishes a channel in both directions, and is safe to call again until it
 * succeeds. Three things had to be true and were not:
 *
 * - Our own end of the upstream channel has to be closed as well as the
 *   client's. A channel number is released only once both sides have closed
 *   it, so answering the agent's CLOSE without sending ours held the number
 *   for the life of the multiplexer. It leaked only when the agent's CLOSE won
 *   the race against the client's, which is why it appeared as one stuck
 *   channel rather than one per command.
 * - A close can be refused while the channel still has records queued, so the
 *   pair may not be freed until the close has actually gone out. Attempting it
 *   once and freeing regardless is the same leak with extra steps.
 * - A channel awaiting its OPEN_OK cannot be closed at all, so that case waits
 *   for the answer rather than being dropped.
 */
static void try_finish(struct mux *m, struct pair *p)
{
    struct client *c = pair_client(m, p);

    if (c == NULL) {
        /* Nobody is waiting for what was held for the client. */
        stash_clear(&p->to_down);
        stash_clear(&p->to_up);
    }
    if (stash_waiting(&p->to_up) || stash_waiting(&p->to_down))
        return; /* flushed first, so nothing in flight is lost */
    if (p->pending)
        return; /* cannot close a channel that has no answer to its OPEN yet */
    if (p->up_open && !p->up_close_sent) {
        if (wl_peer_close(m->up, p->up) != WL_OK)
            return;
        p->up_close_sent = 1;
    }
    if (c != NULL && !p->down_closed && !p->down_close_sent) {
        if (wl_peer_close(c->peer, p->local) != WL_OK)
            return;
        p->down_close_sent = 1;
    }
    pair_free(p);
}

static void up_closed(void *ctx, uint16_t ch)
{
    struct mux *m = ctx;
    struct pair *p = pair_by_up(m, ch);

    if (p == NULL)
        return;
    p->up_closed = 1;
    try_finish(m, p);
}

static void up_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct mux *m = ctx;
    char text[256];

    (void)code;
    if (n >= sizeof(text))
        n = sizeof(text) - 1;
    memcpy(text, msg, n);
    text[n] = '\0';
    log_line(m, "line error: ", text);
}

/* ---------------------------------------------------------------------------
 * handlers on a client's socket
 * ------------------------------------------------------------------------- */

static void down_open(void *ctx, uint16_t ch, uint8_t kind, const uint8_t *args, size_t n)
{
    struct client *c = ctx;
    struct mux *m = c->m;
    struct pair *p = NULL;

    for (unsigned i = 0; i < MUX_PAIRS && p == NULL; i++) {
        if (!m->pairs[i].used)
            p = &m->pairs[i];
    }
    if (p == NULL) {
        (void)wl_peer_reject(c->peer, ch, WL_E_PROCESS, "too many channels open");
        return;
    }
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->client = c->index;
    p->client_serial = c->serial;
    p->local = ch;
    if (!m->up_ready) {
        p->args = malloc(n > 0 ? n : 1);
        if (p->args == NULL) {
            (void)wl_peer_reject(c->peer, ch, WL_E_PROCESS, "agent out of memory");
            pair_free(p);
            return;
        }
        memcpy(p->args, args, n);
        p->args_len = n;
        p->kind = kind;
        p->waiting = 1;
        p->waiting_since = wl_clock_ms();
        return;
    }
    if (wl_peer_open(m->up, kind, args, n, &p->up) != WL_OK) {
        (void)wl_peer_reject(c->peer, ch, WL_E_PROCESS, wl_error());
        pair_free(p);
        return;
    }
    p->pending = 1;
}

/* Opens what was held while the line was down, and gives up on anything that
 * has waited too long. */
static void open_the_waiting(struct mux *m)
{
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        struct pair *p = &m->pairs[i];

        if (!p->used || !p->waiting)
            continue;
        if (pair_client(m, p) == NULL) {
            pair_free(p);
            continue;
        }
        if (m->up_ready) {
            if (wl_peer_open(m->up, p->kind, p->args, p->args_len, &p->up) != WL_OK)
                continue; /* the mux is full for the moment; try again */
            p->waiting = 0;
            p->pending = 1;
            free(p->args);
            p->args = NULL;
            p->args_len = 0;
        } else if (wl_clock_since(p->waiting_since) >= WAIT_FOR_LINE_MS) {
            (void)wl_peer_reject(m->clients[p->client].peer, p->local, WL_E_UNKNOWN,
                                 "the line did not come up");
            pair_free(p);
        }
    }
}

static void down_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *data, size_t n)
{
    struct client *c = ctx;
    struct pair *p = pair_by_local(c->m, c->index, ch);

    if (p == NULL || !p->up_open)
        return;
    if (stash_waiting(&p->to_up)
        || wl_peer_send(c->m->up, p->up, type, data, n) != WL_OK) {
        if (!stash_put(&p->to_up, type, data, n))
            overflowed(c->m, p, "upward");
    }
}

static void down_closed(void *ctx, uint16_t ch)
{
    struct client *c = ctx;
    struct pair *p = pair_by_local(c->m, c->index, ch);

    if (p == NULL)
        return;
    p->down_closed = 1;
    try_finish(c->m, p);
}

/*
 * Retries every teardown that could not complete: a stash that had not
 * drained, a close the mux refused because records were still queued, a
 * channel that had no answer to its OPEN yet. Without this pass the comment
 * explaining why the close was deferred was the only thing that ever finished
 * it.
 */
static void finish_closes(struct mux *m)
{
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        struct pair *p = &m->pairs[i];

        if (!p->used)
            continue;
        if (!p->up_closed && !p->down_closed && pair_client(m, p) != NULL)
            continue;
        try_finish(m, p);
    }
}

static void down_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct client *c = ctx;

    (void)code;
    (void)msg;
    (void)n;
    c->m->clients[c->index].used = 0; /* dropped below, with its channels */
}

/* ---------------------------------------------------------------------------
 * clients
 * ------------------------------------------------------------------------- */

static unsigned count_pairs(const struct mux *m)
{
    unsigned n = 0;

    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        if (m->pairs[i].used)
            n++;
    }
    return n;
}

static void drop_client(struct mux *m, unsigned index)
{
    struct client *c = &m->clients[index];

    /*
     * The client is gone, so its commands are nobody's: closing their channels
     * on the line is what makes the agent kill them. Concurrency without
     * detach means exactly this.
     *
     * The client's slot is released first and the pairs are left for
     * try_finish, because the close upstream may be refused this moment and
     * has to be retried; freeing the pair here was how the number leaked.
     */
    wl_peer_free(c->peer);
    wl_transport_close(&c->t);
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        struct pair *p = &m->pairs[i];

        if (p->used && p->client == index)
            try_finish(m, p);
    }
    log_line(m, "client gone", "");
}

static void accept_client(struct mux *m, int listen_fd)
{
    struct wl_transport t;
    struct wl_peer_handlers h;
    struct client *c = NULL;
    struct wl_session_cfg cfg;

    if (wl_unix_accept(listen_fd, &t) != WL_OK) {
        log_line(m, "accept failed: ", wl_error());
        return;
    }
    for (unsigned i = 0; i < MUX_CLIENTS && c == NULL; i++) {
        if (!m->clients[i].used)
            c = &m->clients[i];
    }
    if (c == NULL) {
        log_line(m, "refused a client: too many already", "");
        wl_transport_close(&t);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->index = (unsigned)(c - m->clients);
    c->serial = ++m->next_serial;
    c->t = t;
    c->m = m;
    memset(&h, 0, sizeof(h));
    h.ctx = c;
    h.open = down_open;
    h.record = down_record;
    h.closed = down_closed;
    h.error = down_error;
    /* The socket is reliable and local, so there is nothing to pace and no
     * reason to spend effort compressing what never leaves the machine. */
    cfg = m->cfg->session;
    cfg.link.bytes_per_sec = 0;
    cfg.level = 0;
    c->peer = wl_peer_new(&cfg, m->seed++, wl_clock_ms(), 0, WL_PLATFORM_LINUX, &h);
    if (c->peer == NULL) {
        wl_transport_close(&c->t);
        memset(c, 0, sizeof(*c));
        return;
    }
    /* What the client asked about is the agent, not this relay. */
    wl_peer_set_identity(c->peer, m->agent.platform, m->agent.build);
    c->used = 1;
    {
        /*
         * The leak detector, sampled here rather than as a client leaves: a
         * channel is released only once both sides have closed it, so a count
         * taken just after we close our own side always reads one and proves
         * nothing. What matters is what the previous clients left behind,
         * which is what is standing here before this one has done anything.
         */
        char note[80];

        (void)snprintf(note, sizeof(note), "client connected (line channels held: %u, pairs: %u)",
                       wl_peer_channels_open(m->up), count_pairs(m));
        log_line(m, note, "");
    }
}

/* Tries the stashed records again; returns nonzero if any is still waiting. */
static int flush_stashes(struct mux *m, int *block_up, int *block_down)
{
    int waiting = 0;

    *block_up = 0;
    memset(block_down, 0, sizeof(int) * MUX_CLIENTS);
    for (unsigned i = 0; i < MUX_PAIRS; i++) {
        struct pair *p = &m->pairs[i];

        if (!p->used)
            continue;
        while (stash_waiting(&p->to_up)) {
            const uint8_t *data;
            uint8_t type;
            size_t n;

            if (!p->up_open || !stash_front(&p->to_up, &type, &data, &n)
                || wl_peer_send(m->up, p->up, type, data, n) != WL_OK) {
                block_down[p->client] = 1; /* its source must wait */
                break;
            }
            stash_drop(&p->to_up);
        }
        if (m->clients[p->client].used) {
            while (stash_waiting(&p->to_down)) {
                const uint8_t *data;
                uint8_t type;
                size_t n;

                if (!stash_front(&p->to_down, &type, &data, &n)
                    || wl_peer_send(m->clients[p->client].peer, p->local, type, data, n)
                           != WL_OK) {
                    *block_up = 1;
                    break;
                }
                stash_drop(&p->to_down);
            }
        } else {
            stash_clear(&p->to_down); /* nobody left to give it to */
        }
        waiting = waiting || stash_waiting(&p->to_up) || stash_waiting(&p->to_down);
    }
    return waiting;
}

/*
 * The socket that stands for this line.
 *
 * Taken from the device the path resolves to rather than the path as it was
 * typed, because one cable has several names: /dev/ttyUSB0 and the
 * /dev/serial/by-id link pointing at it are the same port. Naming the socket
 * after each gave two sockets, so a client using the other name started a
 * second multiplexer, which could not then open the port the first one held.
 * It failed safely and said who had it, but the answer was to stop and use
 * the other name, which is not something anyone should have to know.
 *
 * A path that will not resolve is used as given: the multiplexer can be
 * started before the cable is plugged in, and refusing to name a socket for it
 * would be worse than naming one that a later resolve agrees with anyway.
 */
int wl_mux_socket_path(const char *line, char *out, size_t cap)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char resolved[PATH_MAX];
    const char *use = line;
    const char *base;
    int n;

    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    if (realpath(line, resolved) != NULL)
        use = resolved;
    base = strrchr(use, '/');
    base = base != NULL ? base + 1 : use;
    n = snprintf(out, cap, "%s/devlink-%s.sock", dir, base);
    return n > 0 && (size_t)n < cap;
}

wl_status wl_mux_serve(struct wl_transport *line, const struct wl_mux_cfg *cfg, uint32_t seed)
{
    struct mux *m = calloc(1, sizeof(*m));
    struct wl_peer_handlers h;
    wl_status st = WL_OK;
    int listen_fd = -1;

    if (m == NULL)
        return wl_fail(WL_ERR_NOMEM, "mux: out of memory");
    m->cfg = cfg;
    m->line = line;
    m->seed = seed;
    memset(&h, 0, sizeof(h));
    h.ctx = m;
    h.session = up_session;
    h.ready = up_ready;
    h.opened = up_opened;
    h.record = up_record;
    h.closed = up_closed;
    h.error = up_error;
    m->up = wl_peer_new(&cfg->session, seed, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    if (m->up == NULL) {
        free(m);
        return WL_ERR_NOMEM;
    }
    st = wl_unix_listen(cfg->socket_path, &listen_fd);
    if (st != WL_OK) {
        wl_peer_free(m->up);
        free(m);
        return st;
    }
    log_line(m, "listening on ", cfg->socket_path);
    m->alone_since = wl_clock_ms();

    while (st == WL_OK) {
        struct pollfd fds[MUX_CLIENTS + 2];
        unsigned nfds = 0;
        int block_up = 0;
        int block_down[MUX_CLIENTS];
        int line_fd = wl_transport_fd(line);
        unsigned clients = 0;

        if (cfg->stop != NULL && cfg->stop(cfg->stop_ctx))
            break;
        (void)flush_stashes(m, &block_up, block_down);

        fds[nfds].fd = listen_fd;
        fds[nfds].events = POLLIN;
        fds[nfds++].revents = 0;
        if (line_fd >= 0) {
            fds[nfds].fd = line_fd;
            fds[nfds].events = POLLIN;
            fds[nfds++].revents = 0;
        }
        for (unsigned i = 0; i < MUX_CLIENTS; i++) {
            if (!m->clients[i].used)
                continue;
            clients++;
            fds[nfds].fd = wl_transport_fd(&m->clients[i].t);
            fds[nfds].events = POLLIN;
            fds[nfds++].revents = 0;
        }
        if (clients == 0 && cfg->idle_exit_ms != 0 &&
            wl_clock_since(m->alone_since) >= cfg->idle_exit_ms) {
            log_line(m, "idle, stopping", "");
            break;
        }
        if (clients > 0)
            m->alone_since = wl_clock_ms();
        (void)poll(fds, nfds, (int)LOOP_MS);
        /*
         * Connections are left in the listen backlog until the line is up.
         * Accepting sooner would hand the client a HELLO with no agent
         * identity in it, because the agent's own HELLO has not arrived yet:
         * the client would be told it was talking to platform 0 with no build.
         * Waiting costs the client nothing, since its connect is already
         * complete and it is simply not served for a few milliseconds.
         */
        if (fds[0].revents != 0 && m->up_ready)
            accept_client(m, listen_fd);
        open_the_waiting(m);
        finish_closes(m);

        /* Pumped with no wait: the poll above is the only place this sleeps. A
         * peer whose records cannot be handed on is left alone, which stops its
         * sender rather than losing anything. */
        if (!block_up) {
            st = wl_peer_pump(m->up, line, 0);
            if (st != WL_OK) {
                log_line(m, "line failed: ", wl_error());
                break;
            }
        }
        for (unsigned i = 0; i < MUX_CLIENTS; i++) {
            if (!m->clients[i].used || block_down[i])
                continue;
            if (wl_peer_pump(m->clients[i].peer, &m->clients[i].t, 0) != WL_OK)
                drop_client(m, i);
        }
    }

    for (unsigned i = 0; i < MUX_CLIENTS; i++) {
        if (m->clients[i].used)
            drop_client(m, i);
    }
    (void)close(listen_fd);
    (void)unlink(cfg->socket_path);
    wl_peer_free(m->up);
    free(m);
    return st;
}
