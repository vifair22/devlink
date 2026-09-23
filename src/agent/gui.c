#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include "devlink/agent.h"
#include "devlink/clock.h"
#include "devlink/monitor.h"
#include "devlink/paths.h"
#include "devlink/proto.h"
#include "devlink/transport.h"
#include "devlink/version.h"

/*
 * The agent with a window instead of a console.
 *
 * Everything below this file is shared with DEVLINK.EXE, which keeps its
 * console: this is a second front end over the same agent, not a fork of it.
 * Two binaries rather than one because AttachConsole arrived in XP, so a
 * windowed binary on Windows 2000 cannot write to the console that started it,
 * and the diagnostics (echo, linktest, --version) would go silent.
 *
 * The division of labour is deliberate and worth keeping: nothing in here can
 * be reached by the test suite, which runs on Linux, so nothing in here is
 * allowed to know anything. The agent loop runs untouched on a worker thread,
 * reports through the callbacks it already had, and wl_monitor (core, tested)
 * holds the state. What is left for this file is creating controls, placing
 * them, and turning the model into text.
 */

#define IDC_LIST 101
#define IDC_LOG 102
#define IDC_STATUS 103
#define IDC_START 104
#define IDC_STOP 105
#define ID_TICK 1

/* The one resource both binaries carry (src/agent/devlink.rc). */
#define IDI_DEVLINK 1

/* Tray icon: its own message, and the commands on its menu. */
#define WM_TRAY (WM_APP + 2)
#define IDM_SHOW 200
#define IDM_START 201
#define IDM_STOP 202
#define IDM_EXIT 203
#define IDM_TRAY 204
#define IDM_ABOUT 205
#define IDM_COPY 206
#define IDM_COPY_ALL 207
#define IDM_CLEAR 208

/* The worker has finished, for whatever reason: failed to open the port,
 * asked to stop, or told to stop by the client's @stop. */
#define WM_AGENT_DONE (WM_APP + 1)

/* Fast enough to feel live, slow enough that a 500 MHz machine spends its time
 * serving rather than painting. Bursts coalesce into one repaint. */
#define TICK_MS 250U

#define LOG_PANE_SHARE 60 /* per cent of the split given to the log */

/* How long closing waits for the agent to let the port go before giving up
 * on it. The loop is cooperative and comes back within a pump, so reaching
 * this means something is genuinely stuck. */
#define CLOSE_WAIT_MS 8000U

/* Three columns hold a known shape of text; the fourth is the command line or
 * the path, so it takes whatever width is left. */
#define COL_WHAT 90
#define COL_PROGRESS 150
#define COL_ELAPSED 70

struct app {
    HINSTANCE inst;
    HWND wnd;
    HWND list;
    HWND log;
    HWND status;
    HWND start;
    HWND stop_btn;
    HFONT font;

    HICON icon;
    int in_tray;    /* the window is hidden and the tray entry is up */
    int start_tray; /* asked to begin that way */
    /* What the status bar last said, kept on this side so the tray tooltip
     * can be built without reaching into the model behind the lock. */
    char status_text[96];

    struct wl_monitor *mon;
    CRITICAL_SECTION lock; /* the worker writes mon, the window reads it */

    HANDLE thread;
    volatile int stopping; /* asked for; the loop notices within one pump */
    int running;
    int closing;         /* WM_CLOSE is waiting for the worker to come back */
    uint32_t closing_at; /* when it started waiting */

    char port[32];
    unsigned long baud;

    /* Replacing itself. The paths are taken once at startup, before anything
     * is renamed, so they still name this program afterwards. */
    char exe[WL_PATH_MAX];
    char fresh[WL_PATH_MAX]; /* the upload waiting beside it */
    char spent[WL_PATH_MAX]; /* what this build becomes */
    int update_wanted;       /* the client sent @stop */
    unsigned tidy_tries;     /* attempts to remove the previous build */

    /* What the window has already drawn, so a tick with nothing new costs
     * nothing: the generation for the list, the line count for the log. */
    uint32_t drawn_gen;
    uint32_t drawn_notes;
    int drawn_state;
    unsigned long drawn_seconds;
    /* The log's top line as this window last set it. Anything else means the
     * reader scrolled, and the pane stops following the tail until they come
     * back to it. */
    LRESULT log_top;
};

static struct app app;

/* ---------------------------------------------------------------------------
 * the agent, on its own thread
 * ------------------------------------------------------------------------- */

static void note(const char *msg)
{
    uint64_t utc = 0;

    (void)wl_clock_utc(&utc);
    EnterCriticalSection(&app.lock);
    wl_monitor_note(app.mon, utc + (uint64_t)(wl_clock_local_offset_minutes() * 60), msg);
    LeaveCriticalSection(&app.lock);
}

static void on_log(void *ctx, const char *msg)
{
    (void)ctx;
    note(msg);
}

