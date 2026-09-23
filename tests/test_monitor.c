#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/agent.h"
#include "devlink/client_exec.h"
#include "devlink/monitor.h"
#include "devlink/proto.h"
#include "devlink/version.h"

/*
 * The model behind the agent's window. Two halves: the model on its own, fed
 * events directly, and then the real agent loop over a pty feeding it, which
 * is the half that proves the events are emitted where they are claimed to be.
 */

/* ---------------------------------------------------------------------------
 * the model on its own
 * ------------------------------------------------------------------------- */

static struct wl_monitor *mon;

static int model_setup(void **state)
{
    (void)state;
    mon = wl_monitor_new();
    assert_non_null(mon);
    return 0;
}

static int model_teardown(void **state)
{
    (void)state;
    wl_monitor_free(mon);
    mon = NULL;
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

/* The stamp is the model's, not the display's, so that this can be asserted
 * rather than eyeballed on the target. 14:23:05 on any day. */
static void test_a_log_line_is_stamped_with_the_time_it_happened(void **state)
{
    (void)state;
    wl_monitor_note(mon, 1758643385ULL - 1758643385ULL % 86400ULL + 51785ULL, "exec: dir");
    assert_int_equal(wl_monitor_log_lines(mon), 1);
    assert_string_equal(wl_monitor_log(mon, 0), "14:23:05 exec: dir");
    assert_null(wl_monitor_log(mon, 1));
}

static void test_the_log_keeps_the_newest_lines_when_it_fills(void **state)
{
    struct wl_mon_totals tot;
    char want[32];

    (void)state;
    for (unsigned i = 0; i < WL_MON_LOG_LINES + 10U; i++) {
        char msg[32];

        (void)snprintf(msg, sizeof(msg), "line %u", i);
        wl_monitor_note(mon, 0, msg);
    }
    assert_int_equal(wl_monitor_log_lines(mon), WL_MON_LOG_LINES);

    /* Oldest first, so a window can walk 0..lines-1 straight down the pane.
     * The ten that fell off the back are the ten oldest, not the newest. */
    (void)snprintf(want, sizeof(want), "00:00:00 line %u", 10U);
    assert_string_equal(wl_monitor_log(mon, 0), want);
    (void)snprintf(want, sizeof(want), "00:00:00 line %u", WL_MON_LOG_LINES + 9U);
    assert_string_equal(wl_monitor_log(mon, WL_MON_LOG_LINES - 1U), want);
    wl_monitor_totals(mon, &tot);
    assert_int_equal(tot.dropped, 10);
    assert_int_equal(tot.notes, WL_MON_LOG_LINES + 10U);
}

static void test_a_line_longer_than_the_ring_is_truncated_not_written_past(void **state)
{
    char huge[WL_MON_LINE_MAX * 2];

    (void)state;
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    wl_monitor_note(mon, 0, huge);
    assert_int_equal(strlen(wl_monitor_log(mon, 0)), WL_MON_LINE_MAX - 1U);
}

static void test_the_state_follows_the_session(void **state)
{
    (void)state;
    assert_int_equal(wl_monitor_state(mon), WL_MON_WAITING);
    assert_string_equal(wl_monitor_client(mon), "");

    send_event(WL_EV_SESSION, 0, 0, NULL, 0, 0);
    assert_int_equal(wl_monitor_state(mon), WL_MON_LINKED);

    send_event(WL_EV_READY, 0, 0, "0.1.0_20260923.1633.release", 0, 0);
    assert_int_equal(wl_monitor_state(mon), WL_MON_SERVING);
    assert_string_equal(wl_monitor_client(mon), "0.1.0_20260923.1633.release");

    send_event(WL_EV_GONE, 0, 0, NULL, 0, 0);
    assert_int_equal(wl_monitor_state(mon), WL_MON_WAITING);
    assert_string_equal(wl_monitor_client(mon), "");
}

/* A display that does not sort itself must not have a row jump out from under
 * the pointer because some unrelated command elsewhere finished. */
static void test_rows_keep_the_order_the_work_started_in(void **state)
{
    (void)state;
    send_event(WL_EV_CHANNEL, 2, WL_KIND_EXEC, "first", 0, 0);
    send_event(WL_EV_CHANNEL, 4, WL_KIND_PUT, "C:\\SECOND.TXT", 0, 4096);
    send_event(WL_EV_CHANNEL, 6, WL_KIND_EXEC, "third", 0, 0);
    assert_int_equal(wl_monitor_rows(mon), 3);

    send_event(WL_EV_FINISHED, 4, 0, NULL, 0, 0);
    assert_int_equal(wl_monitor_rows(mon), 2);
    assert_string_equal(wl_monitor_row(mon, 0)->name, "first");
    assert_string_equal(wl_monitor_row(mon, 1)->name, "third");
    assert_int_equal(wl_monitor_row(mon, 1)->ch, 6);
    assert_null(wl_monitor_row(mon, 2));
}

static void test_a_transfer_reports_its_share_and_its_whole(void **state)
{
    struct wl_mon_totals tot;

    (void)state;
    send_event(WL_EV_CHANNEL, 2, WL_KIND_GET, "C:\\BIG.BIN", 0, 10000);
    send_event(WL_EV_PROGRESS, 2, 0, NULL, 4096, 0);
    send_event(WL_EV_PROGRESS, 2, 0, NULL, 8192, 0);
    assert_int_equal(wl_monitor_row(mon, 0)->done, 8192);
    assert_int_equal(wl_monitor_row(mon, 0)->total, 10000);

    /* Progress is cumulative per channel, so the lifetime total moves by the
     * difference. Counting each report in full would have said 12288. */
    wl_monitor_totals(mon, &tot);
    assert_int_equal(tot.bytes, 8192);
    assert_int_equal(tot.transfers, 1);
    assert_int_equal(tot.commands, 0);
}

static void test_a_new_client_takes_the_old_ones_work_with_it(void **state)
{
    struct wl_mon_totals tot;

    (void)state;
    send_event(WL_EV_SESSION, 0, 0, NULL, 0, 0);
    send_event(WL_EV_CHANNEL, 2, WL_KIND_EXEC, "a command nobody will read", 0, 0);
    assert_int_equal(wl_monitor_rows(mon), 1);

    /* The agent kills the previous client's commands when a session starts
     * (on_session), so anything still listed here would be a ghost. */
    send_event(WL_EV_SESSION, 0, 0, NULL, 0, 0);
    assert_int_equal(wl_monitor_rows(mon), 0);
    wl_monitor_totals(mon, &tot);
    assert_int_equal(tot.sessions, 2);
}

/* The window redraws when the generation moves, so an event about work this
 * model never saw must not move it: on the target that is the difference
 * between repainting a list per tick and repainting it per stray record. */
static void test_nothing_changing_leaves_the_generation_alone(void **state)
{
    uint32_t before;

    (void)state;
    send_event(WL_EV_CHANNEL, 2, WL_KIND_EXEC, "live", 0, 0);
    before = wl_monitor_generation(mon);

    send_event(WL_EV_PROGRESS, 99, 0, NULL, 1234, 0);
    send_event(WL_EV_FINISHED, 99, 0, NULL, 0, 0);
    send_event(200, 2, 0, NULL, 0, 0);
    assert_int_equal(wl_monitor_generation(mon), before);

    send_event(WL_EV_PROGRESS, 2, 0, NULL, 1, 0);
    assert_true(wl_monitor_generation(mon) != before);
}

static void test_more_work_than_rows_does_not_run_off_the_end(void **state)
{
    (void)state;
    for (unsigned i = 0; i < WL_MON_ROWS + 8U; i++)
        send_event(WL_EV_CHANNEL, (uint16_t)(2U + 2U * i), WL_KIND_EXEC, "c", 0, 0);
    assert_int_equal(wl_monitor_rows(mon), WL_MON_ROWS);
    assert_non_null(wl_monitor_row(mon, WL_MON_ROWS - 1U));
    assert_null(wl_monitor_row(mon, WL_MON_ROWS));
}

static void test_a_restart_clears_the_work_but_keeps_the_log(void **state)
{
    (void)state;
    wl_monitor_note(mon, 0, "something worth keeping");
    send_event(WL_EV_SESSION, 0, 0, NULL, 0, 0);
    send_event(WL_EV_CHANNEL, 2, WL_KIND_EXEC, "interrupted", 0, 0);

    wl_monitor_reset(mon);
    assert_int_equal(wl_monitor_state(mon), WL_MON_WAITING);
    assert_int_equal(wl_monitor_rows(mon), 0);
    assert_int_equal(wl_monitor_log_lines(mon), 1);
}

/* ---------------------------------------------------------------------------
 * the real agent feeding it
 * ------------------------------------------------------------------------- */

static struct {
    struct wl_transport t;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
    pthread_t th;
} agent;

static struct wl_transport client_t;

/*
 * The agent writes from its own thread and the test reads from this one, which
 * is the same arrangement the window has and the same lock it needs: the model
 * is deliberately not thread-safe, so whoever owns both ends serializes them.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void on_event(void *ctx, const struct wl_agent_event *e)
{
    (void)pthread_mutex_lock(&lock);
    wl_monitor_event(ctx, e);
    (void)pthread_mutex_unlock(&lock);
}

static void on_log(void *ctx, const char *msg)
{
    (void)pthread_mutex_lock(&lock);
    wl_monitor_note(ctx, 0, msg);
    (void)pthread_mutex_unlock(&lock);
}

static void *agent_main(void *arg)
{
    (void)arg;
    agent.result = wl_agent_serve(&agent.t, &agent.cfg, 0x30A17U);
    return NULL;
}

static int should_stop(void *ctx)
{
    (void)ctx;
    return agent.stop;
}

static int live_setup(void **state)
{
    (void)state;
    mon = wl_monitor_new();
    assert_non_null(mon);
    memset(&agent, 0, sizeof(agent));
    pty_pair(&agent.t, &client_t);
    wl_session_cfg_init(&agent.cfg.session);
    agent.cfg.session.link.bytes_per_sec = 0;
    agent.cfg.platform = WL_PLATFORM_LINUX;
    agent.cfg.stop = should_stop;
    agent.cfg.log = on_log;
    agent.cfg.log_ctx = mon;
    agent.cfg.event = on_event;
    agent.cfg.event_ctx = mon;
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, &agent), 0);
    return 0;
}

static int live_teardown(void **state)
{
    (void)state;
    agent.stop = 1;
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    wl_transport_close(&agent.t);
    wl_transport_close(&client_t);
    wl_monitor_free(mon);
    mon = NULL;
    return 0;
}

struct seen {
    int rows;
    int kind;
    char name[WL_PATH_MAX];
    int state;
    char client[64];
};

static struct seen seen;

/*
 * Called with the command's first output in hand, so the command is certainly
 * still running: the sleep after the printf keeps it that way. This is the
 * whole point of the model, and the only moment from which it can be checked.
 */
static void on_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    (void)stream;
    (void)p;
    (void)n;
    if (seen.rows >= 0)
        return; /* only the first chunk; after that the answer changes */
    (void)pthread_mutex_lock(&lock);
    seen.rows = (int)wl_monitor_rows(mon);
    seen.state = wl_monitor_state(mon);
    (void)snprintf(seen.client, sizeof(seen.client), "%s", wl_monitor_client(mon));
    if (wl_monitor_rows(mon) > 0) {
        const struct wl_mon_row *r = wl_monitor_row(mon, 0);

        seen.kind = r->kind;
        (void)snprintf(seen.name, sizeof(seen.name), "%s", r->name);
    }
    (void)pthread_mutex_unlock(&lock);
    (void)ctx;
}

