/*
 * Rich CSS effects of the display list (R3a): box-shadow, the filter and backdrop-filter lists
 * and the group markers that carry them. Everything here is CPU side and testable without a GPU:
 *
 *  - box-shadow becomes EMBI_DI_SHADOW items. An outer shadow is the blur of the border box grown by the spread
 *    (radii adjusted as css-backgrounds 3 section 7.1.1 says), moved by the offset, with the border box cut out; an
 *    inset shadow is the padding box with the blur of the (shrunk, moved) hole cut out of the shadow. A shadow
 *    without a blur is the same item with sigma 0 (the renderer draws plain anti-aliased edges, no layer).
 *  - filter lists become EMBI_FX_* steps in dl->fx. A color function is one 4x5 matrix (sRGB, as the shorthand
 *    functions are computed); neighbouring matrices are multiplied together when the first cannot leave 0..1.
 *    filter: url(#id) needs SVG and is skipped (R4, "not-yet svg-filters").
 *  - a filtered stacking context, and a text-shadow with a blur, are LAYER_BEGIN .. LAYER_END groups of kind
 *    EMBI_LAYER_FILTERED; replay renders the group into a layer, filters it and composites it.
 */
#include "paint/paint_int.h"

#include <math.h>
#include <string.h>

#define SIGMA_MIN 0.05f

/* ------------------------------------------------------------------ box-shadow */

static bool shadow_lens(const embi_pst *p, const embi_style *s, const embi_shadow *sh, float *x, float *y, float *blur,
                        float *spread)
{
    float fs = embi_pb_fontsize(s);
    bool ok = false;
    *x = embi_pb_len(p, &sh->x, fs, fs, &ok);
    if (!ok) {
        return false;
    }
    *y = embi_pb_len(p, &sh->y, fs, fs, &ok);
    if (!ok) {
        return false;
    }
    *blur = embi_pb_len(p, &sh->blur, fs, fs, &ok);
    *blur = ok && *blur > 0.0f ? *blur : 0.0f;
    *spread = embi_pb_len(p, &sh->spread, fs, fs, &ok);
    *spread = ok ? *spread : 0.0f;
    return true;
}

/* The radius of a corner of the outer shadow shape after a spread (css-backgrounds 3 section 7.1.1): a square corner
 * stays square, a corner smaller than a positive spread grows by the cubic ease that keeps the curve continuous. */
float embi_pb_spread_radius(float r, float spread)
{
    if (!(r > 0.0f)) {
        return 0.0f;
    }
    if (spread >= 0.0f) {
        if (r >= spread) {
            return r + spread;
        }
        float q = r / spread - 1.0f;
        return r + spread * (1.0f + q * q * q);
    }
    float v = r + spread;
    return v > 0.0f ? v : 0.0f;
}

static void push_shadow(embi_pst *p, const float quad[4], const embi_dshadow *rec, uint32_t color, uint8_t mode,
                        uint32_t xf, uint32_t clip)
{
    embi_dl *d = p->d;
    uint32_t idx = d->shadows.len;
    embi_vec_push(&d->shadows, *rec);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_SHADOW, xf, clip);
    memcpy(it->rect, quad, sizeof it->rect);
    it->sub = mode;
    it->colors[0] = color;
    it->aux = idx;
}

