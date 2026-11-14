/*
 * CPAL palettes and COLR glyph painting: COLRv0 layers and the COLRv1 paint graph, flattened into
 * the embi_colr_op stream through a visitor.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_COLR_MAX_DEPTH 64u
#define EMBI_COLR_MAX_OPS 65536u
/* Paint nodes visited per call. Layers and COLR glyph references can nest with no emitting paint,
 * so the op budget alone does not bound the work. */
#define EMBI_COLR_MAX_EVALS 65536u
/* Gradient stops read per call: a hostile ColorLine can hold 65535 stops and be referenced by
 * every evaluation, so the eval budget alone does not bound the stop work. */
#define EMBI_COLR_MAX_STOPS 1000000u
#define EMBI_COLR_SORT_MAX 256u
#define EMBI_COLR_NO_VAR 0xFFFFFFFFu
#define EMBI_COLR_PI 3.14159265358979323846

/* ------------------------------------------------------------------ CPAL */

uint32_t embi_font_palette_count(const embi_font *f)
{
    uint32_t n = 0;
    if (f->cpal.n == 0 || !bt_u16(f->cpal, 4, &n)) {
        return 0;
    }
    return n;
}

bool embi_font_cpal_color(const embi_font *f, uint32_t palette, uint32_t entry, uint8_t rgba[4])
{
    embi_bytes c = f->cpal;
    uint32_t nentries = 0, npal = 0, nrec = 0, rec_off = 0, rec_idx = 0;
    if (c.n == 0 || !bt_u16(c, 2, &nentries) || !bt_u16(c, 4, &npal) || !bt_u16(c, 6, &nrec) ||
        !bt_u32(c, 8, &rec_off)) {
        return false;
    }
    if (palette >= npal || entry >= nentries) {
        return false;
    }
    if (!bt_u16(c, 12 + 2ull * palette, &rec_idx)) {
        return false;
    }
    uint32_t r = rec_idx + entry;
    if (r >= nrec) {
        return false;
    }
    /* Records are stored blue, green, red, alpha. */
    uint32_t b = 0, g = 0, rr = 0, a = 0;
    uint64_t p = rec_off + 4ull * r;
    if (!bt_u8(c, p, &b) || !bt_u8(c, p + 1, &g) || !bt_u8(c, p + 2, &rr) || !bt_u8(c, p + 3, &a)) {
        return false;
    }
    rgba[0] = (uint8_t)rr;
    rgba[1] = (uint8_t)g;
    rgba[2] = (uint8_t)b;
    rgba[3] = (uint8_t)a;
    return true;
}

/* ------------------------------------------------------------------ COLR helpers */

typedef struct colr_ctx {
    const embi_font *f;
    embi_bytes colr;
    embi_bytes ivs;     /* ItemVariationStore (may be empty) */
    embi_bytes varmap;  /* DeltaSetIndexMap (may be empty) */
    const int16_t *coords;
    uint32_t nc;
    uint32_t palette;
    const embi_colr_visitor *v;
    uint32_t ops;
    uint32_t evals;
    uint32_t stops_read;
    bool stopped;
} colr_ctx;

static bool emit(colr_ctx *c, const embi_colr_op *op)
{
    if (c->stopped || ++c->ops > EMBI_COLR_MAX_OPS) {
        c->stopped = true;
        return false;
    }
    if (!c->v->op(c->v->ctx, op)) {
        c->stopped = true;
        return false;
    }
    return true;
}

/* Variation delta for field `i` of a Var table whose base index is varbase. */
static double colr_delta(const colr_ctx *c, uint32_t varbase, uint32_t i)
{
    if (varbase == EMBI_COLR_NO_VAR || c->ivs.n == 0) {
        return 0.0;
    }
    uint32_t packed = 0;
    uint32_t vi = varbase + i;
    if (!embi_var_map(c->varmap, vi, &packed)) {
        return 0.0;
    }
    return embi_ivs_delta(c->ivs, packed >> 16, packed & 0xFFFFu, c->coords, c->nc);
}

static bool read_varbase(embi_bytes t, uint64_t off, uint32_t *vb)
{
    return bt_u32(t, off, vb);
}

