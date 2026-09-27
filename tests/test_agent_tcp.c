#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "devlink/agent_tcp.h"
#include "devlink/client_exec.h"
#include "devlink/keyfile.h"
#include "devlink/proto.h"
#include "devlink/tcp.h"
#include "devlink/tcplink.h"

/*
 * The agent's TCP side as its programs start it: the key file, the listener
 * it opens only when there is a key, and then everything real at once, the
 * socket listener, the agent loop and a client connecting over 127.0.0.1.
 */

static char dir[] = "/tmp/devlink-keyXXXXXX";

static void write_file(const char *name, const char *text, char *path, size_t cap)
{
    FILE *f;

    (void)snprintf(path, cap, "%s/%s", dir, name);
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fputs(text, f) >= 0, 1);
    assert_int_equal(fclose(f), 0);
}

static int setup(void **state)
{
    (void)state;
    assert_non_null(mkdtemp(dir));
    return 0;
}

static int teardown(void **state)
{
    char cmd[64];

    (void)state;
    (void)snprintf(cmd, sizeof(cmd), "rm -r %s", dir);
    assert_int_equal(system(cmd), 0);
    return 0;
}

static void test_no_key_file_is_its_own_answer(void **state)
{
    uint8_t psk[WL_NOISE_KEYLEN];
    char path[256];

    (void)state;
    (void)snprintf(path, sizeof(path), "%s/none", dir);
    assert_int_equal(wl_key_load(path, psk), WL_ERR_INVAL);
}

static void test_an_empty_key_is_a_mistake_not_a_key(void **state)
{
    uint8_t psk[WL_NOISE_KEYLEN];
    char path[256];

    (void)state;
    write_file("empty", "\r\nsecond line\n", path, sizeof(path));
    assert_int_equal(wl_key_load(path, psk), WL_ERR_CORRUPT);
}

static void test_line_endings_do_not_change_the_key(void **state)
{
    uint8_t a[WL_NOISE_KEYLEN];
    uint8_t b[WL_NOISE_KEYLEN];
    uint8_t c[WL_NOISE_KEYLEN];
    uint8_t want[WL_NOISE_KEYLEN];
    char path[256];

    (void)state;
    /* Written on Windows, on Linux, or with no newline at all: the same key,
     * and the one the protocol derives from the passphrase itself. */
    write_file("crlf", "open sesame\r\nignored\r\n", path, sizeof(path));
    assert_int_equal(wl_key_load(path, a), WL_OK);
    write_file("lf", "open sesame\n", path, sizeof(path));
    assert_int_equal(wl_key_load(path, b), WL_OK);
    write_file("bare", "open sesame", path, sizeof(path));
    assert_int_equal(wl_key_load(path, c), WL_OK);
    wl_noise_psk_from_passphrase("open sesame", 11, want);
    assert_memory_equal(a, want, sizeof(want));
    assert_memory_equal(b, want, sizeof(want));
    assert_memory_equal(c, want, sizeof(want));
}

static void test_no_key_no_listener(void **state)
{
    struct wl_agent_tcp tcp;
    char path[256];

    (void)state;
    (void)snprintf(path, sizeof(path), "%s/none", dir);
    assert_int_equal(wl_agent_tcp_open(path, "127.0.0.1", 0, &tcp), WL_OK);
    assert_null(tcp.l);
    wl_agent_tcp_close(&tcp);
}

/* ---------------------------------------------------------------------------
 * all of it, over real sockets
 * ------------------------------------------------------------------------- */

struct agent_thread {
    struct wl_agent_tcp tcp;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
};

static int should_stop(void *ctx)
{
    return ((struct agent_thread *)ctx)->stop;
}

static void *agent_main(void *arg)
{
    struct agent_thread *a = arg;

    a->result = wl_agent_run(NULL, &a->tcp.listener, &a->cfg, 0x7C9U);
    return NULL;
}

struct out {
    char text[256];
    size_t len;
};

static void on_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    struct out *o = ctx;

    (void)stream;
    if (o->len + n < sizeof(o->text)) {
        memcpy(o->text + o->len, p, n);
        o->len += n;
        o->text[o->len] = '\0';
    }
}

static wl_status exec_over_tcp(uint16_t port, const char *key_path, const char *cmd,
                               struct out *o, struct wl_exec_result *r)
{
    struct wl_tcplink_cfg tcfg;
    struct wl_session_cfg cfg;
    struct wl_exec_io io;
    struct wl_transport t;
    uint8_t psk[WL_NOISE_KEYLEN];
    wl_status st;

    assert_int_equal(wl_key_load(key_path, psk), WL_OK);
    wl_session_cfg_init(&cfg);
    cfg.level = WL_SESSION_TCP_LEVEL;
    wl_tcplink_cfg_init(&tcfg);
    tcfg.initiator = 1;
    tcfg.psk = psk;
    cfg.make_link = wl_tcplink_make;
    cfg.make_link_arg = &tcfg;
    memset(&io, 0, sizeof(io));
    io.output = on_output;
    io.ctx = o;
    st = wl_tcp_connect("127.0.0.1", port, 3000, &t);
    if (st != WL_OK)
        return st;
    st = wl_client_exec(&t, &cfg, 1234, cmd, NULL, 0, &io, 5000, r);
    wl_transport_close(&t);
    return st;
}

static void test_a_client_reaches_the_agent_over_a_real_socket(void **state)
{
    struct agent_thread a;
    struct wl_exec_result r;
    struct out o;
    pthread_t th;
    char key[256];
    char wrong[256];

    (void)state;
    memset(&a, 0, sizeof(a));
    memset(&o, 0, sizeof(o));
    write_file("DEVLINK.KEY", "a real passphrase\r\n", key, sizeof(key));
    write_file("wrong.key", "not it\n", wrong, sizeof(wrong));
    assert_int_equal(wl_agent_tcp_open(key, "127.0.0.1", 0, &a.tcp), WL_OK);
    assert_non_null(a.tcp.l);
    assert_true(a.tcp.port != 0);
    wl_session_cfg_init(&a.cfg.session);
    a.cfg.platform = WL_PLATFORM_LINUX;
    a.cfg.stop = should_stop;
    a.cfg.stop_ctx = &a;
    assert_int_equal(pthread_create(&th, NULL, agent_main, &a), 0);

    assert_int_equal(exec_over_tcp(a.tcp.port, key, "echo over a real socket", &o, &r), WL_OK);
    assert_true(r.ran);
    assert_int_equal(r.exit_code, 0);
    assert_string_equal(o.text, "over a real socket\n");

    /* The wrong passphrase is closed on, and the client is told what that
     * most likely means. */
    memset(&o, 0, sizeof(o));
    assert_int_not_equal(exec_over_tcp(a.tcp.port, wrong, "echo no", &o, &r), WL_OK);
    assert_non_null(strstr(wl_error(), "before the handshake finished"));
    assert_int_equal(o.len, 0);

    a.stop = 1;
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(a.result, WL_OK);
    wl_agent_tcp_close(&a.tcp);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_no_key_file_is_its_own_answer),
        cmocka_unit_test(test_an_empty_key_is_a_mistake_not_a_key),
        cmocka_unit_test(test_line_endings_do_not_change_the_key),
        cmocka_unit_test(test_no_key_no_listener),
        cmocka_unit_test(test_a_client_reaches_the_agent_over_a_real_socket),
    };

    return cmocka_run_group_tests(tests, setup, teardown);
}
