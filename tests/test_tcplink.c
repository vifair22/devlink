#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "devlink/linkif.h"
#include "devlink/posix_fd.h"
#include "devlink/session.h"
#include "devlink/tcplink.h"

/*
 * The TCP link on its own, in two ways. Driven directly in virtual time, two
 * links hand each other their wire bytes, which makes the timers exact: the
 * keepalive and the dead timer are tested to the millisecond. Then as the
 * layer under real sessions over a socketpair, which is what a connection is
 * once accepted.
 */

/* ---------------------------------------------------------------------------
 * two links, virtual time
 * ------------------------------------------------------------------------- */

struct pair {
    struct wl_linkif a;
    struct wl_linkif b;
    uint32_t now;
    int a_deaf; /* drop everything b sends to a, as if a had lost power */
};

static void make_pair(struct pair *p, uint32_t dead_ms, uint32_t keepalive_ms)
{
    struct wl_tcplink_cfg ca;
    struct wl_tcplink_cfg cb;

    memset(p, 0, sizeof(*p));
    p->now = 1000;
    wl_tcplink_cfg_init(&ca);
    ca.initiator = 1;
    ca.dead_ms = dead_ms;
    ca.keepalive_ms = keepalive_ms;
    cb = ca;
    cb.initiator = 0;
    assert_int_equal(wl_tcplink_make(&ca, 1, p->now, &p->a), WL_OK);
    assert_int_equal(wl_tcplink_make(&cb, 2, p->now, &p->b), WL_OK);
}

static void free_pair(struct pair *p)
{
    p->a.ops->free(p->a.impl);
    p->b.ops->free(p->b.impl);
}

/* Moves whatever one side has for the wire to the other, in odd-sized pieces
 * so no message boundary lines up with a read. */
static void carry(struct wl_linkif *from, struct wl_linkif *to, uint32_t now, int drop)
{
    uint8_t wire[777];
    size_t n;

    for (;;) {
        size_t room = to->ops->rx_room(to->impl);

        /* What the receiving pump would do: read no more than it has room
         * for, and leave the rest with the sender (in TCP, in the kernel). */
        if (!drop && room == 0)
            return;
        n = from->ops->poll_tx(from->impl, wire, !drop && room < sizeof(wire) ? room : sizeof(wire),
                               now);
        if (n == 0)
            return;
        if (!drop)
            to->ops->rx(to->impl, wire, n, now);
    }
}

static void step(struct pair *p, uint32_t ms)
{
    p->now += ms;
    carry(&p->a, &p->b, p->now, 0);
    carry(&p->b, &p->a, p->now, p->a_deaf);
}

static void test_up_at_once_and_one_generation(void **state)
{
    struct pair p;

    (void)state;
    make_pair(&p, 0, 0);
    assert_true(p.a.ops->is_up(p.a.impl));
    assert_int_equal(p.a.ops->generation(p.a.impl), 1);
    assert_int_equal(p.a.ops->failed(p.a.impl), WL_OK);
    free_pair(&p);
}

static void test_stream_arrives_whole_both_ways(void **state)
{
    enum { TOTAL = (3 * 65536) + 123 };
    static uint8_t src[TOTAL];
    static uint8_t dst_b[TOTAL];
    static uint8_t dst_a[TOTAL];
    size_t sent_a = 0, sent_b = 0, got_a = 0, got_b = 0;
    struct pair p;

    (void)state;
    for (size_t i = 0; i < TOTAL; i++)
        src[i] = (uint8_t)((i * 131U) + (i >> 9));
    make_pair(&p, 0, 0);
    for (int guard = 0; guard < 10000 && (got_a < TOTAL || got_b < TOTAL); guard++) {
        sent_a += p.a.ops->send(p.a.impl, src + sent_a, TOTAL - sent_a);
        sent_b += p.b.ops->send(p.b.impl, src + sent_b, TOTAL - sent_b);
        step(&p, 1);
        got_b += p.b.ops->recv(p.b.impl, dst_b + got_b, TOTAL - got_b);
        got_a += p.a.ops->recv(p.a.impl, dst_a + got_a, TOTAL - got_a);
    }
    assert_int_equal(got_a, TOTAL);
    assert_int_equal(got_b, TOTAL);
    assert_memory_equal(dst_a, src, TOTAL);
    assert_memory_equal(dst_b, src, TOTAL);
    assert_int_equal(p.a.ops->unacked(p.a.impl), 0);
    free_pair(&p);
}

