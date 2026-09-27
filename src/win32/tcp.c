#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsock.h>

#include "devlink/tcp.h"

/*
 * TCP for the Windows agent over Winsock 1.1, which is wsock32.dll, present
 * on every Win32 system this agent targets. Winsock 2 would buy nothing here:
 * what it adds that this would use (getaddrinfo, WSAPoll, IPv6) arrived with
 * XP, not with ws2_32 itself. So: IPv4 only, gethostbyname for names, select()
 * for every timeout.
 *
 * Every transport and listener brackets its life with WSAStartup and
 * WSACleanup. Winsock counts those calls itself, so the library is up exactly
 * while something uses it, with no static state here to get wrong across
 * threads.
 *
 * A SOCKET is not a C descriptor on Win32 and cannot be waited on alongside
 * anything that is not a socket, so the fd() operation reports -1. A caller
 * that has to watch several sockets passes them to select() together; note
 * that Winsock's fd_set is a counted array of FD_SETSIZE (64 unless defined
 * larger before winsock.h), not a bitmap, so the limit is on how many sockets
 * one call can wait on, not on their values.
 */

#ifdef __WATCOMC__
#pragma library("wsock32")
#endif

struct tcp_impl {
    SOCKET s;
};

struct wl_tcp_listener {
    SOCKET s;
    uint16_t port;
};

static int winsock_up(void)
{
    WSADATA wd;

    return WSAStartup(MAKEWORD(1, 1), &wd) == 0;
}

/* Winsock's error numbers mean nothing to a person reading a log on the
 * target, so the ones a network actually produces are named. */
static const char *wsa_text(int err)
{
    switch (err) {
    case WSAECONNREFUSED: return "connection refused";
    case WSAECONNRESET: return "connection reset";
    case WSAECONNABORTED: return "connection aborted";
    case WSAETIMEDOUT: return "timed out";
    case WSAEHOSTUNREACH: return "host unreachable";
    case WSAENETUNREACH: return "network unreachable";
    case WSAENETDOWN: return "network down";
    case WSAEADDRINUSE: return "address already in use";
    case WSAEADDRNOTAVAIL: return "address not available";
    case WSAENOBUFS: return "no buffer space";
    case WSAHOST_NOT_FOUND: return "host not found";
    case WSANO_DATA: return "no address for that name";
    case WSATRY_AGAIN: return "name lookup failed, try again";
    default: return "winsock error";
    }
}

static wl_status wsa_fail(const char *op, int err)
{
    return wl_fail(WL_ERR_IO, "%s: %s (%d)", op, wsa_text(err), err);
}

static wl_status peer_gone(const char *op, int err)
{
    if (err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAESHUTDOWN)
        return wl_fail(WL_ERR_IO, "the peer closed the connection (%s: %s)", op,
                       wsa_text(err));
    return wsa_fail(op, err);
}

static void ms_to_timeval(uint32_t ms, struct timeval *tv)
{
    tv->tv_sec = (long)(ms / 1000U);
    tv->tv_usec = (long)((ms % 1000U) * 1000U);
}

/* 1 readable, 0 timed out, -1 failed with the error recorded. */
static int wait_readable(SOCKET s, uint32_t timeout_ms)
{
    fd_set rd;
    struct timeval tv;
    int r;

    FD_ZERO(&rd);
    FD_SET(s, &rd);
    ms_to_timeval(timeout_ms, &tv);
    r = select(0, &rd, NULL, NULL, &tv);
    if (r == SOCKET_ERROR) {
        (void)wsa_fail("select", WSAGetLastError());
        return -1;
    }
    return r > 0;
}

static wl_status tcp_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                          uint32_t timeout_ms)
{
    struct tcp_impl *t = impl;
    int want = cap > (size_t)INT_MAX ? INT_MAX : (int)cap;
    int r;

    *got = 0;
    r = wait_readable(t->s, timeout_ms);
    if (r < 0)
        return WL_ERR_IO;
    if (r == 0)
        return WL_OK;
    r = recv(t->s, (char *)buf, want, 0);
    if (r == SOCKET_ERROR)
        return peer_gone("recv", WSAGetLastError());
    if (r == 0)
        return wl_fail(WL_ERR_IO, "the peer closed the connection");
    *got = (size_t)r;
    return WL_OK;
}

static wl_status tcp_write(void *impl, const uint8_t *buf, size_t len)
{
    struct tcp_impl *t = impl;

    while (len > 0) {
        /* send() takes an int count, so a larger buffer goes in pieces. */
        int chunk = len > 0x10000U ? 0x10000 : (int)len;
        int n = send(t->s, (const char *)buf, chunk, 0);

        if (n == SOCKET_ERROR)
            return peer_gone("send", WSAGetLastError());
        buf += n;
        len -= (size_t)n;
    }
    return WL_OK;
}

