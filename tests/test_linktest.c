#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "w98link/linktest.h"

/* The real-cable test, end to end over a pty: both programs' loops, the
 * pump, the link and the transport together, as the A21m will run them. */

struct server {
    struct wl_transport t;
    struct wl_session_cfg cfg;
    struct wl_linktest_result r;
    wl_status result;
};

static void *serve_thread(void *arg)
{
    struct server *s = arg;

    s->result = wl_linktest_serve(&s->t, &s->cfg, 0x5E4E4U, 10000, 1000, &s->r);
    return NULL;
}

static void test_probe_through_echo_server(void **state)
{
    static struct server srv;
    struct wl_transport probe;
    struct wl_linktest_result r;
    pthread_t th;

    (void)state;
    memset(&srv, 0, sizeof(srv));
    /* A pty moves bytes far faster than 115200, so the pacing rate is set
     * well above what the test will reach; the protocol is the same. */
    wl_session_cfg_init(&srv.cfg);
    pty_pair(&srv.t, &probe);
    assert_int_equal(pthread_create(&th, NULL, serve_thread, &srv), 0);

    assert_int_equal(
        wl_linktest_probe(&probe, &srv.cfg, 0x9B0BEU, NULL, 300000, 20000, &r), WL_OK);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(r.bytes, 300000);
    assert_int_equal(r.corrupt, 0);
    assert_int_equal(r.sessions, 1);
    assert_int_equal(srv.result, WL_OK);
    assert_int_equal(srv.r.bytes, 300000);
    wl_transport_close(&srv.t);
    wl_transport_close(&probe);
}

static void test_serve_gives_up_without_peer(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    struct wl_session_cfg cfg;
    struct wl_linktest_result r;

    (void)state;
    wl_session_cfg_init(&cfg);
    pty_pair(&a, &b);
    assert_int_equal(wl_linktest_serve(&a, &cfg, 1, 300, 1000, &r), WL_OK);
    assert_int_equal(r.bytes, 0);
    assert_int_equal(r.sessions, 0);
    wl_transport_close(&a);
    wl_transport_close(&b);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_probe_through_echo_server),
        cmocka_unit_test(test_serve_gives_up_without_peer),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
