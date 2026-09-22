#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "sim_line.h"
#include "w98link/bytes.h"
#include "w98link/peer.h"
#include "w98link/proto.h"

/* Two peers over the simulated serial line, in virtual time. */

#define EVT_MAX 64

struct event {
    char kind; /* s session, r ready, o open, a answer, d data, c closed, e error */
    uint16_t ch;
    uint8_t type;
    uint16_t code;
    size_t len;
    uint8_t data[256];
};

struct side {
    struct wl_peer *peer;
    struct event ev[EVT_MAX];
    unsigned n_ev;
    uint64_t bytes_in; /* total record payload received */
    uint32_t first_ms; /* when the first record on channel 3 arrived */
    int running;
};

static struct sim_world w;
static struct side cli;
static struct side srv;

static struct event *push(struct side *s, char kind)
{
    struct event *e;

    assert_true(s->n_ev < EVT_MAX);
    e = &s->ev[s->n_ev++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    return e;
}

static void h_session(void *ctx)
{
    push(ctx, 's');
}

static void h_ready(void *ctx, const struct wl_hello *h)
{
    struct event *e = push(ctx, 'r');

    e->code = h->platform;
    e->len = strlen(h->build);
    memcpy(e->data, h->build, e->len);
}

static void h_open(void *ctx, uint16_t ch, uint8_t kind, const uint8_t *args, size_t n)
{
    struct event *e = push(ctx, 'o');

    e->ch = ch;
    e->type = kind;
    e->len = n < sizeof(e->data) ? n : sizeof(e->data);
    memcpy(e->data, args, e->len);
}

static void h_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n)
{
    struct event *e = push(ctx, 'a');

    e->ch = ch;
    e->type = (uint8_t)ok;
    e->code = code;
    e->len = n < sizeof(e->data) ? n : sizeof(e->data);
    memcpy(e->data, msg, e->len);
}

static void h_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct side *s = ctx;
    struct event *e;

    s->bytes_in += n;
    if (ch == 3 && s->first_ms == 0)
        s->first_ms = sim_now_ms(&w);
    /* Bulk traffic would swamp the log; only the first records are kept. */
    if (s->n_ev >= EVT_MAX - 4)
        return;
    e = push(s, 'd');
    e->ch = ch;
    e->type = type;
    e->len = n < sizeof(e->data) ? n : sizeof(e->data);
    memcpy(e->data, p, e->len);
}

static void h_closed(void *ctx, uint16_t ch)
{
    push(ctx, 'c')->ch = ch;
}

static void h_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    struct event *e = push(ctx, 'e');

    e->code = code;
    e->len = n < sizeof(e->data) ? n : sizeof(e->data);
    memcpy(e->data, msg, e->len);
}

static const struct sim_line_cfg serial_115200 = {11520, 2000, 0, 0, 0};

static struct wl_peer *make_peer(struct side *s, int is_client, uint32_t seed, uint8_t plat)
{
    struct wl_session_cfg cfg;
    struct wl_peer_handlers h;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = w.line.bytes_per_sec;
    memset(&h, 0, sizeof(h));
    h.ctx = s;
    h.session = h_session;
    h.ready = h_ready;
    h.open = h_open;
    h.opened = h_opened;
    h.record = h_record;
    h.closed = h_closed;
    h.error = h_error;
    return wl_peer_new(&cfg, seed, sim_now_ms(&w), is_client, plat, &h);
}

static int setup(void **state)
{
    (void)state;
    sim_init_world(&w, &serial_115200);
    memset(&cli, 0, sizeof(cli));
    memset(&srv, 0, sizeof(srv));
    cli.peer = make_peer(&cli, 1, 0xC11U, WL_PLATFORM_LINUX);
    srv.peer = make_peer(&srv, 0, 0x5E4U, WL_PLATFORM_WIN9X);
    assert_non_null(cli.peer);
    assert_non_null(srv.peer);
    cli.running = srv.running = 1;
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    wl_peer_free(cli.peer);
    wl_peer_free(srv.peer);
    return 0;
}

