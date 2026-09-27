#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/agent.h"
#include "devlink/client_exec.h"
#include "devlink/clock.h"
#include "devlink/posix_fd.h"
#include "devlink/proto.h"
#include "devlink/tcplink.h"

/*
 * The agent serving serial and TCP at once (docs/protocol.md, "Sessions and
 * transports"): the real serving loop in a thread, the serial side on a pty,
 * and TCP connections handed to it by a stand-in listener as socketpairs, so
 * everything above the socket calls is the code the target runs. The real
 * sockets are tested in test_tcp.c.
 */

static const uint8_t key[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

/* ---------------------------------------------------------------------------
 * a listener made of socketpairs
 * ------------------------------------------------------------------------- */

#define MAX_PENDING 16

struct fake_listener {
    pthread_mutex_t mu;
    int pending[MAX_PENDING]; /* the agent's ends, waiting to be accepted */
    int n;
    int bell[2]; /* a byte per pending connection: what the agent waits on */
    unsigned serial_no;
    int same_host; /* every connection from one address, new ports */
};

static struct fake_listener lst;

static wl_status fake_accept(void *ctx, struct wl_transport *out, char *peer, size_t cap)
{
    struct fake_listener *l = ctx;
    int fd = -1;
    char b;

    (void)pthread_mutex_lock(&l->mu);
    if (l->n > 0) {
        fd = l->pending[0];
        memmove(l->pending, l->pending + 1, (size_t)(l->n - 1) * sizeof(int));
        l->n--;
    }
    (void)pthread_mutex_unlock(&l->mu);
    if (fd < 0)
        return WL_OK;
    /* One bell byte per queued connection, so it is there to take; read
     * outside the lock, since read may block. The byte only wakes poll. */
    ssize_t got = read(l->bell[0], &b, 1);
    (void)got;
    ++l->serial_no;
    if (l->same_host)
        (void)snprintf(peer, cap, "10.9.9.9:%u", 40000U + l->serial_no);
    else
        (void)snprintf(peer, cap, "10.0.0.%u:5000", l->serial_no);
    return wl_fd_transport(fd, out);
}

/* A client connecting: returns its end, and queues the agent's. */
static int dial(void)
{
    int sv[2];

    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
    (void)pthread_mutex_lock(&lst.mu);
    assert_true(lst.n < MAX_PENDING);
    lst.pending[lst.n++] = sv[1];
    assert_int_equal(write(lst.bell[1], "x", 1), 1);
    (void)pthread_mutex_unlock(&lst.mu);
    return sv[0];
}

/* ---------------------------------------------------------------------------
 * the agent in a thread
 * ------------------------------------------------------------------------- */

struct agent_thread {
    struct wl_transport serial;
    struct wl_agent_listener listener;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
    pthread_t th;
    pthread_mutex_t log_mu;
    char log[8192];
    size_t log_len;
};

static struct agent_thread agent;
static struct wl_transport serial_client;

static int should_stop(void *ctx)
{
    return ((struct agent_thread *)ctx)->stop;
}

static void on_log(void *ctx, const char *msg)
{
    struct agent_thread *a = ctx;
    size_t n = strlen(msg);

    (void)pthread_mutex_lock(&a->log_mu);
    if (a->log_len + n + 2 < sizeof(a->log)) {
        memcpy(a->log + a->log_len, msg, n);
        a->log_len += n;
        a->log[a->log_len++] = '\n';
        a->log[a->log_len] = '\0';
    }
    (void)pthread_mutex_unlock(&a->log_mu);
}

static void *agent_main(void *arg)
{
    struct agent_thread *a = arg;

    a->result = wl_agent_run(&a->serial, &a->listener, &a->cfg, 0x5E551U);
    return NULL;
}

static int setup(void **state)
{
    (void)state;
    memset(&agent, 0, sizeof(agent));
    memset(&lst, 0, sizeof(lst));
    (void)pthread_mutex_init(&lst.mu, NULL);
    (void)pthread_mutex_init(&agent.log_mu, NULL);
    assert_int_equal(pipe(lst.bell), 0);
    pty_pair(&agent.serial, &serial_client);
    wl_session_cfg_init(&agent.cfg.session);
    agent.cfg.session.link.bytes_per_sec = 0;
    agent.cfg.platform = WL_PLATFORM_LINUX;
    agent.cfg.stop = should_stop;
    agent.cfg.stop_ctx = &agent;
    agent.cfg.log = on_log;
    agent.cfg.log_ctx = &agent;
    agent.listener.accept = fake_accept;
    agent.listener.ctx = &lst;
    agent.listener.fd = lst.bell[0];
    agent.listener.psk = key;
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, &agent), 0);
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    agent.stop = 1;
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    wl_transport_close(&agent.serial);
    wl_transport_close(&serial_client);
    (void)close(lst.bell[0]);
    (void)close(lst.bell[1]);
    for (int i = 0; i < lst.n; i++)
        (void)close(lst.pending[i]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * clients
 * ------------------------------------------------------------------------- */

struct client {
    const char *cmd;
    pthread_t th;
    size_t out_len;
    int tcp;
    wl_status st;
    uint32_t took_ms;
    struct wl_exec_result r;
    char out[4096];
};

static void on_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    struct client *c = ctx;

    (void)stream;
    if (c->out_len + n < sizeof(c->out)) {
        memcpy(c->out + c->out_len, p, n);
        c->out_len += n;
        c->out[c->out_len] = '\0';
    }
}