void embi_pb_shadows(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], bool inset, uint32_t xf,
                     uint32_t clip)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_box_shadow);
    if (v == nullptr || v->kind != EMBI_V_SHADOWS || v->shadows.n == 0) {
        return;
    }
    float outer[4], pb[4], inner[4];
    bool have = false;
    /* The first shadow is on top: they are emitted last to first. */
    for (uint32_t k = v->shadows.n; k-- > 0;) {
        const embi_shadow *sh = &v->shadows.p[k];
        if (sh->inset != inset) {
            continue;
        }
        uint32_t color = 0xFF000000u;
        if (!embi_pb_color(s, &sh->color, &color) || (color >> 24) == 0) {
            continue;
        }
        float x, y, blur, spread;
        if (!shadow_lens(p, s, sh, &x, &y, &blur, &spread)) {
            continue;
        }
        if (!have) {
            embi_pb_radii(p, s, bb[2], bb[3], embi_pb_fontsize(s), outer);
            float bdr[4] = {embi_frag_bdr(f, 0), embi_frag_bdr(f, 1), embi_frag_bdr(f, 2), embi_frag_bdr(f, 3)};
            embi_pb_boxes(f, bb, pb, nullptr);
            embi_pb_inner_radii(outer, bdr, inner);
            have = true;
        }
        float sigma = blur * 0.5f;
        if (sigma < SIGMA_MIN) {
            sigma = 0.0f;
        }
        embi_dshadow rec;
        memset(&rec, 0, sizeof rec);
        rec.sigma = sigma;
        float reach = sigma * 3.5f + 1.0f;
        if (!inset) {
            rec.shape[0] = bb[0] + x - spread;
            rec.shape[1] = bb[1] + y - spread;
            rec.shape[2] = bb[2] + 2.0f * spread;
            rec.shape[3] = bb[3] + 2.0f * spread;
            if (!(rec.shape[2] > 0.0f) || !(rec.shape[3] > 0.0f)) {
                continue;
            }
            for (int i = 0; i < 4; i++) {
                rec.shape_radii[i] = embi_pb_spread_radius(outer[i], spread);
                rec.clip_radii[i] = outer[i];
            }
            memcpy(rec.clip, bb, sizeof rec.clip);
            float quad[4] = {rec.shape[0] - reach, rec.shape[1] - reach, rec.shape[2] + 2.0f * reach,
                             rec.shape[3] + 2.0f * reach};
            push_shadow(p, quad, &rec, color, EMBI_SHADOW_OUTER, xf, clip);
        } else {
            if (!(pb[2] > 0.0f) || !(pb[3] > 0.0f)) {
                continue;
            }
            /* The hole: the padding box moved by the offset and shrunk by the spread. */
            rec.shape[0] = pb[0] + x + spread;
            rec.shape[1] = pb[1] + y + spread;
            rec.shape[2] = pb[2] - 2.0f * spread;
            rec.shape[3] = pb[3] - 2.0f * spread;
            if (!(rec.shape[2] > 0.0f) || !(rec.shape[3] > 0.0f)) {
                rec.shape[2] = rec.shape[3] = 0.0f; /* no hole: the whole padding box is shadow */
            }
            for (int i = 0; i < 4; i++) {
                rec.shape_radii[i] = embi_pb_spread_radius(inner[i], -spread);
                rec.clip_radii[i] = inner[i];
            }
            memcpy(rec.clip, pb, sizeof rec.clip);
            push_shadow(p, pb, &rec, color, EMBI_SHADOW_INSET, xf, clip);
        }
    }
}

/* ------------------------------------------------------------------ filter lists */

/* A 4x5 matrix as 20 floats, row major (feColorMatrix order). */
typedef struct cm {
    float m[20];
} cm;

static cm cm_identity(void)
{
    cm c;
    memset(&c, 0, sizeof c);
    c.m[0] = c.m[6] = c.m[12] = c.m[18] = 1.0f;
    return c;
}

/* The matrix of applying a, then b. */
static cm cm_then(const cm *a, const cm *b)
{
    cm o;
    memset(&o, 0, sizeof o);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 5; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += b->m[5 * r + k] * a->m[5 * k + c];
            }
            if (c == 4) {
                sum += b->m[5 * r + 4];
            }
            o.m[5 * r + c] = sum;
        }
    }
    return o;
}

