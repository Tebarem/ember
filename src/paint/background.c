/*
 * Backgrounds and replaced content (W4c section 3): background-color, the layers of background-image (url()
 * and linear, radial and conic gradients) with their size, position, repeat, origin, clip and attachment, and the
 * content of an <img> with object-fit and object-position.
 *
 * Tiles. Each layer is tiled over its origin area (CSS Backgrounds 3 section 3.9): repeat, space and round set the
 * tile grid, and the grid is clipped to the clip area by one clip entry per layer. An image layer is one item: a
 * single tile is an image item, and a repeated one is a tiled image item (EMBI_DF_TILE, see paint.h) whose rect is
 * the part of the clip area that the tiles cover. The item count does not grow with the number of tiles.
 *
 * Gradient tiles. A gradient layer uses one item when the tiles can be drawn as one: a linear gradient along an
 * axis whose stops run from 0 to 1, with contiguous tiles (no space gaps). Its item covers the tiled area and has
 * the repeat flag, so the renderer repeats the gradient line. Other gradients get one item per tile, all sharing one
 * stop range (the stops depend on the tile size only), up to TILE_CAP tiles; past that the layer is drawn as one
 * tile stretched over the area (a coarse fallback, noted in the reftests of W4f).
 *
 * Gradient stops (CSS Images 4 section 3.5 and 3.6). The color stops are expanded into the list the renderer
 * reads, which holds only stops with offsets that never decrease:
 *   - a missing position is spaced evenly, and a position that is smaller than the one before is raised to it;
 *   - a color hint between two stops is sampled as its curve, t^e with e = ln(0.5) / ln(H) for the hint H;
 *   - a gap between two stops that differ in alpha, or that interpolates in a space other than sRGB, is sampled, so
 *     the blend the renderer makes between the samples is the blend of the stated space (premultiplied for the
 *     default sRGB). Spaces with a hue are interpolated with the hue method of the gradient.
 * A sample is a straight (non-premultiplied) RGBA8 color. Spaces the code has no converter for are approximated: the
 * wide RGB spaces (display-p3, a98, prophoto, rec2020 and xyz) blend in linear light, and lab and lch blend in
 * oklab and oklch.
 */
#include "canvas/canvas.h"
#include "paint/paint_int.h"
#include "paint/scroll.h"

#include <math.h>
#include <string.h>

#define WHITE_RGBA 0xFFFFFFFFu

/* Per-tile gradient items before a layer is drawn as one coarse tile (see the file comment). */
#define TILE_CAP 65536u
/* The most stops one gradient is expanded to; a gradient whose expansion would be larger keeps its stops (no
 * samples). */
#define GRAD_STOP_CAP 4096u
/* The count of a repeated axis (its tiles are unbounded). */
#define TILE_UNBOUNDED 0xFFFFFFFFu

static const double k_pi_bg = 3.14159265358979323846;

enum {
    BR_NO = 0,
    BR_REPEAT,
    BR_SPACE,
    BR_ROUND,
};

/* The number of layers of a comma-separated value (1 for any other value). */
static uint32_t layer_count(const embi_value *v)
{
    if (v != nullptr && v->kind == EMBI_V_LIST && v->sep == EMBI_SEP_COMMA && v->list.n > 0) {
        return v->list.n;
    }
    return 1;
}

/* The value of layer i: the i-th item of a comma list (the list repeats when shorter), or the value itself. */
static const embi_value *layer_at(const embi_value *v, uint32_t i)
{
    if (v != nullptr && v->kind == EMBI_V_LIST && v->sep == EMBI_SEP_COMMA && v->list.n > 0) {
        return &v->list.items[i % v->list.n];
    }
    return v;
}

static uint16_t kw_or(const embi_value *v, uint16_t dflt)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : dflt;
}

/* The margin of side i (left, top, right, bottom) of the box of f for a margin box: an absolute length of the style as it
 * is (the used margin of an over-constrained block is not the margin of the box), else the margin of the fragment. */
static float margin_px(const embi_frag *f, int i)
{
    static const embi_prop_id props[4] = {EMBI_PROP_margin_left, EMBI_PROP_margin_top, EMBI_PROP_margin_right,
                                          EMBI_PROP_margin_bottom};
    const embi_value *v = f->box != nullptr && f->box->style != nullptr ? embi_style_value(f->box->style, props[i]) : nullptr;
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    return embi_frag_mg(f, i);
}

/* The area a background box origin or clip names, in page px. */
static void area_of(const embi_frag *f, const float bb[4], uint16_t kw, float out[4])
{
    float pb[4], cb[4];
    embi_pb_boxes(f, bb, pb, cb);
    if (kw == EMBI_KW_border_box || kw == EMBI_KW_border_area || kw == EMBI_KW_stroke_box || kw == EMBI_KW_view_box) {
        memcpy(out, bb, 4 * sizeof(float));
    } else if (kw == EMBI_KW_margin_box) {
        /* mask-origin and mask-clip (R3b): the margin box. The margins are left, top, right, bottom. */
        float l = margin_px(f, 0), t = margin_px(f, 1), r = margin_px(f, 2), b = margin_px(f, 3);
        out[0] = bb[0] - l;
        out[1] = bb[1] - t;
        out[2] = bb[2] + l + r;
        out[3] = bb[3] + t + b;
    } else if (kw == EMBI_KW_content_box || kw == EMBI_KW_fill_box) {
        memcpy(out, cb, 4 * sizeof(float));
    } else {
        memcpy(out, pb, 4 * sizeof(float));
    }
}

/* The radius of a margin-box corner whose border-box radius is r with the margin m beside it: r + m when r >= m, else
 * r + m * (1 + (r / m - 1)^3) (CSS Backgrounds 3 section 5.5); a square corner stays square. */
static float margin_radius(float r, float m)
{
    if (!(r > 0.0f) || !(m > 0.0f)) {
        return r > 0.0f ? r : 0.0f;
    }
    if (r >= m) {
        return r + m;
    }
    float q = r / m - 1.0f;
    return r + m * (1.0f + q * q * q);
}

/* The radii of an area (the border edge, the padding edge or the content edge of the box). */
static void radii_of(const embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4], uint16_t kw,
                     float out[4])
{
    float outer[4], bdr[4] = {embi_frag_bdr(f, 0), embi_frag_bdr(f, 1), embi_frag_bdr(f, 2), embi_frag_bdr(f, 3)};
    float pad[4] = {embi_frag_pad(f, 0), embi_frag_pad(f, 1), embi_frag_pad(f, 2), embi_frag_pad(f, 3)};
    embi_pb_frag_radii(p, f, s, bb[2], bb[3], embi_pb_fontsize(s), outer);
    if (kw == EMBI_KW_border_box || kw == EMBI_KW_border_area || kw == EMBI_KW_stroke_box || kw == EMBI_KW_view_box) {
        memcpy(out, outer, sizeof outer);
        return;
    }
    if (kw == EMBI_KW_margin_box) {
        /* A rounded corner grows by the margins beside it; a square corner stays square (CSS Backgrounds 3 5.5). */
        float mg[4] = {margin_px(f, 0), margin_px(f, 1), margin_px(f, 2), margin_px(f, 3)};
        /* outer order: top-left, top-right, bottom-right, bottom-left; mg order: left, top, right, bottom */
        const float m4[4] = {fmaxf(mg[0], mg[1]), fmaxf(mg[1], mg[2]), fmaxf(mg[2], mg[3]), fmaxf(mg[3], mg[0])};
        for (int i = 0; i < 4; i++) {
            out[i] = margin_radius(outer[i], m4[i]);
        }
        return;
    }
    if (kw == EMBI_KW_fill_box) {
        kw = EMBI_KW_content_box;
    }
    float inner[4];
    embi_pb_inner_radii(outer, bdr, inner);
    if (kw == EMBI_KW_content_box) {
        embi_pb_inner_radii(inner, pad, out);
        return;
    }
    memcpy(out, inner, sizeof inner);
}

void embi_pb_area(const embi_frag *f, const float bb[4], uint16_t kw, float out[4])
{
    area_of(f, bb, kw, out);
}

void embi_pb_area_radii(const embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4], uint16_t kw,
                        float out[4])
{
    radii_of(p, s, f, bb, kw, out);
}

/* Whether the inline box piece f has a broken end on the left (side 0) or right (side 2): the style gives that side a
 * border or padding that the piece does not have. */
static bool piece_open(const embi_frag *f, const embi_style *s, int side)
{
    static const embi_prop_id bw[2] = {EMBI_PROP_border_left_width, EMBI_PROP_border_right_width};
    static const embi_prop_id bs[2] = {EMBI_PROP_border_left_style, EMBI_PROP_border_right_style};
    static const embi_prop_id pd[2] = {EMBI_PROP_padding_left, EMBI_PROP_padding_right};
    int k = side == 0 ? 0 : 1;
    const embi_value *w = embi_style_value(s, bw[k]);
    uint16_t st = kw_or(embi_style_value(s, bs[k]), EMBI_KW_none);
    if (w != nullptr && w->kind == EMBI_V_LENGTH && w->num > 0.0 && st != EMBI_KW_none && st != EMBI_KW_hidden &&
        !(embi_frag_bdr(f, side) > 0.0f)) {
        return true;
    }
    const embi_value *pv = embi_style_value(s, pd[k]);
    return pv != nullptr && pv->kind == EMBI_V_LENGTH && pv->num > 0.0 && !(embi_frag_pad(f, side) > 0.0f);
}

void embi_pb_frag_radii(const embi_pst *p, const embi_frag *f, const embi_style *s, float w, float h, float fs,
                        float r[4])
{
    unsigned zero = 0;
    if (f != nullptr && f->kind == EMBI_FK_BOX && f->inl != 0) {
        zero |= piece_open(f, s, 0) ? 0x9u : 0u; /* top-left, bottom-left */
        zero |= piece_open(f, s, 2) ? 0x6u : 0u; /* top-right, bottom-right */
    }
    embi_pb_radii_mask(p, s, w, h, fs, zero, r);
}

/* Whether a border-box background image is clipped to the padding box instead to avoid bleeding (as Blink does): a
 * rounded box whose four borders are solid, opaque and wider than 0 covers everything outside its padding box, so
 * the background's anti-aliased outer edge would only show as a seam through the border's own edge. */
static bool bleed_avoid(const embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4])
{
    static const embi_prop_id st[4] = {EMBI_PROP_border_left_style, EMBI_PROP_border_top_style,
                                       EMBI_PROP_border_right_style, EMBI_PROP_border_bottom_style};
    static const embi_prop_id co[4] = {EMBI_PROP_border_left_color, EMBI_PROP_border_top_color,
                                       EMBI_PROP_border_right_color, EMBI_PROP_border_bottom_color};
    if (p->canvas_bg) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t rgba = 0;
        if (!(embi_frag_bdr(f, i) > 0.0f) || kw_or(embi_style_value(s, st[i]), EMBI_KW_none) != EMBI_KW_solid ||
            !embi_pb_color(s, embi_style_value(s, co[i]), &rgba) || (rgba >> 24) != 255u) {
            return false;
        }
    }
    float r[4];
    embi_pb_frag_radii(p, f, s, bb[2], bb[3], embi_pb_fontsize(s), r);
    return r[0] > 0.0f || r[1] > 0.0f || r[2] > 0.0f || r[3] > 0.0f;
}

