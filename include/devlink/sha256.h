#ifndef DEVLINK_SHA256_H
#define DEVLINK_SHA256_H

#include <stddef.h>
#include <stdint.h>

/*
 * SHA-256, used by sync to decide whether two files are the same.
 * A weaker hash would be faster, but a collision there means a stale file
 * silently left in place, which is the one failure this tool must not have.
 * Only the first WL_HASH_BYTES of the digest travel on the wire.
 */

#define WL_HASH_BYTES 16U

struct wl_sha256 {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
};

void wl_sha256_init(struct wl_sha256 *s);
void wl_sha256_update(struct wl_sha256 *s, const void *data, size_t n);
void wl_sha256_final(struct wl_sha256 *s, uint8_t out[32]);

/* Hashes a whole file; returns nonzero on success. */
int wl_sha256_file(const char *path, uint8_t out[32]);

#endif
