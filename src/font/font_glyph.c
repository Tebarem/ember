/*
 * glyf/loca outlines (simple and composite glyphs), glyph advances, and the outline dispatch
 * between glyf and CFF/CFF2.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_GLYF_MAX_DEPTH 8u
/* Components visited by one top-level outline request, across every nesting level. A
 * self-referencing composite otherwise costs N^depth. */
#define EMBI_GLYF_MAX_COMPONENTS 4096u

/* glyf flags */
#define GF_ON_CURVE 0x01u
#define GF_X_SHORT 0x02u
#define GF_Y_SHORT 0x04u
#define GF_REPEAT 0x08u
#define GF_X_SAME 0x10u
#define GF_Y_SAME 0x20u

/* composite flags */
#define CF_ARG_WORDS 0x0001u
#define CF_ARGS_XY 0x0002u
#define CF_HAVE_SCALE 0x0008u
#define CF_MORE 0x0020u
#define CF_XY_SCALE 0x0040u
#define CF_TWO_BY_TWO 0x0080u
#define CF_SCALED_OFFSET 0x0800u

/* Byte range of glyph gid in glyf. Empty for glyphs without outline data. */
static bool glyf_bytes(const embi_font *f, uint32_t gid, embi_bytes *g)
{
    *g = (embi_bytes){nullptr, 0};
    if (gid >= f->metrics.num_glyphs || f->glyf.n == 0) {
        return false;
    }
    uint32_t a = 0, b = 0;
    if (f->loca_long) {
        if (!bt_u32(f->loca, 4ull * gid, &a) || !bt_u32(f->loca, 4ull * gid + 4, &b)) {
            return false;
        }
    } else {
        if (!bt_u16(f->loca, 2ull * gid, &a) || !bt_u16(f->loca, 2ull * gid + 2, &b)) {
            return false;
        }
        a *= 2;
        b *= 2;
    }
    if (b < a) {
        return false;
    }
    return bt_sub(f->glyf, a, b - a, g);
}

/* Point count for gvar: simple glyphs have endPts[last] + 1 points, composites one per
 * component. Sets *composite. */
static bool glyf_point_count(const embi_font *f, uint32_t gid, uint32_t *npts, bool *composite)
{
    embi_bytes g;
    *npts = 0;
    *composite = false;
    if (!glyf_bytes(f, gid, &g)) {
        return false;
    }
    if (g.n == 0) {
        return true;
    }
    int32_t nc = 0;
    if (!bt_i16(g, 0, &nc)) {
        return false;
    }
    if (nc >= 0) {
        if (nc == 0) {
            return true;
        }
        uint32_t last = 0;
        if (!bt_u16(g, 10 + 2ull * (uint32_t)(nc - 1), &last)) {
            return false;
        }
        *npts = last + 1;
        return true;
    }
    *composite = true;
    uint64_t pos = 10;
    uint32_t flags = 0;
    uint32_t count = 0;
    do {
        if (!bt_u16(g, pos, &flags)) {
            return false;
        }
        pos += 4;
        pos += (flags & CF_ARG_WORDS) ? 4 : 2;
        if (flags & CF_HAVE_SCALE) {
            pos += 2;
        } else if (flags & CF_XY_SCALE) {
            pos += 4;
        } else if (flags & CF_TWO_BY_TWO) {
            pos += 8;
        }
        count++;
        if (count > EMBI_GLYF_MAX_COMPONENTS) {
            return false;
        }
    } while (flags & CF_MORE);
    *npts = count;
    return true;
}

/* Converts one simple contour (points p, on-curve flags on) to quads through the builder.
 * Handles contours that start off-curve and contours with no on-curve point at all. */
static bool glyf_contour(embi_olb *b, const embi_pt *p, const uint8_t *on, uint32_t n)
{
    if (n == 0) {
        return true;
    }
    uint32_t k = n;
    for (uint32_t i = 0; i < n; i++) {
        if (on[i] & GF_ON_CURVE) {
            k = i;
            break;
        }
    }
    embi_pt start;
    if (k < n) {
        start = p[k];
    } else {
        start = (embi_pt){(p[n - 1].x + p[0].x) * 0.5f, (p[n - 1].y + p[0].y) * 0.5f};
    }
    if (!embi_olb_move(b, start)) {
        return false;
    }
    bool havec = false;
    embi_pt ctrl = {0, 0};
    for (uint32_t j = 0; j < n; j++) {
        uint32_t idx = k < n ? (k + 1 + j) % n : j;
        embi_pt q = p[idx];
        if (on[idx] & GF_ON_CURVE) {
            bool ok = havec ? embi_olb_quad(b, ctrl, q) : embi_olb_line(b, q);
            if (!ok) {
                return false;
            }
            havec = false;
        } else if (havec) {
            embi_pt mid = {(ctrl.x + q.x) * 0.5f, (ctrl.y + q.y) * 0.5f};
            if (!embi_olb_quad(b, ctrl, mid)) {
                return false;
            }
            ctrl = q;
        } else {
            ctrl = q;
            havec = true;
        }
    }
    if (havec && !embi_olb_quad(b, ctrl, start)) {
        return false;
    }
    return embi_olb_close(b);
}