/* The clip of an area with corner radii. Radii that do not fit the area (an inner edge whose radii are larger than the
 * padding or content box, as with border-radius: 100% and thick borders) are not scaled down: each rounded corner is
 * its own clip, a rect from that corner large enough for the curve, so the curves keep their centers and the area cuts
 * them (CSS Backgrounds 3 section 5.2: the inner radii are the outer ones minus the border and padding). */
static bool area_clip_needed(const float a[4], const float r[4])
{
    return !(r[0] + r[1] <= a[2] + 0.01f && r[3] + r[2] <= a[2] + 0.01f && r[0] + r[3] <= a[3] + 0.01f &&
             r[1] + r[2] <= a[3] + 0.01f);
}

static uint32_t area_clip(embi_pst *p, const float a[4], const float r[4], uint32_t xf, uint32_t parent)
{
    if (!area_clip_needed(a, r)) {
        return embi_pb_clip(p, a, r, xf, parent);
    }
    float zero[4] = {0, 0, 0, 0};
    uint32_t c = embi_pb_clip(p, a, zero, xf, parent);
    for (int i = 0; i < 4; i++) {
        if (!(r[i] > 0.0f)) {
            continue;
        }
        float big = 2.0f * r[i] + (a[2] > a[3] ? a[2] : a[3]);
        bool right = i == 1 || i == 2, bottom = i == 2 || i == 3;
        float rect[4] = {right ? a[0] + a[2] - big : a[0], bottom ? a[1] + a[3] - big : a[1], big, big};
        float rr[4] = {0, 0, 0, 0};
        rr[i] = r[i];
        c = embi_pb_clip(p, rect, rr, xf, c);
    }
    return c;
}

/* background-clip: border-area (CSS Backgrounds 4 section 3.7): the border area, the border box minus the padding box, as up to
 * four clips (top, bottom, left and right bands), each inside the border box's rounded clip. The painting repeats once per
 * band. Returns the number of clips written to out. */
static uint32_t ring_clips(embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4], uint32_t xf,
                           uint32_t clip, uint32_t out[4])
{
    float outer[4], zero[4] = {0, 0, 0, 0};
    embi_pb_frag_radii(p, f, s, bb[2], bb[3], embi_pb_fontsize(s), outer);
    uint32_t oc = embi_pb_clip(p, bb, outer, xf, clip);
    float bl = embi_frag_bdr(f, 0), bt = embi_frag_bdr(f, 1), br = embi_frag_bdr(f, 2), bbo = embi_frag_bdr(f, 3);
    float mid_h = bb[3] - bt - bbo;
    if (mid_h < 0.0f) {
        mid_h = 0.0f;
    }
    float bands[4][4] = {
        {bb[0], bb[1], bb[2], bt},
        {bb[0], bb[1] + bb[3] - bbo, bb[2], bbo},
        {bb[0], bb[1] + bt, bl, mid_h},
        {bb[0] + bb[2] - br, bb[1] + bt, br, mid_h},
    };
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) {
        if (bands[i][2] > 0.0f && bands[i][3] > 0.0f) {
            out[n++] = embi_pb_clip(p, bands[i], zero, xf, oc);
        }
    }
    return n;
}

/* ------------------------------------------------------------------ tiles */

/* One tiling axis: the first tile, the distance between tiles, the tile size, the number of tiles (TILE_UNBOUNDED
 * for repeat) and the repeat mode. The tiles are first + i * step for i in [0, count). */
typedef struct tiles1 {
    float first, step, size;
    uint32_t count;
    int mode;
} tiles1;

static tiles1 axis_tiles(int mode, float pos, float size, float a0, float alen)
{
    tiles1 t = {pos, size, size, 1, mode};
    /* A tile that is placed once (no-repeat) is painted even when the positioning area has no extent on this axis. */
    if (!(size > 0.0f) || (!(alen > 0.0f) && mode != BR_NO)) {
        t.count = 0;
        return t;
    }
    switch (mode) {
    case BR_REPEAT:
        /* The grid is anchored at pos and extends both ways: the first tile starts at or before the area. */
        t.first = pos - ceilf((pos - a0) / size) * size;
        t.count = TILE_UNBOUNDED;
        break;
    case BR_SPACE: {
        /* CSS Backgrounds 3 section 3.9.3: whole tiles spread across the area with equal gaps. One tile that fits is
         * placed by background-position, as no-repeat. */
        float n = floorf(alen / size);
        if (n <= 1.0f) {
            t.count = 1;
            break;
        }
        float gap = (alen - n * size) / (n - 1.0f);
        t.first = a0;
        t.step = size + gap;
        /* the spaced grid goes on outside the positioning area, into the rest of the painting area (as in Blink) */
        t.count = TILE_UNBOUNDED;
        break;
    }
    case BR_ROUND: {
        /* CSS Backgrounds 3 section 3.9.4: a whole number of tiles, stretched to fit the area. */
        float n = roundf(alen / size);
        if (!(n >= 1.0f)) {
            n = 1.0f;
        }
        float st = alen / n;
        /* the stretched tiles keep the background-position phase and repeat both ways, like `repeat` */
        t.first = pos - ceilf((pos - a0) / st) * st;
        t.step = st;
        t.size = st;
        t.count = TILE_UNBOUNDED;
        break;
    }
    default:
        t.count = 1;
        break;
    }
    return t;
}

static void tile_mode(const embi_value *v, int *mx, int *my);

/* Whether axis i (0 width, 1 height) of a background-size value is auto. */
static bool size_auto(const embi_value *sz, int i)
{
    if (sz == nullptr) {
        return true;
    }
    if (sz->kind == EMBI_V_LIST && sz->list.n == 2) {
        const embi_value *v = &sz->list.items[i];
        return v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_auto;
    }
    return sz->kind == EMBI_V_KEYWORD && sz->kw == EMBI_KW_auto;
}

/* CSS Backgrounds 3 section 3.9: with round on one axis only and an auto size on the other, the rounded tile keeps the
 * image's aspect ratio (the other side is scaled by the same factor). */
static void round_one_axis(const embi_value *rep, const embi_value *sz, const float area[4], float *tw, float *th)
{
    int mx, my;
    tile_mode(rep, &mx, &my);
    if (mx == BR_ROUND && my != BR_ROUND && size_auto(sz, 1) && area[2] > 0.0f && *tw > 0.0f) {
        float n = roundf(area[2] / *tw);
        float st = area[2] / (n < 1.0f ? 1.0f : n);
        *th *= st / *tw;
        *tw = st;
    } else if (my == BR_ROUND && mx != BR_ROUND && size_auto(sz, 0) && area[3] > 0.0f && *th > 0.0f) {
        float n = roundf(area[3] / *th);
        float st = area[3] / (n < 1.0f ? 1.0f : n);
        *tw *= st / *th;
        *th = st;
    }
}

/* The part of [c0, c1] that the tiles of one axis cover (repeat: all of it). False when they miss it. */
static bool axis_span(const tiles1 *t, float c0, float c1, float *s0, float *s1)
{
    if (t->count == TILE_UNBOUNDED) {
        *s0 = c0;
        *s1 = c1;
        return c1 > c0;
    }
    float lo = t->first, hi = t->first + (float)(t->count - 1) * t->step + t->size;
    *s0 = c0 > lo ? c0 : lo;
    *s1 = c1 < hi ? c1 : hi;
    return *s1 > *s0;
}

/* The tile indices (first, end) that meet the interval [s0, s1] of an axis. */
static void tile_range(const tiles1 *t, float s0, float s1, int64_t *i0, int64_t *i1)
{
    double a = ((double)s0 - (double)t->first) / (double)t->step;
    double b = ((double)s1 - (double)t->first) / (double)t->step;
    int64_t lo = (int64_t)floor(a), hi = (int64_t)ceil(b);
    if (t->count != TILE_UNBOUNDED) {
        if (lo < 0) {
            lo = 0;
        }
        if (hi > (int64_t)t->count) {
            hi = (int64_t)t->count;
        }
    }
    if (hi < lo) {
        hi = lo;
    }
    *i0 = lo;
    *i1 = hi;
}

/* True when the tiles of an axis touch each other (no gap), so a gradient tile can repeat over the whole area. */
static bool contiguous(const tiles1 *t)
{
    return t->mode != BR_SPACE || t->count <= 1;
}

/* The natural size of a layer's image, or of a gradient (the positioning area). has_nat is false for a gradient. */
static void size_of(const embi_pst *p, const embi_value *sz, const float area[4], float nw, float nh, bool has_nat,
                    float fs, float *tw, float *th)
{
    *tw = has_nat ? nw : area[2];
    *th = has_nat ? nh : area[3];
    if (sz == nullptr) {
        return;
    }
    if (sz->kind == EMBI_V_KEYWORD && (sz->kw == EMBI_KW_cover || sz->kw == EMBI_KW_contain)) {
        if (!has_nat) {
            return;
        }
        float sx = area[2] / nw, sy = area[3] / nh;
        float s = sz->kw == EMBI_KW_cover ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        *tw = nw * s;
        *th = nh * s;
        return;
    }
    if (sz->kind != EMBI_V_LIST || sz->list.n != 2) {
        return;
    }
    const embi_value *a = &sz->list.items[0], *b = &sz->list.items[1];
    bool auto_a = a->kind == EMBI_V_KEYWORD && a->kw == EMBI_KW_auto;
    bool auto_b = b->kind == EMBI_V_KEYWORD && b->kw == EMBI_KW_auto;
    float wa = 0.0f, hb = 0.0f;
    bool ok = false;
    if (!auto_a) {
        wa = embi_pb_len(p, a, area[2], fs, &ok);
        if (!ok) {
            wa = *tw;
        }
    }
    if (!auto_b) {
        hb = embi_pb_len(p, b, area[3], fs, &ok);
        if (!ok) {
            hb = *th;
        }
    }
    if (!auto_a && !auto_b) {
        *tw = wa;
        *th = hb;
    } else if (!auto_a) {
        *tw = wa;
        *th = (has_nat && nw > 0.0f) ? wa * nh / nw : area[3];
    } else if (!auto_b) {
        *th = hb;
        *tw = (has_nat && nh > 0.0f) ? hb * nw / nh : area[2];
    }
}

/* ------------------------------------------------------------------ gradient geometry and colors */

/* A gradient angle in degrees reduced to one turn; an infinite or NaN angle (calc(Infinity * 1deg)) is 0. */
static double finite_deg(double deg)
{
    return isfinite(deg) ? fmod(deg, 360.0) : 0.0;
}

/* The geometry of gradient g for the tile t (x, y, w, h): the kind (EMBI_GRAD_*_DL) and the geometry in dl's terms
 * (linear: the start and end points; radial: the center and the radii; conic: the center and the start angle). ref is
 * the length that a length stop is relative to. */
