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

    cfg.link.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.link.window = WL_LINK_DEFAULT_WINDOW;
    cfg.link.bytes_per_sec = 0; /* a pty is far faster than any serial line */
    cfg.level = WL_SESSION_DEFAULT_LEVEL;
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
    struct wl_exec_io io = {&cap, on_output};
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
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
