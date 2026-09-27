#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "devlink/crc32.h"
#include "devlink/frame.h"

/* Frames delivered by the receiver, copied out because the payload pointer
 * is only valid during the callback. */
struct capture {
    int count;
    struct wl_frame last;
    uint8_t payload[WL_SERIAL_MAX_PAYLOAD];
};

static void capture_cb(void *ctx, const struct wl_frame *f)
{
    struct capture *c = ctx;

    c->count++;
    c->last = *f;
    memcpy(c->payload, f->payload, f->payload_len);
    c->last.payload = c->payload;
}

/* The receiver holds two frame-sized buffers; keep it off the stack as the
 * real link will. */
static struct wl_frame_rx rx;
static struct capture cap;

static int setup(void **state)
{
    (void)state;
    wl_frame_rx_init(&rx, WL_SERIAL_MAX_PAYLOAD);
    memset(&cap, 0, sizeof(cap));
    return 0;
}

static size_t encode(uint8_t type, uint16_t seq, const uint8_t *payload, size_t len,
                     uint8_t *out, size_t cap_len)
{
    struct wl_frame f = {type, seq, (uint16_t)(seq + 1), 0xA5A5F00DU, payload, len};
    size_t n;

    assert_int_equal(wl_frame_encode(&f, out, cap_len, &n), WL_OK);
    return n;
}

static void test_roundtrip_all_types(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    const uint8_t payload[] = {0x00, 0x01, 0x00, 0xFF, 0x00};
    size_t n;

    (void)state;
    for (int type = WL_FRAME_SYNC; type <= WL_FRAME_ACK; type++) {
        size_t len = type == WL_FRAME_ACK ? 0 : sizeof(payload);

        n = encode((uint8_t)type, 0xFFFE, payload, len, wire, sizeof(wire));
        assert_int_equal(wire[n - 1], 0);
        assert_null(memchr(wire, 0, n - 1));
        wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
        assert_int_equal(cap.last.type, type);
        assert_int_equal(cap.last.seq, 0xFFFE);
        assert_int_equal(cap.last.ack, 0xFFFF);
        assert_int_equal(cap.last.sack, 0xA5A5F00DU);
        assert_int_equal(cap.last.payload_len, len);
        if (len > 0)
            assert_memory_equal(cap.last.payload, payload, len);
    }
    assert_int_equal(cap.count, 4);
    assert_int_equal(rx.stats.frames, 4);
}

static void test_full_size_payload(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    uint8_t payload[WL_SERIAL_MAX_PAYLOAD];
    size_t n;

    (void)state;
    memset(payload, 0, sizeof(payload)); /* worst case for COBS: all zeros */
    n = encode(WL_FRAME_DATA, 7, payload, sizeof(payload), wire, sizeof(wire));
    assert_true(n <= WL_FRAME_MAX_WIRE);
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 1);
    assert_int_equal(cap.last.payload_len, sizeof(payload));
    assert_memory_equal(cap.last.payload, payload, sizeof(payload));
}

/* Serial reads return arbitrary chunks; one byte at a time is the extreme. */
static void test_byte_at_a_time(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    const uint8_t payload[] = "split across reads";
    size_t n;

    (void)state;
    n = encode(WL_FRAME_DATA, 3, payload, sizeof(payload), wire, sizeof(wire));
    for (size_t i = 0; i < n; i++) {
        wl_frame_rx_feed(&rx, wire + i, 1, capture_cb, &cap);
        assert_int_equal(cap.count, i + 1 == n ? 1 : 0);
    }
    assert_memory_equal(cap.last.payload, payload, sizeof(payload));
}

static void test_back_to_back_and_idle_delimiters(void **state)
{
    uint8_t wire[3 * WL_FRAME_MAX_WIRE];
    size_t n = 0;

    (void)state;
    wire[n++] = 0;
    wire[n++] = 0;
    n += encode(WL_FRAME_ACK, 0, NULL, 0, wire + n, sizeof(wire) - n);
    wire[n++] = 0;
    n += encode(WL_FRAME_DATA, 1, (const uint8_t *)"x", 1, wire + n, sizeof(wire) - n);
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 2);
    assert_int_equal(rx.stats.malformed, 0);
}

/* Flip every bit of an encoded frame in turn. Each flip must be caught: as a
 * CRC failure, a COBS error, or a split frame, but never delivered. */
static void test_single_bit_errors_never_delivered(void **state)
{
    uint8_t good[WL_FRAME_MAX_WIRE];
    uint8_t bad[WL_FRAME_MAX_WIRE + 1];
    const uint8_t payload[] = "integrity matters more than speed";
    size_t n;

    (void)state;
    n = encode(WL_FRAME_DATA, 42, payload, sizeof(payload), good, sizeof(good));
    for (size_t byte = 0; byte + 1 < n; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(bad, good, n);
            bad[byte] ^= (uint8_t)(1U << bit);
            wl_frame_rx_init(&rx, WL_SERIAL_MAX_PAYLOAD);
            cap.count = 0;
            wl_frame_rx_feed(&rx, bad, n, capture_cb, &cap);
            assert_int_equal(cap.count, 0);
        }
    }
}

