#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "devlink/transport.h"

/*
 * Win32 serial transport, synchronous I/O. Overlapped I/O on comm handles
 * works on both 9x and NT, but nothing needs it yet: the read timeout below
 * gives the caller its tick, which is all the link layer asks for.
 *
 * A port that fails is reopened rather than reported. After a standby or
 * hibernate, Windows 2000's serial.sys answers every call on a handle opened
 * before it with ERROR_INVALID_FUNCTION (seen on the A21m, 2026-09-26), while
 * a fresh open of the same port works. Returning the error would end the
 * agent's session and every command running under it; instead the handle is
 * closed, reopened with the same settings, and the gap looks to the link
 * layer like a line that went quiet, which it already recovers from by
 * retransmitting. A port that will not reopen (a USB adapter unplugged, or
 * another program holding it) is retried every REOPEN_MS for as long as it
 * takes. The count and the last error are in the caps output.
 */
#define REOPEN_MS 1000U

struct com_impl {
    HANDLE h;           /* INVALID_HANDLE_VALUE while the port is lost */
    DWORD read_timeout; /* last value given to SetCommTimeouts; 0 after an open */
    char path[64];
    uint32_t baud;
    DWORD lost_at;       /* GetTickCount of the last failed reopen */
    unsigned long lost;  /* times the port has been lost */
    DWORD last_error;    /* the error that lost it most recently */
};

static wl_status open_port(const char *path, uint32_t baud, HANDLE *out);

/* The port has failed: drop the handle, so the next read reopens it. */
static void lose(struct com_impl *c)
{
    c->last_error = GetLastError();
    c->lost++;
    CloseHandle(c->h);
    c->h = INVALID_HANDLE_VALUE;
    c->lost_at = GetTickCount() - REOPEN_MS; /* the first reopen is immediate */
}

/* Nonzero when the port is open, reopening it if the last try is REOPEN_MS
 * old. A failed reopen leaves its message in the error buffer, which nothing
 * reads: the caller reports a quiet line, not an error. */
static int have_port(struct com_impl *c)
{
    if (c->h != INVALID_HANDLE_VALUE)
        return 1;
    if (GetTickCount() - c->lost_at < REOPEN_MS)
        return 0;
    if (open_port(c->path, c->baud, &c->h) != WL_OK) {
        c->h = INVALID_HANDLE_VALUE;
        c->lost_at = GetTickCount();
        return 0;
    }
    c->read_timeout = 0;
    return 1;
}

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
    if (!have_port(c)) {
        /* Keep the caller's tick while there is no port to wait on. */
        Sleep(timeout_ms > REOPEN_MS ? REOPEN_MS : (DWORD)timeout_ms);
        return WL_OK;
    }
    if (!set_read_timeout(c, (DWORD)timeout_ms) || !ReadFile(c->h, buf, (DWORD)cap, &n, NULL)) {
        lose(c);
        return WL_OK;
    }
    *got = (size_t)n;
    return WL_OK;
}

