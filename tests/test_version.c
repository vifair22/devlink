#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "w98link/version.h"

/* The handshake will compare these strings byte for byte, so the format is a
 * contract: semver, underscore, build stamp, dot, variant. */
static void test_version_has_semver_prefix(void **state)
{
    const char *v = wl_version();
    const char *us = strchr(v, '_');

    (void)state;
    assert_non_null(us);
    assert_true(us > v);
    assert_non_null(strchr(us, '.'));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version_has_semver_prefix),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