static struct wl_link *link_of(struct side *s)
{
    return wl_session_link(wl_peer_session(s->peer));
}

static int run_until(int (*cond)(void), uint32_t limit_ms)
{
    uint64_t end = w.now_us + ((uint64_t)limit_ms * 1000U);

    while (w.now_us < end) {
        if (cli.running)
            assert_int_equal(wl_peer_work(cli.peer), WL_OK);
        if (srv.running)
            assert_int_equal(wl_peer_work(srv.peer), WL_OK);
        if (cli.running)
            sim_tx(&w, link_of(&cli), &w.ab);
        if (srv.running)
            sim_tx(&w, link_of(&srv), &w.ba);
        if (cond())
            return 1;
        sim_advance(&w, link_of(&cli), cli.running, link_of(&srv), srv.running);
        sim_deliver(&w, &w.ab, link_of(&srv), srv.running);
        sim_deliver(&w, &w.ba, link_of(&cli), cli.running);
    }
    return cond();
}

static int both_ready(void)
{
    return wl_peer_ready(cli.peer) && wl_peer_ready(srv.peer);
}

static unsigned count(const struct side *s, char kind)
{
    unsigned n = 0;

    for (unsigned i = 0; i < s->n_ev; i++) {
        if (s->ev[i].kind == kind)
            n++;
    }
    return n;
}

static const struct event *last(const struct side *s, char kind)
{
    for (unsigned i = s->n_ev; i > 0; i--) {
        if (s->ev[i - 1].kind == kind)
            return &s->ev[i - 1];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_handshake(void **state)
{
    const struct event *e;

    (void)state;
    assert_true(run_until(both_ready, 5000));
    e = last(&cli, 'r');
    assert_non_null(e);
    assert_int_equal(e->code, WL_PLATFORM_WIN9X);
    assert_true(e->len > 0); /* the agent's build version */
    e = last(&srv, 'r');
    assert_non_null(e);
    assert_int_equal(e->code, WL_PLATFORM_LINUX);
    assert_int_equal(count(&cli, 's'), 1);
}

static int srv_saw_open(void)
{
    return count(&srv, 'o') > 0;
}

static int cli_saw_answer(void)
{
    return count(&cli, 'a') > 0;
}

static int cli_saw_two_answers(void)
{
    return count(&cli, 'a') >= 2;
}

static int cli_saw_close(void)
{
    return count(&cli, 'c') > 0;
}

static void test_open_records_close(void **state)
{
    static const uint8_t args[] = {'h', 'i'};
    uint16_t ch;
    const struct event *e;

    (void)state;
    assert_true(run_until(both_ready, 5000));
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, args, sizeof(args), &ch), WL_OK);
    assert_int_equal(ch & 1U, 1U); /* client channels are odd */
    assert_true(run_until(srv_saw_open, 5000));
    e = last(&srv, 'o');
    assert_int_equal(e->ch, ch);
    assert_int_equal(e->type, WL_KIND_EXEC);
    assert_int_equal(e->len, sizeof(args));
    assert_memory_equal(e->data, args, sizeof(args));

    assert_int_equal(wl_peer_accept(srv.peer, ch), WL_OK);
    assert_true(run_until(cli_saw_answer, 5000));
    assert_int_equal(last(&cli, 'a')->type, 1);

    assert_int_equal(wl_peer_send(srv.peer, ch, WL_EXEC_STDOUT, (const uint8_t *)"out", 3),
                     WL_OK);
    assert_int_equal(wl_peer_send(cli.peer, ch, WL_EXEC_STDIN_EOF, NULL, 0), WL_OK);
    assert_true(run_until(cli_saw_answer, 3000));
    (void)run_until(cli_saw_close, 1000);
    e = last(&cli, 'd');
    assert_non_null(e);
    assert_int_equal(e->type, WL_EXEC_STDOUT);
    assert_memory_equal(e->data, "out", 3);

    assert_int_equal(wl_peer_close(srv.peer, ch), WL_OK);
    assert_true(run_until(cli_saw_close, 5000));
    assert_int_equal(last(&cli, 'c')->ch, ch);
    assert_int_equal(wl_peer_close(cli.peer, ch), WL_OK);
}

