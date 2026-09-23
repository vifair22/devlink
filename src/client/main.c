#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "devlink/client_exec.h"
#include "devlink/clock.h"
#include "devlink/file_ops.h"
#include "devlink/echo.h"
#include "devlink/linktest.h"
#include "devlink/mux.h"
#include "devlink/usock.h"
#include "devlink/proto.h"
#include "devlink/sync.h"
#include "devlink/transport.h"
#include "devlink/version.h"

#define DEFAULT_LINE "/dev/ttyUSB0"
#define DEFAULT_BAUD 115200U

/* ---------------------------------------------------------------------------
 * help
 * ------------------------------------------------------------------------- */

static const char *const help_main =
    "usage: devlink <command> [options]\n"
    "\n"
    "commands:\n"
    "  exec      run a command on the agent and stream its output\n"
    "  put       copy a local file to the agent\n"
    "  get       copy a file from the agent\n"
    "  sync      copy a directory to the agent, sending only what differs\n"
    "  info      show the agent's version and platform\n"
    "  time      compare the agent's clock with this one, or set it\n"
    "  mux       the multiplexer that lets several commands share the line\n"
    "  echo      test the cable and ports against 'DEVLINK echo'\n"
    "  linktest  measure the reliable link against 'DEVLINK linktest'\n"
    "\n"
    "  devlink --version\n"
    "  devlink <command> --help\n";

static const char *const help_echo =
    "usage: devlink echo [--line PATH] [--baud N] [--count N] [--size N]\n"
    "\n"
    "Sends numbered frames and checks each echo from 'DEVLINK echo'.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $DEVLINK_LINE)\n"
    "  --baud N      line speed (default 115200)\n"
    "  --count N     frames to send (default 20)\n"
    "  --size N      payload bytes per frame, up to 1024 (default 1024)\n";

static const char *const help_exec =
    "usage: devlink exec [--line PATH] [--baud N] [--cwd DIR] [--merge] [--] COMMAND...\n"
    "\n"
    "Runs COMMAND on the agent through its shell (command.com on Win9x,\n"
    "cmd.exe on NT) and streams stdout and stderr back. Exits with the\n"
    "command's exit status, or 125 if it could not be run at all.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $DEVLINK_LINE)\n"
    "  --baud N      line speed (default 115200)\n"
    "  --cwd DIR     directory to run in, on the agent (default: its own)\n"
    "  --merge       send stderr down stdout, preserving their interleaving\n";

static const char *const help_sync =
    "usage: devlink sync [--line PATH] [--baud N] [--delete] [--dry-run]\n"
    "                    [--hidden] LOCALDIR REMOTEDIR\n"
    "\n"
    "Makes REMOTEDIR on the agent match LOCALDIR, sending only files that\n"
    "differ in size, time or contents. Names starting with a dot are skipped\n"
    "unless --hidden is given, which is what keeps .git out of it.\n"
    "\n"
    "  --delete      remove files on the agent that are not here\n"
    "  --dry-run     report what would be sent, change nothing\n";

static const char *const help_info =
    "usage: devlink info [--line PATH] [--baud N]\n"
    "\n"
    "Connects to the agent, prints what it reports about itself, and\n"
    "disconnects.\n";

static const char *const help_mux =
    "usage: devlink mux [--line PATH] [--baud N] [--idle SECONDS]\n"
    "       devlink mux --stop [--line PATH]\n"
    "\n"
    "Runs the multiplexer: one process owns the line and keeps a single\n"
    "session to the agent, and every other command reaches the agent through\n"
    "it. That is what lets a second terminal run something without killing\n"
    "what is already running, so it starts by itself when a command needs it\n"
    "and exits after --idle seconds (600 by default) with nobody connected.\n"
    "\n"
    "Commands use it only when DEVLINK_MUX=1 is set, and then start it by\n"
    "themselves if it is not already running, logging to <socket>.log rather\n"
    "than into the terminal that needed it. It is off by default because\n"
    "starting it opens a session of its own, and the agent serves one session:\n"
    "on a line somebody is already using, that kills what they were running.\n"
    "Turn it on deliberately, in the shell you want it in.\n"
    "\n"
    "'echo' and 'linktest' always take the port directly whatever this says,\n"
    "since they test the cable rather than talk to the agent.\n";

static const char *const help_time =
    "usage: devlink time [--line PATH] [--baud N] [--set]\n"
    "\n"
    "Compares the agent's clock with this one and reports the difference.\n"
    "With --set, sets the agent's clock from this machine's first, then\n"
    "reports what is left. UTC is what is set, whatever time zone the target\n"
    "believes it is in.\n"
    "\n"
    "Worth doing before a sync: sync decides what to send by comparing\n"
    "modification times, so a target clock that is wrong makes it resend a\n"
    "whole tree or, worse, miss changes.\n";

