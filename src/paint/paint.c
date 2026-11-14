/*
 * The display list builder (W4c): CSS 2.1 Appendix E paint order over the
 * fragment tree, stacking contexts, opacity layers, overflow clips and 2D transforms. The painters of one
 * box (backgrounds, borders, text, replaced content) are in background.c, border.c and text_paint.c; this
 * file has the style helpers they share, the walk and the context logic.
 *
 * Paint order of a stacking context S (Appendix E, with the W4 rules):
 *   1. the background and borders of S itself;
 *   2. the child stacking contexts with a negative z-index, by z-index, then tree order;
 *   3. the block-level in-flow descendants: their backgrounds and borders (not floats, not positioned);
 *   4. the floats, each painted as a pseudo stacking context;
 *   5. the inline content: inline boxes, text, and inline-level atoms (inline-block, replaced), each atom as a
 *      pseudo stacking context;
 *   6. the positioned descendants with z-index auto (and the stacking contexts with z-index 0), in tree order,
 *      each painted as a pseudo stacking context;
 *   7. the child stacking contexts with a positive z-index, by z-index, then tree order.
 * The flow phases 3 to 5 are separate walks over the same subtree, so each one only emits its own items.
 */
#include "dom/dom.h"
#include "paint/paint_int.h"
#include "paint/plane3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The phases of a walk over the subtree of a stacking context (or pseudo stacking context). */
enum {
    PH_BLOCK = 0, /* step 3 */
    PH_FLOAT,     /* step 4 */
    PH_INLINE,    /* step 5 */
    PH_POS,       /* step 6 */
};

/* ------------------------------------------------------------------ style helpers */

#define WHITE_RGBA 0xFFFFFFFFu

static const embi_style *fstyle(const embi_frag *f)
{
    return f->box != nullptr ? f->box->style : nullptr;
}

uint16_t embi_pb_kw(const embi_style *s, embi_prop_id prop)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, prop) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

typedef struct lenenv {
    const embi_pst *p;
    float fs;
} lenenv;

/* The size of the content box of the nearest ancestor of f that is a size container (container-type: size or
 * inline-size) for the cq units; NaN for an axis it does not contain. */
static void container_size(const embi_frag *f, double *w, double *h)
{
    *w = *h = __builtin_nan("");
    for (const embi_frag *a = f != nullptr ? f->parent : nullptr; a != nullptr; a = a->parent) {
        if (a->kind != EMBI_FK_BOX || a->box == nullptr || a->box->style == nullptr) {
            continue;
        }
        uint16_t ct = embi_pb_kw(a->box->style, EMBI_PROP_container_type);
        if (ct != EMBI_KW_size && ct != EMBI_KW_inline_size) {
            continue;
        }
        *w = (double)(a->w - embi_frag_bdr(a, 0) - embi_frag_bdr(a, 2) - embi_frag_pad(a, 0) - embi_frag_pad(a, 2));
        if (ct == EMBI_KW_size) {
            *h = (double)(a->h - embi_frag_bdr(a, 1) - embi_frag_bdr(a, 3) - embi_frag_pad(a, 1) - embi_frag_pad(a, 3));
        }
        return;
    }
}

/* Relative lengths in px. Viewport units come from the viewport the build was given; the font-relative and
 * container units resolve against the box whose background is being painted (p->bg_f, p->bg_s). */
static double unit_px(void *user, uint8_t unit, double v)
{
    const lenenv *e = user;
    double vw = e->p != nullptr ? e->p->vw : 0.0, vh = e->p != nullptr ? e->p->vh : 0.0;
    if (e->p != nullptr && e->p->bg_s != nullptr) {
        switch (unit) {
        case EMBI_U_LH:
        case EMBI_U_RLH:
        case EMBI_U_CQW:
        case EMBI_U_CQH:
        case EMBI_U_CQI:
        case EMBI_U_CQB:
        case EMBI_U_CQMIN:
        case EMBI_U_CQMAX:
        case EMBI_U_REM:
        {
            embi_len_ctx lc;
            memset(&lc, 0, sizeof lc);
            lc.font_px = e->fs;
            lc.root_px = 16.0;
            if (e->p->html != nullptr && fstyle(e->p->html) != nullptr) {
                lc.root_px = (double)embi_pb_fontsize(fstyle(e->p->html));
            }
            lc.vw = vw;
            lc.vh = vh;
            lc.cq_w = lc.cq_h = __builtin_nan("");
            container_size(e->p->bg_f, &lc.cq_w, &lc.cq_h);
            const embi_value *lh = embi_style_value(e->p->bg_s, EMBI_PROP_line_height);
            double lhpx = 1.2 * e->fs;
            if (lh != nullptr && (lh->kind == EMBI_V_NUMBER || lh->kind == EMBI_V_INTEGER)) {
                lhpx = lh->num * e->fs;
            } else if (lh != nullptr && lh->kind == EMBI_V_LENGTH && lh->unit == EMBI_U_PX) {
                lhpx = lh->num;
            } else if (lh != nullptr && lh->kind == EMBI_V_PERCENT) {
                lhpx = lh->num * e->fs / 100.0;
            }
            lc.font_metrics.lh = lhpx;
            lc.root_metrics.lh = lhpx;
            return embi_len_px_cb(&lc, unit, v);
        }
        default:
            break;
        }
    }
    switch (unit) {
    case EMBI_U_PX:
        return v;
    case EMBI_U_EM:
        return v * e->fs;
    case EMBI_U_REM:
        return v * 16.0;
    case EMBI_U_EX:
    case EMBI_U_CH:
        return v * e->fs * 0.5;
    case EMBI_U_CAP:
        return v * e->fs * 0.7;
    case EMBI_U_LH:
        return v * (e->p != nullptr ? e->p->lh : 0.0);
    case EMBI_U_VW:
    case EMBI_U_SVW:
    case EMBI_U_LVW:
    case EMBI_U_DVW:
        return v * vw / 100.0;
    case EMBI_U_VH:
    case EMBI_U_SVH:
    case EMBI_U_LVH:
    case EMBI_U_DVH:
        return v * vh / 100.0;
    case EMBI_U_VMIN:
    case EMBI_U_SVMIN:
    case EMBI_U_LVMIN:
    case EMBI_U_DVMIN:
        return v * (vw < vh ? vw : vh) / 100.0;
    case EMBI_U_VMAX:
    case EMBI_U_SVMAX:
    case EMBI_U_LVMAX:
    case EMBI_U_DVMAX:
        return v * (vw > vh ? vw : vh) / 100.0;
    default:
        return 0.0;
    }
}

float embi_pb_lineheight(const embi_style *s)
{
    float fs = embi_pb_fontsize(s);
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_line_height) : nullptr;
    if (v != nullptr && (v->kind == EMBI_V_NUMBER || v->kind == EMBI_V_INTEGER)) {
        return (float)v->num * fs;
    }
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    if (v != nullptr && v->kind == EMBI_V_PERCENT) {
        return (float)v->num * fs / 100.0f;
    }
    return 1.2f * fs;
}

float embi_pb_fontsize(const embi_style *s)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_font_size) : nullptr;
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    return 16.0f;
}

float embi_pb_len(const embi_pst *p, const embi_value *v, float ref, float fs, bool *ok)
{
    lenenv e = {p, fs};
    double r = 0.0;
    *ok = true;
    switch (v->kind) {
    case EMBI_V_LENGTH:
        r = unit_px(&e, v->unit, v->num);
        break;
    case EMBI_V_PERCENT:
        r = (double)ref * v->num / 100.0;
        break;
    case EMBI_V_CALC: {
        embi_calc_env ce = {(double)ref, unit_px, &e};
        r = embi_calc_eval(v->calc, &ce);
        if (isnan(r)) {
            *ok = false;
            return 0.0f;
        }
        break;
    }
    default:
        *ok = false;
        return 0.0f;
    }
    return (float)r;
}

bool embi_pb_color_full(const embi_style *s, const embi_value *v, embi_color *out)
{
    if (v == nullptr) {
        return false;
    }
    const embi_color *c = nullptr;
    if (v->kind == EMBI_V_COLOR) {
        c = &v->color;
    } else if (v->kind == EMBI_V_LIGHT_DARK && v->pair != nullptr && v->pair[0].kind == EMBI_V_COLOR) {
        c = &v->pair[0].color;
    } else {
        return false;
    }
    if (c->space == EMBI_CS_CURRENT) {
        const embi_value *cv = embi_style_value(s, EMBI_PROP_color);
        if (cv == nullptr || cv->kind != EMBI_V_COLOR || cv->color.space == EMBI_CS_CURRENT) {
            return false;
        }
        c = &cv->color;
    }
    if (c->space == EMBI_CS_DEFERRED) {
        const embi_value *cv = embi_style_value(s, EMBI_PROP_color);
        if (cv == nullptr || cv->kind != EMBI_V_COLOR || !embi_color_resolve_deferred(c, &cv->color, out)) {
            return false;
        }
        return true;
    }
    *out = *c;
    return true;
}

bool embi_pb_color(const embi_style *s, const embi_value *v, uint32_t *rgba)
{
    embi_color c;
    if (!embi_pb_color_full(s, v, &c)) {
        return false;
    }
    uint8_t px[4];
    if (!embi_color_to_srgb8(&c, px)) {
        return false;
    }
    *rgba = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
    return true;
}

float embi_pb_opacity(const embi_style *s)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_opacity) : nullptr;
    if (v == nullptr || v->kind != EMBI_V_NUMBER) {
        return 1.0f;
    }
    float a = (float)v->num;
    if (!(a >= 0.0f)) {
        return 0.0f;
    }
    return a > 1.0f ? 1.0f : a;
}

bool embi_pb_hidden(const embi_style *s)
{
    uint16_t kw = embi_pb_kw(s, EMBI_PROP_visibility);
    return kw == EMBI_KW_hidden || kw == EMBI_KW_collapse;
}

static const embi_prop_id k_radius[4] = {
    EMBI_PROP_border_top_left_radius,
    EMBI_PROP_border_top_right_radius,
    EMBI_PROP_border_bottom_right_radius,
    EMBI_PROP_border_bottom_left_radius,
};

void embi_pb_radii(const embi_pst *p, const embi_style *s, float w, float h, float fs, float r[4])
{
    embi_pb_radii_mask(p, s, w, h, fs, 0u, r);
}

void embi_pb_radii_mask(const embi_pst *p, const embi_style *s, float w, float h, float fs, unsigned zero, float r[4])
{
    for (int i = 0; i < 4; i++) {
        r[i] = 0.0f;
        const embi_value *v = embi_style_value(s, k_radius[i]);
        if (v == nullptr || (zero & (1u << i)) != 0) {
            continue;
        }
        const embi_value *a = v, *b = v;
        if (v->kind == EMBI_V_LIST && v->list.n >= 1) {
            a = &v->list.items[0];
            b = v->list.n >= 2 ? &v->list.items[1] : a;
        }
        bool ok1 = false, ok2 = false;
        float x = embi_pb_len(p, a, w, fs, &ok1);
        float y = embi_pb_len(p, b, h, fs, &ok2);
        if (ok1 && ok2) {
            float m = x < y ? x : y;
            r[i] = m > 0.0f ? m : 0.0f;
        }
    }
    /* CSS Backgrounds 3 section 5.5: when two radii on one side add up to more than the side, all radii are
     * scaled by the same factor. */
    float f = 1.0f;
    float sides[4][2] = {{r[0], r[1]}, {r[1], r[2]}, {r[2], r[3]}, {r[3], r[0]}};
    float lens[4] = {w, h, w, h};
    for (int i = 0; i < 4; i++) {
        float sum = sides[i][0] + sides[i][1];
        if (sum > lens[i] && sum > 0.0f && lens[i] / sum < f) {
            f = lens[i] / sum;
        }
    }
    if (f < 1.0f) {
        for (int i = 0; i < 4; i++) {
            r[i] *= f;
        }
    }
}

