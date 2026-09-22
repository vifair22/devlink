#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

#include "pty_pair.h"
#include "w98link/agent.h"
#include "w98link/file_ops.h"
#include "w98link/fs.h"
#include "w98link/proto.h"

/* PUT and GET end to end: the real agent in a thread, the real client, a pty
 * between them, and real files in a temporary directory. */

static struct {
    struct wl_transport t;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
    pthread_t th;
} agent;

static struct wl_transport client_t;
static char dir[256];

static void *agent_main(void *arg)
{
    (void)arg;
    agent.result = wl_agent_serve(&agent.t, &agent.cfg, 0xF11E5U);
    return NULL;
}

static int should_stop(void *ctx)
{
    (void)ctx;
    return agent.stop;
}

static struct wl_session_cfg fast_session(void)
{
    struct wl_session_cfg cfg;

    wl_session_cfg_init(&cfg);
    return cfg;
}

static int setup(void **state)
{
    (void)state;
    (void)snprintf(dir, sizeof(dir), "/tmp/w98link-test-%d", (int)getpid());
    assert_int_equal(mkdir(dir, 0700), 0);
    memset(&agent, 0, sizeof(agent));
    pty_pair(&agent.t, &client_t);
    agent.cfg.session = fast_session();
    agent.cfg.platform = WL_PLATFORM_LINUX;
    agent.cfg.stop = should_stop;
    assert_int_equal(pthread_create(&agent.th, NULL, agent_main, NULL), 0);
    return 0;
}

static int teardown(void **state)
{
    char cmd[512];

    (void)state;
    agent.stop = 1;
    assert_int_equal(pthread_join(agent.th, NULL), 0);
    assert_int_equal(agent.result, WL_OK);
    wl_transport_close(&agent.t);
    wl_transport_close(&client_t);
    (void)snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    assert_int_equal(system(cmd), 0);
    return 0;
}

static void path_in(char *buf, size_t cap, const char *name)
{
    (void)snprintf(buf, cap, "%s/%s", dir, name);
}

/* Writes a file whose contents are reproducible from the seed. */
static void write_file(const char *path, size_t n, uint32_t seed)
{
    FILE *f = fopen(path, "wb");
    uint32_t x = seed | 1U;

    assert_non_null(f);
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        /* Mostly text-like, so compression has something to do. */
        assert_int_equal(fputc((int)('a' + (x % 26U)), f), (int)('a' + (x % 26U)));
    }
    assert_int_equal(fclose(f), 0);
}

static void assert_same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int ca;
    int cb;

    assert_non_null(fa);
    assert_non_null(fb);
    do {
        ca = fgetc(fa);
        cb = fgetc(fb);
        assert_int_equal(ca, cb);
    } while (ca != EOF);
    (void)fclose(fa);
    (void)fclose(fb);
}

static wl_status put(const char *local, const char *remote, int force,
                     struct wl_file_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    static uint32_t seed = 500;

    return wl_client_put(&client_t, &cfg, ++seed, local, remote, force, 5000, r);
}

static wl_status get(const char *remote, const char *local, struct wl_file_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    static uint32_t seed = 900;

    return wl_client_get(&client_t, &cfg, ++seed, remote, local, 5000, r);
}

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_put_then_get_roundtrip(void **state)
{
    char src[300];
    char dst[300];
    char back[300];
    struct wl_file_result r;
    uint64_t size;
    uint64_t mtime;
    uint64_t size2;
    uint64_t mtime2;
    int is_dir;

    (void)state;
    path_in(src, sizeof(src), "source.txt");
    path_in(dst, sizeof(dst), "copy.txt");
    path_in(back, sizeof(back), "back.txt");
    write_file(src, 250000, 12345);

    assert_int_equal(put(src, dst, 0, &r), WL_OK);
    assert_true(r.ok);
    assert_int_equal(r.bytes, 250000);
    assert_same_file(src, dst);

    /* The modification time travels with the file, so sync can compare it. */
    assert_int_equal(wl_fs_stat(src, &size, &mtime, &is_dir), WL_OK);
    assert_int_equal(wl_fs_stat(dst, &size2, &mtime2, &is_dir), WL_OK);
    assert_int_equal(size, size2);
    assert_int_equal(mtime, mtime2);

    assert_int_equal(get(dst, back, &r), WL_OK);
    assert_true(r.ok);
    assert_same_file(src, back);
}

