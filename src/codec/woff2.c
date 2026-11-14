#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

/*
 * WOFF 2.0 decoder (W3C WOFF2 recommendation, 2024). The sequence is: parse the table directory and
 * the collection directory for 'ttcf', Brotli-decode the single compressed block, undo the glyf/loca
 * transform (version 0) and the hmtx transform (version 1), then write an sfnt or a TrueType
 * collection. Transformed glyf and loca are rebuilt as a pair. The rebuilt glyph records keep each
 * glyph's outline, metrics and instructions, so decoded outlines equal the original. The bytes can
 * differ: flags and coordinates use the canonical encodings chosen here.
 */

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define W2_SIG TAG('w', 'O', 'F', '2')
#define W2_HEADER 48u
#define TAG_TTCF TAG('t', 't', 'c', 'f')
#define TAG_GLYF TAG('g', 'l', 'y', 'f')
#define TAG_LOCA TAG('l', 'o', 'c', 'a')
#define TAG_HMTX TAG('h', 'm', 't', 'x')
#define TAG_HHEA TAG('h', 'h', 'e', 'a')
#define TAG_MAXP TAG('m', 'a', 'x', 'p')
#define TAG_HEAD TAG('h', 'e', 'a', 'd')

/* WOFF2 4.1, Known Table Tags (flag value = index). */
static const uint32_t kKnownTags[63] = {
    TAG('c', 'm', 'a', 'p'), TAG('h', 'e', 'a', 'd'), TAG('h', 'h', 'e', 'a'), TAG('h', 'm', 't', 'x'),
    TAG('m', 'a', 'x', 'p'), TAG('n', 'a', 'm', 'e'), TAG('O', 'S', '/', '2'), TAG('p', 'o', 's', 't'),
    TAG('c', 'v', 't', ' '), TAG('f', 'p', 'g', 'm'), TAG('g', 'l', 'y', 'f'), TAG('l', 'o', 'c', 'a'),
    TAG('p', 'r', 'e', 'p'), TAG('C', 'F', 'F', ' '), TAG('V', 'O', 'R', 'G'), TAG('E', 'B', 'D', 'T'),
    TAG('E', 'B', 'L', 'C'), TAG('g', 'a', 's', 'p'), TAG('h', 'd', 'm', 'x'), TAG('k', 'e', 'r', 'n'),
    TAG('L', 'T', 'S', 'H'), TAG('P', 'C', 'L', 'T'), TAG('V', 'D', 'M', 'X'), TAG('v', 'h', 'e', 'a'),
    TAG('v', 'm', 't', 'x'), TAG('B', 'A', 'S', 'E'), TAG('G', 'D', 'E', 'F'), TAG('G', 'P', 'O', 'S'),
    TAG('G', 'S', 'U', 'B'), TAG('E', 'B', 'S', 'C'), TAG('J', 'S', 'T', 'F'), TAG('M', 'A', 'T', 'H'),
    TAG('C', 'B', 'D', 'T'), TAG('C', 'B', 'L', 'C'), TAG('C', 'O', 'L', 'R'), TAG('C', 'P', 'A', 'L'),
    TAG('S', 'V', 'G', ' '), TAG('s', 'b', 'i', 'x'), TAG('a', 'c', 'n', 't'), TAG('a', 'v', 'a', 'r'),
    TAG('b', 'd', 'a', 't'), TAG('b', 'l', 'o', 'c'), TAG('b', 's', 'l', 'n'), TAG('c', 'v', 'a', 'r'),
    TAG('f', 'd', 's', 'c'), TAG('f', 'e', 'a', 't'), TAG('f', 'm', 't', 'x'), TAG('f', 'v', 'a', 'r'),
    TAG('g', 'v', 'a', 'r'), TAG('h', 's', 't', 'y'), TAG('j', 'u', 's', 't'), TAG('l', 'c', 'a', 'r'),
    TAG('m', 'o', 'r', 't'), TAG('m', 'o', 'r', 'x'), TAG('o', 'p', 'b', 'd'), TAG('p', 'r', 'o', 'p'),
    TAG('t', 'r', 'a', 'k'), TAG('Z', 'a', 'p', 'f'), TAG('S', 'i', 'l', 'f'), TAG('G', 'l', 'a', 't'),
    TAG('G', 'l', 'o', 'c'), TAG('F', 'e', 'a', 't'), TAG('S', 'i', 'l', 'l'),
};

/* ------------------------------------------------------------------ readers and writers */

/* Bounded reader. Reading past the end sets err and yields zeros. */
typedef struct rdr {
    const uint8_t *p;
    size_t n;
    size_t pos;
    bool err;
} rdr;

static uint8_t r_u8(rdr *r)
{
    if (r->pos >= r->n) {
        r->err = true;
        return 0;
    }
    return r->p[r->pos++];
}

static uint32_t r_u16(rdr *r)
{
    uint32_t hi = r_u8(r);
    return (hi << 8) | r_u8(r);
}

static uint32_t r_u32(rdr *r)
{
    uint32_t hi = r_u16(r);
    return (hi << 16) | r_u16(r);
}

/* UIntBase128 (WOFF2 3.1): at most 5 bytes, no leading zero byte, no overflow. */
static uint32_t r_b128(rdr *r)
{
    uint32_t acc = 0;
    for (unsigned i = 0; i < 5; i++) {
        uint8_t b = r_u8(r);
        if (r->err) {
            return 0;
        }
        if (i == 0 && b == 0x80) {
            r->err = true;
            return 0;
        }
        if (acc & 0xfe000000u) {
            r->err = true;
            return 0;
        }
        acc = (acc << 7) | (b & 0x7fu);
        if (!(b & 0x80)) {
            return acc;
        }
    }
    r->err = true;
    return 0;
}