static void *client_main(void *arg)
{
    static unsigned seed = 100;
    struct client *c = arg;
    struct wl_tcplink_cfg tcfg;
    struct wl_session_cfg cfg;
    struct wl_transport t;
    struct wl_exec_io io;
    uint32_t t0 = wl_clock_ms();

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0;
    memset(&io, 0, sizeof(io));
    io.output = on_output;
    io.ctx = c;
    if (c->tcp) {
        wl_tcplink_cfg_init(&tcfg);
        tcfg.initiator = 1;
        tcfg.psk = key;
        cfg.make_link = wl_tcplink_make;
        cfg.make_link_arg = &tcfg;
        if (wl_fd_transport(dial(), &t) != WL_OK) {
            c->st = WL_ERR_IO;
            return NULL;
        }
        c->st = wl_client_exec(&t, &cfg, __atomic_add_fetch(&seed, 1, __ATOMIC_SEQ_CST), c->cmd,
                               NULL, 0, &io, 5000, &c->r);
        wl_transport_close(&t);
    } else {
        c->st = wl_client_exec(&serial_client, &cfg, __atomic_add_fetch(&seed, 1, __ATOMIC_SEQ_CST),
                               c->cmd, NULL, 0, &io, 5000, &c->r);
    }
    c->took_ms = wl_clock_since(t0);
    return NULL;
}

static void start(struct client *c, int tcp, const char *cmd)
{
    memset(c, 0, sizeof(*c));
    c->tcp = tcp;
    c->cmd = cmd;
    assert_int_equal(pthread_create(&c->th, NULL, client_main, c), 0);
}

static void finish(struct client *c)
{
    assert_int_equal(pthread_join(c->th, NULL), 0);
}

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_one_tcp_session(void **state)
{
    struct client c;

    (void)state;
    start(&c, 1, "echo over tcp");
    finish(&c);
    assert_int_equal(c.st, WL_OK);
    assert_int_equal(c.r.exit_code, 0);
    assert_string_equal(c.out, "over tcp\n");
    assert_non_null(strstr(agent.log, "[10.0.0.1:5000] connected"));
}

static void test_serial_and_two_tcp_sessions_at_once(void **state)
{
    struct client c[3];
    uint32_t t0 = wl_clock_ms();

    (void)state;
    /* Each takes most of a second; run one after another they would take
     * three, so finishing well inside two means they ran together. */
    start(&c[0], 0, "sh -c 'sleep 0.8; echo serial'");
    start(&c[1], 1, "sh -c 'sleep 0.8; echo first'");
    start(&c[2], 1, "sh -c 'sleep 0.8; echo second'");
    for (int i = 0; i < 3; i++)
        finish(&c[i]);
    assert_true(wl_clock_since(t0) < 2000U);
    for (int i = 0; i < 3; i++) {
        assert_int_equal(c[i].st, WL_OK);
        assert_int_equal(c[i].r.exit_code, 0);
    }
    /* Each output whole, on its own connection only. */
    assert_string_equal(c[0].out, "serial\n");
    assert_string_equal(c[1].out, "first\n");
    assert_string_equal(c[2].out, "second\n");
}

static void test_commands_share_eight_slots_across_sessions(void **state)
{
    struct client c[WL_AGENT_MAX_EXEC + 1];
    int refused = 0;

    (void)state;
    /* Nine sessions, one command each, all running at once: the ninth
     * finds the agent's eight slots full, whichever session it is on. */
    start(&c[0], 0, "sleep 1.5");
    for (unsigned i = 1; i <= WL_AGENT_MAX_EXEC; i++)
        start(&c[i], 1, "sleep 1.5");
    for (unsigned i = 0; i <= WL_AGENT_MAX_EXEC; i++) {
        finish(&c[i]);
        assert_int_equal(c[i].st, WL_OK);
        if (!c[i].r.ran) {
            refused++;
            assert_non_null(strstr(c[i].r.message, "too many commands"));
        }
    }
    assert_int_equal(refused, 1);
}