void embi_pb_inner_radii(const float outer[4], const float bdr[4], float out[4])
{
    /* bdr is left, top, right, bottom. The corners are top-left, top-right, bottom-right, bottom-left. */
    static const int adj[4][2] = {{1, 0}, {1, 2}, {3, 2}, {3, 0}};
    for (int i = 0; i < 4; i++) {
        float m = bdr[adj[i][0]] > bdr[adj[i][1]] ? bdr[adj[i][0]] : bdr[adj[i][1]];
        float v = outer[i] - m;
        out[i] = v > 0.0f ? v : 0.0f;
    }
}

void embi_pb_boxes(const embi_frag *f, const float bb[4], float pb[4], float cb[4])
{
    float bl = embi_frag_bdr(f, 0), bt = embi_frag_bdr(f, 1), br = embi_frag_bdr(f, 2), bbt = embi_frag_bdr(f, 3);
    pb[0] = bb[0] + bl;
    pb[1] = bb[1] + bt;
    pb[2] = bb[2] - bl - br;
    pb[3] = bb[3] - bt - bbt;
    if (pb[2] < 0.0f) {
        pb[2] = 0.0f;
    }
    if (pb[3] < 0.0f) {
        pb[3] = 0.0f;
    }
    if (cb != nullptr) {
        float pl = embi_frag_pad(f, 0), pt = embi_frag_pad(f, 1), pr = embi_frag_pad(f, 2), pbo = embi_frag_pad(f, 3);
        cb[0] = pb[0] + pl;
        cb[1] = pb[1] + pt;
        cb[2] = pb[2] - pl - pr;
        cb[3] = pb[3] - pt - pbo;
        if (cb[2] < 0.0f) {
            cb[2] = 0.0f;
        }
        if (cb[3] < 0.0f) {
            cb[3] = 0.0f;
        }
    }
}

/* ------------------------------------------------------------------ transforms */

/* The box's transform is composed as a 4x4 (layout/xform.c, CSS Transforms 2). A matrix that maps the plane of the box
 * affinely is projected to the 2D affine that the display list stores in xforms, and painted in place. A matrix with a
 * perspective part, or a box in a 3D rendering context, paints as a plane (plane3d.c). */

/* The 2D affine of a plane homography h (row-major, W constant), composed with the 2D transform parent_xform of the parent
 * state, as a new display list transform. */
static uint32_t xform_of_plane(embi_pst *p, const double h[9], uint32_t parent_xform)
{
    double m[9] = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]};
    p->d->cur_scroll = embi_pb_xform_scroll(p->d, parent_xform); /* the new transform is inside whatever scrolls its parent */
    if (parent_xform != 0) {
        const float *pm = p->d->xforms.data[parent_xform].m;
        double pa[9] = {pm[0], pm[2], pm[4], pm[1], pm[3], pm[5], 0.0, 0.0, 1.0};
        embi_h3_mul(pa, m, m);
    }
    double w = fabs(m[8]) > 1e-12 ? m[8] : 1.0;
    float out[6] = {(float)(m[0] / w), (float)(m[3] / w), (float)(m[1] / w), (float)(m[4] / w), (float)(m[2] / w),
                    (float)(m[5] / w)};
    return embi_pb_xform(p, out);
}

uint32_t embi_pb_box_xform(embi_pst *p, const embi_style *s, const float bb[4], uint32_t parent_xform)
{
    if (s == nullptr) {
        return parent_xform;
    }
    embi_m4 m;
    if (!embi_lay_box_matrix(s, bb, p->vw, p->vh, p->rem, &m)) {
        return parent_xform;
    }
    double h[9];
    embi_m4_plane(&m, h);
    return xform_of_plane(p, h, parent_xform);
}

