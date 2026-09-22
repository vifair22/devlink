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
    WL_EXEC_EXIT = 5,
    WL_EXEC_CANCEL = 6 /* client: abandon this command, kill it over there */
};
#define WL_EXEC_MERGE_STDERR 0x01U
/* The exit status the agent reports for a command it killed on CANCEL. The
 * shell convention for a command interrupted by SIGINT, which is what the
 * client's Ctrl-C means. */
#define WL_EXEC_EXIT_CANCELLED 130U

/* PUT and GET channel record types. */
enum {
    WL_FILE_DATA = 1,
    WL_FILE_END = 2,  /* u32 crc32 of the whole file */
    WL_FILE_DONE = 3, /* written, checked and in place */
    WL_FILE_FAIL = 4, /* u16 code, str message */
    WL_FILE_META = 5  /* GET only, from the agent: u64 size, u64 mtime */
};
#define WL_PUT_OVERWRITE 0x01U

/* SYNC channel record types. */
enum {
    WL_SYNC_MANIFEST = 1,     /* client: u16 count, then that many entries */
    WL_SYNC_MANIFEST_END = 2, /* client: no payload */
    WL_SYNC_NEED = 3,         /* agent: u16 count, then that many str paths */
    WL_SYNC_NEED_END = 4,     /* agent: u32 same, u32 deleted */
    WL_SYNC_FILE = 5,         /* client: str rel, u64 size, u64 mtime */
    WL_SYNC_DATA = 6,         /* client: bytes */
    WL_SYNC_FILE_END = 7,     /* client: u32 crc32 */
    WL_SYNC_FILE_FAIL = 8,    /* agent: str rel, u16 code, str message */
    WL_SYNC_DONE = 9,         /* client: no more files */
    WL_SYNC_SUMMARY = 10      /* agent: u32 written, u32 failed */
};
/* One manifest entry: str rel, u64 size, u64 mtime, WL_HASH_BYTES of hash. */
#define WL_SYNC_DELETE 0x01U  /* remove files the manifest does not list */
#define WL_SYNC_DRY_RUN 0x02U /* compare and report, change nothing */

/* FAT stores times with two-second resolution, so times this close are the
 * same time. */
#define WL_MTIME_SLACK 2U

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

/* The longest path either side will accept, on the wire or on disk. */
#define WL_PATH_MAX 260U

/* Per-channel flow control (docs/protocol.md, "Flow control"). */
#define WL_CREDIT_INITIAL 65536U
#define WL_CREDIT_TOPUP 32768U /* grant more once this much has been consumed */

#endif
