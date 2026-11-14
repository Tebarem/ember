/*
 * Sizing helpers (W3a, section 2): property access, lengths and percentages, border/padding/margin
 * widths, intrinsic content sizes (CSS Sizing 3), aspect ratios (CSS Sizing 4 section 5), and
 * replaced boxes (CSS 2.1 sections 10.3.2, 10.4 and 10.6.2).
 *
 * Intrinsic sizes are computed once per box and cached on it (EMBI_BF_INTR). They depend only on the
 * box tree and the styles, so the cache is valid for the whole layout run.
 *
 * Container units. The cq* units resolve against the nearest open container with a container-type
 * (lctx.cq, pushed by block.c once the container's content size is known). Without one they fall
 * back to the viewport (no separate small viewport yet).
 *
 * Limits. Replaced min and max constraints transfer through the ratio with the CSS 2.1 table. The
 * min-content and max-content keywords of replaced sizes count as auto.
 */
#include "layout_int.h"

#include "css/props.h"
#include "dom/dom.h"

#include <SDL3/SDL_stdinc.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* The computed value of a property with no calc-size() reading (see embi_lay_prop). */
static const embi_value *raw_prop(const embi_lctx *c, const embi_box *b, embi_prop_id p)
{
    if ((b->flags & EMBI_BF_ANON) != 0 && !embi_prop_is_inherited(p)) {
        return embi_style_value(c->initial, p);
    }
    if (b->style == nullptr) {
        return embi_style_value(c->initial, p);
    }
    return embi_style_value(b->style, p);
}

/* calc-size(<basis>, <calc>) is the FUNC value with two arguments named calc-size (parsed by
 * value_misc.c). */
bool embi_lay_is_calc_size(const embi_value *v)
{
    if (v == nullptr || v->kind != EMBI_V_FUNC || v->func.n != 2) {
        return false;
    }
    embi_str s = embi_atom_str(v->func.name);
    return s.len == 9 && memcmp(s.ptr, "calc-size", 9) == 0;
}

const embi_value *embi_lay_prop(const embi_lctx *c, const embi_box *b, embi_prop_id p)
{
    const embi_value *v = raw_prop(c, b, p);
    /* only the six size properties can hold calc-size() (EMBI_BF2_CALCSIZE says this box's style has one) */
    return (b->flags2 & EMBI_BF2_CALCSIZE) != 0 && embi_lay_is_calc_size(v) ? &v->func.args[0] : v;
}

const embi_value *embi_lay_prop_calc(const embi_lctx *c, const embi_box *b, embi_prop_id p, const embi_value **expr)
{
    const embi_value *v = raw_prop(c, b, p);
    if ((b->flags2 & EMBI_BF2_CALCSIZE) != 0 && embi_lay_is_calc_size(v)) {
        *expr = &v->func.args[1];
        return &v->func.args[0];
    }
    *expr = nullptr;
    return v;
}

const embi_value *embi_lay_size_expr_slow(const embi_lctx *c, const embi_box *b, embi_prop_id p)
{
    const embi_value *v = raw_prop(c, b, p);
    return embi_lay_is_calc_size(v) ? &v->func.args[1] : nullptr;
}

bool embi_lay_is_auto(const embi_value *v)
{
    return v == nullptr || (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_auto);
}

float embi_lay_font_px(const embi_lctx *c, const embi_box *b)
{
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_font_size);
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    return 16.0f;
}

/* The length context of a box: fonts, the viewport and the nearest container's content size. */
static embi_len_ctx len_ctx(const embi_lctx *c, const embi_box *b)
{
    embi_len_ctx lc;
    memset(&lc, 0, sizeof lc);
    lc.font_px = embi_lay_font_px(c, b);
    lc.root_px = c->root_px;
    lc.vw = c->vw;
    lc.vh = c->vh;
    lc.cq_w = NAN;
    lc.cq_h = NAN;
    if (c->ncq > 0) {
        lc.cq_w = c->cq[c->ncq - 1].w;
        lc.cq_h = c->cq[c->ncq - 1].h;
    }
    return lc;
}

float embi_lay_len(const embi_lctx *c, const embi_box *b, const embi_value *v, float pct)
{
    if (v == nullptr) {
        return NAN;
    }
    switch (v->kind) {
    case EMBI_V_LENGTH: {
        if (v->unit == EMBI_U_PX) {
            return (float)v->num;
        }
        embi_len_ctx lc = len_ctx(c, b);
        return (float)embi_len_px_cb(&lc, v->unit, v->num);
    }
    case EMBI_V_PERCENT:
        return isnan(pct) ? NAN : (float)(pct * v->num / 100.0);
    case EMBI_V_CALC: {
        embi_len_ctx lc = len_ctx(c, b);
        embi_calc_env env;
        env.pct_base = isnan(pct) ? NAN : (double)pct;
        env.len_px = embi_len_px_cb;
        env.user = &lc;
        return (float)embi_calc_eval(v->calc, &env);
    }
    default:
        return NAN;
    }
}

float embi_lay_size_eval(const embi_lctx *c, const embi_box *b, const embi_value *expr, float size, float pct)
{
    if (expr == nullptr) {
        return NAN;
    }
    if (expr->kind != EMBI_V_CALC) {
        return embi_lay_len(c, b, expr, pct); /* a plain length: it does not read `size` */
    }
    embi_len_ctx lc = len_ctx(c, b);
    embi_calc_env env = {.pct_base = isnan(pct) ? NAN : (double)pct, .len_px = embi_len_px_cb, .user = &lc};
    return (float)embi_calc_eval_sized(expr->calc, &env, (double)size);
}

/* The used width of a border (CSS 2.1 section 8.5.2): zero when the style is none or hidden. */
static float border_width(const embi_lctx *c, const embi_box *b, embi_prop_id width_p,
                          embi_prop_id style_p)
{
    const embi_value *st = embi_lay_prop(c, b, style_p);
    if (st == nullptr || st->kind != EMBI_V_KEYWORD || st->kw == EMBI_KW_none ||
        st->kw == EMBI_KW_hidden) {
        return 0.0f;
    }
    const embi_value *w = embi_lay_prop(c, b, width_p);
    if (w == nullptr) {
        return 0.0f;
    }
    if (w->kind == EMBI_V_KEYWORD) {
        switch (w->kw) {
        case EMBI_KW_thin:
            return 1.0f;
        case EMBI_KW_thick:
            return 5.0f;
        default:
            return 3.0f;
        }
    }
    float v = embi_lay_len(c, b, w, NAN);
    if (isnan(v) || v <= 0) {
        return 0.0f;
    }
    /* The computed width snaps to whole pixels: at least 1 when it is more than 0, else rounded down (CSS Backgrounds 3
     * section 4.2). Layout runs in CSS px; the snapping uses them as device pixels. */
    return v < 1.0f ? 1.0f : floorf(v);
}

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

float embi_lay_border_side(const embi_lctx *c, const embi_box *b, int side)
{
    static const embi_prop_id wp[4] = {EMBI_PROP_border_top_width, EMBI_PROP_border_right_width,
                                       EMBI_PROP_border_bottom_width, EMBI_PROP_border_left_width};
    static const embi_prop_id sp[4] = {EMBI_PROP_border_top_style, EMBI_PROP_border_right_style,
                                       EMBI_PROP_border_bottom_style, EMBI_PROP_border_left_style};
    return border_width(c, b, wp[side], sp[side]);
}

