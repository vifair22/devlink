#include <stdio.h>
#include <string.h>

#include "devlink/kdf.h"
#include "devlink/keyfile.h"

wl_status wl_key_load(const char *path, uint8_t psk[WL_NOISE_KEYLEN])
{
    char line[512];
    size_t n;
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return wl_fail(WL_ERR_INVAL, "no key file at %s", path);
    if (fgets(line, (int)sizeof(line), f) == NULL)
        line[0] = '\0';
    (void)fclose(f);
    /* fgets leaves a terminator inside the buffer, so n is always in it; the
     * bound is spelled out for the analyzer, which cannot see that. */
    n = strcspn(line, "\r\n");
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    line[n] = '\0';
    if (n == 0) {
        wl_wipe(line, sizeof(line));
        return wl_fail(WL_ERR_CORRUPT, "%s holds no passphrase on its first line", path);
    }
    wl_noise_psk_from_passphrase(line, n, psk);
    wl_wipe(line, sizeof(line));
    return WL_OK;
}
