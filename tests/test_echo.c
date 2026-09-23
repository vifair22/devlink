#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/echo.h"

struct server {
    struct wl_transport t;
    uint32_t idle_ms;
    struct wl_echo_stats st;
    wl_status result;
};

static void *serve_thread(void *arg)
{
    struct server *s = arg;

    s->result = wl_echo_serve(&s->t, s->idle_ms, &s->st);
    return NULL;
}

/* The full test the real cable gets: probe on one end, serve on the other. */
static void test_probe_against_server(void **state)
{
    struct server srv;
    struct wl_transport probe;
    struct wl_echo_stats st;
    pthread_t th;

    (void)state;
    memset(&srv, 0, sizeof(srv));
    pty_pair(&srv.t, &probe);
    srv.idle_ms = 5000;
    assert_int_equal(pthread_create(&th, NULL, serve_thread, &srv), 0);

    assert_int_equal(wl_echo_probe(&probe, 25, WL_SERIAL_MAX_PAYLOAD, 2000, &st), WL_OK);
    assert_int_equal(pthread_join(th, NULL), 0);

    assert_int_equal(st.sent, 25);
    assert_int_equal(st.echoed, 25);
    assert_int_equal(st.mismatched + st.timeouts, 0);
    assert_int_equal(srv.result, WL_OK);
    assert_int_equal(srv.st.echoed, 25);
    /* The server stopped on the probe's closing ACK, not the idle timer. */
    assert_true(srv.st.elapsed_ms < srv.idle_ms);
    wl_transport_close(&srv.t);
    wl_transport_close(&probe);
}

static void test_probe_with_nobody_listening(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    struct wl_echo_stats st;

    (void)state;
    pty_pair(&a, &b);
    assert_int_equal(wl_echo_probe(&a, 3, 16, 30, &st), WL_OK);
    assert_int_equal(st.sent, 3);
    assert_int_equal(st.echoed, 0);
    assert_int_equal(st.timeouts, 3);
    wl_transport_close(&a);
    wl_transport_close(&b);
}

/* A peer that echoes the right sequence number with the wrong bytes, and a
 * valid CRC, must be counted as a mismatch rather than a good echo. */
struct liar {
    struct wl_transport t;
    struct wl_frame_rx rx;
    int frames;
};

static void liar_cb(void *ctx, const struct wl_frame *f)
{
    struct liar *l = ctx;
    uint8_t payload[WL_SERIAL_MAX_PAYLOAD];
    uint8_t wire[WL_FRAME_MAX_WIRE];
    struct wl_frame out = *f;
    size_t n;

    if (f->type != WL_FRAME_DATA)
        return;
    memcpy(payload, f->payload, f->payload_len);
    payload[0] ^= 0xFF;
    out.payload = payload;
    assert_int_equal(wl_frame_encode(&out, wire, sizeof(wire), &n), WL_OK);
    assert_int_equal(wl_transport_write(&l->t, wire, n), WL_OK);
    l->frames++;
}

static void *liar_thread(void *arg)
{
    struct liar *l = arg;
    uint8_t buf[256];
    size_t got;

    wl_frame_rx_init(&l->rx, WL_SERIAL_MAX_PAYLOAD);
    while (l->frames < 2) {
        if (wl_transport_read(&l->t, buf, sizeof(buf), &got, 2000) != WL_OK || got == 0)
            break;
        wl_frame_rx_feed(&l->rx, buf, got, liar_cb, l);
    }
    return NULL;
}

static void test_probe_detects_wrong_payload(void **state)
{
    static struct liar liar;
    struct wl_transport probe;
    struct wl_echo_stats st;
    pthread_t th;

    (void)state;
    memset(&liar, 0, sizeof(liar));
    pty_pair(&liar.t, &probe);
    assert_int_equal(pthread_create(&th, NULL, liar_thread, &liar), 0);
    assert_int_equal(wl_echo_probe(&probe, 2, 64, 2000, &st), WL_OK);
    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(st.mismatched, 2);
    assert_int_equal(st.echoed, 0);
    wl_transport_close(&liar.t);
    wl_transport_close(&probe);
}

static void test_serve_idle_timeout(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    struct wl_echo_stats st;

    (void)state;
    pty_pair(&a, &b);
    assert_int_equal(wl_echo_serve(&a, 100, &st), WL_OK);
    assert_int_equal(st.echoed, 0);
    assert_true(st.elapsed_ms >= 100);
    wl_transport_close(&a);
    wl_transport_close(&b);
}

static void test_probe_rejects_oversize_payload(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    struct wl_echo_stats st;

    (void)state;
    pty_pair(&a, &b);
    assert_int_equal(wl_echo_probe(&a, 1, WL_SERIAL_MAX_PAYLOAD + 1, 10, &st),
                     WL_ERR_INVAL);
    wl_transport_close(&a);
    wl_transport_close(&b);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_probe_against_server),
        cmocka_unit_test(test_probe_with_nobody_listening),
        cmocka_unit_test(test_probe_detects_wrong_payload),
        cmocka_unit_test(test_serve_idle_timeout),
        cmocka_unit_test(test_probe_rejects_oversize_payload),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