/* The used edges of a style that do not depend on the containing block (W3p): border widths, and the
 * paddings and margins that are not percentages or calc(). embi_lay_edges reads 16 properties per call and
 * runs for every box several times in a layout, so the values are computed once per style (and per anon
 * flag: an anonymous box reads the initial style) and kept with the box tree, which is rebuilt whenever
 * the styles, the viewport or the root font could have changed. Not used inside a container (cq units). */
typedef struct lay_ecache {
    float bw[4];                 /* border widths: left, top, right, bottom */
    float cv[8];                 /* padding l, t, r, b then margin l, t, r, b (NaN: auto margin) */
    const embi_value *dyn[8];    /* the value to evaluate against the containing block, else nullptr */
} lay_ecache;

static const lay_ecache *ecache_of(const embi_lctx *c, const embi_box *b)
{
    bool anon = (b->flags & EMBI_BF_ANON) != 0;
    const embi_style *sp = anon || b->style == nullptr ? c->initial : b->style;
    uint64_t key = (uint64_t)(uintptr_t)sp | (anon ? 1u : 0u);
    uint64_t v = 0;
    uint32_t fi = (uint32_t)((key >> 4) * 2654435761u >> 26) & 63u;
    if (c->ld->efront[fi].key == key && c->ld->efront[fi].e != nullptr) {
        return (const lay_ecache *)c->ld->efront[fi].e;
    }
    if (embi_hashmap_get(&c->ld->ecache, key, &v)) {
        c->ld->efront[fi].key = key;
        c->ld->efront[fi].e = (const void *)(uintptr_t)v;
        return (const lay_ecache *)(uintptr_t)v;
    }
    lay_ecache *e = embi_arena_alloc(&c->ld->pa, sizeof *e, alignof(lay_ecache));
    if (e == nullptr) {
        return nullptr;
    }
    memset(e, 0, sizeof *e);
    e->bw[0] = border_width(c, b, EMBI_PROP_border_left_width, EMBI_PROP_border_left_style);
    e->bw[1] = border_width(c, b, EMBI_PROP_border_top_width, EMBI_PROP_border_top_style);
    e->bw[2] = border_width(c, b, EMBI_PROP_border_right_width, EMBI_PROP_border_right_style);
    e->bw[3] = border_width(c, b, EMBI_PROP_border_bottom_width, EMBI_PROP_border_bottom_style);
    static const embi_prop_id props[8] = {
        EMBI_PROP_padding_left, EMBI_PROP_padding_top,  EMBI_PROP_padding_right, EMBI_PROP_padding_bottom,
        EMBI_PROP_margin_left,  EMBI_PROP_margin_top,   EMBI_PROP_margin_right,  EMBI_PROP_margin_bottom};
    for (int i = 0; i < 8; i++) {
        const embi_value *pv = embi_lay_prop(c, b, props[i]);
        if (pv != nullptr && (pv->kind == EMBI_V_PERCENT || pv->kind == EMBI_V_CALC)) {
            e->dyn[i] = pv;
        } else if (i >= 4 && embi_lay_is_auto(pv)) {
            e->cv[i] = NAN;
        } else {
            float l = embi_lay_len(c, b, pv, NAN);
            e->cv[i] = i < 4 ? clamp0(l) : (isnan(l) ? 0.0f : l);
        }
    }
    (void)embi_hashmap_put(&c->ld->ecache, key, (uint64_t)(uintptr_t)e);
    c->ld->efront[fi].key = key;
    c->ld->efront[fi].e = e;
    return e;
}

void embi_lay_edges(const embi_lctx *c, const embi_box *b, float cbw, embi_edges *bdr,
                    embi_edges *pad, embi_edges *mar)
{
    const lay_ecache *ec = c->ncq == 0 ? ecache_of(c, b) : nullptr;
    if (ec != nullptr) {
        if (c->edge_box == b) {
            bdr->t = c->edge_bdr[0];
            bdr->r = c->edge_bdr[1];
            bdr->b = c->edge_bdr[2];
            bdr->l = c->edge_bdr[3];
        } else {
            bdr->l = ec->bw[0];
            bdr->t = ec->bw[1];
            bdr->r = ec->bw[2];
            bdr->b = ec->bw[3];
        }
        float p[4];
        for (int i = 0; i < 4; i++) {
            p[i] = ec->dyn[i] != nullptr ? clamp0(embi_lay_len(c, b, ec->dyn[i], cbw)) : ec->cv[i];
        }
        pad->l = p[0];
        pad->t = p[1];
        pad->r = p[2];
        pad->b = p[3];
        if (embi_lay_is_table(b)) {
            float h[4];
            if (embi_table_collapse_edges(c, b, h)) {
                bdr->t = h[0];
                bdr->r = h[1];
                bdr->b = h[2];
                bdr->l = h[3];
                pad->l = pad->t = pad->r = pad->b = 0;
            }
        }
        if (mar != nullptr) {
            float *out[4] = {&mar->l, &mar->t, &mar->r, &mar->b};
            for (int i = 0; i < 4; i++) {
                if (ec->dyn[4 + i] != nullptr) {
                    float m = embi_lay_len(c, b, ec->dyn[4 + i], cbw);
                    *out[i] = isnan(m) ? 0.0f : m;
                } else {
                    *out[i] = ec->cv[4 + i];
                }
            }
        }
        return;
    }
    if (c->edge_box == b) {
        /* A cell of a collapsing table: its half borders are the table's (table.c, W3g). */
        bdr->t = c->edge_bdr[0];
        bdr->r = c->edge_bdr[1];
        bdr->b = c->edge_bdr[2];
        bdr->l = c->edge_bdr[3];
    } else {
        bdr->l = border_width(c, b, EMBI_PROP_border_left_width, EMBI_PROP_border_left_style);
        bdr->t = border_width(c, b, EMBI_PROP_border_top_width, EMBI_PROP_border_top_style);
        bdr->r = border_width(c, b, EMBI_PROP_border_right_width, EMBI_PROP_border_right_style);
        bdr->b = border_width(c, b, EMBI_PROP_border_bottom_width, EMBI_PROP_border_bottom_style);
    }
    pad->l = clamp0(embi_lay_len(c, b, embi_lay_prop(c, b, EMBI_PROP_padding_left), cbw));
    pad->t = clamp0(embi_lay_len(c, b, embi_lay_prop(c, b, EMBI_PROP_padding_top), cbw));
    pad->r = clamp0(embi_lay_len(c, b, embi_lay_prop(c, b, EMBI_PROP_padding_right), cbw));
    pad->b = clamp0(embi_lay_len(c, b, embi_lay_prop(c, b, EMBI_PROP_padding_bottom), cbw));
    if (embi_lay_is_table(b)) {
        /* A table with border-collapse: collapse has half of its outer borders and no padding. */
        float h[4];
        if (embi_table_collapse_edges(c, b, h)) {
            bdr->t = h[0];
            bdr->r = h[1];
            bdr->b = h[2];
            bdr->l = h[3];
            pad->l = pad->t = pad->r = pad->b = 0;
        }
    }
    if (mar != nullptr) {
        const embi_prop_id mp[4] = {EMBI_PROP_margin_left, EMBI_PROP_margin_top,
                                    EMBI_PROP_margin_right, EMBI_PROP_margin_bottom};
        float *out[4] = {&mar->l, &mar->t, &mar->r, &mar->b};
        for (int i = 0; i < 4; i++) {
            const embi_value *v = embi_lay_prop(c, b, mp[i]);
            if (embi_lay_is_auto(v)) {
                *out[i] = NAN;
            } else {
                float m = embi_lay_len(c, b, v, cbw);
                *out[i] = isnan(m) ? 0.0f : m;
            }
        }
    }
}

