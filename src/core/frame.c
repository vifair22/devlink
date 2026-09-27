#include <string.h>

#include "devlink/bytes.h"
#include "devlink/crc32.h"
#include "devlink/frame.h"

static int type_valid(uint8_t type)
{
    return type >= WL_FRAME_SYNC && type <= WL_FRAME_ACK;
}

wl_status wl_frame_encode(const struct wl_frame *f, uint8_t *out, size_t cap,
                          size_t *out_len)
{
    uint8_t body[WL_FRAME_MAX_BODY];
    size_t body_len;
    size_t cobs_len;
    wl_status st;

    if (!type_valid(f->type))
        return wl_fail(WL_ERR_INVAL, "frame: invalid type %u", (unsigned)f->type);
    if (f->payload_len > WL_SERIAL_MAX_PAYLOAD)
        return wl_fail(WL_ERR_INVAL, "frame: payload %zu exceeds %u", f->payload_len,
                       WL_SERIAL_MAX_PAYLOAD);
    if (f->payload_len > 0 && f->type == WL_FRAME_ACK)
        return wl_fail(WL_ERR_INVAL, "frame: ACK frames carry no payload");

    body[0] = f->type;
    wl_put_u16le(body + 1, f->seq);
    wl_put_u16le(body + 3, f->ack);
    wl_put_u32le(body + 5, f->sack);
    if (f->payload_len > 0)
        memcpy(body + WL_FRAME_HEADER, f->payload, f->payload_len);
    body_len = WL_FRAME_HEADER + f->payload_len;
    wl_put_u32le(body + body_len, wl_crc32(0, body, body_len));
    body_len += WL_FRAME_CRC;

    if (cap < 1)
        return wl_fail(WL_ERR_NOSPACE, "frame: no room for delimiter");
    st = wl_cobs_encode(body, body_len, out, cap - 1, &cobs_len);
    if (st != WL_OK)
        return st;
    out[cobs_len] = 0;
    *out_len = cobs_len + 1;
    return WL_OK;
}

void wl_frame_rx_init(struct wl_frame_rx *rx, size_t max_payload)
{
    memset(rx, 0, sizeof(*rx));
    rx->max_payload = max_payload < WL_SERIAL_MAX_PAYLOAD ? max_payload
                                                          : WL_SERIAL_MAX_PAYLOAD;
}

/* One complete delimited chunk: decode, validate, deliver or count. */
static void rx_frame(struct wl_frame_rx *rx, wl_frame_cb cb, void *ctx)
{
    struct wl_frame f;
    size_t body_len = 0; /* set by a successful decode; GCC's LTO cannot see that */
    uint32_t want;

    if (wl_cobs_decode(rx->wire, rx->wire_len, rx->body, sizeof(rx->body),
                       &body_len) != WL_OK ||
        body_len < WL_FRAME_MIN_BODY) {
        rx->stats.malformed++;
        return;
    }
    want = wl_get_u32le(rx->body + body_len - WL_FRAME_CRC);
    if (wl_crc32(0, rx->body, body_len - WL_FRAME_CRC) != want) {
        rx->stats.crc++;
        return;
    }

    f.type = rx->body[0];
    f.seq = wl_get_u16le(rx->body + 1);
    f.ack = wl_get_u16le(rx->body + 3);
    f.sack = wl_get_u32le(rx->body + 5);
    f.payload = rx->body + WL_FRAME_HEADER;
    f.payload_len = body_len - WL_FRAME_MIN_BODY;
    /* A valid CRC with a bad type or size means a peer bug or a protocol
     * mismatch rather than noise; it is still dropped, but counted apart
     * from CRC failures so the difference shows up in the stats. */
    if (!type_valid(f.type) || f.payload_len > rx->max_payload ||
        (f.type == WL_FRAME_ACK && f.payload_len > 0)) {
        rx->stats.malformed++;
        return;
    }
    rx->stats.frames++;
    cb(ctx, &f);
}

void wl_frame_rx_feed(struct wl_frame_rx *rx, const uint8_t *data, size_t n,
                      wl_frame_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];

        if (b == 0) {
            if (rx->discarding)
                rx->discarding = 0;
            else if (rx->wire_len > 0)
                rx_frame(rx, cb, ctx);
            /* An empty chunk (back-to-back delimiters) is idle filler. */
            rx->wire_len = 0;
            continue;
        }
        if (rx->discarding)
            continue;
        if (rx->wire_len == sizeof(rx->wire)) {
            rx->stats.overflow++;
            rx->discarding = 1;
            rx->wire_len = 0;
            continue;
        }
        rx->wire[rx->wire_len++] = b;
    }
}
