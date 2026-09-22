#include <stdio.h>
#include <string.h>

#include "w98link/version.h"

static void usage(FILE *out)
{
    fprintf(out, "usage: w98link --version\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("w98link %s\n", wl_version());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    usage(stderr);
    return 1;
}
