#include <pthread.h>
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

#include "devlink/clock.h"
#include "devlink/tcp.h"

/* A connected pair over loopback: the listener is gone again by the time the
 * test starts, so each test holds only the two ends. */
static void tcp_pair(const char *addr, struct wl_transport *client, struct wl_transport *server,
                     char *peer, size_t peer_cap)
{
    struct wl_tcp_listener *l = NULL;
    const char *host = addr[0] == '\0' ? "127.0.0.1" : addr;

    assert_int_equal(wl_tcp_listen(addr, 0, &l), WL_OK);
    assert_true(wl_tcp_listener_fd(l) >= 0);
    assert_int_equal(wl_tcp_connect(host, wl_tcp_listener_port(l), 2000, client), WL_OK);
    assert_int_equal(wl_tcp_accept(l, 2000, server, peer, peer_cap), WL_OK);
    assert_non_null(server->ops);
    wl_tcp_listener_close(l);
}

static void read_exactly(struct wl_transport *t, uint8_t *buf, size_t len)
{
    size_t at = 0;

    while (at < len) {
        size_t got = 0;

        assert_int_equal(wl_transport_read(t, buf + at, len - at, &got, 2000), WL_OK);
        assert_true(got > 0);
        at += got;
    }
}

static void test_exchange_both_ways(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    char peer[80];
    uint8_t buf[16];

    (void)state;
    tcp_pair("127.0.0.1", &c, &s, peer, sizeof(peer));
    assert_int_equal(strncmp(peer, "127.0.0.1:", 10), 0);
    assert_int_equal(wl_transport_write(&c, (const uint8_t *)"hello", 5), WL_OK);
    read_exactly(&s, buf, 5);
    assert_memory_equal(buf, "hello", 5);
    assert_int_equal(wl_transport_write(&s, (const uint8_t *)"back\0at\xffyou", 11), WL_OK);
    read_exactly(&c, buf, 11);
    assert_memory_equal(buf, "back\0at\xffyou", 11);
    assert_true(wl_transport_fd(&c) >= 0);
    wl_transport_close(&c);
    wl_transport_close(&s);
}

#define BIG ((3U * 1024U * 1024U) + 17U)

static uint8_t pattern(size_t i)
{
    return (uint8_t)((i * 2654435761U) >> 13);
}

static void *big_writer(void *arg)
{
    struct wl_transport *t = arg;
    uint8_t *buf = malloc(BIG);

    if (buf == NULL)
        return (void *)1;
    for (size_t i = 0; i < BIG; i++)
        buf[i] = pattern(i);
    /* One write of the whole thing: far more than the socket buffers hold,
     * so it only completes because the reader drains as it goes. */
    if (wl_transport_write(t, buf, BIG) != WL_OK) {
        free(buf);
        return (void *)1;
    }
    free(buf);
    return NULL;
}

static void test_large_transfer_arrives_whole(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    pthread_t th;
    void *res = NULL;
    uint8_t *got = malloc(BIG);

    (void)state;
    assert_non_null(got);
    tcp_pair("127.0.0.1", &c, &s, NULL, 0);
    assert_int_equal(pthread_create(&th, NULL, big_writer, &c), 0);
    read_exactly(&s, got, BIG);
    assert_int_equal(pthread_join(th, &res), 0);
    assert_null(res);
    for (size_t i = 0; i < BIG; i++) {
        if (got[i] != pattern(i))
            fail_msg("byte %zu differs", i);
    }
    free(got);
    wl_transport_close(&c);
    wl_transport_close(&s);
}

static void test_read_timeout_is_zero_bytes(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    uint8_t buf[8];
    size_t got = 99;
    uint32_t start;
    uint32_t spent;

    (void)state;
    tcp_pair("127.0.0.1", &c, &s, NULL, 0);
    start = wl_clock_ms();
    assert_int_equal(wl_transport_read(&s, buf, sizeof(buf), &got, 50), WL_OK);
    spent = wl_clock_since(start);
    assert_int_equal(got, 0);
    assert_true(spent >= 40 && spent < 1000);
    wl_transport_close(&c);
    wl_transport_close(&s);
}