static void tcp_close(void *impl)
{
    struct tcp_impl *t = impl;

    (void)closesocket(t->s);
    free(t);
    (void)WSACleanup();
}

static void format_addr(const struct sockaddr_in *a, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%s:%u", inet_ntoa(a->sin_addr), (unsigned)ntohs(a->sin_port));
}

static wl_status tcp_caps(void *impl, char *out, size_t cap)
{
    struct tcp_impl *t = impl;
    struct sockaddr_in peer;
    struct sockaddr_in local;
    int plen = (int)sizeof(peer);
    int llen = (int)sizeof(local);
    char p[32];
    char l[32];
    int n;

    if (getpeername(t->s, (struct sockaddr *)&peer, &plen) == SOCKET_ERROR)
        return wsa_fail("caps: getpeername", WSAGetLastError());
    if (getsockname(t->s, (struct sockaddr *)&local, &llen) == SOCKET_ERROR)
        return wsa_fail("caps: getsockname", WSAGetLastError());
    format_addr(&peer, p, sizeof(p));
    format_addr(&local, l, sizeof(l));
    n = snprintf(out, cap, "transport tcp\npeer %s\nlocal %s\n", p, l);
    if (n < 0 || (size_t)n >= cap)
        return wl_fail(WL_ERR_NOSPACE, "caps: buffer too small");
    return WL_OK;
}

/* No descriptor: see the note at the top. */
static const struct wl_transport_ops tcp_ops = {tcp_read, tcp_write, tcp_close, tcp_caps,
                                                NULL};

/* Takes ownership of s and of one WSAStartup reference: on failure both are
 * released. */
static wl_status tcp_transport(SOCKET s, struct wl_transport *out)
{
    struct tcp_impl *t;
    BOOL one = TRUE;

    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (int)sizeof(one)) ==
        SOCKET_ERROR) {
        wl_status st = wsa_fail("setsockopt TCP_NODELAY", WSAGetLastError());

        (void)closesocket(s);
        (void)WSACleanup();
        return st;
    }
    t = malloc(sizeof(*t));
    if (t == NULL) {
        (void)closesocket(s);
        (void)WSACleanup();
        return wl_fail(WL_ERR_NOMEM, "tcp transport: out of memory");
    }
    t->s = s;
    out->ops = &tcp_ops;
    out->impl = t;
    return WL_OK;
}

/* A dotted address, or failing that a name. Winsock 1.1 has no lookup that
 * takes a timeout, so a name costs whatever the resolver takes. */
static wl_status resolve(const char *host, struct in_addr *out)
{
    unsigned long a = inet_addr(host);
    const struct hostent *he;

    if (a != INADDR_NONE || strcmp(host, "255.255.255.255") == 0) {
        out->s_addr = a;
        return WL_OK;
    }
    he = gethostbyname(host);
    if (he == NULL || he->h_addrtype != AF_INET || he->h_addr_list[0] == NULL)
        return wl_fail(WL_ERR_IO, "cannot resolve %s: %s", host,
                       wsa_text(WSAGetLastError()));
    memcpy(&out->s_addr, he->h_addr_list[0], sizeof(out->s_addr));
    return WL_OK;
}

static int set_nonblocking(SOCKET s, u_long on)
{
    return ioctlsocket(s, (long)FIONBIO, &on) != SOCKET_ERROR;
}

/*
 * A blocking connect waits out the stack's own retries, over twenty seconds
 * for an address that does not answer, so the attempt is made non-blocking
 * and select() holds the deadline. Winsock reports a failed non-blocking
 * connect in the except set, not the write set as BSD does, so both are
 * watched.
 */
