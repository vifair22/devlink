#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/agent.h"
#include "devlink/client_exec.h"
#include "devlink/clock.h"
#include "devlink/file_ops.h"
#include "devlink/mux.h"
#include "devlink/proto.h"
#include "devlink/usock.h"

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
    char dir[128];
    volatile int stop;
    /* Holds the agent back so the line is genuinely not up when a client
     * arrives. On a real 115200 line the handshake takes tens of milliseconds
     * and a client wins that race easily; over a pty it takes almost none, so
     * the race has to be made rather than hoped for. */
    volatile int agent_delay_ms;
    /*
     * An agent that comes back after it stops, as the launchers on the real
     * targets do: AGENT.BAT, DEVLINKW's self-update, init's respawn. Each
     * life has its own seed, so its link id differs and the multiplexer sees
     * a restarted peer, not the same one resuming. agent_leave ends a life
     * without @stop, which the multiplexer cannot see coming.
     */
    volatile int agent_restarts;
    volatile int agent_gap_ms;
    volatile int agent_leave;
    volatile int agent_lives;
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

static int agent_should_stop(void *ctx)
{
    (void)ctx;
    return h.stop || h.agent_leave;
}

static void *agent_main(void *arg)
{
    uint32_t seed = 0x51A7EU;

    (void)arg;
    if (h.agent_delay_ms > 0)
        (void)poll(NULL, 0, h.agent_delay_ms);
    for (;;) {
        h.agent_result = wl_agent_serve(&h.agent_t, &h.agent_cfg, seed++);
        h.agent_lives++;
        if (h.stop || h.agent_restarts == 0)
            break;
        h.agent_restarts--;
        (void)poll(NULL, 0, h.agent_gap_ms);
        h.agent_leave = 0;
    }
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

static int setup_common(int agent_delay_ms, uint32_t line_bps)
{
    memset(&h, 0, sizeof(h));
    h.agent_delay_ms = agent_delay_ms;
    pty_pair(&h.agent_t, &h.line_t);
    (void)snprintf(h.sock, sizeof(h.sock), "/tmp/devlink-test-%ld.sock", (long)getpid());
    (void)remove(h.sock);
    (void)snprintf(h.dir, sizeof(h.dir), "/tmp/devlink-mux-%ld", (long)getpid());
    (void)mkdir(h.dir, 0700);

    h.agent_cfg.session = fast_session();
    h.agent_cfg.session.link.bytes_per_sec = line_bps;
    h.agent_cfg.platform = WL_PLATFORM_LINUX;
    h.agent_cfg.stop = agent_should_stop;
    h.mux_cfg.session = fast_session();
    h.mux_cfg.session.link.bytes_per_sec = line_bps;
    h.mux_cfg.socket_path = h.sock;
    h.mux_cfg.stop = should_stop;
    assert_int_equal(pthread_create(&h.agent_th, NULL, agent_main, NULL), 0);
    assert_int_equal(pthread_create(&h.mux_th, NULL, mux_main, NULL), 0);
    return 0;
}

static int setup(void **state)
{
    (void)state;
    return setup_common(0, 0);
}

/* The multiplexer is listening well before the agent answers. */
static int setup_late_agent(void **state)
{
    (void)state;
    return setup_common(400, 0);
}

/* Both ends paced to a serial line's rate (half of 115200 baud's, so the test
 * is quick), so the line, not the pty, is what a transfer waits on. */
static int setup_paced(void **state)
{
    (void)state;
    return setup_common(0, 57600U);
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
    {
        char cmd[256];

        (void)snprintf(cmd, sizeof(cmd), "rm -rf %s", h.dir);
        assert_int_equal(system(cmd), 0);
    }
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
    struct wl_exec_io io = {&j->cap, on_output, NULL, NULL};
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

/* A job that has not finished in time is a hang, which is what these tests
 * are about; failing beats joining for ever. */
static void join_within(struct job *j, unsigned ms)
{
    struct timespec until;

    assert_int_equal(clock_gettime(CLOCK_REALTIME, &until), 0);
    until.tv_sec += (time_t)(ms / 1000U);
    until.tv_nsec += (long)(ms % 1000U) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    if (pthread_timedjoin_np(j->th, NULL, &until) != 0)
        fail_msg("'%s' was still waiting after %u ms", j->cmd, ms);
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

/*
 * More commands at once than the agent has slots for, as a survey that runs a
 * dozen checks side by side does. The multiplexer used to accept eight
 * clients and reset the rest, which they reported as "Connection reset by
 * peer"; the agent itself refuses a ninth command. Every one of them runs
 * now: those beyond the slots wait in the multiplexer and start as the first
 * finish, so the run takes two waves, not one.
 */
static void test_more_commands_than_slots_all_run_in_turn(void **state)
{
    enum { JOBS = WL_AGENT_MAX_EXEC + 4 };
    static struct job jobs[JOBS];
    static char cmds[JOBS][48];
    uint32_t start;
    uint32_t last = 0;

    (void)state;
    wait_for_the_line();
    memset(jobs, 0, sizeof(jobs));
    start = wl_clock_ms();
    for (unsigned i = 0; i < JOBS; i++) {
        (void)snprintf(cmds[i], sizeof(cmds[i]), "sleep 0.4; echo J%u", i);
        jobs[i].cmd = cmds[i];
        assert_int_equal(pthread_create(&jobs[i].th, NULL, run_job, &jobs[i]), 0);
    }
    for (unsigned i = 0; i < JOBS; i++)
        join_within(&jobs[i], 10000);
    for (unsigned i = 0; i < JOBS; i++) {
        char want[16];
        int n = snprintf(want, sizeof(want), "J%u\n", i);

        assert_int_equal(jobs[i].st, WL_OK);
        assert_true(jobs[i].r.ran);
        assert_int_equal(jobs[i].r.exit_code, 0);
        assert_int_equal(jobs[i].cap.len, (size_t)n);
        assert_memory_equal(jobs[i].cap.out, want, (size_t)n);
        if (jobs[i].done_ms - start > last)
            last = jobs[i].done_ms - start;
    }
    /* Queued, not all at once: the last wave started after the first ended. */
    assert_true(last >= 750U);
}

/* A client that leaves takes its own command with it and nothing else. */
static void test_one_client_leaving_does_not_disturb_the_other(void **state)
{
    struct job slow;
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;
    struct wl_exec_result r;
    struct capture cap;
    struct wl_exec_io io = {&cap, on_output, NULL, NULL};

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

/*
 * The race a real user hit: a command that starts the multiplexer connects at
 * once, before the line's session has been established, and used to be told
 * "the line is not up". No waiting here on purpose, so a regression puts it
 * straight back.
 */
static void test_a_client_arriving_before_the_line_is_up_is_served(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;
    struct wl_exec_result r;
    struct capture cap;
    struct wl_exec_io io = {&cap, on_output, NULL, NULL};
    uint32_t start = wl_clock_ms();

    (void)state;
    memset(&cap, 0, sizeof(cap));
    /* Only the socket is waited for, which is all the client can see, and is
     * exactly what the autostart waits for. The agent is 400 ms behind. */
    while (wl_clock_since(start) < 5000U) {
        if (wl_unix_connect(h.sock, &t) == WL_OK)
            break;
        (void)poll(NULL, 0, 5);
    }
    assert_true(wl_clock_since(start) < 5000U);
    assert_int_equal(wl_client_exec(&t, &cfg, 909, "echo early", NULL, 0, &io, 5000, &r),
                     WL_OK);
    wl_transport_close(&t);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.len, 6);
    assert_memory_equal(cap.out, "early\n", 6);
}

/*
 * A client that connects and goes away without saying anything must not take
 * the multiplexer with it. The autostart probe does exactly this, and it used
 * to be fatal: the multiplexer wrote its HELLO into the closed socket, took a
 * SIGPIPE and died, leaving a stale socket for the next attempt to trip over.
 *
 * The multiplexer runs in a thread of this process, so if a dead peer can still
 * raise SIGPIPE, this test dies rather than fails. That is the point: it proves
 * the library is safe by itself, not that main() remembered to say so.
 */
static void test_a_connection_that_closes_at_once_does_not_kill_the_multiplexer(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_transport probe;
    struct wl_transport t;
    struct wl_hello hello;

    (void)state;
    wait_for_the_line();
    for (int i = 0; i < 3; i++) {
        assert_int_equal(wl_unix_connect(h.sock, &probe), WL_OK);
        wl_transport_close(&probe); /* exactly what the autostart probe does */
        (void)poll(NULL, 0, 40);    /* long enough for it to be accepted */
    }
    /* Still serving. */
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_info(&t, &cfg, 5150, 3000, &hello), WL_OK);
    wl_transport_close(&t);
    assert_int_equal(hello.proto, WL_PROTO_VERSION);
}

/*
 * Channel numbers are finite (WL_PEER_MAX_CHANNELS) and a channel is released
 * only when both ends have closed it. A multiplexer keeps one session for as
 * long as it runs, so anything it fails to close is gone for good, and after
 * sixteen commands it can open nothing at all.
 *
 * Twenty commands through one multiplexer is therefore the whole test: with
 * the leak it dies around the sixteenth, and no introspection is needed to
 * see it. Failures are included because the client used to close its channel
 * only when the command had succeeded.
 */
static void test_channels_are_released_so_a_long_session_does_not_run_out(void **state)
{
    struct wl_session_cfg cfg = local_session();

    (void)state;
    wait_for_the_line();
    for (int i = 0; i < 20; i++) {
        struct wl_exec_result r;
        struct capture cap;
        struct wl_exec_io io = {&cap, on_output, NULL, NULL};
        struct wl_transport t;

        memset(&cap, 0, sizeof(cap));
        assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
        assert_int_equal(wl_client_exec(&t, &cfg, (uint32_t)(1000 + i),
                                        i % 3 == 0 ? "exit 3" : "echo ok", NULL, 0, &io,
                                        5000, &r),
                         WL_OK);
        wl_transport_close(&t);
        assert_true(r.ran);
        assert_int_equal(r.exit_code, i % 3 == 0 ? 3U : 0U);
    }
}

/* Long runs, so the line carries a fraction of the bytes the relay has to hand
 * on: a bitmap of a mostly flat image, which is what found this. */
static void write_compressible(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint8_t run[4096];
    size_t done = 0;

    assert_non_null(f);
    for (unsigned band = 0; done < n; band++) {
        size_t want = n - done < sizeof(run) ? n - done : sizeof(run);

        memset(run, (int)(band % 3U == 0U ? 0xFF : (band % 3U == 1U ? 0x00 : 0xC0)),
               sizeof(run));
        assert_int_equal(fwrite(run, 1, want, f), want);
        done += want;
    }
    assert_int_equal(fclose(f), 0);
}

static void assert_identical(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int ca;
    int cb;

    assert_non_null(fa);
    assert_non_null(fb);
    do {
        ca = fgetc(fa);
        cb = fgetc(fb);
        assert_int_equal(ca, cb);
    } while (ca != EOF);
    (void)fclose(fa);
    (void)fclose(fb);
}

/*
 * A transfer that outruns a serial line has to wait for it, not spin. The
 * multiplexer stops reading a client whose records the line cannot take yet,
 * but it kept asking poll about that client's socket, which always had data
 * waiting, so poll returned at once on every pass: a put to the A21m held the
 * relay at 99% of a core for the whole transfer. The same held for the line's
 * descriptor while a slow client held a get back. Its CPU time across a paced
 * transfer each way has to be a small part of the time the transfer took.
 */
static void test_a_transfer_over_a_slow_line_does_not_spin(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_file_result r;
    struct wl_transport t;
    struct timespec c0;
    struct timespec c1;
    clockid_t cpu;
    char local[256];
    char remote[256];
    char back[256];
    uint32_t start;
    uint32_t wall;
    uint32_t used;
    FILE *f;

    (void)state;
    (void)snprintf(local, sizeof(local), "%s/noise.bin", h.dir);
    (void)snprintf(remote, sizeof(remote), "%s/noise-sent.bin", h.dir);
    (void)snprintf(back, sizeof(back), "%s/noise-back.bin", h.dir);
    f = fopen(local, "wb");
    assert_non_null(f);
    /* Noise, so compression cannot shrink it below the line's pace, and far
     * more than one credit window, so the client is ahead of the line and held
     * back for most of the transfer, which is the state that spun. */
    for (uint32_t i = 0, x = 12345U; i < 262144U; i++) {
        x = x * 1103515245U + 12345U;
        assert_int_equal(fputc((int)(x >> 16) & 0xFF, f), (int)((x >> 16) & 0xFF));
    }
    assert_int_equal(fclose(f), 0);
    wait_for_the_line();
    assert_int_equal(pthread_getcpuclockid(h.mux_th, &cpu), 0);

    assert_int_equal(clock_gettime(cpu, &c0), 0);
    start = wl_clock_ms();
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_put(&t, &cfg, 0x9101U, local, remote, 1, 5000, &r), WL_OK);
    wl_transport_close(&t);
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_get(&t, &cfg, 0x9102U, remote, back, 5000, &r), WL_OK);
    wl_transport_close(&t);
    wall = wl_clock_since(start);
    assert_int_equal(clock_gettime(cpu, &c1), 0);
    assert_identical(local, remote);
    assert_identical(local, back);

    used = (uint32_t)((c1.tv_sec - c0.tv_sec) * 1000L + (c1.tv_nsec - c0.tv_nsec) / 1000000L);
    /* It took seconds at this pace, so a spinning relay would use nearly all
     * of that; one that waits uses a few percent. */
    assert_true(wall >= 2000U);
    if (used * 4U >= wall)
        fail_msg("the multiplexer used %u ms of CPU in %u ms of transfer", used, wall);
}

