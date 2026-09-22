#include <glob.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "sim_line.h"
#include "w98link/session.h"

/*
 * Compression over the simulated line. The corpus is this repository's own C
 * source, including the vendored zlib: about as close to real development
 * traffic as a test can get without the A21m.
 */

struct sside {
    struct wl_session *s;
    const uint8_t *src; /* what this side sends */
    size_t src_len;
    size_t sent;
    const uint8_t *expect; /* what it should receive */
    size_t expect_len;
    size_t recvd;
    uint64_t corrupt;
    int running;
    uint32_t gen;
    uint32_t last_rx_ms; /* when the last plaintext byte arrived */
    /* A plan to switch to when the next session starts. Until then the old
     * session's last frames are still arriving and belong to the old plan. */
    int has_next;
    const uint8_t *next_src;
    size_t next_src_len;
    const uint8_t *next_expect;
    size_t next_expect_len;
};

static struct sim_world w;
static struct sside a;
static struct sside b;
static uint8_t *corpus;
static size_t corpus_len;
static uint8_t *noise;
static size_t noise_len;

static const struct sim_line_cfg serial_115200 = {11520, 2000, 0, 0, 0};

static void load_corpus(void)
{
    static const char *const patterns[] = {"src/*/*.c", "include/w98link/*.h",
                                           "tests/*.c", "third_party/zlib/*.[ch]"};
    size_t cap = 1U << 20;

    corpus = malloc(cap);
    assert_non_null(corpus);
    corpus_len = 0;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        glob_t g;

        if (glob(patterns[p], 0, NULL, &g) != 0)
            continue;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "rb");
            size_t n;

            if (f == NULL)
                continue;
            n = fread(corpus + corpus_len, 1, cap - corpus_len, f);
            corpus_len += n;
            (void)fclose(f);
        }
        globfree(&g);
    }
    /* Run from the repository root (make test does); without the sources
     * there is nothing meaningful to measure. */
    assert_true(corpus_len > 100000);

    noise_len = 100000;
    noise = malloc(noise_len);
    assert_non_null(noise);
    for (size_t i = 0; i < noise_len; i++)
        noise[i] = (uint8_t)sim_rnd(&w);
}

static int group_setup(void **state)
{
    (void)state;
    sim_init_world(&w, &serial_115200);
    load_corpus();
    return 0;
}

static int group_teardown(void **state)
{
    (void)state;
    free(corpus);
    free(noise);
    return 0;
}

static struct wl_session *make_session(uint32_t seed)
{
    struct wl_session_cfg cfg;
    struct wl_session *s;

    cfg.link.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.link.window = WL_LINK_DEFAULT_WINDOW;
    cfg.link.bytes_per_sec = w.line.bytes_per_sec;
    cfg.level = WL_SESSION_DEFAULT_LEVEL;
    s = wl_session_new(&cfg, seed, sim_now_ms(&w));
    assert_non_null(s);
    return s;
}

static void setup_pair(const struct sim_line_cfg *line)
{
    sim_init_world(&w, line);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.s = make_session(0xA11CEU);
    b.s = make_session(0xB0BU);
    a.running = b.running = 1;
}

static void teardown_pair(void)
{
    wl_session_free(a.s);
    wl_session_free(b.s);
}

/* a sends src to b, and b sends its src to a. */
static void plan(const uint8_t *a_src, size_t a_len, const uint8_t *b_src, size_t b_len)
{
    a.src = a_src;
    a.src_len = a_len;
    a.expect = b_src;
    a.expect_len = b_len;
    b.src = b_src;
    b.src_len = b_len;
    b.expect = a_src;
    b.expect_len = a_len;
}

static void new_generation(struct sside *s)
{
    s->gen = wl_session_generation(s->s);
    s->sent = 0;
    s->recvd = 0;
    if (s->has_next) {
        s->src = s->next_src;
        s->src_len = s->next_src_len;
        s->expect = s->next_expect;
        s->expect_len = s->next_expect_len;
        s->has_next = 0;
    }
}