static void on_event(void *ctx, const struct wl_agent_event *e)
{
    (void)ctx;
    /* Noted on this thread and acted on by the window, which is where the
     * process may be replaced: the loop has to finish and let the port go
     * first. */
    if (e->type == WL_EV_STOPPING)
        app.update_wanted = 1;
    EnterCriticalSection(&app.lock);
    wl_monitor_event(app.mon, e);
    LeaveCriticalSection(&app.lock);
}

static int should_stop(void *ctx)
{
    (void)ctx;
    return app.stopping;
}

static DWORD WINAPI serve(LPVOID arg)
{
    struct wl_agent_cfg cfg;
    struct wl_transport t;
    OSVERSIONINFOA vi;
    char msg[256];

    (void)arg;
    memset(&cfg, 0, sizeof(cfg));
    wl_session_cfg_init(&cfg.session);
    cfg.session.link.bytes_per_sec = (uint32_t)(app.baud / 10UL);
    vi.dwOSVersionInfoSize = sizeof(vi);
    cfg.platform = GetVersionExA(&vi) && vi.dwPlatformId == VER_PLATFORM_WIN32_NT
                       ? WL_PLATFORM_WINNT
                       : WL_PLATFORM_WIN9X;
    cfg.stop = should_stop;
    cfg.log = on_log;
    cfg.event = on_event;

    if (wl_serial_open(app.port, (uint32_t)app.baud, &t) != WL_OK) {
        (void)snprintf(msg, sizeof(msg), "cannot open %s: %s", app.port, wl_error());
        note(msg);
    } else {
        (void)snprintf(msg, sizeof(msg), "serving on %s at %lu", app.port, app.baud);
        note(msg);
        if (wl_agent_serve(&t, &cfg, (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId())
            != WL_OK)
            note(wl_error());
        wl_transport_close(&t);
        note("stopped");
    }
    (void)PostMessage(app.wnd, WM_AGENT_DONE, 0, 0);
    return 0;
}

static void start_serving(void)
{
    DWORD id;

    if (app.running)
        return;
    app.stopping = 0;
    EnterCriticalSection(&app.lock);
    wl_monitor_reset(app.mon);
    LeaveCriticalSection(&app.lock);
    app.thread = CreateThread(NULL, 0, serve, NULL, 0, &id);
    if (app.thread == NULL) {
        note("cannot start the serving thread");
        return;
    }
    app.running = 1;
}

/* Cooperative, and it has to stay that way: the loop notices within one pump
 * and unwinds its own commands and transfers. Killing the thread would leave
 * processes and handles behind, which is the mistake wl_proc_free was changed
 * to stop making. */
static void stop_serving(void)
{
    if (app.running)
        app.stopping = 1;
}

/* ---------------------------------------------------------------------------
 * turning the model into text
 * ------------------------------------------------------------------------- */

/* Integer-only, because the whole project avoids floating point on a machine
 * whose FPU is not the point. One decimal is as much as a progress column can
 * use. */
static void human(uint64_t bytes, char *out, size_t cap)
{
    static const char *const unit[] = {"B", "KB", "MB", "GB"};
    unsigned u = 0;
    uint64_t whole = bytes;
    uint64_t tenth = 0;

    while (whole >= 1024U && u < 3U) {
        tenth = ((whole % 1024U) * 10U) / 1024U;
        whole /= 1024U;
        u++;
    }
    if (u == 0)
        (void)snprintf(out, cap, "%lu B", (unsigned long)whole);
    else
        (void)snprintf(out, cap, "%lu.%lu %s", (unsigned long)whole, (unsigned long)tenth,
                       unit[u]);
}

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case WL_KIND_EXEC:
        return "command";
    case WL_KIND_PUT:
        return "receiving";
    case WL_KIND_GET:
        return "sending";
    case WL_KIND_SYNC:
        return "sync";
    default:
        return "?";
    }
}

/* Called from LVN_GETDISPINFO, so the caller already holds the lock. */
static void row_text(const struct wl_mon_row *r, int col, char *out, size_t cap)
{
    char a[32];
    char b[32];

    switch (col) {
    case 0:
        (void)snprintf(out, cap, "%s", kind_name(r->kind));
        break;
    case 1:
        (void)snprintf(out, cap, "%s", r->name);
        break;
    case 2:
        human(r->done, a, sizeof(a));
        if (r->total == 0) {
            /* A command has no size to work towards; what its count means is
             * worth saying, since the column next to it is a percentage. */
            (void)snprintf(out, cap, "%s%s", a, r->kind == WL_KIND_EXEC ? " out" : "");
        } else {
            human(r->total, b, sizeof(b));
            (void)snprintf(out, cap, "%s of %s (%lu%%)", a, b,
                           (unsigned long)(r->done * 100U / r->total));
        }
        break;
    default: {
        unsigned long secs = wl_clock_since(r->started_ms) / 1000UL;

        if (secs < 60UL)
            (void)snprintf(out, cap, "%lus", secs);
        else
            (void)snprintf(out, cap, "%lum %02lus", secs / 60UL, secs % 60UL);
        break;
    }
    }
}