/*
 * A file big enough and compressible enough that the relay cannot hand records
 * on as fast as the line delivers them, in both directions.
 *
 * This is the shape that was reported as "a checksum error on a large bitmap".
 * The cause was neither the checksum nor the compression: a record the far side
 * refused was held in a stash of one, and since records are dispatched from
 * inside a single pump, the next refused record overwrote it. One 4 KiB chunk
 * went missing from the middle of the file and nothing said so.
 */
static void test_a_large_compressible_transfer_loses_nothing(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_file_result r;
    struct wl_transport t;
    char local[256];
    char remote[256];
    char back[256];

    (void)state;
    (void)snprintf(local, sizeof(local), "%s/flat.bmp", h.dir);
    (void)snprintf(remote, sizeof(remote), "%s/flat-sent.bmp", h.dir);
    (void)snprintf(back, sizeof(back), "%s/flat-back.bmp", h.dir);
    write_compressible(local, 2U << 20);
    wait_for_the_line();

    /* Up: the client feeds the relay faster than the line drains. */
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_put(&t, &cfg, 0x9001U, local, remote, 1, 5000, &r), WL_OK);
    wl_transport_close(&t);
    assert_identical(local, remote);

    /* Down: the line feeds the relay faster than the client drains, which is
     * the direction the report came from. */
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_get(&t, &cfg, 0x9002U, remote, back, 5000, &r), WL_OK);
    wl_transport_close(&t);
    assert_identical(local, back);
}

