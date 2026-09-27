#ifndef DEVLINK_KEYFILE_H
#define DEVLINK_KEYFILE_H

#include <stdint.h>

#include "devlink/noise.h"
#include "devlink/status.h"

/*
 * The passphrase for TCP (docs/protocol.md, "Passphrase and key"), read from
 * a file whose first line it is, without the line ending, and turned into
 * the 32-byte key both sides use. The derivation is deliberately slow, so a
 * program does it once, when it starts.
 *
 * WL_ERR_INVAL means there is no such file, which for the agent simply means
 * TCP stays off; WL_ERR_CORRUPT that the file is there but holds no
 * passphrase, which is a mistake to report, never a quiet default.
 */
wl_status wl_key_load(const char *path, uint8_t psk[WL_NOISE_KEYLEN]);

#endif