/* The matrix that the perspective of the parent element of f gives f, when it has one. */
static bool parent_perspective(const embi_pst *p, const embi_frag *f, embi_m4 *out)
{
    if (p->pers.len == 0) {
        return false;
    }
    const embi_element *par = f->box != nullptr ? embi_lay_parent_el(f->box) : nullptr;
    for (uint32_t i = p->pers.len; i-- > 0;) {
        if (par != nullptr && p->pers.data[i].el == par) {
            *out = p->pers.data[i].m;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ the walk */

/* The document element is not a context of its own: root and html are one stacking context, so that the positioned
 * boxes of the viewport's containing block (children of root) and those inside html order against each other. */
static bool is_stacking(const embi_pst *p, const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && (f->flags & EMBI_FF_STACKING) != 0 && f != p->html;
}

/* Positioned boxes that are not stacking contexts (z-index auto) and the abspos ones. */
static bool positioned(const embi_frag *f)
{
    if (f->kind != EMBI_FK_BOX || f->box == nullptr) {
        return false;
    }
    uint16_t pos = embi_pb_kw(f->box->style, EMBI_PROP_position);
    return pos != 0 && pos != EMBI_KW_static;
}

static bool floats(const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && f->box != nullptr && (f->box->flags & EMBI_BF_FLOAT) != 0;
}

/* R4c: the box of a foreignObject: the SVG walk of its svg paints it (embi_pb_paint_foreign), the fragment walks do not. */
static bool svg_fo(const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && f->box != nullptr && (f->box->flags2 & EMBI_BF2_SVGFO) != 0;
}

static void child_origin(embi_pst *p, const embi_frag *k, float ox, float oy, float *kx, float *ky)
{
    float dx = 0.0f, dy = 0.0f;
    if (embi_frag_sticky(k) != nullptr) {
        embi_pb_scroll_rigid(p); /* a sticky box's offset follows the scroll offset: a scroll builds the list again */
    }
    embi_lay_sticky_offset(k, &dx, &dy);
    *kx = ox + k->x + dx;
    *ky = oy + k->y + dy;
}

/* Whether the style includes paint containment (contain: paint, strict or content). */
static bool contains_paint(const embi_style *s)
{
    const embi_value *cv = embi_style_value(s, EMBI_PROP_contain);
    if (cv == nullptr) {
        return false;
    }
    if (cv->kind == EMBI_V_KEYWORD) {
        return cv->kw == EMBI_KW_paint || cv->kw == EMBI_KW_strict || cv->kw == EMBI_KW_content;
    }
    if (cv->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < cv->list.n; i++) {
            const embi_value *k = &cv->list.items[i];
            if (k->kind == EMBI_V_KEYWORD &&
                (k->kw == EMBI_KW_paint || k->kw == EMBI_KW_strict || k->kw == EMBI_KW_content)) {
                return true;
            }
        }
    }
    return false;
}

/* overflow-clip-margin of style s in px (0 when none). The value is the list [visual box, length]. */
static float clip_margin(const embi_pst *p, const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_overflow_clip_margin);
    if (v == nullptr || v->kind != EMBI_V_LIST || v->list.n < 2) {
        return 0.0f;
    }
    bool ok = false;
    float m = embi_pb_len(p, &v->list.items[1], 0.0f, embi_pb_fontsize(s), &ok);
    return ok && m > 0.0f ? m : 0.0f;
}

static const float *snap_rect(const embi_pst *p, uint32_t xf, const float r[4], float out[4]);

/* The clip of a box's padding edge (overflow clips the content to it, with the inner radii). An axis with overflow:
 * clip grows the clip by overflow-clip-margin (the inner radii grow with it). */
static uint32_t box_clip(embi_pst *p, const embi_frag *f, const float bb[4], uint32_t xf, uint32_t parent)
{
    float pb[4];
    embi_pb_boxes(f, bb, pb, nullptr);
    const embi_style *s = fstyle(f);
    float outer[4] = {0, 0, 0, 0}, inner[4] = {0, 0, 0, 0};
    if (s != nullptr) {
        embi_pb_radii(p, s, bb[2], bb[3], embi_pb_fontsize(s), outer);
        float bdr[4] = {embi_frag_bdr(f, 0), embi_frag_bdr(f, 1), embi_frag_bdr(f, 2), embi_frag_bdr(f, 3)};
        embi_pb_inner_radii(outer, bdr, inner);
        bool mx = embi_pb_kw(s, EMBI_PROP_overflow_x) == EMBI_KW_clip;
        bool my = embi_pb_kw(s, EMBI_PROP_overflow_y) == EMBI_KW_clip;
        bool vx0 = embi_pb_kw(s, EMBI_PROP_overflow_x) == EMBI_KW_visible;
        bool vy0 = embi_pb_kw(s, EMBI_PROP_overflow_y) == EMBI_KW_visible;
        if (vx0 && vy0 && contains_paint(s)) {
            mx = my = true; /* contain: paint clips like overflow: clip, and overflow-clip-margin applies to it */
        }
        /* The visual box the margin starts from (CSS Overflow 4 section 3.3): padding-box by default. */
        const embi_value *cmv = (mx || my) ? embi_style_value(s, EMBI_PROP_overflow_clip_margin) : nullptr;
        uint16_t vbox = cmv != nullptr && cmv->kind == EMBI_V_LIST && cmv->list.n >= 1 &&
                                cmv->list.items[0].kind == EMBI_V_KEYWORD
                            ? cmv->list.items[0].kw
                            : EMBI_KW_padding_box;
        if (vbox == EMBI_KW_border_box || vbox == EMBI_KW_content_box) {
            float e[4];
            for (int i = 0; i < 4; i++) {
                e[i] = vbox == EMBI_KW_border_box ? bdr[i] : -embi_frag_pad(f, i);
            }
            if (mx) {
                pb[0] -= e[0];
                pb[2] += e[0] + e[2];
            }
            if (my) {
                pb[1] -= e[1];
                pb[3] += e[1] + e[3];
            }
            if (vbox == EMBI_KW_border_box) {
                memcpy(inner, outer, sizeof inner);
            } else {
                float pads[4] = {embi_frag_pad(f, 0), embi_frag_pad(f, 1), embi_frag_pad(f, 2), embi_frag_pad(f, 3)};
                float pin[4];
                embi_pb_inner_radii(inner, pads, pin);
                memcpy(inner, pin, sizeof inner);
            }
        }
        float m = (mx || my) ? clip_margin(p, s) : 0.0f;
        if (m > 0.0f) {
            if (mx) {
                pb[0] -= m;
                pb[2] += 2.0f * m;
            }
            if (my) {
                pb[1] -= m;
                pb[3] += 2.0f * m;
            }
            for (int i = 0; i < 4; i++) {
                if (inner[i] > 0.0f) {
                    inner[i] += m;
                }
            }
        }
        /* overflow: clip in one axis with visible in the other clips that axis only (CSS Overflow 3 section 3.1): the
         * visible axis is unbounded (clip-002 to clip-005). */
        bool vx = vx0 && !vy0, vy = vy0 && !vx0; /* both visible is contain: paint, which clips both axes */
        if ((mx && vy) || (my && vx)) {
            const float big = 1.0e7f;
            if (vx) {
                pb[0] -= big;
                pb[2] += 2.0f * big;
            }
            if (vy) {
                pb[1] -= big;
                pb[3] += 2.0f * big;
            }
            for (int i = 0; i < 4; i++) {
                inner[i] = 0.0f;
            }
        }
    }
    return embi_pb_clip(p, pb, inner, xf, parent);
}

/* The origin, transform and clip of the content of box k (k's scroll offset moves its children; overflow, and contain: paint,
 * clip them). For a line or text fragment the content is the fragment itself. */
static void enter(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf, uint32_t clip, float *cx,
                  float *cy, uint32_t *cclip, uint32_t *cxf)
{
    *cx = kx;
    *cy = ky;
    *cclip = clip;
    *cxf = xf;
    if (k->kind != EMBI_FK_BOX) {
        return;
    }
    /* The scroll offset is a translation of the content (scroll60): a pure scroll moves it without building the list again. */
    uint32_t sxf = embi_pb_scroll_enter(p, k, kx, ky, xf);
    if (sxf != xf) {
        *cxf = sxf;
    } else {
        *cx -= embi_frag_sx(k);
        *cy -= embi_frag_sy(k);
    }
    const embi_style *s = fstyle(k);
    if ((k->flags & EMBI_FF_CLIP) != 0 || (s != nullptr && contains_paint(s))) {
        float bb[4] = {kx, ky, k->w, k->h};
        /* A table's overflow clips at the table box, which lies between its captions. */
        float cap_t = 0.0f, cap_b = 0.0f;
        embi_frag_caps(k, &cap_t, &cap_b);
        if (cap_t > 0.0f || cap_b > 0.0f) {
            bb[1] += cap_t;
            bb[3] = fmaxf(0.0f, bb[3] - cap_t - cap_b);
        }
        /* The clip follows the box's snapped edges (self_paint), so the content is cut where the box's background
         * ends rather than blended over it at a fractional edge. */
        float sbb[4];
        *cclip = box_clip(p, k, snap_rect(p, xf, bb, sbb), xf, clip);
    }
}

/* The clip of an absolutely positioned box with the legacy clip: rect(top, right, bottom, left) (CSS 2.1 11.1.2), in
 * the box's border box; auto is the edge. It clips the box's own painting and its content. Returns clip when there is
 * none. */
static uint32_t abs_clip(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf, uint32_t clip)
{
    if (k->kind != EMBI_FK_BOX || k->box == nullptr || (k->box->flags & EMBI_BF_ABSPOS) == 0) {
        return clip;
    }
    const embi_style *s = k->box->style;
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_clip) : nullptr;
    if (v == nullptr || v->kind != EMBI_V_LIST || v->list.n != 4) {
        return clip;
    }
    float fs = embi_pb_fontsize(s);
    const embi_value *it = v->list.items; /* top, right, bottom, left */
    float edge[4] = {0.0f, k->w, k->h, 0.0f};
    float side[4];
    for (int i = 0; i < 4; i++) {
        side[i] = edge[i];
        if (it[i].kind != EMBI_V_KEYWORD) {
            bool ok = false;
            float ref = (i & 1) != 0 ? k->w : k->h;
            float px = embi_pb_len(p, &it[i], ref, fs, &ok);
            if (ok) {
                side[i] = px;
            }
        }
    }
    float w = side[1] - side[3], h = side[2] - side[0];
    float rect[4] = {kx + side[3], ky + side[0], w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
    float zero[4] = {0, 0, 0, 0};
    return embi_pb_clip(p, rect, zero, xf, clip);
}

/* The own painting of a box: background, borders, replaced content and outline, in that order. bb is its
 * border box in page px. */
/* A table cell of a border-collapse: collapse table: its own borders are not painted, the table's collapsed grid
 * paints the winning edges instead (CSS 2.1 17.6.2). The cell's row groups and rows lie between it and the table. */
static bool in_collapsed_cell(const embi_frag *k)
{
    if (k->box->tr != EMBI_TR_CELL) {
        return false;
    }
    const embi_frag *t = k->parent;
    while (t != nullptr && t->kind == EMBI_FK_BOX && t->box != nullptr &&
           (t->box->tr == EMBI_TR_ROW || t->box->tr == EMBI_TR_ROWGROUP || t->box->tr == EMBI_TR_THEAD ||
            t->box->tr == EMBI_TR_TFOOT)) {
        t = t->parent;
    }
    return t != nullptr && embi_frag_coll(t) != nullptr;
}

/* The rect snapped to device pixels (Blink's pixel snapping: each edge rounds, the size is the difference), so a
 * fractional edge is not blended with what lies under it; not under a transform, whose origin is not on the grid.
 * Returns r or out. */
static const float *snap_rect(const embi_pst *p, uint32_t xf, const float r[4], float out[4])
{
    if (p->cfg == nullptr) {
        return r;
    }
    float sc = p->cfg->scale > 0.0f ? p->cfg->scale : 1.0f;
    float tx = 0.0f, ty = 0.0f;
    if (xf != 0) {
        /* Under a translation the edges snap where they land on the device grid (a fractional translation, such as a
         * scroll container at a fractional position, would otherwise blend every edge); any other transform keeps
         * the rect as it is. */
        const float *m = p->d->xforms.data[xf].m;
        if (fabsf(m[0] - 1.0f) > 1e-5f || fabsf(m[1]) > 1e-5f || fabsf(m[2]) > 1e-5f || fabsf(m[3] - 1.0f) > 1e-5f) {
            return r;
        }
        tx = m[4];
        ty = m[5];
    }
    float x0 = roundf((r[0] + tx) * sc) / sc - tx, y0 = roundf((r[1] + ty) * sc) / sc - ty;
    out[0] = x0;
    out[1] = y0;
    out[2] = roundf((r[0] + r[2] + tx) * sc) / sc - tx - x0;
    out[3] = roundf((r[1] + r[3] + ty) * sc) / sc - ty - y0;
    return out;
}

/* The cells under a row group, row or table fragment: appends their border boxes (page px, origin ox, oy of the parent's
 * border box) to out (nullptr only counts); returns the running count. */
static uint32_t cell_rects(const embi_frag *f, float ox, float oy, float (*out)[4], uint32_t n)
{
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
            continue;
        }
        uint8_t tr = k->box->tr;
        if (tr == EMBI_TR_CELL) {
            if (out != nullptr) {
                out[n][0] = ox + k->x;
                out[n][1] = oy + k->y;
                out[n][2] = k->w;
                out[n][3] = k->h;
            }
            n++;
        } else if (tr == EMBI_TR_ROW || tr == EMBI_TR_ROWGROUP || tr == EMBI_TR_THEAD || tr == EMBI_TR_TFOOT) {
            n = cell_rects(k, ox + k->x, oy + k->y, out, n);
        }
    }
    return n;
}

/* All the cell rectangles under f in a fresh array (free with embi_free(r, n * sizeof *r)); nullptr when there are none. */
static float (*cell_rects_all(const embi_frag *f, float ox, float oy, uint32_t *count))[4]
{
    uint32_t n = cell_rects(f, ox, oy, nullptr, 0);
    *count = 0;
    if (n == 0) {
        return nullptr;
    }
    float (*r)[4] = embi_alloc((size_t)n * sizeof *r);
    if (r == nullptr) {
        return nullptr;
    }
    *count = cell_rects(f, ox, oy, r, 0);
    return r;
}

/* A row or row group of a separated-borders table paints its background only behind its cells, not in the spacing
 * between them, with the whole box as the positioning area (CSS 2.1 17.5.1 as browsers draw it; WPT
 * table-backgrounds-bs-row-001, -rowgroup-001). True when it painted. */
static bool paint_track_bg_per_cell(embi_pst *p, const embi_frag *k, const embi_style *s, const float bb[4], uint32_t xf,
                                    uint32_t clip)
{
    uint8_t tr = k->box->tr;
    if (tr != EMBI_TR_ROW && tr != EMBI_TR_ROWGROUP && tr != EMBI_TR_THEAD && tr != EMBI_TR_TFOOT) {
        return false;
    }
    const embi_frag *t = k->parent;
    while (t != nullptr && t->kind == EMBI_FK_BOX && t->box != nullptr &&
           (t->box->tr == EMBI_TR_ROW || t->box->tr == EMBI_TR_ROWGROUP || t->box->tr == EMBI_TR_THEAD ||
            t->box->tr == EMBI_TR_TFOOT)) {
        t = t->parent;
    }
    if (t == nullptr || (t->flags & EMBI_FF_TABLE) == 0 || embi_frag_coll(t) != nullptr) {
        return false;
    }
    uint32_t n;
    float (*r)[4] = cell_rects_all(k, bb[0], bb[1], &n);
    if (r == nullptr) {
        return false;
    }
    float covered = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        covered += r[i][2] * r[i][3];
    }
    if (covered >= k->w * k->h - 0.5f) {
        embi_free(r, (size_t)n * sizeof *r);
        return false; /* no gaps: the cells tile the box */
    }
    float zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        float sn[4];
        const float *c = snap_rect(p, xf, r[i], sn);
        uint32_t cc = embi_pb_clip(p, c, zero, xf, clip);
        embi_pb_background(p, k, s, bb, xf, cc);
    }
    embi_free(r, (size_t)n * sizeof *r);
    return true;
}

