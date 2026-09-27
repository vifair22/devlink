#include <stdlib.h>
#include <string.h>

#include <monocypher.h>

#include "devlink/bytes.h"
#include "devlink/kdf.h"
#include "devlink/noise.h"
#include "devlink/random.h"
#include "devlink/sha256.h"

/*
 * Noise_NNpsk0_25519_ChaChaPoly_SHA256, written to follow the Noise
 * specification (revision 34) section by section so it can be checked
 * against it line for line: CipherState (5.1), SymmetricState (5.2),
 * HandshakeState (5.3) with the psk rules of section 9. Only this one
 * pattern exists, so the token loop is unrolled into the two messages it
 * produces:
 *
 *   -> psk, e
 *   <- e, ee
 *
 * psk0 keys the handshake from its very first token, so every payload,
 * even the first, is encrypted; the "no key yet" branch of EncryptAndHash
 * never runs here and is not written.
 */

#define PROTOCOL_NAME "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
#define HASHLEN ((size_t)32U)
#define DHLEN 32U
#define PSK_SALT "devlink psk v1"

/* 2^64 - 1 is reserved by the spec (section 5.1): a CipherState that
 * reaches it refuses to encrypt rather than reuse a nonce. */
#define NONCE_MAX UINT64_MAX

struct cipher {
    uint8_t k[WL_NOISE_KEYLEN];
    uint64_t n;
};

struct wl_noise {
    int initiator;
    int done;   /* handshake messages processed, 0 to 2 */
    int dead;   /* a message failed to authenticate */
    uint8_t h[HASHLEN];
    uint8_t ck[HASHLEN];
    struct cipher hs; /* the handshake's CipherState */
    uint8_t psk[WL_NOISE_KEYLEN];
    uint8_t e_priv[DHLEN];
    uint8_t e_pub[DHLEN];
    uint8_t re[DHLEN];
    struct cipher tx;
    struct cipher rx;
};

void wl_noise_psk_from_passphrase(const char *pass, size_t len,
                                  uint8_t psk[WL_NOISE_KEYLEN])
{
    /* Cannot fail: the iteration count and length are constants in range. */
    (void)wl_pbkdf2_sha256((const uint8_t *)pass, len, (const uint8_t *)PSK_SALT,
                           sizeof(PSK_SALT) - 1U, WL_NOISE_PBKDF2_ITERATIONS, psk,
                           WL_NOISE_KEYLEN);
}

/* ---- ChaChaPoly: the RFC 8439 AEAD, as Noise section 12.3 specifies ---- */

/* Monocypher's own AEAD calls either take a 24-byte XChaCha nonce or
 * ratchet the key after every message, and neither is what Noise means by
 * ChaChaPoly. So the RFC 8439 construction (section 2.8) is assembled here
 * from its two primitives. Noise's nonce is 32 zero bits then the 64-bit
 * counter, little-endian. */
static void aead_nonce(uint8_t nonce[12], uint64_t n)
{
    memset(nonce, 0, 4);
    wl_put_u64le(nonce + 4, n);
}

static void aead_tag(uint8_t tag[WL_NOISE_TAGLEN], const uint8_t key[WL_NOISE_KEYLEN],
                     const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t ct_len)
{
    static const uint8_t zero[16];
    crypto_poly1305_ctx poly;
    uint8_t otk[64];
    uint8_t lens[16];

    /* The one-time Poly1305 key is the first half of keystream block 0;
     * the message itself is encrypted from block 1. */
    crypto_chacha20_ietf(otk, NULL, sizeof(otk), key, nonce, 0);
    wl_put_u64le(lens, (uint64_t)ad_len);
    wl_put_u64le(lens + 8, (uint64_t)ct_len);
    crypto_poly1305_init(&poly, otk);
    crypto_poly1305_update(&poly, ad, ad_len);
    crypto_poly1305_update(&poly, zero, (16U - (ad_len % 16U)) % 16U);
    crypto_poly1305_update(&poly, ct, ct_len);
    crypto_poly1305_update(&poly, zero, (16U - (ct_len % 16U)) % 16U);
    crypto_poly1305_update(&poly, lens, sizeof(lens));
    crypto_poly1305_final(&poly, tag);
    crypto_wipe(otk, sizeof(otk));
}

