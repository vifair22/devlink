#include <stdarg.h>
#include <stdio.h>

#include "devlink/status.h"

/* Watcom and MSVC spell thread-local storage their own way. __declspec(thread)
 * is safe here because it is only used in the EXE, never a DLL, which is the
 * case Win9x does not support. */
#if defined(__WATCOMC__) || defined(_MSC_VER)
#define WL_THREAD_LOCAL __declspec(thread)
#else
#define WL_THREAD_LOCAL _Thread_local
#endif

static WL_THREAD_LOCAL char error_buf[256];

wl_status wl_fail(wl_status code, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(error_buf, sizeof(error_buf), fmt, ap);
    va_end(ap);
    return code;
}

const char *wl_error(void)
{
    return error_buf;
}