/* Like plan(), but taking effect at each side's next session. */
static void plan_next(const uint8_t *a_src, size_t a_len, const uint8_t *b_src, size_t b_len)
{
    a.has_next = b.has_next = 1;
    a.next_src = a_src;
    a.next_src_len = a_len;
    a.next_expect = b_src;
    a.next_expect_len = b_len;
    b.next_src = b_src;
    b.next_src_len = b_len;
    b.next_expect = a_src;
    b.next_expect_len = a_len;
}

static void side_step(struct sside *s)
{
    uint8_t buf[4096];
    size_t n;

    if (!s->running)
        return;
    if (wl_session_generation(s->s) != s->gen)
        new_generation(s);
    if (s->sent < s->src_len)
        s->sent += wl_session_send(s->s, s->src + s->sent, s->src_len - s->sent);
    assert_int_equal(wl_session_work(s->s), WL_OK);
    /* work may have come up and reset; pick up the new generation first */
    if (wl_session_generation(s->s) != s->gen)
        new_generation(s);
    while ((n = wl_session_recv(s->s, buf, sizeof(buf))) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (s->recvd + i >= s->expect_len || buf[i] != s->expect[s->recvd + i])
                s->corrupt++;
        }
        s->recvd += n;
        s->last_rx_ms = sim_now_ms(&w);
    }
}

static int run_until(int (*cond)(void), uint32_t limit_ms)
{
    uint64_t end = w.now_us + ((uint64_t)limit_ms * 1000U);

    while (w.now_us < end) {
        side_step(&a);
        side_step(&b);
        if (a.running)
            sim_tx(&w, wl_session_link(a.s), &w.ab);
        if (b.running)
            sim_tx(&w, wl_session_link(b.s), &w.ba);
        if (cond())
            return 1;
        sim_advance(&w, wl_session_link(a.s), a.running, wl_session_link(b.s), b.running);
        sim_deliver(&w, &w.ab, wl_session_link(b.s), b.running);
        sim_deliver(&w, &w.ba, wl_session_link(a.s), a.running);
    }
    return cond();
}

static int both_up(void)
{
    return wl_session_is_up(a.s) && wl_session_is_up(b.s);
}

static int transfer_done(void)
{
    return a.recvd >= a.expect_len && b.recvd >= b.expect_len && !wl_session_busy(a.s) &&
           !wl_session_busy(b.s);
}

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_source_code_goes_several_times_line_rate(void **state)
{
    struct wl_session_stats st;
    uint64_t start;
    double secs;
    double rate;

    (void)state;
    setup_pair(&serial_115200);
    plan(corpus, corpus_len, NULL, 0);
    assert_true(run_until(both_up, 2000));
    start = w.now_us;
    assert_true(run_until(transfer_done, 600000));
    secs = (double)(w.now_us - start) / 1e6;
    rate = (double)corpus_len / secs;
    wl_session_get_stats(a.s, &st);

    assert_int_equal(b.corrupt, 0);
    assert_int_equal(b.recvd, corpus_len);
    printf("    source: %zu bytes -> %llu on the wire (%.2fx), %.0f B/s effective\n",
           corpus_len, (unsigned long long)st.z_out,
           (double)st.plain_out / (double)st.z_out, rate);
    /* The line carries about 11 KB/s; source code should go at least 3x. */
    assert_true(rate > 3.0 * 11520.0);
    teardown_pair();
}

static void test_incompressible_data_costs_little(void **state)
{
    uint64_t start;
    double rate;

    (void)state;
    setup_pair(&serial_115200);
    plan(noise, noise_len, NULL, 0);
    assert_true(run_until(both_up, 2000));
    start = w.now_us;
    assert_true(run_until(transfer_done, 120000));
    rate = (double)noise_len / ((double)(w.now_us - start) / 1e6);
    printf("    random: %.0f B/s effective\n", rate);
    assert_int_equal(b.corrupt, 0);
    assert_true(rate > 0.90 * 11520.0);
    teardown_pair();
}

static void test_both_directions_at_once(void **state)
{
    (void)state;
    setup_pair(&serial_115200);
    plan(corpus, corpus_len, noise, noise_len);
    assert_true(run_until(transfer_done, 600000));
    assert_int_equal(a.corrupt + b.corrupt, 0);
    assert_int_equal(a.recvd, noise_len);
    assert_int_equal(b.recvd, corpus_len);
    teardown_pair();
}

static int b_has_message(void)
{
    return b.recvd >= b.expect_len;
}