/* Simple glyph: decodes points, applies gvar deltas, and emits contours. All scratch arrays come
 * from one block. */
static bool glyf_simple(const embi_font *f, uint32_t gid, embi_bytes g, const int16_t *coords,
                        embi_outline *out)
{
    int32_t nc = 0;
    uint32_t last = 0;
    if (!bt_i16(g, 0, &nc) || nc <= 0 || !bt_u16(g, 10 + 2ull * (uint32_t)(nc - 1), &last)) {
        return false;
    }
    const size_t n = (size_t)last + 1;
    const size_t nctr = (size_t)nc;
    /* Layout: ends (u16 each), padding to 8, orig points, dx, dy (n + 4 each), flags. */
    const size_t off_orig = (2 * nctr + 7) & ~(size_t)7;
    const size_t off_dx = off_orig + n * sizeof(embi_pt);
    const size_t off_dy = off_dx + (n + 4) * sizeof(float);
    const size_t off_fl = off_dy + (n + 4) * sizeof(float);
    const size_t total = off_fl + n;
    uint8_t *blk = embi_alloc(total);
    if (blk == nullptr) {
        return false;
    }
    uint16_t *ends = (uint16_t *)blk;
    embi_pt *orig = (embi_pt *)(blk + off_orig);
    float *dx = (float *)(blk + off_dx);
    float *dy = (float *)(blk + off_dy);
    uint8_t *flags = blk + off_fl;
    const uint32_t npts = (uint32_t)n;

    bool ok = true;
    uint64_t pos = 10;
    uint32_t prev = 0;
    for (int32_t c = 0; ok && c < nc; c++) {
        uint32_t e = 0;
        if (!bt_u16(g, pos, &e) || (c > 0 && e <= prev) || e > last) {
            ok = false;
            break;
        }
        ends[c] = (uint16_t)e;
        prev = e;
        pos += 2;
    }
    uint32_t ilen = 0;
    if (ok && !bt_u16(g, pos, &ilen)) {
        ok = false;
    }
    pos += 2 + (uint64_t)ilen;
    /* Flags, with REPEAT runs. */
    for (uint32_t i = 0; ok && i < npts;) {
        uint32_t fl = 0;
        if (!bt_u8(g, pos++, &fl)) {
            ok = false;
            break;
        }
        uint32_t reps = 1;
        if (fl & GF_REPEAT) {
            uint32_t r = 0;
            if (!bt_u8(g, pos++, &r)) {
                ok = false;
                break;
            }
            reps = r + 1;
        }
        if (reps > npts - i) {
            ok = false;
            break;
        }
        for (uint32_t r = 0; r < reps; r++) {
            flags[i++] = (uint8_t)fl;
        }
    }
    /* x then y coordinates, as absolute values. */
    int32_t acc = 0;
    for (uint32_t i = 0; ok && i < npts; i++) {
        uint32_t fl = flags[i];
        if (fl & GF_X_SHORT) {
            uint32_t v = 0;
            if (!bt_u8(g, pos++, &v)) {
                ok = false;
                break;
            }
            acc += (fl & GF_X_SAME) ? (int32_t)v : -(int32_t)v;
        } else if (!(fl & GF_X_SAME)) {
            int32_t v = 0;
            if (!bt_i16(g, pos, &v)) {
                ok = false;
                break;
            }
            pos += 2;
            acc += v;
        }
        orig[i].x = (float)acc;
    }
    acc = 0;
    for (uint32_t i = 0; ok && i < npts; i++) {
        uint32_t fl = flags[i];
        if (fl & GF_Y_SHORT) {
            uint32_t v = 0;
            if (!bt_u8(g, pos++, &v)) {
                ok = false;
                break;
            }
            acc += (fl & GF_Y_SAME) ? (int32_t)v : -(int32_t)v;
        } else if (!(fl & GF_Y_SAME)) {
            int32_t v = 0;
            if (!bt_i16(g, pos, &v)) {
                ok = false;
                break;
            }
            pos += 2;
            acc += v;
        }
        orig[i].y = (float)acc;
    }
    if (ok && coords != nullptr && f->axis_count > 0 && f->gvar.n > 0) {
        ok = embi_gvar_glyph_deltas(f, gid, coords, orig, npts, ends, (uint32_t)nc, dx, dy);
        if (ok) {
            for (uint32_t i = 0; i < npts; i++) {
                orig[i].x += dx[i];
                orig[i].y += dy[i];
            }
        }
    }
    embi_olb b;
    embi_olb_init(&b, out);
    uint32_t s = 0;
    for (int32_t c = 0; ok && c < nc; c++) {
        uint32_t e = ends[c];
        ok = glyf_contour(&b, orig + s, flags + s, e - s + 1);
        s = e + 1;
    }
    embi_free(blk, total);
    return ok;
}