/* ------------------------------------------------------------------ aspect ratios */

/* The natural ratio of a replaced box, when it has one. */
static bool natural_ratio(const embi_box *b, float *ratio)
{
    if ((b->flags & EMBI_BF_HAS_RATIO) == 0 || !(b->rw > 0) || !(b->rh > 0)) {
        return false;
    }
    *ratio = b->rw / b->rh;
    return true;
}

/* The contain-intrinsic-width or -height of a box in px: the length (after an optional auto), 0 for none. */
static float ci_len(const embi_lctx *c, const embi_box *b, embi_prop_id p)
{
    const embi_value *v = embi_lay_prop(c, b, p);
    if (v != nullptr && v->kind == EMBI_V_LIST && v->list.n == 2) {
        v = &v->list.items[1];
    }
    if (v == nullptr || v->kind != EMBI_V_LENGTH) {
        return 0.0f;
    }
    float l = embi_lay_len(c, b, v, 0.0f);
    return l > 0.0f ? l : 0.0f;
}

/* A replaced box with size containment (contain: size or strict, container-type: size) sizes as if its natural
 * size were its contain-intrinsic-size and it had no natural ratio (CSS Containment 2 section 3.2, CSS Sizing 4
 * section 5). */
static bool replaced_contained(const embi_lctx *c, const embi_box *b, float *rw, float *rh)
{
    if ((b->flags & EMBI_BF_REPLACED) == 0 || (b->flags2 & EMBI_BF2_CONTROL) != 0 ||
        !embi_lay_block_size_contained(c, b)) {
        return false;
    }
    if (rw != nullptr) {
        *rw = ci_len(c, b, EMBI_PROP_contain_intrinsic_width);
    }
    if (rh != nullptr) {
        *rh = ci_len(c, b, EMBI_PROP_contain_intrinsic_height);
    }
    return true;
}

/* As embi_lay_ratio, and *natural tells whether the ratio is the natural one of a replaced box (it
 * applies to the content box) or comes from the aspect-ratio property (it applies to the box that
 * box-sizing selects). */
static bool ratio_src(const embi_lctx *c, const embi_box *b, float *ratio, bool *natural)
{
    *natural = false;
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_aspect_ratio);
    bool has_auto = false;
    const embi_value *r = nullptr;
    if (v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_auto) {
        has_auto = true;
    } else if (v != nullptr && v->kind == EMBI_V_RATIO) {
        r = v;
    } else if (v != nullptr && v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            const embi_value *k = &v->list.items[i];
            if (k->kind == EMBI_V_KEYWORD && k->kw == EMBI_KW_auto) {
                has_auto = true;
            } else if (k->kind == EMBI_V_RATIO) {
                r = k;
            }
        }
    }
    if (r != nullptr && !(r->ratio.a > 0 && r->ratio.b > 0 && isfinite(r->ratio.a) && isfinite(r->ratio.b))) {
        /* A degenerate ratio (0 or infinite term) is ignored: the box has the natural ratio when it has one (CSS Sizing 4
         * section 4.1, zero-or-infinity-006..010). */
        r = nullptr;
        has_auto = true;
    }
    float nat = 0;
    /* auto with a natural ratio: the natural ratio wins over the specified one (CSS Sizing 4). */
    if (has_auto && !replaced_contained(c, b, nullptr, nullptr) && natural_ratio(b, &nat)) {
        *ratio = nat;
        *natural = true;
        return true;
    }
    if (r != nullptr && r->ratio.a > 0 && r->ratio.b > 0) {
        *ratio = (float)(r->ratio.a / r->ratio.b);
        /* auto <ratio> relates content boxes even when it is the <ratio> that applies (block-aspect-ratio-004). */
        *natural = has_auto;
        return true;
    }
    return false;
}

bool embi_lay_ratio_content(const embi_lctx *c, const embi_box *b)
{
    float r;
    bool natural = false;
    return ratio_src(c, b, &r, &natural) && natural;
}

bool embi_lay_ratio(const embi_lctx *c, const embi_box *b, float *ratio)
{
    bool natural;
    return ratio_src(c, b, ratio, &natural);
}

/* ------------------------------------------------------------------ intrinsic sizes */

static float max2(float a, float b)
{
    return a > b ? a : b;
}

/* The content size of a form control on one axis from its border-box constant: the constant minus
 * the box's border and padding when the control is box-sizing: border-box (the UA sheet's value),
 * else the constant itself. */
static float control_content(embi_lctx *c, embi_box *b, float v, bool vertical)
{
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    if (bs == nullptr || bs->kind != EMBI_V_KEYWORD || bs->kw != EMBI_KW_border_box) {
        return v;
    }
    embi_edges bdr, pad;
    embi_lay_edges(c, b, 0, &bdr, &pad, nullptr);
    float e = vertical ? bdr.t + bdr.b + pad.t + pad.b : bdr.l + bdr.r + pad.l + pad.r;
    return v > e ? v - e : 0;
}

static float replaced_spec(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct, float edges, bool border_box);
static float replaced_limit(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct, float edges, bool border_box,
                            float dflt, float auto_basis);
static void replaced_clamp(float *w, float *h, float minw, float maxw, float minh, float maxh, bool ratio_ok, float ratio);
static bool pct_height(const embi_value *v);
static float intr_height_prop(embi_lctx *c, const embi_box *b, embi_prop_id p, float edges_v, bool border_box);

/* The content width of a replaced box with an auto width, as its used width is without a containing block size
 * (percentages behave as auto): the natural size through the min and max table of CSS 2.1 section 10.4, which moves the
 * width with a min-height or max-height (box-sizing-replaced-001..003). A specified height gives the width through the
 * ratio; a percentage one resolves against the percentage basis of intrinsic sizing (embi_lctx.ih_on), and is auto
 * without one. w0 when the box has no natural size to start from. */