static const char *state_text(int state, const char *client)
{
    static char buf[128];

    switch (state) {
    case WL_MON_LINKED:
        return "A client is connecting";
    case WL_MON_SERVING:
        (void)snprintf(buf, sizeof(buf), "Serving %s", client);
        return buf;
    default:
        return app.running ? "Waiting for a client" : "Stopped";
    }
}

/* ---------------------------------------------------------------------------
 * replacing itself
 * ------------------------------------------------------------------------- */

/*
 * The console agent is restarted by AGENT.BAT, whose loop swaps in the upload
 * once the old process has exited. A windowed program cannot borrow that
 * without keeping a console on the desktop, so it does the swap itself.
 *
 * Windows will not let a running image be written over or deleted, but it will
 * let one be renamed within the same volume: the section holds the contents,
 * not the name. So this build steps aside under its own .OLD name, the upload
 * takes its place, and the new file is started before this process exits. No
 * helper program, and the build it replaced is left on disk, which is as much
 * of a way back as this needs.
 *
 * Nothing here is reachable from the test suite; the part that can be wrong on
 * its own is in wl_path_sibling, which is, and is tested.
 */
static int swap_in_the_update(void)
{
    char msg[WL_PATH_MAX + 64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmd;
    int started;

    if (app.exe[0] == '\0' || app.fresh[0] == '\0' || app.spent[0] == '\0')
        return 0;
    if (GetFileAttributesA(app.fresh) == INVALID_FILE_ATTRIBUTES)
        return 0; /* stopped, but with nothing waiting to take over */

    (void)DeleteFileA(app.spent); /* whatever the last update left */
    if (!MoveFileA(app.exe, app.spent)) {
        (void)snprintf(msg, sizeof(msg), "cannot step aside (%lu); the update was not applied",
                       GetLastError());
        note(msg);
        return 0;
    }
    if (!MoveFileA(app.fresh, app.exe)) {
        (void)snprintf(msg, sizeof(msg), "cannot move the update in (%lu); putting this build back",
                       GetLastError());
        note(msg);
        /* Back where it was, so the next start is this build rather than
         * nothing at all. */
        (void)MoveFileA(app.spent, app.exe);
        return 0;
    }

    /*
     * Started with the command line this process was given, so the port, the
     * rate and -tray all survive the update. The image is named separately
     * because the path in the command line is whatever was typed, which need
     * not be the file that ran.
     */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    cmd = GetCommandLineA();
    {
        size_t n = strlen(cmd) + 1;
        char *copy = malloc(n);

        if (copy == NULL)
            return 0;
        memcpy(copy, cmd, n);
        started = CreateProcessA(app.exe, copy, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) != 0;
        free(copy);
    }
    if (!started) {
        (void)snprintf(msg, sizeof(msg), "the update is in place but would not start (%lu)",
                       GetLastError());
        note(msg);
        (void)MessageBoxA(app.wnd, msg, "devlink", MB_ICONWARNING | MB_OK);
        return 0;
    }
    (void)CloseHandle(pi.hThread);
    (void)CloseHandle(pi.hProcess);
    return 1;
}

/* The build this one replaced, once it has finished exiting. The first
 * attempts fail: the old process is still on its way out while this one is
 * starting, and Windows keeps the file locked until it has gone. */
static void tidy_the_old_build(void)
{
    if (app.spent[0] == '\0' || app.tidy_tries >= 40U)
        return;
    if (GetFileAttributesA(app.spent) == INVALID_FILE_ATTRIBUTES) {
        app.tidy_tries = 40U; /* gone; stop looking */
        return;
    }
    app.tidy_tries++;
    (void)DeleteFileA(app.spent);
}

/* ---------------------------------------------------------------------------
 * the log pane's own commands
 * ------------------------------------------------------------------------- */

/* Hands text to the clipboard, which takes ownership of the block. */
static void to_clipboard(const char *text, size_t n)
{
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n + 1);
    char *at;

    if (mem == NULL)
        return;
    at = GlobalLock(mem);
    if (at == NULL) {
        (void)GlobalFree(mem);
        return;
    }
    memcpy(at, text, n);
    at[n] = '\0';
    (void)GlobalUnlock(mem);
    if (!OpenClipboard(app.wnd)) {
        (void)GlobalFree(mem);
        return;
    }
    (void)EmptyClipboard();
    if (SetClipboardData(CF_TEXT, mem) == NULL)
        (void)GlobalFree(mem); /* still ours if the shell would not take it */
    (void)CloseClipboard();
}

/*
 * Copies the log out, either the selected line or all of it.
 *
 * Worth having for one reason: the message that matters is usually an error,
 * and the alternative to copying it is reading it off the target's screen and
 * typing it somewhere else, which is how a message gets reported approximately
 * rather than exactly.
 */