static uint8_t grad_geo(const embi_pst *p, const embi_gradient *g, const float t[4], float fs, float geo[6],
                        float *ref)
{
    float w = t[2], h = t[3];
    memset(geo, 0, 6 * sizeof(float));
    *ref = 0.0f;
    float cx = t[0] + w * 0.5f, cy = t[1] + h * 0.5f;
    if (g->kind == EMBI_GRAD_LINEAR) {
        double ang;
        if (g->sides != 0) {
            uint8_t sd = g->sides;
            bool top = (sd & EMBI_SIDE_TOP) != 0, right = (sd & EMBI_SIDE_RIGHT) != 0;
            bool bottom = (sd & EMBI_SIDE_BOTTOM) != 0, left = (sd & EMBI_SIDE_LEFT) != 0;
            if (top && right) {
                /* top right: the gradient line is perpendicular to the other diagonal, direction (h, -w) */
                ang = atan2((double)h, (double)w) * 180.0 / k_pi_bg;
            } else if (bottom && right) {
                ang = 180.0 - atan2((double)h, (double)w) * 180.0 / k_pi_bg;
            } else if (bottom && left) {
                ang = 180.0 + atan2((double)h, (double)w) * 180.0 / k_pi_bg;
            } else if (top && left) {
                ang = 360.0 - atan2((double)h, (double)w) * 180.0 / k_pi_bg;
            } else if (top) {
                ang = 0.0;
            } else if (right) {
                ang = 90.0;
            } else if (bottom) {
                ang = 180.0;
            } else {
                ang = 270.0;
            }
        } else if (g->angle.kind == EMBI_V_ANGLE) {
            ang = finite_deg(g->angle.num);
        } else {
            ang = 180.0;
        }
        double r = ang * k_pi_bg / 180.0;
        double dx = sin(r), dy = -cos(r);
        double len = fabs((double)w * dx) + fabs((double)h * -dy);
        *ref = (float)len;
        geo[0] = (float)(cx - dx * len * 0.5);
        geo[1] = (float)(cy - dy * len * 0.5);
        geo[2] = (float)(cx + dx * len * 0.5);
        geo[3] = (float)(cy + dy * len * 0.5);
        return EMBI_GRAD_LINEAR_DL;
    }
    /* radial and conic: the center from the position (default 50% 50%, and an axis that is not written too). */
    cx = t[0] + w * 0.5f;
    cy = t[1] + h * 0.5f;
    if (g->has_pos) {
        bool okx = false, oky = false;
        if (g->pos.x.off.kind != EMBI_V_NONE) {
            float off = embi_pb_len(p, &g->pos.x.off, w, fs, &okx);
            if (okx) {
                cx = t[0] + (g->pos.x.anchor != 0 ? w - off : off);
            }
        }
        if (g->pos.y.off.kind != EMBI_V_NONE) {
            float off = embi_pb_len(p, &g->pos.y.off, h, fs, &oky);
            if (oky) {
                cy = t[1] + (g->pos.y.anchor != 0 ? h - off : off);
            }
        }
    }
    /* an infinite or huge center stands for "very far" (keeps the radii finite) */
    cx = isnan(cx) ? 0.0f : fminf(fmaxf(cx, -1.0e7f), 1.0e7f);
    cy = isnan(cy) ? 0.0f : fminf(fmaxf(cy, -1.0e7f), 1.0e7f);
    if (g->kind == EMBI_GRAD_RADIAL) {
        float dxl = cx - t[0], dxr = t[0] + w - cx, dyt = cy - t[1], dyb = t[1] + h - cy;
        float fx = dxl > dxr ? dxl : dxr, fy = dyt > dyb ? dyt : dyb;
        float nx = dxl < dxr ? dxl : dxr, ny = dyt < dyb ? dyt : dyb;
        float rx = 0, ry = 0;
        switch (g->size) {
        case EMBI_RS_CLOSEST_SIDE:
            rx = nx;
            ry = ny;
            break;
        case EMBI_RS_FARTHEST_SIDE:
            rx = fx;
            ry = fy;
            break;
        case EMBI_RS_CLOSEST_CORNER:
            rx = nx * 1.41421356f;
            ry = ny * 1.41421356f;
            break;
        case EMBI_RS_EXPLICIT: {
            bool ok1 = false, ok2 = false;
            rx = embi_pb_len(p, &g->radius[0], w, fs, &ok1);
            ry = g->ellipse && g->radius[1].kind != EMBI_V_NONE ? embi_pb_len(p, &g->radius[1], h, fs, &ok2) : rx;
            if (!ok1) {
                rx = 0;
            }
            if (!ok2) {
                ry = rx;
            }
            break;
        }
        default: /* farthest-corner */
            rx = fx * 1.41421356f;
            ry = fy * 1.41421356f;
            break;
        }
        if (!g->ellipse) {
            /* A circle: one radius, the distance that the chosen size names. */
            float r;
            if (g->size == EMBI_RS_CLOSEST_SIDE) {
                r = nx < ny ? nx : ny;
            } else if (g->size == EMBI_RS_FARTHEST_SIDE) {
                r = fx > fy ? fx : fy;
            } else if (g->size == EMBI_RS_CLOSEST_CORNER) {
                r = hypotf(nx, ny);
            } else if (g->size == EMBI_RS_EXPLICIT) {
                r = rx;
            } else {
                r = hypotf(fx, fy);
            }
            rx = ry = r;
        }
        geo[0] = cx;
        geo[1] = cy;
        geo[2] = rx;
        geo[3] = ry;
        *ref = rx;
        return EMBI_GRAD_RADIAL_DL;
    }
    geo[0] = cx;
    geo[1] = cy;
    geo[2] = g->angle.kind == EMBI_V_ANGLE ? (float)(finite_deg(g->angle.num) * k_pi_bg / 180.0) : 0.0f;
    return EMBI_GRAD_CONIC_DL;
}

/* The weight of a color hint H (0..1, the position of the midpoint in the gap) at t (0..1 across the gap). */
static double hint_weight(double t, double h)
{
    if (h <= 1e-9) {
        return t > 0.0 ? 1.0 : 0.0;
    }
    if (h >= 1.0 - 1e-9) {
        return t < 1.0 ? 0.0 : 1.0;
    }
    return pow(t, log(0.5) / log(h));
}

/* A color stop as read from the gradient: its color, its positions (p[1] when two are written), and the hint that
 * comes before it (NaN when none). */
typedef struct gstop {
    embi_color col;
    uint32_t rgba;
    float p[2];
    bool two;
    float hint;
} gstop;

/* Positions beyond this are clamped: an infinite position (calc(infinity * 1px)) stands for "very far". */
#define GRAD_POS_LIMIT 1.0e9f

/* A position of a stop in the gradient's units: a fraction of ref for a length, a fraction for a percentage, a
 * fraction of a turn for an angle. NaN when the value is not a position. */
static float resolve_pos(const embi_pst *p, const embi_value *pv, float ref, float fs, bool turn)
{
    float r;
    if (pv->kind == EMBI_V_PERCENT) {
        r = (float)(pv->num / 100.0);
    } else if (pv->kind == EMBI_V_ANGLE) {
        r = (float)(pv->num / 360.0);
    } else if (pv->kind == EMBI_V_CALC && (turn || (pv->calc->type & EMBI_CD_ANGLE) != 0)) {
        /* A calc() of a conic gradient stop (angles, percentages or both): its percentages are of a full turn, the
         * result is in degrees. A NaN result is censored to 0 (CSS Values 4 section 10.9). */
        embi_calc_env ce = {360.0, nullptr, nullptr};
        double deg = embi_calc_eval(pv->calc, &ce);
        if (isnan(deg)) {
            deg = 0.0;
        }
        r = (float)(deg / 360.0);
    } else if (pv->kind == EMBI_V_LENGTH || pv->kind == EMBI_V_CALC) {
        bool ok = false;
        float px = embi_pb_len(p, pv, ref, fs, &ok);
        if (!ok) {
            return NAN;
        }
        r = ref > 0.0f ? px / ref : 0.0f;
    } else {
        return NAN;
    }
    if (isnan(r)) {
        return NAN;
    }
    return r > GRAD_POS_LIMIT ? GRAD_POS_LIMIT : (r < -GRAD_POS_LIMIT ? -GRAD_POS_LIMIT : r);
}

/* Whether the color is one of the legacy syntaxes (hex, named, rgb(), hsl(), hwb()), which interpolate in sRGB by
 * default; any other color makes the default Oklab (CSS Images 4 section 3.6.1, CSS Color 4 section 12.1). */
static bool legacy_color(const embi_color *c)
{
    return c->space == EMBI_CS_SRGB && (c->flags & (EMBI_CF_FUNC | EMBI_CF_NONE_ALL)) == 0;
}

/* Turns the components written as none back into missing ones (NaN) for interpolation. True when the color has a
 * missing component (color(srgb none ...) keeps NaN from parsing). */
static bool missing_comps(embi_color *c)
{
    bool any = false;
    for (int i = 0; i < 3; i++) {
        if ((c->flags & (EMBI_CF_NONE0 << i)) != 0) {
            c->c[i] = NAN;
        }
        any = any || isnan(c->c[i]);
    }
    return any;
}