static const char *const help_put =
    "usage: devlink put [--line PATH] [--baud N] [--force] LOCAL REMOTE\n"
    "\n"
    "Copies LOCAL to REMOTE on the agent, compressed, with a CRC check. The\n"
    "file lands under a temporary name and is renamed once it is verified.\n"
    "\n"
    "  --force       overwrite REMOTE if it exists\n";

static const char *const help_get =
    "usage: devlink get [--line PATH] [--baud N] REMOTE LOCAL\n"
    "\n"
    "Copies REMOTE from the agent to LOCAL, with the same checks.\n";

static const char *const help_linktest =
    "usage: devlink linktest [--line PATH] [--baud N] [--bytes N] [--window N]\n"
    "                        [--file PATH]\n"
    "\n"
    "Streams data through 'DEVLINK linktest', which echoes it back, and\n"
    "checks every byte. Both directions run at once, compressed.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $DEVLINK_LINE)\n"
    "  --baud N      line speed (default 115200)\n"
    "  --bytes N     bytes to stream (default 200000)\n"
    "  --window N    frames in flight, 1-32 (default 16)\n"
    "  --file PATH   stream this file instead of incompressible test data,\n"
    "                to measure real compression\n";

/* The topic shown when a command fails, so the help next to an error is
 * about the command that was being run. */
static const char *active_help = NULL;

static int usage_error(const char *what, const char *arg)
{
    fprintf(stderr, "devlink: %s '%s'\n\n%s", what, arg,
            active_help ? active_help : help_main);
    return 1;
}

/* ---------------------------------------------------------------------------
 * option parsing: every command has a whitelist of flags, each taking a value
 * ------------------------------------------------------------------------- */

/* A flag either takes a value (value != NULL) or is a switch (on != NULL). */
struct opt {
    const char *name;
    const char **value;
    int *on;
};

/* Parses options up to the first non-option argument or "--"; returns the
 * index of the first positional argument, or -1 after reporting an error.
 * Commands without positionals check that it equals argc. */
static int parse_opts(int argc, char **argv, struct opt *opts, size_t n_opts)
{
    int i;

    for (i = 0; i < argc; i++) {
        size_t k;

        if (strcmp(argv[i], "--") == 0)
            return i + 1;
        if (strncmp(argv[i], "--", 2) != 0)
            return i;
        for (k = 0; k < n_opts; k++) {
            if (strcmp(argv[i], opts[k].name) == 0)
                break;
        }
        if (k == n_opts) {
            (void)usage_error("unknown option", argv[i]);
            return -1;
        }
        if (opts[k].on != NULL) {
            *opts[k].on = 1;
            continue;
        }
        if (i + 1 >= argc) {
            (void)usage_error("missing value for option", argv[i]);
            return -1;
        }
        *opts[k].value = argv[++i];
    }
    return i;
}

/* For commands with a fixed number of positional arguments, where options
 * may appear anywhere: "put a b --force" reads as naturally as
 * "put --force a b". Returns the positional count, or -1 on error. */
static int parse_opts_mixed(int argc, char **argv, struct opt *opts, size_t n_opts,
                            char **pos, int max_pos)
{
    int n_pos = 0;
    int only_positional = 0;

    for (int i = 0; i < argc; i++) {
        size_t k;

        if (!only_positional && strcmp(argv[i], "--") == 0) {
            only_positional = 1;
            continue;
        }
        if (only_positional || strncmp(argv[i], "--", 2) != 0) {
            if (n_pos >= max_pos) {
                (void)usage_error("unexpected argument", argv[i]);
                return -1;
            }
            pos[n_pos++] = argv[i];
            continue;
        }
        for (k = 0; k < n_opts; k++) {
            if (strcmp(argv[i], opts[k].name) == 0)
                break;
        }
        if (k == n_opts) {
            (void)usage_error("unknown option", argv[i]);
            return -1;
        }
        if (opts[k].on != NULL) {
            *opts[k].on = 1;
            continue;
        }
        if (i + 1 >= argc) {
            (void)usage_error("missing value for option", argv[i]);
            return -1;
        }
        *opts[k].value = argv[++i];
    }
    return n_pos;
}

static int parse_all_opts(int argc, char **argv, struct opt *opts, size_t n_opts)
{
    int first = parse_opts(argc, argv, opts, n_opts);

    if (first < 0)
        return 1;
    if (first < argc)
        return usage_error("unexpected argument", argv[first]);
    return 0;
}

