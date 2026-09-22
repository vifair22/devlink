#include "w98link/cobs.h"

wl_status wl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                         size_t *out_len)
{
    size_t code_pos = 0;
    size_t o = 1;
    uint8_t code = 1;

    if (cap < WL_COBS_MAX_ENCODED(n))
        return wl_fail(WL_ERR_NOSPACE, "cobs: %zu-byte buffer for %zu bytes", cap, n);

    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos = o++;
            code = 1;
            continue;
        }
        out[o++] = in[i];
        code++;
        /* A full 254-byte group needs no implied zero. Only open a new group
         * if input remains, so the output is the canonical encoding rather
         * than one with a redundant trailing 0x01. */
        if (code == 0xFF && i + 1 < n) {
            out[code_pos] = code;
            code_pos = o++;
            code = 1;
        }
    }
    out[code_pos] = code;
    *out_len = o;
    return WL_OK;
}

wl_status wl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                         size_t *out_len)
{
    size_t i = 0;
    size_t o = 0;

    while (i < n) {
        uint8_t code = in[i++];
        size_t run;

        if (code == 0)
            return wl_fail(WL_ERR_CORRUPT, "cobs: zero code byte at %zu", i - 1);
        run = (size_t)code - 1U;
        if (run > n - i)
            return wl_fail(WL_ERR_CORRUPT, "cobs: group of %zu truncated at %zu", run, n);
        if (run > cap - o)
            return wl_fail(WL_ERR_NOSPACE, "cobs: output exceeds %zu bytes", cap);
        for (size_t k = 0; k < run; k++) {
            if (in[i] == 0)
                return wl_fail(WL_ERR_CORRUPT, "cobs: zero data byte at %zu", i);
            out[o++] = in[i++];
        }
        /* Every group except a full one and the last implies a zero. */
        if (code != 0xFF && i < n) {
            if (o >= cap)
                return wl_fail(WL_ERR_NOSPACE, "cobs: output exceeds %zu bytes", cap);
            out[o++] = 0;
        }
    }
    *out_len = o;
    return WL_OK;
}
