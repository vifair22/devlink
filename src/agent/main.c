#include <stdio.h>
#include <windows.h>

#include "w98link/version.h"

/*
 * Win98 agent entry point. For now it only proves the cross-build: it prints
 * its version and the OS it finds itself on, which is the first thing to
 * check when a binary built on Linux lands on the A21m.
 */
int main(void)
{
    OSVERSIONINFOA vi;

    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA(&vi)) {
        fprintf(stderr, "w98agent: GetVersionEx failed (%lu)\n", GetLastError());
        return 1;
    }
    printf("w98agent %s\n", wl_version());
    printf("windows platform %lu, %lu.%lu build %lu\n", vi.dwPlatformId,
           vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber & 0xFFFFu);
    return 0;
}
