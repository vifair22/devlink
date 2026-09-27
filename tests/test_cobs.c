#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "devlink/cobs.h"

static void check_vector(const uint8_t *raw, size_t raw_len, const uint8_t *enc,
                         size_t enc_len)
{
    uint8_t out[600];
    size_t n;

    assert_int_equal(wl_cobs_encode(raw, raw_len, out, sizeof(out), &n), WL_OK);
    assert_int_equal(n, enc_len);
    assert_memory_equal(out, enc, enc_len);

    assert_int_equal(wl_cobs_decode(enc, enc_len, out, sizeof(out), &n), WL_OK);
    assert_int_equal(n, raw_len);
    if (raw_len > 0)
        assert_memory_equal(out, raw, raw_len);
}

/* Vectors from the COBS paper / Wikipedia, which cover the zero and
 * 254-byte-group edge cases that are easy to get subtly wrong. */
static void test_known_vectors(void **state)
{
    (void)state;
    {
        const uint8_t raw[] = {0x00};
        const uint8_t enc[] = {0x01, 0x01};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
    {
        const uint8_t raw[] = {0x00, 0x00};
        const uint8_t enc[] = {0x01, 0x01, 0x01};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
    {
        const uint8_t raw[] = {0x00, 0x11, 0x00};
        const uint8_t enc[] = {0x01, 0x02, 0x11, 0x01};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
    {
        const uint8_t raw[] = {0x11, 0x22, 0x00, 0x33};
        const uint8_t enc[] = {0x03, 0x11, 0x22, 0x02, 0x33};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
    {
        const uint8_t raw[] = {0x11, 0x22, 0x33, 0x44};
        const uint8_t enc[] = {0x05, 0x11, 0x22, 0x33, 0x44};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
    {
        const uint8_t raw[] = {0x11, 0x00, 0x00, 0x00};
        const uint8_t enc[] = {0x02, 0x11, 0x01, 0x01, 0x01};
        check_vector(raw, sizeof(raw), enc, sizeof(enc));
    }
}

/* 254 non-zero bytes fill exactly one group: FF, the data, and nothing else.
 * 255 bytes spill one byte into a second group. */
static void test_full_group_boundaries(void **state)
{
    uint8_t raw[300];
    uint8_t enc[310];

    (void)state;
    for (size_t i = 0; i < sizeof(raw); i++)
        raw[i] = (uint8_t)((i % 255) + 1);

    enc[0] = 0xFF;
    memcpy(enc + 1, raw, 254);
    check_vector(raw, 254, enc, 255);

    enc[255] = 0x02;
    enc[256] = raw[254];
    check_vector(raw, 255, enc, 257);

    /* A zero straight after a full group. */
    raw[254] = 0x00;
    enc[255] = 0x01;
    enc[256] = 0x01;
    check_vector(raw, 255, enc, 257);
}

static void test_empty_input(void **state)
{
    const uint8_t enc[] = {0x01};

    (void)state;
    check_vector(NULL, 0, enc, sizeof(enc));
}

static void test_random_roundtrip(void **state)
{
    uint8_t raw[2048];
    uint8_t enc[WL_COBS_MAX_ENCODED(2048)];
    uint8_t dec[2048];

    (void)state;
    srand(98);
    for (int round = 0; round < 500; round++) {
        size_t len = (size_t)rand() % sizeof(raw);
        size_t n;
        size_t m;
        /* Vary the zero density so both long runs and dense zeros occur. */
        int zero_every = 1 + (rand() % 400);

        for (size_t i = 0; i < len; i++)
            raw[i] = (rand() % zero_every == 0) ? 0 : (uint8_t)(1 + (rand() % 255));
        assert_int_equal(wl_cobs_encode(raw, len, enc, sizeof(enc), &n), WL_OK);
        assert_true(n <= WL_COBS_MAX_ENCODED(len));
        assert_null(memchr(enc, 0, n));
        assert_int_equal(wl_cobs_decode(enc, n, dec, sizeof(dec), &m), WL_OK);
        assert_int_equal(m, len);
        if (len > 0)
            assert_memory_equal(dec, raw, len);
    }
}

static void test_encode_rejects_small_buffer(void **state)
{
    const uint8_t raw[] = {1, 2, 3};
    uint8_t out[3];
    size_t n;

    (void)state;
    assert_int_equal(wl_cobs_encode(raw, sizeof(raw), out, sizeof(out), &n),
                     WL_ERR_NOSPACE);
}

static void test_decode_rejects_corruption(void **state)
{
    uint8_t out[16];
    size_t n;

    (void)state;
    {
        const uint8_t zero_code[] = {0x00, 0x11};
        assert_int_equal(wl_cobs_decode(zero_code, 2, out, sizeof(out), &n),
                         WL_ERR_CORRUPT);
    }
    {
        const uint8_t truncated[] = {0x05, 0x11, 0x22};
        assert_int_equal(wl_cobs_decode(truncated, 3, out, sizeof(out), &n),
                         WL_ERR_CORRUPT);
    }
    {
        const uint8_t zero_data[] = {0x03, 0x11, 0x00};
        assert_int_equal(wl_cobs_decode(zero_data, 3, out, sizeof(out), &n),
                         WL_ERR_CORRUPT);
    }
}

static void test_decode_rejects_small_buffer(void **state)
{
    const uint8_t enc[] = {0x03, 0x11, 0x22, 0x02, 0x33};
    uint8_t out[4];
    size_t n;

    (void)state;
    /* Group data overflows. */
    assert_int_equal(wl_cobs_decode(enc, sizeof(enc), out, 1, &n), WL_ERR_NOSPACE);
    /* Data fits exactly, but the implied zero does not. */
    assert_int_equal(wl_cobs_decode(enc, sizeof(enc), out, 2, &n), WL_ERR_NOSPACE);
    assert_int_equal(wl_cobs_decode(enc, sizeof(enc), out, sizeof(out), &n), WL_OK);
    assert_int_equal(n, 4);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_known_vectors),
        cmocka_unit_test(test_full_group_boundaries),
        cmocka_unit_test(test_empty_input),
        cmocka_unit_test(test_random_roundtrip),
        cmocka_unit_test(test_encode_rejects_small_buffer),
        cmocka_unit_test(test_decode_rejects_corruption),
        cmocka_unit_test(test_decode_rejects_small_buffer),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