/*
 * One cable has more than one name: /dev/ttyUSB0, and the /dev/serial/by-id
 * link that points at it. Taking the socket name from the path as typed gave
 * two sockets for one port, so pointing a second client at the other name
 * started a second multiplexer, which then could not open the port the first
 * one was holding.
 */
/*
 * A command sent the moment @stop returns, as an update does: put the new
 * binary, @stop, then carry on. The agent is gone before the link can say
 * so, and the multiplexer used to relay the command to it; when the new agent
 * appeared the channel was closed under a client still waiting for its OPEN
 * to be answered, and the client waited for ever (2026-09-26, on the A21m).
 * Now the multiplexer holds it for the new agent, which runs it.
 */
static void test_a_command_straight_after_stop_runs_on_the_next_agent(void **state)
{
    struct wl_session_cfg cfg = local_session();
    struct wl_transport t;
    struct wl_exec_result r;
    struct capture cap;
    struct wl_exec_io io = {&cap, on_output, NULL, NULL};
    struct job after;

    (void)state;
    memset(&cap, 0, sizeof(cap));
    h.agent_restarts = 1;
    h.agent_gap_ms = 300;
    wait_for_the_line();
    memset(&r, 0, sizeof(r));
    assert_int_equal(wl_unix_connect(h.sock, &t), WL_OK);
    assert_int_equal(wl_client_exec(&t, &cfg, 4040, "@stop", NULL, 0, &io, 5000, &r), WL_OK);
    wl_transport_close(&t);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);

    memset(&after, 0, sizeof(after));
    after.cmd = "echo after";
    assert_int_equal(pthread_create(&after.th, NULL, run_job, &after), 0);
    join_within(&after, 10000);
    assert_int_equal(after.st, WL_OK);
    assert_true(after.r.ran);
    assert_int_equal(after.r.exit_code, 0);
    assert_int_equal(after.cap.len, 6);
    assert_memory_equal(after.cap.out, "after\n", 6);
    assert_int_equal(h.agent_lives, 1); /* the second agent is the one serving */
}

