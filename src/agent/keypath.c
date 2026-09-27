#include <string.h>
#include <windows.h>

#include "keypath.h"

int agent_key_path(char *out, size_t cap)
{
    static const char name[] = "DEVLINK.KEY";
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *slash;

    if (n == 0 || n >= cap)
        return 0;
    slash = strrchr(out, '\\');
    slash = slash != NULL ? slash + 1 : out;
    if ((size_t)(slash - out) + sizeof(name) > cap)
        return 0;
    memcpy(slash, name, sizeof(name));
    return 1;
}
