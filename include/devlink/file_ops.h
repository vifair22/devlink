#ifndef DEVLINK_FILE_OPS_H
#define DEVLINK_FILE_OPS_H

#include <stdint.h>

#include "devlink/peer.h"
#include "devlink/transport.h"

/*
 * Client side of PUT and GET. Both check a CRC-32 over the whole file and
 * land it through a temporary name, so a failure never leaves a half-written
 * file where the real one should be.
 */

struct wl_file_result {
    int ok;
    uint64_t bytes;
    uint32_t elapsed_ms;
    uint16_t fail_code;
    char message[256];
};

wl_status wl_client_put(struct wl_transport *t, const struct wl_session_cfg *cfg,
                        uint32_t seed, const char *local, const char *remote, int overwrite,
                        uint32_t connect_timeout_ms, struct wl_file_result *out);

wl_status wl_client_get(struct wl_transport *t, const struct wl_session_cfg *cfg,
                        uint32_t seed, const char *remote, const char *local,
                        uint32_t connect_timeout_ms, struct wl_file_result *out);

#endif