static int parse_u32(const char *s, const char *what, uint32_t max, uint32_t *out)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > max) {
        fprintf(stderr, "devlink: invalid %s '%s'\n", what, s);
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* ---------------------------------------------------------------------------
 * commands
 * ------------------------------------------------------------------------- */

static int cmd_echo(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    const char *count_s = NULL;
    const char *size_s = NULL;
    struct opt opts[] = {
        {"--line", &line, NULL},
        {"--baud", &baud_s, NULL},
        {"--count", &count_s, NULL},
        {"--size", &size_s, NULL},
    };
    uint32_t baud = DEFAULT_BAUD;
    uint32_t count = 20;
    uint32_t size = WL_SERIAL_MAX_PAYLOAD;
    struct wl_transport t;
    struct wl_echo_stats st;
    wl_status s;

    if (parse_all_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
        return 1;
    if (line == NULL)
        line = DEFAULT_LINE;
    if ((baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud)) ||
        (count_s && !parse_u32(count_s, "count", 65535U, &count)) ||
        (size_s && !parse_u32(size_s, "size", WL_SERIAL_MAX_PAYLOAD, &size)))
        return 1;

    if (wl_serial_open(line, baud, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    /* A 1 KiB frame takes about 90 ms each way at 115200; a second covers
     * that and Win98's scheduling with plenty to spare. */
    s = wl_echo_probe(&t, count, size, 1000, &st);
    wl_transport_close(&t);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }

    printf("sent %u, echoed %u, mismatched %u, timed out %u\n", (unsigned)st.sent,
           (unsigned)st.echoed, (unsigned)st.mismatched, (unsigned)st.timeouts);
    printf("rx: %u frames, %u crc errors, %u malformed, %u overflows\n",
           (unsigned)st.rx.frames, (unsigned)st.rx.crc, (unsigned)st.rx.malformed,
           (unsigned)st.rx.overflow);
    if (st.elapsed_ms > 0)
        printf("%.1f s, %.0f payload bytes/s each way (stop-and-wait)\n",
               (double)st.elapsed_ms / 1000.0,
               (double)st.echoed * size * 1000.0 / (double)st.elapsed_ms);
    return st.echoed == count ? 0 : 1;
}

/* Reads a whole file into a new buffer, or reports why not and returns NULL. */
static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long len = -1;

    *len_out = 0;
    if (f == NULL) {
        fprintf(stderr, "devlink: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0)
        len = ftell(f);
    if (len > 0 && fseek(f, 0, SEEK_SET) == 0)
        data = malloc((size_t)len);
    if (data != NULL && fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    (void)fclose(f);
    if (data == NULL) {
        fprintf(stderr, "devlink: cannot read %s: %s\n", path,
                len == 0 ? "empty file" : strerror(errno));
        return NULL;
    }
    *len_out = (size_t)len;
    return data;
}

static int cmd_linktest(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    const char *bytes_s = NULL;
    const char *window_s = NULL;
    const char *file = NULL;
    struct opt opts[] = {
        {"--line", &line, NULL},       {"--baud", &baud_s, NULL}, {"--bytes", &bytes_s, NULL},
        {"--window", &window_s, NULL}, {"--file", &file, NULL},
    };
    uint8_t *data = NULL;
    uint32_t baud = DEFAULT_BAUD;
    uint32_t bytes = 200000;
    uint32_t window = WL_LINK_DEFAULT_WINDOW;
    struct wl_session_cfg cfg;
    struct wl_linktest_result r;
    struct wl_transport t;
    wl_status s;

    if (parse_all_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
        return 1;
    if (line == NULL)
        line = DEFAULT_LINE;
    if ((baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud)) ||
        (bytes_s && !parse_u32(bytes_s, "byte count", 100000000U, &bytes)) ||
        (window_s && !parse_u32(window_s, "window", WL_LINK_MAX_WINDOW, &window)))
        return 1;
    if (window == 0)
        return usage_error("window must be at least", "1");
    if (file != NULL) {
        size_t len;

        data = read_file(file, &len);
        if (data == NULL)
            return 1;
        if (len > 100000000U) {
            free(data);
            return usage_error("file too large for linktest", file);
        }
        bytes = (uint32_t)len;
    }

    wl_session_cfg_init(&cfg);
    cfg.link.window = (uint8_t)window;
    cfg.link.bytes_per_sec = baud / 10U;
    if (wl_serial_open(line, baud, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        free(data);
        return 1;
    }
    /* Allow for the link to come up plus the transfer at a pessimistic
     * quarter of the line rate. */
    s = wl_linktest_probe(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), data, bytes,
                          10000U + (uint32_t)((uint64_t)bytes * 4000U / cfg.link.bytes_per_sec),
                          &r);
    wl_transport_close(&t);
    free(data);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    printf("echoed %llu of %u bytes, %llu corrupt, %u session(s)\n",
           (unsigned long long)r.bytes, (unsigned)bytes, (unsigned long long)r.corrupt,
           (unsigned)r.sessions);
    printf("frames: %u sent, %u resent (%u timer, %u fast); rx %u ok, %u crc, %u malformed\n",
           (unsigned)r.stats.data_sent, (unsigned)r.stats.retransmits,
           (unsigned)r.stats.timeouts, (unsigned)r.stats.fast_retx,
           (unsigned)r.stats.rx.frames, (unsigned)r.stats.rx.crc,
           (unsigned)r.stats.rx.malformed);
    if (r.zstats.z_out > 0)
        printf("compression: %llu -> %llu bytes (%.2fx)\n",
               (unsigned long long)r.zstats.plain_out, (unsigned long long)r.zstats.z_out,
               (double)r.zstats.plain_out / (double)r.zstats.z_out);
    if (r.elapsed_ms > 0)
        printf("%.1f s, %.0f bytes/s each way, rto %u ms\n", (double)r.elapsed_ms / 1000.0,
               (double)r.bytes * 1000.0 / (double)r.elapsed_ms, (unsigned)r.stats.rto_ms);
    return r.bytes == bytes && r.corrupt == 0 ? 0 : 1;
}

static void exec_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    FILE *f = stream == WL_EXEC_STDERR ? stderr : stdout;

    (void)ctx;
    (void)fwrite(p, 1, n, f);
    (void)fflush(f);
}

static struct wl_session_cfg serial_session(uint32_t baud)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = baud / 10U;
    return cfg;
}

/* A session that never leaves the machine: nothing to pace, and compressing
 * for a Unix socket would spend effort to save nothing. */
static struct wl_session_cfg local_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    cfg.link.bytes_per_sec = 0;
    cfg.level = 0;
    return cfg;
}

/*
 * Starts the multiplexer for this line and waits for its socket to answer.
 * Detached deliberately: it has to outlive the command that needed it, since
 * the whole point is that the next command finds it already there.
 */
/* Where an autostarted multiplexer's log goes, beside its socket. */
static int mux_log_path(const char *sock, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s.log", sock);

    return n > 0 && (size_t)n < cap;
}

static int start_mux(const char *line, uint32_t baud, const char *sock)
{
    const char *sock_for_log = sock;
    char baud_text[16];
    uint32_t start;
    pid_t pid;

    (void)snprintf(baud_text, sizeof(baud_text), "%lu", (unsigned long)baud);
    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        /* execv's signature predates const, so the strings are copied into
         * writable storage rather than cast about. */
        char prog[] = "devlink";
        char sub[] = "mux";
        char line_opt[] = "--line";
        char baud_opt[] = "--baud";
        char line_copy[256];
        char *const argv[] = {prog, sub, line_opt, line_copy, baud_opt, baud_text, NULL};
        int null_fd = open("/dev/null", O_RDWR);
        char log_path[288];
        int log_fd = -1;

        (void)snprintf(line_copy, sizeof(line_copy), "%s", line);
        /* Its log goes to a file, not to the terminal that happened to need
         * it: a multiplexer started behind a command should not interleave
         * "client connected" with that command's output. Run it by hand and
         * its log stays where you can see it. */
        if (mux_log_path(sock_for_log, log_path, sizeof(log_path)))
            log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);

        (void)setsid();
        if (null_fd >= 0) {
            (void)dup2(null_fd, 0);
            (void)dup2(null_fd, 1);
            if (null_fd > 2)
                (void)close(null_fd);
        }
        if (log_fd >= 0) {
            (void)dup2(log_fd, 2);
            if (log_fd > 2)
                (void)close(log_fd);
        }
        (void)execv("/proc/self/exe", argv);
        _exit(127);
    }
    /* It has a port to open and a handshake to do; a second is generous. */
    start = wl_clock_ms();
    while (wl_clock_since(start) < 2000U) {
        struct wl_transport probe;

        if (wl_unix_connect(sock, &probe) == WL_OK) {
            wl_transport_close(&probe);
            return 1;
        }
        (void)poll(NULL, 0, 25);
    }
    return 0;
}

