#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "devlink/clock.h"

static void test_write_then_read(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    uint8_t buf[64];
    size_t got = 0;
    size_t total = 0;
    /* Includes every byte a cooked tty would mangle: NUL, CR, LF, ^C, ^S. */
    const uint8_t msg[] = {0x00, 'h', '\r', '\n', 0x03, 0x13, 0x11, 0x7F, 0xFF};

    (void)state;
    pty_pair(&a, &b);
    assert_int_equal(wl_transport_write(&b, msg, sizeof(msg)), WL_OK);
    while (total < sizeof(msg)) {
        assert_int_equal(wl_transport_read(&a, buf + total, sizeof(buf) - total, &got, 1000),
                         WL_OK);
        assert_true(got > 0);
        total += got;
    }
    assert_int_equal(total, sizeof(msg));
    assert_memory_equal(buf, msg, sizeof(msg));

    /* And the other direction. */
    assert_int_equal(wl_transport_write(&a, msg, sizeof(msg)), WL_OK);
    total = 0;
    while (total < sizeof(msg)) {
        assert_int_equal(wl_transport_read(&b, buf + total, sizeof(buf) - total, &got, 1000),
                         WL_OK);
        total += got;
    }
    assert_memory_equal(buf, msg, sizeof(msg));
    wl_transport_close(&a);
    wl_transport_close(&b);
}

static void test_read_timeout_is_not_an_error(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    uint8_t buf[8];
    size_t got = 99;
    uint32_t start;

    (void)state;
    pty_pair(&a, &b);
    start = wl_clock_ms();
    assert_int_equal(wl_transport_read(&a, buf, sizeof(buf), &got, 50), WL_OK);
    assert_int_equal(got, 0);
    assert_true(wl_clock_since(start) >= 40);
    wl_transport_close(&a);
    wl_transport_close(&b);
}

static void test_peer_close_is_an_error(void **state)
{
    struct wl_transport a;
    struct wl_transport b;
    uint8_t buf[8];
    size_t got;

    (void)state;
    pty_pair(&a, &b);
    wl_transport_close(&a);
    assert_int_equal(wl_transport_read(&b, buf, sizeof(buf), &got, 1000), WL_ERR_IO);
    wl_transport_close(&b);
}

static void test_open_failures(void **state)
{
    struct wl_transport t;

    (void)state;
    assert_int_equal(wl_serial_open("/dev/null", 12345, &t), WL_ERR_INVAL);
    assert_non_null(strstr(wl_error(), "12345"));
    assert_int_equal(wl_serial_open("/nonexistent/tty", 115200, &t), WL_ERR_IO);
    /* Not a tty: open works, configuring it does not. */
    assert_int_equal(wl_serial_open("/dev/null", 115200, &t), WL_ERR_IO);
}

/*
 * One user of the line at a time. This is not politeness: the agent serves one
 * client, and a second client displaces the first, killing whatever command
 * was running on the target. It cost somebody an install once.
 *
 * flock is held per open file description, so a second open in this same
 * process is refused exactly as another program's would be.
 */
static void test_a_second_open_of_the_same_port_is_refused(void **state)
{
    struct wl_transport first;
    struct wl_transport second;
    char name[128];
    int master;
    int slave;

    (void)state;
    assert_int_equal(openpty(&master, &slave, name, NULL, NULL), 0);
    assert_int_equal(wl_serial_open(name, 115200, &first), WL_OK);
    assert_int_equal(wl_serial_open(name, 115200, &second), WL_ERR_IO);
    assert_non_null(strstr(wl_error(), "already in use"));

    /* And it is available again once the first user lets go, with no lock file
     * left behind to clean up. */
    wl_transport_close(&first);
    assert_int_equal(wl_serial_open(name, 115200, &second), WL_OK);
    wl_transport_close(&second);
    (void)close(slave);
    (void)close(master);
}

/*
 * Opens the port, optionally takes the lock, reports down the pipe either way,
 * and waits to be killed. Never returns: it is a child, and must not run any
 * of cmocka's bookkeeping on its way out.
 *
 * It lets go of the streams it inherited first. A child that keeps the test's
 * stdout open holds the pipe open with it, and anything reading the test's
 * output then waits for a process that is waiting to be killed.
 */
static void hold_the_port(const char *name, int lock_it, int tell)
{
    int null = open("/dev/null", O_RDWR);
    int fd;

    /*
     * Tied to the test's lifetime before anything else. A failed assertion
     * leaves cmocka by longjmp, so the kill at the end of the test is not
     * reached, and without this a failing run would leave these waiting for a
     * signal that never comes. The getppid check closes the gap between the
     * fork and here; the alarm is the backstop for anything else.
     */
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    if (getppid() == 1)
        _exit(0);
    (void)alarm(60);

    if (null >= 0) {
        (void)dup2(null, STDOUT_FILENO);
        (void)dup2(null, STDERR_FILENO);
        if (null > STDERR_FILENO)
            (void)close(null);
    }
    fd = open(name, O_RDWR | O_NOCTTY);
    (void)!write(tell, fd >= 0 && (!lock_it || flock(fd, LOCK_EX | LOCK_NB) == 0) ? "y" : "n", 1);
    (void)close(tell);
    for (;;)
        pause();
}

/*
 * The refusal has to name the process that is in the way, which is the one
 * holding the lock and not merely one with the port open. The two are
 * different often enough to matter: a multiplexer holds the lock while the
 * thing passing the bytes through also has the device open, and an earlier
 * version of this named the latter. Sending somebody after the wrong pid is
 * worse than naming nobody, because they may well kill it.
 *
 * The decoy is forked first so that it has the lower pid, which is the order
 * /proc lists them in and therefore the order a scan finds them.
 */
static void test_the_refusal_names_the_process_holding_the_lock(void **state)
{
    struct wl_transport t;
    char name[128];
    char want[32];
    int master;
    int slave;
    int tell[2];
    char ready[2];
    pid_t decoy;
    pid_t holder;

    (void)state;
    assert_int_equal(openpty(&master, &slave, name, NULL, NULL), 0);
    assert_int_equal(pipe(tell), 0);

    decoy = fork();
    assert_true(decoy >= 0);
    if (decoy == 0) {
        (void)close(tell[0]);
        hold_the_port(name, 0, tell[1]);
    }
    holder = fork();
    assert_true(holder >= 0);
    if (holder == 0) {
        (void)close(tell[0]);
        hold_the_port(name, 1, tell[1]);
    }
    (void)close(tell[1]);
    /* A pipe read returns with whatever has arrived, so both answers are
     * waited for one at a time rather than asked for together. */
    for (unsigned i = 0; i < 2U; i++) {
        assert_int_equal(read(tell[0], &ready[i], 1), 1);
        assert_int_equal(ready[i], 'y');
    }

    assert_int_equal(wl_serial_open(name, 115200, &t), WL_ERR_IO);
    (void)snprintf(want, sizeof(want), "pid %ld", (long)holder);
    assert_non_null(strstr(wl_error(), want));

    (void)kill(decoy, SIGKILL);
    (void)kill(holder, SIGKILL);
    (void)waitpid(decoy, NULL, 0);
    (void)waitpid(holder, NULL, 0);
    (void)close(tell[0]);
    (void)close(slave);
    (void)close(master);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_write_then_read),
        cmocka_unit_test(test_read_timeout_is_not_an_error),
        cmocka_unit_test(test_peer_close_is_an_error),
        cmocka_unit_test(test_open_failures),
        cmocka_unit_test(test_a_second_open_of_the_same_port_is_refused),
        cmocka_unit_test(test_the_refusal_names_the_process_holding_the_lock),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