static float replaced_auto_width(embi_lctx *c, embi_box *b, float w0)
{
    if ((b->flags2 & (EMBI_BF2_NOSIZE | EMBI_BF2_CONTROL)) != 0) {
        return w0;
    }
    embi_edges bdr, pad;
    embi_lay_edges(c, b, 0, &bdr, &pad, nullptr);
    float edges_h = bdr.l + bdr.r + pad.l + pad.r;
    float edges_v = bdr.t + bdr.b + pad.t + pad.b;
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    bool border_box = bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
    float ratio = 0;
    bool natural_r = false;
    bool has_ratio = ratio_src(c, b, &ratio, &natural_r) && ratio > 0;
    bool bb_ratio = has_ratio && !natural_r && border_box;
    float xh = bb_ratio ? edges_h : 0, xv = bb_ratio ? edges_v : 0;
    float rw = b->rw > 0 ? b->rw : 0, rh = b->rh > 0 ? b->rh : 0;
    float h = NAN;
    float w;
    /* A definite height (a percentage one against the basis of intrinsic sizing) gives the width through the ratio
     * (CSS 2.1 section 10.3.2; intrinsic-percent-replaced-015..024), within the limits. */
    float hs = intr_height_prop(c, b, EMBI_PROP_height, edges_v, border_box);
    if (!isnan(hs)) {
        float ih = embi_lay_ih_get(c);
        float minh0 = replaced_limit(c, b, EMBI_PROP_min_height, ih, edges_v, border_box, 0, 0.0f);
        float maxh0 = replaced_limit(c, b, EMBI_PROP_max_height, ih, edges_v, border_box, INFINITY, NAN);
        if (pct_height(embi_lay_prop(c, b, EMBI_PROP_min_height)) || pct_height(embi_lay_prop(c, b, EMBI_PROP_max_height))) {
            c->ih_need++;
        }
        hs = hs > maxh0 ? maxh0 : hs;
        hs = hs < minh0 ? minh0 : hs;
        w = has_ratio ? (hs + xv) * ratio - xh : rw;
        float minw0 = replaced_limit(c, b, EMBI_PROP_min_width, NAN, edges_h, border_box, 0, 0.0f);
        float maxw0 = replaced_limit(c, b, EMBI_PROP_max_width, NAN, edges_h, border_box, INFINITY, NAN);
        w = w > maxw0 ? maxw0 : w;
        w = w < minw0 ? minw0 : w;
        return clamp0(w);
    }
    if (has_ratio && rw > 0) {
        w = rw;
        h = (w + xh) / ratio - xv;
    } else if (has_ratio && rh > 0) {
        h = rh;
        w = (h + xv) * ratio - xh;
    } else if (has_ratio) {
        return w0;
    } else {
        w = rw;
        h = isnan(h) ? rh : h;
    }
    w = clamp0(w);
    h = clamp0(h);
    float minw = replaced_limit(c, b, EMBI_PROP_min_width, NAN, edges_h, border_box, 0, 0.0f);
    float maxw = replaced_limit(c, b, EMBI_PROP_max_width, NAN, edges_h, border_box, INFINITY, NAN);
    float ih = embi_lay_ih_get(c);
    if (pct_height(embi_lay_prop(c, b, EMBI_PROP_min_height)) || pct_height(embi_lay_prop(c, b, EMBI_PROP_max_height))) {
        c->ih_need++;
    }
    float minh = replaced_limit(c, b, EMBI_PROP_min_height, ih, edges_v, border_box, 0, 0.0f);
    float maxh = replaced_limit(c, b, EMBI_PROP_max_height, ih, edges_v, border_box, INFINITY, NAN);
    maxw = maxw < minw ? minw : maxw;
    maxh = maxh < minh ? minh : maxh;
    replaced_clamp(&w, &h, minw, maxw, minh, maxh, has_ratio, ratio);
    return clamp0(w);
}

/* The intrinsic content sizes of a replaced box: its natural width (moved by the min and max table when its width is
 * auto, replaced_auto_width), or the specified one. */
static void replaced_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    float w = b->rw;
    if (isnan(w) || w < 0) {
        w = 0;
    }
    (void)replaced_contained(c, b, &w, nullptr);
    if ((b->flags2 & EMBI_BF2_CONTROL) != 0) {
        w = control_content(c, b, w, false);
    } else if (embi_lay_is_auto(embi_lay_prop(c, b, EMBI_PROP_width))) {
        w = replaced_auto_width(c, b, w);
    }
    *mn = w;
    *mx = w;
}

/* True when the box has size containment in the inline axis, so its content does not contribute to
 * its intrinsic inline size (CSS Containment 3 section 3): container-type: size or inline-size, and
 * contain: size, inline-size, strict. */
static bool inline_size_contained(const embi_lctx *c, const embi_box *b)
{
    const embi_value *ct = embi_lay_prop(c, b, EMBI_PROP_container_type);
    if (ct != nullptr && ct->kind == EMBI_V_KEYWORD &&
        (ct->kw == EMBI_KW_size || ct->kw == EMBI_KW_inline_size)) {
        return true;
    }
    const embi_value *cv = embi_lay_prop(c, b, EMBI_PROP_contain);
    if (cv == nullptr) {
        return false;
    }
    if (cv->kind == EMBI_V_KEYWORD) {
        return cv->kw == EMBI_KW_size || cv->kw == EMBI_KW_inline_size || cv->kw == EMBI_KW_strict;
    }
    if (cv->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < cv->list.n; i++) {
            const embi_value *k = &cv->list.items[i];
            if (k->kind == EMBI_V_KEYWORD &&
                (k->kw == EMBI_KW_size || k->kw == EMBI_KW_inline_size)) {
                return true;
            }
        }
    }
    return false;
}

/* True when the box has size containment in the block axis: its auto height ignores its content
 * (contain: size or strict, container-type: size). */
bool embi_lay_block_size_contained(const embi_lctx *c, const embi_box *b)
{
    const embi_value *ct = embi_lay_prop(c, b, EMBI_PROP_container_type);
    if (ct != nullptr && ct->kind == EMBI_V_KEYWORD && ct->kw == EMBI_KW_size) {
        return true;
    }
    const embi_value *cv = embi_lay_prop(c, b, EMBI_PROP_contain);
    if (cv == nullptr) {
        return false;
    }
    if (cv->kind == EMBI_V_KEYWORD) {
        return cv->kw == EMBI_KW_size || cv->kw == EMBI_KW_strict;
    }
    if (cv->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < cv->list.n; i++) {
            const embi_value *k = &cv->list.items[i];
            if (k->kind == EMBI_V_KEYWORD && k->kw == EMBI_KW_size) {
                return true;
            }
        }
    }
    return false;
}

static bool same_f(float a, float b)
{
    return (isnan(a) && isnan(b)) || a == b;
}

/* True for a height value that resolves against the containing block's height (a percentage, or calc() that may hold
 * one). Such a value looks at the percentage basis of intrinsic sizing (embi_lctx.ih_need). */
static bool pct_height(const embi_value *v)
{
    return v != nullptr && (v->kind == EMBI_V_PERCENT || v->kind == EMBI_V_CALC);
}

/* A height property of b as a content-box length for intrinsic sizing: its percentages resolve against the basis of
 * intrinsic sizing (embi_lay_ih_get). NaN when auto, a keyword, or a percentage without a basis. */
static float intr_height_prop(embi_lctx *c, const embi_box *b, embi_prop_id p, float edges_v, bool border_box)
{
    const embi_value *v = embi_lay_prop(c, b, p);
    if (v == nullptr || embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD) {
        return NAN;
    }
    if (pct_height(v)) {
        c->ih_need++;
    }
    float h = embi_lay_len(c, b, v, embi_lay_ih_get(c));
    if (isnan(h)) {
        return NAN;
    }
    return clamp0(border_box ? h - edges_v : h);
}

/* The percentage basis that b gives its children while its intrinsic sizes are computed: its definite content height
 * (a length, or a percentage of its own basis cb, within its min-height and max-height), else NaN. An anonymous box
 * passes its parent's on. *dep: the result depends on cb. */
static float intr_basis(embi_lctx *c, const embi_box *b, bool *dep)
{
    *dep = false;
    if ((b->flags & EMBI_BF_ANON) != 0) {
        *dep = true;
        return embi_lay_ih_get(c);
    }
    if (b->tr != EMBI_TR_NONE) {
        return NAN; /* table parts size their heights in the table layout */
    }
    const embi_value *hv = embi_lay_prop(c, b, EMBI_PROP_height);
    if (hv == nullptr || embi_lay_is_auto(hv) || hv->kind == EMBI_V_KEYWORD) {
        return NAN;
    }
    uint32_t need0 = c->ih_need;
    embi_edges bdr, pad;
    embi_lay_edges(c, b, 0, &bdr, &pad, nullptr);
    float ev = bdr.t + bdr.b + pad.t + pad.b;
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    bool border_box = bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
    float h = intr_height_prop(c, b, EMBI_PROP_height, ev, border_box);
    if (!isnan(h)) {
        float mx = intr_height_prop(c, b, EMBI_PROP_max_height, ev, border_box);
        float mn = intr_height_prop(c, b, EMBI_PROP_min_height, ev, border_box);
        h = !isnan(mx) && h > mx ? mx : h;
        h = !isnan(mn) && h < mn ? mn : h;
    }
    *dep = c->ih_need != need0;
    c->ih_need = need0;
    return h;
}

