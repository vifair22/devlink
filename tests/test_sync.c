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
#include "w98link/fs.h"
#include "w98link/proto.h"
#include "w98link/sync.h"

/* Directory sync end to end: the real agent in a thread, the real client, a
 * pty between them, and two real directory trees. */

static struct {
    struct wl_transport t;
    struct wl_agent_cfg cfg;
    volatile int stop;
    wl_status result;
    pthread_t th;
} agent;

static struct wl_transport client_t;
static char base[200];
static char src_dir[240];
static char dst_dir[240];

static void *agent_main(void *arg)
{
    (void)arg;
    agent.result = wl_agent_serve(&agent.t, &agent.cfg, 0x5F17CU);
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

    cfg.link.max_payload = WL_SERIAL_MAX_PAYLOAD;
    cfg.link.window = WL_LINK_DEFAULT_WINDOW;
    cfg.link.bytes_per_sec = 0;
    cfg.level = WL_SESSION_DEFAULT_LEVEL;
    return cfg;
}

static int setup(void **state)
{
    (void)state;
    (void)snprintf(base, sizeof(base), "/tmp/w98link-sync-%d", (int)getpid());
    (void)snprintf(src_dir, sizeof(src_dir), "%s/src", base);
    (void)snprintf(dst_dir, sizeof(dst_dir), "%s/dst", base);
    assert_int_equal(mkdir(base, 0700), 0);
    assert_int_equal(mkdir(src_dir, 0700), 0);
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
    (void)snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    assert_int_equal(system(cmd), 0);
    return 0;
}

static void write_at(const char *dir, const char *rel, const char *text)
{
    char path[WL_PATH_MAX];
    char parent[WL_PATH_MAX];
    char *slash;
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", dir, rel);
    (void)snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    if (slash != NULL) {
        *slash = '\0';
        assert_int_equal(wl_fs_mkdir_p(parent), WL_OK);
    }
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(text, 1, strlen(text), f), strlen(text));
    assert_int_equal(fclose(f), 0);
}

static int exists(const char *dir, const char *rel)
{
    char path[WL_PATH_MAX];
    uint64_t size;
    uint64_t mtime;
    int is_dir;

    (void)snprintf(path, sizeof(path), "%s/%s", dir, rel);
    return wl_fs_stat(path, &size, &mtime, &is_dir) == WL_OK;
}

static void assert_contents(const char *dir, const char *rel, const char *want)
{
    char path[WL_PATH_MAX];
    char buf[4096];
    FILE *f;
    size_t n;

    (void)snprintf(path, sizeof(path), "%s/%s", dir, rel);
    f = fopen(path, "rb");
    assert_non_null(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    assert_int_equal(fclose(f), 0);
    assert_string_equal(buf, want);
}

static wl_status sync_now(uint8_t flags, struct wl_sync_result *r)
{
    struct wl_session_cfg cfg = fast_session();
    static uint32_t seed = 700;

    return wl_client_sync(&client_t, &cfg, ++seed, src_dir, dst_dir, flags, 5000, r);
}

/* ---------------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------------- */

static void test_first_sync_copies_everything(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "main.c", "int main(void) { return 0; }\n");
    write_at(src_dir, "include/app.h", "#define APP 1\n");
    write_at(src_dir, "deep/nested/dir/file.txt", "hello\n");

    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_true(r.ok);
    assert_int_equal(r.scanned, 3);
    assert_int_equal(r.needed, 3);
    assert_int_equal(r.written, 3);
    assert_int_equal(r.same, 0);
    assert_contents(dst_dir, "main.c", "int main(void) { return 0; }\n");
    assert_contents(dst_dir, "include/app.h", "#define APP 1\n");
    assert_contents(dst_dir, "deep/nested/dir/file.txt", "hello\n");
}

/* The point of the whole thing: a second sync with one changed file sends
 * one file, not the tree. */
static void test_second_sync_sends_only_changes(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "a.c", "one\n");
    write_at(src_dir, "b.c", "two\n");
    write_at(src_dir, "c.c", "three\n");
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.written, 3);

    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_true(r.ok);
    assert_int_equal(r.needed, 0);
    assert_int_equal(r.same, 3);
    assert_int_equal(r.bytes, 0);

    write_at(src_dir, "b.c", "two, edited\n");
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.needed, 1);
    assert_int_equal(r.written, 1);
    assert_int_equal(r.same, 2);
    assert_contents(dst_dir, "b.c", "two, edited\n");
}

/* Same contents, different timestamp: the agent hashes, finds them equal,
 * copies nothing and lines the timestamp up for next time. */
