#include <stdio.h>
#include <string.h>

#include "devlink/tui.h"

/* Rows every layout has: the title, three of status, the two rules and the
 * bar of keys. The work and the log share what is left. */
#define FIXED_ROWS 7U

/* Home scrolls to the oldest line; render clamps this to what exists. */
#define BACK_ALL 0x7fffffffU

static const char *const help_text[] = {
    "Keys",
    "",
    "  Up, Down          scroll the log a line",
    "  PgUp, PgDn        scroll the log a page",
    "  Home, End         the oldest line; the newest, and follow from there",
    "  r                 restart the agent (init starts it again)",
    "  P                 power this machine off",
    "  B                 reboot this machine",
    "  Ctrl-L            redraw the screen",
    "  ?                 this help; any key closes it",
    "",
    "Every key that stops something asks first: y to go on, anything else",
    "to leave it. A root shell is on the second console, Alt-F2.",
};

void wl_tui_view_init(struct wl_tui_view *v)
{
    memset(v, 0, sizeof(*v));
}

int wl_tui_key(struct wl_tui_view *v, int key)
{
    if (v->asking != 0) {
        int asked = v->asking;

        v->asking = 0;
        if (key != 'y' && key != 'Y')
            return WL_TUI_NONE;
        switch (asked) {
        case 'r':
            return WL_TUI_RESTART;
        case 'P':
            return WL_TUI_POWEROFF;
        default:
            return WL_TUI_REBOOT;
        }
    }
    if (v->help) {
        /* Any key closes the help; it does nothing else, so a key pressed to
         * dismiss it cannot also restart or power anything off. */
        v->help = 0;
        return WL_TUI_NONE;
    }
    switch (key) {
    case WL_TUI_KEY_UP:
        v->back++;
        break;
    case WL_TUI_KEY_DOWN:
        v->back = v->back > 0U ? v->back - 1U : 0U;
        break;
    case WL_TUI_KEY_PGUP:
        v->back += v->page > 0U ? v->page : 1U;
        break;
    case WL_TUI_KEY_PGDN:
        v->back = v->back > v->page ? v->back - v->page : 0U;
        break;
    case WL_TUI_KEY_HOME:
        v->back = BACK_ALL;
        break;
    case WL_TUI_KEY_END:
        v->back = 0;
        break;
    case 'r':
    case 'P':
    case 'B':
        v->asking = key;
        break;
    case '?':
        v->help = 1;
        break;
    case 12: /* Ctrl-L */
        return WL_TUI_REDRAW;
    default:
        break;
    }
    return WL_TUI_NONE;
}

/* ---------------------------------------------------------------------------
 * the grid
 * ------------------------------------------------------------------------- */

static void fill(struct wl_tui_grid *g, unsigned row, uint8_t attr)
{
    memset(g->ch[row], ' ', g->w);
    memset(g->attr[row], attr, g->w);
}

/* Writes at most max characters of s from col, never past the row's end, and
 * returns the column after them. Anything outside printable ASCII shows as
 * '?': a file name from a disk can hold anything, and a control byte on a
 * terminal is a command. */
static unsigned put_max(struct wl_tui_grid *g, unsigned row, unsigned col, const char *s,
                        size_t max, uint8_t attr)
{
    unsigned end;

    if (col >= g->w)
        return col;
    end = col + (max < (size_t)(g->w - col) ? (unsigned)max : g->w - col);

    for (; *s != '\0' && col < end; s++, col++) {
        unsigned char c = (unsigned char)*s;

        g->ch[row][col] = c >= 0x20U && c < 0x7fU ? (char)c : '?';
        g->attr[row][col] = attr;
    }
    return col;
}

/* put_max with the rest of the row as the limit; NULL writes nothing. */
static unsigned put(struct wl_tui_grid *g, unsigned row, unsigned col, const char *s,
                    uint8_t attr)
{
    if (s == NULL || col >= g->w)
        return col;
    return put_max(g, row, col, s, g->w - col, attr);
}

/* s ending at the row's last column, as far left as it fits. */
static void put_right(struct wl_tui_grid *g, unsigned row, const char *s, uint8_t attr)
{
    size_t n = strlen(s);
    unsigned col = n >= g->w ? 0U : g->w - (unsigned)n;

    (void)put(g, row, col, s, attr);
}

/* "-- title ------...", the width of the screen. */
static void rule(struct wl_tui_grid *g, unsigned row, const char *title)
{
    unsigned col;

    fill(g, row, WL_TUI_NORMAL);
    memset(g->ch[row], '-', g->w);
    col = put(g, row, 3U < g->w ? 3U : g->w, title, WL_TUI_BOLD);
    if (col < g->w)
        g->ch[row][col] = ' ';
    if (g->w > 2U)
        g->ch[row][2] = ' ';
}

static int has(const char *s)
{
    return s != NULL && s[0] != '\0';
}

/* ---------------------------------------------------------------------------
 * the parts of the screen
 * ------------------------------------------------------------------------- */

