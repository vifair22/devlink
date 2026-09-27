#ifndef DEVLINK_RANDOM_H
#define DEVLINK_RANDOM_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/status.h"

/*
 * Cryptographically secure random bytes from the operating system,
 * implemented per platform. Used for Noise's ephemeral keys, where a
 * predictable key would give away the session, so there is no fallback to
 * anything weaker: failure is reported and the connection refused.
 */
wl_status wl_random_bytes(uint8_t *buf, size_t n);

#endif