/* 255UInt16 (WOFF2 3.1). Every encoding is accepted. */
static uint32_t r_255(rdr *r)
{
    uint8_t code = r_u8(r);
    if (code == 253) {
        return r_u16(r);
    }
    if (code == 255) {
        return (uint32_t)r_u8(r) + 253u;
    }
    if (code == 254) {
        return (uint32_t)r_u8(r) + 506u;
    }
    return code;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool w16(embi_buf *b, uint32_t v)
{
    uint8_t t[2];
    put16(t, v);
    return embi_buf_append(b, t, 2);
}

static bool w32(embi_buf *b, uint32_t v)
{
    uint8_t t[4];
    put32(t, v);
    return embi_buf_append(b, t, 4);
}

static bool w_pad4(embi_buf *b)
{
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    size_t extra = (4 - (b->len & 3u)) & 3u;
    return embi_buf_append(b, zeros, extra);
}

static bool bitset(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (7 - (i & 7u))) & 1u;
}

/* ------------------------------------------------------------------ decoder state */

typedef struct w2_entry {
    uint32_t tag;
    uint8_t version;
    bool transformed;
    uint32_t orig_len;
    uint32_t stored_len; /* bytes in the decompressed block (transformed length when transformed) */
    const uint8_t *data; /* table bytes: a block slice, or the reconstruction once done */
    size_t len;
    bool done;
    embi_buf owned;
} w2_entry;

typedef struct w2_font {
    uint32_t flavor;
    uint32_t ntables;
    uint32_t *idx;
} w2_font;

typedef struct w2 {
    w2_entry *ent;
    uint32_t nent;
    w2_font *fonts;
    uint32_t nfonts; /* zero for a single font */
    embi_buf block;
    bool *head_adjusted; /* per entry: checkSumAdjustment already written (collections) */
    bool *used;          /* per entry: referenced by the output (every entry for a single font) */
    uint32_t *font_of;   /* per entry: first font listing it (collections), else UINT32_MAX */
} w2;

#define W2_MAX_TABLES 4096u /* real fonts have far fewer; bounds every per-table scan */

static void w2_free(w2 *w)
{
    for (uint32_t i = 0; i < w->nent; i++) {
        embi_buf_free(&w->ent[i].owned);
    }
    if (w->ent) {
        embi_free(w->ent, w->nent * sizeof *w->ent);
    }
    for (uint32_t f = 0; f < w->nfonts; f++) {
        if (w->fonts[f].idx) {
            embi_free(w->fonts[f].idx, w->fonts[f].ntables * sizeof(uint32_t));
        }
    }
    if (w->fonts) {
        embi_free(w->fonts, w->nfonts * sizeof *w->fonts);
    }
    if (w->head_adjusted) {
        embi_free(w->head_adjusted, w->nent * sizeof(bool));
    }
    if (w->used) {
        embi_free(w->used, w->nent * sizeof(bool));
    }
    if (w->font_of) {
        embi_free(w->font_of, w->nent * sizeof(uint32_t));
    }
    embi_buf_free(&w->block);
    *w = (w2){0};
}

/* Index of the entry with the given tag in the same font as entry e (any entry in a single-font
 * file), or -1. */
static int64_t w2_pair(const w2 *w, uint32_t e, uint32_t tag)
{
    if (w->nfonts == 0) {
        for (uint32_t i = 0; i < w->nent; i++) {
            if (w->ent[i].tag == tag) {
                return i;
            }
        }
        return -1;
    }
    uint32_t f = w->font_of[e];
    if (f == UINT32_MAX) {
        return -1;
    }
    for (uint32_t k = 0; k < w->fonts[f].ntables; k++) {
        if (w->ent[w->fonts[f].idx[k]].tag == tag) {
            return w->fonts[f].idx[k];
        }
    }
    return -1;
}

static uint64_t align4(uint64_t x)
{
    return (x + 3u) & ~3ull;
}

/* Bytes of the output made of the header and directories plus every finished, referenced table
 * (padded). Pending tables (glyf, loca, hmtx before their rebuild) are not counted. */
static uint64_t w2_size(const w2 *w)
{
    uint64_t total = 0;
    if (w->nfonts == 0) {
        total = 12 + 16ull * w->nent;
    } else {
        total = 12 + 4ull * w->nfonts;
        for (uint32_t f = 0; f < w->nfonts; f++) {
            total += 12 + 16ull * w->fonts[f].ntables;
        }
    }
    for (uint32_t e = 0; e < w->nent; e++) {
        if (w->used[e] && w->ent[e].done) {
            total += align4(w->ent[e].len);
        }
    }
    return total;
}

/* Marks every entry that some font of the output references. */
static void w2_mark_used(w2 *w)
{
    for (uint32_t e = 0; e < w->nent; e++) {
        w->used[e] = w->nfonts == 0;
    }
    for (uint32_t e = 0; e < w->nent; e++) {
        w->font_of[e] = UINT32_MAX;
    }
    for (uint32_t f = 0; f < w->nfonts; f++) {
        for (uint32_t k = 0; k < w->fonts[f].ntables; k++) {
            uint32_t e = w->fonts[f].idx[k];
            w->used[e] = true;
            if (w->font_of[e] == UINT32_MAX) {
                w->font_of[e] = f;
            }
        }
    }
}

/* ------------------------------------------------------------------ glyf and loca (WOFF2 5.1, 5.2, 5.3) */

static int32_t with_sign(uint32_t flag, int32_t base)
{
    return (flag & 1u) ? base : -base;
}