static wl_status com_write(void *impl, const uint8_t *buf, size_t len)
{
    struct com_impl *c = impl;

    /* Bytes written to a lost port are dropped, as a noisy line would drop
     * them; the link layer resends what the peer does not acknowledge. */
    if (!have_port(c))
        return WL_OK;
    while (len > 0) {
        DWORD n = 0;
        DWORD chunk = len > 0x10000U ? 0x10000U : (DWORD)len;

        if (!WriteFile(c->h, buf, chunk, &n, NULL)) {
            lose(c);
            return WL_OK;
        }
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

    if (c->h != INVALID_HANDLE_VALUE)
        CloseHandle(c->h);
    free(c);
}

/*
 * What the driver says it can do, asked of the open port so nothing has to be
 * changed to find out. This is the question worth answering before raising the
 * line rate: a plain 16550A clocked at 1.8432 MHz divides down to 115200 at
 * best (a divisor of 1), so anything faster needs a Super I/O high-speed mode
 * that the stock driver does not enable. dwSettableBaud is the driver's own
 * answer rather than an assumption about the chip.
 */
static const struct {
    DWORD flag;
    const char *name;
} baud_flags[] = {
    {BAUD_075, "75"},         {BAUD_110, "110"},       {BAUD_134_5, "134.5"},
    {BAUD_150, "150"},        {BAUD_300, "300"},       {BAUD_600, "600"},
    {BAUD_1200, "1200"},      {BAUD_1800, "1800"},     {BAUD_2400, "2400"},
    {BAUD_4800, "4800"},      {BAUD_7200, "7200"},     {BAUD_9600, "9600"},
    {BAUD_14400, "14400"},    {BAUD_19200, "19200"},   {BAUD_38400, "38400"},
    {BAUD_56K, "56000"},      {BAUD_57600, "57600"},   {BAUD_115200, "115200"},
    {BAUD_128K, "128000"},    {BAUD_USER, "any (BAUD_USER)"}
};

static size_t add_rates(char *buf, size_t at, size_t cap, const char *label, DWORD mask)
{
    at += (size_t)snprintf(buf + at, cap - at, "%s", label);
    for (size_t i = 0; i < sizeof(baud_flags) / sizeof(baud_flags[0]) && at < cap; i++) {
        if ((mask & baud_flags[i].flag) != 0)
            at += (size_t)snprintf(buf + at, cap - at, " %s", baud_flags[i].name);
    }
    if (at < cap)
        at += (size_t)snprintf(buf + at, cap - at, "\n");
    return at;
}

static wl_status com_caps(void *impl, char *out, size_t cap)
{
    struct com_impl *c = impl;
    char buf[1024];
    COMMPROP pr;
    DCB dcb;
    size_t at = 0;
    size_t n;

    if (!have_port(c))
        return wl_fail(WL_ERR_IO, "caps: %s is lost (error %lu), reopening", c->path,
                       c->last_error);
    memset(&pr, 0, sizeof(pr));
    pr.wPacketLength = sizeof(pr);
    if (!GetCommProperties(c->h, &pr))
        return wl_fail(WL_ERR_IO, "caps: GetCommProperties failed (%lu)", GetLastError());
    at += (size_t)snprintf(buf + at, sizeof(buf) - at, "transport serial (win32)\n");
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(c->h, &dcb))
        at += (size_t)snprintf(buf + at, sizeof(buf) - at, "current %lu %u%c%u\n",
                               dcb.BaudRate, (unsigned)dcb.ByteSize,
                               dcb.Parity == NOPARITY ? 'N' : 'P',
                               dcb.StopBits == ONESTOPBIT ? 1U : 2U);
    at += (size_t)snprintf(buf + at, sizeof(buf) - at, "subtype %s\n",
                           (pr.dwProvSubType & 0xFFU) == PST_RS232 ? "RS232" : "other");
    at += (size_t)snprintf(buf + at, sizeof(buf) - at, "buffers rx %lu tx %lu\n",
                           pr.dwCurrentRxQueue, pr.dwCurrentTxQueue);
    at = add_rates(buf, at, sizeof(buf), "maxbaud", pr.dwMaxBaud);
    at = add_rates(buf, at, sizeof(buf), "settable", pr.dwSettableBaud);
    if (at < sizeof(buf))
        at += (size_t)snprintf(buf + at, sizeof(buf) - at, "reopened %lu (last error %lu)\n",
                               c->lost, c->last_error);
    if (at >= sizeof(buf))
        return wl_fail(WL_ERR_NOSPACE, "caps: description did not fit");
    n = strlen(buf);
    if (n + 1 > cap)
        return wl_fail(WL_ERR_NOSPACE, "caps: buffer too small");
    memcpy(out, buf, n + 1);
    return WL_OK;
}

/* No descriptor to wait on: Win32 has no poll, and the agent has only ever one
 * transport, so nothing needs it. */
static const struct wl_transport_ops com_ops = {com_read, com_write, com_close, com_caps,
                                                NULL};

static wl_status open_port(const char *path, uint32_t baud, HANDLE *out)
{
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
    *out = h;
    return WL_OK;
}

wl_status wl_serial_open(const char *path, uint32_t baud, struct wl_transport *out)
{
    struct com_impl *c;
    wl_status st;

    if (strlen(path) >= sizeof(c->path))
        return wl_fail(WL_ERR_INVAL, "serial: port name too long: %s", path);
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return wl_fail(WL_ERR_NOMEM, "serial: out of memory");
    st = open_port(path, baud, &c->h);
    if (st != WL_OK) {
        free(c);
        return st;
    }
    memcpy(c->path, path, strlen(path) + 1);
    c->baud = baud;
    out->ops = &com_ops;
    out->impl = c;
    return WL_OK;
}