/* The color at weight t (0 at a, 1 at b) through the mixer, as a straight RGBA8. */
static uint32_t mixed_rgba(const embi_color_mixer *mx, double t)
{
    embi_color c;
    embi_color_mixer_at(mx, t, &c);
    uint8_t px[4];
    if (!embi_color_to_srgb8(&c, px)) {
        return 0;
    }
    return (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
}

/* A straight RGBA8 with its alpha scaled by opa (a cross-fade() input's opacity). */
static uint32_t alpha_mul(uint32_t rgba, float opa)
{
    if (opa >= 1.0f) {
        return rgba;
    }
    uint32_t a = (uint32_t)((float)(rgba >> 24) * opa + 0.5f);
    return (rgba & 0x00ffffffu) | ((a > 255u ? 255u : a) << 24);
}

/* The color at weight t of the gap from a to b (the mixer fills a missing component from the other end), or fb. */
static uint32_t edge_rgba(const embi_color *a, const embi_color *b, uint8_t ispace, uint8_t hue, double t, uint32_t fb)
{
    embi_color_mixer *mx = nullptr;
    if (!embi_color_mixer_init(&mx, a, b, ispace, hue)) {
        return fb;
    }
    uint32_t r = mixed_rgba(mx, t);
    embi_color_mixer_free(mx);
    return r;
}

/* Expands the stops of gradient g (for the length ref of its geometry) into dl's stops. The stops of one gradient
 * are contiguous at *first, and *count is their number. wmax is the largest gradient position that can be seen (in the
 * units of the positions; the part of a gap outside [0, wmax] is not sampled, which keeps a gradient with huge
 * positions exact). False when the gradient has no color stop. */
static bool grad_stops(embi_pst *p, const embi_gradient *g, const embi_style *s, float ref, float fs, float wmax,
                       float opa, uint32_t *first, uint32_t *count)
{
    if (g->nstops == 0) {
        return false;
    }
    embi_dl *d = p->d;
    gstop *cs = embi_alloc(g->nstops * sizeof *cs);
    uint32_t m = 0;
    float pending = NAN;
    bool all_legacy = true, any_missing = false;
    for (uint32_t i = 0; i < g->nstops; i++) {
        const embi_color_stop *st = &g->stops[i];
        if (st->color.kind == EMBI_V_NONE) {
            /* A color hint: its position lies between the stop before and the stop after. */
            pending = st->pos[0].kind == EMBI_V_NONE ? NAN : resolve_pos(p, &st->pos[0], ref, fs, g->kind == EMBI_GRAD_CONIC);
            continue;
        }
        gstop *e = &cs[m];
        if (!embi_pb_color_full(s, &st->color, &e->col)) {
            pending = NAN;
            continue;
        }
        uint8_t px[4];
        if (!embi_color_to_srgb8(&e->col, px)) {
            pending = NAN;
            continue;
        }
        m++;
        all_legacy = all_legacy && legacy_color(&e->col);
        any_missing = missing_comps(&e->col) || any_missing;
        e->rgba = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
        e->p[0] = st->pos[0].kind == EMBI_V_NONE ? NAN : resolve_pos(p, &st->pos[0], ref, fs, g->kind == EMBI_GRAD_CONIC);
        e->two = st->pos[1].kind != EMBI_V_NONE;
        e->p[1] = e->two ? resolve_pos(p, &st->pos[1], ref, fs, g->kind == EMBI_GRAD_CONIC) : NAN;
        e->hint = pending;
        pending = NAN;
    }
    if (m == 0) {
        embi_free(cs, g->nstops * sizeof *cs);
        return false;
    }

    /* The positions in order, one per stop or two per stop. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < m; i++) {
        n += cs[i].two ? 2u : 1u;
    }
    float *fp = embi_alloc(n * sizeof *fp);
    {
        uint32_t j = 0;
        for (uint32_t i = 0; i < m; i++) {
            fp[j++] = cs[i].p[0];
            if (cs[i].two) {
                fp[j++] = cs[i].p[1];
            }
        }
    }
    /* CSS Images 4 section 3.5.1: a missing first position is 0 and a missing last one is 1; missing ones in between
     * are spaced evenly between the known ones. Then no position is smaller than the one before it. */
    if (isnan(fp[0])) {
        fp[0] = 0.0f;
    }
    if (isnan(fp[n - 1])) {
        fp[n - 1] = 1.0f;
    }
    uint32_t prev = 0;
    for (uint32_t i = 1; i < n; i++) {
        if (isnan(fp[i])) {
            continue;
        }
        for (uint32_t k = prev + 1; k < i; k++) {
            float t01 = (float)(k - prev) / (float)(i - prev);
            fp[k] = fp[prev] + (fp[i] - fp[prev]) * t01;
        }
        prev = i;
    }
    for (uint32_t i = 1; i < n; i++) {
        if (fp[i] < fp[i - 1]) {
            fp[i] = fp[i - 1];
        }
    }

    /* CSS Images 3 section 3.5.1: a repeating gradient whose stops span no distance paints its last color alone. */
    if (g->repeating && fp[n - 1] - fp[0] <= 1.0e-6f) {
        *first = d->stops.len;
        embi_vec_push(&d->stops, ((embi_dstop){0.0f, alpha_mul(cs[m - 1].rgba, opa)}));
        embi_vec_push(&d->stops, ((embi_dstop){1.0f, alpha_mul(cs[m - 1].rgba, opa)}));
        *count = 2;
        embi_free(fp, n * sizeof *fp);
        embi_free(cs, g->nstops * sizeof *cs);
        return true;
    }

    /* The interpolation space: the written one, else Oklab when any stop is not a legacy color, else sRGB. */
    uint8_t ispace = g->has_interp ? g->interp : (all_legacy ? EMBI_IS_SRGB : EMBI_IS_OKLAB);
    bool plain = ispace == EMBI_IS_SRGB || ispace == EMBI_IS_DEFAULT;
    /* Samples per unit of position: about one per pixel along the gradient (a conic gradient: 720 per turn). */
    double per_unit = g->kind == EMBI_GRAD_CONIC ? 720.0 : (ref > 0.0f ? (double)ref : 16.0);
    /* Each gap that needs a curve is sampled over the part that can be seen; the expansion stops growing at the cap. */
    const uint32_t cap_each = 1024u;
    EMBI_VEC(embi_dstop) ov = {0};
    uint32_t fj = 0;
    float prev_pos = 0.0f;
    uint32_t prev_rgba = 0;
    const embi_color *prev_col = nullptr;
    bool repeating = g->repeating;
    for (uint32_t i = 0; i < m; i++) {
        float p1 = fp[fj];
        uint32_t last = fj + (cs[i].two ? 1u : 0u);
        if (i > 0 && p1 > prev_pos) {
            bool alpha_differs = (prev_rgba >> 24) != (cs[i].rgba >> 24);
            bool hinted = !isnan(cs[i].hint);
            if (ov.len < GRAD_STOP_CAP && (hinted || alpha_differs || !plain)) {
                double a = prev_pos, b = p1;
                double lo = a, hi = b;
                if (!repeating) {
                    lo = lo < 0.0 ? 0.0 : lo;
                    hi = hi > (double)wmax ? (double)wmax : hi;
                }
                if (hi > lo) {
                    double h = NAN;
                    if (hinted) {
                        double hp = cs[i].hint;
                        hp = hp < a ? a : (hp > b ? b : hp);
                        h = (hp - a) / (b - a);
                    }
                    embi_color_mixer *mx = nullptr;
                    if (embi_color_mixer_init(&mx, prev_col, &cs[i].col, ispace, g->hue)) {
                        double span = hi - lo;
                        double want = ceil(span * per_unit);
                        /* at most cap_each samples per gap and GRAD_STOP_CAP stops per gradient */
                        uint32_t room = GRAD_STOP_CAP - ov.len;
                        uint32_t lim = cap_each < room ? cap_each : room;
                        uint32_t ns = want < 16.0 ? 16u : (want > (double)lim ? lim : (uint32_t)want);
                        if (ns < 2u) {
                            ns = 2u;
                        }
                        /* The visible window's ends are sampled too, so the blend is exact where it is seen. */
                        for (uint32_t k = 0; k <= ns; k++) {
                            double pos = lo + span * (double)k / (double)ns;
                            if (pos <= a || pos >= b) {
                                continue;
                            }
                            double t = (pos - a) / (b - a);
                            double w = isnan(h) ? t : hint_weight(t, h);
                            embi_vec_push(&ov, ((embi_dstop){(float)pos, mixed_rgba(mx, w)}));
                        }
                        embi_color_mixer_free(mx);
                    }
                }
            }
        }
        uint32_t in_rgba = cs[i].rgba, out_rgba = cs[i].rgba;
        if (any_missing) {
            /* A stop with a missing component takes it from its neighbour on each side (CSS Color 4 section 12.2):
             * the color it ends the gap before with and the one it starts the gap after with can differ. */
            in_rgba = i > 0 ? edge_rgba(prev_col, &cs[i].col, ispace, g->hue, 1.0, in_rgba) : in_rgba;
            out_rgba = i + 1 < m ? edge_rgba(&cs[i].col, &cs[i + 1].col, ispace, g->hue, 0.0, out_rgba) : out_rgba;
        }
        embi_vec_push(&ov, ((embi_dstop){p1, in_rgba}));
        if (out_rgba != in_rgba || cs[i].two) {
            embi_vec_push(&ov, ((embi_dstop){fp[last], out_rgba}));
        }
        prev_pos = fp[last];
        prev_rgba = cs[i].rgba;
        prev_col = &cs[i].col;
        fj = last + 1;
    }

    *first = d->stops.len;
    for (uint32_t i = 0; i < ov.len; i++) {
        embi_dstop ds = ov.data[i];
        ds.color = alpha_mul(ds.color, opa);
        embi_vec_push(&d->stops, ds);
    }
    *count = ov.len;
    bool any = ov.len > 0;
    embi_vec_free(&ov);
    embi_free(fp, n * sizeof *fp);
    embi_free(cs, g->nstops * sizeof *cs);
    return any;
}

/* One input of a cross-fade() (CSS Images 4 section 2.6): an image (a url() or a gradient; a <color> is a one-stop
 * gradient) and the opacity it is drawn with. Drawn in order with source-over, the opacities o_k = p_k / (1 - the
 * sum of the p_j after k) give the weighted sum of the inputs, exact when each later input is opaque. */
#define XFADE_MAX 16u
typedef struct xpart {
    const embi_image *img; /* URL or GRADIENT; nullptr for a color (g) */
    embi_gradient g;
    embi_color_stop st;
    float opa;
} xpart;

/* The url() or gradient an image() fallback list or a plain image paints, or nullptr. */
static const embi_image *simple_image(const embi_pst *p, const embi_image *im)
{
    if (im != nullptr && im->kind == EMBI_IMG_FALLBACKS) {
        const embi_image *pick = im->fb;
        for (uint32_t oi = 0; oi < im->n && p->cfg->image_size != nullptr; oi++) {
            float w0 = 0.0f, h0 = 0.0f;
            bool r0 = false;
            if (im->opts[oi].img != nullptr && im->opts[oi].img->kind == EMBI_IMG_URL &&
                p->cfg->image_size(p->cfg->user, im->opts[oi].img->url, &w0, &h0, &r0) && w0 > 0.0f && h0 > 0.0f) {
                pick = im->opts[oi].img;
                break;
            }
        }
        im = pick;
    }
    return im != nullptr && (im->kind == EMBI_IMG_URL || im->kind == EMBI_IMG_GRADIENT) ? im : nullptr;
}

/* The inputs of cross-fade() im with their drawing opacities, and its natural size (the average of the inputs'
 * natural sizes weighted by their percentages; none when no input has one). Returns the number of parts. */
static uint32_t xfade_parts(const embi_pst *p, const embi_image *im, xpart *parts, float *nw, float *nh, bool *nat)
{
    uint32_t n = im->n < XFADE_MAX ? im->n : XFADE_MAX;
    float w[XFADE_MAX];
    float given = 0.0f;
    uint32_t omitted = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (im->xf[i].pct < 0.0) {
            omitted++;
        } else {
            given += (float)(im->xf[i].pct / 100.0);
        }
    }
    /* Omitted percentages share what the given ones leave; a sum over 100% is scaled down to 100%. */
    float rest = omitted > 0 && given < 1.0f ? (1.0f - given) / (float)omitted : 0.0f;
    float total = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        w[i] = im->xf[i].pct < 0.0 ? rest : (float)(im->xf[i].pct / 100.0);
        total += w[i];
    }
    if (total > 1.0f) {
        for (uint32_t i = 0; i < n; i++) {
            w[i] /= total;
        }
    }
    uint32_t np = 0;
    float sw = 0.0f, sx = 0.0f, sy = 0.0f;
    float after = 0.0f; /* the sum of the weights after input i */
    for (uint32_t i = n; i-- > 0;) {
        float denom = 1.0f - after;
        float o = denom > 1.0e-6f ? w[i] / denom : 0.0f;
        after += w[i];
        xpart *x = &parts[i];
        memset(x, 0, sizeof *x);
        x->opa = o > 1.0f ? 1.0f : o;
        const embi_xfade_item *it = &im->xf[i];
        if (it->img == nullptr) {
            x->st.color = it->color;
            x->g.kind = EMBI_GRAD_LINEAR;
            x->g.size = EMBI_RS_FARTHEST_CORNER;
            x->g.nstops = 1;
            x->g.stops = &x->st;
            continue;
        }
        x->img = simple_image(p, it->img);
        float iw = 0.0f, ih = 0.0f;
        bool r = false;
        if (x->img != nullptr && x->img->kind == EMBI_IMG_URL && p->cfg->image_size != nullptr &&
            p->cfg->image_size(p->cfg->user, x->img->url, &iw, &ih, &r) && iw > 0.0f && ih > 0.0f) {
            sw += w[i];
            sx += w[i] * iw;
            sy += w[i] * ih;
        }
    }
    np = n;
    *nat = sw > 0.0f;
    *nw = *nat ? sx / sw : 0.0f;
    *nh = *nat ? sy / sw : 0.0f;
    return np;
}