static void title_bar(const struct wl_tui_info *info, struct wl_tui_grid *g)
{
    char buf[96];

    fill(g, 0, WL_TUI_INVERSE);
    (void)snprintf(buf, sizeof(buf), " devlink agent %s", has(info->version) ? info->version : "");
    (void)put(g, 0, 0, buf, WL_TUI_INVERSE);
    if (has(info->clock)) {
        (void)snprintf(buf, sizeof(buf), "%s ", info->clock);
        put_right(g, 0, buf, WL_TUI_INVERSE);
    }
}

static void status_lines(const struct wl_monitor *m, const struct wl_tui_info *info,
                         struct wl_tui_grid *g)
{
    unsigned clients = wl_monitor_tcp_clients(m);
    char who[WL_MON_SESSION_MAX + 16];
    char buf[WL_TUI_MAX_W + 1];
    unsigned col;

    fill(g, 1, WL_TUI_NORMAL);
    col = put(g, 1, 1, "serial ", WL_TUI_BOLD);
    (void)snprintf(buf, sizeof(buf), "%s: ", has(info->serial) ? info->serial : "none");
    col = put(g, 1, col, buf, WL_TUI_NORMAL);
    switch (wl_monitor_state(m)) {
    case WL_MON_LINKED:
        (void)put(g, 1, col, "a client is connecting", WL_TUI_NORMAL);
        break;
    case WL_MON_SERVING:
        (void)snprintf(buf, sizeof(buf), "serving %s", wl_monitor_client(m));
        (void)put(g, 1, col, buf, WL_TUI_BOLD);
        break;
    default:
        (void)put(g, 1, col, "waiting for a client", WL_TUI_NORMAL);
        break;
    }

    fill(g, 2, WL_TUI_NORMAL);
    col = put(g, 2, 1, "TCP ", WL_TUI_BOLD);
    if (!info->tcp_on)
        (void)snprintf(who, sizeof(who), "off");
    else if (clients == 0U)
        (void)snprintf(who, sizeof(who), "no clients");
    else if (clients == 1U && wl_monitor_tcp_client(m, 0) != NULL)
        (void)snprintf(who, sizeof(who), "%s", wl_monitor_tcp_client(m, 0)->name);
    else
        (void)snprintf(who, sizeof(who), "%u clients", clients);
    if (has(info->tcp))
        (void)snprintf(buf, sizeof(buf), "%s: %s", info->tcp, who);
    else
        (void)snprintf(buf, sizeof(buf), "%s", who);
    col = put(g, 2, col, buf, WL_TUI_NORMAL);
    if (has(info->net)) {
        col = put(g, 2, col + 3U, "net ", WL_TUI_BOLD);
        (void)put(g, 2, col, info->net, WL_TUI_NORMAL);
    }

    fill(g, 3, WL_TUI_NORMAL);
    col = 1;
    if (has(info->memory)) {
        col = put(g, 3, col, "memory ", WL_TUI_BOLD);
        col = put(g, 3, col, info->memory, WL_TUI_NORMAL) + 3U;
    }
    if (has(info->cpu))
        (void)put(g, 3, col, info->cpu, WL_TUI_NORMAL);
}

/* One piece of work: what, whose, on what, then its progress and age at the
 * right edge, the name giving way when the row is short of room. */
static void work_row(const struct wl_mon_row *r, uint32_t now_ms, struct wl_tui_grid *g,
                     unsigned row)
{
    char left[48];
    char progress[80];
    char age[24];
    char right[112];
    size_t lw;
    size_t rw;
    unsigned col;

    fill(g, row, WL_TUI_NORMAL);
    (void)snprintf(left, sizeof(left), " %-9s %-15.15s ", wl_mon_kind_name(r->kind),
                   r->session);
    wl_mon_progress(r, progress, sizeof(progress));
    wl_mon_elapsed(now_ms - r->started_ms, age, sizeof(age));
    (void)snprintf(right, sizeof(right), "  %s  %6s ", progress, age);
    lw = strlen(left);
    rw = strlen(right);
    col = put(g, row, 0, left, WL_TUI_NORMAL);
    if (lw + rw < g->w)
        (void)put_max(g, row, col, r->name, g->w - lw - rw, WL_TUI_NORMAL);
    put_right(g, row, right, WL_TUI_NORMAL);
}