/* Intrinsic sizes depend on the cq units of the content, which resolve against the innermost open
 * container. The cache is keyed by that container size (cq_w, cq_h): a lookup under another
 * container recomputes. Replaced boxes are cheap and not cached. */
void embi_lay_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    float kw = NAN, kh = NAN;
    if (c->ncq > 0) {
        kw = c->cq[c->ncq - 1].w;
        kh = c->cq[c->ncq - 1].h;
    }
    bool replaced = (b->flags & EMBI_BF_REPLACED) != 0;
    if ((b->flags & EMBI_BF_INTR) && !replaced && same_f(b->cq_w, kw) && same_f(b->cq_h, kh)) {
        *mn = b->min_c;
        *mx = b->max_c;
        return;
    }
    float a = 0, z = 0;
    if (replaced) {
        replaced_intrinsic(c, b, &a, &z);
        *mn = a;
        *mx = z;
        return;
    }
    /* The children's percentage heights resolve against b's definite height (CSS Sizing 3 section 5.2.1 and
     * Chromium): a result that used it and depends on b's own basis is not cached. */
    uint32_t need0 = c->ih_need;
    bool dep = false;
    embi_ih ih = embi_lay_ih_push(c, intr_basis(c, b, &dep));
    if (inline_size_contained(c, b)) {
        /* as if the box had no content */
    } else if (b->kind == EMBI_BK_FLEX) {
        embi_flex_intrinsic(c, b, &a, &z);
    } else if (b->kind == EMBI_BK_GRID) {
        embi_grid_intrinsic(c, b, &a, &z);
    } else if (embi_lay_is_table(b)) {
        embi_table_intrinsic(c, b, &a, &z);
    } else if (b->flags & EMBI_BF_IFC) {
        embi_lay_ifc_intrinsic(c, b, &a, &z);
    } else {
        /* Floats that follow each other sit side by side when nothing limits the width, so their
         * max-content widths add up (CSS 2.1 section 10.3.5 with unbounded available width); a clear or a
         * box in the normal flow ends the row. Their min-content widths still stack. */
        float frow = 0;
        for (embi_box *k = b->first; k != nullptr; k = k->next) {
            if (k->flags & (EMBI_BF_ABSPOS)) {
                continue;
            }
            float cmn = 0, cmx = 0;
            embi_lay_intrinsic_outer(c, k, &cmn, &cmx);
            a = max2(a, cmn);
            if ((k->flags & EMBI_BF_FLOAT) != 0) {
                const embi_value *clr = embi_lay_prop(c, k, EMBI_PROP_clear);
                if (clr != nullptr && clr->kind == EMBI_V_KEYWORD && clr->kw != EMBI_KW_none) {
                    z = max2(z, frow);
                    frow = 0;
                }
                frow += cmx;
            } else {
                z = max2(z, frow);
                frow = 0;
                z = max2(z, cmx);
            }
        }
        z = max2(z, frow);
    }
    if ((b->tflags & EMBI_TF_MC) != 0 && !inline_size_contained(c, b)) {
        embi_lay_mc_intrinsic(c, b, &a, &z);
    }
    embi_lay_ih_pop(c, ih);
    bool used = c->ih_need != need0;
    c->ih_need = need0 + (used && dep ? 1u : 0u);
    if (!(used && dep)) {
        b->min_c = a;
        b->max_c = z;
        b->cq_w = kw;
        b->cq_h = kh;
        b->flags |= EMBI_BF_INTR;
    }
    *mn = a;
    *mx = z;
}

/* A min-width or max-width as a content-box length for intrinsic contributions (CSS Sizing 3
 * section 5.1): percentages behave as auto (min) or none (max), as do keywords. NaN for those. */
/* fit-content(<length>) as an intrinsic contribution (CSS Sizing 3 section 5.2.2): min(max-content, max(min-content,
 * length)) of the box's content, in both modes. NaN when v is not that function with a length (a percentage is
 * cyclic and behaves as auto, or none for a maximum). */
static float contribution_fit(const embi_lctx *c, const embi_box *b, const embi_value *v, float edges,
                              bool border_box, float imn, float imx, float pct_arg)
{
    if (v == nullptr || v->kind != EMBI_V_FUNC || v->func.n < 1 || v->func.name == 0) {
        return NAN;
    }
    embi_str nm = embi_atom_str(v->func.name);
    if (nm.len != 11 || memcmp(nm.ptr, "fit-content", 11) != 0) {
        return NAN;
    }
    float arg;
    if (v->func.args[0].kind == EMBI_V_PERCENT) {
        if (isnan(pct_arg)) {
            return NAN;
        }
        arg = pct_arg; /* cyclic: the property behaves as its initial value (0 for a minimum, infinity for a maximum) */
        border_box = false;
    } else {
        arg = embi_lay_len(c, b, &v->func.args[0], NAN);
    }
    if (isnan(arg)) {
        return NAN;
    }
    arg = clamp0(border_box ? arg - edges : arg);
    float r = arg < imn ? imn : arg;
    return r > imx ? imx : r;
}

static float contribution_limit(const embi_lctx *c, const embi_box *b, embi_prop_id p, float edges,
                                bool border_box, float imn, float imx)
{
    const embi_value *v = embi_lay_prop(c, b, p);
    if (v != nullptr && v->kind == EMBI_V_FUNC) {
        return contribution_fit(c, b, v, edges, border_box, imn, imx, p == EMBI_PROP_min_width ? 0.0f : INFINITY);
    }
    if (v != nullptr && v->kind == EMBI_V_KEYWORD) {
        /* The content keywords limit the contribution by the box's own content sizes (CSS Sizing 3 section 5.2;
         * flex-item-content-is-min-width-max-content). fit-content is min-content here: the available space of an
         * intrinsic size is zero for the minimum and infinite for the maximum. */
        switch (v->kw) {
        case EMBI_KW_min_content:
            return imn;
        case EMBI_KW_max_content:
            return imx;
        case EMBI_KW_fit_content:
            return p == EMBI_PROP_min_width ? imn : imx;
        default:
            return NAN;
        }
    }
    if (v == nullptr || v->kind == EMBI_V_PERCENT) {
        return NAN;
    }
    /* A percentage inside calc() of a minimum counts 0 against an indefinite size (it is not "none"). */
    float len = embi_lay_len(c, b, v, p == EMBI_PROP_min_width && v->kind == EMBI_V_CALC ? 0.0f : NAN);
    if (isnan(len)) {
        return NAN;
    }
    return clamp0(border_box ? len - edges : len);
}

