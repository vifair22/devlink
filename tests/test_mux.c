#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "w98link/agent.h"
#include "w98link/client_exec.h"
#include "w98link/clock.h"
#include "w98link/mux.h"
#include "w98link/proto.h"
#include "w98link/usock.h"

/*
 * The local multiplexer, end to end: the real agent loop on one side of a pty,
 * the real multiplexer on the other, and real clients reaching it through a
 * Unix socket. This is the arrangement that lets two terminals use one line,
 * so what it has to prove is that a second command neither waits for the first
 * nor kills it, which is what a second client used to do.
 */

static struct {
    struct wl_transport agent_t;
    struct wl_transport line_t;
    struct wl_agent_cfg agent_cfg;
    struct wl_mux_cfg mux_cfg;
    char sock[128];
    volatile int stop;
    pthread_t agent_th;
    pthread_t mux_th;
    wl_status agent_result;
    wl_status mux_result;
} h;

static int should_stop(void *ctx)
{
    (void)ctx;
    return h.stop;
}

static void *agent_main(void *arg)
{
    (void)arg;
    h.agent_result = wl_agent_serve(&h.agent_t, &h.agent_cfg, 0x51A7EU);
    return NULL;
}

static void *mux_main(void *arg)
{
    (void)arg;
    h.mux_result = wl_mux_serve(&h.line_t, &h.mux_cfg, 0x9AB12U);
    return NULL;
}

static struct wl_session_cfg fast_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0;
    return cfg;
}

static int setup(void **state)
{
    (void)state;
    memset(&h, 0, sizeof(h));
    pty_pair(&h.agent_t, &h.line_t);
    (void)snprintf(h.sock, sizeof(h.sock), "/tmp/w98link-test-%ld.sock", (long)getpid());
    (void)remove(h.sock);

    h.agent_cfg.session = fast_session();
    h.agent_cfg.platform = WL_PLATFORM_LINUX;
    h.agent_cfg.stop = should_stop;
    h.mux_cfg.session = fast_session();
    h.mux_cfg.socket_path = h.sock;
    h.mux_cfg.stop = should_stop;
    assert_int_equal(pthread_create(&h.agent_th, NULL, agent_main, NULL), 0);
    assert_int_equal(pthread_create(&h.mux_th, NULL, mux_main, NULL), 0);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    h.stop = 1;
    assert_int_equal(pthread_join(h.mux_th, NULL), 0);
    assert_int_equal(pthread_join(h.agent_th, NULL), 0);
    assert_int_equal(h.mux_result, WL_OK);
    assert_int_equal(h.agent_result, WL_OK);
    wl_transport_close(&h.agent_t);
    wl_transport_close(&h.line_t);
    (void)remove(h.sock);
    return 0;
}

static struct wl_session_cfg local_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0;
    cfg.level = 0;
    return cfg;
}

struct capture {
    char out[1 << 16];
    size_t len;
};

static void on_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    struct capture *c = ctx;

    (void)stream;
    if (c->len + n < sizeof(c->out)) {
        memcpy(c->out + c->len, p, n);
        c->len += n;
    }
}

/* One command through the multiplexer, in a thread, so two can overlap. */
struct job {
    const char *cmd;
    struct capture cap;
    struct wl_exec_result r;
    wl_status st;
    uint32_t done_ms;
    pthread_t th;
};

static void *run_job(void *arg)
{
    struct job *j = arg;
    struct wl_session_cfg cfg = local_session();
    struct wl_exec_io io = {&j->cap, on_output, NULL};
    struct wl_transport t;

    j->st = wl_unix_connect(h.sock, &t);
    if (j->st == WL_OK) {
        j->st = wl_client_exec(&t, &cfg, (uint32_t)(uintptr_t)j, j->cmd, NULL, 0, &io, 5000,
                               &j->r);
        wl_transport_close(&t);
    }
    j->done_ms = wl_clock_ms();
    return NULL;
}