static void test_peer_close_is_an_error(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    uint8_t buf[8];
    size_t got = 99;

    (void)state;
    tcp_pair("127.0.0.1", &c, &s, NULL, 0);
    wl_transport_close(&c);
    assert_int_equal(wl_transport_read(&s, buf, sizeof(buf), &got, 1000), WL_ERR_IO);
    assert_int_equal(got, 0);
    assert_non_null(strstr(wl_error(), "peer closed"));
    wl_transport_close(&s);
}

/* A reset (SO_LINGER of zero on close) arrives as ECONNRESET rather than end
 * of stream, and must read the same to the caller. */
static void test_peer_reset_is_an_error(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    struct linger lg = {1, 0};
    uint8_t buf[8];
    size_t got = 99;

    (void)state;
    tcp_pair("127.0.0.1", &c, &s, NULL, 0);
    /* Unread data on the server side is what turns its close into a reset. */
    assert_int_equal(wl_transport_write(&c, (const uint8_t *)"x", 1), WL_OK);
    usleep(20000);
    assert_int_equal(setsockopt(wl_transport_fd(&s), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)),
                     0);
    wl_transport_close(&s);
    assert_int_equal(wl_transport_read(&c, buf, sizeof(buf), &got, 1000), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "peer closed"));
    wl_transport_close(&c);
}

/* Writing to a connection the peer has closed fails, and does not kill the
 * process with SIGPIPE, which is what would end this test if it did. */
static void test_write_to_a_closed_peer_fails(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    uint8_t chunk[4096];
    wl_status st = WL_OK;

    (void)state;
    memset(chunk, 'z', sizeof(chunk));
    tcp_pair("127.0.0.1", &c, &s, NULL, 0);
    wl_transport_close(&s);
    for (int i = 0; i < 1000 && st == WL_OK; i++) {
        st = wl_transport_write(&c, chunk, sizeof(chunk));
        usleep(1000);
    }
    assert_int_equal(st, WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "peer closed"));
    wl_transport_close(&c);
}

static void test_connect_to_a_closed_port_fails_promptly(void **state)
{
    struct wl_tcp_listener *l = NULL;
    struct wl_transport t = {NULL, NULL};
    uint16_t port;
    uint32_t start;

    (void)state;
    assert_int_equal(wl_tcp_listen("127.0.0.1", 0, &l), WL_OK);
    port = wl_tcp_listener_port(l);
    wl_tcp_listener_close(l);
    start = wl_clock_ms();
    assert_int_equal(wl_tcp_connect("127.0.0.1", port, 5000, &t), WL_ERR_IO);
    assert_true(wl_clock_since(start) < 1000);
    assert_non_null(strstr(wl_error(), "refused"));
    assert_null(t.ops);
}

static void test_connect_times_out(void **state)
{
    struct wl_transport t = {NULL, NULL};
    uint32_t start;
    uint32_t spent;
    wl_status st;

    (void)state;
    /* Nothing answers there, so the SYN goes unanswered until the deadline.
     * A sandbox with no route fails it at once instead, which says nothing
     * about the timeout either way. */
    start = wl_clock_ms();
    st = wl_tcp_connect("10.255.255.1", 9, 300, &t);
    spent = wl_clock_since(start);
    assert_int_equal(st, WL_ERR_IO);
    if (strstr(wl_error(), "no answer") == NULL) {
        print_message("connect failed at once (%s); no timeout to measure\n", wl_error());
        skip();
    }
    assert_true(spent >= 250 && spent < 2000);
}

static void test_unresolvable_names_fail(void **state)
{
    struct wl_transport t = {NULL, NULL};
    struct wl_tcp_listener *l = NULL;

    (void)state;
    assert_int_equal(wl_tcp_connect("no-such-host.invalid", 2098, 1000, &t), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "cannot resolve"));
    assert_int_equal(wl_tcp_listen("no-such-host.invalid", 0, &l), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "cannot resolve"));
}

