#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98link/echo.h"
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
    "  echo      test the cable and ports against 'W98AGENT echo'\n"
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

struct opt {
    const char *name;
    const char **value;
};

static int parse_opts(int argc, char **argv, struct opt *opts, size_t n_opts)
{
    for (int i = 0; i < argc; i++) {
        size_t k;

        for (k = 0; k < n_opts; k++) {
            if (strcmp(argv[i], opts[k].name) == 0)
                break;
        }
        if (k == n_opts)
            return usage_error("unknown option", argv[i]);
        if (i + 1 >= argc)
            return usage_error("missing value for option", argv[i]);
        *opts[k].value = argv[++i];
    }
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
        {"--line", &line}, {"--baud", &baud_s}, {"--count", &count_s}, {"--size", &size_s},
    };
    uint32_t baud = DEFAULT_BAUD;
    uint32_t count = 20;
    uint32_t size = WL_SERIAL_MAX_PAYLOAD;
    struct wl_transport t;
    struct wl_echo_stats st;
    wl_status s;

    if (parse_opts(argc, argv, opts, sizeof(opts) / sizeof(opts[0])) != 0)
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

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *help;
};

static const struct command commands[] = {
    {"echo", cmd_echo, help_echo},
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
    /* --help anywhere short-circuits option validation, so asking for help
     * never produces an error about the other arguments. */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            fputs(active_help, stdout);
            return 0;
        }
    }
    return c->run(argc - 2, argv + 2);
}