static void test_idle_link_stays_up_on_keepalives(void **state)
{
    struct pair p;

    (void)state;
    make_pair(&p, 300, 50);
    for (int i = 0; i < 100; i++)
        step(&p, 20); /* two seconds of silence above the link */
    assert_true(p.a.ops->is_up(p.a.impl));
    assert_true(p.b.ops->is_up(p.b.impl));
    free_pair(&p);
}

static void test_keepalive_is_due_on_time(void **state)
{
    uint8_t wire[64];
    struct pair p;

    (void)state;
    make_pair(&p, 300, 50);
    /* Not driven yet, so its timers have not started: call it now. */
    assert_int_equal(p.a.ops->next_timeout(p.a.impl, p.now), 0);
    assert_int_equal(p.a.ops->poll_tx(p.a.impl, wire, sizeof(wire), p.now), 0);
    assert_int_equal(p.a.ops->next_timeout(p.a.impl, p.now), 50);
    assert_int_equal(p.a.ops->poll_tx(p.a.impl, wire, sizeof(wire), p.now + 49), 0);
    /* An empty message: a length of zero and nothing after it. */
    assert_int_equal(p.a.ops->poll_tx(p.a.impl, wire, sizeof(wire), p.now + 50), 2);
    assert_int_equal(wire[0], 0);
    assert_int_equal(wire[1], 0);
    free_pair(&p);
}

static void test_silent_peer_is_declared_gone(void **state)
{
    struct pair p;
    uint32_t start;

    (void)state;
    make_pair(&p, 300, 50);
    p.a_deaf = 1;
    start = p.now;
    while (p.a.ops->failed(p.a.impl) == WL_OK && p.now - start < 1000)
        step(&p, 10);
    assert_int_equal(p.a.ops->failed(p.a.impl), WL_ERR_IO);
    assert_true(p.now - start >= 300 && p.now - start <= 310);
    assert_false(p.a.ops->is_up(p.a.impl));
    /* Gone for good: nothing more is sent or accepted. */
    assert_int_equal(p.a.ops->send(p.a.impl, (const uint8_t *)"x", 1), 0);
    assert_int_equal(p.a.ops->next_timeout(p.a.impl, p.now), UINT32_MAX);
    /* The other side was hearing a's keepalives throughout. */
    assert_int_equal(p.b.ops->failed(p.b.impl), WL_OK);
    free_pair(&p);
}

static void test_a_slow_reader_is_not_a_dead_peer(void **state)
{
    static uint8_t src[4 * 65536];
    static uint8_t dst[sizeof(src)];
    size_t sent = 0, got = 0;
    struct pair p;

    (void)state;
    memset(src, 0x5a, sizeof(src));
    make_pair(&p, 300, 50);
    /* b's session takes nothing for two seconds while a fills everything it
     * can. b's buffers fill, carry() then stops reading as the pump would,
     * and neither side may mistake that for the other having gone. */
    for (int i = 0; i < 100; i++) {
        sent += p.a.ops->send(p.a.impl, src + sent, sizeof(src) - sent);
        step(&p, 20);
    }
    assert_int_equal(p.b.ops->failed(p.b.impl), WL_OK);
    assert_int_equal(p.a.ops->failed(p.a.impl), WL_OK);
    for (int guard = 0; guard < 10000 && got < sizeof(src); guard++) {
        sent += p.a.ops->send(p.a.impl, src + sent, sizeof(src) - sent);
        step(&p, 1);
        got += p.b.ops->recv(p.b.impl, dst + got, sizeof(dst) - got);
    }
    assert_int_equal(got, sizeof(src));
    assert_memory_equal(dst, src, sizeof(src));
    free_pair(&p);
}

static void test_a_message_is_held_until_it_is_whole(void **state)
{
    uint8_t buf[16];
    struct pair p;

    (void)state;
    make_pair(&p, 0, 0);
    /* Length 5, then only three of the five bytes. */
    p.b.ops->rx(p.b.impl, (const uint8_t *)"\x05\x00" "abc", 5, p.now);
    assert_int_equal(p.b.ops->recv(p.b.impl, buf, sizeof(buf)), 0);
    p.b.ops->rx(p.b.impl, (const uint8_t *)"de", 2, p.now);
    assert_int_equal(p.b.ops->recv(p.b.impl, buf, sizeof(buf)), 5);
    assert_memory_equal(buf, "abcde", 5);
    free_pair(&p);
}

/* ---------------------------------------------------------------------------
 * with a key
 * ------------------------------------------------------------------------- */

static const uint8_t key_a[32] = {0x42, 1, 2, 3};
static const uint8_t key_b[32] = {0x42, 1, 2, 4};