void embi_lay_intrinsic_outer(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, b, 0, &bdr, &pad, &mar);
    float ml = isnan(mar.l) ? 0 : mar.l;
    float mr = isnan(mar.r) ? 0 : mar.r;
    float edges = bdr.l + bdr.r + pad.l + pad.r;
    float outer = edges + ml + mr;
    float cmn = 0, cmx = 0;
    embi_lay_intrinsic(c, b, &cmn, &cmx);
    float imn = cmn, imx = cmx; /* the content's own sizes, before width overrides them */

    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_width);
    bool box_sizing_border = false;
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    if (bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box) {
        box_sizing_border = true;
    }
    if (!embi_lay_is_auto(wv) && wv->kind != EMBI_V_KEYWORD) {
        float w = wv->kind == EMBI_V_FUNC ? contribution_fit(c, b, wv, edges, box_sizing_border, imn, imx, NAN)
                                          : embi_lay_len(c, b, wv, NAN);
        if (wv->kind == EMBI_V_FUNC && !isnan(w)) {
            w += box_sizing_border ? edges : 0; /* content below, so the border-box form of the code that follows */
        }
        if (!isnan(w)) {
            float content = box_sizing_border ? w - edges : w;
            content = clamp0(content);
            cmn = content;
            cmx = content;
        }
    }
    else if (embi_lay_is_auto(wv) || (wv != nullptr && wv->kind == EMBI_V_KEYWORD &&
                                      (wv->kw == EMBI_KW_min_content || wv->kw == EMBI_KW_max_content ||
                                       wv->kw == EMBI_KW_fit_content))) {
        /* An auto (or content keyword) width with a preferred aspect ratio and a definite height: the width transfers
         * from the height (CSS Sizing 4 section 5.1), and that is the contribution in both modes. The height is
         * clamped by min-height and max-height first. A replaced box with an auto width has its own rule. */
        float ratio = 0;
        const embi_value *hv = embi_lay_prop(c, b, EMBI_PROP_height);
        bool repl = (b->flags & EMBI_BF_REPLACED) != 0 || b->kind == EMBI_BK_REPLACED;
        if ((!repl || !embi_lay_is_auto(wv)) && embi_lay_ratio(c, b, &ratio) && ratio > 0 && !embi_lay_is_auto(hv) &&
            hv->kind != EMBI_V_KEYWORD) {
            /* A percentage height resolves against the basis of intrinsic sizing (aspect-ratio/intrinsic-size-006). */
            if (pct_height(hv)) {
                c->ih_need++;
            }
            float h = embi_lay_len(c, b, hv, embi_lay_ih_get(c));
            if (!isnan(h)) {
                float vedges = bdr.t + bdr.b + pad.t + pad.b;
                bool ratio_bb = box_sizing_border && !embi_lay_ratio_content(c, b);
                float content_h = clamp0(box_sizing_border ? h - vedges : h);
                const embi_value *mnh = embi_lay_prop(c, b, EMBI_PROP_min_height);
                const embi_value *mxh = embi_lay_prop(c, b, EMBI_PROP_max_height);
                if (mxh != nullptr && mxh->kind != EMBI_V_KEYWORD) {
                    c->ih_need += pct_height(mxh) ? 1u : 0u;
                    float v = embi_lay_len(c, b, mxh, embi_lay_ih_get(c));
                    if (!isnan(v)) {
                        content_h = fminf(content_h, clamp0(box_sizing_border ? v - vedges : v));
                    }
                }
                if (mnh != nullptr && !embi_lay_is_auto(mnh) && mnh->kind != EMBI_V_KEYWORD) {
                    c->ih_need += pct_height(mnh) ? 1u : 0u;
                    float v = embi_lay_len(c, b, mnh, embi_lay_ih_get(c));
                    if (!isnan(v)) {
                        content_h = fmaxf(content_h, clamp0(box_sizing_border ? v - vedges : v));
                    }
                }
                float w = ratio_bb ? (content_h + vedges) * ratio - edges : content_h * ratio;
                cmn = clamp0(w);
                cmx = cmn;
            }
        }
    }
    /* The contribution is clamped by min-width and max-width; the minimum wins. */
    float maxw = contribution_limit(c, b, EMBI_PROP_max_width, edges, box_sizing_border, imn, imx);
    float minw = contribution_limit(c, b, EMBI_PROP_min_width, edges, box_sizing_border, imn, imx);
    if (!isnan(maxw)) {
        if (embi_lay_is_table(b)) {
            /* A table's max-width caps its max-content contribution, never below its min-content
             * width (CSS Tables 3 section 3.5). */
            cmx = fmaxf(fminf(cmx, maxw), cmn);
        } else {
            cmn = fminf(cmn, maxw);
            cmx = fminf(cmx, maxw);
        }
    }
    if (!isnan(minw)) {
        cmn = fmaxf(cmn, minw);
        cmx = fmaxf(cmx, minw);
    }
    *mn = cmn + outer;
    *mx = cmx + outer;
}

/* ------------------------------------------------------------------ replaced boxes */

/* Resolves the horizontal margins of a box of border-box width bw in an available width avail
 * (CSS 2.1 section 10.3.2 and 10.3.4 for replaced and block boxes). ml and mr are in and out. */
void embi_lay_hmargins(float avail, float bw, float *ml, float *mr, bool rtl)
{
    bool la = isnan(*ml);
    bool ra = isnan(*mr);
    float left = la ? 0 : *ml;
    float right = ra ? 0 : *mr;
    float rest = avail - bw - left - right;
    if (la && ra) {
        if (rest < 0) {
            left = rtl ? rest : 0;
            right = rtl ? 0 : rest;
        } else {
            left = rest / 2;
            right = rest / 2;
        }
    } else if (la) {
        if (rest < 0 && !rtl) {
            /* An auto margin in an over-constrained box is zero, and the end margin (the right one, in ltr) is the
             * value that gives way (CSS 2.1 10.3.3; margin-auto-on-block-box). */
            left = 0;
            right = avail - bw;
        } else {
            left = rest;
        }
    } else if (ra) {
        if (rest < 0 && rtl) {
            left = avail - bw;
            right = 0;
        } else {
            right = rest;
        }
    } else if (rtl) {
        left += rest;
    } else {
        right += rest;
    }
    *ml = left;
    *mr = right;
}

bool embi_lay_cb_rtl(const embi_lctx *c, const embi_box *b)
{
    const embi_box *p = b->parent != nullptr ? b->parent : b;
    if ((b->flags & EMBI_BF_ABSPOS) != 0) {
        /* An out-of-flow box takes the direction of its containing block (W3d); the initial containing
         * block takes the root's. */
        const embi_box *cb = embi_lay_abspos_cb(c, b);
        if (cb == nullptr) {
            p = b;
            while (p->parent != nullptr) {
                p = p->parent;
            }
        } else {
            p = cb;
        }
    }
    const embi_value *d = embi_lay_prop(c, p, EMBI_PROP_direction);
    return d != nullptr && d->kind == EMBI_V_KEYWORD && d->kw == EMBI_KW_rtl;
}

/* A width or height property as a content-box length, NaN for auto and keywords. A calc-size() with a length
 * basis applies its calc-sum to the basis (the auto basis is applied by the caller to the used size). */
static float replaced_spec(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct,
                           float edges, bool border_box)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, b, p, &cx);
    if (embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD) {
        return NAN;
    }
    float len = embi_lay_len(c, b, v, pct);
    if (isnan(len)) {
        return NAN;
    }
    if (cx != nullptr) {
        float r = embi_lay_size_eval(c, b, cx, len, pct);
        if (!isnan(r)) {
            len = r;
        }
    }
    return clamp0(border_box ? len - edges : len);
}

/* The auto basis of a calc-size() on a width or height of a replaced box: the calc-sum sees the used size
 * (content box w) as `size`. Returns w when the property has no calc-size() with an auto basis. */
