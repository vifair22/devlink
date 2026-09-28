#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "devlink/agent.h"
#include "devlink/clock.h"
#include "devlink/monitor.h"
#include "devlink/proto.h"
#include "devlink/tui.h"

/*
 * The agent's text screen, rendered from a monitor fed events directly, as
 * test_monitor.c feeds it. What reaches the terminal is the backend's job;
 * everything the screen says is decided here, so it is asserted here.
 */

static struct wl_monitor *mon;
static struct wl_tui_grid *grid;
static struct wl_tui_view view;
static struct wl_tui_info info;

static int setup(void **state)
{
    (void)state;
    mon = wl_monitor_new();
    grid = calloc(1, sizeof(*grid));
    assert_non_null(mon);
    assert_non_null(grid);
    wl_tui_view_init(&view);
    memset(&info, 0, sizeof(info));
    info.version = "0.1.0_test";
    info.serial = "/dev/ttyS0 at 115200";
    info.tcp = "port 2098";
    info.tcp_on = 1;
    info.net = "eth0 10.0.2.15";
    info.memory = "480 MB free";
    info.cpu = "cpu 12%  load 0.21";
    info.clock = "21:40:07";
    /* The monitor stamps work with wl_clock_ms, so the ages are measured
     * against the same clock. */
    info.now_ms = wl_clock_ms();
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    wl_monitor_free(mon);
    free(grid);
    mon = NULL;
    grid = NULL;
    return 0;
}

static void send_event(uint8_t type, uint16_t ch, uint8_t kind, const char *text,
                       uint64_t done, uint64_t total)
{
    struct wl_agent_event e;

    memset(&e, 0, sizeof(e));
    e.type = type;
    e.ch = ch;
    e.kind = kind;
    e.text = text;
    e.done = done;
    e.total = total;
    wl_monitor_event(mon, &e);
}

static void note(const char *msg)
{
    wl_monitor_note(mon, 0, msg);
}

/* TUI_DUMP=1 prints every frame rendered, for reading a layout while
 * changing it. */
static void render(unsigned w, unsigned h)
{
    wl_tui_render(mon, &info, &view, grid, w, h);
    if (getenv("TUI_DUMP") != NULL)
        for (unsigned r = 0; r < grid->h; r++)
            print_message("%2u|%.*s|\n", r, (int)grid->w, grid->ch[r]);
}

/* Row r as a string, trailing spaces dropped. */
static const char *row(unsigned r)
{
    static char buf[WL_TUI_MAX_W + 1];
    size_t n = grid->w;

    assert_true(r < grid->h);
    memcpy(buf, grid->ch[r], n);
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
    return buf;
}

/* The first row that contains s, or -1. */
static int find(const char *s)
{
    for (unsigned r = 0; r < grid->h; r++)
        if (strstr(row(r), s) != NULL)
            return (int)r;
    return -1;
}

/* ---------------------------------------------------------------------------
 * the layout
 * ------------------------------------------------------------------------- */

static void test_an_idle_agent_fills_an_80_by_25_screen(void **state)
{
    (void)state;
    render(80, 25);
    assert_int_equal(grid->w, 80);
    assert_int_equal(grid->h, 25);
    assert_non_null(strstr(row(0), "devlink agent 0.1.0_test"));
    assert_non_null(strstr(row(0), "21:40:07"));
    assert_int_equal(grid->attr[0][0], WL_TUI_INVERSE);
    assert_non_null(strstr(row(1), "/dev/ttyS0 at 115200: waiting for a client"));
    assert_non_null(strstr(row(2), "port 2098: no clients"));
    assert_non_null(strstr(row(2), "eth0 10.0.2.15"));
    assert_non_null(strstr(row(3), "480 MB free"));
    assert_non_null(strstr(row(3), "cpu 12%  load 0.21"));
    assert_null(strstr(row(3), "command(s)"));
    assert_non_null(strstr(row(4), "0 command(s), 0 transfer(s), 0 B moved --"));
    assert_int_equal(find("-- work "), 4);
    assert_int_equal(find("nothing running"), 5);
    assert_int_equal(find("-- log "), 6);
    assert_non_null(strstr(row(24), "? help"));
    assert_int_equal(grid->attr[24][0], WL_TUI_INVERSE);
}

