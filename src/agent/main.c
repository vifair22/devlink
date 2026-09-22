#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "w98link/echo.h"
#include "w98link/linktest.h"
#include "w98link/transport.h"
#include "w98link/version.h"

static const char *const help =
    "usage: W98AGENT --version\n"
    "       W98AGENT echo [PORT] [BAUD] [IDLE_SECONDS]\n"
    "       W98AGENT linktest [PORT] [BAUD] [WAIT_SECONDS]\n"
    "\n"
    "echo   serve 'w98link echo' from Linux: echo every frame back.\n"
    "       Defaults: COM1 115200 60. Returns when the probe finishes or\n"
    "       after IDLE_SECONDS without a frame.\n"
    "linktest  serve 'w98link linktest': echo the reliable stream back.\n"
    "       Defaults: COM1 115200 120. Returns 5 s after the stream stops.\n";

static void print_platform(void)
{
    OSVERSIONINFOA vi;

    vi.dwOSVersionInfoSize = sizeof(vi);
    if (GetVersionExA(&vi))
        printf("windows platform %lu, %lu.%lu build %lu\n", vi.dwPlatformId,
               vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber & 0xFFFFU);
}

static int cmd_echo(int argc, char **argv)
{
    const char *port = argc > 0 ? argv[0] : "COM1";
    unsigned long baud = argc > 1 ? strtoul(argv[1], NULL, 10) : 115200UL;
    unsigned long idle = argc > 2 ? strtoul(argv[2], NULL, 10) : 60UL;
    struct wl_transport t;
    struct wl_echo_stats st;
    wl_status s;

    if (argc > 3 || baud == 0 || idle == 0) {
        fputs(help, stderr);
        return 1;
    }
    if (wl_serial_open(port, (uint32_t)baud, &t) != WL_OK) {
        fprintf(stderr, "W98AGENT: %s\n", wl_error());
        return 1;
    }
    printf("echo: serving on %s at %lu, idle limit %lu s\n", port, baud, idle);
    s = wl_echo_serve(&t, (uint32_t)(idle * 1000UL), &st);
    wl_transport_close(&t);
    printf("echo: echoed %u frames; rx %u ok, %u crc, %u malformed, %u overflow\n",
           (unsigned)st.echoed, (unsigned)st.rx.frames, (unsigned)st.rx.crc,
           (unsigned)st.rx.malformed, (unsigned)st.rx.overflow);
    if (s != WL_OK) {
        fprintf(stderr, "W98AGENT: %s\n", wl_error());
        return 1;
    }
    return 0;
}

static int cmd_linktest(int argc, char **argv)
{
    const char *port = argc > 0 ? argv[0] : "COM1";
    unsigned long baud = argc > 1 ? strtoul(argv[1], NULL, 10) : 115200UL;
    unsigned long wait = argc > 2 ? strtoul(argv[2], NULL, 10) : 120UL;
    struct wl_link_cfg cfg;
    struct wl_linktest_result r;
    struct wl_transport t;
    wl_status s;

    if (argc > 3 || baud == 0 || wait == 0) {
        fputs(help, stderr);
        return 1;
    }
    cfg.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.window = WL_LINK_DEFAULT_WINDOW;
    cfg.bytes_per_sec = (uint32_t)(baud / 10UL);
    if (wl_serial_open(port, (uint32_t)baud, &t) != WL_OK) {
        fprintf(stderr, "W98AGENT: %s\n", wl_error());
        return 1;
    }
    printf("linktest: serving on %s at %lu, waiting up to %lu s\n", port, baud, wait);
    s = wl_linktest_serve(&t, &cfg, (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId(),
                          (uint32_t)(wait * 1000UL), 5000U, &r);
    wl_transport_close(&t);
    printf("linktest: echoed %lu bytes in %u session(s); resent %u (%u timer, %u fast); "
           "rx %u ok, %u crc, %u malformed\n",
           (unsigned long)r.bytes, (unsigned)r.sessions, (unsigned)r.stats.retransmits,
           (unsigned)r.stats.timeouts, (unsigned)r.stats.fast_retx,
           (unsigned)r.stats.rx.frames, (unsigned)r.stats.rx.crc,
           (unsigned)r.stats.rx.malformed);
    if (s != WL_OK) {
        fprintf(stderr, "W98AGENT: %s\n", wl_error());
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("w98agent %s\n", wl_version());
        print_platform();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "echo") == 0)
        return cmd_echo(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "linktest") == 0)
        return cmd_linktest(argc - 2, argv + 2);
    fputs(help, argc >= 2 && strcmp(argv[1], "--help") == 0 ? stdout : stderr);
    return argc >= 2 && strcmp(argv[1], "--help") == 0 ? 0 : 1;
}