/*
 * The case the multiplexer cannot see coming: the agent goes without @stop
 * (a crash, a restart by hand) and a command's OPEN goes to it. When the next
 * agent's session starts, that command must fail and say why, not hang; and
 * the line must serve the next command as usual.
 */
static void test_an_open_lost_to_a_restart_fails_rather_than_hangs(void **state)
{
    struct job lost;
    struct job next;
    uint32_t start;

    (void)state;
    h.agent_restarts = 1;
    h.agent_gap_ms = 500;
    wait_for_the_line();
    h.agent_leave = 1;
    start = wl_clock_ms();
    while (h.agent_lives == 0 && wl_clock_since(start) < 5000U)
        (void)poll(NULL, 0, 5);
    assert_int_equal(h.agent_lives, 1);

    memset(&lost, 0, sizeof(lost));
    lost.cmd = "echo lost";
    assert_int_equal(pthread_create(&lost.th, NULL, run_job, &lost), 0);
    join_within(&lost, 10000);
    assert_int_equal(lost.st, WL_OK);
    assert_false(lost.r.ran);
    assert_non_null(strstr(lost.r.message, "restarted"));

    memset(&next, 0, sizeof(next));
    next.cmd = "echo next";
    assert_int_equal(pthread_create(&next.th, NULL, run_job, &next), 0);
    join_within(&next, 10000);
    assert_int_equal(next.st, WL_OK);
    assert_true(next.r.ran);
    assert_int_equal(next.cap.len, 5);
    assert_memory_equal(next.cap.out, "next\n", 5);
}