static void test_a_running_command_is_listed_while_it_runs(void **state)
{
    static const char *cmd = "printf 'go\\n'; sleep 0.4";
    struct wl_session_cfg cfg;
    struct wl_exec_io io = {NULL, on_output, NULL};
    struct wl_exec_result r;
    struct wl_mon_totals tot;

    (void)state;
    memset(&seen, 0, sizeof(seen));
    seen.rows = -1;
    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0;
    assert_int_equal(wl_client_exec(&client_t, &cfg, 0x51EEDU, cmd, NULL, 0, &io, 5000, &r),
                     WL_OK);
    assert_int_equal(r.exit_code, 0);

    /* What the window would have been showing mid-command. */
    assert_int_equal(seen.rows, 1);
    assert_int_equal(seen.kind, WL_KIND_EXEC);
    assert_string_equal(seen.name, cmd);
    assert_int_equal(seen.state, WL_MON_SERVING);
    assert_string_equal(seen.client, wl_version());

    /* And afterwards nothing is left listed, which is the half that a leak
     * would fail: the row has to be dropped when the command ends. */
    (void)pthread_mutex_lock(&lock);
    assert_int_equal(wl_monitor_rows(mon), 0);
    wl_monitor_totals(mon, &tot);
    assert_true(wl_monitor_log_lines(mon) > 0);
    (void)pthread_mutex_unlock(&lock);
    assert_int_equal(tot.commands, 1);
    assert_int_equal(tot.sessions, 1);
    assert_true(tot.bytes >= 3); /* "go\n" */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_a_log_line_is_stamped_with_the_time_it_happened,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_the_log_keeps_the_newest_lines_when_it_fills,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(
            test_a_line_longer_than_the_ring_is_truncated_not_written_past, model_setup,
            model_teardown),
        cmocka_unit_test_setup_teardown(test_the_state_follows_the_session, model_setup,
                                        model_teardown),
        cmocka_unit_test_setup_teardown(test_rows_keep_the_order_the_work_started_in,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_a_transfer_reports_its_share_and_its_whole,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_a_new_client_takes_the_old_ones_work_with_it,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_nothing_changing_leaves_the_generation_alone,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_more_work_than_rows_does_not_run_off_the_end,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_a_restart_clears_the_work_but_keeps_the_log,
                                        model_setup, model_teardown),
        cmocka_unit_test_setup_teardown(test_a_running_command_is_listed_while_it_runs,
                                        live_setup, live_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