/*
 * Opens the line for one command: directly, or through the multiplexer when
 * asked for with DEVLINK_MUX=1.
 *
 * Opt-in on purpose, and the reason is worth keeping. Routing through a
 * multiplexer changes what a command does to a line that somebody may already
 * be using: the multiplexer opens a session of its own, and the agent serves
 * one session, so starting one displaces whatever session is there and kills
 * its commands. That is the very thing it exists to prevent, and it happened
 * to a real transfer the first time this defaulted to on, because rebuilding
 * the binary changed how an already-running workflow reached the agent.
 *
 * A build that changes behaviour under a running workflow is a trap, however
 * good the behaviour. Turn it on deliberately, once, per shell.
 */
static wl_status open_line(const char *line, uint32_t baud, struct wl_session_cfg *cfg,
                           struct wl_transport *t)
{
    const char *want = getenv("DEVLINK_MUX");
    char sock[256];

    if (strncmp(line, "unix:", 5) == 0) {
        *cfg = local_session();
        return wl_unix_connect(line + 5, t);
    }
    if (want == NULL || want[0] == '0' || want[0] == '\0' ||
        !wl_mux_socket_path(line, sock, sizeof(sock))) {
        *cfg = serial_session(baud);
        return wl_serial_open(line, baud, t);
    }
    *cfg = local_session();
    if (wl_unix_connect(sock, t) == WL_OK)
        return WL_OK;
    if (start_mux(line, baud, sock) && wl_unix_connect(sock, t) == WL_OK)
        return WL_OK;
    /* Asked for but unavailable: opening the line directly gives the real
     * reason (the port is held, the device is missing) instead of a complaint
     * about a socket. */
    *cfg = serial_session(baud);
    return wl_serial_open(line, baud, t);
}

