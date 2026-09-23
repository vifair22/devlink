#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "sim_line.h"
#include "devlink/link.h"

/*
 * Two links joined by a simulated serial line, in virtual time. The line
 * serialises bytes at a fixed rate, adds latency, and can drop bytes, flip
 * bits and inject garbage. Each side streams a deterministic byte sequence to
 * the other and checks every byte it receives, so any loss, duplication,
 * reordering or corruption that gets past the link fails the test.
 */

struct side {
    struct wl_link *l;
    int id;         /* stream identity, so A's bytes differ from B's */
    uint64_t sent;  /* stream bytes accepted by wl_link_send */
    uint64_t recvd; /* stream bytes received and verified */
    uint64_t total; /* how much to send */
    int reading;    /* 0 simulates an application that stopped reading */
    int running;    /* 0 simulates a process that is not there */
    uint32_t seen_gen;
};

struct sim {
    struct sim_world w;
    struct side a;
    struct side b;
    uint64_t corrupt_bytes; /* stream bytes that arrived wrong */
};

static struct sim sim;

static uint8_t stream_byte(int id, uint64_t k)
{
    uint64_t x = (k * 0x9E3779B97F4A7C15ULL) + ((uint64_t)(unsigned)id * 0xBF58476D1CE4E5B9ULL);

    x ^= x >> 31;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 29;
    return (uint8_t)x;
}

static uint32_t now_ms(void)
{
    return sim_now_ms(&sim.w);
}

static void side_app(struct side *s, int peer_id)
{
    uint8_t buf[2048];
    size_t n;

    if (!s->running)
        return;
    /* A new session means the old stream is gone on both ends; restart the
     * byte sequences, as the compression layer will restart its streams. */
    if (wl_link_generation(s->l) != s->seen_gen) {
        s->seen_gen = wl_link_generation(s->l);
        s->sent = 0;
        s->recvd = 0;
    }
    while (s->sent < s->total) {
        size_t want = s->total - s->sent < sizeof(buf) ? (size_t)(s->total - s->sent)
                                                       : sizeof(buf);
        for (size_t i = 0; i < want; i++)
            buf[i] = stream_byte(s->id, s->sent + i);
        n = wl_link_send(s->l, buf, want);
        s->sent += n;
        if (n < want)
            break;
    }
    while (s->reading && (n = wl_link_recv(s->l, buf, sizeof(buf))) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != stream_byte(peer_id, s->recvd + i))
                sim.corrupt_bytes++;
        }
        s->recvd += n;
    }
}

static int both_done(void)
{
    return sim.a.recvd >= sim.b.total && sim.b.recvd >= sim.a.total &&
           wl_link_unacked(sim.a.l) == 0 && wl_link_unacked(sim.b.l) == 0;
}

/* Advances virtual time until cond() holds or limit_ms passes. */
static int run_until(int (*cond)(void), uint32_t limit_ms)
{
    uint64_t end = sim.w.now_us + ((uint64_t)limit_ms * 1000U);

    while (sim.w.now_us < end) {
        side_app(&sim.a, sim.b.id);
        side_app(&sim.b, sim.a.id);
        if (sim.a.running)
            sim_tx(&sim.w, sim.a.l, &sim.w.ab);
        if (sim.b.running)
            sim_tx(&sim.w, sim.b.l, &sim.w.ba);
        if (cond())
            return 1;
        sim_advance(&sim.w, sim.a.l, sim.a.running, sim.b.l, sim.b.running);
        sim_deliver(&sim.w, &sim.w.ab, sim.b.l, sim.b.running);
        sim_deliver(&sim.w, &sim.w.ba, sim.a.l, sim.a.running);
    }
    return cond();
}

static struct wl_link *make_link(uint32_t seed, uint16_t max_payload, uint8_t window)
{
    struct wl_link_cfg cfg;
    struct wl_link *l;

    wl_link_cfg_init(&cfg);
    cfg.max_payload = max_payload;
    cfg.window = window;
    cfg.bytes_per_sec = sim.w.line.bytes_per_sec;
    l = wl_link_new(&cfg, seed, now_ms());
    assert_non_null(l);
    return l;
}