static void self_paint(embi_pst *p, const embi_frag *k, const float bb[4], uint32_t xf, uint32_t clip)
{
    if (k->kind != EMBI_FK_BOX || k->box == nullptr || k->box->style == nullptr) {
        return;
    }
    p->d->cur_scroll = embi_pb_xform_scroll(p->d, xf); /* transforms made while painting the box (an inline SVG) */
    const embi_style *s = k->box->style;
    /* An anonymous box shares its parent's style but its own non-inherited properties are the initial values: it
     * paints no background, border or outline (the parent's would be painted a second time, on top of floats). */
    if ((k->box->flags & EMBI_BF_ANON) != 0) {
        return;
    }
    if (embi_pb_hidden(s)) {
        return;
    }
    /* The box's edges snap to device pixels (Blink's pixel snapping: each edge rounds, the size is the difference), so a
     * fractional edge is not blended with what lies under it; not under a transform, whose origin is not on the grid. */
    float sbb[4], raw[4] = {bb[0], bb[1], bb[2], bb[3]};
    bb = snap_rect(p, xf, bb, sbb);
    /* A table's background and borders are those of the table box, which lies between its captions. */
    float tbb[4] = {bb[0], bb[1], bb[2], bb[3]};
    float cap_t = 0.0f, cap_b = 0.0f;
    embi_frag_caps(k, &cap_t, &cap_b);
    if (cap_t > 0.0f || cap_b > 0.0f) {
        tbb[1] += cap_t;
        tbb[3] -= cap_t + cap_b;
        if (!(tbb[3] > 0.0f)) {
            tbb[3] = 0.0f;
        }
        bb = tbb;
    }
    /* An empty table cell with empty-cells: hide (separate borders) paints no background or border (W3g flag). */
    bool hide_empty = (k->flags & EMBI_FF_HIDE_EMPTY) != 0;
    /* The body's background moved to the canvas (embi_paint_document): it paints nothing itself. */
    if (!hide_empty) {
        if (k != p->html) {
            embi_pb_backdrop(p, k, s, bb, xf, clip);
        }
        embi_pb_shadows(p, k, s, bb, false, xf, clip);
    }
    bool ctl_box = (k->box->flags2 & (EMBI_BF2_CONTROL | EMBI_BF2_CHROME)) != 0 && k->box->el != nullptr; /* W5e */
    if (ctl_box) {
        embi_pb_control_pre(p, k, k->box, s, bb, xf, clip);
    }
    if (!hide_empty && (p->bg_skip == nullptr || k->box->el != p->bg_skip)) {
        p->bg_raw = cap_t > 0.0f || cap_b > 0.0f ? nullptr : raw;
        if (!paint_track_bg_per_cell(p, k, s, bb, xf, clip)) {
            embi_pb_background(p, k, s, bb, xf, clip);
        }
        p->bg_raw = nullptr;
    }
    if (!hide_empty) {
        embi_pb_shadows(p, k, s, bb, true, xf, clip);
    }
    /* a border image replaces the border styles (and paints nothing when its source is none or has not loaded) */
    if (!hide_empty && !in_collapsed_cell(k) && !embi_pb_border_image(p, k, s, bb, xf, clip)) {
        embi_pb_border(p, k, s, bb, xf, clip);
    }
    embi_pb_colrules(p, k, s, bb, xf, clip);
    if (k->box->kind == EMBI_BK_REPLACED) {
        embi_pb_replaced(p, k, k->box, s, bb, xf, clip);
    }
    if (ctl_box) {
        embi_pb_control(p, k, k->box, s, bb, xf, clip);
    }
    embi_pb_outline(p, k, s, bb, xf, clip);
}

static void walk(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip, int ph);
static void paint_ctx(embi_pst *p, const embi_frag *f, float kx, float ky, uint32_t xf_parent, uint32_t clip_parent,
                      uint32_t parent_ctx);

/* True when a row group, row or cell of a collapsed table is positioned: those paint in step 6, above the table's
 * background phase, so the collapsed borders must come after them (WPT collapsed-border-positioned-tr-td.html). */