static void test_reject(void **state)
{
    uint16_t ch;
    const struct event *e;

    (void)state;
    assert_true(run_until(both_ready, 5000));
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_PUT, NULL, 0, &ch), WL_OK);
    assert_true(run_until(srv_saw_open, 5000));
    assert_int_equal(wl_peer_reject(srv.peer, ch, WL_E_UNKNOWN, "not supported"), WL_OK);
    assert_true(run_until(cli_saw_answer, 5000));
    e = last(&cli, 'a');
    assert_int_equal(e->type, 0);
    assert_int_equal(e->code, WL_E_UNKNOWN);
    assert_memory_equal(e->data, "not supported", 13);
    /* The channel is gone on both sides, so the number can be used again. */
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &ch), WL_OK);
}

static uint64_t bulk_target;

static int bulk_done(void)
{
    return srv.bytes_in >= bulk_target;
}

/* One channel streaming bulk data must not stop a small record on another
 * from getting through promptly. */
static void test_interactive_channel_is_not_starved(void **state)
{
    uint8_t chunk[WL_REC_CHUNK];
    uint16_t bulk;
    uint16_t chat;
    uint32_t sent_at;

    (void)state;
    memset(chunk, 'x', sizeof(chunk));
    assert_true(run_until(both_ready, 5000));
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &bulk), WL_OK);
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &chat), WL_OK);
    assert_int_equal(chat, 3); /* h_record watches channel 3 */
    assert_true(run_until(srv_saw_open, 5000));
    assert_int_equal(wl_peer_accept(srv.peer, bulk), WL_OK);
    assert_int_equal(wl_peer_accept(srv.peer, chat), WL_OK);
    /* Both channels must be open before either can be written to. */
    assert_true(run_until(cli_saw_two_answers, 5000));

    /* Fill the bulk channel's queue, then send one small record. */
    bulk_target = 0;
    for (int i = 0; i < 16; i++) {
        size_t n = wl_peer_write(cli.peer, bulk, WL_EXEC_STDIN, chunk, sizeof(chunk));

        bulk_target += n;
        if (n < sizeof(chunk))
            break;
    }
    assert_true(bulk_target >= 4U * WL_REC_CHUNK);
    sent_at = sim_now_ms(&w);
    assert_int_equal(wl_peer_send(cli.peer, chat, WL_EXEC_STDIN, (const uint8_t *)"?", 1),
                     WL_OK);
    assert_true(run_until(bulk_done, 60000));
    assert_true(srv.first_ms != 0);
    /* One 4 KiB record ahead of it at 11.5 KB/s is about 0.4 s; the whole
     * queue would be nearly 3 s. */
    assert_true(srv.first_ms - sent_at < 1500U);
}

static void test_large_transfer_replenishes_credit(void **state)
{
    uint8_t chunk[WL_REC_CHUNK];
    uint16_t ch;

    (void)state;
    memset(chunk, 'z', sizeof(chunk));
    assert_true(run_until(both_ready, 5000));
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &ch), WL_OK);
    assert_true(run_until(srv_saw_open, 5000));
    assert_int_equal(wl_peer_accept(srv.peer, ch), WL_OK);
    assert_true(run_until(cli_saw_answer, 5000));

    /* Well past the initial 64 KiB window: without WINDOW records coming
     * back, this would stall for good. */
    bulk_target = 300000;
    {
        uint64_t sent = 0;
        uint64_t end = w.now_us + 300000000U;

        while (sent < bulk_target && w.now_us < end) {
            sent += wl_peer_write(cli.peer, ch, WL_EXEC_STDIN, chunk, sizeof(chunk));
            if (bulk_done())
                break;
            assert_int_equal(wl_peer_work(cli.peer), WL_OK);
            assert_int_equal(wl_peer_work(srv.peer), WL_OK);
            sim_tx(&w, link_of(&cli), &w.ab);
            sim_tx(&w, link_of(&srv), &w.ba);
            sim_advance(&w, link_of(&cli), 1, link_of(&srv), 1);
            sim_deliver(&w, &w.ab, link_of(&srv), 1);
            sim_deliver(&w, &w.ba, link_of(&cli), 1);
        }
    }
    assert_true(run_until(bulk_done, 120000));
    assert_true(srv.bytes_in >= bulk_target);
}