static void sim_init(const struct sim_line_cfg *line, uint64_t total_each_way)
{
    memset(&sim, 0, sizeof(sim));
    sim_init_world(&sim.w, line);
    sim.a.id = 1;
    sim.b.id = 2;
    sim.a.total = sim.b.total = total_each_way;
    sim.a.reading = sim.b.reading = 1;
    sim.a.running = sim.b.running = 1;
    sim.a.l = make_link(0xA11CEU, WL_SERIAL_MAX_PAYLOAD, WL_LINK_DEFAULT_WINDOW);
    sim.b.l = make_link(0xB0BU, WL_SERIAL_MAX_PAYLOAD, WL_LINK_DEFAULT_WINDOW);
}

static void sim_free(void)
{
    wl_link_free(sim.a.l);
    wl_link_free(sim.b.l);
}

static int both_up(void)
{
    return wl_link_is_up(sim.a.l) && wl_link_is_up(sim.b.l);
}

static const struct sim_line_cfg serial_115200 = {11520, 2000, 0, 0, 0};

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_clean_line_runs_near_line_rate(void **state)
{
    struct wl_link_stats st;
    uint64_t start;
    double secs;
    double rate;

    (void)state;
    sim_init(&serial_115200, 200000);
    assert_true(run_until(both_up, 2000));
    start = sim.w.now_us;
    assert_true(run_until(both_done, 120000));
    secs = (double)(sim.w.now_us - start) / 1e6;
    rate = (double)sim.a.total / secs;

    assert_int_equal(sim.corrupt_bytes, 0);
    wl_link_get_stats(sim.a.l, &st);
    assert_int_equal(st.retransmits, 0);
    /* Full duplex, so each direction should get most of the raw 11520 B/s;
     * the rest is framing, acks sharing the wire, and pacing slack. */
    printf("    clean: %.0f B/s each way, rto %u ms\n", rate, (unsigned)st.rto_ms);
    assert_true(rate > 0.90 * 11520.0);
    sim_free();
}

static void test_noisy_line_never_corrupts(void **state)
{
    static const struct sim_line_cfg noisy = {11520, 2000, 200, 200, 100};
    struct wl_link_stats st;

    (void)state;
    sim_init(&noisy, 150000);
    assert_true(run_until(both_done, 600000));
    assert_int_equal(sim.corrupt_bytes, 0);
    assert_int_equal(sim.a.recvd, sim.b.total);
    assert_int_equal(sim.b.recvd, sim.a.total);
    wl_link_get_stats(sim.a.l, &st);
    printf("    noisy: %u retransmits (%u timer, %u fast), rx crc %u malformed %u\n",
           (unsigned)st.retransmits, (unsigned)st.timeouts, (unsigned)st.fast_retx,
           (unsigned)st.rx.crc, (unsigned)st.rx.malformed);
    assert_true(st.retransmits > 0);
    sim_free();
}

/* About one damaged byte in a thousand: roughly two thirds of full-size
 * frames are lost. (At several per thousand almost no 1 KiB frame survives,
 * which no retransmission scheme fixes; smaller frames would.) */
static void test_very_bad_line_still_delivers(void **state)
{
    static const struct sim_line_cfg awful = {11520, 20000, 500, 300, 200};

    (void)state;
    sim_init(&awful, 30000);
    assert_true(run_until(both_done, 1800000));
    assert_int_equal(sim.corrupt_bytes, 0);
    sim_free();
}

/* No pacing, no serialisation delay: the TCP-like extreme, and enough frames
 * with a tiny payload to wrap the 16-bit sequence numbers several times. */
static void test_sequence_numbers_wrap(void **state)
{
    static const struct sim_line_cfg fast = {0, 500, 0, 0, 0};

    (void)state;
    sim_init(&fast, 0);
    wl_link_free(sim.a.l);
    wl_link_free(sim.b.l);
    sim.a.l = make_link(0xA11CEU, 16, WL_LINK_MAX_WINDOW);
    sim.b.l = make_link(0xB0BU, 16, WL_LINK_MAX_WINDOW);
    sim.a.total = sim.b.total = (uint64_t)16U * 200000U; /* 200k frames each way */
    assert_true(run_until(both_done, 3600000));
    assert_int_equal(sim.corrupt_bytes, 0);
    sim_free();
}

