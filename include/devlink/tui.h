#ifndef DEVLINK_TUI_H
#define DEVLINK_TUI_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/monitor.h"

/*
 * The agent's text screen: what wl_monitor holds, laid out as a grid of
 * characters, and the keys that move around it.
 *
 * Everything that decides what the screen says lives here, in the core, for
 * the same reason the monitor does: it can be tested. A platform's part is
 * only to put the grid on a terminal and hand keys back (src/linux/tui_term.c
 * on Linux; a console backend on Windows would be the same size). Nothing in
 * here reads a clock, a file or a terminal: the caller passes the moment and
 * the facts about the machine in wl_tui_info.
 *
 * Layout, top to bottom: a title bar, three lines of status, the work in
 * progress, the log (the newest lines, or scrolled back), and a bar of keys
 * that becomes the question when one is being asked. It fits 80x24 and uses
 * more room when there is more.
 */

#define WL_TUI_MAX_W 200U
#define WL_TUI_MAX_H 80U
/* The smallest screen laid out at all; anything smaller is left blank. */
#define WL_TUI_MIN_W 20U
#define WL_TUI_MIN_H 8U

enum { WL_TUI_NORMAL = 0, WL_TUI_BOLD = 1, WL_TUI_INVERSE = 2 };

/* The screen as characters and one attribute each. Plain ASCII: the VGA
 * console's font has no guarantees beyond it. */
struct wl_tui_grid {
    unsigned w;
    unsigned h;
    char ch[WL_TUI_MAX_H][WL_TUI_MAX_W];
    uint8_t attr[WL_TUI_MAX_H][WL_TUI_MAX_W];
};

/* What the screen shows about the machine, which the monitor does not know.
 * Every string may be NULL or empty; it is then left out. */
struct wl_tui_info {
    const char *version; /* the agent's build */
    const char *serial;  /* "ttyS0 at 115200", or "none" */
    const char *tcp;     /* "port 2098", or why it is off */
    unsigned tcp_on;     /* nonzero when TCP is listening */
    const char *net;     /* "eth0 10.0.2.15", or "no network" */
    const char *memory;  /* "480 MB free" */
    const char *cpu;     /* "cpu 12%  load 0.21": how busy the machine is */
    const char *clock;   /* "21:40:07" */
    uint32_t now_ms;     /* wl_clock_ms, for how long work has run */
};

/* Keys beyond plain ASCII, as the backend decodes them. */
enum {
    WL_TUI_KEY_UP = 0x100,
    WL_TUI_KEY_DOWN,
    WL_TUI_KEY_PGUP,
    WL_TUI_KEY_PGDN,
    WL_TUI_KEY_HOME,
    WL_TUI_KEY_END
};

/* What a key asks of the program; everything else the view handles itself. */
enum {
    WL_TUI_NONE = 0,
    WL_TUI_REDRAW,   /* repaint every cell: the screen may have been written over */
    WL_TUI_RESTART,  /* stop the agent; init starts it again */
    WL_TUI_POWEROFF,
    WL_TUI_REBOOT
};

/* What the reader has done: where the log is scrolled to, and whether a
 * question or the help is up. Kept by the caller across frames. */
struct wl_tui_view {
    /* Lines back from the newest; 0 follows the log as it grows. */
    unsigned back;
    /* The log's line count and note count when last drawn, so a view that
     * is scrolled back stays on the same lines as new ones arrive. */
    uint32_t notes_seen;
    unsigned lines_seen;
    unsigned page;    /* the log pane's height at the last render */
    int asking;       /* 0, or the key whose question is up: 'r', 'P', 'B' */
    int help;         /* nonzero while the help replaces the log */
};

void wl_tui_view_init(struct wl_tui_view *v);

/* One key. Returns a WL_TUI_* action; the view has already been updated. */
int wl_tui_key(struct wl_tui_view *v, int key);

/* The whole screen, w x h (clipped to WL_TUI_MAX_*). Reads the monitor, so
 * the caller holds whatever lock serializes it. */
void wl_tui_render(const struct wl_monitor *m, const struct wl_tui_info *info,
                   struct wl_tui_view *v, struct wl_tui_grid *g, unsigned w, unsigned h);

#endif
