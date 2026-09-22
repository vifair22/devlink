#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "w98link/transport.h"

/*
 * Win32 serial transport, synchronous I/O. Overlapped I/O on comm handles
 * works on both 9x and NT, but nothing needs it yet: the read timeout below
 * gives the caller its tick, which is all the link layer asks for.
 */
struct com_impl {
    HANDLE h;
    DWORD read_timeout; /* last value given to SetCommTimeouts */
};

/* ReadIntervalTimeout = ReadTotalTimeoutMultiplier = MAXDWORD with a finite
 * constant is the documented combination for "return as soon as any byte is
 * available, or after the constant if none arrives". Setting it is a driver
 * call, so it is skipped when the timeout has not changed. */
static int set_read_timeout(struct com_impl *c, DWORD ms)
{
    COMMTIMEOUTS to;

    if (ms == 0)
        ms = 1; /* 0 would mean "never time out" in this combination */
    if (ms == c->read_timeout)
        return 1;
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 0;
    if (!SetCommTimeouts(c->h, &to))
        return 0;
    c->read_timeout = ms;
    return 1;
}

static wl_status com_read(void *impl, uint8_t *buf, size_t cap, size_t *got,
                          uint32_t timeout_ms)
{
    struct com_impl *c = impl;
    DWORD n = 0;

    *got = 0;
    if (!set_read_timeout(c, (DWORD)timeout_ms))
        return wl_fail(WL_ERR_IO, "serial: SetCommTimeouts failed (%lu)", GetLastError());
    if (!ReadFile(c->h, buf, (DWORD)cap, &n, NULL))
        return wl_fail(WL_ERR_IO, "serial: ReadFile failed (%lu)", GetLastError());
    *got = (size_t)n;
    return WL_OK;
}

static wl_status com_write(void *impl, const uint8_t *buf, size_t len)
{
    struct com_impl *c = impl;

    while (len > 0) {
        DWORD n = 0;
        DWORD chunk = len > 0x10000U ? 0x10000U : (DWORD)len;

        if (!WriteFile(c->h, buf, chunk, &n, NULL))
            return wl_fail(WL_ERR_IO, "serial: WriteFile failed (%lu)", GetLastError());
        if (n == 0)
            return wl_fail(WL_ERR_IO, "serial: WriteFile wrote nothing");
        buf += n;
        len -= n;
    }
    return WL_OK;
}

static void com_close(void *impl)
{
    struct com_impl *c = impl;

    CloseHandle(c->h);
    free(c);
}

static const struct wl_transport_ops com_ops = {com_read, com_write, com_close};

wl_status wl_serial_open(const char *path, uint32_t baud, struct wl_transport *out)
{
    struct com_impl *c;
    DCB dcb;
    HANDLE h;

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return wl_fail(WL_ERR_IO, "serial: cannot open %s (%lu); is another program "
                       "(Kermit, HyperTerminal) holding it?", path, GetLastError());

    /* Driver queues well above one window of frames, so a burst from the
     * peer is not lost while the main loop is busy compressing. */
    (void)SetupComm(h, 16384, 16384);

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        wl_status st = wl_fail(WL_ERR_IO, "serial: GetCommState %s (%lu)", path, GetLastError());
        CloseHandle(h);
        return st;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        wl_status st = wl_fail(WL_ERR_IO, "serial: SetCommState %s at %lu (%lu)", path,
                               (unsigned long)baud, GetLastError());
        CloseHandle(h);
        return st;
    }
    (void)PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    c = malloc(sizeof(*c));
    if (c == NULL) {
        CloseHandle(h);
        return wl_fail(WL_ERR_NOMEM, "serial: out of memory");
    }
    c->h = h;
    c->read_timeout = 0;
    out->ops = &com_ops;
    out->impl = c;
    return WL_OK;
}