static void cm_rgb(cm *c, const float r0[3], const float r1[3], const float r2[3])
{
    *c = cm_identity();
    for (int i = 0; i < 3; i++) {
        c->m[0 + i] = r0[i];
        c->m[5 + i] = r1[i];
        c->m[10 + i] = r2[i];
    }
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The matrix of a color function, and whether its output always lies in 0..1 for inputs in 0..1. */
static bool color_fn(uint8_t fn, double arg, cm *out, bool *bounded)
{
    float a = (float)arg;
    *bounded = true;
    switch (fn) {
    case EMBI_FILTER_GRAYSCALE: {
        float t = 1.0f - clampf(a, 0.0f, 1.0f);
        const float r0[3] = {0.2126f + 0.7874f * t, 0.7152f - 0.7152f * t, 0.0722f - 0.0722f * t};
        const float r1[3] = {0.2126f - 0.2126f * t, 0.7152f + 0.2848f * t, 0.0722f - 0.0722f * t};
        const float r2[3] = {0.2126f - 0.2126f * t, 0.7152f - 0.7152f * t, 0.0722f + 0.9278f * t};
        cm_rgb(out, r0, r1, r2);
        return true;
    }
    case EMBI_FILTER_SEPIA: {
        float t = 1.0f - clampf(a, 0.0f, 1.0f);
        const float r0[3] = {0.393f + 0.607f * t, 0.769f - 0.769f * t, 0.189f - 0.189f * t};
        const float r1[3] = {0.349f - 0.349f * t, 0.686f + 0.314f * t, 0.168f - 0.168f * t};
        const float r2[3] = {0.272f - 0.272f * t, 0.534f - 0.534f * t, 0.131f + 0.869f * t};
        cm_rgb(out, r0, r1, r2);
        *bounded = false;
        return true;
    }
    case EMBI_FILTER_SATURATE: {
        float s = a < 0.0f ? 0.0f : a;
        const float r0[3] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s};
        const float r1[3] = {0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s};
        const float r2[3] = {0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s};
        cm_rgb(out, r0, r1, r2);
        *bounded = s <= 1.0f;
        return true;
    }
    case EMBI_FILTER_HUE_ROTATE: {
        float rad = (float)(arg * 3.14159265358979323846 / 180.0);
        float c = cosf(rad), sn = sinf(rad);
        const float r0[3] = {0.213f + c * 0.787f - sn * 0.213f, 0.715f - c * 0.715f - sn * 0.715f,
                             0.072f - c * 0.072f + sn * 0.928f};
        const float r1[3] = {0.213f - c * 0.213f + sn * 0.143f, 0.715f + c * 0.285f + sn * 0.140f,
                             0.072f - c * 0.072f - sn * 0.283f};
        const float r2[3] = {0.213f - c * 0.213f - sn * 0.787f, 0.715f - c * 0.715f + sn * 0.715f,
                             0.072f + c * 0.928f + sn * 0.072f};
        cm_rgb(out, r0, r1, r2);
        *bounded = false;
        return true;
    }
    case EMBI_FILTER_INVERT: {
        float t = clampf(a, 0.0f, 1.0f);
        *out = cm_identity();
        for (int i = 0; i < 3; i++) {
            out->m[6 * i] = 1.0f - 2.0f * t;
            out->m[5 * i + 4] = t;
        }
        return true;
    }
    case EMBI_FILTER_OPACITY: {
        *out = cm_identity();
        out->m[18] = clampf(a, 0.0f, 1.0f);
        return true;
    }
    case EMBI_FILTER_BRIGHTNESS: {
        float b = a < 0.0f ? 0.0f : a;
        *out = cm_identity();
        for (int i = 0; i < 3; i++) {
            out->m[6 * i] = b;
        }
        *bounded = b <= 1.0f;
        return true;
    }
    case EMBI_FILTER_CONTRAST: {
        float c = a < 0.0f ? 0.0f : a;
        *out = cm_identity();
        for (int i = 0; i < 3; i++) {
            out->m[6 * i] = c;
            out->m[5 * i + 4] = 0.5f - 0.5f * c;
        }
        *bounded = c <= 1.0f;
        return true;
    }
    default:
        return false;
    }
}

static bool is_identity(const cm *c)
{
    cm id = cm_identity();
    for (int i = 0; i < 20; i++) {
        if (fabsf(c->m[i] - id.m[i]) > 1e-6f) {
            return false;
        }
    }
    return true;
}

