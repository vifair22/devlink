#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "devlink/kdf.h"
#include "devlink/noise.h"
#include "devlink/random.h"

/* Big enough for any message Noise allows, plus room to prove the limit is
 * enforced by the code and not by the buffer. */
#define BUF (WL_NOISE_MAX_MSG + 64U)

static uint8_t buf_a[BUF];
static uint8_t buf_b[BUF];

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2U;

    assert_true(n <= cap);
    for (size_t i = 0; i < n; i++) {
        unsigned v;

        assert_int_equal(sscanf(hex + (2U * i), "%2x", &v), 1);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void assert_hex(const uint8_t *got, size_t n, const char *hex)
{
    uint8_t want[256];

    assert_int_equal(unhex(hex, want, sizeof(want)), n);
    assert_memory_equal(got, want, n);
}

/* ---- HMAC-SHA256: RFC 4231 section 4, every case ---- */

static void hmac_case(const uint8_t *key, size_t key_len, const char *data,
                      size_t trunc, const char *hex)
{
    uint8_t out[WL_SHA256_LEN];

    wl_hmac_sha256(key, key_len, data, strlen(data), out);
    assert_hex(out, trunc, hex);
}

static void test_hmac_rfc4231(void **state)
{
    uint8_t key[131];
    uint8_t data[50];

    (void)state;
    memset(key, 0x0B, 20);
    hmac_case(key, 20, "Hi There", 32,
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_case((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 32,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    memset(key, 0xAA, 20);
    memset(data, 0xDD, sizeof(data));
    {
        uint8_t out[WL_SHA256_LEN];

        wl_hmac_sha256(key, 20, data, sizeof(data), out);
        assert_hex(out, 32, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
        for (unsigned i = 0; i < 25; i++)
            key[i] = (uint8_t)(i + 1U);
        memset(data, 0xCD, sizeof(data));
        wl_hmac_sha256(key, 25, data, sizeof(data), out);
        assert_hex(out, 32, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    }

    memset(key, 0x0C, 20);
    hmac_case(key, 20, "Test With Truncation", 16, "a3b6167473100ee06e0c796c2955552b");

    /* Keys longer than a block are hashed first. */
    memset(key, 0xAA, sizeof(key));
    hmac_case(key, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 32,
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    hmac_case(key, 131,
              "This is a test using a larger than block-size key and a larger than "
              "block-size data. The key needs to be hashed before being used by the "
              "HMAC algorithm.",
              32, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

/* ---- HKDF-SHA256: RFC 5869 appendix A.1 to A.3 ---- */

static void test_hkdf_rfc5869(void **state)
{
    uint8_t ikm[80];
    uint8_t salt[80];
    uint8_t info[80];
    uint8_t okm[82];

    (void)state;
    memset(ikm, 0x0B, 22);
    for (unsigned i = 0; i < 13; i++)
        salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < 10; i++)
        info[i] = (uint8_t)(0xF0U + i);
    assert_int_equal(wl_hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42), WL_OK);
    assert_hex(okm, 42,
               "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
               "34007208d5b887185865");

    for (unsigned i = 0; i < 80; i++) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60U + i);
        info[i] = (uint8_t)(0xB0U + i);
    }
    assert_int_equal(wl_hkdf_sha256(salt, 80, ikm, 80, info, 80, okm, 82), WL_OK);
    assert_hex(okm, 82,
               "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
               "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
               "cc30c58179ec3e87c14c01d5c1f3434f1d87");

    memset(ikm, 0x0B, 22);
    assert_int_equal(wl_hkdf_sha256(NULL, 0, ikm, 22, NULL, 0, okm, 42), WL_OK);
    assert_hex(okm, 42,
               "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
               "9d201395faa4b61a96c8");

    /* RFC 5869 caps the output at 255 blocks. */
    assert_int_equal(wl_hkdf_sha256(NULL, 0, ikm, 22, NULL, 0, okm, 255U * 32U + 1U),
                     WL_ERR_INVAL);
}

/* ---- PBKDF2-HMAC-SHA256: RFC 7914 section 11 ---- */

static void test_pbkdf2_rfc7914(void **state)
{
    uint8_t dk[64];

    (void)state;
    /* 64 bytes is two blocks, so the block counter is exercised too. */
    assert_int_equal(wl_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt",
                                      4, 1, dk, sizeof(dk)),
                     WL_OK);
    assert_hex(dk, 64,
               "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
               "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    assert_int_equal(wl_pbkdf2_sha256((const uint8_t *)"Password", 8,
                                      (const uint8_t *)"NaCl", 4, 80000, dk, sizeof(dk)),
                     WL_OK);
    assert_hex(dk, 64,
               "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
               "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");

    assert_int_equal(wl_pbkdf2_sha256((const uint8_t *)"p", 1, (const uint8_t *)"s", 1, 0,
                                      dk, sizeof(dk)),
                     WL_ERR_INVAL);
    assert_int_equal(wl_pbkdf2_sha256((const uint8_t *)"p", 1, (const uint8_t *)"s", 1, 1,
                                      dk, 0),
                     WL_ERR_INVAL);
}

/* The protocol's own derivation, against Python's hashlib.pbkdf2_hmac with
 * the same salt and count, and timed: the count is meant to cost about a
 * second on the target, and this shows what it costs here. */
static void test_psk_from_passphrase(void **state)
{
    static const char pass[] = "correct horse battery staple";
    uint8_t psk[WL_NOISE_KEYLEN];
    struct timespec t0;
    struct timespec t1;

    (void)state;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    wl_noise_psk_from_passphrase(pass, sizeof(pass) - 1U, psk);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    assert_hex(psk, 32, "f1947c0c07d18e993a2d17f9a08c20a70ac11933e4520c45ffef16fd83b8f88e");
    printf("psk from passphrase: %u iterations in %.0f ms\n",
           (unsigned)WL_NOISE_PBKDF2_ITERATIONS,
           ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
               ((double)(t1.tv_nsec - t0.tv_nsec) / 1e6));
}

/* ---- Noise: the published vector ---- */

/*
 * The one Noise_NNpsk0_25519_ChaChaPoly_SHA256 vector in cacophony's set:
 * vectors/cacophony.txt of github.com/haskell-cryptography/cacophony at
 * commit 18b7348c54fd61fcd0c220298883de0d09c8364d. (snow's tests/vectors at
 * eb273c833d519f7519f33bf45f2aad7cf06f2799 has none for this name.)
 * Messages alternate, initiator first: two handshake messages, then four
 * transport messages.
 */
static const char *const vec_prologue = "4a6f686e2047616c74";
static const char *const vec_psk =
    "54686973206973206d7920417573747269616e20706572737065637469766521";
static const char *const vec_init_e =
    "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a";
static const char *const vec_resp_e =
    "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
static const char *const vec_hash =
    "f4d03dc34495c95729ea6de9e1b59004b59733102488b3e24bc441e0be208eaf";
static const struct {
    const char *payload;
    const char *ciphertext;
} vec_msgs[] = {
    {"4c756477696720766f6e204d69736573",
     "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944"
     "79b962b8aff8485742ac32f905ba45369e2465fb59e138a93d67a0d1266b6a54"},
    {"4d757272617920526f746862617264",
     "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843"
     "d6062704d5a9c422a8e834423f8c1feada7e8d0d910a1a2cd030fb584221e3"},
    {"462e20412e20486179656b", "e632c3763d7669067383433197a3baddf146e9e70ad4b4e9e59e0f"},
    {"4361726c204d656e676572", "64c6bee32ea91c8474bb4c21d7a700109ad45af77b29764ba5eb1e"},
    {"4a65616e2d426170746973746520536179",
     "e2fa0bed0603b62d3ccac2ecabbf3fe33f3e86514909b323361626266cb2471cc8"},
    {"457567656e2042f6686d20766f6e2042617765726b",
     "0c01dc9cec1fe4ddd692e8dd32188aa351088dc91183639a53b57aa4692b5ebdef8b8ca111"},
};

static void test_cacophony_vector(void **state)
{
    uint8_t prologue[16];
    uint8_t psk[32];
    uint8_t ie[32];
    uint8_t re[32];
    uint8_t h[32];
    size_t plen = unhex(vec_prologue, prologue, sizeof(prologue));
    struct wl_noise *init;
    struct wl_noise *resp;

    (void)state;
    (void)unhex(vec_psk, psk, sizeof(psk));
    (void)unhex(vec_init_e, ie, sizeof(ie));
    (void)unhex(vec_resp_e, re, sizeof(re));
    init = wl_noise_new(1, psk, prologue, plen, ie);
    resp = wl_noise_new(0, psk, prologue, plen, re);
    assert_non_null(init);
    assert_non_null(resp);

    for (size_t i = 0; i < sizeof(vec_msgs) / sizeof(vec_msgs[0]); i++) {
        struct wl_noise *tx = (i % 2U) == 0 ? init : resp;
        struct wl_noise *rx = (i % 2U) == 0 ? resp : init;
        uint8_t payload[64];
        uint8_t want[128];
        size_t pn = unhex(vec_msgs[i].payload, payload, sizeof(payload));
        size_t wn = unhex(vec_msgs[i].ciphertext, want, sizeof(want));
        size_t n = 0;
        size_t got = 0;

        assert_true(wl_noise_wants_write(tx));
        assert_int_equal(wl_noise_write_message(tx, payload, pn, buf_a, BUF, &n), WL_OK);
        assert_int_equal(n, wn);
        assert_memory_equal(buf_a, want, wn);
        assert_int_equal(wl_noise_read_message(rx, buf_a, n, buf_b, BUF, &got), WL_OK);
        assert_int_equal(got, pn);
        assert_memory_equal(buf_b, payload, pn);
        if (i == 1) {
            assert_true(wl_noise_is_transport(init));
            assert_true(wl_noise_is_transport(resp));
            wl_noise_handshake_hash(init, h);
            assert_hex(h, 32, vec_hash);
            wl_noise_handshake_hash(resp, h);
            assert_hex(h, 32, vec_hash);
        } else if (i == 0) {
            assert_false(wl_noise_is_transport(init));
            assert_false(wl_noise_is_transport(resp));
        }
    }
    wl_noise_free(init);
    wl_noise_free(resp);
}

/* ---- Noise: behaviour ---- */

static const uint8_t test_psk[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
                                     31, 32};
static const uint8_t fixed_ie[32] = {0x11};
static const uint8_t fixed_re[32] = {0x22};

#define PROLOGUE ((const uint8_t *)WL_NOISE_PROLOGUE)
#define PROLOGUE_LEN (sizeof(WL_NOISE_PROLOGUE) - 1U)

/* Runs the handshake with empty payloads, as the protocol does. */
static void handshake(struct wl_noise *init, struct wl_noise *resp)
{
    size_t n = 0;
    size_t got = 1;

    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(n, 48);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_int_equal(got, 0);
    assert_int_equal(wl_noise_write_message(resp, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(n, 48);
    assert_int_equal(wl_noise_read_message(init, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_true(wl_noise_is_transport(init));
    assert_true(wl_noise_is_transport(resp));
}

static void pair(struct wl_noise **init, struct wl_noise **resp)
{
    *init = wl_noise_new(1, test_psk, PROLOGUE, PROLOGUE_LEN, fixed_ie);
    *resp = wl_noise_new(0, test_psk, PROLOGUE, PROLOGUE_LEN, fixed_re);
    assert_non_null(*init);
    assert_non_null(*resp);
}

static void send_ok(struct wl_noise *tx, struct wl_noise *rx, const char *text)
{
    size_t len = strlen(text);
    size_t n = 0;
    size_t got = 0;

    assert_int_equal(wl_noise_write_message(tx, (const uint8_t *)text, len, buf_a, BUF, &n),
                     WL_OK);
    assert_int_equal(n, len + WL_NOISE_TAGLEN);
    assert_int_equal(wl_noise_read_message(rx, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_int_equal(got, len);
    assert_memory_equal(buf_b, text, len);
}

static void test_random_handshake_and_transport(void **state)
{
    struct wl_noise *init = wl_noise_new(1, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);
    struct wl_noise *resp = wl_noise_new(0, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);
    uint8_t hi[32];
    uint8_t hr[32];

    (void)state;
    assert_non_null(init);
    assert_non_null(resp);
    handshake(init, resp);
    wl_noise_handshake_hash(init, hi);
    wl_noise_handshake_hash(resp, hr);
    assert_memory_equal(hi, hr, 32);

    for (int i = 0; i < 5; i++) {
        send_ok(init, resp, "client to agent");
        send_ok(resp, init, "agent to client");
    }
    send_ok(init, resp, "");  /* a keepalive */
    send_ok(init, resp, "two in a row");
    wl_noise_free(init);
    wl_noise_free(resp);
    wl_noise_free(NULL);
}

/* Two sessions with random ephemerals share nothing: the same plaintext
 * encrypts differently, which is the forward secrecy the protocol claims. */
static void test_sessions_have_fresh_keys(void **state)
{
    uint8_t first[64];
    size_t n = 0;

    (void)state;
    for (int round = 0; round < 2; round++) {
        struct wl_noise *init = wl_noise_new(1, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);
        struct wl_noise *resp = wl_noise_new(0, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);

        assert_non_null(init);
        assert_non_null(resp);
        handshake(init, resp);
        assert_int_equal(wl_noise_write_message(init, (const uint8_t *)"same", 4, buf_a, BUF,
                                                &n),
                         WL_OK);
        if (round == 0)
            memcpy(first, buf_a, n);
        else
            assert_memory_not_equal(first, buf_a, n);
        wl_noise_free(init);
        wl_noise_free(resp);
    }
}

static void test_wrong_psk_fails_first_read(void **state)
{
    uint8_t other[32];
    struct wl_noise *init;
    struct wl_noise *resp;
    size_t n = 0;
    size_t got = 0;

    (void)state;
    memcpy(other, test_psk, sizeof(other));
    other[31] ^= 1U;
    init = wl_noise_new(1, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);
    resp = wl_noise_new(0, other, PROLOGUE, PROLOGUE_LEN, NULL);
    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_ERR_CORRUPT);

    /* The responder is dead from then on, so it cannot be coaxed into
     * answering. */
    assert_false(wl_noise_wants_write(resp));
    assert_false(wl_noise_is_transport(resp));
    assert_int_equal(wl_noise_write_message(resp, NULL, 0, buf_a, BUF, &n), WL_ERR_INVAL);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_ERR_INVAL);
    wl_noise_free(init);
    wl_noise_free(resp);
}

static void test_wrong_prologue_fails(void **state)
{
    struct wl_noise *init = wl_noise_new(1, test_psk, PROLOGUE, PROLOGUE_LEN, NULL);
    struct wl_noise *resp =
        wl_noise_new(0, test_psk, (const uint8_t *)"devlink 2", PROLOGUE_LEN, NULL);
    size_t n = 0;
    size_t got = 0;

    (void)state;
    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_ERR_CORRUPT);
    wl_noise_free(init);
    wl_noise_free(resp);
}

/* Every bit of both handshake messages and of a transport message in each
 * direction is covered by authentication. A fresh pair per flip, since a
 * failure kills the receiving side. Fixed ephemerals keep this fast. */
static void test_any_flipped_bit_fails(void **state)
{
    (void)state;
    /* stage 0: message 1; 1: message 2; 2: client to agent; 3: agent to client */
    for (int stage = 0; stage < 4; stage++) {
        for (size_t byte = 0; byte < 48; byte++) {
            struct wl_noise *init;
            struct wl_noise *resp;
            struct wl_noise *tx;
            struct wl_noise *rx;
            uint8_t msg[64];
            size_t n = 0;
            size_t got = 0;

            pair(&init, &resp);
            if (stage >= 2)
                handshake(init, resp);
            else if (stage == 1) {
                assert_int_equal(wl_noise_write_message(init, NULL, 0, msg, sizeof(msg), &n),
                                 WL_OK);
                assert_int_equal(wl_noise_read_message(resp, msg, n, buf_b, BUF, &got),
                                 WL_OK);
            }
            tx = stage == 0 || stage == 2 ? init : resp;
            rx = tx == init ? resp : init;
            /* 32 payload bytes make a transport message 48 long, as the
             * handshake messages are. */
            assert_int_equal(wl_noise_write_message(tx, (const uint8_t *)"0123456789abcdef"
                                                                         "0123456789abcdef",
                                                    stage >= 2 ? 32U : 0U, msg, sizeof(msg),
                                                    &n),
                             WL_OK);
            assert_int_equal(n, 48);
            msg[byte] ^= (uint8_t)(1U << (byte % 8U));
            assert_int_equal(wl_noise_read_message(rx, msg, n, buf_b, BUF, &got),
                             WL_ERR_CORRUPT);
            wl_noise_free(init);
            wl_noise_free(resp);
        }
    }
}

/* The nonce advances per message, so a replayed or reordered message does
 * not authenticate. */
static void test_replay_fails(void **state)
{
    struct wl_noise *init;
    struct wl_noise *resp;
    uint8_t first[64];
    size_t n1 = 0;
    size_t n = 0;
    size_t got = 0;

    (void)state;
    pair(&init, &resp);
    handshake(init, resp);
    assert_int_equal(wl_noise_write_message(init, (const uint8_t *)"one", 3, first,
                                            sizeof(first), &n1),
                     WL_OK);
    assert_int_equal(wl_noise_write_message(init, (const uint8_t *)"two", 3, buf_a, BUF, &n),
                     WL_OK);
    /* Second message first: wrong nonce. */
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_ERR_CORRUPT);
    wl_noise_free(init);
    wl_noise_free(resp);

    pair(&init, &resp);
    handshake(init, resp);
    assert_int_equal(wl_noise_write_message(init, (const uint8_t *)"one", 3, first,
                                            sizeof(first), &n1),
                     WL_OK);
    assert_int_equal(wl_noise_read_message(resp, first, n1, buf_b, BUF, &got), WL_OK);
    assert_int_equal(wl_noise_read_message(resp, first, n1, buf_b, BUF, &got), WL_ERR_CORRUPT);
    wl_noise_free(init);
    wl_noise_free(resp);
}

static void test_size_limits(void **state)
{
    struct wl_noise *init;
    struct wl_noise *resp;
    size_t n = 0;
    size_t got = 0;
    const size_t max_hs = WL_NOISE_MAX_MSG - 48U;
    const size_t max_tr = WL_NOISE_MAX_MSG - WL_NOISE_TAGLEN;

    (void)state;
    pair(&init, &resp);
    memset(buf_b, 'x', BUF);

    /* Handshake: too big is refused without harming the state, too little
     * room is NOSPACE, and the largest allowed payload goes through. */
    assert_int_equal(wl_noise_write_message(init, buf_b, max_hs + 1U, buf_a, BUF, &n),
                     WL_ERR_INVAL);
    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, 47, &n), WL_ERR_NOSPACE);
    assert_int_equal(wl_noise_write_message(init, buf_b, max_hs, buf_a, BUF, &n), WL_OK);
    assert_int_equal(n, WL_NOISE_MAX_MSG);
    assert_int_equal(wl_noise_read_message(resp, buf_a, WL_NOISE_MAX_MSG + 1U, buf_b, BUF,
                                           &got),
                     WL_ERR_INVAL);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, max_hs - 1U, &got),
                     WL_ERR_NOSPACE);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_int_equal(got, max_hs);
    assert_int_equal(wl_noise_write_message(resp, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(wl_noise_read_message(init, buf_a, n, buf_b, BUF, &got), WL_OK);

    /* Transport: 65519 bytes of payload is the most that fits, which is the
     * protocol's max_payload on TCP. */
    memset(buf_b, 'y', BUF);
    assert_int_equal(wl_noise_write_message(init, buf_b, max_tr + 1U, buf_a, BUF, &n),
                     WL_ERR_INVAL);
    assert_int_equal(wl_noise_write_message(init, buf_b, 10, buf_a, 25, &n), WL_ERR_NOSPACE);
    assert_int_equal(wl_noise_write_message(init, buf_b, max_tr, buf_a, BUF, &n), WL_OK);
    assert_int_equal(n, WL_NOISE_MAX_MSG);
    assert_int_equal(wl_noise_read_message(resp, buf_a, WL_NOISE_MAX_MSG + 1U, buf_b, BUF,
                                           &got),
                     WL_ERR_INVAL);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, max_tr - 1U, &got),
                     WL_ERR_NOSPACE);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_int_equal(got, max_tr);

    /* Shorter than a tag cannot be a message at all. */
    assert_int_equal(wl_noise_read_message(resp, buf_a, WL_NOISE_TAGLEN - 1U, buf_b, BUF,
                                           &got),
                     WL_ERR_CORRUPT);
    assert_false(wl_noise_is_transport(resp));
    wl_noise_free(init);
    wl_noise_free(resp);

    /* Nor is a handshake message shorter than a key and a tag. */
    pair(&init, &resp);
    assert_int_equal(wl_noise_read_message(resp, buf_a, 47, buf_b, BUF, &got), WL_ERR_CORRUPT);
    wl_noise_free(init);
    wl_noise_free(resp);
}

static void test_turn_order(void **state)
{
    struct wl_noise *init;
    struct wl_noise *resp;
    size_t n = 0;
    size_t got = 0;

    (void)state;
    pair(&init, &resp);
    assert_true(wl_noise_wants_write(init));
    assert_false(wl_noise_wants_write(resp));
    assert_int_equal(wl_noise_write_message(resp, NULL, 0, buf_a, BUF, &n), WL_ERR_INVAL);
    assert_int_equal(wl_noise_read_message(init, buf_a, 48, buf_b, BUF, &got), WL_ERR_INVAL);

    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_false(wl_noise_wants_write(init));
    assert_int_equal(wl_noise_write_message(init, NULL, 0, buf_a, BUF, &n), WL_ERR_INVAL);
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_OK);
    assert_true(wl_noise_wants_write(resp));
    assert_int_equal(wl_noise_read_message(resp, buf_a, n, buf_b, BUF, &got), WL_ERR_INVAL);

    assert_int_equal(wl_noise_write_message(resp, NULL, 0, buf_a, BUF, &n), WL_OK);
    assert_int_equal(wl_noise_read_message(init, buf_a, n, buf_b, BUF, &got), WL_OK);
    /* In transport either side may write at any time. */
    assert_true(wl_noise_wants_write(init));
    assert_true(wl_noise_wants_write(resp));
    wl_noise_free(init);
    wl_noise_free(resp);
}

