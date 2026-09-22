#include "w98link/version.h"

/* The build defines this; the fallback only exists so an IDE indexer that
 * does not know the Makefile's flags still parses the file. */
#ifndef W98LINK_VERSION
#define W98LINK_VERSION "0.0.0_unknown"
#endif

const char *wl_version(void)
{
    return W98LINK_VERSION;
}