/* Decodes one point's (dx, dy) from the glyph stream. idx is the flag's low 7 bits (the triplet
 * index, WOFF2 5.2). */
static bool triplet(uint32_t idx, rdr *g, int32_t *dx, int32_t *dy)
{
    if (idx < 10) {
        int32_t b0 = r_u8(g);
        *dx = 0;
        *dy = with_sign(idx, (int32_t)((idx & 14u) << 7) + b0);
    } else if (idx < 20) {
        int32_t b0 = r_u8(g);
        *dx = with_sign(idx, (int32_t)(((idx - 10u) & 14u) << 7) + b0);
        *dy = 0;
    } else if (idx < 84) {
        int32_t b0 = r_u8(g);
        int32_t bb = (int32_t)(idx - 20u);
        *dx = with_sign(idx, 1 + (bb & 0x30) + (b0 >> 4));
        *dy = with_sign(idx >> 1, 1 + ((bb & 0x0c) << 2) + (b0 & 0x0f));
    } else if (idx < 120) {
        int32_t b0 = r_u8(g);
        int32_t b1 = r_u8(g);
        int32_t bb = (int32_t)(idx - 84u);
        *dx = with_sign(idx, 1 + (bb / 12) * 256 + b0);
        *dy = with_sign(idx >> 1, 1 + ((bb % 12) >> 2) * 256 + b1);
    } else if (idx < 124) {
        int32_t b0 = r_u8(g);
        int32_t b1 = r_u8(g);
        int32_t b2 = r_u8(g);
        *dx = with_sign(idx, (b0 << 4) + (b1 >> 4));
        *dy = with_sign(idx >> 1, ((b1 & 0x0f) << 8) + b2);
    } else {
        int32_t b0 = r_u8(g);
        int32_t b1 = r_u8(g);
        int32_t b2 = r_u8(g);
        int32_t b3 = r_u8(g);
        *dx = with_sign(idx, (b0 << 8) + b1);
        *dy = with_sign(idx >> 1, (b2 << 8) + b3);
    }
    return !g->err;
}

/* Rebuilds glyf and loca from a transformed glyf table of length tlen (WOFF2 5.1). The loca table
 * gets numGlyphs + 1 offsets in the format given by the transform's indexFormat. The padded glyf
 * table never grows past limit bytes: each glyph is checked before it is written. */
