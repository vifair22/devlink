#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "devlink/agent.h"
#include "devlink/agent_tcp.h"
#include "devlink/clock.h"
#include "devlink/echo.h"
#include "devlink/linktest.h"
#include "devlink/monitor.h"
#include "devlink/proto.h"
#include "devlink/transport.h"
#include "devlink/tui.h"
#include "devlink/version.h"
#include "tui_term.h"

/*
 * The Linux agent: the same agent as DEVLINK.EXE over the POSIX platform
 * layer, for a rescue image that boots on a machine whose own OS will not.
 * It runs under init, which restarts it when it exits, so @stop followed by a
 * put of a new binary is how it is replaced.
 */

static const char *const help =
    "usage: devlink-agent --version\n"
    "       devlink-agent serve [--tui] [PORT] [BAUD] [TCPPORT]\n"
    "       devlink-agent echo [PORT] [BAUD] [IDLE_SECONDS]\n"
    "       devlink-agent linktest [PORT] [BAUD] [WAIT_SECONDS]\n"
    "\n"
    "serve  the agent proper: serve devlink clients until stopped.\n"
    "       Defaults: /dev/ttyS0 115200 2098. PORT none serves TCP alone. TCP\n"
    "       is on only when DEVLINK.KEY beside this program holds a passphrase.\n"
    "       --tui draws a status screen on this terminal instead of a line\n"
    "       per event, with keys to scroll the log, restart, power off, reboot.\n"
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

/* ---------------------------------------------------------------------------
 * serve --tui: the status screen
 *
 * The agent loop runs untouched on a thread and reports through the same
 * callbacks as ever; wl_monitor (core, tested) holds the state and
 * wl_tui_render (core, tested) lays it out. What is left here is the thread,
 * the lock the monitor asks its owner for, the facts about the machine, and
 * running poweroff or reboot when asked.
 * ------------------------------------------------------------------------- */

struct tui_run {
    struct wl_transport *serial;
    const struct wl_agent_listener *tcp;
    const struct wl_agent_cfg *cfg;
    uint32_t seed;
    wl_status result;
    int done;
};

static pthread_mutex_t mon_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wl_monitor *mon;
/* 32 KB: static, as the project keeps to a 64 KB stack. */
static struct wl_tui_grid grid;

static void tui_log(void *ctx, const char *msg)
{
    uint64_t utc = 0;

    (void)ctx;
    (void)wl_clock_utc(&utc);
    (void)pthread_mutex_lock(&mon_lock);
    wl_monitor_note(mon, utc + (uint64_t)((int64_t)wl_clock_local_offset_minutes() * 60), msg);
    (void)pthread_mutex_unlock(&mon_lock);
}

static void tui_event(void *ctx, const struct wl_agent_event *e)
{
    (void)ctx;
    (void)pthread_mutex_lock(&mon_lock);
    wl_monitor_event(mon, e);
    (void)pthread_mutex_unlock(&mon_lock);
}

static void *agent_thread(void *arg)
{
    struct tui_run *r = arg;
    wl_status s = wl_agent_run(r->serial, r->tcp, r->cfg, r->seed);

    (void)pthread_mutex_lock(&mon_lock);
    r->result = s;
    r->done = 1;
    (void)pthread_mutex_unlock(&mon_lock);
    return NULL;
}

/* The first IPv4 address on an interface that is up and not loopback, and
 * how many more there are: what a person at the machine needs to reach it. */
static void net_text(char *out, size_t cap)
{
    struct ifaddrs *all;
    unsigned more = 0;

    (void)snprintf(out, cap, "no network");
    if (getifaddrs(&all) != 0)
        return;
    for (const struct ifaddrs *a = all; a != NULL; a = a->ifa_next) {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in in;

        if (a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET ||
            (a->ifa_flags & IFF_LOOPBACK) != 0U || (a->ifa_flags & IFF_UP) == 0U)
            continue;
        /* Copied out rather than cast: the sockaddr is only as aligned as the
         * generic one. */
        memcpy(&in, a->ifa_addr, sizeof(in));
        if (more++ == 0U && inet_ntop(AF_INET, &in.sin_addr, ip, sizeof(ip)) != NULL)
            (void)snprintf(out, cap, "%s %s", a->ifa_name, ip);
    }
    freeifaddrs(all);
    if (more > 1U) {
        size_t n = strlen(out);

        (void)snprintf(out + n, cap - n, " (+%u)", more - 1U);
    }
}

/* MemAvailable from /proc/meminfo, the kernel's own estimate of what a new
 * program could have. */
static void memory_text(char *out, size_t cap)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];

    out[0] = '\0';
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        static const char key[] = "MemAvailable:";
        char *end;
        unsigned long kb;

        if (strncmp(line, key, sizeof(key) - 1U) != 0)
            continue;
        kb = strtoul(line + sizeof(key) - 1U, &end, 10);
        if (end != line + sizeof(key) - 1U)
            (void)snprintf(out, cap, "%lu MB free", kb / 1024UL);
        break;
    }
    (void)fclose(f);
}