/* Paints one gradient layer over its tiles. span is the part of the clip area that the tiles cover (x, y, w, h); t
 * are the tile grids; lclip is the layer's clip. */
static void gradient_layer(embi_pst *p, const embi_gradient *g, const embi_style *s, const tiles1 *tx,
                           const tiles1 *ty, const float span[4], float fs, float opa, uint32_t xf, uint32_t lclip)
{
    float tile[4] = {tx->first, ty->first, tx->size, ty->size};
    float geo[6];
    float ref = 0.0f;
    uint8_t kind = grad_geo(p, g, tile, fs, geo, &ref);
    /* The largest gradient position that can be seen: 1 for a line or a turn, the farthest corner for a radial one. */
    float wmax = 1.0f;
    if (kind == EMBI_GRAD_RADIAL_DL) {
        /* the farthest tile corner from the center, in units of the radii (a degenerate radius: unbounded) */
        float rx = geo[2], ry = geo[3];
        if (rx > 0.0f && ry > 0.0f) {
            float fdx = fmaxf(fabsf(tile[0] - geo[0]), fabsf(tile[0] + tile[2] - geo[0]));
            float fdy = fmaxf(fabsf(tile[1] - geo[1]), fabsf(tile[1] + tile[3] - geo[1]));
            wmax = hypotf(fdx / rx, fdy / ry) * 1.001f + 0.01f;
        } else {
            wmax = 1.0e9f;
        }
    }
    uint32_t first = 0, n = 0;
    if (tx->mode == BR_NO && ty->mode == BR_NO) {
        if (grad_stops(p, g, s, ref, fs, wmax, opa, &first, &n)) {
            embi_pb_gradient_at(p, kind, tile, geo, first, n, g->repeating, xf, lclip);
        }
        return;
    }
    /* Spaced tiles (space with more than one tile) are drawn one by one below. */
    if (contiguous(tx) && contiguous(ty) && kind == EMBI_GRAD_LINEAR_DL &&
        (fabsf(geo[2] - geo[0]) <= 1e-3f || fabsf(geo[3] - geo[1]) <= 1e-3f)) {
        /* An axis-aligned line: the tile is exactly one period of the line, so the stops that run from 0 to 1 repeat
         * over the whole span with the repeat flag. */
        if (grad_stops(p, g, s, ref, fs, wmax, opa, &first, &n)) {
            const embi_dstop *st = &p->d->stops.data[first];
            if (st[0].offset == 0.0f && st[n - 1].offset == 1.0f) {
                embi_pb_gradient_at(p, kind, span, geo, first, n, true, xf, lclip);
                return;
            }
            /* Stops that do not cover the line: the tiles are drawn one by one from the same stops. */
        } else {
            return;
        }
    }

    int64_t ix0, ix1, iy0, iy1;
    tile_range(tx, span[0], span[0] + span[2], &ix0, &ix1);
    tile_range(ty, span[1], span[1] + span[3], &iy0, &iy1);
    uint64_t total = (uint64_t)(ix1 - ix0) * (uint64_t)(iy1 - iy0);
    if (total == 0) {
        return;
    }
    if (n == 0 && !grad_stops(p, g, s, ref, fs, wmax, opa, &first, &n)) {
        return;
    }
    if (total > TILE_CAP) {
        /* Too many tiles: one tile stretched over the span (a coarse approximation). */
        embi_pb_gradient_at(p, kind, span, geo, first, n, g->repeating, xf, lclip);
        return;
    }
    for (int64_t j = iy0; j < iy1; j++) {
        for (int64_t i = ix0; i < ix1; i++) {
            float x0 = tx->first + (float)i * tx->step;
            float y0 = ty->first + (float)j * ty->step;
            float t[4] = {x0, y0, tx->size, ty->size};
            float g2[6];
            memcpy(g2, geo, sizeof g2);
            float dx = x0 - tile[0], dy = y0 - tile[1];
            g2[0] += dx;
            g2[1] += dy;
            if (kind == EMBI_GRAD_LINEAR_DL) {
                g2[2] += dx;
                g2[3] += dy;
            }
            embi_pb_gradient_at(p, kind, t, g2, first, n, g->repeating, xf, lclip);
        }
    }
}

static bool nearest_flag(const embi_style *s)
{
    uint16_t kw = embi_pb_kw(s, EMBI_PROP_image_rendering);
    return kw == EMBI_KW_pixelated || kw == EMBI_KW_crisp_edges;
}

static void tile_mode(const embi_value *v, int *mx, int *my)
{
    *mx = BR_REPEAT;
    *my = BR_REPEAT;
    if (v == nullptr) {
        return;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        if (v->kw == EMBI_KW_repeat_x) {
            *my = BR_NO;
        } else if (v->kw == EMBI_KW_repeat_y) {
            *mx = BR_NO;
        }
        return;
    }
    if (v->kind == EMBI_V_LIST && v->list.n == 2) {
        int m[2];
        for (int i = 0; i < 2; i++) {
            const embi_value *k = &v->list.items[i];
            uint16_t kw = k->kind == EMBI_V_KEYWORD ? k->kw : 0;
            m[i] = kw == EMBI_KW_no_repeat ? BR_NO
                   : kw == EMBI_KW_space  ? BR_SPACE
                   : kw == EMBI_KW_round  ? BR_ROUND
                                          : BR_REPEAT;
        }
        *mx = m[0];
        *my = m[1];
    }
}

/* The properties that drive a list of image layers: the background-* ones, or the mask-* ones (R3b). */
typedef struct lprops {
    bool mask;
    const embi_value *img, *sz, *rep, *org, *clp;
    const embi_value *att;      /* background-attachment */
    const embi_value *px, *py;  /* background-position-x and -y */
    const embi_value *pos;      /* mask-position */
} lprops;

/* The blend mode keyword of a mix-blend-mode or background-blend-mode value, as EMBI_BM_*. */
uint32_t embi_pb_blend_kw(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_multiply:
        return EMBI_BM_MULTIPLY;
    case EMBI_KW_screen:
        return EMBI_BM_SCREEN;
    case EMBI_KW_overlay:
        return EMBI_BM_OVERLAY;
    case EMBI_KW_darken:
        return EMBI_BM_DARKEN;
    case EMBI_KW_lighten:
        return EMBI_BM_LIGHTEN;
    case EMBI_KW_color_dodge:
        return EMBI_BM_COLOR_DODGE;
    case EMBI_KW_color_burn:
        return EMBI_BM_COLOR_BURN;
    case EMBI_KW_hard_light:
        return EMBI_BM_HARD_LIGHT;
    case EMBI_KW_soft_light:
        return EMBI_BM_SOFT_LIGHT;
    case EMBI_KW_difference:
        return EMBI_BM_DIFFERENCE;
    case EMBI_KW_exclusion:
        return EMBI_BM_EXCLUSION;
    case EMBI_KW_hue:
        return EMBI_BM_HUE;
    case EMBI_KW_saturation:
        return EMBI_BM_SATURATION;
    case EMBI_KW_color:
        return EMBI_BM_COLOR;
    case EMBI_KW_luminosity:
        return EMBI_BM_LUMINOSITY;
    case EMBI_KW_plus_lighter:
        return EMBI_BM_PLUS_LIGHTER;
    case EMBI_KW_plus_darker:
        return EMBI_BM_PLUS_DARKER;
    default:
        return EMBI_BM_NORMAL;
    }
}

/* Draws the inputs of a cross-fade() over the tiles, each stretched to the tile, in order with their opacities. */
static void xfade_draw(embi_pst *p, const embi_style *s, const xpart *parts, uint32_t n, const tiles1 *tx,
                       const tiles1 *ty, const float span[4], float fs, bool nearest, uint32_t xf, uint32_t lclip)
{
    for (uint32_t i = 0; i < n; i++) {
        const xpart *x = &parts[i];
        if (!(x->opa > 0.0f)) {
            continue;
        }
        if (x->img == nullptr || x->img->kind == EMBI_IMG_GRADIENT) {
            gradient_layer(p, x->img != nullptr ? x->img->grad : &x->g, s, tx, ty, span, fs, x->opa, xf, lclip);
            continue;
        }
        uint32_t a = (uint32_t)(x->opa * 255.0f + 0.5f);
        uint32_t tint = 0x00ffffffu | ((a > 255u ? 255u : a) << 24);
        float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        if (tx->mode == BR_NO && ty->mode == BR_NO) {
            float t[4] = {tx->first, ty->first, tx->size, ty->size};
            embi_pb_image(p, x->img->url, t, uv, tint, nearest, xf, lclip);
        } else {
            float first[2] = {tx->first, ty->first};
            float size[2] = {tx->size, ty->size};
            float step[2] = {tx->step, ty->step};
            embi_pb_image_tile(p, x->img->url, span, first, size, step, nullptr, tint, nearest, xf, lclip);
        }
    }
}

/* Paints layer k of a list of image layers: its tiles, clipped to its clip area. Nothing is painted for a layer without an
 * image, with an image that is not loaded, or whose tiles miss the clip area. */
