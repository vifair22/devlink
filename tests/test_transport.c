#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_write_then_read),
        cmocka_unit_test(test_read_timeout_is_not_an_error),
        cmocka_unit_test(test_peer_close_is_an_error),
        cmocka_unit_test(test_open_failures),
        cmocka_unit_test(test_a_second_open_of_the_same_port_is_refused),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