typedef struct glyf_comp {
    uint32_t gid;
    float xf[6];
} glyf_comp;

static bool glyf_outline_b(const embi_font *f, uint32_t gid, const int16_t *coords, uint32_t depth,
                           embi_outline *out, uint32_t *budget);

/* Composite glyph: reads components, applies gvar offset deltas, and appends each component's
 * outline under its transform. Components are charged to the shared budget. */
static bool glyf_composite(const embi_font *f, uint32_t gid, embi_bytes g, const int16_t *coords,
                           uint32_t depth, embi_outline *out, uint32_t *budget)
{
    uint32_t npts = 0;
    bool composite = false;
    if (!glyf_point_count(f, gid, &npts, &composite) || !composite || npts == 0) {
        return false;
    }
    if (npts > *budget) {
        return false;
    }
    *budget -= npts;
    const size_t n = npts;
    const size_t total = n * sizeof(glyf_comp) + 2 * (n + 4) * sizeof(float);
    uint8_t *blk = embi_alloc(total);
    if (blk == nullptr) {
        return false;
    }
    glyf_comp *comps = (glyf_comp *)blk;
    float *dx = (float *)(blk + n * sizeof(glyf_comp));
    float *dy = dx + (n + 4);
    bool ok = true;
    uint64_t pos = 10;
    for (uint32_t i = 0; ok && i < npts; i++) {
        uint32_t flags = 0, cgid = 0;
        if (!bt_u16(g, pos, &flags) || !bt_u16(g, pos + 2, &cgid)) {
            ok = false;
            break;
        }
        pos += 4;
        int32_t a1 = 0, a2 = 0;
        if (flags & CF_ARG_WORDS) {
            if (!bt_i16(g, pos, &a1) || !bt_i16(g, pos + 2, &a2)) {
                ok = false;
                break;
            }
            pos += 4;
        } else {
            uint32_t u1 = 0, u2 = 0;
            if (!bt_u8(g, pos, &u1) || !bt_u8(g, pos + 1, &u2)) {
                ok = false;
                break;
            }
            a1 = (int8_t)u1;
            a2 = (int8_t)u2;
            pos += 2;
        }
        float xs = 1, s01 = 0, s10 = 0, ys = 1;
        int32_t v = 0;
        if (flags & CF_HAVE_SCALE) {
            if (!bt_i16(g, pos, &v)) {
                ok = false;
                break;
            }
            xs = ys = f2dot14(v);
            pos += 2;
        } else if (flags & CF_XY_SCALE) {
            int32_t vy = 0;
            if (!bt_i16(g, pos, &v) || !bt_i16(g, pos + 2, &vy)) {
                ok = false;
                break;
            }
            xs = f2dot14(v);
            ys = f2dot14(vy);
            pos += 4;
        } else if (flags & CF_TWO_BY_TWO) {
            int32_t v1 = 0, v2 = 0, v3 = 0;
            if (!bt_i16(g, pos, &v) || !bt_i16(g, pos + 2, &v1) || !bt_i16(g, pos + 4, &v2) ||
                !bt_i16(g, pos + 6, &v3)) {
                ok = false;
                break;
            }
            xs = f2dot14(v);
            s01 = f2dot14(v1);
            s10 = f2dot14(v2);
            ys = f2dot14(v3);
            pos += 8;
        }
        /* Point matching (ARGS_ARE_XY_VALUES clear) is not supported: the offset stays zero. */
        float ox = 0, oy = 0;
        if (flags & CF_ARGS_XY) {
            ox = (float)a1;
            oy = (float)a2;
            if (flags & CF_SCALED_OFFSET) {
                /* FreeType and Apple scale the offset by the column magnitudes of the 2x2
                 * matrix. The matrix itself does not touch the offset. */
                ox *= SDL_sqrtf(xs * xs + s10 * s10);
                oy *= SDL_sqrtf(s01 * s01 + ys * ys);
            }
        }
        comps[i].gid = cgid;
        comps[i].xf[0] = xs;
        comps[i].xf[1] = s01;
        comps[i].xf[2] = s10;
        comps[i].xf[3] = ys;
        comps[i].xf[4] = ox;
        comps[i].xf[5] = oy;
        if (!(flags & CF_MORE)) {
            if (i + 1 != npts) {
                ok = false;
            }
            break;
        }
    }
    if (ok && coords != nullptr && f->axis_count > 0 && f->gvar.n > 0) {
        ok = embi_gvar_glyph_deltas(f, gid, coords, nullptr, npts, nullptr, 0, dx, dy);
        for (uint32_t i = 0; ok && i < npts; i++) {
            comps[i].xf[4] += dx[i];
            comps[i].xf[5] += dy[i];
        }
    }
    for (uint32_t i = 0; ok && i < npts; i++) {
        embi_outline tmp = {0};
        ok = glyf_outline_b(f, comps[i].gid, coords, depth + 1, &tmp, budget) &&
             embi_outline_append_xf(out, &tmp, comps[i].xf);
        embi_outline_free(&tmp);
    }
    embi_free(blk, total);
    return ok;
}