static void test_new_rejects_bad_arguments(void **state)
{
    (void)state;
    assert_null(wl_noise_new(1, NULL, PROLOGUE, PROLOGUE_LEN, NULL));
    assert_null(wl_noise_new(1, test_psk, NULL, 3, NULL));
}

static void test_random_bytes(void **state)
{
    uint8_t a[32];
    uint8_t b[32];

    (void)state;
    assert_int_equal(wl_random_bytes(a, sizeof(a)), WL_OK);
    assert_int_equal(wl_random_bytes(b, sizeof(b)), WL_OK);
    assert_memory_not_equal(a, b, sizeof(a));
    assert_int_equal(wl_random_bytes(a, 0), WL_OK);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_hmac_rfc4231),
        cmocka_unit_test(test_hkdf_rfc5869),
        cmocka_unit_test(test_pbkdf2_rfc7914),
        cmocka_unit_test(test_psk_from_passphrase),
        cmocka_unit_test(test_cacophony_vector),
        cmocka_unit_test(test_random_handshake_and_transport),
        cmocka_unit_test(test_sessions_have_fresh_keys),
        cmocka_unit_test(test_wrong_psk_fails_first_read),
        cmocka_unit_test(test_wrong_prologue_fails),
        cmocka_unit_test(test_any_flipped_bit_fails),
        cmocka_unit_test(test_replay_fails),
        cmocka_unit_test(test_size_limits),
        cmocka_unit_test(test_turn_order),
        cmocka_unit_test(test_new_rejects_bad_arguments),
        cmocka_unit_test(test_random_bytes),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
