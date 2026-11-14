#pragma once
/*
 * Builders for synthetic font data (tests/unit/font). Not a test: the test glob picks up test_*.c
 * only. Every table is written byte by byte here, so the expected values in the tests follow from
 * the bytes this file writes.
 */

#include "font_fixture.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ byte writer */

typedef struct fb_buf {
    uint8_t *p;
    size_t n, cap;
} fb_buf;

static inline void fb_reserve(fb_buf *b, size_t extra)
{
    if (b->n + extra <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra) {
        cap *= 2;
    }
    uint8_t *np = (uint8_t *)SDL_realloc(b->p, cap);
    if (np == nullptr) {
        SDL_Log("font_build: out of memory");
        abort(); /* also tells GCC's -O3 overflow analysis that p is never null below */
    }
    b->p = np;
    b->cap = cap;
}

static inline void fb_u8(fb_buf *b, uint32_t v)
{
    fb_reserve(b, 1);
    b->p[b->n++] = (uint8_t)v;
}

static inline void fb_u16(fb_buf *b, uint32_t v)
{
    fb_u8(b, v >> 8);
    fb_u8(b, v);
}

static inline void fb_u24(fb_buf *b, uint32_t v)
{
    fb_u8(b, v >> 16);
    fb_u16(b, v);
}

static inline void fb_u32(fb_buf *b, uint32_t v)
{
    fb_u16(b, v >> 16);
    fb_u16(b, v);
}

static inline void fb_bytes(fb_buf *b, const uint8_t *p, size_t n)
{
    fb_reserve(b, n);
    if (n > 0) {
        SDL_memcpy(b->p + b->n, p, n);
    }
    b->n += n;
}

/* Overwrites an already written big-endian value. */
static inline void fb_patch16(fb_buf *b, size_t at, uint32_t v)
{
    b->p[at] = (uint8_t)(v >> 8);
    b->p[at + 1] = (uint8_t)v;
}

static inline void fb_patch32(fb_buf *b, size_t at, uint32_t v)
{
    fb_patch16(b, at, v >> 16);
    fb_patch16(b, at + 2, v);
}

static inline void fb_free(fb_buf *b)
{
    SDL_free(b->p);
    *b = (fb_buf){0};
}

/* A byte run for the synthetic tables. */
typedef struct fb_seg {
    const uint8_t *p;
    size_t n;
} fb_seg;

/* Brace initializer for an fb_seg from a byte array. */
#define FB_SEG(arr) {(arr), sizeof(arr)}

/* Copy of a loaded file, so a test can patch bytes without touching the fixture. */
static inline fx_file fb_copy(const fx_file *src)
{
    fx_file c = {nullptr, src->n};
    c.p = (uint8_t *)SDL_malloc(src->n);
    if (c.p != nullptr) {
        SDL_memcpy(c.p, src->p, src->n);
    }
    return c;
}

/* ------------------------------------------------------------------ sfnt */

typedef struct fb_table {
    uint32_t tag;
    const uint8_t *p;
    size_t n;
} fb_table;

/* Writes an sfnt with the given tables in the given order. Caller frees out with fx_free. */
static inline bool fb_sfnt(uint32_t version, const fb_table *t, uint32_t nt, fx_file *out)
{
    size_t total = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) {
        total += (t[i].n + 3) & ~(size_t)3;
    }
    out->p = (uint8_t *)SDL_malloc(total);
    out->n = total;
    if (out->p == nullptr) {
        return false;
    }
    fb_buf b = {out->p, 0, total};
    fb_u32(&b, version);
    fb_u16(&b, nt);
    fb_u16(&b, 0); /* searchRange, entrySelector, rangeShift: not read by the parser */
    fb_u16(&b, 0);
    fb_u16(&b, 0);
    size_t data = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) {
        fb_u32(&b, t[i].tag);
        fb_u32(&b, 0); /* checksum: not verified */
        fb_u32(&b, (uint32_t)data);
        fb_u32(&b, (uint32_t)t[i].n);
        data += (t[i].n + 3) & ~(size_t)3;
    }
    for (uint32_t i = 0; i < nt; i++) {
        fb_bytes(&b, t[i].p, t[i].n);
        while (b.n & 3) {
            fb_u8(&b, 0);
        }
    }
    return true;
}