/*
 * Ctrl-C during exec has to reach the agent: killing the client on the spot
 * would leave the command running on the target, and on Windows 9x an
 * abandoned command can hold a drive or a file open until the agent's reaper
 * gets to it. The handler only raises a flag, which the exec loop turns into
 * a CANCEL record; the message is written with write(), the only output call
 * safe to make from a handler. The default disposition is restored first, so
 * a second Ctrl-C ends the client at once if the cancel goes nowhere.
 */
static volatile sig_atomic_t exec_cancel;

static void on_interrupt(int sig)
{
    static const char msg[] = "\ndevlink: cancelling on the target...\n";

    (void)signal(sig, SIG_DFL);
    exec_cancel = 1;
    (void)!write(2, msg, sizeof(msg) - 1);
}

static int cmd_exec(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    const char *cwd = NULL;
    int merge = 0;
    struct opt opts[] = {
        {"--line", &line, NULL},
        {"--baud", &baud_s, NULL},
        {"--cwd", &cwd, NULL},
        {"--merge", NULL, &merge},
    };
    uint32_t baud = DEFAULT_BAUD;
    struct wl_session_cfg cfg;
    struct wl_exec_io io = {NULL, exec_output, &exec_cancel};
    struct wl_exec_result r;
    struct wl_transport t;
    char *cmd;
    size_t len = 0;
    int first;
    wl_status s;

    first = parse_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0]));
    if (first < 0)
        return 125;
    if (first >= argc) {
        (void)usage_error("missing", "COMMAND");
        return 125;
    }
    if (line == NULL)
        line = DEFAULT_LINE;
    if (baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud))
        return 125;

    /* The remote shell parses the command line, so the words are simply
     * joined with spaces, as the user would have typed them there. */
    for (int i = first; i < argc; i++)
        len += strlen(argv[i]) + 1;
    cmd = malloc(len);
    if (cmd == NULL)
        return 125;
    {
        size_t at = 0;

        for (int i = first; i < argc; i++) {
            size_t n = strlen(argv[i]);

            if (i > first)
                cmd[at++] = ' ';
            memcpy(cmd + at, argv[i], n);
            at += n;
        }
        cmd[at] = '\0';
    }

    if (open_line(line, baud, &cfg, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        free(cmd);
        return 125;
    }
    (void)signal(SIGINT, on_interrupt);
    s = wl_client_exec(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), cmd, cwd,
                       merge ? WL_EXEC_MERGE_STDERR : 0, &io, 10000, &r);
    (void)signal(SIGINT, SIG_DFL);
    wl_transport_close(&t);
    free(cmd);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 125;
    }
    if (r.cancelled) {
        if (r.cancel_acked)
            fprintf(stderr, "devlink: cancelled; the command was killed on the target\n");
        else if (r.ran)
            fprintf(stderr, "devlink: cancelled, but the agent did not answer; the "
                            "command may still be winding down on the target\n");
        else
            fprintf(stderr, "devlink: cancelled\n");
        return 130;
    }
    if (!r.ran || r.message[0] != '\0') {
        fprintf(stderr, "devlink: %s\n", r.message[0] ? r.message : "command failed");
        return 125;
    }
    return (int)(r.exit_code & 0xFFU);
}

static void report_transfer(const char *verb, const struct wl_file_result *r)
{
    double secs = (double)r->elapsed_ms / 1000.0;

    if (!r->ok) {
        fprintf(stderr, "devlink: %s failed: %s\n", verb,
                r->message[0] != '\0' ? r->message : "unknown error");
        return;
    }
    printf("%s %llu bytes in %.1f s", verb, (unsigned long long)r->bytes, secs);
    if (secs > 0.05)
        printf(" (%.0f bytes/s)", (double)r->bytes / secs);
    printf("\n");
}

static int cmd_transfer(int argc, char **argv, int is_put)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    int force = 0;
    struct opt opts[] = {
        {"--line", &line, NULL}, {"--baud", &baud_s, NULL}, {"--force", NULL, &force},
    };
    uint32_t baud = DEFAULT_BAUD;
    struct wl_session_cfg cfg;
    struct wl_file_result r;
    struct wl_transport t;
    char *paths[2];
    int n_paths;
    wl_status s;

    n_paths = parse_opts_mixed(argc, argv, opts, sizeof(opts) / sizeof(opts[0]), paths, 2);
    if (n_paths < 0)
        return 1;
    if (n_paths != 2)
        return usage_error("expects two paths, got", n_paths == 1 ? paths[0] : "none");
    if (line == NULL)
        line = DEFAULT_LINE;
    if (baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud))
        return 1;

    if (open_line(line, baud, &cfg, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    if (is_put)
        s = wl_client_put(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), paths[0],
                          paths[1], force, 10000, &r);
    else
        s = wl_client_get(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), paths[0],
                          paths[1], 10000, &r);
    wl_transport_close(&t);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    report_transfer(is_put ? "sent" : "received", &r);
    return r.ok ? 0 : 1;
}