static void test_late_starter_connects(void **state)
{
    (void)state;
    sim_init(&serial_115200, 5000);
    sim.b.running = 0;
    assert_false(run_until(both_up, 5000));
    sim.b.running = 1;
    assert_true(run_until(both_up, 2000));
    assert_true(run_until(both_done, 10000));
    assert_int_equal(sim.corrupt_bytes, 0);
    sim_free();
}

/* Killing and restarting one side mid-transfer must be detected by the other
 * as a new session (generation change), never as a stream with a hole. */
static uint32_t restart_gen;

/* Done only once the transfer has happened in the new session; the old
 * session's byte counts would otherwise satisfy both_done straight away. */
static int done_in_new_session(void)
{
    return wl_link_generation(sim.a.l) > restart_gen && both_done();
}

static void test_peer_restart_starts_new_session(void **state)
{
    struct wl_link_stats st;
    uint32_t gen;

    (void)state;
    sim_init(&serial_115200, 100000);
    assert_true(run_until(both_up, 2000));
    (void)run_until(both_done, 3000); /* part way through */
    gen = wl_link_generation(sim.a.l);
    restart_gen = gen;

    wl_link_free(sim.b.l);
    sim.b.l = make_link(0xB0B2U, WL_SERIAL_MAX_PAYLOAD, WL_LINK_DEFAULT_WINDOW);
    sim.b.seen_gen = 0;
    sim.a.total = sim.b.total = 20000;

    assert_true(run_until(done_in_new_session, 60000));
    assert_true(wl_link_generation(sim.a.l) > gen);
    assert_int_equal(sim.corrupt_bytes, 0);
    wl_link_get_stats(sim.a.l, &st);
    assert_int_equal(st.resets, 1);
    sim_free();
}

static int a_down(void)
{
    return !wl_link_is_up(sim.a.l);
}

static void test_silent_peer_times_out_then_reconnects(void **state)
{
    uint32_t gen;

    (void)state;
    sim_init(&serial_115200, 0);
    assert_true(run_until(both_up, 2000));
    gen = wl_link_generation(sim.a.l);
    sim.b.running = 0; /* cable pulled, or the process frozen */
    assert_false(run_until(a_down, 9000));
    assert_true(run_until(a_down, 3000));

    sim.b.running = 1;
    assert_true(run_until(both_up, 15000));
    assert_true(wl_link_generation(sim.a.l) > gen);
    sim_free();
}

/* An application that stops reading must stall the sender, not lose data. */
static void test_backpressure_from_slow_reader(void **state)
{
    (void)state;
    sim_init(&serial_115200, 120000);
    sim.b.reading = 0;
    (void)run_until(both_done, 30000);
    assert_true(sim.b.recvd == 0);
    assert_true(wl_link_unacked(sim.a.l) > 0);
    sim.b.reading = 1;
    assert_true(run_until(both_done, 120000));
    assert_int_equal(sim.corrupt_bytes, 0);
    sim_free();
}

static void test_idle_link_stays_up(void **state)
{
    (void)state;
    sim_init(&serial_115200, 0);
    assert_true(run_until(both_up, 2000));
    /* A minute of silence: keepalive acks must hold the session. */
    assert_false(run_until(a_down, 60000));
    assert_true(both_up());
    sim_free();
}

static void test_send_before_up_is_refused(void **state)
{
    struct wl_link_cfg cfg;
    struct wl_link *l;

    wl_link_cfg_init(&cfg);
    l = wl_link_new(&cfg, 1, 0);
    uint8_t b = 0;

    (void)state;
    assert_non_null(l);
    assert_false(wl_link_is_up(l));
    assert_int_equal(wl_link_send(l, &b, 1), 0);
    assert_int_equal(wl_link_poll_tx(l, &b, 1, 0), 0); /* buffer too small */
    wl_link_free(l);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_clean_line_runs_near_line_rate),
        cmocka_unit_test(test_noisy_line_never_corrupts),
        cmocka_unit_test(test_very_bad_line_still_delivers),
        cmocka_unit_test(test_sequence_numbers_wrap),
        cmocka_unit_test(test_late_starter_connects),
        cmocka_unit_test(test_peer_restart_starts_new_session),
        cmocka_unit_test(test_silent_peer_times_out_then_reconnects),
        cmocka_unit_test(test_backpressure_from_slow_reader),
        cmocka_unit_test(test_idle_link_stays_up),
        cmocka_unit_test(test_send_before_up_is_refused),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