/* out receives len + 16 bytes; it may be the same buffer as pt. */
static void aead_encrypt(const struct cipher *c, const uint8_t *ad, size_t ad_len,
                         const uint8_t *pt, size_t len, uint8_t *out)
{
    uint8_t nonce[12];

    aead_nonce(nonce, c->n);
    crypto_chacha20_ietf(out, pt, len, c->k, nonce, 1);
    aead_tag(out + len, c->k, nonce, ad, ad_len, out, len);
}

/* ct is len bytes of ciphertext followed by its tag. The tag is checked
 * before anything is decrypted, so a forgery never reaches out. */
static wl_status aead_decrypt(const struct cipher *c, const uint8_t *ad, size_t ad_len,
                              const uint8_t *ct, size_t len, uint8_t *out)
{
    uint8_t nonce[12];
    uint8_t tag[WL_NOISE_TAGLEN];
    int bad;

    aead_nonce(nonce, c->n);
    aead_tag(tag, c->k, nonce, ad, ad_len, ct, len);
    bad = crypto_verify16(tag, ct + len);
    if (bad)
        return wl_fail(WL_ERR_CORRUPT, "noise: message failed authentication");
    crypto_chacha20_ietf(out, ct, len, c->k, nonce, 1);
    return WL_OK;
}

/* ---- SymmetricState (section 5.2) ---- */

/* HKDF as Noise defines it (section 4.3): RFC 5869 with the chaining key
 * as salt, no info, and 2 or 3 hash-length outputs. */
static void noise_hkdf(const uint8_t ck[HASHLEN], const uint8_t *ikm, size_t ikm_len,
                       uint8_t *out, size_t outputs)
{
    (void)wl_hkdf_sha256(ck, HASHLEN, ikm, ikm_len, NULL, 0, out, outputs * HASHLEN);
}

static void mix_hash(struct wl_noise *n, const uint8_t *data, size_t len)
{
    struct wl_sha256 s;

    wl_sha256_init(&s);
    wl_sha256_update(&s, n->h, HASHLEN);
    wl_sha256_update(&s, data, len);
    wl_sha256_final(&s, n->h);
}

/* InitializeKey resets the nonce: each new handshake key starts at 0. */
static void mix_key(struct wl_noise *n, const uint8_t *ikm, size_t len)
{
    uint8_t out[2 * HASHLEN];

    noise_hkdf(n->ck, ikm, len, out, 2);
    memcpy(n->ck, out, HASHLEN);
    memcpy(n->hs.k, out + HASHLEN, WL_NOISE_KEYLEN);
    n->hs.n = 0;
    crypto_wipe(out, sizeof(out));
}

static void mix_key_and_hash(struct wl_noise *n, const uint8_t *ikm, size_t len)
{
    uint8_t out[3 * HASHLEN];

    noise_hkdf(n->ck, ikm, len, out, 3);
    memcpy(n->ck, out, HASHLEN);
    mix_hash(n, out + HASHLEN, HASHLEN);
    memcpy(n->hs.k, out + (2U * HASHLEN), WL_NOISE_KEYLEN);
    n->hs.n = 0;
    crypto_wipe(out, sizeof(out));
}

/* The handshake hash is the associated data, which is what binds every
 * handshake payload to everything sent before it. */
static void encrypt_and_hash(struct wl_noise *n, const uint8_t *pt, size_t len,
                             uint8_t *out)
{
    aead_encrypt(&n->hs, n->h, HASHLEN, pt, len, out);
    n->hs.n++;
    mix_hash(n, out, len + WL_NOISE_TAGLEN);
}

static wl_status decrypt_and_hash(struct wl_noise *n, const uint8_t *ct, size_t len,
                                  uint8_t *out)
{
    wl_status st = aead_decrypt(&n->hs, n->h, HASHLEN, ct, len, out);

    if (st != WL_OK)
        return st;
    n->hs.n++;
    mix_hash(n, ct, len + WL_NOISE_TAGLEN);
    return WL_OK;
}

/* Section 5.2's Split. The first key protects initiator to responder. The
 * handshake's secrets are of no further use, so they go now rather than at
 * free; only h stays, as the session's identifier. */
static void split(struct wl_noise *n)
{
    uint8_t out[2 * HASHLEN];

    noise_hkdf(n->ck, NULL, 0, out, 2);
    memcpy(n->initiator ? n->tx.k : n->rx.k, out, WL_NOISE_KEYLEN);
    memcpy(n->initiator ? n->rx.k : n->tx.k, out + HASHLEN, WL_NOISE_KEYLEN);
    n->tx.n = 0;
    n->rx.n = 0;
    crypto_wipe(out, sizeof(out));
    crypto_wipe(n->ck, sizeof(n->ck));
    crypto_wipe(&n->hs, sizeof(n->hs));
    crypto_wipe(n->psk, sizeof(n->psk));
    crypto_wipe(n->e_priv, sizeof(n->e_priv));
}

