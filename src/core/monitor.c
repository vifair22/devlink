#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink/clock.h"
#include "devlink/monitor.h"

struct wl_monitor {
    int state;
    char client[WL_PEER_BUILD_MAX + 1];
    struct wl_mon_row rows[WL_MON_ROWS];
    unsigned n_rows;
    struct wl_mon_client clients[WL_MON_CLIENTS];
    unsigned n_clients;
    char log[WL_MON_LOG_LINES][WL_MON_LINE_MAX];
    unsigned log_at;    /* where the next line goes */
    unsigned log_count; /* lines held, up to WL_MON_LOG_LINES */
    uint32_t gen;
    struct wl_mon_totals tot;
};

struct wl_monitor *wl_monitor_new(void)
{
    /* Around 160 KB of ring, which is why this is on the heap and not a
     * struct the caller keeps: the project holds itself to a 64 KB stack. */
    return calloc(1, sizeof(struct wl_monitor));
}

void wl_monitor_free(struct wl_monitor *m)
{
    free(m);
}

void wl_monitor_reset(struct wl_monitor *m)
{
    m->state = WL_MON_WAITING;
    m->client[0] = '\0';
    m->n_rows = 0;
    m->n_clients = 0;
    m->gen++;
}

static void copy_into(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void wl_monitor_note(struct wl_monitor *m, uint64_t local_time, const char *msg)
{
    char *line = m->log[m->log_at];
    unsigned long secs = (unsigned long)(local_time % 86400U);
    int n;

    m->tot.notes++;
    if (m->log_count == WL_MON_LOG_LINES)
        m->tot.dropped++;
    n = snprintf(line, WL_MON_LINE_MAX, "%02lu:%02lu:%02lu %s", secs / 3600UL,
                 (secs / 60UL) % 60UL, secs % 60UL, msg == NULL ? "" : msg);
    if (n < 0)
        line[0] = '\0';
    m->log_at = (m->log_at + 1U) % WL_MON_LOG_LINES;
    if (m->log_count < WL_MON_LOG_LINES)
        m->log_count++;
    m->gen++;
}

/* An event's session, with the unnamed ones (from before TCP) as serial. */
static const char *session_of(const struct wl_agent_event *e)
{
    return e->session != NULL ? e->session : "serial";
}

static struct wl_mon_row *find_row(struct wl_monitor *m, const char *session, uint16_t ch)
{
    for (unsigned i = 0; i < m->n_rows; i++) {
        if (m->rows[i].ch == ch && strcmp(m->rows[i].session, session) == 0)
            return &m->rows[i];
    }
    return NULL;
}

static struct wl_mon_client *find_client(struct wl_monitor *m, const char *name)
{
    for (unsigned i = 0; i < m->n_clients; i++) {
        if (strcmp(m->clients[i].name, name) == 0)
            return &m->clients[i];
    }
    return NULL;
}

/*
 * A row is dropped by closing the gap rather than by moving the last row into
 * it, so the list keeps the order the work started in. A display that sorts
 * itself would not care, but one that does not would jump a line under the
 * pointer every time an unrelated command finished.
 */
static void drop_row(struct wl_monitor *m, struct wl_mon_row *r)
{
    unsigned i = (unsigned)(r - m->rows);

    if (i + 1U < m->n_rows)
        memmove(r, r + 1, (m->n_rows - i - 1U) * sizeof(*r));
    m->n_rows--;
    memset(&m->rows[m->n_rows], 0, sizeof(m->rows[0]));
}

/* A session's work died with it; everyone else's carries on. */
static void drop_session_rows(struct wl_monitor *m, const char *session)
{
    unsigned i = 0;

    while (i < m->n_rows) {
        if (strcmp(m->rows[i].session, session) == 0)
            drop_row(m, &m->rows[i]);
        else
            i++;
    }
}

static void drop_client(struct wl_monitor *m, struct wl_mon_client *c)
{
    unsigned i = (unsigned)(c - m->clients);

    if (i + 1U < m->n_clients)
        memmove(c, c + 1, (m->n_clients - i - 1U) * sizeof(*c));
    m->n_clients--;
}

/* The TCP half of wl_monitor_event: the list of clients. Returns nonzero if
 * anything changed. */
static int tcp_event(struct wl_monitor *m, const struct wl_agent_event *e)
{
    const char *name = session_of(e);
    struct wl_mon_client *c = find_client(m, name);

    switch (e->type) {
    case WL_EV_CONNECTED:
        if (c != NULL || m->n_clients >= WL_MON_CLIENTS)
            return 0;
        c = &m->clients[m->n_clients++];
        memset(c, 0, sizeof(*c));
        copy_into(c->name, sizeof(c->name), name);
        return 1;
    case WL_EV_SESSION:
        m->tot.sessions++;
        return 1;
    case WL_EV_READY:
        if (c == NULL)
            return 0;
        copy_into(c->build, sizeof(c->build), e->text);
        return 1;
    case WL_EV_GONE:
    case WL_EV_DISCONNECTED:
        drop_session_rows(m, name);
        if (c != NULL)
            drop_client(m, c);
        return 1;
    default:
        return 0;
    }
}

void wl_monitor_event(struct wl_monitor *m, const struct wl_agent_event *e)
{
    const char *session = session_of(e);
    struct wl_mon_row *r;

    if (e->tcp && e->type != WL_EV_CHANNEL && e->type != WL_EV_PROGRESS &&
        e->type != WL_EV_FINISHED) {
        if (tcp_event(m, e))
            m->gen++;
        return;
    }
    switch (e->type) {
    case WL_EV_SESSION:
        /* Whatever the last client had running died with it (on_session in
         * the agent ends all of it), so its rows go here too. */
        m->state = WL_MON_LINKED;
        m->client[0] = '\0';
        drop_session_rows(m, session);
        m->tot.sessions++;
        break;
    case WL_EV_READY:
        m->state = WL_MON_SERVING;
        copy_into(m->client, sizeof(m->client), e->text);
        break;
    case WL_EV_GONE:
        m->state = WL_MON_WAITING;
        m->client[0] = '\0';
        drop_session_rows(m, session);
        break;
    case WL_EV_CHANNEL:
        /* Cannot happen with the agent's current slot counts, and if it ever
         * does, the work still runs: only this row of the display is lost. */
        if (m->n_rows >= WL_MON_ROWS)
            return;
        r = &m->rows[m->n_rows++];
        memset(r, 0, sizeof(*r));
        copy_into(r->session, sizeof(r->session), session);
        r->tcp = e->tcp;
        r->ch = e->ch;
        r->kind = e->kind;
        r->total = e->total;
        r->started_ms = wl_clock_ms();
        copy_into(r->name, sizeof(r->name), e->text);
        if (e->kind == WL_KIND_EXEC)
            m->tot.commands++;
        else
            m->tot.transfers++;
        break;
    case WL_EV_PROGRESS:
        r = find_row(m, session, e->ch);
        if (r == NULL)
            return;
        /* Counted from the start of the work, so the lifetime total moves by
         * the difference. Anything that reports going backwards is ignored
         * rather than allowed to wrap the total. */
        if (e->done > r->done)
            m->tot.bytes += e->done - r->done;
        r->done = e->done;
        if (e->total != 0)
            r->total = e->total;
        break;
    case WL_EV_FINISHED:
        r = find_row(m, session, e->ch);
        if (r == NULL)
            return;
        drop_row(m, r);
        break;
    default:
        return;
    }
    /* Only reached when something actually changed: an event about work this
     * model never saw returns above, so a display is not woken for nothing. */
    m->gen++;
}

int wl_monitor_state(const struct wl_monitor *m)
{
    return m->state;
}

const char *wl_monitor_client(const struct wl_monitor *m)
{
    return m->client;
}

uint32_t wl_monitor_generation(const struct wl_monitor *m)
{
    return m->gen;
}

unsigned wl_monitor_rows(const struct wl_monitor *m)
{
    return m->n_rows;
}

const struct wl_mon_row *wl_monitor_row(const struct wl_monitor *m, unsigned i)
{
    return i < m->n_rows ? &m->rows[i] : NULL;
}

unsigned wl_monitor_tcp_clients(const struct wl_monitor *m)
{
    return m->n_clients;
}

const struct wl_mon_client *wl_monitor_tcp_client(const struct wl_monitor *m, unsigned i)
{
    return i < m->n_clients ? &m->clients[i] : NULL;
}

unsigned wl_monitor_log_lines(const struct wl_monitor *m)
{
    return m->log_count;
}

const char *wl_monitor_log(const struct wl_monitor *m, unsigned i)
{
    unsigned oldest;

    if (i >= m->log_count)
        return NULL;
    oldest = m->log_count == WL_MON_LOG_LINES ? m->log_at : 0U;
    return m->log[(oldest + i) % WL_MON_LOG_LINES];
}

void wl_monitor_totals(const struct wl_monitor *m, struct wl_mon_totals *out)
{
    *out = m->tot;
}