/* Palette colour with alpha multiplied in. palette 0xFFFF sets *fg and keeps only the alpha. */
static void colr_color(const colr_ctx *c, uint32_t pi, float alpha, float rgba[4], bool *fg)
{
    *fg = pi == 0xFFFFu;
    rgba[0] = rgba[1] = rgba[2] = 0.0f;
    rgba[3] = alpha;
    if (*fg) {
        return;
    }
    uint8_t px[4] = {0, 0, 0, 255};
    if (embi_font_cpal_color(c->f, c->palette, pi, px)) {
        rgba[0] = px[0] / 255.0f;
        rgba[1] = px[1] / 255.0f;
        rgba[2] = px[2] / 255.0f;
        rgba[3] = (px[3] / 255.0f) * alpha;
    }
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ------------------------------------------------------------------ paints */

static bool paint_eval(colr_ctx *c, uint64_t off, uint32_t depth);

/* Finds the BaseGlyphList paint for gid. *abs receives its offset inside the COLR table. */
static bool base_paint_lookup(embi_bytes t, uint32_t gid, uint64_t *abs)
{
    uint32_t bl = 0;
    if (!bt_u32(t, 14, &bl) || bl == 0) {
        return false;
    }
    uint32_t nb = 0;
    if (!bt_u32(t, bl, &nb)) {
        return false;
    }
    uint32_t lo = 0, hi = nb;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t g = 0, po = 0;
        uint64_t rec = (uint64_t)bl + 4 + 6ull * mid;
        if (!bt_u16(t, rec, &g) || !bt_u32(t, rec + 2, &po)) {
            return false;
        }
        if (g < gid) {
            lo = mid + 1;
        } else if (g > gid) {
            hi = mid;
        } else {
            *abs = (uint64_t)bl + po;
            return true;
        }
    }
    return false;
}

/* Reads a ColorLine (or VarColorLine when var is set) at cl_off and emits the gradient paint. p
 * holds the geometry. A line with no stops emits nothing; one stop is a solid fill, as the spec
 * and common renderers define it. */
static bool emit_gradient(colr_ctx *c, uint64_t cl_off, embi_colr_paint_kind kind,
                          const float p[6], bool var)
{
    uint32_t extend = 0, nstops = 0;
    if (!bt_u8(c->colr, cl_off, &extend) || !bt_u16(c->colr, cl_off + 1, &nstops)) {
        return false;
    }
    if (nstops == 0) {
        return true;
    }
    /* ColorStop is 6 bytes; VarColorStop adds varIndexBase (u32): 10 bytes. */
    const uint64_t stride = var ? 10u : 6u;
    if (nstops > EMBI_COLR_MAX_STOPS - c->stops_read) {
        c->stopped = true;
        return false;
    }
    c->stops_read += nstops;
    embi_colr_stop *stops = embi_alloc((size_t)nstops * sizeof(embi_colr_stop));
    if (stops == nullptr) {
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < nstops; i++) {
        uint32_t so = 0, pi = 0, al = 0, vb = EMBI_COLR_NO_VAR;
        uint64_t rec = cl_off + 3 + stride * i;
        if (!bt_u16(c->colr, rec, &so) || !bt_u16(c->colr, rec + 2, &pi) ||
            !bt_u16(c->colr, rec + 4, &al) || (var && !read_varbase(c->colr, rec + 6, &vb))) {
            ok = false;
            break;
        }
        float offset = f2dot14((int16_t)so);
        float alpha = f2dot14((int16_t)al);
        if (var) {
            /* varIndexBase + 0 is the stop offset delta, + 1 the alpha delta. */
            offset += (float)(colr_delta(c, vb, 0) / 16384.0);
            alpha += (float)(colr_delta(c, vb, 1) / 16384.0);
        }
        stops[i].offset = offset; /* not clamped: the extend mode handles values outside 0..1 */
        colr_color(c, pi, clamp01(alpha), stops[i].rgba, &stops[i].foreground);
    }
    /* Stable insertion sort by offset (small lines only; larger ones keep font order). */
    if (ok && nstops > 1 && nstops <= EMBI_COLR_SORT_MAX) {
        for (uint32_t i = 1; i < nstops; i++) {
            embi_colr_stop t = stops[i];
            uint32_t j = i;
            while (j > 0 && stops[j - 1].offset > t.offset) {
                stops[j] = stops[j - 1];
                j--;
            }
            stops[j] = t;
        }
    }
    if (ok && nstops == 1) {
        embi_colr_op op = {0};
        op.kind = EMBI_COLR_PAINT;
        op.paint.kind = EMBI_PAINT_SOLID;
        for (int i = 0; i < 4; i++) {
            op.paint.rgba[i] = stops[0].rgba[i];
        }
        op.paint.foreground = stops[0].foreground;
        ok = emit(c, &op);
    } else if (ok) {
        embi_colr_op op = {0};
        op.kind = EMBI_COLR_PAINT;
        op.paint.kind = kind;
        op.paint.extend = (uint8_t)(extend > 2 ? 0 : extend);
        op.paint.stops = stops;
        op.paint.num_stops = nstops;
        for (int i = 0; i < 6; i++) {
            op.paint.p[i] = p[i];
        }
        ok = emit(c, &op);
    }
    embi_free(stops, (size_t)nstops * sizeof(embi_colr_stop));
    return ok;
}