static void clock_text(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm t;

    if (localtime_r(&now, &t) != NULL)
        (void)snprintf(out, cap, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else
        out[0] = '\0';
}

/* poweroff and reboot are busybox's: they signal init, which runs the
 * shutdown actions in inittab and then stops every process, this one too. */
static void run_detached(const char *what)
{
    pid_t pid = fork();

    if (pid == 0) {
        (void)execlp(what, what, (char *)NULL);
        _exit(127);
    }
}

/* Serves with the screen up; returns what the agent loop returned. */
static wl_status serve_tui(struct wl_transport *serial, const struct wl_agent_listener *tcp,
                           struct wl_agent_cfg *cfg, const char *serial_text,
                           const char *tcp_text, unsigned tcp_on)
{
    struct tui_run run;
    struct wl_tui_view view;
    struct wl_tui_info info;
    pthread_t th;
    char net[96] = "";
    char memory[32] = "";
    char clock[16] = "";
    uint32_t net_at = 0;
    int full = 1;
    int done = 0;

    mon = wl_monitor_new();
    if (mon == NULL)
        return wl_fail(WL_ERR_NOMEM, "no memory for the status screen");
    cfg->log = tui_log;
    cfg->event = tui_event;
    memset(&run, 0, sizeof(run));
    run.serial = serial;
    run.tcp = tcp;
    run.cfg = cfg;
    run.seed = seed();
    wl_tui_view_init(&view);
    tui_log(NULL, "devlink agent started; ? for the keys");
    if (pthread_create(&th, NULL, agent_thread, &run) != 0) {
        wl_monitor_free(mon);
        return wl_fail(WL_ERR_IO, "cannot start the agent thread");
    }
    while (!done) {
        unsigned w;
        unsigned h;
        int key;

        /* The addresses change rarely and cost a netlink walk; memory and
         * the clock are cheap. */
        if (net[0] == '\0' || wl_clock_since(net_at) >= 5000U) {
            net_text(net, sizeof(net));
            net_at = wl_clock_ms();
        }
        memory_text(memory, sizeof(memory));
        clock_text(clock, sizeof(clock));
        memset(&info, 0, sizeof(info));
        info.version = wl_version();
        info.serial = serial_text;
        info.tcp = tcp_text;
        info.tcp_on = tcp_on;
        info.net = net;
        info.memory = memory;
        info.clock = clock;
        info.now_ms = wl_clock_ms();
        wl_term_size(&w, &h);
        (void)pthread_mutex_lock(&mon_lock);
        wl_tui_render(mon, &info, &view, &grid, w, h);
        done = run.done;
        (void)pthread_mutex_unlock(&mon_lock);
        wl_term_draw(&grid, full);
        full = 0;
        if (stop_requested)
            break;
        key = wl_term_key(250U);
        if (key < 0)
            continue;
        switch (wl_tui_key(&view, key)) {
        case WL_TUI_REDRAW:
            full = 1;
            break;
        case WL_TUI_RESTART:
            tui_log(NULL, "restart asked for at the keyboard");
            stop_requested = 1;
            break;
        case WL_TUI_POWEROFF:
            tui_log(NULL, "power off asked for at the keyboard");
            run_detached("poweroff");
            break;
        case WL_TUI_REBOOT:
            tui_log(NULL, "reboot asked for at the keyboard");
            run_detached("reboot");
            break;
        default:
            break;
        }
    }
    stop_requested = 1;
    (void)pthread_join(th, NULL);
    wl_term_close();
    wl_monitor_free(mon);
    mon = NULL;
    return run.result;
}

static int cmd_serve(int argc, char **argv)
{
    int tui = argc > 0 && strcmp(argv[0], "--tui") == 0;
    const char *port;
    unsigned long baud;
    unsigned long tcp_port;
    int serial;
    struct wl_agent_cfg cfg;
    struct wl_agent_tcp tcp;
    struct wl_transport t;
    char key[PATH_MAX];
    char serial_text[PATH_MAX + 32];
    char tcp_text[64];
    wl_status s;

    if (tui) {
        argc--;
        argv++;
    }
    port = argc > 0 ? argv[0] : default_port;
    baud = argc > 1 ? strtoul(argv[1], NULL, 10) : 115200UL;
    tcp_port = argc > 2 ? strtoul(argv[2], NULL, 10) : WL_TCP_DEFAULT_PORT;
    serial = strcmp(port, "none") != 0;
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
        (void)snprintf(serial_text, sizeof(serial_text), "%s at %lu", port, baud);
    else
        (void)snprintf(serial_text, sizeof(serial_text), "none");
    if (tcp.l != NULL)
        (void)snprintf(tcp_text, sizeof(tcp_text), "port %u", (unsigned)tcp.port);
    else
        (void)snprintf(tcp_text, sizeof(tcp_text), "no DEVLINK.KEY");
    if (tui && wl_term_open(STDIN_FILENO)) {
        s = serve_tui(serial ? &t : NULL, tcp.l != NULL ? &tcp.listener : NULL, &cfg,
                      serial_text, tcp_text, tcp.l != NULL);
        goto served;
    }
    if (tui)
        printf("devlink-agent: --tui needs a terminal; logging lines instead\n");
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
served:
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
