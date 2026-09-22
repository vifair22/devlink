#ifndef W98LINK_CLIENT_EXEC_H
#define W98LINK_CLIENT_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/peer.h"
#include "w98link/transport.h"

/* Client side of EXEC: connects, runs one command on the agent, streams its
 * output to the caller, and returns its exit status. */

struct wl_exec_io {
    void *ctx;
    /* stream is WL_EXEC_STDOUT or WL_EXEC_STDERR. */
    void (*output)(void *ctx, uint8_t stream, const uint8_t *p, size_t n);
};

struct wl_exec_result {
    int ran;             /* the agent started the command */
    uint32_t exit_code;  /* valid when ran */
    uint16_t fail_code;  /* protocol error code when the agent refused */
    char message[256];   /* the agent's reason, when it refused */
    struct wl_hello agent;
};

wl_status wl_client_exec(struct wl_transport *t, const struct wl_session_cfg *cfg,
                         uint32_t seed, const char *cmd, const char *cwd, uint8_t flags,
                         const struct wl_exec_io *io, uint32_t connect_timeout_ms,
                         struct wl_exec_result *out);

#endif
