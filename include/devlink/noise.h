#ifndef DEVLINK_NOISE_H
#define DEVLINK_NOISE_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/status.h"

/*
 * Noise_NNpsk0_25519_ChaChaPoly_SHA256 (Noise Protocol Framework, revision
 * 34), the TCP link's handshake and transport encryption (docs/protocol.md,
 * "Link layer: TCP"). One object is one side of one connection: it runs the
 * two handshake messages, then encrypts and decrypts transport messages with
 * the keys the handshake produced.
 *
 * Framing (the u16 length in front of each message) is the caller's; these
 * functions see whole messages only.
 */

#define WL_NOISE_KEYLEN 32
#define WL_NOISE_TAGLEN 16
/* Noise's own limit on a message, handshake or transport. */
#define WL_NOISE_MAX_MSG 65535U
#define WL_NOISE_PBKDF2_ITERATIONS 100000U

/* The prologue both sides bind into the handshake, so a peer speaking
 * another protocol version cannot complete it. */
#define WL_NOISE_PROLOGUE "devlink 1"

/* PBKDF2-HMAC-SHA256(pass, "devlink psk v1", WL_NOISE_PBKDF2_ITERATIONS, 32).
 * Deliberately slow: about a second on the target, once at startup. */
void wl_noise_psk_from_passphrase(const char *pass, size_t len,
                                  uint8_t psk[WL_NOISE_KEYLEN]);

struct wl_noise;

/* initiator is nonzero on the client. e_priv NULL generates the ephemeral
 * key with wl_random_bytes(); tests pass a fixed one. Returns NULL with a
 * message set if memory or randomness is unavailable. */
struct wl_noise *wl_noise_new(int initiator, const uint8_t psk[WL_NOISE_KEYLEN],
                              const uint8_t *prologue, size_t prologue_len,
                              const uint8_t *e_priv);

/* Wipes every key before freeing. NULL is accepted. */
void wl_noise_free(struct wl_noise *n);

/*
 * During the handshake: write or read the next handshake message, carrying
 * an optional payload (plen may be 0). Calling the one that is not this
 * side's turn is WL_ERR_INVAL.
 *
 * After it: encrypt or decrypt one transport message. The output is plen +
 * WL_NOISE_TAGLEN bytes when writing, len - WL_NOISE_TAGLEN when reading.
 *
 * A message that fails to authenticate returns WL_ERR_CORRUPT, and the
 * object is then dead: every later call fails. Neither a handshake nor a
 * transport stream can be resumed after that.
 */
wl_status wl_noise_write_message(struct wl_noise *n, const uint8_t *payload, size_t plen,
                                 uint8_t *out, size_t cap, size_t *outlen);
wl_status wl_noise_read_message(struct wl_noise *n, const uint8_t *msg, size_t len,
                                uint8_t *payload, size_t cap, size_t *plen);

/* Nonzero once the handshake is complete and the transport keys are split. */
int wl_noise_is_transport(const struct wl_noise *n);

/* Nonzero when the next call should be wl_noise_write_message: during the
 * handshake, when it is this side's turn; in transport, always, since either
 * side may send at any time. Zero on a dead object. */
int wl_noise_wants_write(const struct wl_noise *n);

/* The handshake hash h, which is the same on both sides of a session and
 * unique to it. Only meaningful once in transport. */
void wl_noise_handshake_hash(const struct wl_noise *n, uint8_t out[32]);

#endif