static void test_a_port_in_use_is_refused(void **state)
{
    struct wl_tcp_listener *a = NULL;
    struct wl_tcp_listener *b = NULL;

    (void)state;
    assert_int_equal(wl_tcp_listen("127.0.0.1", 0, &a), WL_OK);
    assert_int_equal(wl_tcp_listen("127.0.0.1", wl_tcp_listener_port(a), &b), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "in use"));
    wl_tcp_listener_close(a);

    assert_int_equal(wl_tcp_listen(NULL, 0, &a), WL_OK);
    assert_int_equal(wl_tcp_listen("", wl_tcp_listener_port(a), &b), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "in use"));
    wl_tcp_listener_close(a);
    wl_tcp_listener_close(NULL);
}

static void test_accept_timeout_returns_no_transport(void **state)
{
    struct wl_tcp_listener *l = NULL;
    struct wl_transport t = {NULL, NULL};
    char peer[64] = "unchanged";
    uint32_t start;

    (void)state;
    assert_int_equal(wl_tcp_listen("127.0.0.1", 0, &l), WL_OK);
    start = wl_clock_ms();
    assert_int_equal(wl_tcp_accept(l, 50, &t, peer, sizeof(peer)), WL_OK);
    assert_true(wl_clock_since(start) < 1000);
    assert_null(t.ops);
    assert_string_equal(peer, "unchanged");
    wl_tcp_listener_close(l);
}

/* The dual-stack listener takes IPv4, and names an IPv4 peer as one rather
 * than as ::ffff:127.0.0.1. */
static void test_any_interface_listener_takes_ipv4(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    char peer[80];
    char caps[256];

    (void)state;
    tcp_pair("", &c, &s, peer, sizeof(peer));
    assert_int_equal(strncmp(peer, "127.0.0.1:", 10), 0);
    assert_int_equal(wl_transport_caps(&s, caps, sizeof(caps)), WL_OK);
    assert_non_null(strstr(caps, "peer 127.0.0.1:"));
    wl_transport_close(&c);
    wl_transport_close(&s);
}

static void test_caps_names_both_ends(void **state)
{
    struct wl_transport c;
    struct wl_transport s;
    char peer[80];
    char want[128];
    char caps[256];
    char small[8];

    (void)state;
    tcp_pair("127.0.0.1", &c, &s, peer, sizeof(peer));
    assert_int_equal(wl_transport_caps(&s, caps, sizeof(caps)), WL_OK);
    assert_int_equal(strncmp(caps, "transport tcp\npeer ", 19), 0);
    (void)snprintf(want, sizeof(want), "peer %s\n", peer);
    assert_non_null(strstr(caps, want));
    assert_non_null(strstr(caps, "\nlocal 127.0.0.1:"));
    /* The client's local end is the server's peer. */
    assert_int_equal(wl_transport_caps(&c, caps, sizeof(caps)), WL_OK);
    (void)snprintf(want, sizeof(want), "local %s\n", peer);
    assert_non_null(strstr(caps, want));
    assert_int_equal(wl_transport_caps(&c, small, sizeof(small)), WL_ERR_NOSPACE);
    wl_transport_close(&c);
    wl_transport_close(&s);
}

