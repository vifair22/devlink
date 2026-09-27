#ifndef DEVLINK_KDF_H
#define DEVLINK_KDF_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/sha256.h"
#include "devlink/status.h"

/*
 * The key derivation the TCP link needs, all over the one SHA-256 that sync
 * already uses: HMAC (RFC 2104), HKDF (RFC 5869), which Noise builds its key
 * schedule on, and PBKDF2 (RFC 8018), which turns the passphrase into the
 * pre-shared key (docs/protocol.md, "Passphrase and key").
 */

#define WL_SHA256_LEN 32U
#define WL_SHA256_BLOCK 64U

/* HMAC-SHA256 as a streaming context, so a key's padded blocks are hashed
 * once and the context copied for each message. PBKDF2 depends on that: it
 * would otherwise spend half its time rehashing the same two blocks. */
struct wl_hmac_sha256 {
    struct wl_sha256 inner;
    struct wl_sha256 outer;
};

void wl_hmac_sha256_init(struct wl_hmac_sha256 *h, const uint8_t *key, size_t key_len);
void wl_hmac_sha256_update(struct wl_hmac_sha256 *h, const void *data, size_t n);
/* Also wipes the context, which holds key material. */
void wl_hmac_sha256_final(struct wl_hmac_sha256 *h, uint8_t out[WL_SHA256_LEN]);

void wl_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t n,
                    uint8_t out[WL_SHA256_LEN]);

/* HKDF-SHA256, extract then expand. out_len is at most 255 * 32. */
wl_status wl_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                         size_t ikm_len, const uint8_t *info, size_t info_len,
                         uint8_t *out, size_t out_len);

/* PBKDF2-HMAC-SHA256. iterations is at least 1. */
wl_status wl_pbkdf2_sha256(const uint8_t *pass, size_t pass_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t *out,
                           size_t out_len);

/* Clears memory that held a secret in a way the compiler may not drop as a
 * dead store. */
void wl_wipe(void *p, size_t n);

#endif