static float replaced_auto_calc(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct, float edges,
                                bool border_box, float w)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, b, p, &cx);
    if (cx == nullptr || !(v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_auto)) {
        return w;
    }
    float r = embi_lay_size_eval(c, b, cx, border_box ? w + edges : w, pct);
    return isnan(r) ? w : clamp0(border_box ? r - edges : r);
}

/* A min or max property as a content-box length. dflt when auto, none or a keyword. A calc-size() applies its
 * calc-sum to the length basis; with an auto basis the minimum sees 0 (as for a block) and the maximum sees
 * auto_basis (the used size of the side, content box; NaN: none). */
static float replaced_limit(const embi_lctx *c, const embi_box *b, embi_prop_id p, float pct,
                            float edges, bool border_box, float dflt, float auto_basis)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(c, b, p, &cx);
    if (v == nullptr) {
        return dflt;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        if (cx == nullptr || v->kw != EMBI_KW_auto || isnan(auto_basis)) {
            return dflt;
        }
        float r = embi_lay_size_eval(c, b, cx, border_box ? auto_basis + edges : auto_basis, pct);
        return isnan(r) ? dflt : clamp0(border_box ? r - edges : r);
    }
    float len = embi_lay_len(c, b, v, pct);
    if (isnan(len)) {
        return dflt;
    }
    if (cx != nullptr) {
        float r = embi_lay_size_eval(c, b, cx, len, pct);
        if (!isnan(r)) {
            len = r;
        }
    }
    return clamp0(border_box ? len - edges : len);
}

/* The min and max constraints of a replaced box (CSS 2.1 section 10.4 table). With a ratio the
 * violated constraint is transferred to the other side through the ratio; without one each side is
 * clamped on its own. */
static void replaced_clamp(float *w, float *h, float minw, float maxw, float minh, float maxh,
                           bool ratio_ok, float ratio)
{
    float W = *w, H = *h;
    if (ratio_ok && ratio > 0.0f && !(W > 0) && !(H > 0)) {
        /* A zero size with a ratio (width: 0 and height: auto): the table works with the ratio alone, so a minimum
         * on one side moves the other (CSS 2.1 section 10.4 and 10.3.2). */
        W = ratio * 1e-3f;
        H = 1e-3f;
    }
    if (!ratio_ok || !(W > 0) || !(H > 0)) {
        *w = W > maxw ? maxw : (W < minw ? minw : W);
        *h = H > maxh ? maxh : (H < minh ? minh : H);
        return;
    }
    bool wmax = W > maxw, wmin = W < minw, hmax = H > maxh, hmin = H < minh;
    (void)ratio;
    if (wmax && hmax) {
        if (maxw / W <= maxh / H) {
            *w = maxw;
            *h = max2(minh, maxw * H / W);
        } else {
            *h = maxh;
            *w = max2(minw, maxh * W / H);
        }
    } else if (wmin && hmin) {
        if (minw / W <= minh / H) {
            *w = fminf(maxw, minh * W / H);
            *h = minh;
        } else {
            *w = minw;
            *h = fminf(maxh, minw * H / W);
        }
    } else if (wmax && hmin) {
        *w = maxw;
        *h = minh;
    } else if (wmin && hmax) {
        *w = minw;
        *h = maxh;
    } else if (wmax) {
        *w = maxw;
        *h = max2(minh, maxw * H / W);
    } else if (wmin) {
        *w = minw;
        *h = fminf(maxh, minw * H / W);
    } else if (hmax) {
        *h = maxh;
        *w = max2(minw, maxh * W / H);
    } else if (hmin) {
        *h = minh;
        *w = fminf(maxw, minh * W / H);
    }
}

/* ------------------------------------------------------------------ R4c: foreignObject boxes */

/* Set at every document creation, possibly from several threads: stored atomically. */
static _Atomic(embi_layout_svg_fo_fn) g_svg_fo_fn;

void embi_layout_set_svg_fo(embi_layout_svg_fo_fn fn)
{
    atomic_store_explicit(&g_svg_fo_fn, fn, memory_order_release);
}

/* A plain number attribute (optionally px), the fallback of a missing hook. */
static float fo_attr(embi_element *el, const char *name)
{
    const embi_attr *a = embi_el_get_attribute(el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr) {
        return 0.0f;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    float r = 0.0f;
    if (sb.len > 0 && sb.len < 64) {
        char tmp[64];
        memcpy(tmp, sb.data, sb.len);
        tmp[sb.len] = '\0';
        double v = SDL_strtod(tmp, nullptr);
        r = isfinite(v) ? (float)v : 0.0f;
    }
    embi_sb_free(&sb);
    return r;
}

/* Lays out the foreignObject boxes of the svg root b (its fragment f, content box cw x ch at (cx, cy) in f) at their sizes,
 * and appends their fragments to f where their origins land. Each box is a formatting context of its own: nothing outside
 * it depends on its content. */
static void svg_fo_layout(embi_lctx *c, embi_box *b, embi_frag *f, float cx, float cy, float cw, float ch)
{
    embi_layout_svg_fo_fn fn = atomic_load_explicit(&g_svg_fo_fn, memory_order_acquire);
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (embi_box_is_text(k) || (k->flags2 & EMBI_BF2_SVGFO) == 0 || k->el == nullptr) {
            continue;
        }
        embi_lay_clear_frags(c, k);
        float g[4] = {0, 0, 0, 0};
        bool ok;
        if (fn != nullptr) {
            ok = fn(c->style, b->el, k->el, cw, ch, g);
        } else {
            g[0] = fo_attr(k->el, "x");
            g[1] = fo_attr(k->el, "y");
            g[2] = fo_attr(k->el, "width");
            g[3] = fo_attr(k->el, "height");
            ok = true;
        }
        if (!ok || !(g[2] > 0.0f) || !(g[3] > 0.0f) || !isfinite(g[0]) || !isfinite(g[1]) || !(g[2] < 1e7f) ||
            !(g[3] < 1e7f)) {
            continue; /* no fragment: the painter and the hit test skip it */
        }
        embi_box *save_box = c->force_box;
        float save_w = c->force_w, save_h = c->force_h;
        bool save_ws = c->force_wset, save_hs = c->force_hset, save_hn = c->force_hnone;
        bool save_nm = c->force_nominmaxw, save_soft = c->force_hsoft;
        c->force_box = k;
        c->force_wset = true;
        c->force_hset = true;
        c->force_w = g[2];
        c->force_h = g[3];
        c->force_hnone = false;
        c->force_nominmaxw = true;
        c->force_hsoft = false;
        embi_cs ks;
        memset(&ks, 0, sizeof ks);
        ks.avail_w = g[2];
        ks.cb_w = g[2];
        ks.cb_h = g[3];
        ks.fit = EMBI_FIT_FILL;
        embi_lres r;
        memset(&r, 0, sizeof r);
        c->via_flow = false;
        embi_lay_box(c, k, &ks, &r);
        c->force_box = save_box;
        c->force_w = save_w;
        c->force_h = save_h;
        c->force_wset = save_ws;
        c->force_hset = save_hs;
        c->force_hnone = save_hn;
        c->force_nominmaxw = save_nm;
        c->force_hsoft = save_soft;
        if (r.frag != nullptr) {
            r.frag->x = cx + g[0];
            r.frag->y = cy + g[1];
            embi_lay_frag_append(f, r.frag);
        }
    }
}

