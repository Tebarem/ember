#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

/* WOFF 1.0: each table is zlib-compressed when compLength < origLength, stored otherwise. The
 * output is an sfnt with the directory sorted by tag and every checksum recomputed. Stored tables
 * must lie after the directory without overlapping. Like deployed decoders, directory order is not
 * enforced (tags are sorted on output; duplicates fail) and totalSfntSize is only a hint: the size
 * actually produced is bounded by max_out. */

#define WOFF1_SIG 0x774F4646u /* 'wOFF' */
#define WOFF1_HEADER 44u
#define WOFF1_ENTRY 20u

typedef struct woff1_ent {
    uint32_t tag;
    uint32_t off;
    uint32_t comp;
    uint32_t orig;
} woff1_ent;

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static int cmp_off(const void *a, const void *b)
{
    uint32_t x = ((const woff1_ent *)a)->off;
    uint32_t y = ((const woff1_ent *)b)->off;
    return (x > y) - (x < y);
}

bool embi_woff1_decode(const uint8_t *in, size_t in_len, size_t max_out, embi_buf *out)
{
    out->len = 0;
    if (in_len < WOFF1_HEADER || rd32(in) != WOFF1_SIG) {
        return false;
    }
    uint32_t flavor = rd32(in + 4);
    uint32_t length = rd32(in + 8);
    uint32_t n = rd16(in + 12);
    if (length > in_len || n == 0 || (uint64_t)length < WOFF1_HEADER + (uint64_t)WOFF1_ENTRY * n) {
        return false;
    }

    woff1_ent *ents = embi_calloc(n, sizeof *ents);
    embi_sfnt_table *tabs = embi_calloc(n, sizeof *tabs);
    embi_buf *owned = embi_calloc(n, sizeof *owned);
    bool ok = true;
    const uint64_t dir_end = WOFF1_HEADER + (uint64_t)WOFF1_ENTRY * n;
    /* The sfnt keeps the offset table and directory plus every table padded to 4 bytes. */
    uint64_t sum_orig = 12 + 16 * (uint64_t)n;

    /* Directory: bounds, the header/directory as the lowest bytes, and the output size bound. */
    for (uint32_t i = 0; i < n && ok; i++) {
        const uint8_t *e = in + WOFF1_HEADER + (size_t)WOFF1_ENTRY * i;
        woff1_ent *t = &ents[i];
        t->tag = rd32(e);
        t->off = rd32(e + 4);
        t->comp = rd32(e + 8);
        t->orig = rd32(e + 12);
        if (t->off < dir_end || (uint64_t)t->off + t->comp > length || t->comp > t->orig) {
            ok = false;
            break;
        }
        sum_orig += ((uint64_t)t->orig + 3) & ~(uint64_t)3;
        if (t->orig > max_out || sum_orig > max_out) {
            ok = false;
            break;
        }
    }

    /* Stored tables must not overlap. Sorting by offset makes it a single pass. */
    if (ok) {
        SDL_qsort(ents, n, sizeof *ents, cmp_off);
        for (uint32_t i = 1; i < n && ok; i++) {
            if (ents[i].comp && ents[i - 1].comp && ents[i].off < (uint64_t)ents[i - 1].off + ents[i - 1].comp) {
                ok = false;
            }
        }
    }

    for (uint32_t i = 0; i < n && ok; i++) {
        /* Entries are sorted by offset now; the directory order is restored by embi_sfnt_build. */
        const woff1_ent *t = &ents[i];
        if (t->comp == t->orig) {
            tabs[i].tag = t->tag;
            tabs[i].data = in + t->off;
            tabs[i].len = t->orig;
        } else if (embi_inflate_all(in + t->off, t->comp, true, t->orig, &owned[i]) && owned[i].len == t->orig) {
            tabs[i].tag = t->tag;
            tabs[i].data = owned[i].data;
            tabs[i].len = t->orig;
        } else {
            ok = false;
        }
    }

    if (ok) {
        ok = embi_sfnt_build(flavor, tabs, n, out);
    }
    if (!ok) {
        out->len = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        embi_buf_free(&owned[i]);
    }
    embi_free(owned, n * sizeof *owned);
    embi_free(tabs, n * sizeof *tabs);
    embi_free(ents, n * sizeof *ents);
    return ok;
}
