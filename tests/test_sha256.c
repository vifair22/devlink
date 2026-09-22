#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <cmocka.h>

#include "w98link/sha256.h"

static void check(const char *msg, size_t n, const char *hex)
{
    uint8_t out[32];
    struct wl_sha256 s;
    char got[65];

    wl_sha256_init(&s);
    wl_sha256_update(&s, msg, n);
    wl_sha256_final(&s, out);
    for (unsigned i = 0; i < 32; i++)
        (void)sprintf(got + (i * 2), "%02x", out[i]);
    assert_string_equal(got, hex);
}

/* The published FIPS 180-4 examples. */
static void test_known_vectors(void **state)
{
    (void)state;
    check("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_million_a(void **state)
{
    struct wl_sha256 s;
    uint8_t out[32];
    char got[65];

    (void)state;
    wl_sha256_init(&s);
    for (int i = 0; i < 1000000; i++)
        wl_sha256_update(&s, "a", 1);
    wl_sha256_final(&s, out);
    for (unsigned i = 0; i < 32; i++)
        (void)sprintf(got + (i * 2), "%02x", out[i]);
    assert_string_equal(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* Feeding the same data in different chunk sizes must not change the
 * digest: the file hasher works in 8 KiB reads. */
static void test_chunking_does_not_matter(void **state)
{
    uint8_t data[5000];
    uint8_t whole[32];
    struct wl_sha256 s;

    (void)state;
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7U);
    wl_sha256_init(&s);
    wl_sha256_update(&s, data, sizeof(data));
    wl_sha256_final(&s, whole);

    for (size_t chunk = 1; chunk <= 200; chunk += 37) {
        uint8_t piece[32];
        size_t at = 0;

        wl_sha256_init(&s);
        while (at < sizeof(data)) {
            size_t k = sizeof(data) - at < chunk ? sizeof(data) - at : chunk;

            wl_sha256_update(&s, data + at, k);
            at += k;
        }
        wl_sha256_final(&s, piece);
        assert_memory_equal(piece, whole, 32);
    }
}

static void test_file_hash(void **state)
{
    char path[128];
    uint8_t out[32];
    FILE *f;

    (void)state;
    (void)snprintf(path, sizeof(path), "/tmp/w98link-sha-%d", (int)getpid());
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite("abc", 1, 3, f), 3);
    assert_int_equal(fclose(f), 0);
    assert_int_equal(wl_sha256_file(path, out), 1);
    {
        char got[65];

        for (unsigned i = 0; i < 32; i++)
            (void)sprintf(got + (i * 2), "%02x", out[i]);
        assert_string_equal(got, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    (void)remove(path);
    assert_int_equal(wl_sha256_file("/nonexistent/file", out), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_known_vectors),
        cmocka_unit_test(test_million_a),
        cmocka_unit_test(test_chunking_does_not_matter),
        cmocka_unit_test(test_file_hash),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