void embi_lay_replaced(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, b, cs->cb_w, &bdr, &pad, &mar);
    float edges_h = bdr.l + bdr.r + pad.l + pad.r;
    float edges_v = bdr.t + bdr.b + pad.t + pad.b;
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    bool border_box = bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;

    float rw = b->rw > 0 ? b->rw : 0;
    float rh = b->rh > 0 ? b->rh : 0;
    if ((b->flags2 & EMBI_BF2_CONTROL) != 0) {
        rw = control_content(c, b, rw, false);
        rh = control_content(c, b, rh, true);
    }
    (void)replaced_contained(c, b, &rw, &rh);
    float ratio = 0;
    bool natural_r = false;
    bool has_ratio = ratio_src(c, b, &ratio, &natural_r);
    bool rtl = embi_lay_cb_rtl(c, b);
    /* A ratio from the aspect-ratio property relates the boxes box-sizing selects: with border-box
     * sizing it relates border boxes, so the paddings and borders convert between the axes. */
    bool bb_ratio = has_ratio && !natural_r && border_box;
    float xh = bb_ratio ? edges_h : 0, xv = bb_ratio ? edges_v : 0;

    float w = replaced_spec(c, b, EMBI_PROP_width, cs->cb_w, edges_h, border_box);
    float h = replaced_spec(c, b, EMBI_PROP_height, cs->cb_h, edges_v, border_box);
    /* A flex item's used main or cross size is final (W3e), so the other side follows through the
     * ratio below. */
    if (c->force_box == b && c->force_hnone) {
        h = NAN; /* measured for its content height: its own height does not count */
    }
    if (c->force_box == b && c->force_wset) {
        w = clamp0(c->force_w - edges_h);
    }
    if (c->force_box == b && c->force_hset) {
        h = clamp0(c->force_h - edges_v);
    }
    if ((b->flags2 & EMBI_BF2_NOSIZE) != 0 && isnan(w) && isnan(h) && isfinite(cs->avail_w) && cs->avail_w > 0 &&
        !(c->force_box == b && (c->force_wset || c->force_hset || c->force_hnone))) {
        /* a ratio without a natural size: the width is what is available, the height follows */
        float ml0 = isnan(mar.l) ? 0.0f : mar.l, mr0 = isnan(mar.r) ? 0.0f : mar.r;
        w = clamp0(cs->avail_w - ml0 - mr0 - edges_h);
        h = (w + xh) / ratio - xv;
    } else if (!isnan(w) && !isnan(h)) {
        /* both given */
    } else if (has_ratio) {
        if (!isnan(w)) {
            h = (w + xh) / ratio - xv;
        } else if (!isnan(h)) {
            w = (h + xv) * ratio - xh;
        } else if (rw > 0) {
            w = rw;
            h = (w + xh) / ratio - xv;
        } else if (rh > 0) {
            h = rh;
            w = (h + xv) * ratio - xh;
        } else {
            w = 0;
            h = 0;
        }
    } else {
        if (isnan(w)) {
            w = rw;
        }
        if (isnan(h)) {
            h = rh;
        }
    }
    w = clamp0(w);
    h = clamp0(h);
    if (!(c->force_box == b && c->force_wset)) {
        w = replaced_auto_calc(c, b, EMBI_PROP_width, cs->cb_w, edges_h, border_box, w);
    }
    if (!(c->force_box == b && c->force_hset)) {
        h = replaced_auto_calc(c, b, EMBI_PROP_height, cs->cb_h, edges_v, border_box, h);
    }

    /* min and max constraints (CSS 2.1 section 10.4). */
    bool nomm = c->force_box == b && c->force_nominmaxw;
    float minw = nomm ? 0 : replaced_limit(c, b, EMBI_PROP_min_width, cs->cb_w, edges_h, border_box, 0, 0.0f);
    float maxw = nomm ? INFINITY
                      : replaced_limit(c, b, EMBI_PROP_max_width, cs->cb_w, edges_h, border_box, INFINITY, w);
    float minh = replaced_limit(c, b, EMBI_PROP_min_height, cs->cb_h, edges_v, border_box, 0, 0.0f);
    float maxh = replaced_limit(c, b, EMBI_PROP_max_height, cs->cb_h, edges_v, border_box, INFINITY, h);
    if (maxw < minw) {
        maxw = minw;
    }
    if (maxh < minh) {
        maxh = minh;
    }
    /* A forced side (a flex item's used main or cross size, W3e) is final: the min and max limits of
     * the other side apply alone, without the ratio moving the forced side (CSS Flexbox 1 section 9.8
     * and Chromium: an image with a forced width and a max-height keeps its width). */
    bool fw = c->force_box == b && c->force_wset;
    bool fh = c->force_box == b && c->force_hset;
    if (fw || fh) {
        if (!fh) {
            h = h > maxh ? maxh : (h < minh ? minh : h);
        }
        if (!fw) {
            w = w > maxw ? maxw : (w < minw ? minw : w);
        }
    } else {
        replaced_clamp(&w, &h, minw, maxw, minh, maxh, has_ratio, ratio);
    }
    w = clamp0(w);
    h = clamp0(h);

    embi_frag *f = embi_lay_new_frag(c, b, EMBI_FK_BOX);
    float ml = mar.l, mr = mar.r;
    float bwb = w + edges_h;
    if ((cs->fit != EMBI_FIT_SHRINK || (b->flags & EMBI_BF_INLINE_LEVEL) == 0) && (b->flags & EMBI_BF_FLOAT) == 0) {
        /* (a floated replaced box has no free width to share: its auto margins are 0, CSS 2.1 10.3.5) */
        embi_lay_hmargins(cs->avail_w, bwb, &ml, &mr, rtl);
    } else {
        if (isnan(ml)) {
            ml = 0;
        }
        if (isnan(mr)) {
            mr = 0;
        }
    }
    f->w = bwb;
    f->h = h + edges_v;
    embi_lay_frag_set_edges(c->ld, f, &bdr, &pad);
    float mt0 = isnan(mar.t) ? 0 : mar.t;
    float mb0 = isnan(mar.b) ? 0 : mar.b;
    embi_lay_frag_set_mg(c->ld, f, ml, mt0, mr, mb0);
    embi_fo(f)->ov[0] = 0;
    embi_fo(f)->ov[1] = 0;
    embi_fo(f)->ov[2] = f->w;
    embi_fo(f)->ov[3] = f->h;
    f->bl_first = NAN;
    embi_fl(f)->bl_last = NAN;
    if ((b->tflags & EMBI_TF_CTLBASE) != 0 && b->style != nullptr) {
        /* The baseline of a text field or a drop-down select is its text's: one line, centred in the content box. */
        float la, ld;
        embi_lay_line_metrics(c, b->style, &la, &ld);
        float bl = bdr.t + pad.t + (h - (la + ld)) * 0.5f + la;
        f->bl_first = bl;
        embi_fl(f)->bl_last = bl;
    }
    if (b->first != nullptr) {
        svg_fo_layout(c, b, f, bdr.l + pad.l, bdr.t + pad.t, w, h);
    }
    out->frag = f;
    out->ml = ml;
    out->mr = mr;
    out->mt = mt0;
    out->mb = mb0;
    out->mt_s = (embi_strut){mt0 > 0 ? mt0 : 0, mt0 < 0 ? mt0 : 0};
    out->mb_s = (embi_strut){mb0 > 0 ? mb0 : 0, mb0 < 0 ? mb0 : 0};
    out->selfc = false;
}