static void work_pane(const struct wl_monitor *m, uint32_t now_ms, struct wl_tui_grid *g,
                      unsigned top, unsigned height)
{
    struct wl_mon_totals tot;
    unsigned n = wl_monitor_rows(m);
    unsigned shown = n <= height ? n : height - 1U;
    char bytes[32];
    char buf[96];

    (void)snprintf(buf, sizeof(buf), n == 0U ? "work" : "work (%u)", n);
    rule(g, top, buf);
    /* Everything done since the agent started, at the rule's right end,
     * beside the work it counts. */
    wl_monitor_totals(m, &tot);
    wl_mon_bytes(tot.bytes, bytes, sizeof(bytes));
    (void)snprintf(buf, sizeof(buf), " %lu command(s), %lu transfer(s), %s moved --",
                   (unsigned long)tot.commands, (unsigned long)tot.transfers, bytes);
    if (strlen(buf) + 16U < g->w)
        put_right(g, top, buf, WL_TUI_NORMAL);
    if (n == 0U) {
        fill(g, top + 1U, WL_TUI_NORMAL);
        (void)put(g, top + 1U, 1, "nothing running", WL_TUI_NORMAL);
        for (unsigned i = 1; i < height; i++)
            fill(g, top + 1U + i, WL_TUI_NORMAL);
        return;
    }
    for (unsigned i = 0; i < shown; i++) {
        const struct wl_mon_row *r = wl_monitor_row(m, i);

        if (r != NULL)
            work_row(r, now_ms, g, top + 1U + i);
        else
            fill(g, top + 1U + i, WL_TUI_NORMAL);
    }
    if (shown < n) {
        fill(g, top + 1U + shown, WL_TUI_NORMAL);
        (void)snprintf(buf, sizeof(buf), " ... and %u more", n - shown);
        (void)put(g, top + 1U + shown, 0, buf, WL_TUI_NORMAL);
        shown++;
    }
    for (unsigned i = shown; i < height; i++)
        fill(g, top + 1U + i, WL_TUI_NORMAL);
}

static void log_pane(const struct wl_monitor *m, struct wl_tui_view *v, struct wl_tui_grid *g,
                     unsigned top, unsigned height)
{
    struct wl_mon_totals tot;
    unsigned lines = wl_monitor_log_lines(m);
    unsigned most = lines > height ? lines - height : 0U;
    unsigned first;
    char buf[64];

    /* A view scrolled back stays on the lines it shows: new ones push the
     * newest further below, so the distance back grows by as many. */
    wl_monitor_totals(m, &tot);
    if (v->back > 0U && v->back != BACK_ALL)
        v->back += (unsigned)(tot.notes - v->notes_seen);
    v->notes_seen = tot.notes;
    v->lines_seen = lines;
    if (v->back > most)
        v->back = most;
    v->page = height;

    if (v->help) {
        rule(g, top, "help");
        for (unsigned i = 0; i < height; i++) {
            fill(g, top + 1U + i, WL_TUI_NORMAL);
            if (i < sizeof(help_text) / sizeof(help_text[0]))
                (void)put(g, top + 1U + i, 1, help_text[i], i == 0U ? WL_TUI_BOLD : WL_TUI_NORMAL);
        }
        return;
    }
    if (v->back > 0U)
        (void)snprintf(buf, sizeof(buf), "log, %u line(s) back (End follows)", v->back);
    else
        (void)snprintf(buf, sizeof(buf), "log");
    rule(g, top, buf);
    first = lines > height + v->back ? lines - height - v->back : 0U;
    for (unsigned i = 0; i < height; i++) {
        fill(g, top + 1U + i, WL_TUI_NORMAL);
        if (first + i < lines - v->back)
            (void)put(g, top + 1U + i, 1, wl_monitor_log(m, first + i), WL_TUI_NORMAL);
    }
}

static void key_bar(const struct wl_tui_view *v, struct wl_tui_grid *g, unsigned row)
{
    const char *text;

    switch (v->asking) {
    case 'r':
        text = " Restart the agent? y to restart, any other key to leave it";
        break;
    case 'P':
        text = " Power this machine off? y to power off, any other key to leave it";
        break;
    case 'B':
        text = " Reboot this machine? y to reboot, any other key to leave it";
        break;
    default:
        text = " Up/Down PgUp/PgDn scroll  End follow  r restart  P power off  "
               "B reboot  ? help";
        break;
    }
    /* A question is bright on black, the reverse of the usual bar, so it
     * cannot be missed. Bold on inverse would be the obvious choice and is
     * the wrong one: the VGA console draws it as grey on light grey. */
    if (v->asking != 0) {
        fill(g, row, WL_TUI_NORMAL);
        (void)put(g, row, 0, text, WL_TUI_BOLD);
    } else {
        fill(g, row, WL_TUI_INVERSE);
        (void)put(g, row, 0, text, WL_TUI_INVERSE);
    }
}

void wl_tui_render(const struct wl_monitor *m, const struct wl_tui_info *info,
                   struct wl_tui_view *v, struct wl_tui_grid *g, unsigned w, unsigned h)
{
    unsigned rest;
    unsigned work_h;
    unsigned n;

    g->w = w < WL_TUI_MAX_W ? w : WL_TUI_MAX_W;
    g->h = h < WL_TUI_MAX_H ? h : WL_TUI_MAX_H;
    for (unsigned r = 0; r < g->h; r++)
        fill(g, r, WL_TUI_NORMAL);
    if (g->w < WL_TUI_MIN_W || g->h < WL_TUI_MIN_H)
        return;

    /* The work pane grows with the work, up to a third of the free rows, and
     * never below one line; the log has the rest. */
    rest = g->h - FIXED_ROWS;
    n = wl_monitor_rows(m);
    work_h = rest / 3U > 0U ? rest / 3U : 1U;
    if (n < work_h)
        work_h = n > 0U ? n : 1U;

    title_bar(info, g);
    status_lines(m, info, g);
    work_pane(m, info->now_ms, g, 4U, work_h);
    log_pane(m, v, g, 5U + work_h, rest - work_h);
    key_bar(v, g, g->h - 1U);
}