static void copy_log(int everything)
{
    LRESULT count = SendMessageA(app.log, LB_GETCOUNT, 0, 0);
    LRESULT first = everything ? 0 : SendMessageA(app.log, LB_GETCURSEL, 0, 0);
    LRESULT last = everything ? count - 1 : first;
    char *text;
    size_t at = 0;
    size_t cap;

    if (count <= 0 || first == LB_ERR || first < 0)
        return;
    cap = ((size_t)(last - first + 1) * (WL_MON_LINE_MAX + 2U)) + 1U;
    text = malloc(cap);
    if (text == NULL)
        return;
    for (LRESULT i = first; i <= last; i++) {
        LRESULT n = SendMessageA(app.log, LB_GETTEXTLEN, (WPARAM)i, 0);

        if (n == LB_ERR || at + (size_t)n + 3U > cap)
            break;
        n = SendMessageA(app.log, LB_GETTEXT, (WPARAM)i, (LPARAM)(text + at));
        if (n == LB_ERR)
            break;
        at += (size_t)n;
        /* CRLF, because this is going into a Windows clipboard and from there
         * most likely into Notepad. */
        text[at++] = '\r';
        text[at++] = '\n';
    }
    to_clipboard(text, at);
    free(text);
}

static void log_menu(int x, int y)
{
    HMENU menu = CreatePopupMenu();
    int has_line = SendMessageA(app.log, LB_GETCURSEL, 0, 0) != LB_ERR;

    if (menu == NULL)
        return;
    (void)AppendMenuA(menu, MF_STRING | (has_line ? 0U : MF_GRAYED), IDM_COPY, "&Copy line");
    (void)AppendMenuA(menu, MF_STRING, IDM_COPY_ALL, "Copy &all");
    (void)AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    (void)AppendMenuA(menu, MF_STRING, IDM_CLEAR, "C&lear");
    (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, app.wnd, NULL);
    (void)DestroyMenu(menu);
}

/* Empties the pane, not the model: the lines are still counted, so what
 * arrives after this is appended rather than the whole history coming back. */
static void clear_log(void)
{
    (void)SendMessageA(app.log, LB_RESETCONTENT, 0, 0);
    (void)SendMessageA(app.log, LB_SETHORIZONTALEXTENT, 0, 0);
    app.log_top = 0;
}

/* ---------------------------------------------------------------------------
 * the tray
 * ------------------------------------------------------------------------- */

/*
 * The tray entry, declared at the size shell32 4.0 used.
 *
 * Nothing here needs the balloon fields that arrived with Windows 2000, and
 * asking for a structure larger than the shell understands makes the call fail
 * outright, so the oldest size that carries an icon and a tooltip is the one
 * that works everywhere this runs.
 */
static void tray_send(DWORD action)
{
    NOTIFYICONDATAA n;

    memset(&n, 0, sizeof(n));
    n.cbSize = NOTIFYICONDATAA_V1_SIZE;
    n.hWnd = app.wnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = app.icon;
    (void)snprintf(n.szTip, 64, "devlink: %s", app.status_text);
    (void)Shell_NotifyIconA(action, &n);
}

static void go_to_tray(void)
{
    if (app.in_tray)
        return;
    tray_send(NIM_ADD);
    app.in_tray = 1;
    /* Hidden rather than merely minimised, so it leaves the taskbar too:
     * something that lives in the tray should be in one place, not two. */
    (void)ShowWindow(app.wnd, SW_HIDE);
}

static void leave_tray(void)
{
    (void)ShowWindow(app.wnd, SW_SHOW);
    (void)ShowWindow(app.wnd, SW_RESTORE);
    (void)SetForegroundWindow(app.wnd);
    if (app.in_tray) {
        tray_send(NIM_DELETE);
        app.in_tray = 0;
    }
}

static void tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    if (menu == NULL)
        return;
    (void)AppendMenuA(menu, MF_STRING, IDM_SHOW, "&Open");
    (void)AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    (void)AppendMenuA(menu, MF_STRING | (app.running ? MF_GRAYED : 0U), IDM_START, "&Start");
    (void)AppendMenuA(menu, MF_STRING | (app.running && !app.stopping ? 0U : MF_GRAYED),
                      IDM_STOP, "S&top");
    (void)AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    (void)AppendMenuA(menu, MF_STRING, IDM_EXIT, "E&xit");
    (void)GetCursorPos(&pt);
    /*
     * The two calls around TrackPopupMenu are the documented workaround for a
     * tray menu: without the first the menu appears behind, and without the
     * second it stays up after the pointer goes elsewhere.
     */
    (void)SetForegroundWindow(app.wnd);
    (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.wnd, NULL);
    (void)PostMessage(app.wnd, WM_NULL, 0, 0);
    (void)DestroyMenu(menu);
}

