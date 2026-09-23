#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devlink/clock.h"
#include "devlink/crc32.h"
#include "devlink/file_ops.h"
#include "devlink/fs.h"
#include "devlink/proto.h"
#include "devlink/wire.h"

#define PUMP_MS 50U
#define CHUNK 4096U
#define LINGER_MS 2000U

enum phase { PH_CONNECT, PH_OPENING, PH_RUNNING, PH_DONE, PH_FAILED };

struct xfer {
    struct wl_peer *peer;
    struct wl_file_result *out;
    enum phase phase;
    uint16_t ch;
    int is_put;
    FILE *f;
    char tmp[WL_PATH_MAX + 8]; /* GET: where the file is written first */
    const char *local; /* GET: final name */
    uint64_t size;
    uint64_t mtime;
    uint64_t done;
    uint32_t crc;
    int sent_end;
    int sessions;
};

static void fail_msg(struct xfer *x, uint16_t code, const char *msg, size_t n)
{
    if (n >= sizeof(x->out->message))
        n = sizeof(x->out->message) - 1;
    memcpy(x->out->message, msg, n);
    x->out->message[n] = '\0';
    x->out->fail_code = code;
    x->phase = PH_FAILED;
}

static void on_session(void *ctx)
{
    struct xfer *x = ctx;

    if (++x->sessions > 1 && x->phase != PH_DONE)
        fail_msg(x, 0, "link reset during the transfer", 30);
}

static void on_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n)
{
    struct xfer *x = ctx;

    if (ch != x->ch || x->phase != PH_OPENING)
        return;
    if (!ok) {
        fail_msg(x, code, msg, n);
        return;
    }
    x->phase = PH_RUNNING;
}

/* GET: the first record is META, then the file's bytes. */
static void get_record(struct xfer *x, uint8_t type, const uint8_t *p, size_t n)
{
    if (type == WL_FILE_META && n == 16) {
        x->size = wl_get_u64le(p);
        x->mtime = wl_get_u64le(p + 8);
        x->f = fopen(x->tmp, "wb");
        if (x->f == NULL)
            fail_msg(x, WL_E_ACCESS, "cannot create the local file", 28);
        return;
    }
    if (x->f == NULL)
        return;
    if (type == WL_FILE_DATA) {
        if (n > 0 && fwrite(p, 1, n, x->f) != n) {
            fail_msg(x, WL_E_DISK_FULL, "cannot write the local file", 27);
            return;
        }
        x->crc = wl_crc32(x->crc, p, n);
        x->done += n;
    } else if (type == WL_FILE_END && n == 4) {
        uint32_t want = wl_get_u32le(p);

        if (fclose(x->f) != 0) {
            x->f = NULL;
            fail_msg(x, WL_E_DISK_FULL, "cannot finish the local file", 28);
            return;
        }
        x->f = NULL;
        if (x->done != x->size || x->crc != want) {
            fail_msg(x, WL_E_CRC, "checksum mismatch", 17);
            return;
        }
        if (wl_fs_replace(x->tmp, x->local) != WL_OK) {
            fail_msg(x, WL_E_ACCESS, wl_error(), strlen(wl_error()));
            return;
        }
        x->tmp[0] = '\0';
        if (x->mtime != 0)
            (void)wl_fs_set_mtime(x->local, x->mtime);
        x->out->bytes = x->done;
        x->phase = PH_DONE;
    } else if (type == WL_FILE_FAIL) {
        struct wl_rbuf r;
        uint16_t code;
        size_t len;
        const char *msg;

        wl_rbuf_init(&r, p, n);
        code = wl_r_u16(&r);
        msg = wl_r_str(&r, &len);
        fail_msg(x, code, msg != NULL ? msg : "transfer failed", msg != NULL ? len : 15);
    }
}

static void on_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct xfer *x = ctx;

    if (ch != x->ch)
        return;
    if (!x->is_put) {
        get_record(x, type, p, n);
        return;
    }
    if (type == WL_FILE_DONE) {
        x->out->bytes = x->done;
        x->phase = PH_DONE;
    } else if (type == WL_FILE_FAIL) {
        struct wl_rbuf r;
        uint16_t code;
        size_t len;
        const char *msg;

        wl_rbuf_init(&r, p, n);
        code = wl_r_u16(&r);
        msg = wl_r_str(&r, &len);
        fail_msg(x, code, msg != NULL ? msg : "transfer failed", msg != NULL ? len : 15);
    }
}

static void on_closed(void *ctx, uint16_t ch)
{
    struct xfer *x = ctx;

    if (ch == x->ch && x->phase == PH_RUNNING)
        fail_msg(x, 0, "the agent closed the transfer early", 35);
}

static void on_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    fail_msg(ctx, code, msg, n);
}

/* PUT: feed the file into the channel as the mux accepts it. */
static void push_file(struct xfer *x)
{
    uint8_t buf[CHUNK];

    while (x->done < x->size) {
        size_t want = x->size - x->done < CHUNK ? (size_t)(x->size - x->done) : CHUNK;
        size_t got = fread(buf, 1, want, x->f);
        size_t sent;

        if (got == 0) {
            fail_msg(x, WL_E_ACCESS, "cannot read the local file", 26);
            return;
        }
        sent = wl_peer_write(x->peer, x->ch, WL_FILE_DATA, buf, got);
        x->crc = wl_crc32(x->crc, buf, sent);
        x->done += sent;
        if (sent < got) {
            /* The mux took only part of it; the rest is read again next
             * time round, so the file position follows what was sent. */
            if (fseek(x->f, (long)(x->done), SEEK_SET) != 0)
                fail_msg(x, WL_E_ACCESS, "cannot seek the local file", 26);
            return;
        }
    }
    if (!x->sent_end) {
        uint8_t crc[4];

        wl_put_u32le(crc, x->crc);
        if (wl_peer_send(x->peer, x->ch, WL_FILE_END, crc, sizeof(crc)) == WL_OK)
            x->sent_end = 1;
    }
}