static bool coll_has_positioned(const embi_frag *t)
{
    for (const embi_frag *g = embi_frag_first(t); g != nullptr; g = g->next) {
        if (g->kind != EMBI_FK_BOX || g->box == nullptr) {
            continue;
        }
        if (positioned(g)) {
            return true;
        }
        for (const embi_frag *r = embi_frag_first(g); r != nullptr; r = r->next) {
            if (r->kind != EMBI_FK_BOX || r->box == nullptr) {
                continue;
            }
            if (positioned(r)) {
                return true;
            }
            for (const embi_frag *cl = embi_frag_first(r); cl != nullptr; cl = cl->next) {
                if (cl->kind == EMBI_FK_BOX && cl->box != nullptr && positioned(cl)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* An atomic or floated or positioned box painted as a pseudo stacking context: its own painting and then its
 * whole subtree in the flow phases. Its positioned descendants are not painted here (see walk, PH_POS). */
static void paint_pseudo(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf, uint32_t clip)
{
    clip = abs_clip(p, k, kx, ky, xf, clip);
    float bb[4] = {kx, ky, k->w, k->h};
    self_paint(p, k, bb, xf, clip);
    float cx, cy;
    uint32_t cclip, cxf;
    enter(p, k, kx, ky, xf, clip, &cx, &cy, &cclip, &cxf);
    walk(p, k, cx, cy, cxf, cclip, PH_BLOCK);
    if (embi_frag_coll(k) != nullptr && !coll_has_positioned(k)) {
        embi_pb_collapsed(p, k, kx, ky, xf, clip); /* after the cells' backgrounds, below their content (CSS 2.1 17.6.2) */
    }
    walk(p, k, cx, cy, cxf, cclip, PH_FLOAT);
    walk(p, k, cx, cy, cxf, cclip, PH_INLINE);
}

static void paint_line_items(embi_pst *p, const embi_frag *line, float lx, float ly, uint32_t xf, uint32_t clip,
                             int ph);

/* The children of a flex or grid container are painted in the order of their `order` property, then tree order
 * (CSS Flexbox 9 and Grid 8.5: order-modified document order). Other containers keep the fragment list. */
static bool is_ordered(const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && f->box != nullptr &&
           (f->box->kind == EMBI_BK_FLEX || f->box->kind == EMBI_BK_GRID);
}

static int64_t order_of(const embi_frag *k)
{
    /* An absolutely positioned child is not a flex or grid item: order does not apply to it
     * (flexbox-paint-ordering-003). */
    if (k->kind != EMBI_FK_BOX || k->box == nullptr || k->box->style == nullptr ||
        (k->box->flags & EMBI_BF_ABSPOS) != 0) {
        return 0;
    }
    const embi_value *v = embi_style_value(k->box->style, EMBI_PROP_order);
    if (v == nullptr || (v->kind != EMBI_V_INTEGER && v->kind != EMBI_V_NUMBER)) {
        return 0;
    }
    double d = v->num;
    if (!(d > -4.0e15)) {
        return -4000000000000000LL;
    }
    if (d > 4.0e15) {
        return 4000000000000000LL;
    }
    return (int64_t)d;
}

static int cmp_ordk(const void *a, const void *b)
{
    const embi_ordk *x = a, *y = b;
    if (x->order != y->order) {
        return x->order < y->order ? -1 : 1;
    }
    if (x->seq != y->seq) {
        return x->seq < y->seq ? -1 : 1;
    }
    return 0;
}

/* Pushes the children of the flex or grid container f onto p->ord in paint order, and returns the index of the first
 * (the caller reads up to p->ord.len, then truncates p->ord to that index). */
static uint32_t push_ordered(embi_pst *p, const embi_frag *f)
{
    uint32_t mark = p->oks.len;
    uint32_t seq = 0;
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        embi_ordk e = {order_of(k), seq++, k};
        embi_vec_push(&p->oks, e);
    }
    uint32_t n = p->oks.len - mark;
    if (n > 1) {
        qsort(&p->oks.data[mark], n, sizeof(embi_ordk), cmp_ordk);
    }
    uint32_t start = p->ord.len;
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&p->ord, p->oks.data[mark + i].f);
    }
    p->oks.len = mark;
    return start;
}

/* ------------------------------------------------------------------ step 6 units

Step 6 paints the positioned boxes and the z-index 0 contexts of a context in tree order (CSS 2.1 Appendix E). Their
fragments are not always in tree order (an abspos fragment follows its siblings whatever its DOM parent), and a flow
box between two of them can hold units of its own. So the units of a scope are gathered through its flow boxes first,
then sorted by their DOM position, and painted. Flex and grid scopes keep their order-modified sequence. */

static uint32_t sib_index(embi_pst *p, const embi_node *n)
{
    uint32_t i = 0;
    const embi_node *q = n->prev;
    for (; q != nullptr && i < 24; q = q->prev) {
        i++;
    }
    if (q == nullptr) {
        return i;
    }
    /* A wide parent: index all its children once, so N siblings cost O(N) rather than O(N^2). */
    uint64_t v;
    if (n->parent != nullptr) {
        uint64_t par = (uint64_t)(uintptr_t)n->parent;
        if (!embi_hashmap_get(&p->sibs_done, par, &v)) {
            uint64_t idx = 0;
            bool ok = true;
            for (const embi_node *c = n->parent->first_child; c != nullptr && ok; c = c->next) {
                ok = embi_hashmap_put(&p->sibs, (uint64_t)(uintptr_t)c, idx++);
            }
            if (ok) {
                embi_hashmap_put(&p->sibs_done, par, 1);
            }
        }
        if (embi_hashmap_get(&p->sibs, (uint64_t)(uintptr_t)n, &v)) {
            return (uint32_t)v;
        }
    }
    for (; q != nullptr; q = q->prev) {
        i++;
    }
    return i;
}

/* The element node of a unit, or nullptr. */
static const embi_node *unit_node(const embi_pent *e)
{
    return e->f->box != nullptr && e->f->box->el != nullptr ? &e->f->box->el->base : nullptr;
}

/* Whether the DOM node n is an element whose box is a flex or grid container. */
static bool node_ordered(const embi_node *n)
{
    if (n == nullptr || n->type != EMBI_NODE_ELEMENT) {
        return false;
    }
    const embi_box *b = ((const embi_element *)n)->box;
    return b != nullptr && (b->kind == EMBI_BK_FLEX || b->kind == EMBI_BK_GRID);
}

static uint64_t style_order_key(const embi_style *st)
{
    int64_t o = 0;
    if (st != nullptr) {
        const embi_value *v = embi_style_value(st, EMBI_PROP_order);
        if (v != nullptr && (v->kind == EMBI_V_INTEGER || v->kind == EMBI_V_NUMBER)) {
            double d = v->num;
            o = !(d > -4.0e15) ? -4000000000000000LL : d > 4.0e15 ? 4000000000000000LL : (int64_t)d;
        }
    }
    return (uint64_t)(o + 4000000000000000LL);
}

static uint64_t node_order_key(const embi_node *n)
{
    if (n->type == EMBI_NODE_ELEMENT) {
        const embi_box *b = ((const embi_element *)n)->box;
        if (b != nullptr && (b->flags & EMBI_BF_ABSPOS) == 0) {
            return style_order_key(b->style);
        }
    }
    return style_order_key(nullptr);
}

/* Appends the document-order key of node n to p->keys and returns its length in words (2 per tree level, root-most
 * level first). A level is the pair (order key, sibling index + 1): the order key is 0 unless the parent is a flex or
 * grid container, where it is the child's order (order-modified document order, CSS Flexbox 9.1 and Grid 8.5). Comparing
 * keys word by word gives that order; an ancestor's key is a prefix of its descendants' keys, so it sorts first.
 * A generated box (pb non-null, its pe set) is keyed as its originating element plus one final pair that sorts before
 * every real child (::before, ::marker) or after every one (::after); the pair carries the pseudo's own order when the
 * originating element is a flex or grid container, where the pseudo is an item. */
static uint32_t build_key(embi_pst *p, const embi_node *n, const embi_box *pb)
{
    uint32_t start = p->keys.len;
    if (pb != nullptr && pb->pe != 0) {
        bool after = pb->pe == EMBI_PE_AFTER;
        uint64_t o = node_ordered(n) ? style_order_key(pb->style) : 0;
        embi_vec_push(&p->keys, o);
        embi_vec_push(&p->keys, after ? UINT64_MAX : 0);
    }
    uint32_t base = p->keys.len;
    for (const embi_node *q = n; q->parent != nullptr; q = q->parent) {
        embi_vec_push(&p->keys, node_ordered(q->parent) ? node_order_key(q) : 0);
        embi_vec_push(&p->keys, (uint64_t)sib_index(p, q) + 1);
    }
    /* reverse the ancestor pairs; the pseudo pair (if any) stays last, so rotate it after */
    uint32_t len = p->keys.len - base;
    for (uint32_t i = 0, j = len / 2; i + 1 < j; i++, j--) {
        uint64_t *a = &p->keys.data[base + 2 * i], *b = &p->keys.data[base + 2 * (j - 1)];
        uint64_t t0 = a[0], t1 = a[1];
        a[0] = b[0];
        a[1] = b[1];
        b[0] = t0;
        b[1] = t1;
    }
    if (base != start) {
        uint64_t t0 = p->keys.data[start], t1 = p->keys.data[start + 1];
        for (uint32_t i = 0; i < len; i++) {
            p->keys.data[start + i] = p->keys.data[base + i];
        }
        p->keys.data[start + len] = t0;
        p->keys.data[start + len + 1] = t1;
        return len + 2;
    }
    return len;
}

/* Gives the n entries of v their keys (pushed on p->keys above its length on entry; the caller truncates them) when
 * every one has an element; otherwise none gets one and the sort keeps the gathered order. */
static void key_entries(embi_pst *p, embi_pent *v, uint32_t n, bool (*node_of)(const embi_pent *, const embi_node **))
{
    bool all = true;
    for (uint32_t i = 0; i < n && all; i++) {
        const embi_node *nd = nullptr;
        all = node_of(&v[i], &nd);
    }
    uint32_t base = p->keys.len;
    for (uint32_t i = 0; i < n; i++) {
        const embi_node *nd = nullptr;
        v[i].klen = 0;
        v[i].koff = base;
        if (all && node_of(&v[i], &nd)) {
            v[i].koff = p->keys.len;
            v[i].klen = build_key(p, nd, v[i].f->box);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        v[i].kp = (all && p->keys.data != nullptr) ? p->keys.data + v[i].koff : nullptr;
    }
}

static bool unit_node_of(const embi_pent *e, const embi_node **out)
{
    *out = unit_node(e);
    return *out != nullptr;
}

/* Document order of two entries by key, then by gathered position. */
static int cmp_key(const embi_pent *x, const embi_pent *y)
{
    if (x->kp != nullptr && y->kp != nullptr) {
        uint32_t n = x->klen < y->klen ? x->klen : y->klen;
        for (uint32_t i = 0; i < n; i++) {
            if (x->kp[i] != y->kp[i]) {
                return x->kp[i] < y->kp[i] ? -1 : 1;
            }
        }
        if (x->klen != y->klen) {
            return x->klen < y->klen ? -1 : 1;
        }
    }
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int cmp_unit(const void *a, const void *b)
{
    return cmp_key(a, b);
}

static void push_unit(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf, uint32_t clip)
{
    embi_pent e;
    e.f = k;
    e.ox = kx;
    e.oy = ky;
    e.xform = xf;
    e.clip = clip;
    e.z = 0;
    e.seq = p->units.len;
    e.kp = nullptr;
    e.klen = 0;
    e.koff = 0;
    embi_vec_push(&p->units, e);
}

static void gather_child(embi_pst *p, const embi_frag *k, float ox, float oy, uint32_t xf, uint32_t clip);

/* The units of the children of f (origin ox, oy), in fragment order (order-modified for flex and grid). */
static void gather_scope(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip)
{
    bool ord = is_ordered(f);
    uint32_t start = ord ? push_ordered(p, f) : 0;
    uint32_t end = ord ? p->ord.len : 0;
    const embi_frag *k = ord ? nullptr : embi_frag_first(f);
    for (uint32_t i = start;;) {
        if (ord) {
            if (i >= end) {
                break;
            }
            k = p->ord.data[i++];
        } else {
            if (k == nullptr) {
                break;
            }
        }
        gather_child(p, k, ox, oy, xf, clip);
        if (!ord) {
            k = k->next;
        }
    }
    if (ord) {
        p->ord.len = start;
    }
}

/* A flow box (not a unit itself): its children belong to the same step 6 scope. */
static void gather_flow(embi_pst *p, const embi_frag *k, float kx, float ky, uint32_t xf, uint32_t clip)
{
    uint32_t kclip = abs_clip(p, k, kx, ky, xf, clip);
    float cx, cy;
    uint32_t cclip, cxf;
    enter(p, k, kx, ky, xf, kclip, &cx, &cy, &cclip, &cxf);
    gather_scope(p, k, cx, cy, cxf, cclip);
}

/* An inline box piece that is a stacking context without being positioned (opacity, isolation, ...): its text and
 * nested boxes are siblings of the piece in the line, so the piece and those are painted together in the line's inline
 * phase (paint_line_items, paint_inline_group), as one group. */
static bool inline_group(const embi_pst *p, const embi_frag *c)
{
    if (!(c->kind == EMBI_FK_BOX && c->inl != 0 && c->box != nullptr && is_stacking(p, c) && !positioned(c) &&
          embi_frag_z_index(c) == 0)) {
        return false;
    }
    /* Only an opacity group: a piece with a filter, a clip-path, a mask, a blend mode or an isolation (R3a, R3b) is painted
     * as its own context (paint_ctx), which builds the filtered or composite group. */
    const embi_style *s = fstyle(c);
    const embi_value *fv = s != nullptr ? embi_style_value(s, EMBI_PROP_filter) : nullptr;
    if (fv != nullptr && fv->kind == EMBI_V_FILTERS && fv->filters.n > 0) {
        return false;
    }
    embi_pcomp pc;
    embi_pb_comp_scan(s, &pc);
    return !embi_pb_comp_group(&pc) && !pc.backdrop_root;
}

/* The children of a line for step 6: the z-index 0 contexts, the positioned boxes, and the flow inline boxes. */
static void gather_line(embi_pst *p, const embi_frag *line, float lx, float ly, uint32_t xf, uint32_t clip)
{
    for (const embi_frag *c = embi_frag_first(line); c != nullptr; c = c->next) {
        if (svg_fo(c)) {
            continue;
        }
        if (is_stacking(p, c)) {
            if (embi_frag_z_index(c) == 0 && !inline_group(p, c)) {
                float kx, ky;
                child_origin(p, c, lx, ly, &kx, &ky);
                push_unit(p, c, kx, ky, xf, clip);
            }
            continue;
        }
        if (c->kind != EMBI_FK_BOX || c->box == nullptr) {
            continue;
        }
        float kx, ky;
        child_origin(p, c, lx, ly, &kx, &ky);
        if (positioned(c)) {
            push_unit(p, c, kx, ky, xf, clip);
        } else {
            gather_flow(p, c, kx, ky, xf, clip);
        }
    }
}

static void gather_child(embi_pst *p, const embi_frag *k, float ox, float oy, uint32_t xf, uint32_t clip)
{
    if (svg_fo(k)) {
        return;
    }
    float kx, ky;
    child_origin(p, k, ox, oy, &kx, &ky);
    if (is_stacking(p, k)) {
        if (embi_frag_z_index(k) == 0) {
            push_unit(p, k, kx, ky, xf, clip);
        }
        return;
    }
    if (k->kind == EMBI_FK_LINE) {
        gather_line(p, k, kx, ky, xf, clip);
        return;
    }
    if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
        return;
    }
    if (positioned(k) || embi_frag_coll(k) != nullptr) {
        push_unit(p, k, kx, ky, xf, clip);
        return;
    }
    gather_flow(p, k, kx, ky, xf, clip);
}

/* Paints one unit of step 6: a z-index 0 context, or a positioned or table box with its own scope. */
static void paint_unit(embi_pst *p, embi_pent u)
{
    const embi_frag *k = u.f;
    if (is_stacking(p, k)) {
        paint_ctx(p, k, u.ox, u.oy, u.xform, u.clip, p->cur_ctx);
        return;
    }
    uint32_t kclip = abs_clip(p, k, u.ox, u.oy, u.xform, u.clip);
    if (positioned(k)) {
        paint_pseudo(p, k, u.ox, u.oy, u.xform, u.clip);
    }
    float cx, cy;
    uint32_t cclip, cxf;
    enter(p, k, u.ox, u.oy, u.xform, kclip, &cx, &cy, &cclip, &cxf);
    walk(p, k, cx, cy, cxf, cclip, PH_POS);
    /* The collapsed borders of a table are painted in its background phase (walk_child, paint_pseudo), except when a
     * row or cell is positioned: they go over those, after everything of the table's flow. */
    if (embi_frag_coll(k) != nullptr && coll_has_positioned(k)) {
        embi_pb_collapsed(p, k, u.ox, u.oy, u.xform, u.clip);
    }
}

/* Step 6 of the context whose content is f: gathers its units, sorts them into document order (order-modified under
 * flex and grid containers) and paints them. The units of nested scopes are pushed above these and popped before the
 * next. */
static void walk_pos(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip)
{
    uint32_t start = p->units.len;
    uint32_t kmark = p->keys.len;
    gather_scope(p, f, ox, oy, xf, clip);
    uint32_t end = p->units.len;
    if (end - start > 1) {
        key_entries(p, p->units.data + start, end - start, unit_node_of);
        qsort(p->units.data + start, end - start, sizeof(embi_pent), cmp_unit);
    }
    for (uint32_t i = start; i < end; i++) {
        paint_unit(p, p->units.data[i]);
    }
    p->units.len = start;
    p->keys.len = kmark;
}

static void walk_child(embi_pst *p, const embi_frag *k, float ox, float oy, uint32_t xf, uint32_t clip, int ph)
{
    if (is_stacking(p, k)) {
        /* Contexts with a z-index are painted from the lists of their context; the z-index 0 ones are step 6 units
         * (see walk_pos). */
        return;
    }
    float kx, ky;
    child_origin(p, k, ox, oy, &kx, &ky);
    if (k->kind == EMBI_FK_LINE) {
        paint_line_items(p, k, kx, ky, xf, clip, ph);
        return;
    }
    if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
        return;
    }
    bool pos = positioned(k);
    bool flt = floats(k);
    if (pos) {
        return; /* step 6 */
    }
    if (ph == PH_BLOCK) {
        if (flt) {
            return;
        }
        float bb[4] = {kx, ky, k->w, k->h};
        self_paint(p, k, bb, xf, clip);
        float cx, cy;
        uint32_t cclip, cxf;
        enter(p, k, kx, ky, xf, clip, &cx, &cy, &cclip, &cxf);
        walk(p, k, cx, cy, cxf, cclip, PH_BLOCK);
        if (embi_frag_coll(k) != nullptr && !coll_has_positioned(k)) {
            embi_pb_collapsed(p, k, kx, ky, xf, clip); /* after the cells' backgrounds, below their content (CSS 2.1 17.6.2) */
        }
    } else if (ph == PH_FLOAT) {
        if (flt) {
            paint_pseudo(p, k, kx, ky, xf, p->clamp_clip != 0 ? p->clamp_clip : clip);
        } else {
            float cx, cy;
            uint32_t cclip, cxf;
            enter(p, k, kx, ky, xf, clip, &cx, &cy, &cclip, &cxf);
            walk(p, k, cx, cy, cxf, cclip, PH_FLOAT);
        }
    } else if (ph == PH_INLINE) {
        if (flt) {
            return;
        }
        float cx, cy;
        uint32_t cclip, cxf;
        enter(p, k, kx, ky, xf, clip, &cx, &cy, &cclip, &cxf);
        walk(p, k, cx, cy, cxf, cclip, PH_INLINE);
    }
}

/* The backgrounds of the column groups and columns of a table fragment, below its row groups and rows (CSS 2.1 17.5.1).
 * Each shows in the row bands only, so the spacing between rows keeps the table's own background. */
static bool has_background(const embi_style *s);

static void paint_cols(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip)
{
    const embi_tcols *t = embi_frag_cols(f);
    if (t == nullptr || t->nrows == 0) {
        return;
    }
    /* The bands are one when the rows touch each other; else every band is clipped out of the whole grid column. */
    bool contiguous = true;
    for (uint32_t r = 0; r + 1 < t->nrows; r++) {
        if (fabsf(t->rows[2 * r] + t->rows[2 * r + 1] - t->rows[2 * r + 2]) > 0.01f) {
            contiguous = false;
        }
    }
    float gy0 = t->rows[0], gy1 = t->rows[2 * (t->nrows - 1)] + t->rows[2 * (t->nrows - 1) + 1];
    /* Cell rects of the whole table, gathered once for all columns (separated borders) on first use. */
    float (*cr)[4] = nullptr;
    uint32_t n = 0;
    bool cr_done = false;
    for (uint32_t i = 0; i < t->n; i++) {
        const embi_tcol *it = &t->items[i];
        const embi_style *s = it->box != nullptr ? it->box->style : nullptr;
        if (s == nullptr || embi_pb_hidden(s) || !(it->w > 0.0f) || !(gy1 > gy0) || !has_background(s)) {
            continue;
        }
        embi_frag tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.box = (embi_box *)(uintptr_t)it->box;
        tmp.kind = EMBI_FK_BOX;
        tmp.x = it->x;
        tmp.y = gy0;
        tmp.w = it->w;
        tmp.h = gy1 - gy0;
        float bb0[4] = {ox + tmp.x, oy + tmp.y, tmp.w, tmp.h};
        float bbs[4];
        const float *bb = snap_rect(p, xf, bb0, bbs);
        if (embi_frag_coll(f) == nullptr) {
            /* Separated borders: a column shows behind its cells only, not in the spacing around them (WPT
             * table-backgrounds-bs-colgroup-001), the box being the positioning area. */
            if (!cr_done) {
                cr = cell_rects_all(f, ox, oy, &n);
                cr_done = true;
            }
            if (cr != nullptr) {
                float covered = 0.0f;
                for (uint32_t q = 0; q < n; q++) {
                    float x0 = cr[q][0] > bb0[0] ? cr[q][0] : bb0[0];
                    float x1 = cr[q][0] + cr[q][2] < bb0[0] + bb0[2] ? cr[q][0] + cr[q][2] : bb0[0] + bb0[2];
                    if (x1 > x0) {
                        covered += (x1 - x0) * cr[q][3];
                    }
                }
                if (covered < bb0[2] * (gy1 - gy0) - 0.5f) {
                    float zero[4] = {0, 0, 0, 0};
                    for (uint32_t q = 0; q < n; q++) {
                        float x0 = cr[q][0] > bb0[0] ? cr[q][0] : bb0[0];
                        float x1 = cr[q][0] + cr[q][2] < bb0[0] + bb0[2] ? cr[q][0] + cr[q][2] : bb0[0] + bb0[2];
                        if (!(x1 > x0)) {
                            continue;
                        }
                        float piece0[4] = {x0, cr[q][1], x1 - x0, cr[q][3]};
                        float pieces[4];
                        const float *piece = snap_rect(p, xf, piece0, pieces);
                        uint32_t pc = embi_pb_clip(p, piece, zero, xf, clip);
                        embi_pb_background(p, &tmp, s, bb, xf, pc);
                    }
                    continue;
                }
                if (!contiguous) {
                    /* The cells cover the whole column box (a rowspan cell spans the spacing between rows): one fill. */
                    embi_pb_background(p, &tmp, s, bb, xf, clip);
                    continue;
                }
            }
        }
        if (contiguous) {
            embi_pb_background(p, &tmp, s, bb, xf, clip);
            continue;
        }
        for (uint32_t r = 0; r < t->nrows; r++) {
            float band0[4] = {bb0[0], oy + t->rows[2 * r], bb0[2], t->rows[2 * r + 1]};
            if (!(band0[3] > 0.0f)) {
                continue;
            }
            float bands[4];
            const float *band = snap_rect(p, xf, band0, bands);
            float zero[4] = {0, 0, 0, 0};
            uint32_t bc = embi_pb_clip(p, band, zero, xf, clip);
            embi_pb_background(p, &tmp, s, bb, xf, bc);
        }
    }
    if (cr != nullptr) {
        embi_free(cr, (size_t)n * sizeof *cr);
    }
}

/* Walks the children of f (its content, origin ox, oy) in one phase. */
static void walk(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip, int ph)
{
    if (ph == PH_POS) {
        walk_pos(p, f, ox, oy, xf, clip);
        return;
    }
    if (ph == PH_BLOCK && (f->flags & EMBI_FF_TABLE) != 0) {
        paint_cols(p, f, ox, oy, xf, clip);
    }
    uint32_t saved_cc = p->clamp_clip;
    if (f->kind == EMBI_FK_BOX && f->box != nullptr && (f->box->tflags & EMBI_TF_CLAMP) != 0 && ph == PH_FLOAT) {
        /* The floats of a line-clamp container are clipped at its block-end content edge; the other directions stay
         * open (CSS Overflow 4 section 5.1). */
        const float big = 1.0e7f;
        float cb = oy + f->h - embi_frag_bdr(f, 3) - embi_frag_pad(f, 3);
        float pbx[4] = {ox - big, oy - big, f->w + 2.0f * big, cb - (oy - big)};
        float zero[4] = {0, 0, 0, 0};
        p->clamp_clip = embi_pb_clip(p, pbx, zero, xf, clip);
    }
    bool ord = is_ordered(f);
    uint32_t start = ord ? push_ordered(p, f) : 0;
    uint32_t end = ord ? p->ord.len : 0;
    const embi_frag *k = ord ? nullptr : embi_frag_first(f);
    for (uint32_t i = start;;) {
        if (ord) {
            if (i >= end) {
                break;
            }
            k = p->ord.data[i++];
        } else {
            if (k == nullptr) {
                break;
            }
        }
        walk_child(p, k, ox, oy, xf, clip, ph);
        if (!ord) {
            k = k->next;
        }
    }
    if (ord) {
        p->ord.len = start;
    }
    p->clamp_clip = saved_cc;
}

/* The children of a line: text runs, inline box pieces and inline-level atoms. */
static void finish_layer(embi_pst *p, uint32_t begin, uint32_t ctx_index, float alpha);

/* The text fragment c of a line (the line's origin lx, ly and baseline). */
static void paint_text_frag(embi_pst *p, const embi_frag *c, float lx, float ly, float baseline, uint32_t xf,
                            uint32_t clip)
{
    float rect[4] = {lx + c->x, ly + c->y, c->w, c->h};
    /* Text raised or lowered by vertical-align sits on its own baseline: the top of its fragment plus the
     * ascent of its content area (the fragment's height is that area's ascent and descent). */
    float tb = baseline;
    float ca = 0.0f, cd = 0.0f;
    if (c->box->lt != nullptr && embi_tshim_content_metrics(c->box->lt, &ca, &cd) && fabsf(c->h - (ca + cd)) < 0.01f) {
        tb = rect[1] + ca;
    }
    if (!embi_pb_marker_shape(p, c, c->box, rect, tb, xf, clip)) {
        embi_pb_text(p, c, c->box, rect, tb, baseline, xf, clip);
    }
}

/* Whether box d is inside box a. */
static bool box_within(const embi_box *d, const embi_box *a)
{
    for (; d != nullptr; d = d->parent) {
        if (d == a) {
            return true;
        }
    }
    return false;
}

/* Paints the inline stacking context piece g (see inline_group) and the siblings after it that belong to its box, as
 * one group: a layer when the box has an opacity below 1. Returns the first sibling that is not part of the group. */
static const embi_frag *paint_inline_group(embi_pst *p, const embi_frag *g, float lx, float ly, float baseline,
                                           uint32_t xf, uint32_t clip)
{
    embi_dl *d = p->d;
    float alpha = embi_pb_opacity(fstyle(g));
    uint32_t begin = d->items.len;
    bool layer = alpha < 1.0f;
    if (layer) {
        embi_ditem open;
        memset(&open, 0, sizeof open);
        open.kind = EMBI_DI_LAYER_BEGIN;
        open.f[0] = alpha;
        embi_vec_push(&d->items, open);
    }
    float kx, ky;
    child_origin(p, g, lx, ly, &kx, &ky);
    float bb[4] = {kx, ky, g->w, g->h};
    self_paint(p, g, bb, xf, clip);
    const embi_frag *c = g->next;
    while (c != nullptr && c->box != nullptr && box_within(c->box, g->box)) {
        if (inline_group(p, c)) {
            c = paint_inline_group(p, c, lx, ly, baseline, xf, clip);
            continue;
        }
        if (c->kind == EMBI_FK_TEXT) {
            paint_text_frag(p, c, lx, ly, baseline, xf, clip);
        } else if (c->kind == EMBI_FK_BOX && !positioned(c) && !is_stacking(p, c)) {
            float cx, cy;
            child_origin(p, c, lx, ly, &cx, &cy);
            if (c->inl) {
                float cbb[4] = {cx, cy, c->w, c->h};
                self_paint(p, c, cbb, xf, clip);
            } else {
                paint_pseudo(p, c, cx, cy, xf, clip);
            }
        }
        c = c->next;
    }
    if (layer) {
        finish_layer(p, begin, p->cur_ctx, alpha);
    }
    return c;
}

static void paint_line_items(embi_pst *p, const embi_frag *line, float lx, float ly, uint32_t xf, uint32_t clip,
                             int ph)
{
    float lbl = embi_frag_bl_first(line);
    float baseline = ly + (isnan(lbl) ? 0.0f : lbl);
    const embi_frag *c = embi_frag_first(line);
    while (c != nullptr) {
        const embi_frag *cur = c;
        c = c->next;
        if (is_stacking(p, cur)) {
            if (ph == PH_INLINE && inline_group(p, cur)) {
                c = paint_inline_group(p, cur, lx, ly, baseline, xf, clip); /* the rest of the line follows it */
            }
            continue;
        }
        if (cur->kind == EMBI_FK_TEXT) {
            if (ph == PH_INLINE && cur->box != nullptr) {
                paint_text_frag(p, cur, lx, ly, baseline, xf, clip);
            }
            continue;
        }
        if (cur->kind != EMBI_FK_BOX || cur->box == nullptr) {
            continue;
        }
        float kx, ky;
        child_origin(p, cur, lx, ly, &kx, &ky);
        bool pos = positioned(cur);
        if (pos) {
            continue;
        }
        if (ph != PH_INLINE) {
            continue;
        }
        if (cur->inl) {
            /* An inline box piece: its own background and borders, no children. */
            float bb[4] = {kx, ky, cur->w, cur->h};
            self_paint(p, cur, bb, xf, clip);
        } else {
            paint_pseudo(p, cur, kx, ky, xf, clip);
        }
    }
}

/* ------------------------------------------------------------------ stacking contexts */

static void collect(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip);

/* One child of a scope for collect (see collect). */
static void collect_child(embi_pst *p, const embi_frag *k, float ox, float oy, uint32_t xf, uint32_t clip)
{
    if (svg_fo(k)) {
        return;
    }
    float kx, ky;
    child_origin(p, k, ox, oy, &kx, &ky);
    if (is_stacking(p, k)) {
        int32_t z = embi_frag_z_index(k);
        if (z != 0) {
            embi_pent e;
            e.f = k;
            e.ox = kx;
            e.oy = ky;
            e.xform = xf;
            e.clip = clip;
            e.z = z;
            e.seq = p->pend.len;
            e.kp = nullptr;
            e.klen = 0;
            e.koff = 0;
            embi_vec_push(&p->pend, e);
        }
        return;
    }
    if (k->kind == EMBI_FK_TEXT || k->kind == EMBI_FK_BOX || k->kind == EMBI_FK_LINE) {
        float cx, cy;
        uint32_t cclip, cxf;
        uint32_t kclip = abs_clip(p, k, kx, ky, xf, clip);
        enter(p, k, kx, ky, xf, kclip, &cx, &cy, &cclip, &cxf);
        collect(p, k, cx, cy, cxf, cclip);
    }
}

/* Collects the child stacking contexts of f's scope with a z-index other than 0 (they paint in steps 2 and 7),
 * in paint order (order-modified for flex and grid containers), into the pending list. Stacking contexts are not
 * entered: their own children belong to them. */
static void collect(embi_pst *p, const embi_frag *f, float ox, float oy, uint32_t xf, uint32_t clip)
{
    bool ord = is_ordered(f);
    uint32_t start = ord ? push_ordered(p, f) : 0;
    uint32_t end = ord ? p->ord.len : 0;
    const embi_frag *k = ord ? nullptr : embi_frag_first(f);
    for (uint32_t i = start;;) {
        if (ord) {
            if (i >= end) {
                break;
            }
            k = p->ord.data[i++];
        } else {
            if (k == nullptr) {
                break;
            }
        }
        collect_child(p, k, ox, oy, xf, clip);
        if (!ord) {
            k = k->next;
        }
    }
    if (ord) {
        p->ord.len = start;
    }
}

/* Contexts of one z-index are in document order (not fragment order: an abspos fragment follows its siblings). */
static int cmp_pent(const void *a, const void *b)
{
    const embi_pent *x = a, *y = b;
    if (x->z != y->z) {
        return x->z < y->z ? -1 : 1;
    }
    return cmp_key(x, y);
}

static uint32_t scale_alpha(uint32_t c, float a)
{
    uint32_t na = (uint32_t)lroundf((float)(c >> 24) * a);
    return (c & 0x00FFFFFFu) | (na << 24);
}

/* Folds an opacity into the one item of a layer: the item's colors (and gradient stops) carry the alpha. */
static bool fold_alpha(embi_dl *d, embi_ditem *it, float a)
{
    switch (it->kind) {
    case EMBI_DI_TEXT:
        if ((it->flags & EMBI_DF_NOFOLD) != 0) {
            return false;
        }
        it->colors[0] = scale_alpha(it->colors[0], a);
        return true;
    case EMBI_DI_FILL:
    case EMBI_DI_DECORATION:
    case EMBI_DI_IMAGE:
    case EMBI_DI_SHADOW:
        it->colors[0] = scale_alpha(it->colors[0], a);
        return true;
    case EMBI_DI_BORDER:
        for (int i = 0; i < 4; i++) {
            it->colors[i] = scale_alpha(it->colors[i], a);
        }
        return true;
    case EMBI_DI_GRADIENT:
        for (uint32_t i = 0; i < it->aux2; i++) {
            embi_dstop *s = &d->stops.data[it->aux + i];
            s->color = scale_alpha(s->color, a);
        }
        return true;
    default:
        return false;
    }
}

/* Closes a layer opened at index begin: a layer with no items is dropped, and a layer with one item (and no
 * nested context) folds its opacity into that item. */
static void finish_layer(embi_pst *p, uint32_t begin, uint32_t ctx_index, float alpha)
{
    embi_dl *d = p->d;
    embi_ditem end;
    memset(&end, 0, sizeof end);
    end.kind = EMBI_DI_LAYER_END;
    end.aux = begin;
    embi_vec_push(&d->items, end);
    /* The open item names its end (dl.h): replay skips a group whose begin has no end. */
    d->items.data[begin].aux = d->items.len - 1;
    uint32_t n = d->items.len - begin - 2;
    bool nested = d->ctxs.len > ctx_index + 1;
    if (n == 0) {
        d->items.len = begin;
    } else if (d->items.data[begin].sub == EMBI_LAYER_FILTERED || d->items.data[begin].sub == EMBI_LS_COMPOSITE) {
        d->fx_items++; /* a filtered or composite group is never folded */
        if (d->items.data[begin].colors[0] != EMBI_BM_NORMAL) {
            p->nblend++;
        }
    } else if (n == 1 && !nested && fold_alpha(d, &d->items.data[begin + 1], alpha)) {
        d->items.data[begin] = d->items.data[begin + 1];
        d->items.len = begin + 1;
    }
    d->ctxs.data[ctx_index].end = d->items.len;
}

/* Paints the stacking context f from its own items down (its box at kx, ky, border box bb), under the transform xf and the
 * clip clip_parent. alpha below 1 puts the content in an opacity layer. */
void embi_pb_paint_body(embi_pst *p, const embi_frag *f, float kx, float ky, const float bb[4], uint32_t xf,
                        uint32_t clip_parent, uint32_t parent_ctx, float alpha)
{
    embi_dl *d = p->d;
    const embi_style *s = fstyle(f);
    clip_parent = abs_clip(p, f, kx, ky, xf, clip_parent);
    uint32_t ctx_index = d->ctxs.len;
    embi_dctx c = {d->items.len, 0, parent_ctx};
    embi_vec_push(&d->ctxs, c);
    if (p->rec != nullptr) {
        embi_pctx_rec r = {f, kx, ky, p->rec_xf_parent, p->rec_clip_parent, parent_ctx, 0};
        embi_vec_push(p->rec, r);
    }
    uint32_t begin = d->items.len;
    uint32_t saved_ctx = p->cur_ctx;
    p->cur_ctx = ctx_index;
    /* filter (R3a): a filtered context is a group whose steps replay applies before the opacity. The effect properties of
     * the root and html elements apply to their one context (the style of html, whose box is the reference box). */
    bool root_ctx = f == p->root && p->html != nullptr;
    const embi_style *es = root_ctx ? fstyle(p->html) : s;
    uint32_t fx_first = 0;
    uint32_t fx_n = embi_pb_filter_ops(p, es, EMBI_PROP_filter, &fx_first);
    /* blend mode, clip-path and mask (R3b): a composite group. */
    embi_pcomp cc;
    embi_pb_comp_scan(es, &cc);
    bool comp = embi_pb_comp_group(&cc);
    bool layer = alpha < 1.0f || fx_n > 0 || comp;
    const uint32_t nblend0 = p->nblend;
    if (comp) {
        const embi_frag *ef = root_ctx ? p->html : f;
        float ebb[4] = {root_ctx ? kx + p->html->x : bb[0], root_ctx ? ky + p->html->y : bb[1], ef->w, ef->h};
        embi_pb_comp_open(p, ef, es, &cc, ebb, fx_first, fx_n, alpha, xf, clip_parent);
    } else if (layer) {
        embi_ditem open;
        memset(&open, 0, sizeof open);
        open.kind = EMBI_DI_LAYER_BEGIN;
        open.f[0] = alpha;
        if (fx_n > 0) {
            open.sub = EMBI_LAYER_FILTERED;
            open.aux2 = fx_first;
            open.aux3 = fx_n;
            open.xform = xf;
            open.clip = clip_parent;
        }
        embi_vec_push(&d->items, open);
    }
    if (p->bg_pending && f == p->root) {
        p->bg_pending = false;
        p->canvas_bg = true;
        embi_pb_background(p, p->bg_src, p->bg_style, p->bg_rbb, 0, p->bg_vclip);
        p->canvas_bg = false;
    }
    /* The perspective of this box applies to its children. */
    embi_pers ps;
    bool has_persp = s != nullptr && f->box->el != nullptr && embi_lay_perspective_matrix(s, bb, p->vw, p->vh, p->rem, &ps.m);
    if (has_persp) {
        ps.el = f->box->el;
        embi_vec_push(&p->pers, ps);
    }
    self_paint(p, f, bb, xf, clip_parent);
    /* The first items of a context that is not a group are its own backdrop-filter items: the isolated group of a blending
     * descendant starts after them. */
    uint32_t iso_at = d->items.len;
    const uint32_t nbackdrop1 = p->nbackdrop;
    if (!layer) {
        iso_at = begin;
        while (iso_at < d->items.len && d->items.data[iso_at].kind == EMBI_DI_BACKDROP) {
            iso_at++;
        }
    }

    float cx, cy;
    uint32_t cclip, cxf;
    enter(p, f, kx, ky, xf, clip_parent, &cx, &cy, &cclip, &cxf);
    size_t mark = p->pend.len;
    collect(p, f, cx, cy, cxf, cclip);
    size_t end = p->pend.len;
    uint32_t kmark = p->keys.len;
    if (end > mark + 1) {
        key_entries(p, &p->pend.data[mark], (uint32_t)(end - mark), unit_node_of);
        qsort(&p->pend.data[mark], end - mark, sizeof(embi_pent), cmp_pent);
    }
    /* Step 2: negative z-index. The entries are copied out before recursing, since the list may grow. */
    for (size_t i = mark; i < end && p->pend.data[i].z < 0; i++) {
        embi_pent e = p->pend.data[i];
        paint_ctx(p, e.f, e.ox, e.oy, e.xform, e.clip, ctx_index);
    }
    /* Steps 3 to 5 and 6. */
    walk(p, f, cx, cy, cxf, cclip, PH_BLOCK);
    walk(p, f, cx, cy, cxf, cclip, PH_FLOAT);
    walk(p, f, cx, cy, cxf, cclip, PH_INLINE);
    walk(p, f, cx, cy, cxf, cclip, PH_POS);
    /* Step 7: positive z-index. */
    for (size_t i = mark; i < end; i++) {
        if (p->pend.data[i].z > 0) {
            embi_pent e = p->pend.data[i];
            paint_ctx(p, e.f, e.ox, e.oy, e.xform, e.clip, ctx_index);
        }
    }
    p->pend.len = (uint32_t)mark;
    p->keys.len = kmark;
    if (has_persp) {
        p->pers.len--;
    }
    p->cur_ctx = saved_ctx;
    if (p->rec != nullptr) {
        p->rec->data[ctx_index].sub_end = d->ctxs.len;
    }
    /* A stacking context is an isolated group: when a blend group, or a backdrop-filter below a backdrop root, is inside a
     * context that has no group of its own, its items get one (composite.c). The root context never isolates. */
    bool isolate = !layer && f != p->root && p->c3 == nullptr &&
                   (p->nblend != nblend0 || (cc.backdrop_root && p->nbackdrop != nbackdrop1));
    if (isolate) {
        embi_pb_isolate_from(p, iso_at, ctx_index);
    }
    if (layer || isolate) {
        p->nblend = nblend0; /* the blends inside are absorbed by the group */
        if (layer || cc.backdrop_root) {
            p->nbackdrop = nbackdrop1;
        }
    }
    if (layer) {
        finish_layer(p, begin, ctx_index, alpha);
    } else {
        d->ctxs.data[ctx_index].end = d->items.len;
    }
}

/* Paints the stacking context f (its box at kx, ky) under the transform and clip of its parent state. */
static void paint_ctx(embi_pst *p, const embi_frag *f, float kx, float ky, uint32_t xf_parent, uint32_t clip_parent,
                      uint32_t parent_ctx)
{
    const embi_style *s = fstyle(f);
    float bb[4] = {kx, ky, f->w, f->h};
    uint32_t xf;
    p->d->cur_scroll = embi_pb_xform_scroll(p->d, xf_parent);
    if (f == p->root && p->html != nullptr) {
        /* The document element's own opacity and transform apply to the one context of root and html. */
        xf = embi_pb_box_xform(p, s, bb, xf_parent);
        float alpha = embi_pb_opacity(s);
        const embi_style *hs = fstyle(p->html);
        float hbb[4] = {kx + p->html->x, ky + p->html->y, p->html->w, p->html->h};
        xf = embi_pb_box_xform(p, hs, hbb, xf);
        alpha *= embi_pb_opacity(hs);
        p->rec_xf_parent = xf_parent; /* W6b: what embi_paint_reenter is called with */
        p->rec_clip_parent = clip_parent;
        embi_pb_paint_body(p, f, kx, ky, bb, xf, clip_parent, parent_ctx, alpha);
        return;
    }
    xf = xf_parent;
    if (s != nullptr) {
        bool own = embi_lay_has_transform(s);
        bool part = p->c3 != nullptr && embi_p3_participant(p, f);
        if (own || part || embi_lay_preserve3d_used(s)) {
            embi_m4 om, pm, rel;
            embi_lay_box_matrix(s, bb, p->vw, p->vh, p->rem, &om);
            if (!embi_m4_invertible(&om)) {
                return; /* a singular transform (scale3d(2, 2, 0)) draws nothing */
            }
            if (parent_perspective(p, f, &pm)) {
                embi_m4_mul(&pm, &om, &rel);
            } else {
                rel = om;
            }
            if (part) {
                embi_p3_defer(p, f, kx, ky, &rel, parent_ctx);
                return;
            }
            double h[9];
            embi_m4_plane(&rel, h);
            bool pres = embi_lay_preserve3d_used(s);
            if (!pres && embi_lay_backface_hidden(s) && embi_h3_det(h) < 0.0) {
                return; /* the box faces away from the viewer and hides its back (a 3D root drops only its own plane) */
            }
            bool affine = fabs(h[6]) < 1e-9 && fabs(h[7]) < 1e-9 && h[8] > 0.0;
            if (!affine || pres) {
                embi_p3_paint(p, f, kx, ky, &rel, xf_parent, clip_parent, parent_ctx);
                return;
            }
            xf = xform_of_plane(p, h, xf_parent);
        }
    }
    p->rec_xf_parent = xf_parent;
    p->rec_clip_parent = clip_parent;
    embi_pb_paint_body(p, f, kx, ky, bb, xf, clip_parent, parent_ctx, embi_pb_opacity(s));
}

/* Whether the box background of a style paints anything (for the propagation to the canvas). */
static bool has_background(const embi_style *s)
{
    uint32_t rgba = 0;
    const embi_value *c = embi_style_value(s, EMBI_PROP_background_color);
    if (embi_pb_color(s, c, &rgba) && (rgba >> 24) != 0) {
        return true;
    }
    const embi_value *img = embi_style_value(s, EMBI_PROP_background_image);
    return img != nullptr && !(img->kind == EMBI_V_KEYWORD && img->kw == EMBI_KW_none);
}

bool embi_pb_named(const embi_element *el, const char *name)
{
    embi_str n = embi_atom_str(el->base.name);
    size_t len = strlen(name);
    if (n.len != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = n.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != name[i]) {
            return false;
        }
    }
    return true;
}

bool embi_paint_begin(embi_pst *pp, embi_dl *d, const embi_layout_engine *e, const embi_document *doc,
                      const embi_paint_cfg *cfg, bool emit_bg, uint32_t *vclip_out)
{
    const embi_frag *root = embi_layout_root(e, doc);
    *vclip_out = 0;
    if (root == nullptr) {
        return false;
    }
    static const embi_paint_cfg k_default_cfg = {nullptr, nullptr, 0xFFE8731Au, 0.0f, 0.0f, 1.0f};
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.rec = pp->rec;
    p.d = d;
    p.cfg = cfg != nullptr ? cfg : &k_default_cfg;
    p.vw = cfg != nullptr && cfg->viewport_w > 0.0f ? cfg->viewport_w : root->w;
    p.vh = cfg != nullptr && cfg->viewport_h > 0.0f ? cfg->viewport_h : root->h;
    p.root = root;
    p.rem = embi_lay_root_font_px(root);
    p.se = embi_layout_style_engine(e);
    p.text = embi_layout_text(e);
    p.ctl_fn = embi_layout_control_hook(e, &p.ctl_user);

    /* CSS Backgrounds 3 section 2.11.2: the root's background paints the canvas; without one, the body's does,
     * and the body paints nothing itself. */
    /* The document element (html) is the root's child box; its body is a child of it. */
    const embi_frag *html = nullptr;
    for (const embi_frag *k = embi_frag_first(root); k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el != nullptr && embi_pb_named(k->box->el, "html")) {
            html = k;
            break;
        }
    }
    const embi_frag *body = nullptr;
    if (html != nullptr) {
        for (const embi_frag *k = embi_frag_first(html); k != nullptr; k = k->next) {
            if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el != nullptr && embi_pb_named(k->box->el, "body")) {
                body = k;
                break;
            }
        }
    }
    if (body == nullptr) {
        /* An absolutely positioned body is a child of the viewport's containing block, not of html's fragment. */
        for (const embi_frag *k = embi_frag_first(root); k != nullptr; k = k->next) {
            if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el != nullptr && embi_pb_named(k->box->el, "body")) {
                body = k;
                break;
            }
        }
    }
    const embi_frag *src = html != nullptr ? html : root;
    const embi_style *ss = fstyle(src);
    bool html_bg = ss != nullptr && has_background(ss);
    /* Paint containment on the root or on the body keeps the body's background on the body (CSS Containment 2 section 4). */
    bool propagate = !html_bg && body != nullptr && body->box->style != nullptr && has_background(body->box->style) &&
                     !contains_paint(body->box->style) && (ss == nullptr || !contains_paint(ss));
    /* The canvas is the viewport: its background paints all of it, whatever the size of the root boxes. */
    float vp[4] = {0, 0, p.vw, p.vh};
    float zero[4] = {0, 0, 0, 0};
    uint32_t vclip = emit_bg ? embi_pb_clip(&p, vp, zero, 0, 0) : 0;
    if (html_bg || propagate) {
        const embi_frag *from = html_bg ? html : body;
        p.bg_skip = from->box->el;
        if (emit_bg) {
            /* The positioning area is the root element's box; the painting area is the whole canvas. */
            float rbb[4] = {src == root ? 0.0f : src->x, src == root ? 0.0f : src->y, src->w, src->h};
            /* The opacity, filter and the other effects of the root element apply to the canvas background too (Compositing
             * and Blending 1, the page backdrop): when the root context has a group, the background is painted inside it. */
            embi_pcomp rc;
            embi_pb_comp_scan(ss, &rc);
            float ralpha = embi_pb_opacity(ss) * (src != root ? embi_pb_opacity(fstyle(root)) : 1.0f);
            bool group = ralpha < 1.0f || embi_pb_comp_group(&rc) ||
                         (ss != nullptr && embi_style_value(ss, EMBI_PROP_filter) != nullptr &&
                          embi_style_value(ss, EMBI_PROP_filter)->kind == EMBI_V_FILTERS &&
                          embi_style_value(ss, EMBI_PROP_filter)->filters.n > 0);
            if (group) {
                p.bg_src = src;
                p.bg_style = html_bg ? ss : body->box->style;
                memcpy(p.bg_rbb, rbb, sizeof rbb);
                p.bg_vclip = vclip;
                p.bg_pending = true;
                p.bg_deferred = true;
            } else {
                p.canvas_bg = true;
                embi_pb_background(&p, src, html_bg ? ss : body->box->style, rbb, 0, vclip);
                p.canvas_bg = false;
            }
        }
    }
    p.html = html;
    *vclip_out = vclip;
    *pp = p;
    return true;
}

