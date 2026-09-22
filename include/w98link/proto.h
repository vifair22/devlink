#ifndef W98LINK_PROTO_H
#define W98LINK_PROTO_H

/*
 * Constants from docs/protocol.md, above the compression layer. Kept in one
 * header so the numbers on the wire exist in exactly one place.
 */

#define WL_PROTO_VERSION 1U

/* Mux record header: u16 channel, u8 type, u16 length. */
#define WL_REC_HEADER 5U
#define WL_REC_MAX_PAYLOAD 16384U
/* Bulk payloads are cut to this so one channel cannot hog the line. */
#define WL_REC_CHUNK 4096U

#define WL_CONTROL_CHANNEL 0U

/* Control channel record types. */
enum {
    WL_CTL_HELLO = 1,
    WL_CTL_ERROR = 2,
    WL_CTL_OPEN = 3,
    WL_CTL_OPEN_OK = 4,
    WL_CTL_OPEN_FAIL = 5,
    WL_CTL_CLOSE = 6,
    WL_CTL_WINDOW = 7,
    WL_CTL_PING = 8,
    WL_CTL_PONG = 9
};

/* Channel kinds, sent in OPEN. */
enum { WL_KIND_EXEC = 1, WL_KIND_PUT = 2, WL_KIND_GET = 3, WL_KIND_SYNC = 4 };

/* EXEC channel record types. */
enum {
    WL_EXEC_STDIN = 1,
    WL_EXEC_STDIN_EOF = 2,
    WL_EXEC_STDOUT = 3,
    WL_EXEC_STDERR = 4,
    WL_EXEC_EXIT = 5
};
#define WL_EXEC_MERGE_STDERR 0x01U

/* HELLO platform byte. */
enum { WL_PLATFORM_WIN9X = 1, WL_PLATFORM_WINNT = 2, WL_PLATFORM_LINUX = 3 };

/* Error codes for ERROR, OPEN_FAIL and FAIL. */
enum {
    WL_E_VERSION = 1,
    WL_E_MALFORMED = 2,
    WL_E_UNKNOWN = 3,
    WL_E_CREDIT = 4,
    WL_E_NOT_FOUND = 256,
    WL_E_ACCESS = 257,
    WL_E_EXISTS = 258,
    WL_E_DISK_FULL = 259,
    WL_E_CRC = 260,
    WL_E_PATH = 261,
    WL_E_PROCESS = 262
};

/* Per-channel flow control (docs/protocol.md, "Flow control"). */
#define WL_CREDIT_INITIAL 65536U
#define WL_CREDIT_TOPUP 32768U /* grant more once this much has been consumed */

#endif
