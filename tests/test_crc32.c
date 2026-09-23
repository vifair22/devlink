#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "devlink/crc32.h"

/* The standard check value: CRC-32 of the ASCII digits "123456789". */
static void test_check_value(void **state)
{
    (void)state;
    assert_int_equal(wl_crc32(0, "123456789", 9), 0xCBF43926U);
}

static void test_empty_is_identity(void **state)
{
    (void)state;
    assert_int_equal(wl_crc32(0, "", 0), 0);
    assert_int_equal(wl_crc32(0x12345678U, "", 0), 0x12345678U);
}

/* Continuing from a previous result must equal one pass over the whole
 * buffer; the frame code relies on this zlib-compatible convention. */
static void test_incremental_matches_single_pass(void **state)
{
    const char *s = "The quick brown fox jumps over the lazy dog";
    size_t n = strlen(s);

    (void)state;
    for (size_t split = 0; split <= n; split++) {
        uint32_t c = wl_crc32(0, s, split);
        c = wl_crc32(c, s + split, n - split);
        assert_int_equal(c, 0x414FA339U);
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_check_value),
        cmocka_unit_test(test_empty_is_identity),
        cmocka_unit_test(test_incremental_matches_single_pass),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