/* head (54 bytes): unitsPerEm at 18, indexToLocFormat 0 (short) at 50. */
static inline void fb_put_head(fb_buf *b, uint32_t upm)
{
    fb_u32(b, 0x00010000u);
    fb_u32(b, 0);
    fb_u32(b, 0);
    fb_u32(b, 0x5F0F3CF5u);
    fb_u16(b, 0);
    fb_u16(b, upm);
    for (int i = 0; i < 4; i++) {
        fb_u32(b, 0); /* created, modified */
    }
    fb_u16(b, 0); /* xMin */
    fb_u16(b, 0); /* yMin */
    fb_u16(b, 0); /* xMax */
    fb_u16(b, 0); /* yMax */
    fb_u16(b, 0); /* macStyle */
    fb_u16(b, 0); /* lowestRecPPEM */
    fb_u16(b, 0); /* fontDirectionHint */
    fb_u16(b, 0); /* indexToLocFormat */
    fb_u16(b, 0); /* glyphDataFormat */
}

/* hhea (36 bytes) with numberOfHMetrics = nhm at 34. */
static inline void fb_put_hhea(fb_buf *b, uint32_t nhm)
{
    fb_u32(b, 0x00010000u);
    for (int i = 0; i < 14; i++) { /* ascender .. caretOffset, then 4 reserved fields */
        fb_u16(b, 0);
    }
    fb_u16(b, 0); /* metricDataFormat */
    fb_u16(b, nhm);
}

/* maxp version 0.5 (6 bytes): numGlyphs at 4. */
static inline void fb_put_maxp(fb_buf *b, uint32_t ng)
{
    fb_u32(b, 0x00005000u);
    fb_u16(b, ng);
}

/* hmtx for ng glyphs, each with advance 500 and lsb 0. */
static inline void fb_put_hmtx(fb_buf *b, uint32_t ng)
{
    for (uint32_t i = 0; i < ng; i++) {
        fb_u16(b, 500);
        fb_u16(b, 0);
    }
}

/* Builds a complete sfnt: head, hhea, maxp and hmtx for ng glyphs, plus the extra tables. */
static inline bool fb_font(uint32_t version, uint32_t upm, uint32_t ng, const fb_table *extra,
                           uint32_t nextra, fx_file *out)
{
    fb_buf head = {0}, hhea = {0}, maxp = {0}, hmtx = {0};
    fb_put_head(&head, upm);
    fb_put_hhea(&hhea, ng);
    fb_put_maxp(&maxp, ng);
    fb_put_hmtx(&hmtx, ng);
    fb_table t[4 + 8];
    t[0] = (fb_table){EMBI_TAG('h', 'e', 'a', 'd'), head.p, head.n};
    t[1] = (fb_table){EMBI_TAG('h', 'h', 'e', 'a'), hhea.p, hhea.n};
    t[2] = (fb_table){EMBI_TAG('m', 'a', 'x', 'p'), maxp.p, maxp.n};
    t[3] = (fb_table){EMBI_TAG('h', 'm', 't', 'x'), hmtx.p, hmtx.n};
    for (uint32_t i = 0; i < nextra && i < 8; i++) {
        t[4 + i] = extra[i];
    }
    bool ok = fb_sfnt(version, t, 4 + (nextra < 8 ? nextra : 8), out);
    fb_free(&head);
    fb_free(&hhea);
    fb_free(&maxp);
    fb_free(&hmtx);
    return ok;
}