static void test_two_names_for_one_line_share_a_socket(void **state)
{
    char target[128];
    char alias[128];
    char from_target[256];
    char from_alias[256];
    int fd;

    (void)state;
    (void)snprintf(target, sizeof(target), "/tmp/devlink-line-%ld", (long)getpid());
    (void)snprintf(alias, sizeof(alias), "/tmp/devlink-alias-%ld", (long)getpid());
    (void)remove(target);
    (void)remove(alias);
    fd = open(target, O_CREAT | O_WRONLY, 0600);
    assert_true(fd >= 0);
    (void)close(fd);
    assert_int_equal(symlink(target, alias), 0);

    assert_int_equal(wl_mux_socket_path(target, from_target, sizeof(from_target)), 1);
    assert_int_equal(wl_mux_socket_path(alias, from_alias, sizeof(from_alias)), 1);
    assert_string_equal(from_target, from_alias);

    (void)remove(target);
    (void)remove(alias);
}

/* A line that is not plugged in yet cannot be resolved, and still has to name
 * a socket: the multiplexer may be started before the cable is in. */
static void test_a_line_that_is_not_there_still_names_a_socket(void **state)
{
    char out[256];

    (void)state;
    assert_int_equal(wl_mux_socket_path("/dev/ttyUSB9", out, sizeof(out)), 1);
    assert_non_null(strstr(out, "devlink-ttyUSB9.sock"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_two_names_for_one_line_share_a_socket),
        cmocka_unit_test(test_a_line_that_is_not_there_still_names_a_socket),
        cmocka_unit_test_setup_teardown(test_a_client_reaches_the_agent_through_the_multiplexer,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_two_clients_run_at_once_without_disturbing_each_other,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_more_commands_than_slots_all_run_in_turn, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_one_client_leaving_does_not_disturb_the_other,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_a_client_arriving_before_the_line_is_up_is_served,
                                        setup_late_agent, teardown),
        cmocka_unit_test_setup_teardown(
            test_a_connection_that_closes_at_once_does_not_kill_the_multiplexer, setup,
            teardown),
        cmocka_unit_test_setup_teardown(test_a_command_straight_after_stop_runs_on_the_next_agent,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_an_open_lost_to_a_restart_fails_rather_than_hangs,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_a_transfer_over_a_slow_line_does_not_spin,
                                        setup_paced, teardown),
        cmocka_unit_test_setup_teardown(test_a_large_compressible_transfer_loses_nothing, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(
            test_channels_are_released_so_a_long_session_does_not_run_out, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