static void make_keyed(struct pair *p, const uint8_t *ka, const uint8_t *kb)
{
    struct wl_tcplink_cfg ca;
    struct wl_tcplink_cfg cb;

    memset(p, 0, sizeof(*p));
    p->now = 1000;
    wl_tcplink_cfg_init(&ca);
    ca.initiator = 1;
    ca.psk = ka;
    cb = ca;
    cb.initiator = 0;
    cb.psk = kb;
    assert_int_equal(wl_tcplink_make(&ca, 1, p->now, &p->a), WL_OK);
    assert_int_equal(wl_tcplink_make(&cb, 2, p->now, &p->b), WL_OK);
}

static void test_handshake_brings_both_sides_up(void **state)
{
    struct pair p;

    (void)state;
    make_keyed(&p, key_a, key_a);
    assert_false(p.a.ops->is_up(p.a.impl));
    assert_int_equal(p.a.ops->generation(p.a.impl), 0);
    /* Nothing can be sent before the keys exist. */
    assert_int_equal(p.a.ops->send(p.a.impl, (const uint8_t *)"x", 1), 0);
    step(&p, 1); /* the client's message out, the agent's answer back */
    step(&p, 1);
    assert_true(p.a.ops->is_up(p.a.impl));
    assert_true(p.b.ops->is_up(p.b.impl));
    assert_int_equal(p.a.ops->generation(p.a.impl), 1);
    free_pair(&p);
}

static void test_encrypted_stream_both_ways(void **state)
{
    enum { TOTAL = (2 * 65536) + 99 };
    static uint8_t src[TOTAL];
    static uint8_t dst_a[TOTAL];
    static uint8_t dst_b[TOTAL];
    size_t sent_a = 0, sent_b = 0, got_a = 0, got_b = 0;
    struct pair p;

    (void)state;
    for (size_t i = 0; i < TOTAL; i++)
        src[i] = (uint8_t)(i * 7U);
    make_keyed(&p, key_a, key_a);
    for (int guard = 0; guard < 10000 && (got_a < TOTAL || got_b < TOTAL); guard++) {
        sent_a += p.a.ops->send(p.a.impl, src + sent_a, TOTAL - sent_a);
        sent_b += p.b.ops->send(p.b.impl, src + sent_b, TOTAL - sent_b);
        step(&p, 1);
        got_b += p.b.ops->recv(p.b.impl, dst_b + got_b, TOTAL - got_b);
        got_a += p.a.ops->recv(p.a.impl, dst_a + got_a, TOTAL - got_a);
    }
    assert_int_equal(got_a, TOTAL);
    assert_int_equal(got_b, TOTAL);
    assert_memory_equal(dst_a, src, TOTAL);
    assert_memory_equal(dst_b, src, TOTAL);
    free_pair(&p);
}

static void test_plaintext_never_appears_on_the_wire(void **state)
{
    static const char secret[] = "the secret that must not show";
    uint8_t wire[WL_LINKIF_WIRE];
    size_t n;
    struct pair p;

    (void)state;
    make_keyed(&p, key_a, key_a);
    step(&p, 1);
    step(&p, 1);
    assert_int_equal(p.a.ops->send(p.a.impl, (const uint8_t *)secret, sizeof(secret)),
                     sizeof(secret));
    n = p.a.ops->poll_tx(p.a.impl, wire, sizeof(wire), p.now);
    assert_int_equal(n, 2 + sizeof(secret) + 16);
    assert_null(memmem(wire, n, "secret", 6));
    free_pair(&p);
}

static void test_wrong_key_fails_at_the_agent(void **state)
{
    struct pair p;

    (void)state;
    make_keyed(&p, key_a, key_b);
    step(&p, 1);
    /* The agent reads the proof in the client's first message and refuses;
     * it never answers, so the client learns nothing about the key. */
    assert_int_equal(p.b.ops->failed(p.b.impl), WL_ERR_CORRUPT);
    assert_false(p.b.ops->is_up(p.b.impl));
    step(&p, 1);
    assert_false(p.a.ops->is_up(p.a.impl));
    free_pair(&p);
}

static void test_a_changed_byte_ends_the_session(void **state)
{
    uint8_t wire[WL_LINKIF_WIRE];
    size_t n;
    struct pair p;

    (void)state;
    make_keyed(&p, key_a, key_a);
    step(&p, 1);
    step(&p, 1);
    (void)p.a.ops->send(p.a.impl, (const uint8_t *)"hello", 5);
    n = p.a.ops->poll_tx(p.a.impl, wire, sizeof(wire), p.now);
    assert_true(n > 4);
    wire[4] ^= 0x01;
    p.b.ops->rx(p.b.impl, wire, n, p.now);
    assert_int_equal(p.b.ops->failed(p.b.impl), WL_ERR_CORRUPT);
    assert_int_equal(p.b.ops->recv(p.b.impl, wire, sizeof(wire)), 0);
    free_pair(&p);
}

