#include <windows.h>
#include <wincrypt.h>

#include "devlink/random.h"

/* CryptoAPI, which Windows 98 and 2000 both ship (advapi32). RtlGenRandom
 * would avoid the context, but it arrived with XP. CRYPT_VERIFYCONTEXT asks
 * for no key container: only the generator is wanted, and without it the
 * call fails for a user who has never had a key set created. */
wl_status wl_random_bytes(uint8_t *buf, size_t n)
{
    HCRYPTPROV prov;
    size_t done = 0;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return wl_fail(WL_ERR_IO, "random: CryptAcquireContext failed (error %lu)",
                       (unsigned long)GetLastError());
    while (done < n) {
        DWORD k = n - done > 0x10000U ? 0x10000U : (DWORD)(n - done);

        if (!CryptGenRandom(prov, k, buf + done)) {
            DWORD err = GetLastError();

            (void)CryptReleaseContext(prov, 0);
            return wl_fail(WL_ERR_IO, "random: CryptGenRandom failed (error %lu)",
                           (unsigned long)err);
        }
        done += k;
    }
    (void)CryptReleaseContext(prov, 0);
    return WL_OK;
}
