#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "tui_term.h"

static int term_fd = -1;
static struct termios saved;
/* What is on the screen now, to draw only the difference. */
static struct wl_tui_grid shown;
static unsigned shown_w;
static unsigned shown_h;

/* Output is gathered and written in one go per frame, so a slow console
 * never shows half a row. */
static char out[(WL_TUI_MAX_W * 16U) + 64U];
static size_t out_len;

static void emit(const char *s, size_t n)
{
    if (out_len + n > sizeof(out)) {
        (void)!write(term_fd, out, out_len);
        out_len = 0;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void emits(const char *s)
{
    emit(s, strlen(s));
}

static void flush_out(void)
{
    size_t done = 0;

    while (done < out_len) {
        ssize_t n = write(term_fd, out + done, out_len - done);

        if (n <= 0)
            break;
        done += (size_t)n;
    }
    out_len = 0;
}

int wl_term_open(int fd)
{
    struct termios raw;

    if (!isatty(fd) || tcgetattr(fd, &saved) != 0)
        return 0;
    raw = saved;
    /* No line editing, no echo, and no signals from the keyboard: Ctrl-C at
     * the machine should not take the agent down, and every key is ours. */
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) != 0)
        return 0;
    term_fd = fd;
    shown_w = 0;
    shown_h = 0;
    /* Attributes off, cursor hidden, screen cleared. */
    emits("\033[0m\033[?25l\033[2J");
    flush_out();
    return 1;
}

void wl_term_close(void)
{
    if (term_fd < 0)
        return;
    emits("\033[0m\033[2J\033[H\033[?25h");
    flush_out();
    (void)tcsetattr(term_fd, TCSANOW, &saved);
    term_fd = -1;
}

void wl_term_size(unsigned *w, unsigned *h)
{
    struct winsize ws;

    *w = 80;
    *h = 25;
    if (term_fd >= 0 && ioctl(term_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
}

static void set_attr(uint8_t a)
{
    emits("\033[0m");
    if (a & WL_TUI_BOLD)
        emits("\033[1m");
    if (a & WL_TUI_INVERSE)
        emits("\033[7m");
}

void wl_term_draw(const struct wl_tui_grid *g, int full)
{
    char pos[24];

    if (term_fd < 0)
        return;
    if (g->w != shown_w || g->h != shown_h)
        full = 1;
    if (full)
        emits("\033[0m\033[2J");
    for (unsigned r = 0; r < g->h; r++) {
        int cur = -1;

        if (!full && memcmp(g->ch[r], shown.ch[r], g->w) == 0 &&
            memcmp(g->attr[r], shown.attr[r], g->w) == 0)
            continue;
        (void)snprintf(pos, sizeof(pos), "\033[%u;1H", r + 1U);
        emits(pos);
        /* The last cell of the last row is left alone: writing it scrolls
         * some terminals, the Linux console among them, by a line. */
        for (unsigned c = 0; c < g->w && !(r + 1U == g->h && c + 1U == g->w); c++) {
            if (g->attr[r][c] != cur) {
                set_attr(g->attr[r][c]);
                cur = g->attr[r][c];
            }
            emit(&g->ch[r][c], 1);
        }
        memcpy(shown.ch[r], g->ch[r], g->w);
        memcpy(shown.attr[r], g->attr[r], g->w);
    }
    emits("\033[0m");
    flush_out();
    shown_w = g->w;
    shown_h = g->h;
}

/* The sequences the Linux console and xterm-likes send for the keys the
 * screen uses: CSI A-D, CSI H/F, CSI 1~ 4~ 5~ 6~ 7~ 8~, SS3 H/F. */
static int decode(const unsigned char *b, size_t n)
{
    if (n == 1U || b[0] != 0x1bU)
        return b[0];
    if (n >= 3U && (b[1] == '[' || b[1] == 'O')) {
        switch (b[2]) {
        case 'A':
            return WL_TUI_KEY_UP;
        case 'B':
            return WL_TUI_KEY_DOWN;
        case 'H':
            return WL_TUI_KEY_HOME;
        case 'F':
            return WL_TUI_KEY_END;
        default:
            break;
        }
        if (n >= 4U && b[3] == '~') {
            switch (b[2]) {
            case '1':
            case '7':
                return WL_TUI_KEY_HOME;
            case '4':
            case '8':
                return WL_TUI_KEY_END;
            case '5':
                return WL_TUI_KEY_PGUP;
            case '6':
                return WL_TUI_KEY_PGDN;
            default:
                break;
            }
        }
    }
    return 0x1b;
}

int wl_term_key(unsigned timeout_ms)
{
    struct pollfd p;
    unsigned char b[16];
    ssize_t n;

    if (term_fd < 0)
        return -1;
    p.fd = term_fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, (int)timeout_ms) <= 0)
        return -1;
    /* An escape sequence arrives in one read from the console; a bare Esc
     * is one byte on its own. */
    n = read(term_fd, b, sizeof(b));
    if (n <= 0)
        return -1;
    return decode(b, (size_t)n);
}