/* ---- HandshakeState (section 5.3) ---- */

struct wl_noise *wl_noise_new(int initiator, const uint8_t psk[WL_NOISE_KEYLEN],
                              const uint8_t *prologue, size_t prologue_len,
                              const uint8_t *e_priv)
{
    struct wl_sha256 s;
    struct wl_noise *n;

    if (psk == NULL || (prologue == NULL && prologue_len > 0)) {
        (void)wl_fail(WL_ERR_INVAL, "noise: no psk, or a prologue length without data");
        return NULL;
    }
    n = calloc(1, sizeof(*n));
    if (n == NULL) {
        (void)wl_fail(WL_ERR_NOMEM, "noise: out of memory");
        return NULL;
    }
    n->initiator = initiator != 0;
    memcpy(n->psk, psk, WL_NOISE_KEYLEN);
    if (e_priv != NULL) {
        memcpy(n->e_priv, e_priv, DHLEN);
    } else if (wl_random_bytes(n->e_priv, DHLEN) != WL_OK) {
        wl_noise_free(n);
        return NULL;
    }
    crypto_x25519_public_key(n->e_pub, n->e_priv);

    /* The name is longer than HASHLEN, so h starts as its hash rather than
     * the name padded with zeros. */
    wl_sha256_init(&s);
    wl_sha256_update(&s, PROTOCOL_NAME, sizeof(PROTOCOL_NAME) - 1U);
    wl_sha256_final(&s, n->h);
    memcpy(n->ck, n->h, HASHLEN);
    mix_hash(n, prologue, prologue_len);
    return n;
}

void wl_noise_free(struct wl_noise *n)
{
    if (n == NULL)
        return;
    crypto_wipe(n, sizeof(*n));
    free(n);
}

int wl_noise_is_transport(const struct wl_noise *n)
{
    return !n->dead && n->done == 2;
}

int wl_noise_wants_write(const struct wl_noise *n)
{
    if (n->dead)
        return 0;
    if (n->done == 2)
        return 1;
    /* The initiator writes message 0, the responder message 1. */
    return (n->done == 0) == n->initiator;
}

void wl_noise_handshake_hash(const struct wl_noise *n, uint8_t out[32])
{
    memcpy(out, n->h, HASHLEN);
}

/* A failed authentication leaves the state half-mixed (the handshake) or
 * says the peer is not who it should be (transport); either way nothing
 * sensible can follow, so the object refuses all further use. */
static wl_status kill(struct wl_noise *n, wl_status st)
{
    n->dead = 1;
    return st;
}

static wl_status transport_write(struct wl_noise *n, const uint8_t *payload, size_t plen,
                                 uint8_t *out, size_t cap, size_t *outlen)
{
    size_t need = plen + WL_NOISE_TAGLEN;

    if (plen > WL_NOISE_MAX_MSG - WL_NOISE_TAGLEN)
        return wl_fail(WL_ERR_INVAL, "noise: payload %zu exceeds %u", plen,
                       WL_NOISE_MAX_MSG - WL_NOISE_TAGLEN);
    if (cap < need)
        return wl_fail(WL_ERR_NOSPACE, "noise: %zu bytes needed, %zu available", need, cap);
    if (n->tx.n == NONCE_MAX)
        return kill(n, wl_fail(WL_ERR_INVAL, "noise: send nonce exhausted"));
    aead_encrypt(&n->tx, NULL, 0, payload, plen, out);
    n->tx.n++;
    *outlen = need;
    return WL_OK;
}

