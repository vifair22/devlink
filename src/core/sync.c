#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w98link/clock.h"
#include "w98link/crc32.h"
#include "w98link/fs.h"
#include "w98link/proto.h"
#include "w98link/sha256.h"
#include "w98link/sync.h"
#include "w98link/wire.h"

#define PUMP_MS 50U
#define CHUNK 4096U
#define LINGER_MS 2000U
/* Entries per MANIFEST record: enough to keep the overhead small, small
 * enough that a record stays well inside the 16 KiB limit. */
#define BATCH 40U

enum phase { PH_CONNECT, PH_OPENING, PH_MANIFEST, PH_WAITING, PH_SENDING, PH_DONE, PH_FAILED };

struct entry {
    char rel[WL_PATH_MAX];
    uint64_t size;
    uint64_t mtime;
    uint8_t hash[WL_HASH_BYTES];
};

struct sync {
    struct wl_peer *peer;
    struct wl_sync_result *out;
    const char *root; /* local directory */
    enum phase phase;
    uint16_t ch;
    int sessions;

    struct entry *entries; /* the local tree */
    size_t n_entries;
    size_t cap_entries;
    size_t sent_entries; /* how many have gone into MANIFEST records */

    char (*need)[WL_PATH_MAX]; /* what the agent asked for */
    size_t n_need;
    size_t cap_need;
    size_t sending; /* index of the file being sent */
    int done_sent;  /* the client has said there are no more files */
    FILE *f;
    uint64_t file_left;
    uint32_t file_crc;
    int need_end;
};

/* Temporary tracing, enabled with W98LINK_DEBUG=1. */
static int trace_on(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("W98LINK_DEBUG") != NULL;
    return on;
}

#define TRACE(...)                                                                       \
    do {                                                                                 \
        if (trace_on()) {                                                                \
            fprintf(stderr, "[sync] ");                                                  \
            fprintf(stderr, __VA_ARGS__);                                                \
            fprintf(stderr, "\n");                                                       \
        }                                                                                \
    } while (0)

static void fail_msg(struct sync *s, uint16_t code, const char *msg, size_t n)
{
    if (n >= sizeof(s->out->message))
        n = sizeof(s->out->message) - 1;
    memcpy(s->out->message, msg, n);
    s->out->message[n] = '\0';
    s->out->fail_code = code;
    s->phase = PH_FAILED;
}

/* ---------------------------------------------------------------------------
 * collecting the local tree
 * ------------------------------------------------------------------------- */

static int collect(void *ctx, const char *rel, uint64_t size, uint64_t mtime)
{
    struct sync *s = ctx;
    struct entry *e;
    char full[WL_PATH_MAX];
    uint8_t digest[32];

    if (strlen(rel) >= WL_PATH_MAX)
        return 1; /* skipped: it could not be named on the other side */
    if (s->n_entries == s->cap_entries) {
        size_t cap = s->cap_entries == 0 ? 256 : s->cap_entries * 2;
        struct entry *grown = realloc(s->entries, cap * sizeof(*grown));

        if (grown == NULL)
            return 0;
        s->entries = grown;
        s->cap_entries = cap;
    }
    e = &s->entries[s->n_entries];
    if (!wl_fs_join(full, sizeof(full), s->root, rel))
        return 1;
    if (!wl_sha256_file(full, digest))
        return 1; /* unreadable: leave it out rather than claim to sync it */
    memcpy(e->rel, rel, strlen(rel) + 1);
    e->size = size;
    e->mtime = mtime;
    memcpy(e->hash, digest, WL_HASH_BYTES);
    s->n_entries++;
    return 1;
}

/* ---------------------------------------------------------------------------
 * peer handlers
 * ------------------------------------------------------------------------- */

static void on_session(void *ctx)
{
    struct sync *s = ctx;

    if (++s->sessions > 1 && s->phase != PH_DONE)
        fail_msg(s, 0, "link reset during the sync", 26);
}

static void on_opened(void *ctx, uint16_t ch, int ok, uint16_t code, const char *msg, size_t n)
{
    struct sync *s = ctx;

    if (ch != s->ch || s->phase != PH_OPENING)
        return;
    if (!ok) {
        fail_msg(s, code, msg, n);
        return;
    }
    s->phase = PH_MANIFEST;
}