wl_status wl_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                         struct wl_transport *out)
{
    struct sockaddr_in addr;
    fd_set wr;
    fd_set ex;
    struct timeval tv;
    wl_status st;
    SOCKET s;
    int r;

    if (!winsock_up())
        return wl_fail(WL_ERR_IO, "winsock 1.1 is not available");
    memset(&addr, 0, sizeof(addr));
    st = resolve(host, &addr.sin_addr);
    if (st != WL_OK) {
        (void)WSACleanup();
        return st;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        st = wsa_fail("socket", WSAGetLastError());
        (void)WSACleanup();
        return st;
    }
    if (!set_nonblocking(s, 1)) {
        st = wsa_fail("ioctlsocket", WSAGetLastError());
        goto fail;
    }
    if (connect(s, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err != WSAEWOULDBLOCK) {
            st = wl_fail(WL_ERR_IO, "connect %s:%u: %s (%d)", host, (unsigned)port,
                         wsa_text(err), err);
            goto fail;
        }
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        FD_ZERO(&ex);
        FD_SET(s, &ex);
        ms_to_timeval(timeout_ms, &tv);
        r = select(0, NULL, &wr, &ex, &tv);
        if (r == SOCKET_ERROR) {
            st = wsa_fail("select", WSAGetLastError());
            goto fail;
        }
        if (r == 0) {
            st = wl_fail(WL_ERR_IO, "connect %s:%u: no answer within %lu ms", host,
                         (unsigned)port, (unsigned long)timeout_ms);
            goto fail;
        }
        if (FD_ISSET(s, &ex)) {
            int soerr = 0;
            int len = (int)sizeof(soerr);

            (void)getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
            st = wl_fail(WL_ERR_IO, "connect %s:%u: %s (%d)", host, (unsigned)port,
                         wsa_text(soerr), soerr);
            goto fail;
        }
    }
    if (!set_nonblocking(s, 0)) {
        st = wsa_fail("ioctlsocket", WSAGetLastError());
        goto fail;
    }
    return tcp_transport(s, out);

fail:
    (void)closesocket(s);
    (void)WSACleanup();
    return st;
}

wl_status wl_tcp_listen(const char *bind_addr, uint16_t port, struct wl_tcp_listener **out)
{
    struct wl_tcp_listener *l;
    struct sockaddr_in addr;
    int len = (int)sizeof(addr);
    wl_status st;
    SOCKET s;

    if (!winsock_up())
        return wl_fail(WL_ERR_IO, "winsock 1.1 is not available");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_addr == NULL || bind_addr[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        st = resolve(bind_addr, &addr.sin_addr);
        if (st != WL_OK) {
            (void)WSACleanup();
            return st;
        }
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        st = wsa_fail("socket", WSAGetLastError());
        (void)WSACleanup();
        return st;
    }
    /* No SO_REUSEADDR: on Windows it lets another process bind the same port
     * and take connections meant for this one (see tcp.h). */
    if (bind(s, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();

        st = wl_fail(WL_ERR_IO, "bind port %u: %s (%d)", (unsigned)port, wsa_text(err), err);
        goto fail;
    }
    /* 5 is the most a Windows 98 or 2000 Workstation backlog honours. */
    if (listen(s, 5) == SOCKET_ERROR) {
        st = wsa_fail("listen", WSAGetLastError());
        goto fail;
    }
    if (getsockname(s, (struct sockaddr *)&addr, &len) == SOCKET_ERROR) {
        st = wsa_fail("getsockname", WSAGetLastError());
        goto fail;
    }
    l = malloc(sizeof(*l));
    if (l == NULL) {
        st = wl_fail(WL_ERR_NOMEM, "tcp listener: out of memory");
        goto fail;
    }
    l->s = s;
    l->port = ntohs(addr.sin_port);
    *out = l;
    return WL_OK;

fail:
    (void)closesocket(s);
    (void)WSACleanup();
    return st;
}

wl_status wl_tcp_accept(struct wl_tcp_listener *l, uint32_t timeout_ms, struct wl_transport *out,
                        char *peer, size_t peer_cap)
{
    struct sockaddr_in addr;
    int len = (int)sizeof(addr);
    SOCKET s;
    int r;

    out->ops = NULL;
    out->impl = NULL;
    r = wait_readable(l->s, timeout_ms);
    if (r < 0)
        return WL_ERR_IO;
    if (r == 0)
        return WL_OK;
    s = accept(l->s, (struct sockaddr *)&addr, &len);
    if (s == INVALID_SOCKET) {
        int err = WSAGetLastError();

        /* The caller reset the connection while it waited in the queue:
         * nobody arrived, as far as the caller is concerned. */
        if (err == WSAECONNRESET || err == WSAEWOULDBLOCK || err == WSAECONNABORTED)
            return WL_OK;
        return wsa_fail("accept", err);
    }
    if (peer != NULL && peer_cap > 0)
        format_addr(&addr, peer, peer_cap);
    /* The new socket holds its own reference, released by its close. */
    if (!winsock_up()) {
        (void)closesocket(s);
        return wl_fail(WL_ERR_IO, "winsock 1.1 is not available");
    }
    return tcp_transport(s, out);
}

int wl_tcp_listener_fd(const struct wl_tcp_listener *l)
{
    (void)l;
    return -1;
}

uint16_t wl_tcp_listener_port(const struct wl_tcp_listener *l)
{
    return l->port;
}

void wl_tcp_listener_close(struct wl_tcp_listener *l)
{
    if (l == NULL)
        return;
    (void)closesocket(l->s);
    free(l);
    (void)WSACleanup();
}
