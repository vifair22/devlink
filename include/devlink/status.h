#ifndef DEVLINK_STATUS_H
#define DEVLINK_STATUS_H

/*
 * Every fallible function returns a wl_status. The deepest function that
 * detects a failure records a message with wl_fail(); callers propagate the
 * code without overwriting it, so the message names the real cause.
 */
typedef enum {
    WL_OK = 0,
    WL_ERR_INVAL = -1,   /* caller passed an invalid argument */
    WL_ERR_NOSPACE = -2, /* output buffer too small */
    WL_ERR_CORRUPT = -3, /* malformed data from the peer */
    WL_ERR_NOMEM = -4,
    WL_ERR_IO = -5
} wl_status;

#ifdef __GNUC__
#define WL_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define WL_PRINTF(fmt, args)
#endif

/* Records the message for the calling thread and returns code, so failure
 * sites read `return wl_fail(WL_ERR_X, "...", ...);`. */
wl_status wl_fail(wl_status code, const char *fmt, ...) WL_PRINTF(2, 3);

/* The last message recorded on this thread, or "" if none. */
const char *wl_error(void);

#endif