static int cmd_sync(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    int delete_extra = 0;
    int dry_run = 0;
    int hidden = 0;
    struct opt opts[] = {
        {"--line", &line, NULL},        {"--baud", &baud_s, NULL},
        {"--delete", NULL, &delete_extra}, {"--dry-run", NULL, &dry_run},
        {"--hidden", NULL, &hidden},
    };
    uint32_t baud = DEFAULT_BAUD;
    struct wl_session_cfg cfg;
    struct wl_sync_result r;
    struct wl_transport t;
    char *paths[2];
    int n_paths;
    uint8_t flags = 0;
    wl_status s;

    n_paths = parse_opts_mixed(argc, argv, opts, sizeof(opts) / sizeof(opts[0]), paths, 2);
    if (n_paths < 0)
        return 1;
    if (n_paths != 2)
        return usage_error("expects two directories, got",
                           n_paths == 1 ? paths[0] : "none");
    if (line == NULL)
        line = DEFAULT_LINE;
    if (baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud))
        return 1;
    flags = (uint8_t)((delete_extra ? WL_SYNC_DELETE : 0) | (dry_run ? WL_SYNC_DRY_RUN : 0) |
                      (hidden ? WL_SYNC_HIDDEN : 0));

    if (open_line(line, baud, &cfg, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    s = wl_client_sync(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), paths[0],
                       paths[1], flags, 10000, &r);
    wl_transport_close(&t);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    printf("%u file(s) here, %u already there, %u to send\n", (unsigned)r.scanned,
           (unsigned)r.same, (unsigned)r.needed);
    /* Checked before the dry-run branch: a dry run that failed used to print
     * a plausible summary and exit 0, which is exactly the wrong answer for
     * the command people run before deleting things. */
    if (wl_sync_exit_code(&r) != 0) {
        fprintf(stderr, "devlink: sync failed: %s\n",
                r.message[0] != '\0' ? r.message : "unknown error");
        return 1;
    }
    if (dry_run) {
        printf("dry run: nothing was changed\n");
        return 0;
    }
    printf("sent %u, deleted %u, failed %u; %llu bytes in %.1f s\n", (unsigned)r.written,
           (unsigned)r.deleted, (unsigned)r.failed, (unsigned long long)r.bytes,
           (double)r.elapsed_ms / 1000.0);
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    struct opt opts[] = {{"--line", &line, NULL}, {"--baud", &baud_s, NULL}};
    uint32_t baud = DEFAULT_BAUD;
    static const char *const platforms[] = {"?", "Windows 9x", "Windows NT", "Linux"};
    struct wl_session_cfg cfg;
    struct wl_transport t;
    struct wl_hello h;
    wl_status s;

    if (parse_all_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
        return 1;
    if (line == NULL)
        line = DEFAULT_LINE;
    if (baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud))
        return 1;
    if (open_line(line, baud, &cfg, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    s = wl_client_info(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), 10000, &h);
    wl_transport_close(&t);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    printf("agent   %s\n", h.build);
    printf("platform %s\n", h.platform < 4 ? platforms[h.platform] : "?");
    printf("protocol %u\n", (unsigned)h.proto);
    printf("client  %s\n", wl_version());
    return 0;
}

/* Collects the agent's reply to an @ command, which is a few short lines. */
struct reply {
    char text[512];
    size_t len;
};

static void reply_output(void *ctx, uint8_t stream, const uint8_t *p, size_t n)
{
    struct reply *r = ctx;

    if (stream != WL_EXEC_STDOUT)
        return;
    if (n > sizeof(r->text) - 1 - r->len)
        n = sizeof(r->text) - 1 - r->len;
    memcpy(r->text + r->len, p, n);
    r->len += n;
    r->text[r->len] = '\0';
}

/* Reads "name <number>" out of the agent's reply. */
static int reply_number(const struct reply *r, const char *name, long long *out)
{
    const char *at = strstr(r->text, name);

    if (at == NULL)
        return 0;
    at += strlen(name);
    while (*at == ' ')
        at++;
    if (*at != '-' && (*at < '0' || *at > '9'))
        return 0;
    *out = strtoll(at, NULL, 10);
    return 1;
}

/* Reads "name <rest of the line>" out of the agent's reply. */
static int reply_text(const struct reply *r, const char *name, char *out, size_t cap)
{
    const char *at = strstr(r->text, name);
    size_t n = 0;

    if (at == NULL)
        return 0;
    at += strlen(name);
    while (*at == ' ')
        at++;
    while (at[n] != '\0' && at[n] != '\n' && n + 1 < cap)
        n++;
    memcpy(out, at, n);
    out[n] = '\0';
    return n > 0;
}

/* Asks the agent one @ command and hands back what it said. */
static int ask_agent(struct wl_transport *t, const struct wl_session_cfg *cfg,
                     const char *cmd, struct reply *out)
{
    struct wl_exec_io io = {out, reply_output, NULL};
    struct wl_exec_result r;

    memset(out, 0, sizeof(*out));
    if (wl_client_exec(t, cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), cmd, NULL, 0,
                       &io, 10000, &r) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 0;
    }
    if (!r.ran || r.exit_code != 0) {
        fprintf(stderr, "devlink: %s\n", r.message[0] != '\0' ? r.message : "the agent refused");
        return 0;
    }
    return 1;
}