static void test_a_client_and_its_work_are_shown(void **state)
{
    (void)state;
    send_event(WL_EV_SESSION, 0, 0, NULL, 0, 0);
    send_event(WL_EV_READY, 0, 0, "0.1.0_20260928.0100.release", 0, 0);
    send_event(WL_EV_CHANNEL, 2, WL_KIND_PUT, "/tmp/DEVLINK.EFI", 0, 10U * 1024U * 1024U);
    send_event(WL_EV_PROGRESS, 2, 0, NULL, 4U * 1024U * 1024U, 0);
    send_event(WL_EV_CHANNEL, 4, WL_KIND_EXEC, "cat /proc/cpuinfo", 0, 0);
    render(80, 25);

    assert_non_null(strstr(row(1), "serving 0.1.0_20260928.0100.release"));
    assert_int_equal(find("-- work (2)"), 4);
    assert_non_null(strstr(row(5), "receiving"));
    assert_non_null(strstr(row(5), "/tmp/DEVLINK.EFI"));
    assert_non_null(strstr(row(5), "4.0 MB of 10.0 MB (40%)"));
    assert_non_null(strstr(row(6), "command"));
    assert_non_null(strstr(row(6), "cat /proc/cpuinfo"));
    assert_non_null(strstr(row(6), "Running (0 B out)"));
}

static void test_tcp_off_says_so(void **state)
{
    (void)state;
    info.tcp = "no DEVLINK.KEY";
    info.tcp_on = 0;
    render(80, 25);
    assert_non_null(strstr(row(2), "no DEVLINK.KEY: off"));
}

static void test_more_work_than_room_says_how_much_more(void **state)
{
    char name[32];

    (void)state;
    for (uint16_t ch = 2; ch < 2 + 16; ch += 2) {
        (void)snprintf(name, sizeof(name), "job %u", ch);
        send_event(WL_EV_CHANNEL, ch, WL_KIND_EXEC, name, 0, 0);
    }
    render(80, 25);
    /* 25 rows leave 18 for work and log, and work gets a third: 6 rows, so
     * eight jobs show five and say how many more. */
    assert_true(find("job 10") > 0);
    assert_int_equal(find("job 12"), -1);
    assert_true(find("... and 3 more") > 0);
    assert_true(find("-- log ") > find("... and 3 more"));
}

static void test_a_long_name_gives_way_to_the_progress(void **state)
{
    char name[300];

    (void)state;
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    send_event(WL_EV_CHANNEL, 2, WL_KIND_GET, name, 0, 2048);
    render(80, 25);
    assert_non_null(strstr(row(5), "0 B of 2.0 KB (0%)"));
    assert_non_null(strstr(row(5), "sending"));
}

static void test_control_bytes_never_reach_the_terminal(void **state)
{
    (void)state;
    note("evil\033[2Jname\x07");
    render(80, 25);
    for (unsigned r = 0; r < grid->h; r++)
        for (unsigned c = 0; c < grid->w; c++) {
            unsigned char ch = (unsigned char)grid->ch[r][c];

            assert_true(ch >= 0x20U && ch < 0x7fU);
        }
    assert_true(find("evil?[2Jname?") > 0);
}

static void test_a_screen_too_small_is_left_blank(void **state)
{
    (void)state;
    render(10, 5);
    for (unsigned r = 0; r < grid->h; r++)
        assert_string_equal(row(r), "");
}

static void test_a_huge_screen_is_clipped_to_the_grid(void **state)
{
    (void)state;
    render(1000, 1000);
    assert_int_equal(grid->w, WL_TUI_MAX_W);
    assert_int_equal(grid->h, WL_TUI_MAX_H);
    assert_non_null(strstr(row(WL_TUI_MAX_H - 1), "? help"));
}

