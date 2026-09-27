/*
 * The two numbers docs/plan.md step 9g sets for TCP, measured where they
 * matter: on the target's CPU.
 *
 *   TCPBENCH [FILE]
 *
 * - How long deriving the key from a passphrase takes at the protocol's
 *   iteration count (WL_NOISE_PBKDF2_ITERATIONS). The protocol wants about a
 *   second on the target: slow enough to make guessing expensive, quick
 *   enough that nobody waits on an agent starting.
 * - How fast deflate runs at each level on FILE (default: this program,
 *   which is at least real machine code). Over TCP the line is no longer the
 *   bottleneck; the CPU compressing for it is, so the level wants to be the
 *   best ratio the CPU can still keep ahead of the network.
 *
 * Built for Win32 with Watcom (make tcpbench) and for Linux alike, from the
 * same core sources the agents use. It is a tool, not part of either program,
 * and in no gate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "devlink/clock.h"
#include "devlink/noise.h"

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    (void)fclose(f);
    *len = (size_t)n;
    return buf;
}

/* One raw-deflate stream over the whole input, as a session compresses:
 * returns the compressed size, and the time taken in *ms. */
static unsigned long deflate_once(const unsigned char *in, size_t n, int level, uint32_t *ms)
{
    static unsigned char out[1 << 16];
    z_stream z;
    unsigned long total = 0;
    uint32_t t0 = wl_clock_ms();

    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    do {
        z.next_out = out;
        z.avail_out = sizeof(out);
        (void)deflate(&z, Z_FINISH);
        total += (unsigned long)(sizeof(out) - z.avail_out);
    } while (z.avail_out == 0);
    (void)deflateEnd(&z);
    *ms = wl_clock_since(t0);
    return total;
}

int main(int argc, char **argv)
{
    uint8_t psk[WL_NOISE_KEYLEN];
    const char *path = argc > 1 ? argv[1] : argv[0];
    unsigned char *data;
    size_t len = 0;
    uint32_t t0;
    uint32_t ms;

    t0 = wl_clock_ms();
    wl_noise_psk_from_passphrase("a passphrase of ordinary length", 31, psk);
    ms = wl_clock_since(t0);
    printf("pbkdf2: %u iterations in %lu ms\n", (unsigned)WL_NOISE_PBKDF2_ITERATIONS,
           (unsigned long)ms);

    data = read_all(path, &len);
    if (data == NULL) {
        fprintf(stderr, "tcpbench: cannot read %s\n", path);
        return 1;
    }
    printf("deflate over %s, %lu bytes, one stream:\n", path, (unsigned long)len);
    for (int level = 1; level <= 9; level++) {
        unsigned long out = deflate_once(data, len, level, &ms);
        unsigned long kbs = ms > 0 ? (unsigned long)(len / ms) : 0UL;

        printf("  level %d: %lu bytes (%lu%%), %lu ms, %lu KB/s in\n", level, out,
               len > 0 ? (unsigned long)(out * 100UL / len) : 0UL, (unsigned long)ms, kbs);
    }
    free(data);
    return 0;
}