static bool paint_layer(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf, uint32_t clip,
                        const lprops *lp, uint32_t k, float fs, bool nearest, bool bleed)
{
    p->lh = embi_pb_lineheight(s);
    p->bg_f = f; /* the font- and container-relative units of the layer resolve against this box (masks too) */
    p->bg_s = s;
    const embi_value *lv = layer_at(lp->img, k);
    if (lv == nullptr || lv->kind != EMBI_V_IMAGE || lv->image == nullptr) {
        return true;
    }
    const embi_image *im = lv->image;
    if (im->kind == EMBI_IMG_FALLBACKS) {
        /* image(): the first source that loads, else the color */
        const embi_image *pick = im->fb;
        for (uint32_t oi = 0; oi < im->n && p->cfg->image_size != nullptr; oi++) {
            float w0 = 0.0f, h0 = 0.0f;
            bool r0 = false;
            if (im->opts[oi].img != nullptr && im->opts[oi].img->kind == EMBI_IMG_URL &&
                p->cfg->image_size(p->cfg->user, im->opts[oi].img->url, &w0, &h0, &r0) && w0 > 0.0f && h0 > 0.0f) {
                pick = im->opts[oi].img;
                break;
            }
        }
        if (pick == nullptr) {
            return true;
        }
        im = pick;
    }
    float dppx = 1.0f;
    if (im->kind == EMBI_IMG_IMAGE_SET) {
        /* image-set(): the option for the device (W4g4c, content_img.c); its resolution scales the natural size (CSS
         * Images 4 section 2.5, image-set-resolution-002). */
        im = embi_lay_image_resolve(im, &dppx);
        if (im == nullptr) {
            return true;
        }
    }
    xpart parts[XFADE_MAX];
    uint32_t nparts = 0;
    float xnw = 0.0f, xnh = 0.0f;
    bool xnat = false;
    if (im->kind == EMBI_IMG_CROSS_FADE) {
        nparts = xfade_parts(p, im, parts, &xnw, &xnh, &xnat);
        if (nparts == 0) {
            return true;
        }
    }
    bool is_url = im->kind == EMBI_IMG_URL;
    if (!is_url && im->kind != EMBI_IMG_GRADIENT && nparts == 0) {
        return true;
    }
    if (is_url && lp->mask && im->url.len > 0 && im->url.ptr[0] == '#') {
        /* R4e: a mask element of the document. Its content is the layer, with the mask value as alpha. A reference that names
         * no mask is a layer of transparent black; a mask in a tree that is not rendered is disabled (no layer). The mask of a
         * foreignObject is applied by the SVG walk. */
        const embi_box *fb = f != nullptr && f->kind == EMBI_FK_BOX ? f->box : nullptr;
        if (fb == nullptr || fb->el == nullptr || (fb->flags2 & EMBI_BF2_SVGFO) != 0) {
            return false;
        }
        embi_svg_env env;
        embi_pb_svg_env(p, &env);
        if (embi_svg_ref_content(&env, p->d, fb->el->base.doc, im->url, false, bb, xf, clip) == EMBI_SVGFX_REF_OFF) {
            return false;
        }
        p->mask_svg = true;
        return true;
    }
    uint16_t origin_kw = kw_or(layer_at(lp->org, k), lp->mask ? EMBI_KW_border_box : EMBI_KW_padding_box);
    uint16_t clip_kw = kw_or(layer_at(lp->clp, k), EMBI_KW_border_box);
    uint16_t attach = lp->mask ? EMBI_KW_scroll : kw_or(layer_at(lp->att, k), EMBI_KW_scroll);
    if ((bleed || (attach == EMBI_KW_local && (f->flags & EMBI_FF_SCROLL) != 0)) && clip_kw == EMBI_KW_border_box) {
        clip_kw = EMBI_KW_padding_box;
    }
    bool fixed = attach == EMBI_KW_fixed;
    float area[4];
    if (fixed) {
        /* The viewport, in the space of the box: the scroll containers around it move the content, not the viewport. A
         * scroll changes where this lies, so it is built again for each offset. */
        float ux, uy;
        embi_pb_unscroll_delta(p, xf, &ux, &uy);
        area[0] = -ux;
        area[1] = -uy;
        area[2] = p->vw;
        area[3] = p->vh;
        embi_pb_scroll_rigid(p);
    } else {
        /* an image is placed from the unsnapped box and its phase snapped below (a gradient uses the snapped box) */
        area_of(f, p->bg_raw != nullptr && is_url ? p->bg_raw : bb, origin_kw, area);
    }
    float carea[4], cradii[4];
    if (clip_kw == EMBI_KW_no_clip) {
        /* mask-clip: no-clip. The tiles are not cut at the box: the area is the box grown by a viewport. */
        float g = p->vw + p->vh;
        carea[0] = bb[0] - g;
        carea[1] = bb[1] - g;
        carea[2] = bb[2] + 2.0f * g;
        carea[3] = bb[3] + 2.0f * g;
        memset(cradii, 0, sizeof cradii);
    } else {
        area_of(f, bb, clip_kw, carea);
        radii_of(p, s, f, bb, clip_kw, cradii);
    }
    if (p->canvas_bg) {
        carea[0] = carea[1] = 0.0f;
        carea[2] = p->vw;
        carea[3] = p->vh;
        memset(cradii, 0, sizeof cradii);
    }

    float nw = xnw, nh = xnh;
    bool has_nat = is_url || xnat;
    if (is_url) {
        bool ratio = false;
        if (p->cfg->image_size == nullptr || !p->cfg->image_size(p->cfg->user, im->url, &nw, &nh, &ratio) ||
            !(nw > 0.0f) || !(nh > 0.0f)) {
            return false; /* an image that is not loaded: a mask layer is dropped, a background layer paints nothing */
        }
        if (dppx > 0.0f && dppx != 1.0f) {
            nw /= dppx;
            nh /= dppx;
        }
    }
    if (attach == EMBI_KW_local && !fixed && (f->flags & EMBI_FF_SCROLL) != 0) {
        /* local: the positioning area is the scrollable overflow area of the scroll container (CSS Backgrounds 3
         * section 3.5), the origin box grown by the scroll range. */
        float smx = 0.0f, smy = 0.0f;
        embi_scroll_max(f, &smx, &smy);
        area[2] += smx;
        area[3] += smy;
    }
    float tw, th;
    size_of(p, layer_at(lp->sz, k), area, nw, nh, has_nat, fs, &tw, &th);
    if (!(tw > 0.0f) || !(th > 0.0f)) {
        return true;
    }
    if (is_url) {
        /* An image tile is at least one px each way (as in Blink): a near-zero size still paints the image. */
        tw = tw < 1.0f ? 1.0f : tw;
        th = th < 1.0f ? 1.0f : th;
    }
    round_one_axis(layer_at(lp->rep, k), layer_at(lp->sz, k), area, &tw, &th);
    /* local: the origin scrolls with the content of a scroll container (its background moves with the scroll
     * offset); scroll and fixed do not. */
    if (attach == EMBI_KW_local && !fixed && (f->flags & EMBI_FF_SCROLL) != 0) {
        embi_pb_scroll_rigid(p); /* the origin follows this container's offset, which is not a transform */
        area[0] -= embi_frag_sx(f);
        area[1] -= embi_frag_sy(f);
    }

    /* Position: an offset from the origin's left (or top) edge, or from the right (or bottom) edge. */
    embi_value pxv0 = {0}, pyv0 = {0};
    const embi_value *pxv = nullptr, *pyv = nullptr;
    if (lp->mask) {
        const embi_value *pv = layer_at(lp->pos, k);
        if (pv != nullptr && pv->kind == EMBI_V_POSITION && pv->pos != nullptr) {
            pxv0.kind = EMBI_V_POS_AXIS;
            pxv0.axis = &pv->pos->x;
            pyv0.kind = EMBI_V_POS_AXIS;
            pyv0.axis = &pv->pos->y;
            pxv = &pxv0;
            pyv = &pyv0;
        }
    } else {
        pxv = layer_at(lp->px, k);
        pyv = layer_at(lp->py, k);
    }
    float ox = area[0], oy = area[1];
    bool ok = false;
    if (pxv != nullptr && pxv->kind == EMBI_V_POS_AXIS && pxv->axis != nullptr) {
        float off = embi_pb_len(p, &pxv->axis->off, area[2] - tw, fs, &ok);
        if (ok) {
            ox = pxv->axis->anchor != 0 ? area[0] + (area[2] - tw) - off : area[0] + off;
        }
    }
    if (pyv != nullptr && pyv->kind == EMBI_V_POS_AXIS && pyv->axis != nullptr) {
        float off = embi_pb_len(p, &pyv->axis->off, area[3] - th, fs, &ok);
        if (ok) {
            oy = pyv->axis->anchor != 0 ? area[1] + (area[3] - th) - off : area[1] + off;
        }
    }

    if (is_url) {
        /* An image's tile phase is snapped to whole px, as browsers do (a half px phase would blur every tile). A tile
         * that is not repeated on an axis snaps its far edge too (its rect is pixel snapped, as in Blink). */
        float ex = ox + tw, ey = oy + th;
        ox = roundf(ox);
        oy = roundf(oy);
        int bx, by;
        tile_mode(layer_at(lp->rep, k), &bx, &by);
        if (bx == BR_NO) {
            tw = fmaxf(1.0f, roundf(ex) - ox);
        }
        if (by == BR_NO) {
            th = fmaxf(1.0f, roundf(ey) - oy);
        }
    }
    int mx, my;
    tile_mode(layer_at(lp->rep, k), &mx, &my);
    tiles1 tx = axis_tiles(mx, ox, tw, area[0], area[2]);
    tiles1 ty = axis_tiles(my, oy, th, area[1], area[3]);
    if (tx.count == 0 || ty.count == 0) {
        return true;
    }
    float span[4];
    float sx0, sx1, sy0, sy1;
    if (!axis_span(&tx, carea[0], carea[0] + carea[2], &sx0, &sx1) ||
        !axis_span(&ty, carea[1], carea[1] + carea[3], &sy0, &sy1)) {
        return true; /* the tiles miss the clip area */
    }
    span[0] = sx0;
    span[1] = sy0;
    span[2] = sx1 - sx0;
    span[3] = sy1 - sy0;
    uint32_t rc[4], nr = 1;
    if (clip_kw == EMBI_KW_border_area && !p->canvas_bg) {
        nr = ring_clips(p, s, f, bb, xf, clip, rc);
    } else {
        rc[0] = area_clip(p, carea, cradii, xf, clip);
    }
    for (uint32_t ri = 0; ri < nr; ri++) {
        uint32_t lclip = rc[ri];
        if (nparts > 0) {
            xfade_draw(p, s, parts, nparts, &tx, &ty, span, fs, nearest, xf, lclip);
        } else if (is_url) {
            float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            if (mx == BR_NO && my == BR_NO) {
                float t[4] = {tx.first, ty.first, tx.size, ty.size};
                embi_pb_image(p, im->url, t, uv, WHITE_RGBA, nearest, xf, lclip);
            } else {
                float first[2] = {tx.first, ty.first};
                float size[2] = {tx.size, ty.size};
                float step[2] = {tx.step, ty.step};
                embi_pb_image_tile(p, im->url, span, first, size, step, nullptr, WHITE_RGBA, nearest, xf, lclip);
            }
        } else {
            gradient_layer(p, im->grad, s, &tx, &ty, span, fs, 1.0f, xf, lclip);
        }
    }
    return true;
}

/* The blend mode of background layer k (background-blend-mode repeats its list over the layers). */
static uint32_t layer_blend(const embi_value *bm, uint32_t k)
{
    const embi_value *v = layer_at(bm, k);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? embi_pb_blend_kw(v->kw) : EMBI_BM_NORMAL;
}

/* Whether the padding box of a bleed-avoiding box has square corners: only then does the color avoid the bleed too. A curved
 * padding edge would be anti-aliased twice (the color's clip and the border's inner edge), and what is behind the box
 * would show through the seam; the color then keeps its border-box clip and the border is drawn over it. */
