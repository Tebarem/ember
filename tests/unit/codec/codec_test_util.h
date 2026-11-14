#pragma once
/*
 * Helpers shared by the codec tests: fixture loading (EMBER_SOURCE_DIR based), CRC-32, a fixed-seed
 * PRNG for the fuzz-style tests, big-endian readers, sfnt table lookup, and bit writers for
 * hand-built DEFLATE and Brotli streams. Everything is static inline, so a test that does not use a
 * helper does not warn.
 */
#include "emb_test.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include "codec/codec.h"

/* Loads a file relative to the source root. Returns nullptr on failure. A zero-length file may
 * also return nullptr; callers treat a zero length as valid. */
static inline uint8_t *tu_load(const char *rel, size_t *len)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    *len = 0;
    return (uint8_t *)SDL_LoadFile(path, len);
}

static inline uint32_t tu_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline uint32_t tu_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint32_t tu_be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/* Finds a table in a single sfnt (or the first font of a collection). */
static inline bool tu_sfnt_table(const uint8_t *f, size_t n, uint32_t tag, const uint8_t **data, size_t *len)
{
    if (n < 12) {
        return false;
    }
    uint32_t nt = tu_be16(f + 4);
    if ((uint64_t)12 + 16ull * nt > n) {
        return false;
    }
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + 12 + 16 * (size_t)i;
        if (tu_be32(e) == tag) {
            uint32_t off = tu_be32(e + 8);
            uint32_t l = tu_be32(e + 12);
            if (off > n || l > n - off) {
                return false;
            }
            *data = f + off;
            *len = l;
            return true;
        }
    }
    return false;
}

static inline uint32_t tu_tag(const char *s)
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) |
           (uint32_t)(uint8_t)s[3];
}

/* Fixed-seed PRNG (xorshift32). Never zero. */
typedef struct tu_rng {
    uint32_t s;
} tu_rng;

static inline uint32_t tu_rand(tu_rng *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* LSB-first bit writer (DEFLATE integers and Brotli). Fixed capacity; tests stay small. */
typedef struct tu_bits {
    uint8_t buf[4096];
    size_t len;
    unsigned acc;
    unsigned nacc;
} tu_bits;

static inline void tu_put_bits(tu_bits *w, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        w->acc |= ((value >> i) & 1u) << w->nacc;
        w->nacc++;
        if (w->nacc == 8) {
            w->buf[w->len++] = (uint8_t)w->acc;
            w->acc = 0;
            w->nacc = 0;
        }
    }
}

/* Huffman codes go in most-significant bit first (DEFLATE and Brotli prefix codes). */
static inline void tu_put_code(tu_bits *w, uint32_t code, unsigned len)
{
    for (unsigned i = len; i-- > 0;) {
        tu_put_bits(w, (code >> i) & 1u, 1);
    }
}

static inline size_t tu_bits_finish(tu_bits *w)
{
    if (w->nacc) {
        w->buf[w->len++] = (uint8_t)w->acc;
        w->acc = 0;
        w->nacc = 0;
    }
    return w->len;
}

/* DEFLATE fixed Huffman literal/length code (RFC 1951 3.2.6). */
static inline void tu_put_fixed_litlen(tu_bits *w, unsigned sym)
{
    if (sym < 144) {
        tu_put_code(w, 0x30u + sym, 8);
    } else if (sym < 256) {
        tu_put_code(w, 0x190u + (sym - 144), 9);
    } else if (sym < 280) {
        tu_put_code(w, sym - 256, 7);
    } else {
        tu_put_code(w, 0xC0u + (sym - 280), 8);
    }
}

#define TU_MIN(a, b) ((a) < (b) ? (a) : (b))