static bool w2_glyf(const uint8_t *t, size_t tlen, uint64_t limit, embi_buf *glyf, embi_buf *loca,
                    uint32_t *num_glyphs, uint32_t *index_format)
{
    /* Header: reserved u16, optionFlags u16, numGlyphs u16, indexFormat u16, seven u32 sizes. */
    if (tlen < 36) {
        return false;
    }
    uint32_t opt = be16(t + 2);
    uint32_t ng = be16(t + 4);
    uint32_t idxfmt = be16(t + 6);
    if (idxfmt > 1 || ng == 0) {
        return false;
    }
    uint64_t sizes[7];
    uint64_t sum = 36;
    for (unsigned i = 0; i < 7; i++) {
        sizes[i] = be32(t + 8 + 4 * i);
        sum += sizes[i];
    }
    uint64_t ov_size = (opt & 1u) ? (ng + 7u) / 8u : 0;
    if (sum + ov_size != tlen) {
        return false;
    }
    /* Stream order: nContour, nPoints, flag, glyph, composite, bbox, instruction, overlap. */
    const uint8_t *base = t + 36;
    rdr nc = {base, sizes[0], 0, false};
    rdr np = {base + sizes[0], sizes[1], 0, false};
    rdr fl = {np.p + sizes[1], sizes[2], 0, false};
    rdr gl = {fl.p + sizes[2], sizes[3], 0, false};
    rdr cp = {gl.p + sizes[3], sizes[4], 0, false};
    uint64_t bbox_bitmap = 4u * ((ng + 31u) / 32u);
    if (sizes[5] < bbox_bitmap) {
        return false;
    }
    const uint8_t *bb_bits = cp.p + sizes[4];
    rdr bb = {bb_bits + bbox_bitmap, sizes[5] - bbox_bitmap, 0, false};
    rdr ins = {bb_bits + sizes[5], sizes[6], 0, false};
    const uint8_t *ov_bits = ins.p + sizes[6];

    bool ok = true;
    uint32_t *offs = embi_alloc((size_t)(ng + 1) * sizeof *offs);
    embi_buf comp = {0};
    embi_buf flags_out = {0};
    embi_buf xs = {0};
    embi_buf ys = {0};
    uint32_t *endpts = nullptr;
    size_t endpts_cap = 0;
    int32_t *dxs = nullptr;
    int32_t *dys = nullptr;
    uint8_t *on = nullptr;
    size_t pts_cap = 0;

    for (uint32_t g = 0; g < ng && ok; g++) {
        ok = w_pad4(glyf);
        offs[g] = (uint32_t)glyf->len;
        int32_t ncont = (int16_t)(uint16_t)r_u16(&nc);
        bool explicit_bbox = bitset(bb_bits, g);
        if (nc.err || ncont < -1) { /* contour counts below -1 are not valid (WOFF2 5.1) */
            ok = false;
            break;
        }
        if (ncont == 0) {
            ok = !explicit_bbox; /* an empty glyph has no bounding box */
            continue;
        }
        if (ncont == -1) {
            /* Composite: components are copied verbatim, then the optional instructions. */
            comp.len = 0;
            bool more = true;
            bool has_instr = false;
            while (more && ok) {
                uint32_t cf = r_u16(&cp);
                /* flags and glyphIndex (4 bytes), then the arguments and the optional transform. */
                uint32_t nbytes = 4 + ((cf & 0x0001u) ? 4u : 2u) + ((cf & 0x0008u) ? 2u : 0u) +
                                  ((cf & 0x0040u) ? 4u : 0u) + ((cf & 0x0080u) ? 8u : 0u);
                ok = w16(&comp, cf);
                for (uint32_t k = 2; k < nbytes && ok && !cp.err; k++) {
                    uint8_t b = r_u8(&cp);
                    ok = embi_buf_append(&comp, &b, 1);
                }
                more = (cf & 0x0020u) != 0;
                has_instr = has_instr || (cf & 0x0100u) != 0;
                ok = ok && !cp.err;
            }
            if (!ok || !explicit_bbox) {
                ok = false; /* composites must carry their bounding box */
                break;
            }
            uint32_t ilen = 0;
            if (has_instr) {
                ilen = r_255(&gl);
                if (gl.err || ilen > ins.n - ins.pos) {
                    ok = false;
                    break;
                }
            }
            /* header (10 bytes), components, instruction length and bytes */
            uint64_t csize = 10 + (uint64_t)comp.len + (has_instr ? 2u + ilen : 0u);
            if (align4(glyf->len) + csize > limit) {
                ok = false;
                break;
            }
            uint32_t xmin = r_u16(&bb);
            uint32_t ymin = r_u16(&bb);
            uint32_t xmax = r_u16(&bb);
            uint32_t ymax = r_u16(&bb);
            if (bb.err) {
                ok = false;
                break;
            }
            ok = w16(glyf, 0xffffu) && w16(glyf, xmin) && w16(glyf, ymin) && w16(glyf, xmax) && w16(glyf, ymax) &&
                 embi_buf_append(glyf, comp.data, comp.len);
            if (ok && has_instr) {
                ok = w16(glyf, ilen) && embi_buf_append(glyf, ins.p + ins.pos, ilen);
                ins.pos += ilen;
            }
            continue;
        }

        /* Simple glyph. */
        uint32_t nctr = (uint32_t)ncont;
        if (nctr > endpts_cap) {
            endpts = embi_realloc(endpts, endpts_cap * sizeof *endpts, nctr * sizeof *endpts);
            endpts_cap = nctr;
        }
        uint32_t total = 0;
        for (uint32_t c = 0; c < nctr && ok; c++) {
            uint32_t npts = r_255(&np);
            if (np.err || npts == 0 || npts > 65535u - total) {
                ok = false;
                break;
            }
            total += npts;
            endpts[c] = total - 1;
        }
        if (!ok) {
            break;
        }
        if (total > pts_cap) {
            dxs = embi_realloc(dxs, pts_cap * sizeof *dxs, total * sizeof *dxs);
            dys = embi_realloc(dys, pts_cap * sizeof *dys, total * sizeof *dys);
            on = embi_realloc(on, pts_cap, total);
            pts_cap = total;
        }
        for (uint32_t i = 0; i < total && ok; i++) {
            uint8_t f = r_u8(&fl);
            on[i] = (uint8_t)((f & 0x80u) ? 0 : 1);
            ok = triplet(f & 0x7fu, &gl, &dxs[i], &dys[i]);
        }
        ok = ok && !fl.err;
        if (!ok) {
            break;
        }
        uint32_t ilen = r_255(&gl);
        if (gl.err || ilen > ins.n - ins.pos) {
            ok = false;
            break;
        }
        /* Output size of this record: header, end points, instructions, one flag byte per point,
         * then 0, 1 or 2 bytes per coordinate (the encoding chosen below). */
        uint64_t gsize = 10 + 2ull * nctr + 2 + ilen + total;
        for (uint32_t i = 0; i < total; i++) {
            gsize += dxs[i] == 0 ? 0u : (dxs[i] > -256 && dxs[i] < 256) ? 1u : 2u;
            gsize += dys[i] == 0 ? 0u : (dys[i] > -256 && dys[i] < 256) ? 1u : 2u;
        }
        if (align4(glyf->len) + gsize > limit) {
            ok = false;
            break;
        }

        /* Absolute coordinates for the bounding box, in 64-bit so long deltas cannot overflow. */
        int64_t x = 0;
        int64_t y = 0;
        int64_t bx0 = 0;
        int64_t by0 = 0;
        int64_t bx1 = 0;
        int64_t by1 = 0;
        for (uint32_t i = 0; i < total; i++) {
            x += dxs[i];
            y += dys[i];
            if (i == 0 || x < bx0) {
                bx0 = x;
            }
            if (i == 0 || x > bx1) {
                bx1 = x;
            }
            if (i == 0 || y < by0) {
                by0 = y;
            }
            if (i == 0 || y > by1) {
                by1 = y;
            }
        }
        int64_t xmin = bx0;
        int64_t ymin = by0;
        int64_t xmax = bx1;
        int64_t ymax = by1;
        if (explicit_bbox) {
            xmin = (int16_t)(uint16_t)r_u16(&bb);
            ymin = (int16_t)(uint16_t)r_u16(&bb);
            xmax = (int16_t)(uint16_t)r_u16(&bb);
            ymax = (int16_t)(uint16_t)r_u16(&bb);
            if (bb.err) {
                ok = false;
                break;
            }
        }
        for (uint32_t i = 0; i < total; i++) {
            if (dxs[i] < -32768 || dxs[i] > 32767 || dys[i] < -32768 || dys[i] > 32767) {
                ok = false;
            }
        }
        if (!ok) {
            break;
        }

        /* TrueType record: header, end points, instructions, flags, then x and y coordinates. */
        ok = w16(glyf, nctr) && w16(glyf, (uint32_t)(uint16_t)(int16_t)xmin) &&
             w16(glyf, (uint32_t)(uint16_t)(int16_t)ymin) && w16(glyf, (uint32_t)(uint16_t)(int16_t)xmax) &&
             w16(glyf, (uint32_t)(uint16_t)(int16_t)ymax);
        for (uint32_t c = 0; c < nctr && ok; c++) {
            ok = w16(glyf, endpts[c]);
        }
        ok = ok && w16(glyf, ilen) && embi_buf_append(glyf, ins.p + ins.pos, ilen);
        ins.pos += ilen;
        if (!ok) {
            break;
        }

        bool overlap = (opt & 1u) && bitset(ov_bits, g);
        flags_out.len = 0;
        xs.len = 0;
        ys.len = 0;
        for (uint32_t i = 0; i < total && ok; i++) {
            uint8_t f = on[i] ? 0x01 : 0x00;
            if (i == 0 && overlap) {
                f |= 0x40;
            }
            int32_t dx = dxs[i];
            int32_t dy = dys[i];
            if (dx == 0) {
                f |= 0x10;
            } else if (dx > -256 && dx < 256) {
                f |= 0x02;
                if (dx > 0) {
                    f |= 0x10;
                }
                uint8_t v = (uint8_t)(dx < 0 ? -dx : dx);
                ok = embi_buf_append(&xs, &v, 1);
            } else {
                ok = w16(&xs, (uint32_t)(uint16_t)(int16_t)dx);
            }
            if (dy == 0) {
                f |= 0x20;
            } else if (dy > -256 && dy < 256) {
                f |= 0x04;
                if (dy > 0) {
                    f |= 0x20;
                }
                uint8_t v = (uint8_t)(dy < 0 ? -dy : dy);
                ok = ok && embi_buf_append(&ys, &v, 1);
            } else {
                ok = ok && w16(&ys, (uint32_t)(uint16_t)(int16_t)dy);
            }
            ok = ok && embi_buf_append(&flags_out, &f, 1);
        }
        ok = ok && embi_buf_append(glyf, flags_out.data, flags_out.len) &&
             embi_buf_append(glyf, xs.data, xs.len) && embi_buf_append(glyf, ys.data, ys.len);
    }

    /* Every stream must be consumed exactly. */
    if (ok && (nc.pos != nc.n || np.pos != np.n || fl.pos != fl.n || gl.pos != gl.n || cp.pos != cp.n ||
               bb.pos != bb.n || ins.pos != ins.n)) {
        ok = false;
    }
    if (ok) {
        ok = w_pad4(glyf) && glyf->len <= limit;
        offs[ng] = (uint32_t)glyf->len;
        bool lng = idxfmt == 1;
        if (!lng && offs[ng] / 2u > 0xffffu) {
            ok = false; /* the short loca format cannot address this much glyf data */
        }
        for (uint32_t g = 0; ok && g <= ng; g++) {
            ok = lng ? w32(loca, offs[g]) : w16(loca, offs[g] / 2u);
        }
        if (ok) {
            *num_glyphs = ng;
            *index_format = idxfmt;
        }
    }

    embi_buf_free(&comp);
    embi_buf_free(&flags_out);
    embi_buf_free(&xs);
    embi_buf_free(&ys);
    if (endpts) {
        embi_free(endpts, endpts_cap * sizeof *endpts);
    }
    if (dxs) {
        embi_free(dxs, pts_cap * sizeof *dxs);
    }
    if (dys) {
        embi_free(dys, pts_cap * sizeof *dys);
    }
    if (on) {
        embi_free(on, pts_cap);
    }
    embi_free(offs, (size_t)(ng + 1) * sizeof *offs);
    return ok;
}