static bool square_inside(embi_pst *p, const embi_style *s, const embi_frag *f, const float bb[4])
{
    float r[4];
    radii_of(p, s, f, bb, EMBI_KW_padding_box, r);
    return !(r[0] > 0.0f || r[1] > 0.0f || r[2] > 0.0f || r[3] > 0.0f);
}

void embi_pb_background(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                        uint32_t clip)
{
    if (s == nullptr) {
        return;
    }
    float fs = embi_pb_fontsize(s);
    p->bg_f = f;
    p->bg_s = s;
    const embi_value *clp = embi_style_value(s, EMBI_PROP_background_clip);
    /* The number of layers is the number of background-image values; the list of background-clip is cut or repeated to it
     * and the color is clipped by the last layer. */
    uint32_t nclip = layer_count(embi_style_value(s, EMBI_PROP_background_image));
    uint16_t last_clip = kw_or(layer_at(clp, nclip > 0 ? nclip - 1 : 0), EMBI_KW_border_box);
    bool bleed = bleed_avoid(p, s, f, bb);
    /* A local background of a scroll container is painted with its contents, inside the padding box (as in Blink). */
    bool local_last = (f->flags & EMBI_FF_SCROLL) != 0 &&
                      kw_or(layer_at(embi_style_value(s, EMBI_PROP_background_attachment), nclip > 0 ? nclip - 1 : 0),
                            EMBI_KW_scroll) == EMBI_KW_local;
    if ((local_last || (bleed && square_inside(p, s, f, bb))) && last_clip == EMBI_KW_border_box) {
        last_clip = EMBI_KW_padding_box;
    }

    /* background-blend-mode (R3b): the layers blend with the layers below them inside the background, which is an isolated
     * group (a layer does not blend with what is behind the element). The group exists only when a layer blends. */
    const embi_value *img = embi_style_value(s, EMBI_PROP_background_image);
    const embi_value *bm = embi_style_value(s, EMBI_PROP_background_blend_mode);
    bool has_img = img != nullptr && !(img->kind == EMBI_V_KEYWORD && img->kw == EMBI_KW_none);
    uint32_t n = has_img ? layer_count(img) : 0;
    bool blends = false;
    for (uint32_t k = 0; k < n && bm != nullptr; k++) {
        const embi_value *lv = layer_at(img, k);
        if (layer_blend(bm, k) != EMBI_BM_NORMAL && lv != nullptr && lv->kind == EMBI_V_IMAGE) {
            blends = true;
        }
    }
    uint32_t wrap = 0;
    uint32_t nblend0 = p->nblend;
    if (blends) {
        wrap = embi_pb_group_begin(p, EMBI_BM_NORMAL, true, xf, clip);
    }

    /* The color paints the last layer's clip area, under every image. */
    uint32_t rgba = 0;
    if (embi_pb_color(s, embi_style_value(s, EMBI_PROP_background_color), &rgba) && (rgba >> 24) != 0) {
        float area[4], radii[4];
        area_of(f, bb, last_clip, area);
        radii_of(p, s, f, bb, last_clip, radii);
        if (p->canvas_bg) {
            area[0] = area[1] = 0.0f;
            area[2] = p->vw;
            area[3] = p->vh;
            memset(radii, 0, sizeof radii);
        }
        if (last_clip == EMBI_KW_border_area && !p->canvas_bg) {
            uint32_t rc[4];
            uint32_t nr = ring_clips(p, s, f, bb, xf, clip, rc);
            for (uint32_t i = 0; i < nr; i++) {
                embi_pb_fill(p, area, radii, rgba, xf, rc[i]);
            }
        } else if (area_clip_needed(area, radii)) {
            float zero[4] = {0, 0, 0, 0};
            embi_pb_fill(p, area, zero, rgba, xf, area_clip(p, area, radii, xf, clip));
        } else {
            embi_pb_fill(p, area, radii, rgba, xf, clip);
        }
    }

    if (has_img) {
        lprops lp = {false,
                     img,
                     embi_style_value(s, EMBI_PROP_background_size),
                     embi_style_value(s, EMBI_PROP_background_repeat),
                     embi_style_value(s, EMBI_PROP_background_origin),
                     clp,
                     embi_style_value(s, EMBI_PROP_background_attachment),
                     embi_style_value(s, EMBI_PROP_background_position_x),
                     embi_style_value(s, EMBI_PROP_background_position_y),
                     nullptr};
        bool nearest = nearest_flag(s) != 0;
        /* The first layer is the top one, so the last is painted first. */
        for (uint32_t k = n; k-- > 0;) {
            uint32_t mode = blends ? layer_blend(bm, k) : EMBI_BM_NORMAL;
            uint32_t g = 0;
            if (mode != EMBI_BM_NORMAL) {
                g = embi_pb_group_begin(p, mode, false, xf, clip);
            }
            paint_layer(p, f, s, bb, xf, clip, &lp, k, fs, nearest, bleed);
            if (mode != EMBI_BM_NORMAL) {
                embi_pb_group_end(p, g);
            }
        }
    }
    if (blends) {
        embi_pb_group_end(p, wrap);
        p->nblend = nblend0; /* the blends are inside the isolated background */
    }
}

/* The mask layers of a box (R3b, mask-image and its companions): one EMBI_LS_MASK group per layer, topmost first, each holding
 * the tiles of its image. Returns the number of layers emitted (0 when mask-image has no image or every reference is unresolvable: then there is no mask). The caller has opened the composite
 * group that owns them. */
uint32_t embi_pb_mask_layers(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                             uint32_t clip)
{
    const embi_value *img = embi_style_value(s, EMBI_PROP_mask_image);
    if (img == nullptr || (img->kind == EMBI_V_KEYWORD && img->kw == EMBI_KW_none)) {
        return 0;
    }
    uint32_t n = layer_count(img);
    lprops lp = {true,
                 img,
                 embi_style_value(s, EMBI_PROP_mask_size),
                 embi_style_value(s, EMBI_PROP_mask_repeat),
                 embi_style_value(s, EMBI_PROP_mask_origin),
                 embi_style_value(s, EMBI_PROP_mask_clip),
                 nullptr,
                 nullptr,
                 nullptr,
                 embi_style_value(s, EMBI_PROP_mask_position)};
    const embi_value *mode = embi_style_value(s, EMBI_PROP_mask_mode);
    const embi_value *comp = embi_style_value(s, EMBI_PROP_mask_composite);
    float fs = embi_pb_fontsize(s);
    bool nearest = nearest_flag(s) != 0;
    uint32_t emitted = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t mk = kw_or(layer_at(mode, k), EMBI_KW_match_source);
        uint16_t ck = kw_or(layer_at(comp, k), EMBI_KW_add);
        uint32_t m = mk == EMBI_KW_luminance ? EMBI_MM_LUMINANCE : EMBI_MM_ALPHA;
        uint32_t c = ck == EMBI_KW_subtract    ? EMBI_MC_SUBTRACT
                     : ck == EMBI_KW_intersect ? EMBI_MC_INTERSECT
                     : ck == EMBI_KW_exclude   ? EMBI_MC_EXCLUDE
                                               : EMBI_MC_ADD;
        uint32_t mb = embi_pb_mask_begin(p, m, c, xf, clip);
        p->mask_svg = false;
        if (!paint_layer(p, f, s, bb, xf, clip, &lp, k, fs, nearest, false)) {
            /* An unresolvable reference (an image that is not loaded, a mask that is not rendered) is no layer at all, as in
             * browsers. */
            p->d->items.len = mb;
            continue;
        }
        if (p->mask_svg) {
            p->d->items.data[mb].colors[0] = EMBI_MM_ALPHA; /* the SVG mask's items carry the mask value as alpha */
            p->mask_svg = false;
        }
        embi_pb_mask_end(p, mb);
        emitted++;
    }
    return emitted;
}

/* W4g4c: the URL of an img (into sb) from its src and srcset attributes, and the density of that candidate. False when it
 * has none. */
static bool img_source(embi_element *el, embi_sb *sb, float *dens)
{
    const embi_attr *a = embi_el_get_attribute(el, (embi_str){"src", 3});
    const embi_attr *set = embi_el_get_attribute(el, (embi_str){"srcset", 6});
    if (a == nullptr && set == nullptr) {
        return false;
    }
    embi_sb src = {0}, ss = {0};
    if (a != nullptr) {
        embi_dstr_append_utf8(&src, a->value);
    }
    if (set != nullptr) {
        embi_dstr_append_utf8(&ss, set->value);
    }
    embi_str url = embi_lay_img_source((embi_str){src.data != nullptr ? src.data : "", src.len},
                                       (embi_str){ss.data != nullptr ? ss.data : "", ss.len}, dens);
    if (url.len > 0) {
        embi_sb_append(sb, url);
    }
    embi_sb_free(&src);
    embi_sb_free(&ss);
    if (sb->len == 0) {
        embi_sb_free(sb);
        return false;
    }
    return true;
}

/* W4g4c: the image of a replaced box that the content property made (generated content, CSS Content 3 section 2, or
 * element replacement) or list-style-image (an image marker), resolved through image-set(); false for the other replaced
 * boxes. The image items of a generated box's content are its replaced boxes in order. */
static bool replaced_content_image(const embi_box *b, const embi_style *s, embi_image *out)
{
    if (s == nullptr) {
        return false;
    }
    if (b->pe == EMBI_PE_MARKER) {
        const embi_value *v = embi_style_value(s, EMBI_PROP_list_style_image);
        const embi_image *im = nullptr;
        embi_image one;
        memset(&one, 0, sizeof one);
        if (v != nullptr && v->kind == EMBI_V_URL) {
            one.kind = EMBI_IMG_URL;
            one.url = v->str;
            im = &one;
        } else if (v != nullptr && v->kind == EMBI_V_IMAGE && v->image != nullptr) {
            im = embi_lay_image_resolve(v->image, nullptr);
        }
        if (im == nullptr || (im->kind != EMBI_IMG_URL && im->kind != EMBI_IMG_GRADIENT)) {
            return false;
        }
        *out = *im;
        return true;
    }
    const embi_value *cv = embi_style_value(s, EMBI_PROP_content);
    if (cv == nullptr || cv->kind == EMBI_V_KEYWORD) {
        return false;
    }
    if (b->pe == 0) {
        return embi_lay_content_replaces(cv, out);
    }
    uint32_t idx = 0;
    if (b->parent != nullptr) {
        for (const embi_box *k = b->parent->first; k != nullptr && k != b; k = k->next) {
            if (k->kind == EMBI_BK_REPLACED && k->pe == b->pe && k->el == b->el) {
                idx++;
            }
        }
    }
    return embi_lay_content_image(cv, idx, out);
}

/* A gradient that replaces a box's content fills its content box once (no natural size: the concrete object size is
 * the box, CSS Images 3 section 4.3). */
