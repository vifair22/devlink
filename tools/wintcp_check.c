/*
 * A check of src/win32/tcp.c without a Windows machine: built with Open
 * Watcom exactly as the agent is and run under Wine by tools/wintcp-check.sh.
 * It is evidence, not a gate: Wine's wsock32 sits on the host's sockets, so
 * this proves the code drives the Winsock 1.1 API correctly, not that
 * Windows 98's or 2000's stack behaves identically.
 *
 *   WINTCP self               everything below against itself, in one process
 *   WINTCP echo PORT          accept one connection on 127.0.0.1:PORT, echo it
 *   WINTCP send HOST PORT N   send N patterned bytes, expect them echoed back
 *
 * Exit status 0 means every check passed; each failure prints a FAIL line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink/clock.h"
#include "devlink/tcp.h"

static int failures;

static void check(int ok, const char *what)
{
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s: %s\n", what, wl_error());
        failures++;
    }
}

static unsigned char pattern(size_t i)
{
    return (unsigned char)((i * 2654435761U) >> 13);
}

static int read_exactly(struct wl_transport *t, unsigned char *buf, size_t len)
{
    size_t at = 0;

    while (at < len) {
        size_t got = 0;

        if (wl_transport_read(t, buf + at, len - at, &got, 5000) != WL_OK || got == 0)
            return 0;
        at += got;
    }
    return 1;
}

static int self_test(void)
{
    struct wl_tcp_listener *l = NULL;
    struct wl_transport c = {NULL, NULL};
    struct wl_transport s = {NULL, NULL};
    char peer[64];
    char caps[256];
    unsigned char out[32768];
    unsigned char in[32768];
    size_t got = 1;
    uint16_t port;
    uint32_t start;
    size_t i;

    check(wl_tcp_listen("127.0.0.1", 0, &l) == WL_OK, "listen on 127.0.0.1 port 0");
    if (l == NULL)
        return 1;
    port = wl_tcp_listener_port(l);
    printf("     bound port %u, listener fd %d\n", (unsigned)port, wl_tcp_listener_fd(l));
    check(port != 0 && wl_tcp_listener_fd(l) == -1, "a real port, and no descriptor");

    start = wl_clock_ms();
    check(wl_tcp_accept(l, 100, &s, peer, sizeof(peer)) == WL_OK && s.ops == NULL,
          "accept with nobody there times out as ops == NULL");
    printf("     accept timeout took %lu ms\n", (unsigned long)wl_clock_since(start));

    /* The connection completes in the stack's backlog, so one thread can
     * connect and then accept. */
    check(wl_tcp_connect("127.0.0.1", port, 2000, &c) == WL_OK, "connect");
    check(wl_tcp_accept(l, 2000, &s, peer, sizeof(peer)) == WL_OK && s.ops != NULL,
          "accept");
    if (c.ops == NULL || s.ops == NULL)
        return 1;
    printf("     peer %s\n", peer);
    check(strncmp(peer, "127.0.0.1:", 10) == 0, "peer is a.b.c.d:port");
    check(wl_transport_fd(&s) == -1, "transport has no descriptor");
    check(wl_transport_caps(&s, caps, sizeof(caps)) == WL_OK, "caps");
    printf("%s", caps);
    check(strncmp(caps, "transport tcp\npeer 127.0.0.1:", 29) == 0 &&
              strstr(caps, "\nlocal 127.0.0.1:") != NULL,
          "caps names both ends");

    /* Less than a socket buffer each way, so neither write waits on a reader
     * in this one thread. */
    for (i = 0; i < sizeof(out); i++)
        out[i] = pattern(i);
    check(wl_transport_write(&c, out, sizeof(out)) == WL_OK, "client write 32 KiB");
    check(read_exactly(&s, in, sizeof(in)) && memcmp(in, out, sizeof(in)) == 0,
          "server reads it whole");
    check(wl_transport_write(&s, in, sizeof(in)) == WL_OK, "server writes it back");
    memset(in, 0, sizeof(in));
    check(read_exactly(&c, in, sizeof(in)) && memcmp(in, out, sizeof(in)) == 0,
          "client reads it whole");

    start = wl_clock_ms();
    check(wl_transport_read(&c, in, sizeof(in), &got, 100) == WL_OK && got == 0,
          "read timeout is WL_OK with zero bytes");
    printf("     read timeout took %lu ms\n", (unsigned long)wl_clock_since(start));

    wl_transport_close(&c);
    check(wl_transport_read(&s, in, sizeof(in), &got, 2000) == WL_ERR_IO &&
              strstr(wl_error(), "peer closed") != NULL,
          "peer close is WL_ERR_IO");
    printf("     %s\n", wl_error());
    wl_transport_close(&s);
    wl_tcp_listener_close(l);

    start = wl_clock_ms();
    check(wl_tcp_connect("127.0.0.1", port, 3000, &c) == WL_ERR_IO,
          "connect to the closed port fails");
    printf("     %s (%lu ms)\n", wl_error(), (unsigned long)wl_clock_since(start));
    check(wl_tcp_connect("no-such-host.invalid", 2098, 1000, &c) == WL_ERR_IO,
          "an unresolvable name fails");
    printf("     %s\n", wl_error());
    return failures != 0;
}