/* ---------------------------------------------------------------------------
 * the window
 * ------------------------------------------------------------------------- */

static void add_column(HWND list, int i, const char *title, int width)
{
    LVCOLUMNA c;

    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.iSubItem = i;
    c.cx = width;
    /* The struct wants a writable pointer even though it only reads it. */
    c.pszText = (char *)title;
    (void)SendMessageA(list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&c);
}

/* The shell's own message font: Tahoma on 2000, MS Sans Serif on 98. Asking
 * the system beats naming a face that one of the two does not have. */
static HFONT shell_font(void)
{
    NONCLIENTMETRICSA m;

    memset(&m, 0, sizeof(m));
    m.cbSize = sizeof(m);
    if (!SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0))
        return NULL;
    return CreateFontIndirectA(&m.lfMessageFont);
}

static HWND child(const char *cls, const char *text, DWORD style, DWORD ex, int id)
{
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, app.wnd,
                             (HMENU)(INT_PTR)id, app.inst, NULL);

    if (h != NULL && app.font != NULL)
        (void)SendMessageA(h, WM_SETFONT, (WPARAM)app.font, TRUE);
    return h;
}

/* File and Help, which is all there is to command. Everything on it is also
 * reachable without it: the two buttons, the tray menu, and minimising. */
static void create_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    if (bar == NULL || file == NULL || help == NULL)
        return;
    (void)AppendMenuA(file, MF_STRING, IDM_START, "&Start");
    (void)AppendMenuA(file, MF_STRING, IDM_STOP, "S&top");
    (void)AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    (void)AppendMenuA(file, MF_STRING, IDM_TRAY, "&Minimise to tray");
    (void)AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    (void)AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit");
    (void)AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About");
    (void)AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");
    (void)AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    (void)SetMenu(app.wnd, bar);
}

static void about_box(void)
{
    char text[512];

    (void)snprintf(text, sizeof(text),
                   "devlink agent %s\n\n"
                   "Serves a devlink client over %s at %lu.\n"
                   "The same agent as DEVLINK.EXE, with a window instead of a console.\n\n"
                   "The list shows what is running now; the log shows what has happened.\n"
                   "Minimising puts it in the notification area; -tray starts it there.",
                   wl_version(), app.port, app.baud);
    (void)MessageBoxA(app.wnd, text, "About devlink", MB_ICONINFORMATION | MB_OK);
}

static void create_children(void)
{
    app.font = shell_font();
    app.start = child("BUTTON", "&Start", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_START);
    app.stop_btn = child("BUTTON", "S&top", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_STOP);
    app.list = child(WC_LISTVIEWA, NULL,
                     LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                     WS_EX_CLIENTEDGE, IDC_LIST);
    app.log = child("LISTBOX", NULL,
                    WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                    WS_EX_CLIENTEDGE, IDC_LOG);
    app.status = CreateWindowExA(0, STATUSCLASSNAMEA, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                 app.wnd, (HMENU)(INT_PTR)IDC_STATUS, app.inst, NULL);
    if (app.list != NULL) {
        (void)SendMessageA(app.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                           LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);
        add_column(app.list, 0, "What", COL_WHAT);
        add_column(app.list, 1, "Detail", 300);
        add_column(app.list, 2, "Progress", COL_PROGRESS);
        add_column(app.list, 3, "Elapsed", COL_ELAPSED);
    }
    /* Long log lines are worth scrolling to rather than wrapping: a wrapped
     * path in a fixed list is harder to read than one that runs off. The
     * extent grows with the longest line seen, so the scrollbar is live only
     * when there is something to scroll to. */
    if (app.log != NULL)
        (void)SendMessageA(app.log, LB_SETHORIZONTALEXTENT, 0, 0);
}

static void layout(void)
{
    RECT rc;
    int pad = 8;
    int bar = 0;
    int row;
    int top;
    int rest;
    int log_h;

    if (!GetClientRect(app.wnd, &rc))
        return;
    if (app.status != NULL) {
        RECT sr;

        (void)SendMessageA(app.status, WM_SIZE, 0, 0);
        if (GetWindowRect(app.status, &sr))
            bar = sr.bottom - sr.top;
    }
    row = 24;
    (void)MoveWindow(app.start, pad, pad, 80, row, TRUE);
    (void)MoveWindow(app.stop_btn, pad + 88, pad, 80, row, TRUE);

    top = pad + row + pad;
    rest = rc.bottom - bar - top - pad;
    if (rest < 80)
        rest = 80;
    log_h = rest * LOG_PANE_SHARE / 100;
    (void)MoveWindow(app.list, pad, top, rc.right - (2 * pad), rest - log_h - pad, TRUE);
    {
        /* Roughly the vertical scrollbar plus the frame: enough that the
         * detail column stops short of one rather than forcing a horizontal
         * scrollbar by a few pixels. */
        int slack = rc.right - (2 * pad) - COL_WHAT - COL_PROGRESS - COL_ELAPSED - 24;

        if (slack < 120)
            slack = 120;
        (void)SendMessageA(app.list, LVM_SETCOLUMNWIDTH, 1, (LPARAM)slack);
    }
    (void)MoveWindow(app.log, pad, top + rest - log_h, rc.right - (2 * pad), log_h, TRUE);
}

