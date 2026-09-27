#include <string.h>

#include "devlink/agent_tcp.h"
#include "devlink/kdf.h"
#include "devlink/keyfile.h"

/* The agent's loop never blocks on the listener: it waits on its descriptor
 * with everything else, then takes whatever is there. */
static wl_status take(void *ctx, struct wl_transport *out, char *peer, size_t peer_cap)
{
    return wl_tcp_accept(ctx, 0, out, peer, peer_cap);
}

wl_status wl_agent_tcp_open(const char *key_path, const char *bind_addr, uint16_t port,
                            struct wl_agent_tcp *t)
{
    wl_status st;

    memset(t, 0, sizeof(*t));
    st = wl_key_load(key_path, t->psk);
    if (st == WL_ERR_INVAL)
        return WL_OK; /* no key: serial only */
    if (st != WL_OK)
        return st;
    st = wl_tcp_listen(bind_addr, port, &t->l);
    if (st != WL_OK) {
        wl_wipe(t->psk, sizeof(t->psk));
        t->l = NULL;
        return st;
    }
    t->port = wl_tcp_listener_port(t->l);
    t->listener.accept = take;
    t->listener.ctx = t->l;
    t->listener.fd = wl_tcp_listener_fd(t->l);
    t->listener.psk = t->psk;
    return WL_OK;
}

void wl_agent_tcp_close(struct wl_agent_tcp *t)
{
    if (t->l != NULL)
        wl_tcp_listener_close(t->l);
    wl_wipe(t->psk, sizeof(t->psk));
    memset(t, 0, sizeof(*t));
}
