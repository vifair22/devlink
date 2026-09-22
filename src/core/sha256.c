#include <stdio.h>
#include <string.h>

#include "w98link/sha256.h"

/* FIPS 180-4. Straightforward implementation: the link moves 11 KB/s, so
 * hashing is never the bottleneck. */

static const uint32_t K[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
    0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
    0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
    0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32U - n));
}

static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (size_t i = 0; i < 16; i++) {
        const uint8_t *src = block + (i * 4);

        w[i] = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
               ((uint32_t)src[2] << 8) | (uint32_t)src[3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void wl_sha256_init(struct wl_sha256 *s)
{
    static const uint32_t iv[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                   0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};

    memcpy(s->state, iv, sizeof(iv));
    s->bits = 0;
    s->used = 0;
}

void wl_sha256_update(struct wl_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;

    s->bits += (uint64_t)n * 8U;
    while (n > 0) {
        size_t room = 64U - s->used;
        size_t k = n < room ? n : room;

        memcpy(s->block + s->used, p, k);
        s->used += k;
        p += k;
        n -= k;
        if (s->used == 64U) {
            compress(s->state, s->block);
            s->used = 0;
        }
    }
}

void wl_sha256_final(struct wl_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bits;
    uint8_t pad = 0x80U;
    uint8_t len[8];

    wl_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56U)
        wl_sha256_update(s, &pad, 1);
    for (size_t i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56U - (i * 8U)));
    /* The length is appended outside the bit counter, so it is written
     * straight into the block rather than through update(). */
    memcpy(s->block + 56, len, 8);
    compress(s->state, s->block);
    for (size_t i = 0; i < 8; i++) {
        uint8_t *o = out + (i * 4);

        o[0] = (uint8_t)(s->state[i] >> 24);
        o[1] = (uint8_t)(s->state[i] >> 16);
        o[2] = (uint8_t)(s->state[i] >> 8);
        o[3] = (uint8_t)s->state[i];
    }
}

int wl_sha256_file(const char *path, uint8_t out[32])
{
    struct wl_sha256 s;
    FILE *f = fopen(path, "rb");
    uint8_t buf[8192];
    size_t n;

    if (f == NULL)
        return 0;
    wl_sha256_init(&s);
    /* Stops at the first short read, so fread is never called again once
     * the stream has hit end of file. */
    do {
        n = fread(buf, 1, sizeof(buf), f);
        if (n > 0)
            wl_sha256_update(&s, buf, n);
    } while (n == sizeof(buf));
    if (ferror(f)) {
        (void)fclose(f);
        return 0;
    }
    (void)fclose(f);
    wl_sha256_final(&s, out);
    return 1;
}
