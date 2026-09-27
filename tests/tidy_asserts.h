#ifndef TEST_TIDY_ASSERTS_H
#define TEST_TIDY_ASSERTS_H

/*
 * For clang-tidy's static analyser only: `make tidy` force-includes this
 * ahead of each test file, and outside the analyser it defines nothing.
 *
 * A cmocka assertion that fails ends the test (a longjmp out of
 * _assert_true), but cmocka marks only _fail noreturn, so the analyser
 * believes every assertion can fall through. A guard such as
 * `assert_non_null(f)` after fopen then reads, to it, as no guard at all,
 * and each one is reported as a null dereference. Under the analyser the
 * assertions the tests use as guards become a test that calls fail(), which
 * it does understand. The comparisons are the same ones cmocka makes.
 */
#ifdef __clang_analyzer__
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#undef assert_true
#undef assert_false
#undef assert_non_null
#undef assert_null
#undef assert_int_equal
#undef assert_int_not_equal
#define assert_true(c) do { if (!(c)) fail(); } while (0)
#define assert_false(c) do { if (c) fail(); } while (0)
#define assert_non_null(c) do { if ((c) == NULL) fail(); } while (0)
#define assert_null(c) do { if ((c) != NULL) fail(); } while (0)
#define assert_int_equal(a, b) \
    do { if ((uintmax_t)(a) != (uintmax_t)(b)) fail(); } while (0)
#define assert_int_not_equal(a, b) \
    do { if ((uintmax_t)(a) == (uintmax_t)(b)) fail(); } while (0)
#endif

#endif