void embi_paint_reenter(embi_pst *p, const embi_frag *f, float kx, float ky, uint32_t xf_parent, uint32_t clip_parent,
                        uint32_t parent_ctx)
{
    if (svg_fo(f)) {
        /* R4c: the SVG walk gave the context its transform, clip and opacity (embi_pb_paint_foreign) */
        float bb[4] = {kx, ky, f->w, f->h};
        p->rec_xf_parent = xf_parent;
        p->rec_clip_parent = clip_parent;
        embi_pb_paint_body(p, f, kx, ky, bb, xf_parent, clip_parent, parent_ctx, 1.0f);
        return;
    }
    paint_ctx(p, f, kx, ky, xf_parent, clip_parent, parent_ctx);
}

void embi_pb_paint_foreign(embi_pst *p, const embi_element *fo, uint32_t xf, uint32_t clip)
{
    if (p == nullptr || fo == nullptr || fo->box == nullptr) {
        return;
    }
    const embi_box *b = (const embi_box *)fo->box;
    const embi_frag *f = b->frag;
    if ((b->flags2 & EMBI_BF2_SVGFO) == 0 || f == nullptr || f->box != b || !svg_fo(f)) {
        return;
    }
    /* Its own transform, opacity, clip-path and mask are applied by the SVG walk as for any SVG element: the context is
     * painted at the origin of the transform the walk gives (its x, y in its user space). */
    float bb[4] = {0.0f, 0.0f, f->w, f->h};
    p->rec_xf_parent = xf;
    p->rec_clip_parent = clip;
    embi_pb_paint_body(p, f, 0.0f, 0.0f, bb, xf, clip, p->cur_ctx, 1.0f);
}

void embi_paint_end(embi_pst *pp)
{
    embi_pst p = *pp;
    embi_vec_free(&p.pend);
    embi_vec_free(&p.pers);
    embi_vec_free(&p.ord);
    embi_vec_free(&p.oks);
    embi_vec_free(&p.units);
    embi_vec_free(&p.keys);
    embi_hashmap_free(&p.sibs);
    embi_hashmap_free(&p.sibs_done);
    embi_hashmap_free(&p.scr);
    *pp = p;
}

void embi_paint_document(embi_dl *d, const embi_layout_engine *e, const embi_document *doc,
                         const embi_paint_cfg *cfg)
{
    embi_dl_reset(d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    uint32_t vclip;
    if (!embi_paint_begin(&p, d, e, doc, cfg, true, &vclip)) {
        return;
    }
    paint_ctx(&p, p.root, 0.0f, 0.0f, 0, vclip, 0);
    embi_pb_control_popup(&p); /* W5e: the open select's list over everything */
    embi_paint_end(&p);
}