static void set_part(int part, const char *text)
{
    (void)SendMessageA(app.status, SB_SETTEXTA, (WPARAM)part, (LPARAM)text);
}

static void size_status_parts(void)
{
    RECT rc;
    int parts[3];

    if (app.status == NULL || !GetClientRect(app.wnd, &rc))
        return;
    parts[0] = rc.right * 45 / 100;
    parts[1] = rc.right * 65 / 100;
    parts[2] = -1;
    (void)SendMessageA(app.status, SB_SETPARTS, 3, (LPARAM)parts);
}

/* Keeps the log's scrollable width at the longest line it has been given. */
static void widen_for(const char *line)
{
    HDC dc = GetDC(app.log);
    SIZE sz;
    LRESULT have;

    if (dc == NULL)
        return;
    if (app.font != NULL)
        (void)SelectObject(dc, app.font);
    if (GetTextExtentPoint32A(dc, line, (int)strlen(line), &sz)) {
        have = SendMessageA(app.log, LB_GETHORIZONTALEXTENT, 0, 0);
        if ((LRESULT)sz.cx + 8 > have)
            (void)SendMessageA(app.log, LB_SETHORIZONTALEXTENT, (WPARAM)(sz.cx + 8), 0);
    }
    (void)ReleaseDC(app.log, dc);
}

/* New log lines only. The model counts every line it was ever given, so the
 * difference against the count last drawn is exactly what has not been shown,
 * and a wrap that dropped lines nobody read cannot make that negative. */
