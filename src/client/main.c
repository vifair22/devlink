#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "w98link/client_exec.h"
#include "w98link/clock.h"
#include "w98link/echo.h"
#include "w98link/linktest.h"
#include "w98link/proto.h"
#include "w98link/transport.h"
#include "w98link/version.h"

#define DEFAULT_LINE "/dev/ttyUSB0"
#define DEFAULT_BAUD 115200U

/* ---------------------------------------------------------------------------
 * help
 * ------------------------------------------------------------------------- */

static const char *const help_main =
    "usage: w98link <command> [options]\n"
    "\n"
    "commands:\n"
    "  exec      run a command on the agent and stream its output\n"
    "  echo      test the cable and ports against 'W98AGENT echo'\n"
    "  linktest  measure the reliable link against 'W98AGENT linktest'\n"
    "\n"
    "  w98link --version\n"
    "  w98link <command> --help\n";

static const char *const help_echo =
    "usage: w98link echo [--line PATH] [--baud N] [--count N] [--size N]\n"
    "\n"
    "Sends numbered frames and checks each echo from 'W98AGENT echo'.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $W98LINK_LINE)\n"
    "  --baud N      line speed (default 115200)\n"
    "  --count N     frames to send (default 20)\n"
    "  --size N      payload bytes per frame, up to 1024 (default 1024)\n";

static const char *const help_exec =
    "usage: w98link exec [--line PATH] [--baud N] [--cwd DIR] [--merge] [--] COMMAND...\n"
    "\n"
    "Runs COMMAND on the agent through its shell (command.com on Win9x,\n"
    "cmd.exe on NT) and streams stdout and stderr back. Exits with the\n"
    "command's exit status, or 125 if it could not be run at all.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $W98LINK_LINE)\n"
    "  --baud N      line speed (default 115200)\n"
    "  --cwd DIR     directory to run in, on the agent (default: its own)\n"
    "  --merge       send stderr down stdout, preserving their interleaving\n";

static const char *const help_linktest =
    "usage: w98link linktest [--line PATH] [--baud N] [--bytes N] [--window N]\n"
    "                        [--file PATH]\n"
    "\n"
    "Streams data through 'W98AGENT linktest', which echoes it back, and\n"
    "checks every byte. Both directions run at once, compressed.\n"
    "\n"
    "  --line PATH   serial device (default " DEFAULT_LINE ", or $W98LINK_LINE)\n"
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
    fprintf(stderr, "w98link: %s '%s'\n\n%s", what, arg,
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
        fprintf(stderr, "w98link: invalid %s '%s'\n", what, s);
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
    const char *line = getenv("W98LINK_LINE");
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
        fprintf(stderr, "w98link: %s\n", wl_error());
        return 1;
    }
    /* A 1 KiB frame takes about 90 ms each way at 115200; a second covers
     * that and Win98's scheduling with plenty to spare. */
    s = wl_echo_probe(&t, count, size, 1000, &st);
    wl_transport_close(&t);
    if (s != WL_OK) {
        fprintf(stderr, "w98link: %s\n", wl_error());
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
        fprintf(stderr, "w98link: cannot open %s: %s\n", path, strerror(errno));
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
        fprintf(stderr, "w98link: cannot read %s: %s\n", path,
                len == 0 ? "empty file" : strerror(errno));
        return NULL;
    }
    *len_out = (size_t)len;
    return data;
}

static int cmd_linktest(int argc, char **argv)
{
    const char *line = getenv("W98LINK_LINE");
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

    cfg.link.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.link.window = (uint8_t)window;
    cfg.link.bytes_per_sec = baud / 10U;
    cfg.level = WL_SESSION_DEFAULT_LEVEL;
    if (wl_serial_open(line, baud, &t) != WL_OK) {
        fprintf(stderr, "w98link: %s\n", wl_error());
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
        fprintf(stderr, "w98link: %s\n", wl_error());
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

    cfg.link.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.link.window = WL_LINK_DEFAULT_WINDOW;
    cfg.link.bytes_per_sec = baud / 10U;
    cfg.level = WL_SESSION_DEFAULT_LEVEL;
    return cfg;
}

static int cmd_exec(int argc, char **argv)
{
    const char *line = getenv("W98LINK_LINE");
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
    struct wl_exec_io io = {NULL, exec_output};
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

    cfg = serial_session(baud);
    if (wl_serial_open(line, baud, &t) != WL_OK) {
        fprintf(stderr, "w98link: %s\n", wl_error());
        free(cmd);
        return 125;
    }
    s = wl_client_exec(&t, &cfg, wl_clock_ms() ^ ((uint32_t)getpid() << 16), cmd, cwd,
                       merge ? WL_EXEC_MERGE_STDERR : 0, &io, 10000, &r);
    wl_transport_close(&t);
    free(cmd);
    if (s != WL_OK) {
        fprintf(stderr, "w98link: %s\n", wl_error());
        return 125;
    }
    if (!r.ran || r.message[0] != '\0') {
        fprintf(stderr, "w98link: %s\n", r.message[0] ? r.message : "command failed");
        return 125;
    }
    return (int)(r.exit_code & 0xFFU);
}

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *help;
};

static const struct command commands[] = {
    {"exec", cmd_exec, help_exec},
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
        printf("w98link %s\n", wl_version());
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
