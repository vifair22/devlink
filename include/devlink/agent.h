#ifndef DEVLINK_AGENT_H
#define DEVLINK_AGENT_H

#include <stddef.h>
#include <stdint.h>

#include "devlink/peer.h"
#include "devlink/transport.h"

/*
 * The agent's serving loop: answers whatever client connects on the serial
 * port, one session after another, and every client that connects over TCP,
 * all at once (docs/protocol.md, "Sessions and transports"). A session is
 * answered only over the connection it came in on. A client that disappears
 * is noticed by the link (restart, liveness, or its connection closing) and
 * its processes are killed; the next client gets a clean session.
 *
 * Platform-independent: processes come from proc.h and sockets from the
 * listener below, so the same loop runs on Win32 on the target and on Linux
 * in the tests.
 */

#define WL_AGENT_MAX_EXEC 8U /* commands at once, across every session */
#define WL_AGENT_MAX_XFER 4U /* per session */
#define WL_AGENT_MAX_CONN 8U /* TCP sessions at once, beside serial */

/*
 * What the agent is doing, as facts rather than prose.
 *
 * `log` below reports the same moments as a line of English, which is what a
 * console wants and all it can use. A display that shows work in progress
 * needs the channel, the kind and the byte counts, and prose cannot be turned
 * back into those. Both are reported; neither is derived from the other.
 */
enum {
    WL_EV_SESSION = 1, /* a client attached; the previous one's work is gone */
    WL_EV_READY,       /* it identified itself: text is its build string */
    WL_EV_GONE,        /* the session ended, by restart or by liveness */
    WL_EV_CHANNEL,     /* work started: ch, kind, text, and total if known */
    WL_EV_PROGRESS,    /* ch, done: bytes so far, counted from the start */
    WL_EV_FINISHED,    /* ch, and code: an exit status for WL_KIND_EXEC */
    /* @stop: the client asked the agent to exit, which is how an update is
     * applied. A front end that replaces itself needs to tell this apart from
     * being stopped at the keyboard, and the two are the same to the loop. */
    WL_EV_STOPPING,
    /* A TCP connection was accepted, or has ended; `session` names it. The
     * serial port is never connected or disconnected, only used. */
    WL_EV_CONNECTED,
    WL_EV_DISCONNECTED
};

struct wl_agent_event {
    uint8_t type;
    uint8_t kind; /* WL_KIND_*, on WL_EV_CHANNEL */
    uint16_t ch;
    uint32_t code;
    uint64_t done;
    uint64_t total; /* 0 when the size is not known in advance */
    const char *text; /* NULL when the event carries none */
    /* Which session: "serial", or a TCP client's address. Valid only during
     * the call, as text is. */
    const char *session;
    uint8_t tcp; /* nonzero for a TCP session */
};

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
    /* Called for the same moments in structured form, for a display that
     * shows current work rather than a history. NULL discards them. */
    void (*event)(void *ctx, const struct wl_agent_event *e);
    void *event_ctx;
    /* Checks a drive letter named by a command before the command is
     * started; NULL uses wl_fs_drive_ready. Injected so the tests can
     * present an empty drive on a machine that has no drive letters. */
    int (*drive_ready)(char letter);
};

/*
 * Where TCP clients come from. The core knows nothing of sockets, so the
 * platform hands it these: a way to take a waiting connection without
 * blocking, and something to wait on.
 */
struct wl_agent_listener {
    /* Takes one waiting connection: WL_OK with out->ops NULL when none is
     * waiting; peer gets the client's address. Must not block. */
    wl_status (*accept)(void *ctx, struct wl_transport *out, char *peer, size_t peer_cap);
    void *ctx;
    int fd; /* readable when a connection is waiting; -1 if it cannot be waited on */
    /* The 32-byte key from the passphrase. Required: no TCP without it. */
    const uint8_t *psk;
};

/* Serves serial (NULL for none) and TCP (NULL for none) until cfg->stop says
 * so or a client sends @stop. Fails if the serial port fails; a TCP
 * connection that fails only ends its own session. */
wl_status wl_agent_run(struct wl_transport *serial, const struct wl_agent_listener *tcp,
                       const struct wl_agent_cfg *cfg, uint32_t seed);

/* Serial only, as before TCP existed. */
wl_status wl_agent_serve(struct wl_transport *t, const struct wl_agent_cfg *cfg,
                         uint32_t seed);

#endif