static wl_status run(struct wl_transport *t, const struct wl_session_cfg *cfg, uint32_t seed,
                     struct xfer *x, uint8_t kind, const uint8_t *args, size_t args_n,
                     uint32_t connect_timeout_ms)
{
    struct wl_peer_handlers h;
    uint32_t start = wl_clock_ms();
    wl_status st = WL_OK;

    memset(&h, 0, sizeof(h));
    h.ctx = x;
    h.session = on_session;
    h.opened = on_opened;
    h.record = on_record;
    h.closed = on_closed;
    h.error = on_error;
    x->peer = wl_peer_new(cfg, seed, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    if (x->peer == NULL)
        return WL_ERR_NOMEM;

    while (x->phase != PH_DONE && x->phase != PH_FAILED) {
        st = wl_peer_pump(x->peer, t, PUMP_MS);
        if (st != WL_OK)
            break;
        if (x->phase == PH_CONNECT) {
            if (wl_peer_ready(x->peer)) {
                st = wl_peer_open(x->peer, kind, args, args_n, &x->ch);
                if (st != WL_OK)
                    break;
                x->phase = PH_OPENING;
            } else if (wl_clock_since(start) >= connect_timeout_ms) {
                st = wl_fail(WL_ERR_IO, "no answer from the agent within %u s",
                             (unsigned)(connect_timeout_ms / 1000U));
                break;
            }
        } else if (x->phase == PH_RUNNING && x->is_put) {
            push_file(x);
        }
    }
    if (st == WL_OK) {
        uint32_t linger = wl_clock_ms();

        while (wl_peer_close(x->peer, x->ch) == WL_ERR_NOSPACE &&
               wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(x->peer, t, PUMP_MS);
        while (!wl_peer_idle(x->peer) && wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(x->peer, t, PUMP_MS);
    }
    wl_peer_free(x->peer);
    if (x->f != NULL)
        (void)fclose(x->f);
    /* A failed GET leaves nothing behind. */
    if (!x->is_put && x->tmp[0] != '\0' && x->phase != PH_DONE)
        (void)wl_fs_remove(x->tmp);
    x->out->ok = x->phase == PH_DONE;
    return st;
}

wl_status wl_client_put(struct wl_transport *t, const struct wl_session_cfg *cfg,
                        uint32_t seed, const char *local, const char *remote, int overwrite,
                        uint32_t connect_timeout_ms, struct wl_file_result *out)
{
    struct xfer x;
    uint8_t args[WL_REC_MAX_PAYLOAD];
    struct wl_wbuf w;
    uint64_t size;
    uint64_t mtime;
    uint32_t start;
    int is_dir;
    wl_status st;

    memset(out, 0, sizeof(*out));
    memset(&x, 0, sizeof(x));
    x.out = out;
    x.is_put = 1;
    if (wl_fs_stat(local, &size, &mtime, &is_dir) != WL_OK)
        return WL_ERR_IO;
    if (is_dir)
        return wl_fail(WL_ERR_INVAL, "%s is a directory", local);
    x.f = fopen(local, "rb");
    if (x.f == NULL)
        return wl_fail(WL_ERR_IO, "cannot read %s", local);
    x.size = size;

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, remote, strlen(remote));
    wl_w_u64(&w, size);
    wl_w_u64(&w, mtime);
    wl_w_u8(&w, overwrite ? WL_PUT_OVERWRITE : 0);
    if (w.overflow) {
        (void)fclose(x.f);
        return wl_fail(WL_ERR_INVAL, "path too long: %s", remote);
    }
    start = wl_clock_ms();
    st = run(t, cfg, seed, &x, WL_KIND_PUT, args, w.len, connect_timeout_ms);
    out->elapsed_ms = wl_clock_since(start);
    return st;
}

wl_status wl_client_get(struct wl_transport *t, const struct wl_session_cfg *cfg,
                        uint32_t seed, const char *remote, const char *local,
                        uint32_t connect_timeout_ms, struct wl_file_result *out)
{
    struct xfer x;
    uint8_t args[WL_REC_MAX_PAYLOAD];
    struct wl_wbuf w;
    uint32_t start;
    wl_status st;

    memset(out, 0, sizeof(*out));
    memset(&x, 0, sizeof(x));
    x.out = out;
    x.local = local;
    if (wl_fs_temp_name(local, x.tmp, sizeof(x.tmp)) != WL_OK)
        return WL_ERR_NOSPACE;

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, remote, strlen(remote));
    if (w.overflow)
        return wl_fail(WL_ERR_INVAL, "path too long: %s", remote);
    start = wl_clock_ms();
    st = run(t, cfg, seed, &x, WL_KIND_GET, args, w.len, connect_timeout_ms);
    out->elapsed_ms = wl_clock_since(start);
    return st;
}
