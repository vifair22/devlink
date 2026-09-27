#include <string.h>

#include <monocypher.h>

#include "devlink/kdf.h"

/* Both HKDF and PBKDF2 number their output blocks, HKDF in a single byte. */
#define MAX_OUT ((size_t)255U * WL_SHA256_LEN)

void wl_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}

void wl_hmac_sha256_init(struct wl_hmac_sha256 *h, const uint8_t *key, size_t key_len)
{
    uint8_t block[WL_SHA256_BLOCK];

    /* A key longer than a block is replaced by its hash (RFC 2104 section 2);
     * a shorter one is zero-padded, which the memset does. */
    memset(block, 0, sizeof(block));
    if (key_len > WL_SHA256_BLOCK) {
        wl_sha256_init(&h->inner);
        wl_sha256_update(&h->inner, key, key_len);
        wl_sha256_final(&h->inner, block);
    } else if (key_len > 0) {
        memcpy(block, key, key_len);
    }

    for (size_t i = 0; i < sizeof(block); i++)
        block[i] ^= 0x36U;
    wl_sha256_init(&h->inner);
    wl_sha256_update(&h->inner, block, sizeof(block));

    /* 0x36 ^ 0x5C turns the inner pad into the outer one in place. */
    for (size_t i = 0; i < sizeof(block); i++)
        block[i] ^= 0x36U ^ 0x5CU;
    wl_sha256_init(&h->outer);
    wl_sha256_update(&h->outer, block, sizeof(block));
    wl_wipe(block, sizeof(block));
}

void wl_hmac_sha256_update(struct wl_hmac_sha256 *h, const void *data, size_t n)
{
    wl_sha256_update(&h->inner, data, n);
}

void wl_hmac_sha256_final(struct wl_hmac_sha256 *h, uint8_t out[WL_SHA256_LEN])
{
    uint8_t inner[WL_SHA256_LEN];

    wl_sha256_final(&h->inner, inner);
    wl_sha256_update(&h->outer, inner, sizeof(inner));
    wl_sha256_final(&h->outer, out);
    wl_wipe(inner, sizeof(inner));
    wl_wipe(h, sizeof(*h));
}

void wl_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t n,
                    uint8_t out[WL_SHA256_LEN])
{
    struct wl_hmac_sha256 h;

    wl_hmac_sha256_init(&h, key, key_len);
    wl_hmac_sha256_update(&h, data, n);
    wl_hmac_sha256_final(&h, out);
}

wl_status wl_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                         size_t ikm_len, const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len)
{
    uint8_t prk[WL_SHA256_LEN];
    uint8_t t[WL_SHA256_LEN];
    size_t done = 0;
    uint8_t counter = 1;

    if (out_len > MAX_OUT)
        return wl_fail(WL_ERR_INVAL, "hkdf: %zu bytes requested, at most %zu", out_len,
                       MAX_OUT);

    /* An absent salt is a block of zeros, which HMAC's own padding makes
     * the same as an empty key. */
    wl_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    while (done < out_len) {
        struct wl_hmac_sha256 h;
        size_t k = out_len - done < WL_SHA256_LEN ? out_len - done : WL_SHA256_LEN;

        wl_hmac_sha256_init(&h, prk, sizeof(prk));
        if (counter > 1)
            wl_hmac_sha256_update(&h, t, sizeof(t));
        if (info_len > 0)
            wl_hmac_sha256_update(&h, info, info_len);
        wl_hmac_sha256_update(&h, &counter, 1);
        wl_hmac_sha256_final(&h, t);
        memcpy(out + done, t, k);
        done += k;
        counter++;
    }
    wl_wipe(prk, sizeof(prk));
    wl_wipe(t, sizeof(t));
    return WL_OK;
}

wl_status wl_pbkdf2_sha256(const uint8_t *pass, size_t pass_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t *out,
                           size_t out_len)
{
    struct wl_hmac_sha256 keyed;
    uint8_t u[WL_SHA256_LEN];
    uint8_t acc[WL_SHA256_LEN];
    size_t done = 0;
    uint32_t block = 1;

    if (iterations == 0)
        return wl_fail(WL_ERR_INVAL, "pbkdf2: iteration count is zero");
    if (out_len == 0 || out_len > MAX_OUT)
        return wl_fail(WL_ERR_INVAL, "pbkdf2: %zu bytes requested", out_len);

    /* The password is the HMAC key throughout, so its padded blocks are
     * hashed once here and every iteration starts from a copy. */
    wl_hmac_sha256_init(&keyed, pass, pass_len);

    while (done < out_len) {
        struct wl_hmac_sha256 h = keyed;
        uint8_t be[4];
        size_t k = out_len - done < WL_SHA256_LEN ? out_len - done : WL_SHA256_LEN;

        be[0] = (uint8_t)(block >> 24);
        be[1] = (uint8_t)(block >> 16);
        be[2] = (uint8_t)(block >> 8);
        be[3] = (uint8_t)block;
        wl_hmac_sha256_update(&h, salt, salt_len);
        wl_hmac_sha256_update(&h, be, sizeof(be));
        wl_hmac_sha256_final(&h, u);
        memcpy(acc, u, sizeof(acc));
        for (uint32_t i = 1; i < iterations; i++) {
            h = keyed;
            wl_hmac_sha256_update(&h, u, sizeof(u));
            wl_hmac_sha256_final(&h, u);
            for (size_t j = 0; j < sizeof(acc); j++)
                acc[j] ^= u[j];
        }
        memcpy(out + done, acc, k);
        done += k;
        block++;
    }
    wl_wipe(&keyed, sizeof(keyed));
    wl_wipe(u, sizeof(u));
    wl_wipe(acc, sizeof(acc));
    return WL_OK;
}