static void on_record(void *ctx, uint16_t ch, uint8_t type, const uint8_t *p, size_t n)
{
    struct sync *s = ctx;
    struct wl_rbuf r;

    if (ch != s->ch)
        return;
    wl_rbuf_init(&r, p, n);
    switch (type) {
    case WL_SYNC_NEED: {
        uint16_t count = wl_r_u16(&r);

        for (uint16_t i = 0; i < count; i++) {
            size_t len;
            const char *rel = wl_r_str(&r, &len);

            if (r.bad || len >= WL_PATH_MAX)
                break;
            if (s->n_need == s->cap_need) {
                size_t cap = s->cap_need == 0 ? 64 : s->cap_need * 2;
                char (*grown)[WL_PATH_MAX] = realloc(s->need, cap * WL_PATH_MAX);

                if (grown == NULL) {
                    fail_msg(s, 0, "out of memory for the file list", 31);
                    return;
                }
                s->need = grown;
                s->cap_need = cap;
            }
            memcpy(s->need[s->n_need], rel, len);
            s->need[s->n_need][len] = '\0';
            s->n_need++;
        }
        return;
    }
    case WL_SYNC_NEED_END:
        TRACE("NEED_END");
        s->out->same = wl_r_u32(&r);
        s->out->deleted = wl_r_u32(&r);
        s->need_end = 1;
        return;
    case WL_SYNC_FILE_FAIL: {
        size_t len;
        const char *rel = wl_r_str(&r, &len);
        uint16_t code = wl_r_u16(&r);
        size_t mlen;
        const char *msg = wl_r_str(&r, &mlen);

        s->out->failed++;
        /* Only the first failure is kept; the count says how many there were. */
        if (s->out->message[0] == '\0' && !r.bad)
            (void)snprintf(s->out->message, sizeof(s->out->message), "%.*s: %.*s",
                           (int)len, rel, (int)mlen, msg);
        (void)code;
        return;
    }
    case WL_SYNC_SUMMARY:
        TRACE("SUMMARY");
        s->out->written = wl_r_u32(&r);
        s->out->failed = wl_r_u32(&r);
        s->phase = PH_DONE;
        return;
    default:
        return;
    }
}

static void on_closed(void *ctx, uint16_t ch)
{
    struct sync *s = ctx;

    if (ch == s->ch && s->phase != PH_DONE)
        fail_msg(s, 0, "the agent closed the sync early", 31);
}

static void on_error(void *ctx, uint16_t code, const char *msg, size_t n)
{
    fail_msg(ctx, code, msg, n);
}

/* ---------------------------------------------------------------------------
 * sending
 * ------------------------------------------------------------------------- */

static void send_manifest(struct sync *s)
{
    uint8_t buf[WL_REC_MAX_PAYLOAD];

    while (s->sent_entries < s->n_entries) {
        struct wl_wbuf w;
        size_t count = s->n_entries - s->sent_entries;

        if (count > BATCH)
            count = BATCH;
        wl_wbuf_init(&w, buf, sizeof(buf));
        wl_w_u16(&w, (uint16_t)count);
        for (size_t i = 0; i < count; i++) {
            const struct entry *e = &s->entries[s->sent_entries + i];

            wl_w_str(&w, e->rel, strlen(e->rel));
            wl_w_u64(&w, e->size);
            wl_w_u64(&w, e->mtime);
            wl_w_bytes(&w, e->hash, WL_HASH_BYTES);
        }
        if (w.overflow) {
            fail_msg(s, 0, "a manifest batch did not fit", 28);
            return;
        }
        if (wl_peer_send(s->peer, s->ch, WL_SYNC_MANIFEST, buf, w.len) != WL_OK)
            return; /* the queue is full; carry on next time round */
        s->sent_entries += count;
    }
    if (wl_peer_send(s->peer, s->ch, WL_SYNC_MANIFEST_END, NULL, 0) == WL_OK) {
        TRACE("manifest sent: %u entries", (unsigned)s->n_entries);
        s->phase = PH_WAITING;
    }
}

/* Streams the files the agent asked for, one after another. */
static void send_files(struct sync *s)
{
    uint8_t buf[CHUNK];

    while (s->sending < s->n_need) {
        char full[WL_PATH_MAX];
        uint64_t size;
        uint64_t mtime;
        int is_dir;

        if (s->f == NULL) {
            struct wl_wbuf w;
            uint8_t head[WL_PATH_MAX + 32];

            if (!wl_fs_join(full, sizeof(full), s->root, s->need[s->sending])) {
                s->sending++;
                continue;
            }
            if (wl_fs_stat(full, &size, &mtime, &is_dir) != WL_OK || is_dir) {
                /* It vanished since the manifest was built. */
                s->sending++;
                continue;
            }
            wl_wbuf_init(&w, head, sizeof(head));
            wl_w_str(&w, s->need[s->sending], strlen(s->need[s->sending]));
            wl_w_u64(&w, size);
            wl_w_u64(&w, mtime);
            if (wl_peer_send(s->peer, s->ch, WL_SYNC_FILE, head, w.len) != WL_OK)
                return;
            s->f = fopen(full, "rb");
            if (s->f == NULL) {
                s->sending++;
                continue;
            }
            s->file_left = size;
            s->file_crc = 0;
        }
        while (s->file_left > 0) {
            size_t want = s->file_left < CHUNK ? (size_t)s->file_left : CHUNK;
            size_t got = fread(buf, 1, want, s->f);
            size_t taken;

            /* The size came from stat before the file was opened; a short
             * read means it changed underneath us. */
            if (got != want) {
                fail_msg(s, 0, "a file changed while it was being sent", 38);
                return;
            }
            taken = wl_peer_write(s->peer, s->ch, WL_SYNC_DATA, buf, got);
            s->file_crc = wl_crc32(s->file_crc, buf, taken);
            s->file_left -= taken;
            s->out->bytes += taken;
            if (taken < got) {
                /* Only part was accepted: rewind to match. */
                if (fseek(s->f, -(long)(got - taken), SEEK_CUR) != 0)
                    fail_msg(s, 0, "cannot rewind a file being sent", 31);
                return;
            }
        }
        {
            uint8_t crc[4];

            wl_put_u32le(crc, s->file_crc);
            if (wl_peer_send(s->peer, s->ch, WL_SYNC_FILE_END, crc, sizeof(crc)) != WL_OK)
                return;
        }
        (void)fclose(s->f);
        s->f = NULL;
        s->sending++;
    }
    if (wl_peer_send(s->peer, s->ch, WL_SYNC_DONE, NULL, 0) == WL_OK) {
        TRACE("DONE sent after %u file(s)", (unsigned)s->sending);
        s->done_sent = 1;
    }
}

