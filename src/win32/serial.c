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
