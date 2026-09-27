#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/clock.h"
#include "devlink/wire.h"
#include "devlink/agent.h"
#include "devlink/client_exec.h"
#include "devlink/proto.h"
#ifdef EXEC_OVER_TCP
#include <sys/socket.h>

#include "devlink/posix_fd.h"
#include "devlink/tcplink.h"
#endif

/*
 * EXEC end to end: the real agent loop (with the POSIX process backend) in a
 * thread, the real client, and a pty between them. Everything the A21m runs
 * except the Win32 process code.
 *
 * test_exec_tcp.c compiles this same file with EXEC_OVER_TCP: the agent then
 * serves TCP only, with a key, and every new session gets a new connection,
 * handed to it as a socketpair. new_line() is where the two differ.
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
/* The line the current test's own peer runs on. */
static struct wl_transport *line;

#ifdef EXEC_OVER_TCP
static const uint8_t key[32] = {0x7e, 0x57};
static int pending_fd = -1; /* the agent's end of the next connection */
static int bell[2];
static struct wl_agent_listener listener;
static struct wl_tcplink_cfg client_link;

static wl_status one_accept(void *ctx, struct wl_transport *out, char *peer, size_t cap)
{
    char b;

    (void)ctx;
    if (pending_fd < 0)
        return WL_OK;
    (void)read(bell[0], &b, 1);
    (void)snprintf(peer, cap, "127.0.0.1:%d", pending_fd);
    assert_int_equal(wl_fd_transport(pending_fd, out), WL_OK);
    pending_fd = -1;
    return WL_OK;
}

/* A new connection for a new session, the previous one closed: over TCP a
 * session is its connection. */
static struct wl_transport *new_line(void)
{
    int sv[2];

    wl_transport_close(&client_t);
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
    pending_fd = sv[1];
    assert_int_equal(write(bell[1], "x", 1), 1);
    assert_int_equal(wl_fd_transport(sv[0], &client_t), WL_OK);
    return &client_t;
}
#else
/* Serial: every session shares the one line, and the link resynchronises. */
static struct wl_transport *new_line(void)
{
    return &client_t;
}
#endif

static int should_stop(void *ctx)
{
    return ((struct agent_thread *)ctx)->stop;
}

/* Stands in for the target's drives: D is the CD drive with no disc in it.
 * Linux has no drive letters, so this is the only way to reach the refusal
 * that keeps an empty drive away from command.com. */
static int stub_drive_ready(char letter)
{
    return letter != 'D';
}

static void *agent_main(void *arg)
{
    struct agent_thread *a = arg;

#ifdef EXEC_OVER_TCP
    a->result = wl_agent_run(NULL, &listener, &a->cfg, 0xA6E47U);
#else
    a->result = wl_agent_serve(&a->t, &a->cfg, 0xA6E47U);
#endif
    return NULL;
}

static struct wl_session_cfg fast_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0; /* a pty is far faster than any serial line */
#ifdef EXEC_OVER_TCP
    /* The client's side; the agent builds its own, as the responder. */
    wl_tcplink_cfg_init(&client_link);
    client_link.initiator = 1;
    client_link.psk = key;
    cfg.make_link = wl_tcplink_make;
    cfg.make_link_arg = &client_link;
#endif
    return cfg;
}