static void refresh_log(void)
{
    struct wl_mon_totals tot;
    unsigned held;
    unsigned fresh;
    LRESULT count;
    LRESULT was_at;
    LRESULT lost = 0;
    int follow;

    wl_monitor_totals(app.mon, &tot);
    if (tot.notes == app.drawn_notes)
        return;

    /* Decided before anything is added: the pane follows the newest line only
     * while it is already showing it. Scrolling back to read holds the view
     * still, and scrolling to the bottom again resumes. Using the selection
     * for this instead would mean a reader who clicked a line once never saw
     * the log move again. */
    was_at = SendMessageA(app.log, LB_GETTOPINDEX, 0, 0);
    follow = was_at == app.log_top;

    held = wl_monitor_log_lines(app.mon);
    fresh = tot.notes - app.drawn_notes > held ? held : (unsigned)(tot.notes - app.drawn_notes);
    app.drawn_notes = tot.notes;
    for (unsigned i = held - fresh; i < held; i++) {
        const char *line = wl_monitor_log(app.mon, i);

        if (line != NULL) {
            (void)SendMessageA(app.log, LB_ADDSTRING, 0, (LPARAM)line);
            widen_for(line);
        }
    }
    count = SendMessageA(app.log, LB_GETCOUNT, 0, 0);
    while (count > (LRESULT)WL_MON_LOG_LINES) {
        (void)SendMessageA(app.log, LB_DELETESTRING, 0, 0);
        count--;
        lost++;
    }
    if (follow && count > 0) {
        /* Read back rather than assumed: the control clamps this to the last
         * whole page, so what it settles on is what counts as "at the end". */
        (void)SendMessageA(app.log, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
        app.log_top = SendMessageA(app.log, LB_GETTOPINDEX, 0, 0);
    } else if (lost > 0) {
        /* Lines went from the top, so the held view slid up with them. */
        app.log_top = was_at > lost ? was_at - lost : 0;
    }
}

static void refresh(void)
{
    struct wl_mon_totals tot;
    unsigned rows;
    int state;
    unsigned long elapsed;
    char buf[160];
    char bytes[32];
    int changed;

    EnterCriticalSection(&app.lock);
    state = wl_monitor_state(app.mon);
    rows = wl_monitor_rows(app.mon);
    wl_monitor_totals(app.mon, &tot);
    changed = wl_monitor_generation(app.mon) != app.drawn_gen || state != app.drawn_state;
    /* The elapsed column moves on its own, so a second passing is a change
     * even when nothing was reported. */
    elapsed = (unsigned long)(wl_clock_ms() / 1000U);
    if (elapsed != app.drawn_seconds && rows > 0)
        changed = 1;
    if (changed) {
        (void)snprintf(app.status_text, sizeof(app.status_text), "%s",
                       state_text(state, wl_monitor_client(app.mon)));
        set_part(0, app.status_text);
        human(tot.bytes, bytes, sizeof(bytes));
        (void)snprintf(buf, sizeof(buf), "%lu command(s), %lu transfer(s), %s",
                       (unsigned long)tot.commands, (unsigned long)tot.transfers, bytes);
        set_part(2, buf);
        (void)SendMessageA(app.list, LVM_SETITEMCOUNT, (WPARAM)rows, 0);
        (void)InvalidateRect(app.list, NULL, FALSE);
        app.drawn_gen = wl_monitor_generation(app.mon);
        app.drawn_state = state;
        app.drawn_seconds = elapsed;
    }
    refresh_log();
    LeaveCriticalSection(&app.lock);

    (void)EnableWindow(app.start, app.running ? FALSE : TRUE);
    (void)EnableWindow(app.stop_btn, app.running && !app.stopping ? TRUE : FALSE);
    if (changed && app.in_tray)
        tray_send(NIM_MODIFY);
}

/* The list holds no data of its own; every cell is answered from the model as
 * it is painted, which is why a finished command cannot leave a row behind. */
static void on_getdispinfo(LPARAM lparam)
{
    NMLVDISPINFOA *di = (NMLVDISPINFOA *)lparam;
    static char cell[WL_PATH_MAX + 64];

    if ((di->item.mask & LVIF_TEXT) == 0)
        return;
    cell[0] = '\0';
    EnterCriticalSection(&app.lock);
    {
        const struct wl_mon_row *r = wl_monitor_row(app.mon, (unsigned)di->item.iItem);

        if (r != NULL)
            row_text(r, di->item.iSubItem, cell, sizeof(cell));
    }
    LeaveCriticalSection(&app.lock);
    di->item.pszText = cell;
}

static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        app.wnd = wnd;
        create_menu();
        create_children();
        size_status_parts();
        set_part(1, "");
        (void)SetTimer(wnd, ID_TICK, TICK_MS, NULL);
        return 0;
    case WM_SIZE:
        /* Minimising is what someone does to get it out of the way, so that
         * is what sends it to the tray rather than a separate command. */
        if (wparam == SIZE_MINIMIZED) {
            go_to_tray();
            return 0;
        }
        layout();
        size_status_parts();
        return 0;
    case WM_CONTEXTMENU:
        if ((HWND)wparam == app.log) {
            /* Sent with screen coordinates already, except from the keyboard,
             * where both are -1 and the corner of the pane will do. */
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);

            if (x == -1 && y == -1) {
                RECT rc;

                if (GetWindowRect(app.log, &rc)) {
                    x = rc.left + 16;
                    y = rc.top + 16;
                }
            }
            log_menu(x, y);
            return 0;
        }
        break;
    case WM_TRAY:
        if (lparam == WM_LBUTTONDBLCLK)
            leave_tray();
        else if (lparam == WM_RBUTTONUP)
            tray_menu();
        return 0;
    case WM_TIMER:
        if (wparam == ID_TICK) {
            refresh();
            tidy_the_old_build();
            /*
             * Closing gave the agent a bounded wait; past it, leave rather
             * than keep a window that will not shut. This exits the process
             * outright instead of destroying the window, because the worker
             * thread may still be inside the monitor: unwinding WinMain would
             * free what it is holding, while exiting hands the lot back to the
             * kernel with nothing left to read it.
             */
            if (app.closing && wl_clock_since(app.closing_at) > CLOSE_WAIT_MS)
                ExitProcess(0);
        }
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lparam)->code == (UINT)LVN_GETDISPINFOA)
            on_getdispinfo(lparam);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_START:
        case IDM_START:
            start_serving();
            break;
        case IDC_STOP:
        case IDM_STOP:
            stop_serving();
            break;
        case IDM_SHOW:
            leave_tray();
            break;
        case IDM_TRAY:
            go_to_tray();
            break;
        case IDM_ABOUT:
            about_box();
            break;
        case IDM_COPY:
            copy_log(0);
            break;
        case IDM_COPY_ALL:
            copy_log(1);
            break;
        case IDM_CLEAR:
            clear_log();
            break;
        case IDM_EXIT:
            (void)PostMessageA(wnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
        refresh();
        return 0;
    case WM_AGENT_DONE:
        if (app.thread != NULL) {
            (void)WaitForSingleObject(app.thread, 2000);
            (void)CloseHandle(app.thread);
            app.thread = NULL;
        }
        app.running = 0;
        app.stopping = 0;
        /*
         * Back to nothing attached and nothing running. The loop ending is not
         * something the model can see for itself: it learns a client has gone
         * from a transition the loop notices, and there is no loop any more, so
         * without this the status bar goes on naming the last client that was
         * connected before the agent was stopped.
         */
        EnterCriticalSection(&app.lock);
        wl_monitor_reset(app.mon);
        LeaveCriticalSection(&app.lock);
        refresh();
        /*
         * Only now, with the loop finished and the port released, is it safe
         * to be replaced: the successor opens the same port as this one exits.
         * A @stop with no upload waiting is just a stop, and the window stays
         * up with Start available.
         */
        if (app.update_wanted) {
            app.update_wanted = 0;
            if (swap_in_the_update()) {
                if (app.in_tray) {
                    tray_send(NIM_DELETE);
                    app.in_tray = 0;
                }
                ExitProcess(0);
            }
            note("@stop arrived with nothing waiting to replace this build");
        }
        if (app.closing)
            (void)DestroyWindow(wnd);
        return 0;
    case WM_CLOSE:
        /*
         * The port is released by the loop on its way out, so closing waits
         * for it rather than exiting under a thread that still owns a handle.
         * The wait is the message loop itself, so the window stays alive and
         * the last log lines appear while it happens.
         */
        if (app.running) {
            app.closing = 1;
            app.closing_at = wl_clock_ms();
            stop_serving();
            return 0;
        }
        (void)DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        if (app.in_tray) {
            tray_send(NIM_DELETE);
            app.in_tray = 0;
        }
        (void)KillTimer(wnd, ID_TICK);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(wnd, msg, wparam, lparam);
}