/* Finds a table in a loaded sfnt (face 0). Returns false when absent or the directory is short. */
static inline bool fb_find_table(const uint8_t *p, size_t n, uint32_t tag, size_t *off, size_t *len)
{
    if (n < 12) {
        return false;
    }
    const uint32_t nt = ((uint32_t)p[4] << 8) | p[5];
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = p + 12 + 16 * (size_t)i;
        if ((size_t)(r - p) + 16 > n) {
            return false;
        }
        const uint32_t t = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];
        if (t == tag) {
            *off = ((size_t)r[8] << 24) | ((size_t)r[9] << 16) | ((size_t)r[10] << 8) | r[11];
            *len = ((size_t)r[12] << 24) | ((size_t)r[13] << 16) | ((size_t)r[14] << 8) | r[15];
            return *off <= n && *len <= n - *off;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ CFF (version 1) */

/* Writes a CFF1 INDEX with 4-byte offsets. */
static inline void fb_cff_index(fb_buf *b, const fb_seg *items, uint32_t n)
{
    fb_u16(b, n);
    if (n == 0) {
        return;
    }
    fb_u8(b, 4);
    uint32_t off = 1;
    for (uint32_t i = 0; i <= n; i++) {
        fb_u32(b, off);
        if (i < n) {
            off += (uint32_t)items[i].n;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        fb_bytes(b, items[i].p, items[i].n);
    }
}

/* DICT integer operand, always 5 bytes (fixed size, so offsets can be written up front). */
static inline void fb_dict_i32(fb_buf *b, uint32_t v)
{
    fb_u8(b, 29);
    fb_u32(b, v);
}

/* Writes a CFF1 table holding the given charstrings and optional local subroutines. The Private
 * DICT holds a 60-operand BlueValues (more than the CFF1 limit of 48, which must not fail the
 * font) and, with subrs, a Subrs offset. No charset (ISOAdobe) and no FontMatrix. */
static inline void fb_cff1(fb_buf *out, const fb_seg *cs, uint32_t ncs, const fb_seg *subrs,
                           uint32_t nsubrs)
{
    size_t cs_index = 2 + 1 + 4 * ((size_t)ncs + 1);
    for (uint32_t i = 0; i < ncs; i++) {
        cs_index += cs[i].n;
    }
    const size_t pos_cs = 4 + 6 + 22 + 4; /* header, name, top DICT, strings, gsubrs */
    const size_t pos_priv = pos_cs + cs_index;
    const size_t priv_len = 61 + (nsubrs > 0 ? 6 : 0);
    fb_u8(out, 1); /* header: major, minor, hdrSize, offSize */
    fb_u8(out, 0);
    fb_u8(out, 4);
    fb_u8(out, 4);
    /* Name INDEX: one name "E". */
    fb_u16(out, 1);
    fb_u8(out, 1);
    fb_u8(out, 1);
    fb_u8(out, 2);
    fb_u8(out, 'E');
    /* Top DICT INDEX: CharStrings (17) and Private size then offset (18). */
    fb_u16(out, 1);
    fb_u8(out, 1);
    fb_u8(out, 1);
    fb_u8(out, 18);
    fb_dict_i32(out, (uint32_t)pos_cs);
    fb_u8(out, 17);
    fb_dict_i32(out, (uint32_t)priv_len);
    fb_dict_i32(out, (uint32_t)pos_priv);
    fb_u8(out, 18);
    fb_u16(out, 0); /* String INDEX */
    fb_u16(out, 0); /* Global subr INDEX */
    fb_cff_index(out, cs, ncs);
    /* Private DICT: 60 zero operands then BlueValues, and the Subrs offset when present. */
    for (int i = 0; i < 60; i++) {
        fb_u8(out, 139);
    }
    fb_u8(out, 6);
    if (nsubrs > 0) {
        fb_dict_i32(out, (uint32_t)priv_len);
        fb_u8(out, 19);
        fb_cff_index(out, subrs, nsubrs);
    }
}

/* ------------------------------------------------------------------ synthetic CFF glyphs */

/* Charstrings used by test_font_cff and the fuzz test. Each one draws a shape whose bbox the
 * tests check, or exercises a path (hintmask, flex family, subr bias, operator edge cases). */
static const uint8_t fb_cs_notdef[] = {14};
/* rmoveto 0 0; rlineto (100,0) (0,100) (-100,0); endchar: square (0,0)-(100,100). */
static const uint8_t fb_cs_square[] = {139, 139, 21, 239, 139, 139, 239, 39, 139, 5, 14};
/* hstem 0 0; hintmask with one mask byte; then the square. */
static const uint8_t fb_cs_hintmask[] = {139, 139, 1, 19, 128, 139, 139, 21,
                                         239, 139, 139, 239, 39, 139, 5, 14};
/* flex: (50,0)(50,50)(0,50) then (-50,0)(-50,-50)(0,-50), fd 50. Bbox (0,0)-(100,100). */
static const uint8_t fb_cs_flex[] = {139, 139, 21, 189, 139, 189, 189, 139, 189, 89, 139,
                                     89,  89,  139, 89, 189, 12, 35, 14};
/* hflex: dx1..dx3 and dx4..dx6 of 50, dy2 50 and dy5 -50. Bbox (0,0)-(300,50). */
static const uint8_t fb_cs_hflex[] = {139, 139, 21, 189, 189, 189, 189, 189, 189,
                                      189, 12,  34, 14};
/* hflex1: bbox (0,0)-(300,50). */
static const uint8_t fb_cs_hflex1[] = {139, 139, 21, 189, 139, 189, 189, 189, 189,
                                       189, 89, 189, 12,  36,  14};
/* flex1 closing back to the origin. Bbox (0,0)-(100,100). */
static const uint8_t fb_cs_flex1[] = {139, 139, 21, 189, 139, 189, 189, 139, 189, 89, 139,
                                      89,  89,  89, 12,  37,  14};
/* -107 callsubr, with subr 0 (bias 107 for fewer than 1240 subrs) drawing the square. */
static const uint8_t fb_cs_subrcall[] = {139, 139, 21, 32, 10, 14};
static const uint8_t fb_subr0[] = {239, 139, 139, 239, 39, 139, 5, 11};
/* 255 0x7FFFFFFF (about 2^15), then eight "dup mul" (about 2^(15*256), infinite), then vsindex
 * (overflow must fail, not convert). */
static const uint8_t fb_cs_ovf_vsindex[] = {255, 127, 255, 255, 255, 27, 24, 27, 24, 27, 24, 27,
                                            24,  27,  24,  27,  24,  27, 24, 15, 14};
/* Same overflowing value, then index. */
static const uint8_t fb_cs_ovf_index[] = {255, 127, 255, 255, 255, 27, 24, 27, 24, 27, 24, 27,
                                          24,  27,  24,  27,  24,  27, 24, 29, 14};
/* Escape 0 (dotsection) is ignored: no outline. */
static const uint8_t fb_cs_dotsection[] = {139, 139, 21, 12, 0, 14};
/* Stack 0 100 2 1 roll: rotates [0 100] right by 1 to [100 0]; rlineto (100,0). Bbox y stays 0. */
static const uint8_t fb_cs_roll[] = {139, 139, 21, 139, 239, 141, 140, 12, 30, 5, 14};

#define FB_NSYNTH_GLYPHS 12
static const fb_seg fb_synth_cs[FB_NSYNTH_GLYPHS] = {
    FB_SEG(fb_cs_notdef),    FB_SEG(fb_cs_square),      FB_SEG(fb_cs_hintmask),
    FB_SEG(fb_cs_flex),      FB_SEG(fb_cs_hflex),       FB_SEG(fb_cs_hflex1),
    FB_SEG(fb_cs_flex1),     FB_SEG(fb_cs_subrcall),    FB_SEG(fb_cs_ovf_vsindex),
    FB_SEG(fb_cs_ovf_index), FB_SEG(fb_cs_dotsection),  FB_SEG(fb_cs_roll),
};
static const fb_seg fb_synth_subrs[1] = {FB_SEG(fb_subr0)};

/* Builds the synthetic CFF1 sfnt with the given unitsPerEm. Caller frees with fx_free. */
static inline bool fb_synth_cff_font(uint32_t upm, fx_file *out)
{
    fb_buf cff = {0};
    fb_cff1(&cff, fb_synth_cs, FB_NSYNTH_GLYPHS, fb_synth_subrs, 1);
    fb_table extra[1] = {{EMBI_TAG('C', 'F', 'F', ' '), cff.p, cff.n}};
    bool ok = fb_font(EMBI_TAG('O', 'T', 'T', 'O'), upm, FB_NSYNTH_GLYPHS, extra, 1, out);
    fb_free(&cff);
    return ok;
}
