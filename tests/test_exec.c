#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "w98link/clock.h"
#include "w98link/wire.h"
#include "w98link/agent.h"
#include "w98link/client_exec.h"
#include "w98link/proto.h"

/*
 * EXEC end to end: the real agent loop (with the POSIX process backend) in a
 * thread, the real client, and a pty between them. Everything the A21m runs
 * except the Win32 process code.
 */

struct agent_thread {
    struct wl_transport t;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
    pthread_t th;
};

static struct agent_thread agent;
static struct wl_transport client_t;

static int should_stop(void *ctx)
{
    return ((struct agent_thread *)ctx)->stop;
}

static void *agent_main(void *arg)
{
    struct agent_thread *a = arg;

    a->result = wl_agent_serve(&a->t, &a->cfg, 0xA6E47U);
    return NULL;
}

static struct wl_session_cfg fast_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0; /* a pty is far faster than any serial line */
    return cfg;
}

static int setup(void **state)
{
    (void)state;
    memset(&agent, 0, sizeof(agent));
    pty_pair(&agent.t, &client_t);
    agent.cfg.session = fast_session();
    agent.cfg.platform = WL_PLATFORM_LINUX;
    agent.cfg.stop = should_stop;
    agent.cfg.stop_ctx = &agent;
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, &agent), 0);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    agent.stop = 1;
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    wl_transport_close(&agent.t);
    wl_transport_close(&client_t);
    return 0;
}

/* Captured output of one command, per stream. */
struct capture {
    char out[1 << 20];
    size_t out_len;
    char err[4096];
    size_t err_len;
};

static struct capture cap;

static void on_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    struct capture *c = ctx;

    if (stream == WL_EXEC_STDOUT) {
        assert_true(c->out_len + n <= sizeof(c->out));
        memcpy(c->out + c->out_len, p, n);
        c->out_len += n;
    } else {
        assert_true(c->err_len + n <= sizeof(c->err));
        memcpy(c->err + c->err_len, p, n);
        c->err_len += n;
    }
}

static wl_status run(const char *cmd, const char *cwd, uint8_t flags, struct wl_exec_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_exec_io io = {&cap, on_output, NULL};
    static uint32_t seed = 1000;

    memset(&cap, 0, sizeof(cap));
    return wl_client_exec(&client_t, &cfg, ++seed, cmd, cwd, flags, &io, 5000, r);
}

static void test_output_streams_and_exit_code(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("printf 'hello\\n'; printf 'oops\\n' >&2; exit 3", NULL, 0, &r),
                     WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 3);
    assert_int_equal(cap.out_len, 6);
    assert_memory_equal(cap.out, "hello\n", 6);
    assert_int_equal(cap.err_len, 5);
    assert_memory_equal(cap.err, "oops\n", 5);
    assert_int_equal(r.agent.platform, WL_PLATFORM_LINUX);
    assert_int_equal(r.agent.proto, WL_PROTO_VERSION);
}

/* Much more output than any buffer on the path: every byte must arrive, in
 * order, before the exit status. */
static void test_large_output_arrives_complete(void **state)
{
    struct wl_exec_result r;
    char *want = malloc(1 << 20);
    size_t want_len = 0;

    (void)state;
    assert_non_null(want);
    for (int i = 1; i <= 60000; i++)
        want_len += (size_t)sprintf(want + want_len, "%d\n", i);
    assert_int_equal(run("seq 1 60000", NULL, 0, &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, want_len);
    assert_memory_equal(cap.out, want, want_len);
    free(want);
}

static void test_merge_keeps_interleaving(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("echo a; echo b >&2; echo c", NULL, WL_EXEC_MERGE_STDERR, &r), WL_OK);
    assert_int_equal(cap.out_len, 6);
    assert_memory_equal(cap.out, "a\nb\nc\n", 6);
    assert_int_equal(cap.err_len, 0);
}

static void test_working_directory(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("pwd", "/tmp", 0, &r), WL_OK);
    assert_int_equal(cap.out_len, 5);
    assert_memory_equal(cap.out, "/tmp\n", 5);
}

static void test_bad_directory_is_refused(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("pwd", "/nonexistent/dir", 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_PATH);
    assert_non_null(strstr(r.message, "/nonexistent/dir"));
}

/* v1 gives the command an empty stdin, so a reader sees EOF at once rather
 * than hanging the session. */
static void test_stdin_is_empty(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("cat; echo done", NULL, 0, &r), WL_OK);
    assert_int_equal(cap.out_len, 5);
    assert_memory_equal(cap.out, "done\n", 5);
}

/* Each client run is a new session to the agent; it must serve them one
 * after another without being restarted. */
static void test_consecutive_clients(void **state)
{
    struct wl_exec_result r;

    (void)state;
    for (int i = 0; i < 5; i++) {
        char cmd[32];

        (void)snprintf(cmd, sizeof(cmd), "exit %d", i + 10);
        assert_int_equal(run(cmd, NULL, 0, &r), WL_OK);
        assert_true(r.ran);
        assert_int_equal(r.exit_code, (uint32_t)(i + 10));
    }
}