/* The multiplexer needs its line session up before a client can be served. */
static void wait_for_the_line(void)
{
    uint32_t start = wl_clock_ms();
    struct wl_hello hello;
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;

    while (wl_clock_since(start) < 5000U) {
        if (wl_unix_connect(h.sock, &t) == WL_OK) {
            wl_status st = wl_client_info(&t, &cfg, 4242, 2000, &hello);

            wl_transport_close(&t);
            if (st == WL_OK)
                return;
        }
        (void)poll(NULL, 0, 20);
    }
    fail_msg("the multiplexer never came up");
}

static void test_a_client_reaches_the_agent_through_the_multiplexer(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;
    struct wl_hello hello;

    (void)state;
    wait_for_the_line();
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_info(&t, &cfg, 777, 3000, &hello), WL_OK);
    wl_transport_close(&t);
    /* The identity reported is the agent's, not the relay's: what the client
     * asked about is the far end of the line. */
    assert_int_equal(hello.platform, WL_PLATFORM_LINUX);
    assert_int_equal(hello.proto, WL_PROTO_VERSION);
}

/*
 * The point of the whole thing: a second command runs while the first is still
 * going, finishes on its own schedule, and does not disturb it. Before the
 * multiplexer, a second client displaced the first and its command was killed.
 */
static void test_two_clients_run_at_once_without_disturbing_each_other(void **state)
{
    struct job slow;
    struct job quick;

    (void)state;
    wait_for_the_line();
    memset(&slow, 0, sizeof(slow));
    memset(&quick, 0, sizeof(quick));
    slow.cmd = "for i in 1 2 3; do echo S$i; sleep 0.3; done";
    quick.cmd = "echo Q";

    assert_int_equal(pthread_create(&slow.th, NULL, run_job, &slow), 0);
    (void)poll(NULL, 0, 150); /* let it be well under way */
    assert_int_equal(pthread_create(&quick.th, NULL, run_job, &quick), 0);
    assert_int_equal(pthread_join(quick.th, NULL), 0);
    assert_int_equal(pthread_join(slow.th, NULL), 0);

    assert_int_equal(slow.st, WL_OK);
    assert_int_equal(quick.st, WL_OK);
    assert_true(slow.r.ran);
    assert_true(quick.r.ran);
    assert_int_equal(slow.r.exit_code, 0);
    assert_int_equal(quick.r.exit_code, 0);
    /* Whole and unmixed. */
    assert_int_equal(slow.cap.len, 9);
    assert_memory_equal(slow.cap.out, "S1\nS2\nS3\n", 9);
    assert_int_equal(quick.cap.len, 2);
    assert_memory_equal(quick.cap.out, "Q\n", 2);
    /* And the quick one did not queue behind the slow one. */
    assert_true(quick.done_ms < slow.done_ms);
}

/* A client that leaves takes its own command with it and nothing else. */
static void test_one_client_leaving_does_not_disturb_the_other(void **state)
{
    struct job slow;
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;
    struct wl_exec_result r;
    struct capture cap;
    struct wl_exec_io io = {&cap, on_output, NULL};

    (void)state;
    wait_for_the_line();
    memset(&slow, 0, sizeof(slow));
    memset(&cap, 0, sizeof(cap));
    slow.cmd = "for i in 1 2 3; do echo S$i; sleep 0.3; done";
    assert_int_equal(pthread_create(&slow.th, NULL, run_job, &slow), 0);
    (void)poll(NULL, 0, 100);

    /* A whole second client comes and goes while the first runs. */
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_exec(&t, &cfg, 31337, "echo gone", NULL, 0, &io, 5000, &r),
                     WL_OK);
    wl_transport_close(&t);
    assert_int_equal(r.exit_code, 0);

    assert_int_equal(pthread_join(slow.th, NULL), 0);
    assert_int_equal(slow.st, WL_OK);
    assert_int_equal(slow.r.exit_code, 0);
    assert_int_equal(slow.cap.len, 9);
    assert_memory_equal(slow.cap.out, "S1\nS2\nS3\n", 9);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_a_client_reaches_the_agent_through_the_multiplexer,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_two_clients_run_at_once_without_disturbing_each_other,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_one_client_leaving_does_not_disturb_the_other,
                                        setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