static wl_status transport_read(struct wl_noise *n, const uint8_t *msg, size_t len,
                                uint8_t *payload, size_t cap, size_t *plen)
{
    wl_status st;

    if (len > WL_NOISE_MAX_MSG)
        return wl_fail(WL_ERR_INVAL, "noise: message %zu exceeds %u", len,
                       WL_NOISE_MAX_MSG);
    if (len < WL_NOISE_TAGLEN)
        return kill(n, wl_fail(WL_ERR_CORRUPT, "noise: message of %zu bytes is too short",
                               len));
    if (cap < len - WL_NOISE_TAGLEN)
        return wl_fail(WL_ERR_NOSPACE, "noise: %zu bytes needed, %zu available",
                       len - WL_NOISE_TAGLEN, cap);
    if (n->rx.n == NONCE_MAX)
        return kill(n, wl_fail(WL_ERR_INVAL, "noise: receive nonce exhausted"));
    st = aead_decrypt(&n->rx, NULL, 0, msg, len - WL_NOISE_TAGLEN, payload);
    if (st != WL_OK)
        return kill(n, st);
    n->rx.n++;
    *plen = len - WL_NOISE_TAGLEN;
    return WL_OK;
}

/* Both handshake messages are an ephemeral key then an encrypted payload. */
#define HS_OVERHEAD (DHLEN + WL_NOISE_TAGLEN)

wl_status wl_noise_write_message(struct wl_noise *n, const uint8_t *payload, size_t plen,
                                 uint8_t *out, size_t cap, size_t *outlen)
{
    size_t need = plen + HS_OVERHEAD;

    if (n->dead)
        return wl_fail(WL_ERR_INVAL, "noise: session has failed");
    if (n->done == 2)
        return transport_write(n, payload, plen, out, cap, outlen);
    if (!wl_noise_wants_write(n))
        return wl_fail(WL_ERR_INVAL, "noise: not this side's turn to write");
    if (plen > WL_NOISE_MAX_MSG - HS_OVERHEAD)
        return wl_fail(WL_ERR_INVAL, "noise: payload %zu exceeds %u", plen,
                       WL_NOISE_MAX_MSG - HS_OVERHEAD);
    if (cap < need)
        return wl_fail(WL_ERR_NOSPACE, "noise: %zu bytes needed, %zu available", need, cap);

    if (n->done == 0) /* -> psk, e */
        mix_key_and_hash(n, n->psk, WL_NOISE_KEYLEN);
    /* e: in a psk handshake the ephemeral key is also mixed into the key
     * (section 9.2), which is what makes the payload's nonce 0 safe. */
    memcpy(out, n->e_pub, DHLEN);
    mix_hash(n, n->e_pub, DHLEN);
    mix_key(n, n->e_pub, DHLEN);
    if (n->done == 1) { /* <- e, ee */
        uint8_t dh[DHLEN];

        crypto_x25519(dh, n->e_priv, n->re);
        mix_key(n, dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));
    }
    encrypt_and_hash(n, payload, plen, out + DHLEN);
    *outlen = need;
    if (++n->done == 2)
        split(n);
    return WL_OK;
}

wl_status wl_noise_read_message(struct wl_noise *n, const uint8_t *msg, size_t len,
                                uint8_t *payload, size_t cap, size_t *plen)
{
    wl_status st;

    if (n->dead)
        return wl_fail(WL_ERR_INVAL, "noise: session has failed");
    if (n->done == 2)
        return transport_read(n, msg, len, payload, cap, plen);
    if (wl_noise_wants_write(n))
        return wl_fail(WL_ERR_INVAL, "noise: not this side's turn to read");
    if (len > WL_NOISE_MAX_MSG)
        return wl_fail(WL_ERR_INVAL, "noise: message %zu exceeds %u", len,
                       WL_NOISE_MAX_MSG);
    if (len < HS_OVERHEAD)
        return kill(n, wl_fail(WL_ERR_CORRUPT,
                               "noise: handshake message of %zu bytes is too short", len));
    if (cap < len - HS_OVERHEAD)
        return wl_fail(WL_ERR_NOSPACE, "noise: %zu bytes needed, %zu available",
                       len - HS_OVERHEAD, cap);

    if (n->done == 0) /* -> psk, e */
        mix_key_and_hash(n, n->psk, WL_NOISE_KEYLEN);
    memcpy(n->re, msg, DHLEN);
    mix_hash(n, n->re, DHLEN);
    mix_key(n, n->re, DHLEN);
    if (n->done == 1) { /* <- e, ee */
        uint8_t dh[DHLEN];

        crypto_x25519(dh, n->e_priv, n->re);
        mix_key(n, dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));
    }
    st = decrypt_and_hash(n, msg + DHLEN, len - HS_OVERHEAD, payload);
    if (st != WL_OK)
        return kill(n, st);
    *plen = len - HS_OVERHEAD;
    if (++n->done == 2)
        split(n);
    return WL_OK;
}