/* ---------------------------------------------------------------------------
 * the log
 * ------------------------------------------------------------------------- */

static void notes(unsigned from, unsigned to)
{
    char buf[32];

    for (unsigned i = from; i <= to; i++) {
        (void)snprintf(buf, sizeof(buf), "line %03u", i);
        note(buf);
    }
}

static void test_the_log_shows_the_newest_lines(void **state)
{
    (void)state;
    notes(1, 40);
    render(80, 25);
    /* The last log row is 23, just above the bar. */
    assert_non_null(strstr(row(23), "line 040"));
    assert_non_null(strstr(row(7), "line 024"));
}

static void test_scrolling_back_holds_its_place_as_lines_arrive(void **state)
{
    (void)state;
    notes(1, 40);
    render(80, 25);
    (void)wl_tui_key(&view, WL_TUI_KEY_UP);
    (void)wl_tui_key(&view, WL_TUI_KEY_UP);
    render(80, 25);
    assert_non_null(strstr(row(23), "line 038"));
    assert_non_null(strstr(row(6), "2 line(s) back"));

    /* New lines arrive; the view stays on line 038. */
    notes(41, 45);
    render(80, 25);
    assert_non_null(strstr(row(23), "line 038"));

    (void)wl_tui_key(&view, WL_TUI_KEY_END);
    render(80, 25);
    assert_non_null(strstr(row(23), "line 045"));
    assert_null(strstr(row(6), "back"));
}

static void test_page_keys_move_a_pane_at_a_time_and_home_goes_to_the_top(void **state)
{
    (void)state;
    notes(1, 100);
    render(80, 25); /* the log pane is 17 rows */
    (void)wl_tui_key(&view, WL_TUI_KEY_PGUP);
    render(80, 25);
    assert_non_null(strstr(row(23), "line 083"));
    (void)wl_tui_key(&view, WL_TUI_KEY_PGDN);
    render(80, 25);
    assert_non_null(strstr(row(23), "line 100"));
    (void)wl_tui_key(&view, WL_TUI_KEY_HOME);
    render(80, 25);
    assert_non_null(strstr(row(7), "line 001"));
    (void)wl_tui_key(&view, WL_TUI_KEY_DOWN);
    render(80, 25);
    assert_non_null(strstr(row(7), "line 002"));
}

static void test_scrolling_a_short_log_goes_nowhere(void **state)
{
    (void)state;
    notes(1, 3);
    (void)wl_tui_key(&view, WL_TUI_KEY_PGUP);
    render(80, 25);
    assert_int_equal(view.back, 0);
    assert_non_null(strstr(row(7), "line 001"));
}

/* ---------------------------------------------------------------------------
 * keys that stop something ask first
 * ------------------------------------------------------------------------- */

static void test_each_stopping_key_asks_and_y_confirms(void **state)
{
    (void)state;
    assert_int_equal(wl_tui_key(&view, 'r'), WL_TUI_NONE);
    render(80, 25);
    assert_non_null(strstr(row(24), "Restart the agent?"));
    assert_int_equal(wl_tui_key(&view, 'y'), WL_TUI_RESTART);

    assert_int_equal(wl_tui_key(&view, 'P'), WL_TUI_NONE);
    render(80, 25);
    assert_non_null(strstr(row(24), "Power this machine off?"));
    /* Bright on black, not bold on inverse: the VGA console makes that
     * grey on light grey. */
    assert_int_equal(grid->attr[24][1], WL_TUI_BOLD);
    assert_int_equal(wl_tui_key(&view, 'Y'), WL_TUI_POWEROFF);

    assert_int_equal(wl_tui_key(&view, 'B'), WL_TUI_NONE);
    assert_int_equal(wl_tui_key(&view, 'y'), WL_TUI_REBOOT);
    render(80, 25);
    assert_non_null(strstr(row(24), "? help"));
}

