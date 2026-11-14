#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_TAG_HEAD 0x68656164u /* 'head' */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int cmp_tag(const void *a, const void *b)
{
    uint32_t x = ((const embi_sfnt_table *)a)->tag;
    uint32_t y = ((const embi_sfnt_table *)b)->tag;
    return (x > y) - (x < y);
}

uint32_t embi_sfnt_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        sum += ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) |
               (uint32_t)p[i + 3];
    }
    if (i < len) {
        uint32_t word = 0;
        for (size_t k = 0; i + k < len; k++) {
            word |= (uint32_t)p[i + k] << (24 - 8 * k);
        }
        sum += word;
    }
    return sum;
}

bool embi_sfnt_build(uint32_t flavor, const embi_sfnt_table *tables, size_t n, embi_buf *out)
{
    if (n == 0 || n > 0xffffu || out->len != 0) {
        return false;
    }

    /* Sorted copy of the directory. Tags must be unique. */
    embi_sfnt_table *t = embi_alloc(n * sizeof *t);
    SDL_memcpy(t, tables, n * sizeof *t);
    SDL_qsort(t, n, sizeof *t, cmp_tag);
    bool ok = true;
    for (size_t i = 1; i < n; i++) {
        if (t[i].tag == t[i - 1].tag) {
            ok = false;
        }
    }

    uint64_t *offs = ok ? embi_alloc(n * sizeof *offs) : nullptr;
    uint64_t total = 12 + 16 * (uint64_t)n;
    if (ok) {
        for (size_t i = 0; i < n; i++) {
            offs[i] = total;
            total += ((uint64_t)t[i].len + 3) & ~(uint64_t)3;
        }
        if (total > UINT32_MAX) {
            ok = false;
        }
    }

    if (ok) {
        ok = embi_buf_reserve(out, (size_t)total);
    }
    if (ok) {
        out->len = (size_t)total;
        SDL_memset(out->data, 0, out->len);

        unsigned entry_selector = 0;
        while ((2u << entry_selector) <= n) {
            entry_selector++;
        }
        uint32_t search_range = (1u << entry_selector) * 16u;
        uint8_t *hdr = out->data;
        put32(hdr, flavor);
        put16(hdr + 4, (uint32_t)n);
        put16(hdr + 6, search_range);
        put16(hdr + 8, entry_selector);
        put16(hdr + 10, (uint32_t)n * 16u - search_range);

        for (size_t i = 0; i < n; i++) {
            uint8_t *dst = out->data + offs[i];
            if (t[i].len) {
                SDL_memcpy(dst, t[i].data, t[i].len);
            }
            if (t[i].tag == EMBI_TAG_HEAD && t[i].len >= 12) {
                SDL_memset(dst + 8, 0, 4); /* checkSumAdjustment is 0 while summing */
            }
            uint8_t *e = out->data + 12 + 16 * i;
            put32(e, t[i].tag);
            put32(e + 4, embi_sfnt_checksum(dst, t[i].len));
            put32(e + 8, (uint32_t)offs[i]);
            put32(e + 12, (uint32_t)t[i].len);
        }

        /* Whole-file checksum with the adjustment field zero, then store the adjustment. */
        for (size_t i = 0; i < n; i++) {
            if (t[i].tag == EMBI_TAG_HEAD && t[i].len >= 12) {
                uint32_t sum = embi_sfnt_checksum(out->data, out->len);
                put32(out->data + offs[i] + 8, 0xB1B0AFBAu - sum);
                break;
            }
        }
    }

    if (offs) {
        embi_free(offs, n * sizeof *offs);
    }
    embi_free(t, n * sizeof *t);
    return ok;
}