/* Emits a transform push, evaluates the child paint, and emits the pop. */
static bool with_transform(colr_ctx *c, const float m[6], uint64_t child, uint32_t depth)
{
    embi_colr_op op = {0};
    op.kind = EMBI_COLR_TRANSFORM_PUSH;
    for (int i = 0; i < 6; i++) {
        op.xf[i] = m[i];
    }
    if (!emit(c, &op)) {
        return false;
    }
    bool ok = paint_eval(c, child, depth + 1);
    embi_colr_op pop = {0};
    pop.kind = EMBI_COLR_TRANSFORM_POP;
    return emit(c, &pop) && ok;
}

/* Composes an affine matrix with a translation around the centre (cx, cy): T(c) M T(-c). */
static void around_center(float m[6], float cx, float cy)
{
    m[4] = cx - (m[0] * cx + m[2] * cy);
    m[5] = cy - (m[1] * cx + m[3] * cy);
}

static bool paint_eval(colr_ctx *c, uint64_t off, uint32_t depth)
{
    if (depth > EMBI_COLR_MAX_DEPTH || c->stopped) {
        return false;
    }
    if (++c->evals > EMBI_COLR_MAX_EVALS) {
        c->stopped = true;
        return false;
    }
    uint32_t fmt = 0;
    if (!bt_u8(c->colr, off, &fmt)) {
        return false;
    }
    embi_bytes t = c->colr;
    switch (fmt) {
    case 1: { /* PaintColrLayers */
        uint32_t n = 0, first = 0;
        if (!bt_u8(t, off + 1, &n) || !bt_u32(t, off + 2, &first)) {
            return false;
        }
        uint32_t lloff = 0;
        uint32_t nlayers = 0;
        if (!bt_u32(t, 18, &lloff) || lloff == 0 || !bt_u32(t, lloff, &nlayers)) {
            return false;
        }
        if ((uint64_t)first + n > nlayers) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t po = 0;
            if (!bt_u32(t, (uint64_t)lloff + 4 + 4ull * (first + i), &po)) {
                return false;
            }
            if (!paint_eval(c, (uint64_t)lloff + po, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case 2:  /* PaintSolid */
    case 3: { /* PaintVarSolid */
        uint32_t pi = 0, al = 0;
        if (!bt_u16(t, off + 1, &pi) || !bt_u16(t, off + 3, &al)) {
            return false;
        }
        float alpha = f2dot14((int16_t)al);
        if (fmt == 3) {
            uint32_t vb = 0;
            if (!read_varbase(t, off + 5, &vb)) {
                return false;
            }
            alpha += (float)(colr_delta(c, vb, 0) / 16384.0);
        }
        embi_colr_op op = {0};
        op.kind = EMBI_COLR_PAINT;
        op.paint.kind = EMBI_PAINT_SOLID;
        bool fg = false;
        colr_color(c, pi, clamp01(alpha), op.paint.rgba, &fg);
        op.paint.foreground = fg;
        return emit(c, &op);
    }
    case 4:  /* PaintLinearGradient */
    case 5: { /* PaintVarLinearGradient */
        uint32_t cl = 0;
        if (!bt_u24(t, off + 1, &cl)) {
            return false;
        }
        int32_t v[6];
        for (int i = 0; i < 6; i++) {
            if (!bt_i16(t, off + 4 + 2ull * i, &v[i])) {
                return false;
            }
        }
        float p[6];
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (fmt == 5 && !read_varbase(t, off + 16, &vb)) {
            return false;
        }
        for (int i = 0; i < 6; i++) {
            p[i] = (float)v[i] + (float)colr_delta(c, vb, (uint32_t)i);
        }
        return emit_gradient(c, (uint64_t)off + cl, EMBI_PAINT_LINEAR, p, fmt == 5);
    }
    case 6:  /* PaintRadialGradient */
    case 7: { /* PaintVarRadialGradient */
        uint32_t cl = 0;
        if (!bt_u24(t, off + 1, &cl)) {
            return false;
        }
        /* x0, y0 (FWORD), r0 (UFWORD), x1, y1 (FWORD), r1 (UFWORD). */
        int32_t v[6];
        uint32_t r0 = 0, r1 = 0;
        if (!bt_i16(t, off + 4, &v[0]) || !bt_i16(t, off + 6, &v[1]) ||
            !bt_u16(t, off + 8, &r0) || !bt_i16(t, off + 10, &v[3]) ||
            !bt_i16(t, off + 12, &v[4]) || !bt_u16(t, off + 14, &r1)) {
            return false;
        }
        v[2] = (int32_t)r0;
        v[5] = (int32_t)r1;
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (fmt == 7 && !read_varbase(t, off + 16, &vb)) {
            return false;
        }
        float p[6];
        for (int i = 0; i < 6; i++) {
            p[i] = (float)v[i] + (float)colr_delta(c, vb, (uint32_t)i);
        }
        return emit_gradient(c, (uint64_t)off + cl, EMBI_PAINT_RADIAL, p, fmt == 7);
    }
    case 8:  /* PaintSweepGradient */
    case 9: { /* PaintVarSweepGradient */
        uint32_t cl = 0;
        if (!bt_u24(t, off + 1, &cl)) {
            return false;
        }
        int32_t cx = 0, cy = 0, sa = 0, ea = 0;
        if (!bt_i16(t, off + 4, &cx) || !bt_i16(t, off + 6, &cy) || !bt_i16(t, off + 8, &sa) ||
            !bt_i16(t, off + 10, &ea)) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (fmt == 9 && !read_varbase(t, off + 12, &vb)) {
            return false;
        }
        float p[6] = {0};
        p[0] = (float)cx + (float)colr_delta(c, vb, 0);
        p[1] = (float)cy + (float)colr_delta(c, vb, 1);
        p[2] = (float)(f2dot14(sa) * 180.0) + (float)(colr_delta(c, vb, 2) / 16384.0 * 180.0);
        p[3] = (float)(f2dot14(ea) * 180.0) + (float)(colr_delta(c, vb, 3) / 16384.0 * 180.0);
        return emit_gradient(c, (uint64_t)off + cl, EMBI_PAINT_SWEEP, p, fmt == 9);
    }
    case 10: { /* PaintGlyph */
        uint32_t po = 0, gid = 0;
        if (!bt_u24(t, off + 1, &po) || !bt_u16(t, off + 4, &gid)) {
            return false;
        }
        embi_colr_op clip = {0};
        clip.kind = EMBI_COLR_CLIP_PUSH;
        clip.gid = gid;
        if (!emit(c, &clip)) {
            return false;
        }
        bool ok = paint_eval(c, off + po, depth + 1);
        embi_colr_op pop = {0};
        pop.kind = EMBI_COLR_CLIP_POP;
        return emit(c, &pop) && ok;
    }
    case 11: { /* PaintColrGlyph */
        uint32_t gid = 0;
        uint64_t base = 0;
        if (!bt_u16(t, off + 1, &gid) || !base_paint_lookup(t, gid, &base)) {
            return false;
        }
        return paint_eval(c, base, depth + 1);
    }
    case 12:  /* PaintTransform */
    case 13: { /* PaintVarTransform */
        uint32_t child = 0, tro = 0;
        if (!bt_u24(t, off + 1, &child) || !bt_u24(t, off + 4, &tro)) {
            return false;
        }
        int32_t v[6];
        uint64_t ta = (uint64_t)off + tro;
        for (int i = 0; i < 6; i++) {
            uint32_t u = 0;
            if (!bt_u32(t, ta + 4ull * i, &u)) {
                return false;
            }
            v[i] = (int32_t)u;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (fmt == 13 && !read_varbase(t, ta + 24, &vb)) {
            return false;
        }
        float m[6];
        for (int i = 0; i < 6; i++) {
            /* Fixed 16.16 values; variation deltas are in the same units. */
            m[i] = (float)(v[i] / 65536.0 + colr_delta(c, vb, (uint32_t)i) / 65536.0);
        }
        /* xx, yx, xy, yy, dx, dy -> a, b, c, d, e, f. */
        float a[6] = {m[0], m[1], m[2], m[3], m[4], m[5]};
        return with_transform(c, a, (uint64_t)off + child, depth);
    }
    case 14:  /* PaintTranslate */
    case 15: { /* PaintVarTranslate */
        uint32_t child = 0;
        int32_t dx = 0, dy = 0;
        if (!bt_u24(t, off + 1, &child) || !bt_i16(t, off + 4, &dx) || !bt_i16(t, off + 6, &dy)) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (fmt == 15 && !read_varbase(t, off + 8, &vb)) {
            return false;
        }
        float m[6] = {1, 0, 0, 1, dx + (float)colr_delta(c, vb, 0),
                      dy + (float)colr_delta(c, vb, 1)};
        return with_transform(c, m, (uint64_t)off + child, depth);
    }
    case 16:  /* PaintScale */
    case 17:  /* PaintVarScale */
    case 18:  /* PaintScaleAroundCenter */
    case 19: { /* PaintVarScaleAroundCenter */
        uint32_t child = 0;
        int32_t sx = 0, sy = 0, cx = 0, cy = 0;
        bool around = fmt == 18 || fmt == 19;
        bool var = fmt == 17 || fmt == 19;
        if (!bt_u24(t, off + 1, &child) || !bt_i16(t, off + 4, &sx) || !bt_i16(t, off + 6, &sy)) {
            return false;
        }
        if (around && (!bt_i16(t, off + 8, &cx) || !bt_i16(t, off + 10, &cy))) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (var && !read_varbase(t, off + (around ? 12 : 8), &vb)) {
            return false;
        }
        float ssx = f2dot14(sx) + (float)(colr_delta(c, vb, 0) / 16384.0);
        float ssy = f2dot14(sy) + (float)(colr_delta(c, vb, 1) / 16384.0);
        float m[6] = {ssx, 0, 0, ssy, 0, 0};
        if (around) {
            around_center(m, (float)cx + (float)colr_delta(c, vb, 2),
                          (float)cy + (float)colr_delta(c, vb, 3));
        }
        return with_transform(c, m, (uint64_t)off + child, depth);
    }
    case 20:  /* PaintScaleUniform */
    case 21:  /* PaintVarScaleUniform */
    case 22:  /* PaintScaleUniformAroundCenter */
    case 23: { /* PaintVarScaleUniformAroundCenter */
        uint32_t child = 0;
        int32_t s = 0, cx = 0, cy = 0;
        bool around = fmt == 22 || fmt == 23;
        bool var = fmt == 21 || fmt == 23;
        if (!bt_u24(t, off + 1, &child) || !bt_i16(t, off + 4, &s)) {
            return false;
        }
        if (around && (!bt_i16(t, off + 6, &cx) || !bt_i16(t, off + 8, &cy))) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (var && !read_varbase(t, off + (around ? 10 : 6), &vb)) {
            return false;
        }
        float ss = f2dot14(s) + (float)(colr_delta(c, vb, 0) / 16384.0);
        float m[6] = {ss, 0, 0, ss, 0, 0};
        if (around) {
            around_center(m, (float)cx + (float)colr_delta(c, vb, 1),
                          (float)cy + (float)colr_delta(c, vb, 2));
        }
        return with_transform(c, m, (uint64_t)off + child, depth);
    }
    case 24:  /* PaintRotate */
    case 25:  /* PaintVarRotate */
    case 26:  /* PaintRotateAroundCenter */
    case 27: { /* PaintVarRotateAroundCenter */
        uint32_t child = 0;
        int32_t ang = 0, cx = 0, cy = 0;
        bool around = fmt == 26 || fmt == 27;
        bool var = fmt == 25 || fmt == 27;
        if (!bt_u24(t, off + 1, &child) || !bt_i16(t, off + 4, &ang)) {
            return false;
        }
        if (around && (!bt_i16(t, off + 6, &cx) || !bt_i16(t, off + 8, &cy))) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (var && !read_varbase(t, off + (around ? 10 : 6), &vb)) {
            return false;
        }
        /* Angle in semicircles, counter-clockwise in y-up font space. */
        double th = (f2dot14(ang) + colr_delta(c, vb, 0) / 16384.0) * EMBI_COLR_PI;
        float cs = (float)SDL_cos(th), sn = (float)SDL_sin(th);
        float m[6] = {cs, sn, -sn, cs, 0, 0};
        if (around) {
            around_center(m, (float)cx + (float)colr_delta(c, vb, 1),
                          (float)cy + (float)colr_delta(c, vb, 2));
        }
        return with_transform(c, m, (uint64_t)off + child, depth);
    }
    case 28:  /* PaintSkew */
    case 29:  /* PaintVarSkew */
    case 30:  /* PaintSkewAroundCenter */
    case 31: { /* PaintVarSkewAroundCenter */
        uint32_t child = 0;
        int32_t xs = 0, ys = 0, cx = 0, cy = 0;
        bool around = fmt == 30 || fmt == 31;
        bool var = fmt == 29 || fmt == 31;
        if (!bt_u24(t, off + 1, &child) || !bt_i16(t, off + 4, &xs) || !bt_i16(t, off + 6, &ys)) {
            return false;
        }
        if (around && (!bt_i16(t, off + 8, &cx) || !bt_i16(t, off + 10, &cy))) {
            return false;
        }
        uint32_t vb = EMBI_COLR_NO_VAR;
        if (var && !read_varbase(t, off + (around ? 12 : 8), &vb)) {
            return false;
        }
        double ax = (f2dot14(xs) + colr_delta(c, vb, 0) / 16384.0) * EMBI_COLR_PI;
        double ay = (f2dot14(ys) + colr_delta(c, vb, 1) / 16384.0) * EMBI_COLR_PI;
        /* x' = x + tan(xSkew) y, y' = y + tan(ySkew) x. */
        /* Skew angles are counter-clockwise like rotation, so the x skew tilts the top of a
         * vertical edge to the left: x' = x - tan(ax) y, y' = y + tan(ay) x (fontTools uses
         * skew(-xSkew, ySkew)). */
        float m[6] = {1, (float)SDL_tan(ay), (float)-SDL_tan(ax), 1, 0, 0};
        if (around) {
            around_center(m, (float)cx + (float)colr_delta(c, vb, 2),
                          (float)cy + (float)colr_delta(c, vb, 3));
        }
        return with_transform(c, m, (uint64_t)off + child, depth);
    }
    case 32: { /* PaintComposite */
        uint32_t src = 0, bd = 0, mode = 0;
        if (!bt_u24(t, off + 1, &src) || !bt_u8(t, off + 4, &mode) || !bt_u24(t, off + 5, &bd)) {
            return false;
        }
        if (!paint_eval(c, (uint64_t)off + bd, depth + 1)) {
            return false;
        }
        embi_colr_op g = {0};
        g.kind = EMBI_COLR_GROUP_PUSH;
        g.composite = (uint8_t)mode;
        if (!emit(c, &g)) {
            return false;
        }
        bool ok = paint_eval(c, (uint64_t)off + src, depth + 1);
        embi_colr_op pop = {0};
        pop.kind = EMBI_COLR_GROUP_POP;
        return emit(c, &pop) && ok;
    }
    default:
        return false;
    }
}

/* COLRv0: layers of a base glyph are solid fills of their glyph outlines, in order. */
static bool colr_v0_layers(colr_ctx *c, uint32_t nrec_off, uint32_t nrec, uint32_t lay_off,
                           uint32_t nlay, uint32_t gid)
{
    uint32_t lo = 0, hi = nrec;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t g = 0;
        if (!bt_u16(c->colr, nrec_off + 6ull * mid, &g)) {
            return false;
        }
        if (g < gid) {
            lo = mid + 1;
        } else if (g > gid) {
            hi = mid;
        } else {
            uint32_t first = 0, n = 0;
            if (!bt_u16(c->colr, nrec_off + 6ull * mid + 2, &first) ||
                !bt_u16(c->colr, nrec_off + 6ull * mid + 4, &n)) {
                return false;
            }
            if ((uint64_t)first + n > nlay) {
                return false;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint32_t lg = 0, pi = 0;
                uint64_t rec = lay_off + 4ull * (first + i);
                if (!bt_u16(c->colr, rec, &lg) || !bt_u16(c->colr, rec + 2, &pi)) {
                    return false;
                }
                embi_colr_op clip = {0};
                clip.kind = EMBI_COLR_CLIP_PUSH;
                clip.gid = lg;
                if (!emit(c, &clip)) {
                    return false;
                }
                embi_colr_op paint = {0};
                paint.kind = EMBI_COLR_PAINT;
                paint.paint.kind = EMBI_PAINT_SOLID;
                bool fg = false;
                colr_color(c, pi, 1.0f, paint.paint.rgba, &fg);
                paint.paint.foreground = fg;
                if (!emit(c, &paint)) {
                    return false;
                }
                embi_colr_op pop = {0};
                pop.kind = EMBI_COLR_CLIP_POP;
                if (!emit(c, &pop)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool embi_font_colr_paint(const embi_font *f, uint32_t gid, const int16_t *coords,
                          uint32_t palette, const embi_colr_visitor *v)
{
    embi_bytes t = f->colr;
    if (t.n == 0 || v == nullptr || v->op == nullptr) {
        return false;
    }
    uint32_t version = 0, nrec = 0, rec_off = 0, lay_off = 0, nlay = 0;
    if (!bt_u16(t, 0, &version) || !bt_u16(t, 2, &nrec) || !bt_u32(t, 4, &rec_off) ||
        !bt_u32(t, 8, &lay_off) || !bt_u16(t, 12, &nlay)) {
        return false;
    }
    uint32_t npal = embi_font_palette_count(f);
    if (npal > 0 && palette >= npal) {
        palette = 0;
    }
    colr_ctx c = {0};
    c.f = f;
    c.colr = t;
    c.coords = coords;
    c.nc = f->axis_count;
    c.palette = palette;
    c.v = v;
    /* Variations need coordinates: at the default instance every region scalar is zero. */
    if (version >= 1 && coords != nullptr) {
        uint32_t vmo = 0, ivso = 0;
        if (bt_u32(t, 26, &vmo) && vmo != 0) {
            (void)bt_tail(t, vmo, &c.varmap);
        }
        if (bt_u32(t, 30, &ivso) && ivso != 0) {
            (void)bt_tail(t, ivso, &c.ivs);
        }
    }

    /* COLRv1 paint graph. */
    uint64_t base = 0;
    if (version >= 1 && base_paint_lookup(t, gid, &base)) {
        return paint_eval(&c, base, 0);
    }
    /* COLRv0 layers. */
    if (nrec == 0 || rec_off == 0) {
        return false;
    }
    return colr_v0_layers(&c, rec_off, nrec, lay_off, nlay, gid);
}