static void test_empty_file(void **state)
{
    char src[300];
    char dst[300];
    struct wl_file_result r;

    (void)state;
    path_in(src, sizeof(src), "empty");
    path_in(dst, sizeof(dst), "empty-copy");
    write_file(src, 0, 1);
    assert_int_equal(put(src, dst, 0, &r), WL_OK);
    assert_true(r.ok);
    assert_int_equal(r.bytes, 0);
    assert_same_file(src, dst);
}

static void test_binary_file(void **state)
{
    char src[300];
    char dst[300];
    struct wl_file_result r;
    FILE *f;

    (void)state;
    path_in(src, sizeof(src), "binary.bin");
    path_in(dst, sizeof(dst), "binary-copy.bin");
    f = fopen(src, "wb");
    assert_non_null(f);
    for (int i = 0; i < 70000; i++)
        assert_int_not_equal(fputc(i & 0xFF, f), EOF); /* every byte value, zeros included */
    assert_int_equal(fclose(f), 0);
    assert_int_equal(put(src, dst, 0, &r), WL_OK);
    assert_true(r.ok);
    assert_same_file(src, dst);
}

static void test_put_refuses_to_overwrite_without_force(void **state)
{
    char src[300];
    char dst[300];
    struct wl_file_result r;

    (void)state;
    path_in(src, sizeof(src), "a.txt");
    path_in(dst, sizeof(dst), "b.txt");
    write_file(src, 100, 7);
    write_file(dst, 50, 9);

    assert_int_equal(put(src, dst, 0, &r), WL_OK);
    assert_false(r.ok);
    assert_int_equal(r.fail_code, WL_E_EXISTS);
    /* The existing file is untouched. */
    {
        uint64_t size;
        uint64_t mtime;
        int is_dir;

        assert_int_equal(wl_fs_stat(dst, &size, &mtime, &is_dir), WL_OK);
        assert_int_equal(size, 50);
    }
    assert_int_equal(put(src, dst, 1, &r), WL_OK);
    assert_true(r.ok);
    assert_same_file(src, dst);
}

static void test_get_missing_file(void **state)
{
    char missing[300];
    char local[300];
    struct wl_file_result r;
    uint64_t size;
    uint64_t mtime;
    int is_dir;

    (void)state;
    path_in(missing, sizeof(missing), "nope.txt");
    path_in(local, sizeof(local), "nope-local.txt");
    assert_int_equal(get(missing, local, &r), WL_OK);
    assert_false(r.ok);
    assert_int_equal(r.fail_code, WL_E_NOT_FOUND);
    /* Nothing, not even a temporary file, is left behind. */
    assert_int_not_equal(wl_fs_stat(local, &size, &mtime, &is_dir), WL_OK);
}

static void test_get_directory_is_refused(void **state)
{
    char local[300];
    struct wl_file_result r;

    (void)state;
    path_in(local, sizeof(local), "dir-local");
    assert_int_equal(get(dir, local, &r), WL_OK);
    assert_false(r.ok);
    assert_int_equal(r.fail_code, WL_E_PATH);
}

static void test_put_to_bad_directory(void **state)
{
    char src[300];
    struct wl_file_result r;

    (void)state;
    path_in(src, sizeof(src), "src2.txt");
    write_file(src, 10, 3);
    assert_int_equal(put(src, "/nonexistent/dir/file.txt", 0, &r), WL_OK);
    assert_false(r.ok);
    assert_int_equal(r.fail_code, WL_E_ACCESS);
}

static void test_put_missing_local_file(void **state)
{
    struct wl_file_result r;
    char dst[300];

    (void)state;
    path_in(dst, sizeof(dst), "never.txt");
    assert_int_not_equal(put("/nonexistent/local.txt", dst, 0, &r), WL_OK);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_put_then_get_roundtrip, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_file, setup, teardown),
        cmocka_unit_test_setup_teardown(test_binary_file, setup, teardown),
        cmocka_unit_test_setup_teardown(test_put_refuses_to_overwrite_without_force, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_get_missing_file, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_directory_is_refused, setup, teardown),
        cmocka_unit_test_setup_teardown(test_put_to_bad_directory, setup, teardown),
        cmocka_unit_test_setup_teardown(test_put_missing_local_file, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
