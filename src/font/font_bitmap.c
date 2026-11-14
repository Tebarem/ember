/*
 * Bitmap colour glyphs (W4i): the strikes of CBDT/CBLC (index formats 1 to 5, image formats 17, 18 and 19) and
 * sbix ('png ' graphic type, and 'dupe' references). Only the table structure is read here; the PNG bytes are
 * returned as a slice of the font data and decoded by the text module (src/text/draw.c). Every offset and count is
 * bounds-checked: malformed data drops the glyph, never reads outside the tables.
 */

#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

/* True when the n bytes at o lie inside b. */
static bool bytes_ok(embi_bytes b, uint64_t o, uint64_t n)
{
    return o <= b.n && b.n - o >= n;
}

/* The location of one glyph's image in CBDT: its index subtable and the image record. */
typedef struct bm_loc {
    uint32_t img_fmt;      /* 17, 18 or 19 */
    uint64_t off;          /* byte offset of the image record inside CBDT */
    uint32_t len;          /* bytes of the image record (for formats 2 and 5: the fixed image size) */
    bool has_big;          /* index format 2 or 5: bigMetrics of the glyph are in big[] */
    uint8_t big[8];
} bm_loc;

/* Index subtable of one glyph, from the index array of a strike. Returns false when the glyph is not in it. */
static bool bm_locate(embi_bytes cblc, embi_bytes cbdt, uint64_t sub, uint32_t gid, uint32_t first,
                      uint32_t last, bm_loc *out)
{
    uint32_t fmt = 0, img = 0, data_base = 0;
    if (!bt_u16(cblc, sub, &fmt) || !bt_u16(cblc, sub + 2, &img) || !bt_u32(cblc, sub + 4, &data_base)) {
        return false;
    }
    if (gid < first || gid > last) {
        return false;
    }
    uint64_t i = gid - first;
    bm_loc loc;
    SDL_memset(&loc, 0, sizeof loc);
    loc.img_fmt = img;
    uint32_t v0 = 0, v1 = 0;
    switch (fmt) {
    case 1: /* 32-bit offsets, one per glyph plus the end */
        if (!bt_u32(cblc, sub + 8 + 4 * i, &v0) || !bt_u32(cblc, sub + 8 + 4 * (i + 1), &v1) || v1 <= v0) {
            return false;
        }
        loc.off = (uint64_t)data_base + v0;
        loc.len = v1 - v0;
        break;
    case 2: { /* one image size, bigMetrics */
        uint32_t size = 0;
        if (!bt_u32(cblc, sub + 8, &size) || size == 0 || !bytes_ok(cblc, sub + 12, 8)) {
            return false;
        }
        for (uint32_t k = 0; k < 8; k++) {
            loc.big[k] = cblc.p[sub + 12 + k];
        }
        loc.has_big = true;
        loc.off = (uint64_t)data_base + i * size;
        loc.len = size;
        break;
    }
    case 3: /* 16-bit offsets */
        if (!bt_u16(cblc, sub + 8 + 2 * i, &v0) || !bt_u16(cblc, sub + 8 + 2 * (i + 1), &v1) || v1 <= v0) {
            return false;
        }
        loc.off = (uint64_t)data_base + v0;
        loc.len = v1 - v0;
        break;
    case 4: { /* sparse: (glyph, offset) pairs, the last pair ends the previous image */
        uint32_t n = 0;
        if (!bt_u32(cblc, sub + 8, &n)) {
            return false;
        }
        bool found = false;
        for (uint32_t j = 0; j < n && j < 65536u; j++) {
            uint32_t g = 0, o = 0, g1 = 0, o1 = 0;
            if (!bt_u16(cblc, sub + 12 + 4 * (uint64_t)j, &g) || !bt_u16(cblc, sub + 14 + 4 * (uint64_t)j, &o)) {
                return false;
            }
            if (g != gid) {
                continue;
            }
            if (!bt_u16(cblc, sub + 12 + 4 * (uint64_t)j + 4, &g1) ||
                !bt_u16(cblc, sub + 14 + 4 * (uint64_t)j + 4, &o1) || o1 <= o) {
                return false;
            }
            loc.off = (uint64_t)data_base + o;
            loc.len = o1 - o;
            found = true;
            break;
        }
        if (!found) {
            return false;
        }
        break;
    }
    case 5: { /* one image size, bigMetrics, then the glyph id list */
        uint32_t size = 0, n = 0;
        if (!bt_u32(cblc, sub + 8, &size) || size == 0 || !bytes_ok(cblc, sub + 12, 8) ||
            !bt_u32(cblc, sub + 20, &n)) {
            return false;
        }
        bool found = false;
        for (uint32_t j = 0; j < n && j < 65536u; j++) {
            uint32_t g = 0;
            if (!bt_u16(cblc, sub + 24 + 2 * (uint64_t)j, &g)) {
                return false;
            }
            if (g == gid) {
                for (uint32_t k = 0; k < 8; k++) {
                    loc.big[k] = cblc.p[sub + 12 + k];
                }
                loc.has_big = true;
                loc.off = (uint64_t)data_base + (uint64_t)j * size;
                loc.len = size;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    (void)cbdt;
    *out = loc;
    return true;
}

/* Reads the image record of a CBDT location: the metrics (formats 17 and 18) and the PNG slice. */
static bool bm_image(embi_bytes cbdt, const bm_loc *loc, embi_bitmap_glyph *g)
{
    if (loc->off > cbdt.n) {
        return false;
    }
    uint64_t at = loc->off;
    uint64_t lim = loc->off + loc->len;
    if (lim > cbdt.n) {
        lim = cbdt.n;
    }
    uint32_t dlen = 0;
    uint64_t png_at = 0;
    if (loc->img_fmt == 17) {
        /* smallGlyphMetrics: height, width, bearingX, bearingY, advance (5 bytes), then dataLen. */
        if (lim - at < 9) {
            return false;
        }
        g->height = cbdt.p[at];
        g->width = cbdt.p[at + 1];
        g->left = (float)(int8_t)cbdt.p[at + 2];
        g->top = (float)(int8_t)cbdt.p[at + 3];
        if (!bt_u32(cbdt, at + 5, &dlen)) {
            return false;
        }
        png_at = at + 9;
    } else if (loc->img_fmt == 18) {
        /* bigGlyphMetrics: height, width, horiBearingX, horiBearingY, horiAdvance, vert... (8 bytes), dataLen. */
        if (lim - at < 12) {
            return false;
        }
        g->height = cbdt.p[at];
        g->width = cbdt.p[at + 1];
        g->left = (float)(int8_t)cbdt.p[at + 2];
        g->top = (float)(int8_t)cbdt.p[at + 3];
        if (!bt_u32(cbdt, at + 8, &dlen)) {
            return false;
        }
        png_at = at + 12;
    } else if (loc->img_fmt == 19) {
        /* The metrics come from the index (bigMetrics of formats 2 and 5), else from the PNG itself. */
        if (lim - at < 4) {
            return false;
        }
        if (!bt_u32(cbdt, at, &dlen)) {
            return false;
        }
        png_at = at + 4;
        if (loc->has_big) {
            g->height = loc->big[0];
            g->width = loc->big[1];
            g->left = (float)(int8_t)loc->big[2];
            g->top = (float)(int8_t)loc->big[3];
        }
    } else {
        return false;
    }
    if (dlen == 0 || png_at > cbdt.n || dlen > cbdt.n - png_at) {
        return false;
    }
    g->png = cbdt.p + png_at;
    g->len = dlen;
    return true;
}

bool embi_font_has_bitmaps(const embi_font *f)
{
    return f != nullptr && ((f->cblc.n > 0 && f->cbdt.n > 0) || f->sbix.n > 0);
}

/* Keeps the strike with the best rank for target: the smallest ppem at least target, else the largest one. */
static bool better_strike(uint32_t ppem, uint32_t best, bool have, float target)
{
    if (!have) {
        return true;
    }
    bool cur_big = (float)ppem >= target;
    bool best_big = (float)best >= target;
    if (cur_big && best_big) {
        return ppem < best;
    }
    if (cur_big != best_big) {
        return cur_big;
    }
    return ppem > best;
}

/* CBDT/CBLC: every strike of the table that has the glyph. */
static bool cbdt_glyph(const embi_font *f, uint32_t gid, float target, embi_bitmap_glyph *out)
{
    embi_bytes cblc = f->cblc, cbdt = f->cbdt;
    uint32_t nsizes = 0;
    if (!bt_u32(cblc, 4, &nsizes)) {
        return false;
    }
    bool have = false;
    uint32_t best_ppem = 0;
    bm_loc best_loc;
    SDL_memset(&best_loc, 0, sizeof best_loc);
    for (uint32_t s = 0; s < nsizes && s < 4096u; s++) {
        uint64_t rec = 8 + 48ull * s;
        uint32_t arr = 0, nsub = 0, sg = 0, eg = 0, ppem = 0;
        if (!bt_u32(cblc, rec, &arr) || !bt_u32(cblc, rec + 8, &nsub) || !bt_u16(cblc, rec + 40, &sg) ||
            !bt_u16(cblc, rec + 42, &eg) || !bt_u8(cblc, rec + 44, &ppem)) {
            break;
        }
        if (gid < sg || gid > eg || ppem == 0) {
            continue;
        }
        if (!better_strike(ppem, best_ppem, have, target)) {
            continue;
        }
        for (uint32_t k = 0; k < nsub && k < 65536u; k++) {
            uint64_t e = arr + 8ull * k;
            uint32_t first = 0, last = 0, add = 0;
            if (!bt_u16(cblc, e, &first) || !bt_u16(cblc, e + 2, &last) || !bt_u32(cblc, e + 4, &add)) {
                break;
            }
            if (gid < first || gid > last) {
                continue;
            }
            bm_loc loc;
            if (bm_locate(cblc, cbdt, arr + add, gid, first, last, &loc)) {
                /* Only strikes whose image parses count. */
                embi_bitmap_glyph tmp;
                SDL_memset(&tmp, 0, sizeof tmp);
                if (bm_image(cbdt, &loc, &tmp) && better_strike(ppem, best_ppem, have, target)) {
                    have = true;
                    best_ppem = ppem;
                    best_loc = loc;
                }
            }
            break;
        }
    }
    if (!have) {
        return false;
    }
    SDL_memset(out, 0, sizeof *out);
    out->ppem = best_ppem;
    return bm_image(cbdt, &best_loc, out);
}

/* sbix: every strike with the glyph's data. */
static bool sbix_glyph(const embi_font *f, uint32_t gid, float target, embi_bitmap_glyph *out, uint32_t depth)
{
    embi_bytes sb = f->sbix;
    uint32_t nstrikes = 0;
    if (!bt_u32(sb, 4, &nstrikes)) {
        return false;
    }
    uint32_t ng = f->metrics.num_glyphs;
    if (gid >= ng) {
        return false;
    }
    bool have = false;
    uint32_t best_ppem = 0;
    uint64_t best_at = 0, best_len = 0;
    int32_t best_ox = 0, best_oy = 0;
    for (uint32_t s = 0; s < nstrikes && s < 4096u; s++) {
        uint32_t base = 0;
        if (!bt_u32(sb, 8 + 4ull * s, &base) || base == 0 || base >= sb.n) {
            break;
        }
        uint32_t ppem = 0;
        if (!bt_u16(sb, base, &ppem) || ppem == 0) {
            continue;
        }
        uint32_t o0 = 0, o1 = 0;
        uint64_t oa = (uint64_t)base + 4 + 4ull * gid;
        if (!bt_u32(sb, oa, &o0) || !bt_u32(sb, oa + 4, &o1) || o1 <= o0) {
            continue;
        }
        if (!better_strike(ppem, best_ppem, have, target)) {
            continue;
        }
        uint64_t at = (uint64_t)base + o0;
        uint64_t n = o1 - o0;
        int32_t ox = 0, oy = 0;
        if (n < 8 || !bytes_ok(sb, at, n) || !bt_i16(sb, at, &ox) || !bt_i16(sb, at + 2, &oy)) {
            continue;
        }
        have = true;
        best_ppem = ppem;
        best_at = at;
        best_len = n;
        best_ox = ox;
        best_oy = oy;
    }
    if (!have) {
        return false;
    }
    embi_bytes gtype = {nullptr, 0};
    gtype.p = sb.p + best_at + 4;
    gtype.n = 4;
    /* graphicType 'dupe': the data is a glyph id whose strike data is used (one level). */
    if (gtype.p[0] == 'd' && gtype.p[1] == 'u' && gtype.p[2] == 'p' && gtype.p[3] == 'e') {
        uint32_t ref = 0;
        if (depth > 0 || !bt_u16(sb, best_at + 8, &ref)) {
            return false;
        }
        return sbix_glyph(f, ref, target, out, depth + 1);
    }
    if (!(gtype.p[0] == 'p' && gtype.p[1] == 'n' && gtype.p[2] == 'g' && gtype.p[3] == ' ')) {
        return false;
    }
    if (best_len <= 8) {
        return false;
    }
    SDL_memset(out, 0, sizeof *out);
    out->ppem = best_ppem;
    out->png = sb.p + best_at + 8;
    out->len = (uint32_t)(best_len - 8);
    out->left = (float)best_ox;
    out->origin_y = (float)best_oy;
    out->top_from_bottom = true;
    return true;
}

bool embi_font_bitmap_glyph(const embi_font *f, uint32_t gid, float target_ppem, embi_bitmap_glyph *out)
{
    if (f == nullptr || out == nullptr || !(target_ppem > 0.0f)) {
        return false;
    }
    if (f->cblc.n > 0 && f->cbdt.n > 0 && cbdt_glyph(f, gid, target_ppem, out)) {
        return true;
    }
    if (f->sbix.n > 0) {
        return sbix_glyph(f, gid, target_ppem, out, 0);
    }
    return false;
}