/* Noise between frames costs at most the frame it lands next to; the one
 * after the next delimiter still arrives. */
static void test_resync_after_garbage(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    const uint8_t noise[] = {0x13, 0x37, 0xFF, 0x42};
    size_t n;

    (void)state;
    n = encode(WL_FRAME_DATA, 9, (const uint8_t *)"ok", 2, wire, sizeof(wire));
    wl_frame_rx_feed(&rx, noise, sizeof(noise), capture_cb, &cap);
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap); /* noise glued to frame */
    assert_int_equal(cap.count, 0);
    assert_int_equal(rx.stats.malformed + rx.stats.crc, 1);
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 1);
    assert_int_equal(cap.last.seq, 9);
}

static void test_overflow_skips_to_next_delimiter(void **state)
{
    static uint8_t junk[WL_FRAME_MAX_WIRE + 100];
    uint8_t wire[WL_FRAME_MAX_WIRE];
    size_t n;

    (void)state;
    memset(junk, 0x55, sizeof(junk));
    wl_frame_rx_feed(&rx, junk, sizeof(junk), capture_cb, &cap);
    assert_int_equal(rx.stats.overflow, 1);
    n = encode(WL_FRAME_DATA, 1, (const uint8_t *)"a", 1, wire, sizeof(wire));
    /* The first delimiter only ends the discarded run... */
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 0);
    /* ...and the next frame is received normally. */
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 1);
}

static void test_payload_over_negotiated_limit(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    uint8_t payload[64] = {0};
    size_t n;

    (void)state;
    wl_frame_rx_init(&rx, 32);
    n = encode(WL_FRAME_DATA, 1, payload, sizeof(payload), wire, sizeof(wire));
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 0);
    assert_int_equal(rx.stats.malformed, 1);
}

/* A frame with a valid CRC but an unknown type is a peer bug, counted as
 * malformed rather than as line noise. Built by hand since the encoder
 * refuses to produce one. */
static void test_valid_crc_bad_type(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    size_t n;

    (void)state;
    n = encode(WL_FRAME_DATA, 1, NULL, 0, wire, sizeof(wire));
    {
        uint8_t body[WL_FRAME_MIN_BODY];
        size_t m;
        assert_int_equal(wl_cobs_decode(wire, n - 1, body, sizeof(body), &m), WL_OK);
        body[0] = 99;
        {
            uint32_t c = wl_crc32(0, body, WL_FRAME_HEADER);
            body[9] = (uint8_t)c;
            body[10] = (uint8_t)(c >> 8);
            body[11] = (uint8_t)(c >> 16);
            body[12] = (uint8_t)(c >> 24);
        }
        assert_int_equal(wl_cobs_encode(body, m, wire, sizeof(wire) - 1, &n), WL_OK);
        wire[n++] = 0;
    }
    wl_frame_rx_feed(&rx, wire, n, capture_cb, &cap);
    assert_int_equal(cap.count, 0);
    assert_int_equal(rx.stats.malformed, 1);
    assert_int_equal(rx.stats.crc, 0);
}

static void test_encode_rejects_invalid_frames(void **state)
{
    uint8_t wire[WL_FRAME_MAX_WIRE];
    uint8_t big[WL_SERIAL_MAX_PAYLOAD + 1] = {0};
    size_t n;

    (void)state;
    {
        struct wl_frame f = {0, 0, 0, 0, NULL, 0};
        assert_int_equal(wl_frame_encode(&f, wire, sizeof(wire), &n), WL_ERR_INVAL);
    }
    {
        struct wl_frame f = {WL_FRAME_DATA, 0, 0, 0, big, sizeof(big)};
        assert_int_equal(wl_frame_encode(&f, wire, sizeof(wire), &n), WL_ERR_INVAL);
    }
    {
        struct wl_frame f = {WL_FRAME_ACK, 0, 0, 0, big, 1};
        assert_int_equal(wl_frame_encode(&f, wire, sizeof(wire), &n), WL_ERR_INVAL);
    }
    {
        struct wl_frame f = {WL_FRAME_ACK, 0, 0, 0, NULL, 0};
        assert_int_equal(wl_frame_encode(&f, wire, 0, &n), WL_ERR_NOSPACE);
        assert_int_equal(wl_frame_encode(&f, wire, 5, &n), WL_ERR_NOSPACE);
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_roundtrip_all_types, setup),
        cmocka_unit_test_setup(test_full_size_payload, setup),
        cmocka_unit_test_setup(test_byte_at_a_time, setup),
        cmocka_unit_test_setup(test_back_to_back_and_idle_delimiters, setup),
        cmocka_unit_test_setup(test_single_bit_errors_never_delivered, setup),
        cmocka_unit_test_setup(test_resync_after_garbage, setup),
        cmocka_unit_test_setup(test_overflow_skips_to_next_delimiter, setup),
        cmocka_unit_test_setup(test_payload_over_negotiated_limit, setup),
        cmocka_unit_test_setup(test_valid_crc_bad_type, setup),
        cmocka_unit_test_setup(test_encode_rejects_invalid_frames, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