static int cli_second_session(void)
{
    return count(&cli, 's') > 1;
}

static void test_peer_restart_drops_channels(void **state)
{
    uint16_t ch;

    (void)state;
    assert_true(run_until(both_ready, 5000));
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &ch), WL_OK);
    assert_true(run_until(srv_saw_open, 5000));

    wl_peer_free(srv.peer);
    memset(&srv, 0, sizeof(srv));
    srv.peer = make_peer(&srv, 0, 0x5E42U, WL_PLATFORM_WIN9X);
    srv.running = 1;

    assert_true(run_until(cli_second_session, 30000));
    assert_true(run_until(both_ready, 10000));
    /* Channel numbers start again, and the old one is gone. */
    assert_int_equal(wl_peer_send(cli.peer, ch, WL_EXEC_STDIN, NULL, 0), WL_ERR_INVAL);
    assert_int_equal(wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &ch), WL_OK);
}

static int srv_saw_error(void)
{
    return count(&srv, 'e') > 0;
}

/* A record for a channel that was never opened is a protocol violation: the
 * session ends rather than carrying on with a confused peer. */
static void test_record_on_unknown_channel_is_ignored(void **state)
{
    uint8_t rec[WL_REC_HEADER + 2];

    (void)state;
    assert_true(run_until(both_ready, 5000));
    wl_put_u16le(rec, 99);
    rec[2] = WL_EXEC_STDIN;
    wl_put_u16le(rec + 3, 2);
    rec[5] = 'h';
    rec[6] = 'i';
    assert_int_equal(wl_session_send(wl_peer_session(cli.peer), rec, sizeof(rec)),
                     sizeof(rec));
    (void)run_until(srv_saw_error, 3000);
    assert_int_equal(count(&srv, 'e'), 0); /* dropped, not fatal */
    assert_int_equal(count(&srv, 'd'), 0);
    assert_true(wl_peer_ready(srv.peer));
}

static void test_malformed_control_record_ends_the_session(void **state)
{
    uint8_t rec[WL_REC_HEADER + 1];

    (void)state;
    assert_true(run_until(both_ready, 5000));
    wl_put_u16le(rec, WL_CONTROL_CHANNEL);
    rec[2] = 200; /* no such control type */
    wl_put_u16le(rec + 3, 1);
    rec[5] = 0;
    assert_int_equal(wl_session_send(wl_peer_session(cli.peer), rec, sizeof(rec)),
                     sizeof(rec));
    assert_true(run_until(srv_saw_error, 5000));
    assert_int_equal(last(&srv, 'e')->code, WL_E_MALFORMED);
    assert_int_equal(wl_peer_failed(srv.peer), WL_E_MALFORMED);
    assert_false(wl_peer_ready(srv.peer));
}

static void test_channel_table_limit(void **state)
{
    uint16_t ch;
    unsigned opened = 0;

    (void)state;
    assert_true(run_until(both_ready, 5000));
    while (wl_peer_open(cli.peer, WL_KIND_EXEC, NULL, 0, &ch) == WL_OK)
        opened++;
    assert_int_equal(opened, WL_PEER_MAX_CHANNELS);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_handshake, setup, teardown),
        cmocka_unit_test_setup_teardown(test_open_records_close, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reject, setup, teardown),
        cmocka_unit_test_setup_teardown(test_interactive_channel_is_not_starved, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_large_transfer_replenishes_credit, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_peer_restart_drops_channels, setup, teardown),
        cmocka_unit_test_setup_teardown(test_record_on_unknown_channel_is_ignored, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_malformed_control_record_ends_the_session,
                                        setup, teardown),
        cmocka_unit_test_setup_teardown(test_channel_table_limit, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
