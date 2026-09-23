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

static struct wl_mon_row *find_row(struct wl_monitor *m, uint16_t ch)
{
    for (unsigned i = 0; i < m->n_rows; i++) {
        if (m->rows[i].ch == ch)
            return &m->rows[i];
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

void wl_monitor_event(struct wl_monitor *m, const struct wl_agent_event *e)
{
    struct wl_mon_row *r;

    switch (e->type) {
    case WL_EV_SESSION:
        /* Whatever the last client had running died with it (on_session in
         * the agent ends all of it), so the list starts empty here too. */
        m->state = WL_MON_LINKED;
        m->client[0] = '\0';
        m->n_rows = 0;
        m->tot.sessions++;
        break;
    case WL_EV_READY:
        m->state = WL_MON_SERVING;
        copy_into(m->client, sizeof(m->client), e->text);
        break;
    case WL_EV_GONE:
        m->state = WL_MON_WAITING;
        m->client[0] = '\0';
        m->n_rows = 0;
        break;
    case WL_EV_CHANNEL:
        /* Cannot happen with the agent's current slot counts, and if it ever
         * does, the work still runs: only this row of the display is lost. */
        if (m->n_rows >= WL_MON_ROWS)
            return;
        r = &m->rows[m->n_rows++];
        memset(r, 0, sizeof(*r));
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
        r = find_row(m, e->ch);
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
        r = find_row(m, e->ch);
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
