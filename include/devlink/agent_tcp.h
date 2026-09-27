#ifndef DEVLINK_AGENT_TCP_H
#define DEVLINK_AGENT_TCP_H

#include <stdint.h>

#include "devlink/agent.h"
#include "devlink/noise.h"
#include "devlink/tcp.h"

/*
 * The agent's TCP side as its programs start it, the same on Win32 and
 * Linux: read the key file, and only if there is one, listen. Without a key
 * the agent serves serial alone, which is what it did before TCP existed;
 * there is never a TCP listener without a passphrase.
 */
struct wl_agent_tcp {
    struct wl_tcp_listener *l; /* NULL when TCP is off */
    struct wl_agent_listener listener;
    uint8_t psk[WL_NOISE_KEYLEN];
    uint16_t port;
};

/* WL_OK with t->l set when listening; WL_OK with t->l NULL when there is no
 * key file at key_path (TCP off, which is not an error); otherwise the
 * reason it could not listen. bind_addr NULL listens on every interface. */
wl_status wl_agent_tcp_open(const char *key_path, const char *bind_addr, uint16_t port,
                            struct wl_agent_tcp *t);

void wl_agent_tcp_close(struct wl_agent_tcp *t);

#endif