static void test_ipv6_loopback(void **state)
{
    struct wl_tcp_listener *l = NULL;
    struct wl_transport c;
    struct wl_transport s;
    char peer[80];
    char caps[256];
    uint8_t buf[4];

    (void)state;
    if (wl_tcp_listen("::1", 0, &l) != WL_OK) {
        print_message("no IPv6 loopback here (%s)\n", wl_error());
        skip();
    }
    assert_int_equal(wl_tcp_connect("::1", wl_tcp_listener_port(l), 2000, &c), WL_OK);
    assert_int_equal(wl_tcp_accept(l, 2000, &s, peer, sizeof(peer)), WL_OK);
    wl_tcp_listener_close(l);
    assert_int_equal(strncmp(peer, "[::1]:", 6), 0);
    assert_int_equal(wl_transport_caps(&c, caps, sizeof(caps)), WL_OK);
    assert_non_null(strstr(caps, "peer [::1]:"));
    assert_int_equal(wl_transport_write(&c, (const uint8_t *)"six", 3), WL_OK);
    read_exactly(&s, buf, 3);
    assert_memory_equal(buf, "six", 3);
    wl_transport_close(&c);
    wl_transport_close(&s);
}

static void test_parse(void **state)
{
    char host[64];
    char tiny[4];
    uint16_t port = 0;

    (void)state;
    assert_int_equal(wl_tcp_parse("tcp:a21m", host, sizeof(host), &port), WL_OK);
    assert_string_equal(host, "a21m");
    assert_int_equal(port, WL_TCP_DEFAULT_PORT);

    assert_int_equal(wl_tcp_parse("tcp:10.0.0.5:4000", host, sizeof(host), &port), WL_OK);
    assert_string_equal(host, "10.0.0.5");
    assert_int_equal(port, 4000);

    assert_int_equal(wl_tcp_parse("tcp:[::1]:99", host, sizeof(host), &port), WL_OK);
    assert_string_equal(host, "::1");
    assert_int_equal(port, 99);

    assert_int_equal(wl_tcp_parse("tcp:[fe80::1]", host, sizeof(host), &port), WL_OK);
    assert_string_equal(host, "fe80::1");
    assert_int_equal(port, WL_TCP_DEFAULT_PORT);

    assert_int_equal(wl_tcp_parse("tcp:h:65535", host, sizeof(host), &port), WL_OK);
    assert_int_equal(port, 65535);

    /* Not a TCP spec at all: the caller's cue to treat it as a serial port. */
    assert_int_equal(wl_tcp_parse("/dev/ttyUSB0", host, sizeof(host), &port), WL_ERR_INVAL);
    assert_int_equal(wl_tcp_parse("COM1", host, sizeof(host), &port), WL_ERR_INVAL);
    assert_int_equal(wl_tcp_parse("tcp", host, sizeof(host), &port), WL_ERR_INVAL);

    /* A TCP spec that is wrong, which must not fall through to serial. */
    assert_int_equal(wl_tcp_parse("tcp:", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp::99", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:h:", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:h:12x", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:h:65536", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:h:0", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:h:99999999999", host, sizeof(host), &port),
                     WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:::1", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_non_null(strstr(wl_error(), "brackets"));
    assert_int_equal(wl_tcp_parse("tcp:[::1", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:[::1]x", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:[]:5", host, sizeof(host), &port), WL_ERR_CORRUPT);
    assert_int_equal(wl_tcp_parse("tcp:longhost", tiny, sizeof(tiny), &port), WL_ERR_NOSPACE);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_exchange_both_ways),
        cmocka_unit_test(test_large_transfer_arrives_whole),
        cmocka_unit_test(test_read_timeout_is_zero_bytes),
        cmocka_unit_test(test_peer_close_is_an_error),
        cmocka_unit_test(test_peer_reset_is_an_error),
        cmocka_unit_test(test_write_to_a_closed_peer_fails),
        cmocka_unit_test(test_connect_to_a_closed_port_fails_promptly),
        cmocka_unit_test(test_connect_times_out),
        cmocka_unit_test(test_unresolvable_names_fail),
        cmocka_unit_test(test_a_port_in_use_is_refused),
        cmocka_unit_test(test_accept_timeout_returns_no_transport),
        cmocka_unit_test(test_any_interface_listener_takes_ipv4),
        cmocka_unit_test(test_caps_names_both_ends),
        cmocka_unit_test(test_ipv6_loopback),
        cmocka_unit_test(test_parse),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
