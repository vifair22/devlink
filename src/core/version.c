#include "devlink/version.h"

/* The build defines this; the fallback only exists so an IDE indexer that
 * does not know the Makefile's flags still parses the file. */
#ifndef DEVLINK_VERSION
#define DEVLINK_VERSION "0.0.0_unknown"
#endif

const char *wl_version(void)
{
    return DEVLINK_VERSION;
}
