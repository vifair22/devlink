#ifndef DEVLINK_MONITOR_H
#define DEVLINK_MONITOR_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/agent.h"
#include "devlink/peer.h"
#include "devlink/proto.h"

/*
 * What the agent is doing, in a form something can display.
 *
 * The console agent prints a line per event and forgets it. A window has to
 * answer a different question: not what happened, but what is happening now.
 * One session can carry eight commands, four transfers and a sync at the same
 * time, and no amount of scrollback shows that, so it needs a model rather
 * than a log.
 *
 * The model lives here, in the core, for one reason: nothing in a window can
 * be reached from the tests. Keeping the state and the formatting on this side
 * of the line means the part that can be wrong is the part that is tested,
 * and the window above it stays a matter of creating controls and laying them
 * out.
 *
 * Not thread-safe, deliberately. The agent loop produces on its own thread and
 * the display consumes on another, so whoever owns both serializes them (a
 * CRITICAL_SECTION, on the target). A lock in here would mean picking a
 * threading library for code that also compiles under Watcom, where the answer
 * is a different one.
 */

/* Roughly a console's worth of scrollback. The line length holds a stamp, the
 * longest prefix the agent logs and a full path. */
#define WL_MON_LOG_LINES 512U
#define WL_MON_LINE_MAX 304U

/* Eight commands across every session, plus four transfers and a sync for
 * each session that is running them: serial and a few TCP clients fit with
 * room to spare, and past that only a display row is lost, never the work. */
#define WL_MON_ROWS 64U

/* A session's name: "serial", or a TCP client's address. */
#define WL_MON_SESSION_MAX 64U

/* The serial session: none, a session whose client has not said who it is
 * yet, and a client that has. The window adds "stopped" itself: whether the
 * agent thread is running is something it knows and the model does not. TCP
 * clients are listed separately (wl_monitor_tcp_client), since there can be
 * several and each is either connected or gone. */
enum { WL_MON_WAITING = 0, WL_MON_LINKED = 1, WL_MON_SERVING = 2 };

/* One piece of work in progress. total is 0 when the size is not known in
 * advance, which is every command and every sync. */
struct wl_mon_row {
    char session[WL_MON_SESSION_MAX]; /* whose work: channels are per session */
    uint8_t tcp;
    uint16_t ch;
    uint8_t kind; /* WL_KIND_EXEC, _PUT, _GET, _SYNC */
    uint64_t done;
    uint64_t total;
    uint32_t started_ms; /* wl_clock_ms when the work opened */
    char name[WL_PATH_MAX];
};

/* A connected TCP client. build is empty until it has identified itself. */
struct wl_mon_client {
    char name[WL_MON_SESSION_MAX];
    char build[WL_PEER_BUILD_MAX + 1];
};

#define WL_MON_CLIENTS WL_AGENT_MAX_CONN

struct wl_mon_totals {
    uint32_t sessions;
    uint32_t commands;
    uint32_t transfers;
    uint64_t bytes;
    /* Lines ever noted, and of those, the ones the ring overwrote. A display
     * that remembers the count it last drew appends the difference rather
     * than rebuilding its pane, which on the target is the difference between
     * redrawing 512 lines four times a second and redrawing none. */
    uint32_t notes;
    uint32_t dropped;
};

struct wl_monitor;

struct wl_monitor *wl_monitor_new(void);
void wl_monitor_free(struct wl_monitor *m);

/* Back to "no session, nothing running", keeping the log and the totals: what
 * the window wants when the agent has been stopped and started again. */
void wl_monitor_reset(struct wl_monitor *m);

/*
 * Producer side, called from the agent's thread.
 *
 * wl_monitor_note takes the wall clock as an argument rather than reading it,
 * so the stamp it formats is something a test can assert. It wants local time,
 * since that is what the stamp shows; seconds past midnight is all it uses.
 */
void wl_monitor_note(struct wl_monitor *m, uint64_t local_time, const char *msg);
void wl_monitor_event(struct wl_monitor *m, const struct wl_agent_event *e);

/* Display side. */
int wl_monitor_state(const struct wl_monitor *m);
const char *wl_monitor_client(const struct wl_monitor *m);

/* Bumped by every change. A display that has already drawn this generation
 * has nothing to do, which on the target is the difference between a repaint
 * per event and a repaint per tick. */
uint32_t wl_monitor_generation(const struct wl_monitor *m);

unsigned wl_monitor_rows(const struct wl_monitor *m);
/* NULL past the last row. Rows keep the order the work started in. */
const struct wl_mon_row *wl_monitor_row(const struct wl_monitor *m, unsigned i);

unsigned wl_monitor_tcp_clients(const struct wl_monitor *m);
/* In the order they connected; NULL past the last. */
const struct wl_mon_client *wl_monitor_tcp_client(const struct wl_monitor *m, unsigned i);

unsigned wl_monitor_log_lines(const struct wl_monitor *m);
/* Oldest line still held is 0, so a display walks 0..lines-1 top to bottom.
 * NULL past the last line. */
const char *wl_monitor_log(const struct wl_monitor *m, unsigned i);

void wl_monitor_totals(const struct wl_monitor *m, struct wl_mon_totals *out);

#endif
