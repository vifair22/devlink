#ifndef DEVLINK_LINUX_TUI_TERM_H
#define DEVLINK_LINUX_TUI_TERM_H

#include "devlink/tui.h"

/*
 * A wl_tui_grid on a terminal: the Linux console the rescue image boots to,
 * or any VT100-like terminal. Raw input, the cursor hidden, and only the rows
 * that changed redrawn, since a 9600-baud serial console or a slow VGA text
 * screen should not repaint 2000 characters four times a second.
 */

/* Takes over the terminal on fd (both directions). 0 when fd is not a
 * terminal, which is the caller's cue to stay with the line log. */
int wl_term_open(int fd);

/* Gives the terminal back as it was found: modes, cursor, attributes. */
void wl_term_close(void);

/* The terminal's size, 80x25 when it will not say. */
void wl_term_size(unsigned *w, unsigned *h);

/* Draws g, rows that differ from what was drawn last; everything when full
 * is nonzero or the size changed. */
void wl_term_draw(const struct wl_tui_grid *g, int full);

/* Waits up to timeout_ms for a key. Returns it (ASCII, or WL_TUI_KEY_*), or
 * -1 when none came. */
int wl_term_key(unsigned timeout_ms);

#endif