static void print_gmtime(const char *label, long long secs)
{
    time_t t = (time_t)secs;
    struct tm tm;
    char when[32] = "?";

    if (gmtime_r(&t, &tm) != NULL)
        (void)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    printf("%-11s %s UTC\n", label, when);
}

/* "2 m 28 s behind", from the target's point of view. */
static void print_difference(long long delta)
{
    long long mag = delta < 0 ? -delta : delta;

    if (delta == 0) {
        printf("difference  none\n");
        return;
    }
    printf("difference  ");
    if (mag >= 86400)
        printf("%lld d ", mag / 86400);
    if (mag >= 3600)
        printf("%lld h ", (mag % 86400) / 3600);
    if (mag >= 60)
        printf("%lld m ", (mag % 3600) / 60);
    printf("%lld s %s\n", mag % 60, delta < 0 ? "behind" : "ahead");
}

static int cmd_time(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    int set = 0;
    struct opt opts[] = {
        {"--line", &line, NULL},
        {"--baud", &baud_s, NULL},
        {"--set", NULL, &set},
    };
    uint32_t baud = DEFAULT_BAUD;
    struct wl_session_cfg cfg;
    struct wl_transport t;
    struct reply reply;
    uint64_t here = 0;
    long long there = 0;
    long long offset = 0;
    char zone[64] = "";
    uint32_t rtt;

    if (parse_all_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
        return 1;
    if (line == NULL)
        line = DEFAULT_LINE;
    if (baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud))
        return 1;
    if (open_line(line, baud, &cfg, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }

    if (set) {
        char cmd[64];

        (void)wl_clock_utc(&here);
        sprintf(cmd, "@time set %llu", (unsigned long long)here);
        if (!ask_agent(&t, &cfg, cmd, &reply)) {
            wl_transport_close(&t);
            return 1;
        }
    }
    /* Timed, because the reply is as old as half the round trip; on this line
     * that is tens of milliseconds, far inside what matters here, and saying
     * so is better than implying the comparison is exact. */
    rtt = wl_clock_ms();
    if (!ask_agent(&t, &cfg, "@time", &reply)) {
        wl_transport_close(&t);
        return 1;
    }
    rtt = wl_clock_since(rtt);
    wl_transport_close(&t);
    (void)wl_clock_utc(&here);
    if (!reply_number(&reply, "utc", &there)) {
        fprintf(stderr, "devlink: the agent did not report a time\n");
        return 1;
    }
    (void)reply_number(&reply, "localoffset", &offset);
    (void)reply_text(&reply, "zone", zone, sizeof(zone));

    print_gmtime("workstation", (long long)here);
    print_gmtime("target", there);
    print_difference(there - (long long)here);
    printf("target tz   UTC%+lld:%02lld", offset / 60, (offset < 0 ? -offset : offset) % 60);
    if (zone[0] != '\0')
        printf(" (%s)", zone);
    printf("\n");
    {
        char mine[64] = "";

        /* Both names, when they differ, because a zone that is wrong for
         * where the machine sits reads perfectly plausibly on its own. */
        wl_clock_zone_name(mine, sizeof(mine));
        if (mine[0] != '\0' && zone[0] != '\0' && offset != wl_clock_local_offset_minutes())
            printf("here tz     UTC%+d:%02d (%s)\n", wl_clock_local_offset_minutes() / 60,
                   (wl_clock_local_offset_minutes() < 0 ? -wl_clock_local_offset_minutes()
                                                        : wl_clock_local_offset_minutes()) % 60,
                   mine);
    }
    printf("round trip  %u ms\n", (unsigned)rtt);
    if (!set && (there - (long long)here > 2 || (long long)here - there > 2))
        printf("\nrun 'devlink time --set' to correct it; sync compares file times\n");
    return 0;
}

static volatile sig_atomic_t mux_stop;

static void on_mux_signal(int sig)
{
    (void)sig;
    mux_stop = 1;
}

static int mux_should_stop(void *ctx)
{
    (void)ctx;
    return mux_stop != 0;
}

static void mux_log(void *ctx, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "mux: %s\n", msg);
}

