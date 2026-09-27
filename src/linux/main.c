#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "devlink/agent.h"
#include "devlink/agent_tcp.h"
#include "devlink/clock.h"
#include "devlink/echo.h"
#include "devlink/linktest.h"
#include "devlink/proto.h"
#include "devlink/transport.h"
#include "devlink/version.h"

/*
 * The Linux agent: the same agent as DEVLINK.EXE over the POSIX platform
 * layer, for a rescue image that boots on a machine whose own OS will not.
 * It runs under init, which restarts it when it exits, so @stop followed by a
 * put of a new binary is how it is replaced.
 */

static const char *const help =
    "usage: devlink-agent --version\n"
    "       devlink-agent serve [PORT] [BAUD] [TCPPORT]\n"
    "       devlink-agent echo [PORT] [BAUD] [IDLE_SECONDS]\n"
    "       devlink-agent linktest [PORT] [BAUD] [WAIT_SECONDS]\n"
    "\n"
    "serve  the agent proper: serve devlink clients until stopped.\n"
    "       Defaults: /dev/ttyS0 115200 2098. PORT none serves TCP alone. TCP\n"
    "       is on only when DEVLINK.KEY beside this program holds a passphrase.\n"
    "echo   serve 'devlink echo': echo every frame back.\n"
    "       Defaults: /dev/ttyS0 115200 60. Returns when the probe finishes or\n"
    "       after IDLE_SECONDS without a frame.\n"
    "linktest  serve 'devlink linktest': echo the reliable stream back.\n"
    "       Defaults: /dev/ttyS0 115200 120. Returns 5 s after the stream stops.\n";

static const char *const default_port = "/dev/ttyS0";

static volatile sig_atomic_t stop_requested;