static bool glyf_outline_b(const embi_font *f, uint32_t gid, const int16_t *coords, uint32_t depth,
                           embi_outline *out, uint32_t *budget)
{
    if (depth > EMBI_GLYF_MAX_DEPTH) {
        return false;
    }
    embi_bytes g;
    if (f->glyf.n == 0 || !glyf_bytes(f, gid, &g)) {
        return false;
    }
    if (g.n == 0) {
        return true;
    }
    int32_t nc = 0;
    if (!bt_i16(g, 0, &nc)) {
        return false;
    }
    if (nc < 0) {
        return glyf_composite(f, gid, g, coords, depth, out, budget);
    }
    if (nc == 0) {
        return true;
    }
    return glyf_simple(f, gid, g, coords, out);
}

bool embi_glyf_outline(const embi_font *f, uint32_t gid, const int16_t *coords, uint32_t depth,
                       embi_outline *out)
{
    uint32_t budget = EMBI_GLYF_MAX_COMPONENTS;
    return glyf_outline_b(f, gid, coords, depth, out, &budget);
}

/* ------------------------------------------------------------------ public glyph API */

bool embi_font_glyph_advance(const embi_font *f, uint32_t gid, const int16_t *coords,
                             float *advance)
{
    if (gid >= f->metrics.num_glyphs || advance == nullptr) {
        return false;
    }
    uint32_t idx = gid < f->num_hmetrics ? gid : f->num_hmetrics - 1;
    uint32_t base = 0;
    if (!bt_u16(f->hmtx, 4ull * idx, &base)) {
        return false;
    }
    double a = base;
    if (coords != nullptr && f->axis_count > 0) {
        if (f->hvar_ivs.n > 0) {
            uint32_t packed = 0;
            if (!embi_var_map(f->hvar_adv_map, gid, &packed)) {
                return false;
            }
            a += embi_ivs_delta(f->hvar_ivs, packed >> 16, packed & 0xFFFFu, coords,
                                f->axis_count);
        } else if (f->gvar.n > 0 && f->glyf.n > 0) {
            uint32_t npts = 0;
            bool composite = false;
            if (!glyf_point_count(f, gid, &npts, &composite)) {
                return false;
            }
            const size_t total = 2 * ((size_t)npts + 4) * sizeof(float);
            float *blk = embi_alloc(total);
            if (blk == nullptr) {
                return false;
            }
            float *dx = blk;
            float *dy = blk + (npts + 4);
            bool ok = embi_gvar_glyph_deltas(f, gid, coords, nullptr, npts, nullptr, 0, dx, dy);
            if (ok) {
                a += (double)dx[npts + 1] - (double)dx[npts];
            }
            embi_free(blk, total);
            if (!ok) {
                return false;
            }
        }
    }
    *advance = (float)a;
    return true;
}

bool embi_font_glyph_outline(const embi_font *f, uint32_t gid, const int16_t *coords,
                             embi_outline *out)
{
    if (f == nullptr || out == nullptr || gid >= f->metrics.num_glyphs) {
        return false;
    }
    out->quads.len = 0;
    out->contours.len = 0;
    out->xmin = out->ymin = out->xmax = out->ymax = 0.0f;
    bool ok;
    if (f->cff_data != nullptr) {
        ok = embi_cff_outline(f, gid, coords, out);
    } else {
        ok = embi_glyf_outline(f, gid, coords, 0, out);
    }
    if (!ok) {
        out->quads.len = 0;
        out->contours.len = 0;
        return false;
    }
    embi_olb b;
    embi_olb_init(&b, out);
    return embi_olb_finish(&b);
}