/* @stop asks the agent to exit; the launcher on the target then copies a
 * new binary over it and starts it again. The client must still see the
 * command succeed. */
static void test_agent_stop_command(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@stop", NULL, 0, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    /* The agent returns on its own, without the test setting the stop flag. */
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    /* teardown joins again, which must not block. */
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, &agent), 0);
    agent.stop = 1;
}

static void test_unknown_agent_command_is_refused(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@wat", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_UNKNOWN);
}

/*
 * The invariant the whole design rests on: whatever a command is doing,
 * the agent answers the next client. Here a client starts a long-running
 * command and vanishes, as one would if it were killed.
 */
static void test_abandoned_command_does_not_block_the_next_client(void **state)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_peer_handlers h;
    struct wl_peer *p;
    uint8_t args[64];
    struct wl_wbuf w;
    uint16_t ch;
    uint32_t start;
    struct wl_hello hello;

    (void)state;
    memset(&h, 0, sizeof(h));
    p = wl_peer_new(&cfg, 31337, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    assert_non_null(p);
    start = wl_clock_ms();
    while (!wl_peer_ready(p) && wl_clock_since(start) < 3000U)
        assert_int_equal(wl_peer_pump(p, &client_t, 20), WL_OK);
    assert_true(wl_peer_ready(p));

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, "sleep 30", 8);
    wl_w_str(&w, "", 0);
    wl_w_u8(&w, 0);
    assert_int_equal(wl_peer_open(p, WL_KIND_EXEC, args, w.len, &ch), WL_OK);
    start = wl_clock_ms();
    while (wl_clock_since(start) < 300U)
        assert_int_equal(wl_peer_pump(p, &client_t, 20), WL_OK);
    wl_peer_free(p); /* the client is gone, the command is still running */

    /* A new client must be served promptly, not after the old command has
     * been confirmed dead. */
    start = wl_clock_ms();
    assert_int_equal(wl_client_info(&client_t, &cfg, 4242, 3000, &hello), WL_OK);
    assert_true(wl_clock_since(start) < 2500U);
    assert_int_equal(hello.platform, WL_PLATFORM_LINUX);
}

/* The flag the client watches, set the way a Ctrl-C would set it: from
 * somewhere else while wl_client_exec is pumping. */
static volatile sig_atomic_t test_cancel;

static void *cancel_after_300ms(void *arg)
{
    (void)arg;
    usleep(300000);
    test_cancel = 1;
    return NULL;
}

/*
 * Cancelling must do three things: return quickly rather than waiting out the
 * command, tell the caller the agent confirmed it, and actually stop the work
 * on the agent's side. The marker file proves the third: the command would
 * create it two seconds in, and it must never appear.
 */
static void test_cancel_stops_the_command_on_the_agent(void **state)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_exec_io io = {&cap, on_output, &test_cancel};
    struct wl_exec_result r;
    struct wl_hello hello;
    char marker[64];
    char cmd[128];
    pthread_t th;
    uint32_t start;

    (void)state;
    memset(&cap, 0, sizeof(cap));
    test_cancel = 0;
    sprintf(marker, "/tmp/w98link-cancel-%ld", (long)getpid());
    (void)remove(marker);
    sprintf(cmd, "sleep 2; touch %s", marker);

    assert_int_equal(pthread_create(&th, NULL, cancel_after_300ms, NULL), 0);
    start = wl_clock_ms();
    assert_int_equal(wl_client_exec(&client_t, &cfg, 7777, cmd, NULL, 0, &io, 5000, &r),
                     WL_OK);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_true(wl_clock_since(start) < 2000U);
    assert_true(r.ran);
    assert_true(r.cancelled);
    assert_true(r.cancel_acked);
    assert_int_equal(r.exit_code, WL_EXEC_EXIT_CANCELLED);

    /* The agent must still answer, and the command must be dead rather than
     * merely disowned. */
    start = wl_clock_ms();
    assert_int_equal(wl_client_info(&client_t, &cfg, 8888, 3000, &hello), WL_OK);
    assert_true(wl_clock_since(start) < 2500U);
    usleep(2500000);
    assert_int_equal(access(marker, F_OK), -1);
    (void)remove(marker);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_output_streams_and_exit_code, setup, teardown),
        cmocka_unit_test_setup_teardown(test_large_output_arrives_complete, setup, teardown),
        cmocka_unit_test_setup_teardown(test_merge_keeps_interleaving, setup, teardown),
        cmocka_unit_test_setup_teardown(test_working_directory, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bad_directory_is_refused, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stdin_is_empty, setup, teardown),
        cmocka_unit_test_setup_teardown(test_consecutive_clients, setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_stop_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_unknown_agent_command_is_refused, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_abandoned_command_does_not_block_the_next_client,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_stops_the_command_on_the_agent, setup,
                                        teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
