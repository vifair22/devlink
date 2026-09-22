#ifndef W98LINK_AGENT_H
#define W98LINK_AGENT_H

#include <stdint.h>

#include "w98link/peer.h"
#include "w98link/transport.h"

/*
 * The agent's serving loop: answers whatever client connects on the
 * transport, one session after another, for as long as it runs. A client
 * that disappears is noticed by the link (restart or liveness) and its
 * processes are killed; the next client gets a clean session.
 *
 * Platform-independent: processes come from proc.h, so the same loop runs
 * on Win32 on the target and on Linux in the tests.
 */

#define WL_AGENT_MAX_EXEC 8U
#define WL_AGENT_MAX_XFER 4U

struct wl_agent_cfg {
    struct wl_session_cfg session;
    uint8_t platform;
    /* Called once per loop iteration; returning nonzero stops the agent.
     * NULL runs forever. */
    int (*stop)(void *ctx);
    void *stop_ctx;
    /* Called for each session, command and failure, for the console. */
    void (*log)(void *ctx, const char *msg);
    void *log_ctx;
    /* Checks a drive letter named by a command before the command is
     * started; NULL uses wl_fs_drive_ready. Injected so the tests can
     * present an empty drive on a machine that has no drive letters. */
    int (*drive_ready)(char letter);
};

wl_status wl_agent_serve(struct wl_transport *t, const struct wl_agent_cfg *cfg,
                         uint32_t seed);

#endif