static int setup(void **state)
{
    (void)state;
    memset(&agent, 0, sizeof(agent));
#ifdef EXEC_OVER_TCP
    memset(&client_t, 0, sizeof(client_t));
    assert_int_equal(pipe(bell), 0);
    listener.accept = one_accept;
    listener.fd = bell[0];
    listener.psk = key;
#else
    pty_pair(&agent.t, &client_t);
#endif
    agent.cfg.session = fast_session();
    agent.cfg.platform = WL_PLATFORM_LINUX;
    agent.cfg.stop = should_stop;
    agent.cfg.stop_ctx = &agent;
    agent.cfg.drive_ready = stub_drive_ready;
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
#ifdef EXEC_OVER_TCP
    (void)close(bell[0]);
    (void)close(bell[1]);
    if (pending_fd >= 0)
        (void)close(pending_fd);
    pending_fd = -1;
#endif
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

/*
 * Stands in for a pipe or a terminal on the client's side. 'quiet' is how
 * many times it answers "nothing yet" before handing anything over, which is
 * what a person typing looks like: the loop has to keep the command's output
 * moving while nobody is pressing a key. It is a global for the same reason
 * the capture is: one struct wl_exec_io carries one ctx, and the output
 * callback already owns it.
 */
struct feed {
    const char *data;
    size_t len;
    size_t at;
    size_t most; /* the most to hand over at once, 0 for as much as fits */
    int quiet;
    int calls;
};

static struct feed feed;

static size_t on_input(void *ctx, uint8_t *p, size_t cap_, int *eof)
{
    size_t n;

    (void)ctx;
    feed.calls++;
    if (feed.quiet > 0) {
        feed.quiet--;
        return 0;
    }
    n = feed.len - feed.at;
    if (n > cap_)
        n = cap_;
    if (feed.most > 0 && n > feed.most)
        n = feed.most;
    memcpy(p, feed.data + feed.at, n);
    feed.at += n;
    if (feed.at == feed.len)
        *eof = 1;
    return n;
}

static wl_status run_with_input(const char *cmd, struct wl_exec_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_exec_io io = {&cap, on_output, NULL, on_input};
    static uint32_t seed = 5000;

    memset(&cap, 0, sizeof(cap));
    return wl_client_exec(new_line(), &cfg, ++seed, cmd, NULL, 0, &io, 5000, r);
}

static wl_status run(const char *cmd, const char *cwd, uint8_t flags, struct wl_exec_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_exec_io io = {&cap, on_output, NULL, NULL};
    static uint32_t seed = 1000;

    memset(&cap, 0, sizeof(cap));
    return wl_client_exec(new_line(), &cfg, ++seed, cmd, cwd, flags, &io, 5000, r);
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

/* What the caller offers reaches the command, and the command's answer comes
 * back: the whole point of the input hook. */
static void test_stdin_reaches_the_command(void **state)
{
    struct wl_exec_result r;

    (void)state;
    memset(&feed, 0, sizeof(feed));
    feed.data = "hello\nworld\n";
    feed.len = 12;
    assert_int_equal(run_with_input("cat", &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, 12);
    assert_memory_equal(cap.out, "hello\nworld\n", 12);
}

/*
 * More input than the channel's credit window, handed over in small pieces.
 * This is the path where a send is refused for want of credit and the same
 * bytes have to go again next time round: losing them would truncate the
 * command's input silently, which is the worst shape this bug could take.
 */
static void test_stdin_larger_than_the_window_arrives_whole(void **state)
{
    struct wl_exec_result r;
    char *want = malloc(200000);
    size_t at = 0;

    (void)state;
    assert_non_null(want);
    while (at < 200000 - 16)
        at += (size_t)sprintf(want + at, "%zu\n", at);
    memset(&feed, 0, sizeof(feed));
    feed.data = want;
    feed.len = at;
    feed.most = 700; /* small enough to need many turns of the loop */
    assert_int_equal(run_with_input("cat", &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, at);
    assert_memory_equal(cap.out, want, at);
    free(want);
}

/*
 * A command that reads its input, but not yet: far more than the agent's
 * stdin queue arrives while it sleeps. Every byte must still reach it.
 *
 * This is the case that fails if the agent tells the client it may send more
 * as soon as a record is delivered rather than as the process reads: the
 * queue fills, the rest has nowhere to go, and the command quietly gets a
 * truncated input with nothing anywhere reporting a problem.
 */
static void test_input_for_a_slow_reader_is_not_dropped(void **state)
{
    struct wl_exec_result r;
    size_t len = 400000;
    char *want = malloc(len);

    (void)state;
    assert_non_null(want);
    for (size_t i = 0; i < len; i++)
        want[i] = (char)('a' + (i % 26));
    memset(&feed, 0, sizeof(feed));
    feed.data = want;
    feed.len = len;
    assert_int_equal(run_with_input("sleep 1; cat", &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, len);
    assert_memory_equal(cap.out, want, len);
    free(want);
}

/*
 * Input for a command that never reads it. The agent's queue fills, the pipe
 * behind it fills, and the channel runs out of credit, so sends start being
 * refused. The command must still finish and the client must still return
 * its output rather than wedging with bytes it cannot place: "echo y | put
 * this in front of an installer that ignores stdin" is an ordinary mistake.
 */
static void test_input_nobody_reads_does_not_wedge(void **state)
{
    struct wl_exec_result r;
    char *want = malloc(400000);

    (void)state;
    assert_non_null(want);
    memset(want, 'y', 400000);
    memset(&feed, 0, sizeof(feed));
    feed.data = want;
    feed.len = 400000;
    assert_int_equal(run_with_input("sleep 1; echo ignored", &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, 8);
    assert_memory_equal(cap.out, "ignored\n", 8);
    free(want);
}

/* A source with nothing to say yet must not look like a closed one, and must
 * not stop the command's output while it thinks. */
static void test_a_quiet_source_is_not_end_of_input(void **state)
{
    struct wl_exec_result r;

    (void)state;
    memset(&feed, 0, sizeof(feed));
    feed.data = "late\n";
    feed.len = 5;
    feed.quiet = 20;
    assert_int_equal(run_with_input("echo first; cat", &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, 11);
    assert_memory_equal(cap.out, "first\nlate\n", 11);
    assert_true(feed.calls > 20);
}

/* A source that is empty from the start closes stdin rather than leaving a
 * reader waiting for input that is never coming. */
static void test_an_empty_source_closes_stdin(void **state)
{
    struct wl_exec_result r;

    (void)state;
    memset(&feed, 0, sizeof(feed));
    feed.data = "";
    feed.len = 0;
    assert_int_equal(run_with_input("cat; echo done", &r), WL_OK);
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
    line = new_line();
    p = wl_peer_new(&cfg, 31337, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    assert_non_null(p);
    start = wl_clock_ms();
    while (!wl_peer_ready(p) && wl_clock_since(start) < 3000U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    assert_true(wl_peer_ready(p));

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, "sleep 30", 8);
    wl_w_str(&w, "", 0);
    wl_w_u8(&w, 0);
    assert_int_equal(wl_peer_open(p, WL_KIND_EXEC, args, w.len, &ch), WL_OK);
    start = wl_clock_ms();
    while (wl_clock_since(start) < 300U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    wl_peer_free(p); /* the client is gone, the command is still running */

    /* A new client must be served promptly, not after the old command has
     * been confirmed dead. */
    start = wl_clock_ms();
    assert_int_equal(wl_client_info(new_line(), &cfg, 4242, 3000, &hello), WL_OK);
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
    struct wl_exec_io io = {&cap, on_output, &test_cancel, NULL};
    struct wl_exec_result r;
    struct wl_hello hello;
    char marker[64];
    char cmd[128];
    pthread_t th;
    uint32_t start;

    (void)state;
    memset(&cap, 0, sizeof(cap));
    test_cancel = 0;
    sprintf(marker, "/tmp/devlink-cancel-%ld", (long)getpid());
    (void)remove(marker);
    sprintf(cmd, "sleep 2; touch %s", marker);

    assert_int_equal(pthread_create(&th, NULL, cancel_after_300ms, NULL), 0);
    start = wl_clock_ms();
    assert_int_equal(wl_client_exec(new_line(), &cfg, 7777, cmd, NULL, 0, &io, 5000, &r),
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
    assert_int_equal(wl_client_info(new_line(), &cfg, 8888, 3000, &hello), WL_OK);
    assert_true(wl_clock_since(start) < 2500U);
    usleep(2500000);
    assert_int_equal(access(marker, F_OK), -1);
    (void)remove(marker);
}

/*
 * The same, for work the command's shell handed to a child of its own: here a
 * pipeline, the shape a cancelled "dd | gzip" has. Killing only the shell
 * leaves the subshell running with the output pipe open, so the marker
 * appears two seconds later. The whole tree has to go.
 */
static void test_cancel_stops_what_the_command_started(void **state)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_exec_io io = {&cap, on_output, &test_cancel, NULL};
    struct wl_exec_result r;
    char marker[64];
    char cmd[160];
    pthread_t th;

    (void)state;
    memset(&cap, 0, sizeof(cap));
    test_cancel = 0;
    sprintf(marker, "/tmp/devlink-cancel-tree-%ld", (long)getpid());
    (void)remove(marker);
    sprintf(cmd, "(sleep 2; touch %s) | cat", marker);

    assert_int_equal(pthread_create(&th, NULL, cancel_after_300ms, NULL), 0);
    assert_int_equal(wl_client_exec(new_line(), &cfg, 7778, cmd, NULL, 0, &io, 5000, &r),
                     WL_OK);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_true(r.cancelled);
    assert_true(r.cancel_acked);
    usleep(2500000);
    assert_int_equal(access(marker, F_OK), -1);
    (void)remove(marker);
}

/*
 * A command naming an empty drive must be refused before it starts. On the
 * target, starting it costs the whole agent: the DOS prompt it raises freezes
 * Windows behind it and only a keypress there ends it.
 */
static void test_command_naming_an_empty_drive_is_refused(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("dir D:\\", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_NOT_FOUND);
    assert_string_equal(r.message, "drive D: is not ready");

    /* In an argument, lower case, and with the agent still serving after. */
    assert_int_equal(run("copy README d:/x", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_NOT_FOUND);
}

/* The guard must not become a reason for ordinary commands to fail: a ready
 * drive, and a colon that is not a drive at all, both have to get through. */
static void test_ready_drives_and_colons_are_not_refused(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("echo C:/ok", NULL, 0, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    assert_int_equal(cap.out_len, 6);
    assert_memory_equal(cap.out, "C:/ok\n", 6);

    /* "prod:" ends in d, which would be drive D if letters were taken out of
     * the middle of a word. */
    assert_int_equal(run("echo prod: done", NULL, 0, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
}

/* @dir is handled inside the agent, so the listing must carry what DOS cannot
 * report: names as stored, exact sizes, a real status. */
static void test_agent_dir_lists_a_directory(void **state)
{
    char dir[] = "/tmp/devlink-dir-XXXXXX";
    struct wl_exec_result r;
    char cmd[128];
    char path[128];
    FILE *f;

    (void)state;
    assert_non_null(mkdtemp(dir));
    sprintf(path, "%s/a long name.txt", dir);
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite("12345", 1, 5, f), 5);
    (void)fclose(f);
    sprintf(path, "%s/sub", dir);
    assert_int_equal(mkdir(path, 0700), 0);

    sprintf(cmd, "@dir %s", dir);
    assert_int_equal(run(cmd, NULL, 0, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    cap.out[cap.out_len] = '\0';
    /* One line each, the name last because it contains spaces. */
    assert_non_null(strstr(cap.out, "f 5 "));
    assert_non_null(strstr(cap.out, " a long name.txt\n"));
    assert_non_null(strstr(cap.out, "d 0 "));
    assert_non_null(strstr(cap.out, " sub\n"));
    assert_int_equal(cap.err_len, 0);

    sprintf(path, "%s/a long name.txt", dir);
    (void)remove(path);
    sprintf(path, "%s/sub", dir);
    (void)remove(path);
    (void)remove(dir);
}

/* Output longer than one mux chunk has to arrive whole: the listing goes
 * through the same flow control as a command's output. */
static void test_agent_dir_handles_a_big_directory(void **state)
{
    char dir[] = "/tmp/devlink-big-XXXXXX";
    struct wl_exec_result r;
    char cmd[128];
    char path[160];
    size_t lines = 0;

    (void)state;
    assert_non_null(mkdtemp(dir));
    for (int i = 0; i < 400; i++) {
        FILE *f;

        sprintf(path, "%s/file-%03d-with-a-longish-name.dat", dir, i);
        f = fopen(path, "wb");
        assert_non_null(f);
        (void)fclose(f);
    }
    sprintf(cmd, "@dir %s", dir);
    assert_int_equal(run(cmd, NULL, 0, &r), WL_OK);
    assert_int_equal(r.exit_code, 0);
    for (size_t i = 0; i < cap.out_len; i++) {
        if (cap.out[i] == '\n')
            lines++;
    }
    assert_int_equal(lines, 400);
    assert_true(cap.out_len > 4096); /* past one OUT_CHUNK, so it was streamed */
    for (int i = 0; i < 400; i++) {
        sprintf(path, "%s/file-%03d-with-a-longish-name.dat", dir, i);
        (void)remove(path);
    }
    (void)remove(dir);
}

static void test_agent_dir_failures(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@dir /nonexistent/place", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_NOT_FOUND);

    assert_int_equal(run("@dir", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_PATH);

    /* The drive guard applies here too: an agent command must not be a way
     * around it. */
    assert_int_equal(run("@dir D:\\", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_NOT_FOUND);
    assert_string_equal(r.message, "drive D: is not ready");
}

/* @caps asks the port about itself. Over a pty that is the POSIX serial
 * backend, which reports the rate the port is actually running at; on the
 * target it is the Windows driver's own list of rates it will accept, which is
 * the thing worth knowing before changing how the line is driven. */
static void test_agent_caps_reports_the_port(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@caps", NULL, 0, &r), WL_OK);
#ifdef EXEC_OVER_TCP
    /* A socketpair has nothing to say about itself: refused, as the
     * protocol says, with code 3. (The real TCP transport reports itself,
     * tested in test_tcp.c.) */
    assert_false(r.ran);
    assert_int_equal(r.fail_code, 3);
#else
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    cap.out[cap.out_len] = '\0';
    assert_non_null(strstr(cap.out, "transport serial"));
    assert_non_null(strstr(cap.out, "current 115200\n"));
#endif
}

/*
 * Many commands abandoned one after another, more than the eight exec slots
 * the agent has: the reaper used to be a fixed array of that size, and once
 * full it fell back to the blocking teardown in the middle of serving. The
 * agent must stay responsive throughout and shut down cleanly after.
 */
static void test_many_abandoned_commands_keep_the_agent_responsive(void **state)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_hello hello;

    (void)state;
    for (int i = 0; i < 12; i++) {
        struct wl_peer_handlers h;
        struct wl_peer *p;
        uint8_t args[64];
        struct wl_wbuf w;
        uint16_t ch;
        uint32_t start = wl_clock_ms();

        memset(&h, 0, sizeof(h));
        line = new_line();
        p = wl_peer_new(&cfg, (uint32_t)(50000 + i), wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
        assert_non_null(p);
        while (!wl_peer_ready(p) && wl_clock_since(start) < 3000U)
            assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
        assert_true(wl_peer_ready(p));

        wl_wbuf_init(&w, args, sizeof(args));
        wl_w_str(&w, "sleep 30", 8);
        wl_w_str(&w, "", 0);
        wl_w_u8(&w, 0);
        assert_int_equal(wl_peer_open(p, WL_KIND_EXEC, args, w.len, &ch), WL_OK);
        start = wl_clock_ms();
        while (wl_clock_since(start) < 100U)
            assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
        wl_peer_free(p); /* abandoned with the command still running */
    }

    /* Still answering, promptly, after twelve of them. */
    {
        uint32_t start = wl_clock_ms();

        assert_int_equal(wl_client_info(new_line(), &cfg, 4243, 3000, &hello), WL_OK);
        assert_true(wl_clock_since(start) < 2500U);
    }
}

static void test_agent_time_reports_the_clock(void **state)
{
    struct wl_exec_result r;
    const char *at;
    long long secs;
    uint64_t here = 0;

    (void)state;
    assert_int_equal(run("@time", NULL, 0, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    cap.out[cap.out_len] = '\0';
    at = strstr(cap.out, "utc ");
    assert_non_null(at);
    assert_non_null(strstr(cap.out, "localoffset "));
    secs = strtoll(at + 4, NULL, 10);
    {
        /* The zone name is what tells a misconfigured machine from a drifting
         * one, so it has to be there and it has to say something. */
        const char *zone = strstr(cap.out, "zone ");

        assert_non_null(zone);
        assert_true(zone[5] != '\n' && zone[5] != '\0');
    }
    assert_int_equal(wl_clock_utc(&here), WL_OK);
    /* The agent is this process, so the two clocks are the same one. */
    assert_true(secs >= (long long)here - 5 && secs <= (long long)here + 5);
}

/*
 * The POSIX backend refuses to set the clock, by design rather than for want
 * of an implementation: this test suite must never be able to move the clock
 * of the machine it runs on, and this is where that is nailed down.
 */
static void test_agent_time_will_not_set_the_clock_here(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@time set 1000000000", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_ACCESS);
    assert_non_null(strstr(r.message, "not supported"));
}

static void test_agent_time_rejects_nonsense(void **state)
{
    struct wl_exec_result r;

    (void)state;
    assert_int_equal(run("@time set soon", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_MALFORMED);

    assert_int_equal(run("@time forward", NULL, 0, &r), WL_OK);
    assert_false(r.ran);
    assert_int_equal(r.fail_code, WL_E_UNKNOWN);
}

/* ---------------------------------------------------------------------------
 * two commands at once on one session
 * ------------------------------------------------------------------------- */

struct multi {
    uint16_t ch[2];
    int opened[2];
    char out[2][4096];
    size_t out_len[2];
    int exited[2];
    uint32_t code[2];
    uint32_t exit_ms[2];
};

static int multi_index(const struct multi *m, uint16_t ch)
{
    if (m->ch[0] == ch)
        return 0;
    if (m->ch[1] == ch)
        return 1;
    return -1;
}

static void multi_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg,
                         size_t n)
{
    struct multi *m = ctx;
    int i = multi_index(m, ch);

    (void)code;
    (void)msg;
    (void)n;
    if (i >= 0)
        m->opened[i] = ok ? 1 : -1;
}

static void multi_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct multi *m = ctx;
    int i = multi_index(m, ch);

    if (i < 0)
        return;
    if (type == WL_EXEC_STDOUT) {
        assert_true(m->out_len[i] + n < sizeof(m->out[i]));
        memcpy(m->out[i] + m->out_len[i], p, n);
        m->out_len[i] += n;
    } else if (type == WL_EXEC_EXIT && n == 4) {
        m->code[i] = wl_get_u32le(p);
        m->exit_ms[i] = wl_clock_ms();
        m->exited[i] = 1;
    }
}

/*
 * The mux, the agent's eight exec slots and the per-channel credit windows are
 * all designed for several commands at once, but nothing had ever run two.
 * This is the foundation a local multiplexing daemon would stand on, so it is
 * worth proving before anything is built on it: a second command must open
 * while the first is still running, finish on its own schedule, and keep its
 * output to its own channel.
 */
static void test_two_commands_run_at_once_on_one_session(void **state)
{
    struct wl_session_cfg cfg = fast_session();
    struct wl_peer_handlers h;
    struct wl_peer *p;
    struct multi m;
    uint8_t args[128];
    struct wl_wbuf w;
    uint32_t start;

    (void)state;
    memset(&m, 0, sizeof(m));
    memset(&h, 0, sizeof(h));
    h.ctx = &m;
    h.opened = multi_opened;
    h.record = multi_record;
    line = new_line();
    p = wl_peer_new(&cfg, 61616, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    assert_non_null(p);
    start = wl_clock_ms();
    while (!wl_peer_ready(p) && wl_clock_since(start) < 3000U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    assert_true(wl_peer_ready(p));

    /* The slow one first: three lines about a second apart. */
    {
        const char *slow = "for i in 1 2 3; do echo A$i; sleep 0.3; done";

        /* strlen, not a counted literal: a length one short here truncates the
         * loop and the shell answers with a syntax error instead of running
         * anything, which is a confusing way to fail a concurrency test. */
        wl_wbuf_init(&w, args, sizeof(args));
        wl_w_str(&w, slow, strlen(slow));
        wl_w_str(&w, "", 0);
        wl_w_u8(&w, 0);
    }
    assert_int_equal(wl_peer_open(p, WL_KIND_EXEC, args, w.len, &m.ch[0]), WL_OK);
    start = wl_clock_ms();
    while (m.opened[0] == 0 && wl_clock_since(start) < 3000U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    assert_int_equal(m.opened[0], 1);
    (void)wl_peer_send(p, m.ch[0], WL_EXEC_STDIN_EOF, NULL, 0);

    /* The quick one, opened while the first is still going. */
    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, "echo B", strlen("echo B"));
    wl_w_str(&w, "", 0);
    wl_w_u8(&w, 0);
    assert_int_equal(wl_peer_open(p, WL_KIND_EXEC, args, w.len, &m.ch[1]), WL_OK);
    assert_int_not_equal(m.ch[0], m.ch[1]);
    start = wl_clock_ms();
    while (m.opened[1] == 0 && wl_clock_since(start) < 3000U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    assert_int_equal(m.opened[1], 1);
    (void)wl_peer_send(p, m.ch[1], WL_EXEC_STDIN_EOF, NULL, 0);
    assert_false(m.exited[0]); /* the slow one cannot be done yet */

    start = wl_clock_ms();
    while ((!m.exited[0] || !m.exited[1]) && wl_clock_since(start) < 10000U)
        assert_int_equal(wl_peer_pump(p, line, 20), WL_OK);
    assert_true(m.exited[0]);
    assert_true(m.exited[1]);
    assert_int_equal(m.code[0], 0);
    assert_int_equal(m.code[1], 0);

    /* Each channel kept its own output, whole and in order. */
    assert_int_equal(m.out_len[0], 9);
    assert_memory_equal(m.out[0], "A1\nA2\nA3\n", 9);
    assert_int_equal(m.out_len[1], 2);
    assert_memory_equal(m.out[1], "B\n", 2);

    /* And the quick one really did not wait for the slow one: that is the
     * difference between a mux and a queue. */
    assert_true(m.exit_ms[1] < m.exit_ms[0]);
    wl_peer_free(p);
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
        cmocka_unit_test_setup_teardown(test_stdin_reaches_the_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_stdin_larger_than_the_window_arrives_whole,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_input_for_a_slow_reader_is_not_dropped, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_input_nobody_reads_does_not_wedge, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_a_quiet_source_is_not_end_of_input, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_an_empty_source_closes_stdin, setup, teardown),
        cmocka_unit_test_setup_teardown(test_consecutive_clients, setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_stop_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_unknown_agent_command_is_refused, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_abandoned_command_does_not_block_the_next_client,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_stops_the_command_on_the_agent, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_cancel_stops_what_the_command_started, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_command_naming_an_empty_drive_is_refused, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_ready_drives_and_colons_are_not_refused, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_agent_dir_lists_a_directory, setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_dir_handles_a_big_directory, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_agent_dir_failures, setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_caps_reports_the_port, setup, teardown),
        cmocka_unit_test_setup_teardown(test_many_abandoned_commands_keep_the_agent_responsive,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_time_reports_the_clock, setup, teardown),
        cmocka_unit_test_setup_teardown(test_agent_time_will_not_set_the_clock_here, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_agent_time_rejects_nonsense, setup, teardown),
        cmocka_unit_test_setup_teardown(test_two_commands_run_at_once_on_one_session, setup,
                                        teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