static void test_touched_file_is_not_resent(void **state)
{
    struct wl_sync_result r;
    char path[WL_PATH_MAX];

    (void)state;
    write_at(src_dir, "same.c", "unchanged\n");
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.written, 1);

    (void)snprintf(path, sizeof(path), "%s/same.c", src_dir);
    assert_int_equal(wl_fs_set_mtime(path, 1000000000U), WL_OK);
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.needed, 0);
    assert_int_equal(r.same, 1);

    {
        char remote[WL_PATH_MAX];
        uint64_t size;
        uint64_t mtime;
        int is_dir;

        (void)snprintf(remote, sizeof(remote), "%s/same.c", dst_dir);
        assert_int_equal(wl_fs_stat(remote, &size, &mtime, &is_dir), WL_OK);
        assert_int_equal(mtime, 1000000000U);
    }
}

static void test_dot_files_are_skipped_unless_asked(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "visible.c", "yes\n");
    write_at(src_dir, ".hidden", "no\n");
    write_at(src_dir, ".git/config", "no\n");

    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.scanned, 1);
    assert_false(exists(dst_dir, ".hidden"));
    assert_false(exists(dst_dir, ".git/config"));

    assert_int_equal(sync_now(WL_SYNC_HIDDEN, &r), WL_OK);
    assert_int_equal(r.scanned, 3);
    assert_true(exists(dst_dir, ".hidden"));
    assert_true(exists(dst_dir, ".git/config"));
}

static void test_delete_removes_extra_files(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "keep.c", "keep\n");
    assert_int_equal(sync_now(0, &r), WL_OK);
    write_at(dst_dir, "stale.c", "stale\n");
    write_at(dst_dir, "sub/also-stale.c", "stale\n");

    /* Without --delete the extra files stay. */
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.deleted, 0);
    assert_true(exists(dst_dir, "stale.c"));

    assert_int_equal(sync_now(WL_SYNC_DELETE, &r), WL_OK);
    assert_int_equal(r.deleted, 2);
    assert_false(exists(dst_dir, "stale.c"));
    assert_false(exists(dst_dir, "sub/also-stale.c"));
    assert_true(exists(dst_dir, "keep.c"));
}

static void test_dry_run_changes_nothing(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "new.c", "new\n");
    assert_int_equal(sync_now(WL_SYNC_DRY_RUN, &r), WL_OK);
    assert_int_equal(r.needed, 1);
    assert_int_equal(r.written, 0);
    assert_false(exists(dst_dir, "new.c"));

    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_true(exists(dst_dir, "new.c"));
}

/* A file whose size is unchanged but whose bytes differ must still be
 * detected: this is what the hash is for. */
static void test_same_size_different_contents(void **state)
{
    struct wl_sync_result r;

    (void)state;
    write_at(src_dir, "x.bin", "AAAAAAAA\n");
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_contents(dst_dir, "x.bin", "AAAAAAAA\n");

    /* Rewrite with the same length and force a different timestamp. */
    write_at(src_dir, "x.bin", "BBBBBBBB\n");
    {
        char path[WL_PATH_MAX];

        (void)snprintf(path, sizeof(path), "%s/x.bin", src_dir);
        assert_int_equal(wl_fs_set_mtime(path, 1400000000U), WL_OK);
    }
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.needed, 1);
    assert_contents(dst_dir, "x.bin", "BBBBBBBB\n");
}

static void test_larger_tree(void **state)
{
    struct wl_sync_result r;
    char rel[64];

    (void)state;
    for (int i = 0; i < 60; i++) {
        char text[128];

        (void)snprintf(rel, sizeof(rel), "mod%02d/file%02d.c", i % 6, i);
        (void)snprintf(text, sizeof(text), "/* file %d */\nint f%d(void) { return %d; }\n", i,
                       i, i);
        write_at(src_dir, rel, text);
    }
    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_true(r.ok);
    assert_int_equal(r.scanned, 60);
    assert_int_equal(r.written, 60);

    assert_int_equal(sync_now(0, &r), WL_OK);
    assert_int_equal(r.same, 60);
    assert_int_equal(r.needed, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_first_sync_copies_everything, setup, teardown),
        cmocka_unit_test_setup_teardown(test_second_sync_sends_only_changes, setup, teardown),
        cmocka_unit_test_setup_teardown(test_touched_file_is_not_resent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_dot_files_are_skipped_unless_asked, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_delete_removes_extra_files, setup, teardown),
        cmocka_unit_test_setup_teardown(test_dry_run_changes_nothing, setup, teardown),
        cmocka_unit_test_setup_teardown(test_same_size_different_contents, setup, teardown),
        cmocka_unit_test_setup_teardown(test_larger_tree, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
