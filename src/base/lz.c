#include "base/base.h"

/* Decoder for the byte-oriented LZ format of tools/lz/lz_encode.h (SZ1): embedded assets are compressed at build time
 * under EMBER_MIN_SIZE and expanded where they are used. Bounds-checked: a damaged stream fails, it does not overrun. */

static bool lz_len(const uint8_t **p, const uint8_t *end, size_t *len)
{
    uint8_t b;
    do {
        if (*p >= end) {
            return false;
        }
        b = *(*p)++;
        *len += b;
    } while (b == 255);
    return true;
}

bool embi_lz_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    const uint8_t *p = src, *end = src + n;
    size_t o = 0;
    while (p < end) {
        unsigned token = *p++;
        size_t lit = token >> 4;
        if (lit == 15 && !lz_len(&p, end, &lit)) {
            return false;
        }
        if (lit > (size_t)(end - p) || lit > cap - o) {
            return false;
        }
        if (lit != 0) {
            memcpy(dst + o, p, lit);
        }
        p += lit;
        o += lit;
        if (o == cap && p == end) {
            return true;
        }
        if (end - p < 2) {
            return false;
        }
        size_t off = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
        size_t len = token & 15;
        if (len == 15 && !lz_len(&p, end, &len)) {
            return false;
        }
        len += 4;
        if (off == 0 || off > o || len > cap - o) {
            return false;
        }
        const uint8_t *from = dst + o - off;
        uint8_t *to = dst + o;
        if (off >= len) {
            memcpy(to, from, len);
        } else {
            for (size_t i = 0; i < len; i++) { /* overlapping copy repeats the pattern */
                to[i] = from[i];
            }
        }
        o += len;
    }
    return o == cap;
}

size_t embi_lz_raw_size(const uint8_t *packed, size_t n)
{
    if (n < 4) {
        return 0;
    }
    return (size_t)packed[0] | ((size_t)packed[1] << 8) | ((size_t)packed[2] << 16) | ((size_t)packed[3] << 24);
}

uint8_t *embi_lz_unpack(const uint8_t *packed, size_t n, size_t *raw_size)
{
    if (n < 4) {
        return nullptr;
    }
    size_t raw = embi_lz_raw_size(packed, n);
    /* A stream gives at most 255 bytes per input byte (a length extension byte adds 255): a header beyond that is damaged,
     * and must not make us allocate up to 4 GB for it. */
    if (raw > (n - 4) * 255 + 64) {
        return nullptr;
    }
    uint8_t *out = embi_alloc(raw + 1);
    if (!embi_lz_decode(packed + 4, n - 4, out, raw)) {
        embi_free(out, raw + 1);
        return nullptr;
    }
    out[raw] = 0;
    if (raw_size != nullptr) {
        *raw_size = raw;
    }
    return out;
}