/* Small messages, as interactive command output will be, must go out at
 * once rather than wait for more data to fill a deflate block. */
static void test_small_messages_are_flushed_promptly(void **state)
{
    static const uint8_t msg[] = "C:\\SRC> make\r\n";
    struct wl_session_stats st;

    (void)state;
    setup_pair(&serial_115200);
    plan(NULL, 0, NULL, 0);
    assert_true(run_until(both_up, 2000));
    for (int i = 0; i < 20; i++) {
        uint32_t sent_at;

        (void)run_until(b_has_message, 500); /* idle gap */
        a.src = msg;
        a.src_len = sizeof(msg) - 1;
        a.sent = 0;
        b.expect = msg;
        b.expect_len = sizeof(msg) - 1;
        b.recvd = 0;
        sent_at = sim_now_ms(&w);
        assert_true(run_until(b_has_message, 1000));
        assert_true(b.last_rx_ms - sent_at < 50U);
    }
    assert_int_equal(b.corrupt, 0);
    wl_session_get_stats(a.s, &st);
    assert_true(st.flushes >= 20);
    teardown_pair();
}

static uint32_t first_gen;

static int done_in_new_session(void)
{
    return wl_session_generation(a.s) > first_gen && transfer_done();
}

/* After a restart, both compressors must start from nothing: if either kept
 * its history, the other side's fresh decompressor would reject the stream. */
static void test_restart_resets_both_streams(void **state)
{
    (void)state;
    setup_pair(&serial_115200);
    plan(corpus, corpus_len, corpus, corpus_len);
    assert_true(run_until(both_up, 2000));
    (void)run_until(transfer_done, 5000); /* part way */
    first_gen = wl_session_generation(a.s);

    wl_session_free(b.s);
    b.s = make_session(0xB0B2U);
    b.gen = 0;
    plan_next(corpus, 50000, corpus + 1000, 50000);
    assert_true(run_until(done_in_new_session, 120000));
    assert_int_equal(a.corrupt + b.corrupt, 0);
    teardown_pair();
}

static void test_noisy_line_with_compression(void **state)
{
    static const struct sim_line_cfg noisy = {11520, 2000, 200, 200, 100};

    (void)state;
    setup_pair(&noisy);
    plan(corpus, 120000, noise, 30000);
    assert_true(run_until(transfer_done, 600000));
    assert_int_equal(a.corrupt + b.corrupt, 0);
    teardown_pair();
}

/* A reader that stops must stall the whole pipeline without losing bytes:
 * plaintext queue, compressor, link window, all backing up in turn. */
static void test_slow_reader_backs_up_the_pipeline(void **state)
{
    (void)state;
    setup_pair(&serial_115200);
    plan(corpus, corpus_len, NULL, 0);
    b.running = 1;
    assert_true(run_until(both_up, 2000));
    {
        /* Stop b's application from reading: only its link keeps running,
         * which is what fills every buffer between the two applications. */
        struct wl_session *hold = b.s;
        uint64_t end = w.now_us + 30000000U;

        while (w.now_us < end) {
            side_step(&a);
            assert_int_equal(wl_session_work(hold), WL_OK);
            sim_tx(&w, wl_session_link(a.s), &w.ab);
            sim_tx(&w, wl_session_link(hold), &w.ba);
            sim_advance(&w, wl_session_link(a.s), 1, wl_session_link(hold), 1);
            sim_deliver(&w, &w.ab, wl_session_link(hold), 1);
            sim_deliver(&w, &w.ba, wl_session_link(a.s), 1);
        }
        assert_true(a.sent < corpus_len || wl_session_busy(a.s));
    }
    assert_true(run_until(transfer_done, 600000));
    assert_int_equal(b.corrupt, 0);
    assert_int_equal(b.recvd, corpus_len);
    teardown_pair();
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_source_code_goes_several_times_line_rate),
        cmocka_unit_test(test_incompressible_data_costs_little),
        cmocka_unit_test(test_both_directions_at_once),
        cmocka_unit_test(test_small_messages_are_flushed_promptly),
        cmocka_unit_test(test_restart_resets_both_streams),
        cmocka_unit_test(test_noisy_line_with_compression),
        cmocka_unit_test(test_slow_reader_backs_up_the_pipeline),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