wl_status wl_client_sync(struct wl_transport *t, const struct wl_session_cfg *cfg,
                         uint32_t seed, const char *local_root, const char *remote_root,
                         uint8_t flags, uint32_t connect_timeout_ms,
                         struct wl_sync_result *out)
{
    struct sync s;
    struct wl_peer_handlers h;
    uint8_t args[WL_PATH_MAX + 8];
    struct wl_wbuf w;
    uint32_t start = wl_clock_ms();
    wl_status st;

    memset(out, 0, sizeof(*out));
    memset(&s, 0, sizeof(s));
    s.out = out;
    s.root = local_root;
    st = wl_fs_walk(local_root, (flags & WL_SYNC_HIDDEN) != 0, collect, &s);
    if (st != WL_OK) {
        free(s.entries);
        return st;
    }
    out->scanned = (uint32_t)s.n_entries;

    wl_wbuf_init(&w, args, sizeof(args));
    wl_w_str(&w, remote_root, strlen(remote_root));
    wl_w_u8(&w, flags & (WL_SYNC_DELETE | WL_SYNC_DRY_RUN));
    if (w.overflow) {
        free(s.entries);
        return wl_fail(WL_ERR_INVAL, "path too long: %s", remote_root);
    }

    memset(&h, 0, sizeof(h));
    h.ctx = &s;
    h.session = on_session;
    h.opened = on_opened;
    h.record = on_record;
    h.closed = on_closed;
    h.error = on_error;
    s.peer = wl_peer_new(cfg, seed, wl_clock_ms(), 1, WL_PLATFORM_LINUX, &h);
    if (s.peer == NULL) {
        free(s.entries);
        return WL_ERR_NOMEM;
    }

    st = WL_OK;
    while (s.phase != PH_DONE && s.phase != PH_FAILED) {
        st = wl_peer_pump(s.peer, t, PUMP_MS);
        if (st != WL_OK)
            break;
        switch (s.phase) {
        case PH_CONNECT:
            if (wl_peer_ready(s.peer)) {
                st = wl_peer_open(s.peer, WL_KIND_SYNC, args, w.len, &s.ch);
                if (st != WL_OK)
                    goto out_done;
                s.phase = PH_OPENING;
            } else if (wl_clock_since(start) >= connect_timeout_ms) {
                st = wl_fail(WL_ERR_IO, "no answer from the agent within %u s",
                             (unsigned)(connect_timeout_ms / 1000U));
                goto out_done;
            }
            break;
        case PH_MANIFEST:
            send_manifest(&s);
            break;
        case PH_WAITING:
            if (s.need_end) {
                out->needed = (uint32_t)s.n_need;
                if ((flags & WL_SYNC_DRY_RUN) != 0)
                    s.sending = s.n_need; /* nothing to send; just finish */
                s.phase = PH_SENDING;
            }
            break;
        case PH_SENDING:
            if (!s.done_sent)
                send_files(&s);
            break;
        default:
            break;
        }
    }
out_done:
    if (st == WL_OK) {
        uint32_t linger = wl_clock_ms();

        while (wl_peer_close(s.peer, s.ch) == WL_ERR_NOSPACE &&
               wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(s.peer, t, PUMP_MS);
        while (!wl_peer_idle(s.peer) && wl_clock_since(linger) < LINGER_MS)
            (void)wl_peer_pump(s.peer, t, PUMP_MS);
    }
    if (s.f != NULL)
        (void)fclose(s.f);
    wl_peer_free(s.peer);
    free(s.entries);
    free(s.need);
    out->ok = s.phase == PH_DONE && out->failed == 0;
    out->elapsed_ms = wl_clock_since(start);
    return st;
}
