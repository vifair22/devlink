#ifndef W98LINK_FRAME_H
#define W98LINK_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "w98link/cobs.h"
#include "w98link/status.h"

/*
 * Serial link frames (docs/protocol.md, "Link layer: serial"). This module
 * only turns frames into bytes and back; sequencing, acks and retransmission
 * belong to the link state machine above it.
 */

enum wl_frame_type {
    WL_FRAME_SYNC = 1,
    WL_FRAME_SYNC_ACK = 2,
    WL_FRAME_DATA = 3,
    WL_FRAME_ACK = 4
};

/* type u8, seq u16, ack u16, sack u32 */
#define WL_FRAME_HEADER 9U
#define WL_FRAME_CRC 4U
#define WL_FRAME_MIN_BODY (WL_FRAME_HEADER + WL_FRAME_CRC)

/* Largest payload this build accepts on serial. The negotiated max_payload
 * can only be smaller, so buffers are sized from this once. */
#define WL_SERIAL_MAX_PAYLOAD 1024U
#define WL_FRAME_MAX_BODY (WL_FRAME_MIN_BODY + WL_SERIAL_MAX_PAYLOAD)
/* COBS output plus the 0x00 delimiter. */
#define WL_FRAME_MAX_WIRE (WL_COBS_MAX_ENCODED(WL_FRAME_MAX_BODY) + 1U)

struct wl_frame {
    uint8_t type;
    uint16_t seq;
    uint16_t ack;
    uint32_t sack;
    const uint8_t *payload; /* borrowed; valid only for the call it is seen in */
    size_t payload_len;
};

/* Encodes f as COBS(body) followed by the delimiter. out needs
 * WL_FRAME_MAX_WIRE bytes for a full-size payload. */
wl_status wl_frame_encode(const struct wl_frame *f, uint8_t *out, size_t cap,
                          size_t *out_len);

/*
 * Receive side. Bytes arrive from the serial port in arbitrary chunks and may
 * include line noise; the receiver accumulates up to each delimiter and
 * delivers only frames that decode and pass the CRC. Everything else is
 * dropped and counted, never reported as an error: the sender's retransmit
 * timer is what recovers a lost frame.
 */
struct wl_frame_rx_stats {
    uint32_t frames;    /* delivered */
    uint32_t crc;       /* dropped: CRC mismatch */
    uint32_t malformed; /* dropped: bad COBS, too short, bad type or length */
    uint32_t overflow;  /* dropped: no delimiter within WL_FRAME_MAX_WIRE bytes */
};

struct wl_frame_rx {
    uint8_t wire[WL_FRAME_MAX_WIRE];
    size_t wire_len;
    int discarding; /* overflowed; skip to the next delimiter */
    uint8_t body[WL_FRAME_MAX_BODY];
    size_t max_payload;
    struct wl_frame_rx_stats stats;
};

typedef void (*wl_frame_cb)(void *ctx, const struct wl_frame *f);

/* max_payload is the negotiated limit; larger payloads count as malformed. */
void wl_frame_rx_init(struct wl_frame_rx *rx, size_t max_payload);

void wl_frame_rx_feed(struct wl_frame_rx *rx, const uint8_t *data, size_t n,
                      wl_frame_cb cb, void *ctx);

#endif