static void test_any_other_key_leaves_it(void **state)
{
    (void)state;
    (void)wl_tui_key(&view, 'P');
    assert_int_equal(wl_tui_key(&view, 'n'), WL_TUI_NONE);
    assert_int_equal(view.asking, 0);
    /* Not even a second P confirms the first. */
    (void)wl_tui_key(&view, 'P');
    assert_int_equal(wl_tui_key(&view, 'P'), WL_TUI_NONE);
    assert_int_equal(view.asking, 0);
}

static void test_lower_case_p_and_b_do_nothing(void **state)
{
    (void)state;
    assert_int_equal(wl_tui_key(&view, 'p'), WL_TUI_NONE);
    assert_int_equal(wl_tui_key(&view, 'b'), WL_TUI_NONE);
    assert_int_equal(view.asking, 0);
}

static void test_help_replaces_the_log_and_any_key_only_closes_it(void **state)
{
    (void)state;
    notes(1, 5);
    (void)wl_tui_key(&view, '?');
    render(80, 25);
    assert_true(find("-- help ") > 0);
    assert_true(find("power this machine off") > 0);
    /* The key that closes the help does nothing else. */
    assert_int_equal(wl_tui_key(&view, 'P'), WL_TUI_NONE);
    assert_int_equal(view.asking, 0);
    render(80, 25);
    assert_true(find("line 005") > 0);
}

static void test_ctrl_l_asks_for_a_full_redraw(void **state)
{
    (void)state;
    assert_int_equal(wl_tui_key(&view, 12), WL_TUI_REDRAW);
}

/* ---------------------------------------------------------------------------
 * the formatting shared with the window
 * ------------------------------------------------------------------------- */

static void test_sizes_times_and_kinds_read_the_same_everywhere(void **state)
{
    char buf[64];
    struct wl_mon_row r;

    (void)state;
    wl_mon_bytes(512, buf, sizeof(buf));
    assert_string_equal(buf, "512 B");
    wl_mon_bytes(1536, buf, sizeof(buf));
    assert_string_equal(buf, "1.5 KB");
    wl_mon_bytes(3U * 1024U * 1024U * 1024U, buf, sizeof(buf));
    assert_string_equal(buf, "3.0 GB");
    wl_mon_elapsed(42000, buf, sizeof(buf));
    assert_string_equal(buf, "42s");
    wl_mon_elapsed(187000, buf, sizeof(buf));
    assert_string_equal(buf, "3m 07s");
    assert_string_equal(wl_mon_kind_name(WL_KIND_SYNC), "sync");
    assert_string_equal(wl_mon_kind_name(0xEE), "?");

    memset(&r, 0, sizeof(r));
    r.kind = WL_KIND_PUT;
    r.done = 700;
    wl_mon_progress(&r, buf, sizeof(buf));
    assert_string_equal(buf, "700 B");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_an_idle_agent_fills_an_80_by_25_screen, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_a_client_and_its_work_are_shown, setup, teardown),
        cmocka_unit_test_setup_teardown(test_tcp_off_says_so, setup, teardown),
        cmocka_unit_test_setup_teardown(test_more_work_than_room_says_how_much_more, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_a_long_name_gives_way_to_the_progress, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_control_bytes_never_reach_the_terminal, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_a_screen_too_small_is_left_blank, setup, teardown),
        cmocka_unit_test_setup_teardown(test_a_huge_screen_is_clipped_to_the_grid, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_the_log_shows_the_newest_lines, setup, teardown),
        cmocka_unit_test_setup_teardown(test_scrolling_back_holds_its_place_as_lines_arrive,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_page_keys_move_a_pane_at_a_time_and_home_goes_to_the_top, setup, teardown),
        cmocka_unit_test_setup_teardown(test_scrolling_a_short_log_goes_nowhere, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_each_stopping_key_asks_and_y_confirms, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_any_other_key_leaves_it, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lower_case_p_and_b_do_nothing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_help_replaces_the_log_and_any_key_only_closes_it,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_ctrl_l_asks_for_a_full_redraw, setup, teardown),
        cmocka_unit_test_setup_teardown(test_sizes_times_and_kinds_read_the_same_everywhere,
                                        setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