/*
 * DEVLINKW [-tray] [PORT] [BAUD], the same defaults as DEVLINK serve.
 *
 * -tray starts it already in the notification area, which is what a shortcut
 * in Startup wants: serving from the moment the machine is logged in, without
 * a window in the way of whoever is sitting there.
 */
static void parse_args(const char *cmd)
{
    char line[256];
    char *tok;
    unsigned seen = 0;

    (void)snprintf(app.port, sizeof(app.port), "COM1");
    app.baud = 115200UL;
    if (cmd == NULL)
        return;
    (void)snprintf(line, sizeof(line), "%s", cmd);
    for (tok = strtok(line, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
        if (strcmp(tok, "-tray") == 0 || strcmp(tok, "/tray") == 0) {
            app.start_tray = 1;
        } else if (seen == 0) {
            (void)snprintf(app.port, sizeof(app.port), "%s", tok);
            seen++;
        } else if (seen == 1) {
            unsigned long b = strtoul(tok, NULL, 10);

            if (b != 0)
                app.baud = b;
            seen++;
        }
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA cls;
    MSG msg;
    char title[128];
    char where[64];

    (void)prev;
    /* Same reason as the console agent: a drive with no media must fail
     * rather than raise a prompt nobody is sitting in front of
     * (docs/bug-critical-error-hang.md). Children inherit the mode. */
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    InitCommonControls();

    app.inst = inst;
    parse_args(cmd);
    /* Taken before anything is renamed, so they still name this program when
     * it is time to step aside. */
    if (GetModuleFileNameA(NULL, app.exe, sizeof(app.exe)) > 0) {
        if (!wl_path_sibling(app.exe, ".NEW", app.fresh, sizeof(app.fresh)))
            app.fresh[0] = '\0';
        if (!wl_path_sibling(app.exe, ".OLD", app.spent, sizeof(app.spent)))
            app.spent[0] = '\0';
    }
    app.mon = wl_monitor_new();
    if (app.mon == NULL) {
        (void)MessageBoxA(NULL, "Out of memory.", "devlink", MB_ICONERROR | MB_OK);
        return 1;
    }
    InitializeCriticalSection(&app.lock);

    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursor(NULL, IDC_ARROW);
    app.icon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_DEVLINK));
    if (app.icon == NULL)
        app.icon = LoadIcon(NULL, IDI_APPLICATION);
    cls.hIcon = app.icon;
    cls.hIconSm = app.icon;
    cls.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_BTNFACE + 1);
    cls.lpszClassName = "DevlinkAgent";
    if (RegisterClassExA(&cls) == 0) {
        (void)MessageBoxA(NULL, "Cannot register the window class.", "devlink",
                          MB_ICONERROR | MB_OK);
        return 1;
    }
    (void)snprintf(title, sizeof(title), "devlink agent %s", wl_version());
    if (CreateWindowExA(0, cls.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, 720, 480, NULL, NULL, inst, NULL)
        == NULL) {
        (void)MessageBoxA(NULL, "Cannot create the window.", "devlink", MB_ICONERROR | MB_OK);
        return 1;
    }
    (void)snprintf(where, sizeof(where), "%s at %lu", app.port, app.baud);
    set_part(1, where);
    (void)snprintf(app.status_text, sizeof(app.status_text), "%s", "Starting");
    if (app.start_tray) {
        go_to_tray();
    } else {
        (void)ShowWindow(app.wnd, show);
        (void)UpdateWindow(app.wnd);
    }
    start_serving();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(app.wnd, &msg)) {
            (void)TranslateMessage(&msg);
            (void)DispatchMessage(&msg);
        }
    }
    DeleteCriticalSection(&app.lock);
    if (app.font != NULL)
        (void)DeleteObject(app.font);
    wl_monitor_free(app.mon);
    return 0;
}
