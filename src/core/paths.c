#include <string.h>

#include "devlink/paths.h"

int wl_path_sibling(const char *path, const char *ext, char *out, size_t cap)
{
    size_t n;
    size_t stem;
    size_t i;

    if (out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
    if (path == NULL || ext == NULL)
        return 0;
    n = strlen(path);
    stem = n;
    /* Backwards to the last dot, stopping at a separator: a dot passed on the
     * way up from a directory name is part of the directory, not this file. */
    for (i = n; i > 0; i--) {
        char c = path[i - 1];

        if (c == '/' || c == '\\')
            break;
        if (c == '.') {
            stem = i - 1;
            break;
        }
    }
    if (stem + strlen(ext) >= cap)
        return 0;
    memcpy(out, path, stem);
    memcpy(out + stem, ext, strlen(ext) + 1);
    return 1;
}