static void on_term(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int should_stop(void *ctx)
{
    (void)ctx;
    return stop_requested;
}

static void print_platform(void)
{
    struct utsname u;

    if (uname(&u) == 0)
        printf("linux %s %s\n", u.release, u.machine);
}

static void console_log(void *ctx, const char *msg)
{
    time_t now = time(NULL);
    struct tm t;

    (void)ctx;
    if (localtime_r(&now, &t) != NULL)
        printf("%02d:%02d:%02d %s\n", t.tm_hour, t.tm_min, t.tm_sec, msg);
    else
        printf("%s\n", msg);
    fflush(stdout);
}

static uint32_t seed(void)
{
    return wl_clock_ms() ^ ((uint32_t)getpid() << 16);
}

/* DEVLINK.KEY beside the running program, as on Windows. */
static int key_path(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    char *slash;

    if (n <= 0 || (size_t)n >= cap - 1)
        return 0;
    out[n] = '\0';
    slash = strrchr(out, '/');
    slash = slash != NULL ? slash + 1 : out;
    return snprintf(slash, cap - (size_t)(slash - out), "DEVLINK.KEY") <
           (int)(cap - (size_t)(slash - out));
}

static int cmd_serve(int argc, char **argv)
{
    const char *port = argc > 0 ? argv[0] : default_port;
    unsigned long baud = argc > 1 ? strtoul(argv[1], NULL, 10) : 115200UL;
    unsigned long tcp_port = argc > 2 ? strtoul(argv[2], NULL, 10) : WL_TCP_DEFAULT_PORT;
    int serial = strcmp(port, "none") != 0;
    struct wl_agent_cfg cfg;
    struct wl_agent_tcp tcp;
    struct wl_transport t;
    char key[PATH_MAX];
    wl_status s;

    if (argc > 3 || baud == 0 || tcp_port == 0 || tcp_port > 65535UL) {
        fputs(help, stderr);
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    wl_session_cfg_init(&cfg.session);
    cfg.session.link.bytes_per_sec = (uint32_t)(baud / 10UL);
    cfg.platform = WL_PLATFORM_LINUX;
    cfg.log = console_log;
    cfg.stop = should_stop;
    if (!key_path(key, sizeof(key)) ||
        wl_agent_tcp_open(key, NULL, (uint16_t)tcp_port, &tcp) != WL_OK) {
        fprintf(stderr, "devlink-agent: TCP: %s\n", wl_error());
        return 1;
    }
    if (!serial && tcp.l == NULL) {
        fprintf(stderr, "devlink-agent: nothing to serve: no serial port, and no %s\n", key);
        return 1;
    }
    if (serial && wl_serial_open(port, (uint32_t)baud, &t) != WL_OK) {
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        wl_agent_tcp_close(&tcp);
        return 1;
    }
    if (serial)
        printf("devlink agent %s serving on %s at %lu\n", wl_version(), port, baud);
    else
        printf("devlink agent %s, no serial port\n", wl_version());
    if (tcp.l != NULL)
        printf("TCP: listening on port %u, encrypted with the passphrase in %s\n",
               (unsigned)tcp.port, key);
    else
        printf("TCP: off (no %s)\n", key);
    fflush(stdout);
    s = wl_agent_run(serial ? &t : NULL, tcp.l != NULL ? &tcp.listener : NULL, &cfg, seed());
    if (serial)
        wl_transport_close(&t);
    wl_agent_tcp_close(&tcp);
    if (s != WL_OK) {
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        return 1;
    }
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    const char *port = argc > 0 ? argv[0] : default_port;
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
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        return 1;
    }
    printf("echo: serving on %s at %lu, idle limit %lu s\n", port, baud, idle);
    s = wl_echo_serve(&t, (uint32_t)(idle * 1000UL), &st);
    wl_transport_close(&t);
    printf("echo: echoed %u frames; rx %u ok, %u crc, %u malformed, %u overflow\n",
           (unsigned)st.echoed, (unsigned)st.rx.frames, (unsigned)st.rx.crc,
           (unsigned)st.rx.malformed, (unsigned)st.rx.overflow);
    if (s != WL_OK) {
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        return 1;
    }
    return 0;
}

static int cmd_linktest(int argc, char **argv)
{
    const char *port = argc > 0 ? argv[0] : default_port;
    unsigned long baud = argc > 1 ? strtoul(argv[1], NULL, 10) : 115200UL;
    unsigned long wait = argc > 2 ? strtoul(argv[2], NULL, 10) : 120UL;
    struct wl_session_cfg cfg;
    struct wl_linktest_result r;
    struct wl_transport t;
    wl_status s;

    if (argc > 3 || baud == 0 || wait == 0) {
        fputs(help, stderr);
        return 1;
    }
    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = (uint32_t)(baud / 10UL);
    if (wl_serial_open(port, (uint32_t)baud, &t) != WL_OK) {
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        return 1;
    }
    printf("linktest: serving on %s at %lu, waiting up to %lu s\n", port, baud, wait);
    s = wl_linktest_serve(&t, &cfg, seed(), (uint32_t)(wait * 1000UL), 5000U, &r);
    wl_transport_close(&t);
    printf("linktest: echoed %lu bytes in %u session(s); resent %u (%u timer, %u fast); "
           "rx %u ok, %u crc, %u malformed\n",
           (unsigned long)r.bytes, (unsigned)r.sessions, (unsigned)r.stats.retransmits,
           (unsigned)r.stats.timeouts, (unsigned)r.stats.fast_retx,
           (unsigned)r.stats.rx.frames, (unsigned)r.stats.rx.crc,
           (unsigned)r.stats.rx.malformed);
    if (s != WL_OK) {
        fprintf(stderr, "devlink-agent: %s\n", wl_error());
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;

    /* SIGTERM from init at shutdown ends the serve loop cleanly rather than
     * mid-frame. SIGINT the same, for running it by hand. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("devlink agent %s\n", wl_version());
        print_platform();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
        return cmd_serve(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "echo") == 0)
        return cmd_echo(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "linktest") == 0)
        return cmd_linktest(argc - 2, argv + 2);
    fputs(help, argc >= 2 && strcmp(argv[1], "--help") == 0 ? stdout : stderr);
    return argc >= 2 && strcmp(argv[1], "--help") == 0 ? 0 : 1;
}