static int echo(uint16_t port)
{
    struct wl_tcp_listener *l = NULL;
    struct wl_transport s = {NULL, NULL};
    char peer[64];
    char caps[256];
    unsigned char buf[16384];
    unsigned long total = 0;

    if (wl_tcp_listen("127.0.0.1", port, &l) != WL_OK) {
        printf("FAIL listen: %s\n", wl_error());
        return 1;
    }
    printf("listening on %u\n", (unsigned)wl_tcp_listener_port(l));
    fflush(stdout);
    while (s.ops == NULL) {
        if (wl_tcp_accept(l, 1000, &s, peer, sizeof(peer)) != WL_OK) {
            printf("FAIL accept: %s\n", wl_error());
            return 1;
        }
    }
    wl_tcp_listener_close(l);
    printf("accepted %s\n", peer);
    if (wl_transport_caps(&s, caps, sizeof(caps)) == WL_OK)
        printf("%s", caps);
    for (;;) {
        size_t got = 0;

        if (wl_transport_read(&s, buf, sizeof(buf), &got, 10000) != WL_OK)
            break;
        if (got == 0) {
            printf("FAIL idle for 10 s\n");
            return 1;
        }
        if (wl_transport_write(&s, buf, got) != WL_OK) {
            printf("FAIL write: %s\n", wl_error());
            return 1;
        }
        total += (unsigned long)got;
    }
    printf("echoed %lu bytes, then: %s\n", total, wl_error());
    wl_transport_close(&s);
    return strstr(wl_error(), "peer closed") == NULL;
}

static int send_n(const char *host, uint16_t port, size_t n)
{
    struct wl_transport c = {NULL, NULL};
    unsigned char *out = malloc(n);
    unsigned char *in = malloc(n);
    size_t sent = 0;
    size_t recvd = 0;
    char caps[256];

    if (out == NULL || in == NULL || wl_tcp_connect(host, port, 3000, &c) != WL_OK) {
        printf("FAIL connect: %s\n", wl_error());
        return 1;
    }
    if (wl_transport_caps(&c, caps, sizeof(caps)) == WL_OK)
        printf("%s", caps);
    for (size_t i = 0; i < n; i++)
        out[i] = pattern(i);
    /* Interleaved, a slice at a time, so the echo never backs up behind a
     * writer that is not reading. */
    while (recvd < n) {
        size_t got = 0;

        if (sent < n) {
            size_t k = n - sent > 8192U ? 8192U : n - sent;

            if (wl_transport_write(&c, out + sent, k) != WL_OK) {
                printf("FAIL write: %s\n", wl_error());
                return 1;
            }
            sent += k;
        }
        if (wl_transport_read(&c, in + recvd, n - recvd, &got, 5000) != WL_OK) {
            printf("FAIL read: %s\n", wl_error());
            return 1;
        }
        if (got == 0 && sent == n) {
            printf("FAIL no echo for 5 s at %lu bytes\n", (unsigned long)recvd);
            return 1;
        }
        recvd += got;
    }
    wl_transport_close(&c);
    if (memcmp(in, out, n) != 0) {
        printf("FAIL echoed bytes differ\n");
        return 1;
    }
    printf("ok   %lu bytes sent and echoed back intact\n", (unsigned long)n);
    free(out);
    free(in);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "self") == 0)
        return self_test();
    if (argc == 3 && strcmp(argv[1], "echo") == 0)
        return echo((uint16_t)atoi(argv[2]));
    if (argc == 5 && strcmp(argv[1], "send") == 0)
        return send_n(argv[2], (uint16_t)atoi(argv[3]), (size_t)atol(argv[4]));
    fprintf(stderr, "usage: WINTCP self | echo PORT | send HOST PORT BYTES\n");
    return 2;
}