static void test_too_many_connections_are_refused(void **state)
{
    struct client c[WL_AGENT_MAX_CONN + 1];
    int failed = 0;

    (void)state;
    for (unsigned i = 0; i <= WL_AGENT_MAX_CONN; i++)
        start(&c[i], 1, "sh -c 'sleep 1'");
    for (unsigned i = 0; i <= WL_AGENT_MAX_CONN; i++) {
        finish(&c[i]);
        if (c[i].st != WL_OK)
            failed++;
    }
    assert_true(failed >= 1);
    assert_non_null(strstr(agent.log, "refused: 8 TCP sessions already"));
}

static void test_a_tcp_client_leaving_ends_only_its_session(void **state)
{
    struct client s;
    struct client c;
    int fd;

    (void)state;
    /* A connection that is opened and dropped before saying anything. */
    fd = dial();
    (void)close(fd);
    start(&s, 0, "sh -c 'sleep 0.5; echo still here'");
    start(&c, 1, "echo and here");
    finish(&s);
    finish(&c);
    assert_int_equal(s.st, WL_OK);
    assert_string_equal(s.out, "still here\n");
    assert_int_equal(c.st, WL_OK);
    assert_string_equal(c.out, "and here\n");
    assert_non_null(strstr(agent.log, "session ended"));
}

static void test_wrong_key_gets_nothing(void **state)
{
    static const uint8_t wrong[32] = {9};
    struct wl_tcplink_cfg tcfg;
    struct wl_session_cfg cfg;
    struct wl_transport t;
    struct wl_hello hello;

    (void)state;
    wl_session_cfg_init(&cfg);
    wl_tcplink_cfg_init(&tcfg);
    tcfg.initiator = 1;
    tcfg.psk = wrong;
    cfg.make_link = wl_tcplink_make;
    cfg.make_link_arg = &tcfg;
    assert_int_equal(wl_fd_transport(dial(), &t), WL_OK);
    assert_int_not_equal(wl_client_info(&t, &cfg, 99, 3000, &hello), WL_OK);
    wl_transport_close(&t);
}

static wl_status info_with(const uint8_t *k)
{
    struct wl_tcplink_cfg tcfg;
    struct wl_session_cfg cfg;
    struct wl_transport t;
    struct wl_hello hello;
    wl_status st;

    wl_session_cfg_init(&cfg);
    wl_tcplink_cfg_init(&tcfg);
    tcfg.initiator = 1;
    tcfg.psk = k;
    cfg.make_link = wl_tcplink_make;
    cfg.make_link_arg = &tcfg;
    assert_int_equal(wl_fd_transport(dial(), &t), WL_OK);
    st = wl_client_info(&t, &cfg, 99, 1500, &hello);
    wl_transport_close(&t);
    return st;
}

static void test_a_wrong_key_costs_the_address_a_pause(void **state)
{
    static const uint8_t wrong[32] = {9};
    uint32_t t0;

    (void)state;
    lst.same_host = 1;
    assert_int_not_equal(info_with(wrong), WL_OK);
    t0 = wl_clock_ms();
    /* The right key, straight after, from the same address: ignored. */
    assert_int_not_equal(info_with(key), WL_OK);
    assert_true(wl_clock_since(t0) < 2000U);
    while (wl_clock_since(t0) < 2200U)
        (void)usleep(50000);
    assert_int_equal(info_with(key), WL_OK);
    assert_non_null(strstr(agent.log, "does not hold the same passphrase"));
}

static void test_stop_over_tcp_stops_the_agent(void **state)
{
    struct client c;

    (void)state;
    start(&c, 1, "@stop");
    finish(&c);
    assert_int_equal(c.st, WL_OK);
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    /* teardown joins again; give it a thread that has nothing to do. */
    agent.stop = 1;
    agent.result = WL_OK;
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, &agent), 0);
}

static void test_nothing_to_serve_is_an_error(void **state)
{
    struct wl_agent_cfg cfg;
    struct wl_agent_listener l;

    (void)state;
    memset(&cfg, 0, sizeof(cfg));
    memset(&l, 0, sizeof(l));
    assert_int_equal(wl_agent_run(NULL, NULL, &cfg, 1), WL_ERR_INVAL);
    /* A listener without a key: no TCP without a passphrase. */
    assert_int_equal(wl_agent_run(NULL, &l, &cfg, 1), WL_ERR_INVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_one_tcp_session, setup, teardown),
        cmocka_unit_test_setup_teardown(test_serial_and_two_tcp_sessions_at_once, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_commands_share_eight_slots_across_sessions, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_too_many_connections_are_refused, setup, teardown),
        cmocka_unit_test_setup_teardown(test_a_tcp_client_leaving_ends_only_its_session, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_wrong_key_gets_nothing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_a_wrong_key_costs_the_address_a_pause, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_stop_over_tcp_stops_the_agent, setup, teardown),
        cmocka_unit_test(test_nothing_to_serve_is_an_error),
    };

    signal(SIGPIPE, SIG_IGN);
    return cmocka_run_group_tests(tests, NULL, NULL);
}