/* Where the pid is left, beside the socket, so --stop has something to aim
 * at: the protocol has no "go away" record and does not need one. */
static int mux_pid_path(const char *sock, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s.pid", sock);

    return n > 0 && (size_t)n < cap;
}

static int cmd_mux(int argc, char **argv)
{
    const char *line = getenv("DEVLINK_LINE");
    const char *baud_s = NULL;
    const char *idle_s = NULL;
    int stop = 0;
    struct opt opts[] = {
        {"--line", &line, NULL},
        {"--baud", &baud_s, NULL},
        {"--idle", &idle_s, NULL},
        {"--stop", NULL, &stop},
    };
    uint32_t baud = DEFAULT_BAUD;
    uint32_t idle = 600;
    struct wl_mux_cfg cfg;
    struct wl_transport t;
    char sock[256];
    char pidfile[288];
    wl_status s;
    FILE *f;

    if (parse_all_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
        return 1;
    if (line == NULL)
        line = DEFAULT_LINE;
    if ((baud_s && !parse_u32(baud_s, "baud rate", 4000000U, &baud)) ||
        (idle_s && !parse_u32(idle_s, "idle seconds", 86400U, &idle)))
        return 1;
    if (!wl_mux_socket_path(line, sock, sizeof(sock)) ||
        !mux_pid_path(sock, pidfile, sizeof(pidfile))) {
        fprintf(stderr, "devlink: cannot build a socket path for %s\n", line);
        return 1;
    }

    if (stop) {
        long pid = 0;

        f = fopen(pidfile, "r");
        if (f != NULL) {
            char text[32] = "";

            if (fgets(text, (int)sizeof(text), f) != NULL)
                pid = strtol(text, NULL, 10);
            (void)fclose(f);
        }
        if (pid <= 0) {
            fprintf(stderr, "devlink: no multiplexer is running for %s\n", line);
            return 1;
        }
        if (kill((pid_t)pid, SIGTERM) != 0) {
            fprintf(stderr, "devlink: cannot stop pid %ld: %s\n", pid, strerror(errno));
            return 1;
        }
        printf("stopping the multiplexer (pid %ld)\n", pid);
        return 0;
    }

    /*
     * An autostarted multiplexer arrives here through execv("/proc/self/exe"),
     * which is what guarantees it is this exact binary rather than whatever now
     * sits at the same path. The cost is the name: the kernel takes it from the
     * file that was executed, so the daemon runs as "exe" and neither ps nor
     * pkill finds it under the name it was started by. Saying so restores that,
     * without giving up execing the running image.
     */
    (void)prctl(PR_SET_NAME, "devlink", 0, 0, 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = serial_session(baud);
    cfg.socket_path = sock;
    cfg.idle_exit_ms = idle * 1000U;
    cfg.stop = mux_should_stop;
    cfg.log = mux_log;
    if (wl_serial_open(line, baud, &t) != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    (void)signal(SIGINT, on_mux_signal);
    (void)signal(SIGTERM, on_mux_signal);
    f = fopen(pidfile, "w");
    if (f != NULL) {
        (void)fprintf(f, "%ld\n", (long)getpid());
        (void)fclose(f);
    }
    s = wl_mux_serve(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16));
    wl_transport_close(&t);
    (void)remove(pidfile);
    if (s != WL_OK) {
        fprintf(stderr, "devlink: %s\n", wl_error());
        return 1;
    }
    return 0;
}

static int cmd_put(int argc, char **argv)
{
    return cmd_transfer(argc, argv, 1);
}

static int cmd_get(int argc, char **argv)
{
    return cmd_transfer(argc, argv, 0);
}

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *help;
};

static const struct command commands[] = {
    {"exec", cmd_exec, help_exec},
    {"sync", cmd_sync, help_sync},
    {"info", cmd_info, help_info},
    {"time", cmd_time, help_time},
    {"mux", cmd_mux, help_mux},
    {"put", cmd_put, help_put},
    {"get", cmd_get, help_get},
    {"echo", cmd_echo, help_echo},
    {"linktest", cmd_linktest, help_linktest},
};

static const struct command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const struct command *c;

    if (argc < 2) {
        fputs(help_main, stderr);
        return 1;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("devlink %s\n", wl_version());
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0) {
        fputs(help_main, stdout);
        return 0;
    }
    c = find_command(argv[1]);
    if (c == NULL)
        return usage_error("unknown command", argv[1]);
    active_help = c->help;
    /* --help among the options short-circuits their validation, so asking
     * for help never produces an error about the other arguments. It stops
     * at "--" or the first positional: after that the words belong to the
     * remote command, which may have a --help of its own. */
    for (int i = 2; i < argc && strcmp(argv[i], "--") != 0 && strncmp(argv[i], "--", 2) == 0;
         i++) {
        if (strcmp(argv[i], "--help") == 0) {
            fputs(active_help, stdout);
            return 0;
        }
    }
    return c->run(argc - 2, argv + 2);
}