static void test_a_handshake_that_never_finishes_times_out(void **state)
{
    struct pair p;
    uint32_t start;

    (void)state;
    make_keyed(&p, key_a, key_a);
    p.a_deaf = 1; /* the agent's answer never arrives */
    start = p.now;
    while (p.a.ops->failed(p.a.impl) == WL_OK && p.now - start < 20000)
        step(&p, 100);
    assert_int_equal(p.a.ops->failed(p.a.impl), WL_ERR_IO);
    assert_true(p.now - start >= WL_TCPLINK_HANDSHAKE_MS);
    assert_true(p.now - start <= WL_TCPLINK_HANDSHAKE_MS + 200);
    free_pair(&p);
}

/* ---------------------------------------------------------------------------
 * sessions over a socketpair
 * ------------------------------------------------------------------------- */

struct end {
    struct wl_tcplink_cfg tcfg;
    struct wl_session_cfg scfg;
    struct wl_session *s;
    struct wl_transport t;
};

static void open_end(struct end *e, int fd, int initiator)
{
    wl_tcplink_cfg_init(&e->tcfg);
    e->tcfg.initiator = initiator;
    e->tcfg.psk = key_a;
    wl_session_cfg_init(&e->scfg);
    e->scfg.make_link = wl_tcplink_make;
    e->scfg.make_link_arg = &e->tcfg;
    e->s = wl_session_new(&e->scfg, 7, 0);
    assert_non_null(e->s);
    assert_int_equal(wl_fd_transport(fd, &e->t), WL_OK);
}

static void close_end(struct end *e)
{
    wl_session_free(e->s);
    wl_transport_close(&e->t);
}

static void socket_ends(struct end *a, struct end *b)
{
    int sv[2];

    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
    open_end(a, sv[0], 1);
    open_end(b, sv[1], 0);
}

static void test_session_over_a_socket(void **state)
{
    enum { TOTAL = 1 << 20 };
    static uint8_t src[TOTAL];
    static uint8_t dst[TOTAL];
    size_t sent = 0, got = 0;
    struct end a;
    struct end b;

    (void)state;
    for (size_t i = 0; i < TOTAL; i++)
        src[i] = (uint8_t)((i * 2654435761U) >> 13);
    socket_ends(&a, &b);
    assert_null(wl_session_link(a.s));
    for (int guard = 0; guard < 100000 && got < TOTAL; guard++) {
        sent += wl_session_send(a.s, src + sent, TOTAL - sent);
        assert_int_equal(wl_session_pump(a.s, &a.t, 0), WL_OK);
        assert_int_equal(wl_session_pump(b.s, &b.t, 1), WL_OK);
        got += wl_session_recv(b.s, dst + got, TOTAL - got);
    }
    assert_int_equal(got, TOTAL);
    assert_memory_equal(dst, src, TOTAL);
    close_end(&a);
    close_end(&b);
}

static void test_closed_connection_ends_the_session(void **state)
{
    struct end a;
    struct end b;
    wl_status st = WL_OK;

    (void)state;
    socket_ends(&a, &b);
    close_end(&a);
    for (int i = 0; i < 10 && st == WL_OK; i++)
        st = wl_session_pump(b.s, &b.t, 10);
    assert_int_equal(st, WL_ERR_IO);
    close_end(&b);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_up_at_once_and_one_generation),
        cmocka_unit_test(test_stream_arrives_whole_both_ways),
        cmocka_unit_test(test_idle_link_stays_up_on_keepalives),
        cmocka_unit_test(test_keepalive_is_due_on_time),
        cmocka_unit_test(test_silent_peer_is_declared_gone),
        cmocka_unit_test(test_a_slow_reader_is_not_a_dead_peer),
        cmocka_unit_test(test_a_message_is_held_until_it_is_whole),
        cmocka_unit_test(test_handshake_brings_both_sides_up),
        cmocka_unit_test(test_encrypted_stream_both_ways),
        cmocka_unit_test(test_plaintext_never_appears_on_the_wire),
        cmocka_unit_test(test_wrong_key_fails_at_the_agent),
        cmocka_unit_test(test_a_changed_byte_ends_the_session),
        cmocka_unit_test(test_a_handshake_that_never_finishes_times_out),
        cmocka_unit_test(test_session_over_a_socket),
        cmocka_unit_test(test_closed_connection_ends_the_session),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
