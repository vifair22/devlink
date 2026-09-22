#ifndef W98LINK_SYNC_H
#define W98LINK_SYNC_H

#include <stdint.h>

#include "w98link/peer.h"
#include "w98link/transport.h"

/*
 * Client side of SYNC: make a directory on the agent match one here, sending
 * only the files that differ (docs/protocol.md, "SYNC").
 */

/* Also accepted in the flags, alongside WL_SYNC_DELETE and WL_SYNC_DRY_RUN:
 * include files and directories whose name starts with a dot. */
#define WL_SYNC_HIDDEN 0x04U

struct wl_sync_result {
    int ok;
    uint32_t scanned; /* files found locally */
    uint32_t needed;  /* files the agent asked for */
    uint32_t written; /* files it reports having written */
    uint32_t same;    /* files it already had */
    uint32_t deleted;
    uint32_t failed;
    uint64_t bytes; /* file bytes handed to the link, before compression */
    uint32_t elapsed_ms;
    uint16_t fail_code;
    char message[256];
};

wl_status wl_client_sync(struct wl_transport *t, const struct wl_session_cfg *cfg,
                         uint32_t seed, const char *local_root, const char *remote_root,
                         uint8_t flags, uint32_t connect_timeout_ms,
                         struct wl_sync_result *out);

#endif