uint32_t embi_pb_filter_ops(embi_pst *p, const embi_style *s, embi_prop_id prop, uint32_t *first)
{
    embi_dl *d = p->d;
    *first = d->fx.len;
    const embi_value *v = embi_style_value(s, prop);
    if (v == nullptr || v->kind != EMBI_V_FILTERS || v->filters.n == 0) {
        return 0;
    }
    float fs = embi_pb_fontsize(s);
    bool have_m = false; /* the last step is a matrix that can be merged into */
    bool m_bounded = false;
    for (uint32_t i = 0; i < v->filters.n; i++) {
        const embi_filter *fl = &v->filters.p[i];
        embi_dfx op;
        memset(&op, 0, sizeof op);
        switch (fl->fn) {
        case EMBI_FILTER_BLUR: {
            bool ok = false;
            float r = embi_pb_len(p, &fl->arg, fs, fs, &ok);
            if (!ok || !(r > SIGMA_MIN)) {
                continue;
            }
            op.kind = EMBI_FX_BLUR;
            op.sigma = r;
            embi_vec_push(&d->fx, op);
            have_m = false;
            break;
        }
        case EMBI_FILTER_DROP_SHADOW: {
            if (fl->shadow == nullptr) {
                continue;
            }
            float x, y, blur, spread;
            if (!shadow_lens(p, s, fl->shadow, &x, &y, &blur, &spread)) {
                continue;
            }
            uint32_t color = 0xFF000000u;
            if (!embi_pb_color(s, &fl->shadow->color, &color)) {
                color = 0xFF000000u;
            }
            op.kind = EMBI_FX_SHADOW;
            op.dx = x;
            op.dy = y;
            op.sigma = blur;
            op.color = color;
            embi_vec_push(&d->fx, op);
            have_m = false;
            break;
        }
        case EMBI_FILTER_URL:
            continue; /* needs SVG: not-yet svg-filters */
        default: {
            cm c;
            bool bounded = false;
            double arg = fl->arg.kind == EMBI_V_NUMBER || fl->arg.kind == EMBI_V_ANGLE || fl->arg.kind == EMBI_V_PERCENT
                             ? fl->arg.num
                             : (fl->fn == EMBI_FILTER_HUE_ROTATE ? 0.0 : 1.0); /* hue-rotate() defaults to 0deg */
            if (fl->arg.kind == EMBI_V_PERCENT) {
                arg /= 100.0;
            }
            if (!color_fn(fl->fn, arg, &c, &bounded)) {
                continue;
            }
            if (have_m && m_bounded) {
                cm *last = (cm *)(void *)&d->fx.data[d->fx.len - 1].m[0];
                *last = cm_then(last, &c);
            } else {
                op.kind = EMBI_FX_MATRIX;
                memcpy(op.m, c.m, sizeof op.m);
                embi_vec_push(&d->fx, op);
            }
            have_m = true;
            m_bounded = bounded;
            break;
        }
        }
    }
    /* A merged matrix that came out as the identity does nothing. */
    if (have_m) {
        const embi_dfx *last = &d->fx.data[d->fx.len - 1];
        cm c;
        memcpy(c.m, last->m, sizeof c.m);
        if (is_identity(&c)) {
            d->fx.len--;
        }
    }
    return d->fx.len - *first;
}

/* ------------------------------------------------------------------ groups */

uint32_t embi_pb_fx_begin(embi_pst *p, uint32_t first, uint32_t n, float opacity, uint32_t xf, uint32_t clip)
{
    embi_dl *d = p->d;
    embi_ditem open;
    memset(&open, 0, sizeof open);
    open.kind = EMBI_DI_LAYER_BEGIN;
    open.sub = n > 0 ? EMBI_LAYER_FILTERED : 0;
    open.f[0] = opacity;
    open.aux2 = first;
    open.aux3 = n;
    open.xform = xf;
    open.clip = clip;
    embi_vec_push(&d->items, open);
    return d->items.len - 1;
}

void embi_pb_fx_end(embi_pst *p, uint32_t begin)
{
    embi_dl *d = p->d;
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = begin;
    embi_vec_push(&d->items, end);
    d->items.data[begin].aux = d->items.len - 1;
    if (d->items.len - begin - 2 == 0) {
        d->items.len = begin; /* an empty group */
        return;
    }
    if (d->items.data[begin].sub == EMBI_LAYER_FILTERED) {
        d->fx_items++;
    }
}

/* ------------------------------------------------------------------ backdrop-filter */

void embi_pb_backdrop(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                      uint32_t clip)
{
    (void)f;
    if (!(bb[2] > 0.0f) || !(bb[3] > 0.0f)) {
        return;
    }
    uint32_t first = 0;
    uint32_t n = embi_pb_filter_ops(p, s, EMBI_PROP_backdrop_filter, &first);
    if (n == 0) {
        return;
    }
    float r[4];
    embi_pb_radii(p, s, bb[2], bb[3], embi_pb_fontsize(s), r);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_BACKDROP, xf, clip);
    memcpy(it->rect, bb, sizeof it->rect);
    memcpy(it->radii, r, sizeof it->radii);
    it->aux2 = first;
    it->aux3 = n;
    p->d->fx_items++;
    p->nbackdrop++;
}