/* xMin of glyph gid from glyf and loca (0 for an empty glyph). */
static bool w2_xmin(const uint8_t *glyf, size_t glen, const uint8_t *loca, size_t llen, bool lng, uint32_t gid,
                    int32_t *xmin)
{
    size_t esz = lng ? 4 : 2;
    if ((uint64_t)(gid + 2) * esz > llen) {
        return false;
    }
    uint64_t a;
    uint64_t b;
    if (lng) {
        a = be32(loca + (size_t)gid * 4);
        b = be32(loca + (size_t)gid * 4 + 4);
    } else {
        a = (uint64_t)be16(loca + (size_t)gid * 2) * 2u;
        b = (uint64_t)be16(loca + (size_t)gid * 2 + 2) * 2u;
    }
    if (a == b) {
        *xmin = 0;
        return true;
    }
    if (a > b || b > glen || b - a < 10) {
        return false;
    }
    *xmin = (int16_t)(uint16_t)be16(glyf + a + 2);
    return true;
}

/* ------------------------------------------------------------------ hmtx (WOFF2 5.4) */

static bool w2_hmtx(const uint8_t *t, size_t tlen, uint32_t ng, uint32_t nhm, const uint8_t *glyf, size_t glen,
                    const uint8_t *loca, size_t llen, bool lng, uint64_t limit, embi_buf *out)
{
    if (tlen < 1 || nhm == 0 || nhm > ng) {
        return false;
    }
    /* The rebuilt table is 4 bytes per full metric and 2 per side bearing: 2 * (ng + nhm). */
    if (align4(2ull * ng + 2ull * nhm) > limit) {
        return false;
    }
    uint8_t flags = t[0];
    if (flags & 0xfc) {
        return false; /* bits 2..7 are reserved */
    }
    /* advanceWidth[nhm] always; lsb[nhm] unless bit 0; leftSideBearing[ng - nhm] unless bit 1. */
    size_t pos = 1 + 2 * (size_t)nhm;
    size_t lsb_at = pos;
    if (!(flags & 1)) {
        pos += 2 * (size_t)nhm;
    }
    size_t mono_at = pos;
    if (!(flags & 2)) {
        pos += 2 * (size_t)(ng - nhm);
    }
    if (pos != tlen) {
        return false;
    }
    const uint8_t *lsb = t + lsb_at;
    const uint8_t *mono = t + mono_at;
    for (uint32_t i = 0; i < ng; i++) {
        int32_t lsb_v = 0;
        if (i < nhm) {
            if (!(flags & 1)) {
                lsb_v = (int16_t)(uint16_t)be16(lsb + 2 * (size_t)i);
            } else if (!w2_xmin(glyf, glen, loca, llen, lng, i, &lsb_v)) {
                return false;
            }
            if (!w16(out, be16(t + 1 + 2 * (size_t)i))) {
                return false;
            }
        } else {
            if (!(flags & 2)) {
                lsb_v = (int16_t)(uint16_t)be16(mono + 2 * (size_t)(i - nhm));
            } else if (!w2_xmin(glyf, glen, loca, llen, lng, i, &lsb_v)) {
                return false;
            }
        }
        if (!w16(out, (uint32_t)(uint16_t)lsb_v)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ collections (WOFF2 4.2) */

/* Writes a TrueType collection. A table referenced by several fonts is stored once. Every directory
 * checksum is computed while each head table still has a zero checkSumAdjustment; the adjustments
 * are written afterwards. A head table shared by several fonts keeps the adjustment of the first
 * font that lists it. Fails when a font repeats a tag or the output would exceed max_out. */
typedef struct w2_tagref {
    uint32_t tag;
    uint32_t e;
} w2_tagref;

static int cmp_tagref(const void *a, const void *b)
{
    uint32_t x = ((const w2_tagref *)a)->tag;
    uint32_t y = ((const w2_tagref *)b)->tag;
    return (x > y) - (x < y);
}

static bool w2_build_ttc(w2 *w, embi_buf *out, uint64_t max_out)
{
    /* Sort each font's (tag, entry) pairs once; duplicates are then adjacent. */
    for (uint32_t f = 0; f < w->nfonts; f++) {
        const w2_font *ft = &w->fonts[f];
        w2_tagref *refs = embi_alloc((size_t)ft->ntables * sizeof *refs);
        for (uint32_t i = 0; i < ft->ntables; i++) {
            refs[i].tag = w->ent[ft->idx[i]].tag;
            refs[i].e = ft->idx[i];
        }
        SDL_qsort(refs, ft->ntables, sizeof *refs, cmp_tagref);
        bool dup = false;
        for (uint32_t i = 1; i < ft->ntables; i++) {
            dup = dup || refs[i].tag == refs[i - 1].tag;
        }
        embi_free(refs, (size_t)ft->ntables * sizeof *refs);
        if (dup) {
            return false;
        }
    }

    uint64_t pos = 12 + 4ull * w->nfonts;
    for (uint32_t f = 0; f < w->nfonts; f++) {
        pos += 12 + 16ull * w->fonts[f].ntables;
    }
    /* Each table is stored once, after the directories, at the first font's reference. */
    uint64_t *eoff = embi_calloc(w->nent, sizeof *eoff);
    uint32_t *adj = embi_calloc(w->nent, sizeof *adj);
    for (uint32_t f = 0; f < w->nfonts; f++) {
        for (uint32_t k = 0; k < w->fonts[f].ntables; k++) {
            uint32_t e = w->fonts[f].idx[k];
            if (eoff[e] == 0) {
                eoff[e] = pos;
                pos += align4(w->ent[e].len);
            }
        }
    }
    bool ok = pos <= max_out && pos <= UINT32_MAX && embi_buf_reserve(out, (size_t)pos);
    if (ok) {
        out->len = (size_t)pos;
        SDL_memset(out->data, 0, out->len);
        for (uint32_t e = 0; e < w->nent; e++) {
            if (eoff[e] == 0) {
                continue;
            }
            uint8_t *dst = out->data + eoff[e];
            if (w->ent[e].len) {
                SDL_memcpy(dst, w->ent[e].data, w->ent[e].len);
            }
            if (w->ent[e].tag == TAG_HEAD && w->ent[e].len >= 12) {
                SDL_memset(dst + 8, 0, 4); /* checkSumAdjustment is zero while summing */
            }
        }

        /* TTC header: 'ttcf', version 1.0, numFonts, one offset per font. */
        uint8_t *hdr = out->data;
        put32(hdr, TAG_TTCF);
        put32(hdr + 4, 0x00010000u);
        put32(hdr + 8, w->nfonts);

        /* Pass 1: every font directory, entries sorted by tag as the sfnt format requires. */
        uint64_t dir_at = 12 + 4ull * w->nfonts;
        for (uint32_t f = 0; f < w->nfonts; f++) {
            put32(hdr + 12 + 4 * (size_t)f, (uint32_t)dir_at);
            uint32_t nt = w->fonts[f].ntables;
            uint8_t *fo = out->data + dir_at;
            uint32_t es = 0;
            while ((2u << es) <= nt) {
                es++;
            }
            uint32_t sr = (1u << es) * 16u;
            put32(fo, w->fonts[f].flavor);
            put16(fo + 4, nt);
            put16(fo + 6, sr);
            put16(fo + 8, es);
            put16(fo + 10, nt * 16u - sr);

            w2_tagref *order = embi_alloc((size_t)nt * sizeof *order);
            for (uint32_t k = 0; k < nt; k++) {
                order[k].tag = w->ent[w->fonts[f].idx[k]].tag;
                order[k].e = w->fonts[f].idx[k];
            }
            SDL_qsort(order, nt, sizeof *order, cmp_tagref);
            for (uint32_t k = 0; k < nt; k++) {
                uint32_t e = order[k].e;
                uint8_t *d = fo + 12 + 16 * (size_t)k;
                put32(d, w->ent[e].tag);
                put32(d + 4, embi_sfnt_checksum(out->data + eoff[e], w->ent[e].len));
                put32(d + 8, (uint32_t)eoff[e]);
                put32(d + 12, (uint32_t)w->ent[e].len);
            }
            embi_free(order, (size_t)nt * sizeof *order);
            dir_at += 12 + 16ull * nt;
        }

        /* Pass 2: a font's sum is its header and directory words plus its table checksums (which
         * already hold the zero adjustment). The first font listing a head table gets its value. */
        dir_at = 12 + 4ull * w->nfonts;
        for (uint32_t f = 0; f < w->nfonts; f++) {
            uint32_t nt = w->fonts[f].ntables;
            const uint8_t *fo = out->data + dir_at;
            uint32_t font_sum = embi_sfnt_checksum(fo, 12 + 16 * (size_t)nt);
            for (uint32_t k = 0; k < nt; k++) {
                font_sum += be32(fo + 12 + 16 * (size_t)k + 4);
            }
            for (uint32_t k = 0; k < nt; k++) {
                uint32_t e = w->fonts[f].idx[k];
                if (w->ent[e].tag == TAG_HEAD && w->ent[e].len >= 12 && !w->head_adjusted[e]) {
                    w->head_adjusted[e] = true;
                    adj[e] = 0xB1B0AFBAu - font_sum;
                }
            }
            dir_at += 12 + 16ull * nt;
        }

        /* Pass 3: store the adjustments. */
        for (uint32_t e = 0; e < w->nent; e++) {
            if (w->head_adjusted[e]) {
                put32(out->data + eoff[e] + 8, adj[e]);
            }
        }
    }
    embi_free(adj, w->nent * sizeof *adj);
    embi_free(eoff, w->nent * sizeof *eoff);
    return ok;
}

/* ------------------------------------------------------------------ top level */

bool embi_woff2_decode(const uint8_t *in, size_t in_len, size_t max_out, embi_buf *out)
{
    out->len = 0;
    if (in_len < W2_HEADER || be32(in) != W2_SIG) {
        return false;
    }
    uint32_t flavor = be32(in + 4);
    uint32_t length = be32(in + 8);
    uint32_t ntables = be16(in + 12);
    uint32_t total_comp = be32(in + 20);
    if (length > in_len || ntables == 0 || ntables > W2_MAX_TABLES) {
        return false;
    }

    w2 w = {0};
    w.nent = ntables;
    w.ent = embi_calloc(ntables, sizeof *w.ent);
    w.head_adjusted = embi_calloc(ntables, sizeof(bool));
    w.used = embi_calloc(ntables, sizeof(bool));
    w.font_of = embi_calloc(ntables, sizeof(uint32_t));
    rdr r = {in, length, W2_HEADER, false};
    bool ok = true;

    /* Table directory (WOFF2 4.1). */
    for (uint32_t i = 0; i < ntables && ok; i++) {
        w2_entry *e = &w.ent[i];
        uint8_t flags = r_u8(&r);
        uint32_t tidx = flags & 0x3fu;
        e->version = (uint8_t)(flags >> 6);
        e->tag = tidx == 63 ? r_u32(&r) : kKnownTags[tidx];
        e->orig_len = r_b128(&r);
        if (e->tag == TAG_GLYF || e->tag == TAG_LOCA) {
            /* version 0 = transformed, version 3 = null transform */
            e->transformed = e->version == 0;
            ok = e->version == 0 || e->version == 3;
        } else if (e->tag == TAG_HMTX) {
            /* version 0 = null transform, version 1 = transformed */
            e->transformed = e->version == 1;
            ok = e->version == 0 || e->version == 1;
        } else {
            ok = e->version == 0;
        }
        e->stored_len = e->orig_len;
        if (ok && e->transformed) {
            e->stored_len = r_b128(&r);
        }
        ok = ok && !r.err;
    }

    /* Collection directory (WOFF2 4.2). */
    if (ok && flavor == TAG_TTCF) {
        r_u32(&r); /* TTC version */
        uint32_t nf = r_255(&r);
        if (r.err || nf == 0 || nf > 1024) {
            ok = false;
        } else {
            w.nfonts = nf;
            w.fonts = embi_calloc(nf, sizeof *w.fonts);
        }
        for (uint32_t f = 0; f < nf && ok; f++) {
            uint32_t nt = r_255(&r);
            uint32_t fflavor = r_u32(&r);
            if (r.err || nt == 0 || nt > ntables) {
                ok = false;
                break;
            }
            w.fonts[f].flavor = fflavor;
            w.fonts[f].ntables = nt;
            w.fonts[f].idx = embi_calloc(nt, sizeof(uint32_t));
            for (uint32_t k = 0; k < nt && ok; k++) {
                uint32_t e = r_255(&r);
                ok = e < ntables;
                w.fonts[f].idx[k] = e;
            }
            ok = ok && !r.err;
        }
    }

    /* Compressed table data (WOFF2 5). */
    size_t comp_start = r.pos;
    if (ok && (r.err || (uint64_t)comp_start + total_comp > length)) {
        ok = false;
    }
    if (ok) {
        w2_mark_used(&w);
    }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < ntables; i++) {
        sum += w.ent[i].stored_len;
    }
    if (ok && sum > max_out) {
        ok = false;
    }
    if (ok) {
        ok = embi_brotli_decode(in + comp_start, total_comp, (size_t)sum, &w.block) && w.block.len == sum;
    }
    if (ok) {
        size_t off = 0;
        for (uint32_t i = 0; i < ntables; i++) {
            w2_entry *e = &w.ent[i];
            e->data = w.block.data ? w.block.data + off : nullptr;
            e->len = e->stored_len;
            off += e->stored_len;
            e->done = !e->transformed;
            if (!e->transformed) {
                e->len = e->orig_len; /* equal to stored_len for untransformed tables */
            }
        }
    }

    /* glyf is rebuilt together with its loca, which must be the transformed, zero-length entry. */
    for (uint32_t i = 0; ok && i < ntables; i++) {
        w2_entry *e = &w.ent[i];
        if (e->tag != TAG_GLYF || !e->transformed) {
            continue;
        }
        int64_t li = w2_pair(&w, i, TAG_LOCA);
        if (li < 0 || !w.ent[li].transformed || w.ent[li].stored_len != 0 || w.ent[li].done) {
            ok = false;
            break;
        }
        /* glyf may use what is left after the finished tables and the (fixed-size) loca. */
        uint64_t base = w2_size(&w) + align4(w.ent[li].orig_len);
        if (base > max_out) {
            ok = false;
            break;
        }
        embi_buf glyf = {0};
        embi_buf loca = {0};
        uint32_t ng = 0;
        uint32_t idxfmt = 0;
        int64_t hd = w2_pair(&w, i, TAG_HEAD);
        /* head.indexToLocFormat must agree with the loca format chosen by the transform (WOFF2 5.1). */
        bool head_ok = hd >= 0 && w.ent[hd].done && !w.ent[hd].transformed && w.ent[hd].len >= 52;
        if (!head_ok || !w2_glyf(e->data, e->stored_len, max_out - base, &glyf, &loca, &ng, &idxfmt) ||
            be16(w.ent[hd].data + 50) != idxfmt || loca.len != w.ent[li].orig_len) {
            embi_buf_free(&glyf);
            embi_buf_free(&loca);
            ok = false;
            break;
        }
        e->owned = glyf;
        e->data = e->owned.data;
        e->len = e->owned.len;
        e->done = true;
        w.ent[li].owned = loca;
        w.ent[li].data = w.ent[li].owned.data;
        w.ent[li].len = w.ent[li].owned.len;
        w.ent[li].done = true;
    }
    for (uint32_t i = 0; ok && i < ntables; i++) {
        /* Only hmtx may still be pending; it is rebuilt below. */
        ok = w.ent[i].done || w.ent[i].tag == TAG_HMTX;
    }

    /* hmtx (version 1): needs glyf, loca, hhea and maxp from the same font, and the head table's
     * indexToLocFormat. */
    for (uint32_t i = 0; ok && i < ntables; i++) {
        w2_entry *e = &w.ent[i];
        if (e->tag != TAG_HMTX || !e->transformed) {
            continue;
        }
        int64_t gi = w2_pair(&w, i, TAG_GLYF);
        int64_t li = w2_pair(&w, i, TAG_LOCA);
        int64_t hi = w2_pair(&w, i, TAG_HHEA);
        int64_t mi = w2_pair(&w, i, TAG_MAXP);
        int64_t hd = w2_pair(&w, i, TAG_HEAD);
        if (gi < 0 || li < 0 || hi < 0 || mi < 0 || hd < 0) {
            ok = false;
            break;
        }
        const w2_entry *he = &w.ent[hi];
        const w2_entry *me = &w.ent[mi];
        const w2_entry *de = &w.ent[hd];
        const w2_entry *ge = &w.ent[gi];
        const w2_entry *le = &w.ent[li];
        if (!he->done || !me->done || !de->done || !ge->done || !le->done || me->len < 6 || he->len < 36 ||
            de->len < 52) {
            ok = false;
            break;
        }
        uint32_t ng_v = be16(me->data + 4);
        uint32_t nhm = be16(he->data + 34);
        bool lng = be16(de->data + 50) != 0;
        uint64_t base = w2_size(&w);
        if (base > max_out) {
            ok = false;
            break;
        }
        embi_buf hm = {0};
        if (!w2_hmtx(e->data, e->stored_len, ng_v, nhm, ge->data, ge->len, le->data, le->len, lng, max_out - base,
                     &hm)) {
            embi_buf_free(&hm);
            ok = false;
            break;
        }
        if (hm.len != e->orig_len) {
            embi_buf_free(&hm);
            ok = false;
            break;
        }
        e->owned = hm;
        e->data = e->owned.data;
        e->len = e->owned.len;
        e->done = true;
    }
    for (uint32_t i = 0; ok && i < ntables; i++) {
        ok = w.ent[i].done;
    }

    if (ok) {
        if (w.nfonts == 0) {
            ok = w2_size(&w) <= max_out;
            embi_sfnt_table *tabs = ok ? embi_calloc(ntables, sizeof *tabs) : nullptr;
            if (ok) {
                for (uint32_t i = 0; i < ntables; i++) {
                    tabs[i].tag = w.ent[i].tag;
                    tabs[i].data = w.ent[i].data;
                    tabs[i].len = w.ent[i].len;
                }
                ok = embi_sfnt_build(flavor, tabs, ntables, out);
                embi_free(tabs, ntables * sizeof *tabs);
            }
        } else {
            ok = w2_build_ttc(&w, out, max_out);
        }
    }
    if (!ok) {
        out->len = 0;
    }
    w2_free(&w);
    return ok;
}