static void content_gradient(embi_pst *p, const embi_frag *f, const embi_gradient *g, const embi_style *s, const float bb[4],
                             uint32_t xf, uint32_t clip)
{
    if (g == nullptr) {
        return;
    }
    float pb[4], cb[4];
    embi_pb_boxes(f, bb, pb, cb);
    if (!(cb[2] > 0.0f) || !(cb[3] > 0.0f)) {
        return;
    }
    p->lh = embi_pb_lineheight(s);
    p->bg_f = f;
    p->bg_s = s;
    tiles1 tx = axis_tiles(BR_NO, cb[0], cb[2], cb[0], cb[2]);
    tiles1 ty = axis_tiles(BR_NO, cb[1], cb[3], cb[1], cb[3]);
    gradient_layer(p, g, s, &tx, &ty, cb, embi_pb_fontsize(s), 1.0f, xf, clip);
}

void embi_pb_replaced(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4],
                      uint32_t xf, uint32_t clip)
{
    if (b->el != nullptr && b->el->base.extra == EMBI_NS_SVG && embi_pb_named(b->el, "svg")) {
        float pb[4], cb[4];
        embi_pb_boxes(f, bb, pb, cb);
        embi_svg_env env;
        embi_pb_svg_env(p, &env);
        const uint32_t items0 = p->d->items.len, fx0 = p->d->fx_items;
        embi_svg_paint(&env, p->d, b->el, cb, xf, clip);
        if (p->d->fx_items != fx0) {
            /* R4e: a blend group inside the svg counts for the isolation of the stacking context that holds it (composite.c) */
            for (uint32_t i = items0; i < p->d->items.len; i++) {
                const embi_ditem *it = &p->d->items.data[i];
                if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE && it->colors[0] != EMBI_BM_NORMAL) {
                    p->nblend++;
                    break;
                }
            }
        }
        return;
    }
    if (b->el == nullptr) {
        return;
    }
    if (b->el->base.extra != EMBI_NS_SVG && embi_pb_named(b->el, "iframe")) {
        /* IF1: the frame's document fills the content box (its viewport), clipped to it and to its curved corners. The
         * renderer draws it from the frame's texture, found by the element in the name. */
        float pb[4], cb[4];
        embi_pb_boxes(f, bb, pb, cb);
        if (!(cb[2] > 0.0f) || !(cb[3] > 0.0f)) {
            return;
        }
        char name[48];
        int n = SDL_snprintf(name, sizeof name, "ember-frame:%p", (const void *)b->el);
        float crad[4];
        radii_of(p, s, f, bb, EMBI_KW_content_box, crad);
        uint32_t lclip = clip;
        if (crad[0] > 0.0f || crad[1] > 0.0f || crad[2] > 0.0f || crad[3] > 0.0f) {
            lclip = area_clip(p, cb, crad, xf, clip);
        } else {
            float zero[4] = {0, 0, 0, 0};
            lclip = embi_pb_clip(p, cb, zero, xf, clip);
        }
        float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        embi_pb_image(p, (embi_str){name, (uint32_t)n}, cb, uv, WHITE_RGBA, false, xf, lclip);
        return;
    }
    embi_image cimg;
    bool from_content = replaced_content_image(b, s, &cimg);
    if (from_content && cimg.kind == EMBI_IMG_GRADIENT) {
        content_gradient(p, f, cimg.grad, s, bb, xf, clip);
        return;
    }
    bool canvas = !from_content && embi_pb_named(b->el, "canvas");
    if (!from_content && !canvas && !embi_pb_named(b->el, "img")) {
        return;
    }
    embi_sb sb = {0};
    float iw = 0.0f, ih = 0.0f, dens = 1.0f;
    uint32_t cver = 0;
    if (from_content) {
        embi_sb_append(&sb, cimg.url);
        if (sb.len == 0) {
            embi_sb_free(&sb);
            return;
        }
    } else if (canvas) {
        /* R4b: a canvas with a context is a picture of its own, named by the element (the GPU side finds the texture
         * from the name). The version rides in aux2 so that a draw changes the item and damages its rect. */
        uint32_t cw = 0, ch = 0;
        if (!embi_canvas_paint_info(b->el, &cver, &cw, &ch) || cw == 0 || ch == 0) {
            return;
        }
        char name[48];
        int n = SDL_snprintf(name, sizeof name, "ember-canvas:%p", (const void *)b->el);
        embi_sb_append(&sb, (embi_str){name, (uint32_t)n});
        iw = (float)cw;
        ih = (float)ch;
    } else if (!img_source(b->el, &sb, &dens)) {
        return;
    }
    embi_str url = {sb.data, sb.len};
    /* The natural size comes from the image cache (the callback). The layout's rw and rh are not used: for an
     * image it cannot size they hold the width and height attributes, which would paint a broken image. */
    bool ratio = false;
    if (!canvas && (p->cfg->image_size == nullptr || !p->cfg->image_size(p->cfg->user, url, &iw, &ih, &ratio) ||
                    !(iw > 0.0f) || !(ih > 0.0f))) {
        embi_sb_free(&sb);
        return; /* a broken image paints nothing */
    }
    if (dens > 0.0f && dens != 1.0f) {
        iw /= dens; /* a srcset density candidate: the natural size the layout gave the box */
        ih /= dens;
    }
    float pb[4], cb[4];
    embi_pb_boxes(f, bb, pb, cb);
    float fs = embi_pb_fontsize(s);
    float dw = cb[2], dh = cb[3];
    uint16_t fit = kw_or(embi_style_value(s, EMBI_PROP_object_fit), EMBI_KW_fill);
    float sx = cb[2] / iw, sy = cb[3] / ih;
    switch (fit) {
    case EMBI_KW_contain:
        dw = dh = 0;
        {
            float sc = sx < sy ? sx : sy;
            dw = iw * sc;
            dh = ih * sc;
        }
        break;
    case EMBI_KW_cover: {
        float sc = sx > sy ? sx : sy;
        dw = iw * sc;
        dh = ih * sc;
        break;
    }
    case EMBI_KW_none:
        dw = iw;
        dh = ih;
        break;
    case EMBI_KW_scale_down: {
        float sc = sx < sy ? sx : sy;
        if (sc > 1.0f) {
            sc = 1.0f;
        }
        dw = iw * sc;
        dh = ih * sc;
        break;
    }
    default:
        break;
    }
    /* object-position, default 50% 50%. */
    float x = cb[0] + (cb[2] - dw) * 0.5f, y = cb[1] + (cb[3] - dh) * 0.5f;
    const embi_value *op = embi_style_value(s, EMBI_PROP_object_position);
    if (op != nullptr && op->kind == EMBI_V_POSITION && op->pos != nullptr) {
        bool ok = false;
        float off = embi_pb_len(p, &op->pos->x.off, cb[2] - dw, fs, &ok);
        if (ok) {
            x = op->pos->x.anchor != 0 ? cb[0] + (cb[2] - dw) - off : cb[0] + off;
        }
        off = embi_pb_len(p, &op->pos->y.off, cb[3] - dh, fs, &ok);
        if (ok) {
            y = op->pos->y.anchor != 0 ? cb[1] + (cb[3] - dh) - off : cb[1] + off;
        }
    }
    /* The position snaps to whole px like a background image's tile phase (the size keeps its fraction). */
    x = roundf(x);
    y = roundf(y);
    float dest[4] = {x, y, dw, dh};
    uint32_t lclip = clip;
    /* The content is clipped to the content box's curved corners (CSS Backgrounds 3 section 5.3). */
    float crad[4];
    radii_of(p, s, f, bb, EMBI_KW_content_box, crad);
    if (crad[0] > 0.0f || crad[1] > 0.0f || crad[2] > 0.0f || crad[3] > 0.0f) {
        lclip = area_clip(p, cb, crad, xf, clip);
    } else if (x < cb[0] - 0.01f || y < cb[1] - 0.01f || x + dw > cb[0] + cb[2] + 0.01f ||
               y + dh > cb[1] + cb[3] + 0.01f) {
        float zero[4] = {0, 0, 0, 0};
        lclip = embi_pb_clip(p, cb, zero, xf, clip);
    }
    float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    size_t before = p->d->items.len;
    embi_pb_image(p, url, dest, uv, WHITE_RGBA, nearest_flag(s), xf, lclip);
    if (canvas && p->d->items.len > before) {
        p->d->items.data[before].aux2 = cver;
    }
    embi_sb_free(&sb);
}

static const embi_svg_image *embi_pb_svg_image(void *user, embi_str url)
{
    return embi_pb_svg_lookup(user, url);
}

static void embi_pb_svg_foreign(void *host, const embi_element *fo, uint32_t xf, uint32_t clip)
{
    embi_pb_paint_foreign((embi_pst *)host, fo, xf, clip);
}

/* R4e: the mask layers of an SVG element, which has no box fragment: a fragment of the reference box's size stands in (no
 * borders, padding or margins, so every box keyword names the reference box). */
static uint32_t embi_pb_svg_mask_layers(void *host, const embi_element *el, const embi_style *st, const float bb[4],
                                        uint32_t xf, uint32_t clip)
{
    (void)el;
    embi_pst *p = host;
    embi_frag f;
    memset(&f, 0, sizeof f);
    f.kind = EMBI_FK_BOX;
    f.w = bb[2];
    f.h = bb[3];
    const embi_frag *saved_f = p->bg_f;
    const embi_style *saved_s = p->bg_s;
    uint32_t n = embi_pb_mask_layers(p, &f, st, bb, xf, clip);
    p->bg_f = saved_f;
    p->bg_s = saved_s;
    return n;
}

void embi_pb_svg_env(embi_pst *p, embi_svg_env *env)
{
    memset(env, 0, sizeof *env);
    env->se = p->se;
    env->text = p->text;
    env->dev_scale = p->cfg->scale > 0 ? p->cfg->scale : 1.0f;
    env->vw = p->vw;
    env->vh = p->vh;
    env->user = p->cfg->user;
    env->image_size = p->cfg->image_size;
    env->svg_image = embi_pb_svg_image;
    env->host = p;
    env->paint_foreign = embi_pb_svg_foreign;
    env->mask_layers = embi_pb_svg_mask_layers;
}

/* ------------------------------------------------------------------ border images (bimage.c) */

const embi_image *embi_pb_simple_image(const embi_pst *p, const embi_image *im)
{
    return simple_image(p, im);
}

bool embi_pb_grad_prepare(embi_pst *p, const embi_gradient *g, const embi_style *s, const float img[4], float fs,
                          uint8_t *kind, float geo[6], uint32_t *first, uint32_t *n)
{
    float ref = 0.0f;
    *kind = grad_geo(p, g, img, fs, geo, &ref);
    /* the same largest visible position as gradient_layer: the farthest corner of the image box from a radial center */
    float wmax = 1.0f;
    if (*kind == EMBI_GRAD_RADIAL_DL) {
        float rx = geo[2], ry = geo[3];
        if (rx > 0.0f && ry > 0.0f) {
            float fdx = fmaxf(fabsf(img[0] - geo[0]), fabsf(img[0] + img[2] - geo[0]));
            float fdy = fmaxf(fabsf(img[1] - geo[1]), fabsf(img[1] + img[3] - geo[1]));
            wmax = hypotf(fdx / rx, fdy / ry) * 1.001f + 0.01f;
        } else {
            wmax = 1.0e9f;
        }
    }
    return grad_stops(p, g, s, ref, fs, wmax, 1.0f, first, n);
}
