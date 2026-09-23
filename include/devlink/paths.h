#ifndef DEVLINK_PATHS_H
#define DEVLINK_PATHS_H

#include <stddef.h>

/*
 * Pure path arithmetic, so the parts that can be wrong are the parts the tests
 * reach. Nothing here touches a filesystem or knows which one it is: both
 * separators are treated as separators, since a Windows path may carry either.
 */

/*
 * The file beside this one with the same name and a different extension:
 * "C:\DEVLINK\DEVLINKW.EXE" and ".NEW" gives "C:\DEVLINK\DEVLINKW.NEW".
 *
 * A name with no extension gains one. A dot in a directory is not an
 * extension, which is the case that makes this worth writing down rather than
 * reaching for strrchr at the call site. Zero if it would not fit, and then
 * out is left empty rather than half written.
 */
int wl_path_sibling(const char *path, const char *ext, char *out, size_t cap);

#endif
