#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "devlink/paths.h"

/* The windowed agent replaces itself by renaming, so these three names have to
 * agree: the running file, the one uploaded beside it, and the one it is
 * renamed to. Getting this wrong means a failed update on a machine that takes
 * a walk to reach. */
static void test_the_extension_is_replaced(void **state)
{
    char out[64];

    (void)state;
    assert_int_equal(wl_path_sibling("C:\\DEVLINK\\DEVLINKW.EXE", ".NEW", out, sizeof(out)), 1);
    assert_string_equal(out, "C:\\DEVLINK\\DEVLINKW.NEW");
    assert_int_equal(wl_path_sibling("/opt/devlink/agent.exe", ".old", out, sizeof(out)), 1);
    assert_string_equal(out, "/opt/devlink/agent.old");
}

static void test_a_name_without_one_gains_it(void **state)
{
    char out[64];

    (void)state;
    assert_int_equal(wl_path_sibling("C:\\DEVLINK\\AGENT", ".NEW", out, sizeof(out)), 1);
    assert_string_equal(out, "C:\\DEVLINK\\AGENT.NEW");
    assert_int_equal(wl_path_sibling("agent", ".NEW", out, sizeof(out)), 1);
    assert_string_equal(out, "agent.NEW");
}

/* The case a plain strrchr gets wrong. */
static void test_a_dot_in_a_directory_is_not_an_extension(void **state)
{
    char out[64];

    (void)state;
    assert_int_equal(wl_path_sibling("C:\\v1.0\\DEVLINKW", ".NEW", out, sizeof(out)), 1);
    assert_string_equal(out, "C:\\v1.0\\DEVLINKW.NEW");
    assert_int_equal(wl_path_sibling("/srv/rel.2/agent", ".old", out, sizeof(out)), 1);
    assert_string_equal(out, "/srv/rel.2/agent.old");
}

static void test_it_refuses_rather_than_truncating(void **state)
{
    char out[16];

    (void)state;
    assert_int_equal(wl_path_sibling("C:\\SOMEWHERE\\LONG\\DEVLINKW.EXE", ".NEW", out,
                                     sizeof(out)),
                     0);
    assert_string_equal(out, "");
    assert_int_equal(wl_path_sibling(NULL, ".NEW", out, sizeof(out)), 0);
    assert_int_equal(wl_path_sibling("a", NULL, out, sizeof(out)), 0);
    assert_int_equal(wl_path_sibling("a", ".b", NULL, 8), 0);
    assert_int_equal(wl_path_sibling("a", ".b", out, 0), 0);
}

/* The boundary: a result that exactly fills the buffer is allowed, because the
 * terminator is counted in; one byte more is refused. */
static void test_the_terminator_is_counted(void **state)
{
    char out[9];

    (void)state;
    assert_int_equal(wl_path_sibling("abcd.exe", ".NEW", out, sizeof(out)), 1);
    assert_string_equal(out, "abcd.NEW"); /* eight characters and a NUL */
    assert_int_equal(wl_path_sibling("abcde.exe", ".NEW", out, sizeof(out)), 0);
    assert_string_equal(out, "");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_the_extension_is_replaced),
        cmocka_unit_test(test_a_name_without_one_gains_it),
        cmocka_unit_test(test_a_dot_in_a_directory_is_not_an_extension),
        cmocka_unit_test(test_it_refuses_rather_than_truncating),
        cmocka_unit_test(test_the_terminator_is_counted),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
