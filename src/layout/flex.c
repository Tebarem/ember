/*
 * Flex containers (CSS Flexbox 1 section 9, W3e section 7). The container is a block box whose
 * content is laid out here: block_pass (block.c) sizes the container and calls embi_flex_content
 * with the content box geometry, which places the items into the fragment of the container.
 *
 * The algorithm follows section 9 in order:
 *   - the items are the in-flow children, sorted by order (stable, DOM order for ties); abspos
 *     children are placed after the layout, at the static position of the container's content;
 *   - each item's flex base size (flex-basis, or its width or height, or its content size), its
 *     min and max sizes (border-box, with the automatic minimum of section 4.5), and its hypothetical
 *     main size (the base size clamped);
 *   - the items are collected into lines (flex-wrap; a multi-line container with a definite main
 *     size breaks lines where the outer hypothetical sizes overflow);
 *   - per line, the flexible lengths are resolved (section 9.7, with the frozen items and the
 *     min and max violations), giving each item its target main size;
 *   - each item is laid out at its target main size (its cross size is its own, or its line's
 *     when it stretches); the line cross sizes follow from the items (or from the container's
 *     definite cross size), align-content distributes the free cross space, and the items are
 *     positioned along the main axis by justify-content and auto margins, and along the cross axis
 *     by align-self (stretch, start, end, center, baseline);
 *   - the baselines of the container come from its first and last items.
 *
 * Coordinates. The layout works in logical main and cross coordinates, starting at the main-start
 * and cross-start edges. flip_main and cflip say when a logical start is the physical end (row-reverse
 * and column-reverse, rtl rows, wrap-reverse), and the item positions are mirrored at the end.
 * Sizes are border-box sizes everywhere in this file, so the sizes forced on an item (fw, fh of
 * embi_cs) are border boxes too.
 *
 * Intrinsic sizes (embi_flex_intrinsic) follow CSS Flexbox 1 section 9.9 as Chromium does where the spec is
 * underspecified: a row container sums the items' flexed contributions (an item that cannot shrink
 * contributes its base size, one that can shrink its min-content or max-content contribution), a wrapping
 * row takes the largest min-content contribution, and a column container takes the widest item per line
 * and sums the lines when it wraps at a definite height.
 *
 * visibility: collapse items (section 4.4) are struts: zero main size and zero main margins, but they
 * still size their line along the cross axis (their cross size comes from a layout at their
 * hypothetical main size); they are not placed in the fragment tree. They take no gaps (line_gaps).
 *
 * Deviations. The automatic minimum of a column item uses its
 * height at its natural width. Intrinsic sizes do not know an item's stretched cross size, except
 * through the measured layout of items that have a replaced or aspect-ratio descendant with a
 * percentage height (definite_content_width).
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

/* The alignment of a box along the cross axis, from its align-self (or align-items). */
enum {
    AL_START,
    AL_END,
    AL_CENTER,
    AL_STRETCH,
    AL_BASELINE,
    AL_LBASELINE,
};

/* An alignment value: the position keyword and the baseline flags. */
typedef struct align_v {
    uint16_t pos;  /* the position keyword, 0 when none */
    bool baseline; /* a baseline value (baseline, first baseline, last baseline) */
    bool last;     /* last baseline */
    bool safe;     /* the safe overflow keyword: an overflowing box aligns to the start edge */
} align_v;

/* One item of the flex container (in order, after sorting). */
typedef struct fitem {
    embi_box *box;
    int32_t order;
    float grow, shrink;
    float bdr_main, bdr_cross;      /* border and padding along the main and cross axes */
    float spec_main, spec_cross;    /* border-box sizes when definite, else NaN */
    float scross;                   /* spec_cross, or the stretched cross size when definite, else NaN */
    bool cross_auto;                /* the cross size property is auto (only auto items stretch) */
    const embi_value *cross_expr;   /* the calc-sum of calc-size(auto, ...) on the cross size, else nullptr */
    bool bb;                        /* box-sizing: border-box */
    float min_main, max_main;       /* border-box limits along the main axis */
    float min_cross, max_cross;     /* border-box limits along the cross axis */
    bool auto_min;                  /* min-main is auto (the automatic minimum applies) */
    float basis;                    /* flex base size, border-box */
    float hyp;                      /* hypothetical main size, border-box, clamped */
    float nat;                      /* natural height of a column item at its natural width, or NaN */
    float nat_w;                    /* max-content width of a row item, or NaN */
    bool ratio_box;                 /* a replaced box or one with an aspect ratio (natural sizes) */
    bool basis_auto;                /* flex-basis is auto (the main size comes from the content) */
    bool main_def;                  /* the flexed main size is definite even in an indefinite container (lay_item_mode) */
    float ratio;                    /* the preferred aspect ratio (width / height), 0 when none */
    bool stretched;                 /* stretched to the line: definite, so it is laid out again */
    float target;                   /* target main size, border-box */
    float mms, mme, cms, cme;       /* the margins at the logical start and end, auto as 0 */
    bool mas, mae, cas, cae;        /* the margin is auto */
    float cross;                    /* used cross size, border-box */
    int cal;                        /* AL_* along the cross axis */
    bool frozen;
    bool collapsed;                 /* visibility: collapse, a strut */
    float coll_main;                /* the main size a strut is measured at for its cross size */
    float strut;                    /* a strut's cross size: its line's in a layout where it is not collapsed, or NaN */
    embi_lres lr;                   /* the layout at the used sizes */
    float mpos;                     /* logical start of the margin box along the main axis */
    float cpos;                     /* logical start of the margin box within its line */
    float lstart;                   /* logical start of the line along the cross axis */
} fitem;

/* The state of one flex layout. */
typedef struct fctx {
    embi_lctx *c;
    embi_box *b;
    embi_frag *f;
    bool row;      /* the main axis is horizontal */
    bool rtl;      /* the container's direction is rtl */
    bool wrapping; /* flex-wrap is wrap or wrap-reverse */
    bool reverse;  /* row-reverse or column-reverse */
    bool wrap_rev; /* wrap-reverse */
    bool flip_main;
    bool cflip;
    float cw;      /* the content width */
    float chd;     /* the content height when definite, else NaN */
    float minh, maxh;
    float ox, oy;  /* the content box origin in the container's border box */
    float gap_m, gap_c;
    align_v ai;    /* align-items */
    bool no_collapse; /* item_prepare treats visibility: collapse items as ordinary ones (strut_sizes) */
} fctx;

struct fctx;
static int cal_for(const struct fctx *x, const embi_box *k, align_v a);

static bool is_collapsed(embi_lctx *c, embi_box *k)
{
    const embi_value *v = embi_lay_prop(c, k, EMBI_PROP_visibility);
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_collapse;
}

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

static uint16_t kw_of(const embi_value *v)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* An alignment value: a keyword, a list with a position and a baseline, or nothing. */
static align_v align_of(const embi_value *v)
{
    align_v a = {0, false, false, false};
    if (v == nullptr) {
        return a;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        a.pos = v->kw;
    } else if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            const embi_value *k = &v->list.items[i];
            if (k->kind != EMBI_V_KEYWORD || k->kw == EMBI_KW_first) {
                continue;
            }
            if (k->kw == EMBI_KW_last) {
                a.last = true;
                continue;
            }
            if (k->kw == EMBI_KW_safe || k->kw == EMBI_KW_unsafe) {
                a.safe = k->kw == EMBI_KW_safe;
                continue;
            }
            a.pos = k->kw;
        }
    }
    if (a.pos == EMBI_KW_baseline) {
        a.baseline = true;
    }
    return a;
}

/* The cross alignment that an alignment value gives a flex item. normal and stretch stretch the
 * item when its cross size is auto. */
static int cal_of(align_v a)
{
    if (a.baseline && a.last) {
        /* last baseline: the last baselines of the items line up, flush with the cross end. */
        return AL_LBASELINE;
    }
    if (a.baseline) {
        return AL_BASELINE;
    }
    switch (a.pos) {
    case EMBI_KW_flex_end:
    case EMBI_KW_end:
    case EMBI_KW_self_end:
        return AL_END;
    case EMBI_KW_center:
        return AL_CENTER;
    case EMBI_KW_flex_start:
    case EMBI_KW_start:
    case EMBI_KW_self_start:
    case EMBI_KW_left:
    case EMBI_KW_right:
        return AL_START;
    default:
        return AL_STRETCH;
    }
}

/* calc-size(<keyword>, <calc>) on a width property (CSS Values 5 section 10.1): the min-content, max-content or
 * fit-content size of the item (in the box-sizing space) is the basis, so the property is a definite length. NaN for
 * the auto basis and for the block-axis properties, where those keywords behave as auto. */
static float size_bb_kw(const fctx *x, embi_box *k, embi_prop_id p, uint16_t kw, const embi_value *cx, float pct,
                        float edges, bool bb)
{
    if (p != EMBI_PROP_width && p != EMBI_PROP_min_width && p != EMBI_PROP_max_width) {
        return NAN;
    }
    float mn = 0, mx = 0;
    float basis;
    switch (kw) {
    case EMBI_KW_min_content:
        embi_lay_intrinsic(x->c, k, &mn, &mx);
        basis = mn;
        break;
    case EMBI_KW_max_content:
        embi_lay_intrinsic(x->c, k, &mn, &mx);
        basis = mx;
        break;
    case EMBI_KW_fit_content: {
        embi_lay_intrinsic(x->c, k, &mn, &mx);
        float avail = isnan(x->cw) ? mx : x->cw - edges;
        basis = mx < (mn > avail ? mn : avail) ? mx : (mn > avail ? mn : avail);
        break;
    }
    default:
        return NAN;
    }
    float r = embi_lay_size_eval(x->c, k, cx, bb ? basis + edges : basis, pct);
    if (isnan(r)) {
        return NAN;
    }
    return clamp0(bb ? r : r + edges);
}

/* A length property of a sizing box as a border-box length (CSS Sizing 3: box-sizing selects
 * whether the value is a content or a border size). NaN for auto, keywords and percentages of an
 * indefinite base. */
static float size_bb(const fctx *x, embi_box *k, embi_prop_id p, float pct, float edges, bool bb)
{
    const embi_value *cx;
    const embi_value *v = embi_lay_prop_calc(x->c, k, p, &cx);
    if (v == nullptr) {
        return NAN;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        if (cx == nullptr) {
            return NAN;
        }
        return size_bb_kw(x, k, p, v->kw, cx, pct, edges, bb);
    }
    float len = embi_lay_len(x->c, k, v, pct);
    if (isnan(len)) {
        return NAN;
    }
    /* calc-size(<basis>, <calc>) on the property: the basis is the length just read (in the box-sizing space). */
    if (cx != nullptr) {
        float r = embi_lay_size_eval(x->c, k, cx, len, pct);
        if (!isnan(r)) {
            len = r;
        }
    }
    return clamp0(bb ? len : len + edges);
}

static bool is_box_sizing_bb(const fctx *x, embi_box *k)
{
    const embi_value *bs = embi_lay_prop(x->c, k, EMBI_PROP_box_sizing);
    return bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
}

/* A number property (flex-grow, flex-shrink), or the default. */
static float num_of(const embi_value *v, float dflt)
{
    if (v == nullptr || (v->kind != EMBI_V_NUMBER && v->kind != EMBI_V_INTEGER)) {
        return dflt;
    }
    float n = (float)v->num;
    return isnan(n) || n < 0 ? 0.0f : n;
}

/* An HTML fieldset is a flex container through its anonymous content box, which has no min-height of
 * its own: with an indefinite height, its main size is the sum of the items (Chromium and the
 * fieldset rules of the HTML spec; the test fieldset-as-container-justify-center). */
static bool is_fieldset(const embi_box *b)
{
    if (b->el == nullptr || b->el->base.extra != EMBI_NS_HTML) {
        return false;
    }
    embi_str s = embi_atom_str(b->el->base.name);
    return s.len == 8 && memcmp(s.ptr, "fieldset", 8) == 0;
}

/* An input that is a push button (button, submit, reset): its min-content size is its label, so it is not
 * a compressible replaced element. */
static bool is_button_input(const embi_box *b)
{
    if (b->el == nullptr || b->el->base.extra != EMBI_NS_HTML) {
        return false;
    }
    embi_str nm = embi_atom_str(b->el->base.name);
    if (nm.len != 5 || memcmp(nm.ptr, "input", 5) != 0) {
        return false;
    }
    const embi_attr *a = embi_el_get_attribute(b->el, EMBI_STR("type"));
    if (a == nullptr) {
        return false;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    bool r = (sb.len == 6 && (SDL_strncasecmp(sb.data, "button", 6) == 0 ||
                              SDL_strncasecmp(sb.data, "submit", 6) == 0)) ||
             (sb.len == 5 && SDL_strncasecmp(sb.data, "reset", 5) == 0);
    embi_sb_free(&sb);
    return r;
}

/* True when the item scrolls along the main axis (overflow hidden, scroll or auto there): its
 * automatic minimum size is then zero (CSS Flexbox 1 section 4.5). overflow: clip does not count. */
static bool scrolls_in_main(const fctx *x, const embi_box *k)
{
    /* The main axis scrolls: its overflow is hidden, scroll or auto, or it is visible while the other axis scrolls
     * (visible then computes to auto, CSS Overflow 3 section 3.1; flexbox-min-width-auto-004). A clip axis keeps its
     * automatic minimum whatever the other axis does (flex-minimum-size-single-axis-scroll-container). */
    uint16_t ox = kw_of(embi_lay_prop(x->c, k, EMBI_PROP_overflow_x));
    uint16_t oy = kw_of(embi_lay_prop(x->c, k, EMBI_PROP_overflow_y));
    uint16_t mk = x->row ? ox : oy, ck = x->row ? oy : ox;
    if (mk == EMBI_KW_hidden || mk == EMBI_KW_scroll || mk == EMBI_KW_auto) {
        return true;
    }
    return (mk == EMBI_KW_visible || mk == 0) && (ck == EMBI_KW_hidden || ck == EMBI_KW_scroll || ck == EMBI_KW_auto);
}

/* The gap of a gap property (row-gap, column-gap): normal is 0, percentages of an indefinite base 0. */
static float gap_len(const fctx *x, embi_prop_id p, float pct)
{
    const embi_value *v = embi_lay_prop(x->c, x->b, p);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return 0.0f;
    }
    return clamp0(embi_lay_len(x->c, x->b, v, pct));
}

/* Lays out an item at the given sizes (border-box, forced along the main axis when fm and along the
 * cross axis when fc). The item's fragments are dropped first, so the result is its only layout. */
static bool lay_item_mode(fctx *x, fitem *it, float avail, bool fm, float main_bd, bool fc,
                          float cross_bd, bool content, embi_lres *r);

static bool lay_item(fctx *x, fitem *it, float avail, bool fm, float main_bd, bool fc, float cross_bd,
                     embi_lres *r)
{
    return lay_item_mode(x, it, avail, fm, main_bd, fc, cross_bd, false, r);
}

/* As lay_item. content: the item's own height is ignored (a column item's content height). */
static bool lay_item_mode(fctx *x, fitem *it, float avail, bool fm, float main_bd, bool fc,
                          float cross_bd, bool content, embi_lres *r)
{
    embi_cs cs;
    memset(&cs, 0, sizeof cs);
    cs.avail_w = avail;
    cs.cb_w = x->cw;
    cs.cb_h = x->chd;
    cs.fit = EMBI_FIT_SHRINK;
    embi_lctx *c = x->c;
    /* The forced sizes of this item (border-box): the main size along the row or the column, and the
     * cross size when it is forced. They apply to this box only (block.c, size.c). */
    embi_box *save_box = c->force_box;
    float save_w = c->force_w, save_h = c->force_h;
    bool save_ws = c->force_wset, save_hs = c->force_hset, save_hn = c->force_hnone, save_hf = c->force_hsoft;
    const embi_box *save_hm = c->hraw_box;
    c->force_box = it->box;
    c->hraw_box = content && !x->row ? it->box : nullptr;
    /* A column item's flexed height is definite for its children's percentages only when the container's height
     * is definite, or when the item's height is its specified one (CSS Flexbox 1 section 9.8; Chromium: a
     * flex-basis other than auto, or an auto height, is not definite then; percentage-heights-008/015..018). */
    c->force_hsoft = !x->row && fm && isnan(x->chd) && !it->main_def;
    c->force_wset = false;
    c->force_hset = false;
    c->force_hnone = content && !x->row;
    if (fm) {
        if (x->row) {
            c->force_wset = true;
            c->force_w = main_bd;
        } else {
            c->force_hset = true;
            c->force_h = main_bd;
        }
    }
    if (fc) {
        if (x->row) {
            c->force_hset = true;
            c->force_h = cross_bd;
        } else {
            c->force_wset = true;
            c->force_w = cross_bd;
        }
    }
    embi_lay_clear_frags(c, it->box);
    memset(r, 0, sizeof *r);
    embi_lay_box(c, it->box, &cs, r);
    c->force_box = save_box;
    c->force_w = save_w;
    c->force_h = save_h;
    c->force_wset = save_ws;
    c->force_hset = save_hs;
    c->force_hnone = save_hn;
    c->force_hsoft = save_hf;
    c->hraw_box = save_hm;
    return r->frag != nullptr;
}

/* The natural height of a column item (its border box height at its natural width, or at its
 * definite width). Computed once per item. */
static float natural_height(fctx *x, fitem *it)
{
    if (isnan(it->nat)) {
        embi_lres r;
        bool fc = !isnan(it->scross);
        if (lay_item_mode(x, it, x->cw, false, 0, fc, fc ? it->scross : 0, true, &r)) {
            it->nat = r.frag->h;
        } else {
            it->nat = it->bdr_main;
        }
    }
    return it->nat;
}

/* The max-content width of a row item (border-box): the item laid out with no width limit, so that
 * its shrink-to-fit width is its max-content width and a replaced box keeps its ratio transfers. */
static float natural_width(fctx *x, fitem *it)
{
    if (isnan(it->nat_w)) {
        embi_lres r;
        /* The item's own min-width and max-width do not count: they clamp the hypothetical size, not
         * the flex base size (section 9.2 step 3 E). */
        x->c->force_nominmaxw = true;
        bool ok = lay_item(x, it, INFINITY, false, 0, false, 0, &r);
        x->c->force_nominmaxw = false;
        it->nat_w = ok ? r.frag->w : it->bdr_main;
    }
    return it->nat_w;
}

/* The transferred main size of an item with a preferred aspect ratio and a definite cross size
 * (CSS Flexbox 1 section 9.2 B and 4.5): the cross size through the ratio, as a border-box size. False
 * when there is no such size. */
static bool transferred_main(fctx *x, fitem *it, float edge_main, float edge_cross, float *out)
{
    float ratio = 0;
    if (isnan(it->scross) || !embi_lay_ratio(x->c, it->box, &ratio) || !(ratio > 0)) {
        return false;
    }
    /* The cross size within the item's cross limits (flexbox-min-height-auto-002c: width 60px, max-width 30px). */
    float sc = it->scross > it->max_cross ? it->max_cross : it->scross;
    sc = sc < it->min_cross ? it->min_cross : sc;
    if (is_box_sizing_bb(x, it->box) && !embi_lay_ratio_content(x->c, it->box)) {
        /* The ratio is of the border box (CSS Sizing 4 section 5.1), never below the border and padding
         * (aspect-ratio/flex-aspect-ratio-031). */
        float m = x->row ? sc * ratio : sc / ratio;
        *out = m < edge_main ? edge_main : m;
        return true;
    }
    float cc = sc - edge_cross;
    cc = cc < 0 ? 0 : cc;
    float mc = x->row ? cc * ratio : cc / ratio;
    *out = mc + edge_main;
    return true;
}

/* The rightmost edge of the content below a fragment, in the frame of the fragment (line boxes are
 * looked through: they span their block, the boxes and text in them are what takes room). */
static void frag_right_extent(const embi_frag *f, float ox, float *right)
{
    for (const embi_frag *c = embi_frag_first(f); c != nullptr; c = c->next) {
        float cx = ox + c->x;
        if (c->kind != EMBI_FK_LINE) {
            float r = cx + c->w + embi_frag_mg(c, 2);
            if (r > *right) {
                *right = r;
            }
        }
        frag_right_extent(c, cx, right);
    }
}

/* True when a replaced or aspect-ratio descendant (looked for in a bounded part of the subtree) has a
 * percentage height: its width then depends on a definite height that the intrinsic sizes do not know. */
static bool has_pct_height_replaced(const embi_lctx *c, const embi_box *b, uint32_t *budget)
{
    for (const embi_box *k = b->first; k != nullptr && *budget > 0; k = k->next) {
        (*budget)--;
        const embi_value *hv = embi_lay_prop(c, k, EMBI_PROP_height);
        float rr = 0;
        if (hv != nullptr && hv->kind == EMBI_V_PERCENT &&
            ((k->flags & EMBI_BF_REPLACED) != 0 || k->kind == EMBI_BK_REPLACED || embi_lay_ratio(c, k, &rr))) {
            return true;
        }
        if (!embi_box_is_text(k) && k->first != nullptr && has_pct_height_replaced(c, k, budget)) {
            return true;
        }
    }
    return false;
}

/* The width of a row item laid out with its stretched cross size as a definite height, from the extent
 * of its content (the percentage heights of replaced descendants resolve, and their ratios give widths
 * that the intrinsic sizes could not). NaN when no such measure applies. */
static float definite_content_width(fctx *x, fitem *it)
{
    uint32_t budget = 48;
    if (!x->row || isnan(it->scross) || !isnan(it->spec_cross) || it->ratio_box ||
        !has_pct_height_replaced(x->c, it->box, &budget)) {
        return NAN;
    }
    embi_lres r;
    if (!lay_item(x, it, INFINITY, false, 0, true, it->scross, &r) || r.frag == nullptr) {
        return NAN;
    }
    float right = 0;
    frag_right_extent(r.frag, 0, &right);
    return right + embi_frag_bdr(r.frag, 2) + embi_frag_pad(r.frag, 2);
}

/* Sets the sizes, margins, basis and limits of an item (CSS Flexbox 1 section 9.2 step 3 and section
 * 4.5). */
static void item_prepare(fctx *x, fitem *it)
{
    embi_lctx *c = x->c;
    embi_box *k = it->box;
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, k, x->cw, &bdr, &pad, &mar);
    float eh = bdr.l + bdr.r + pad.l + pad.r;
    float ev = bdr.t + bdr.b + pad.t + pad.b;
    bool bb = is_box_sizing_bb(x, k);
    float edge_main = x->row ? eh : ev;
    float edge_cross = x->row ? ev : eh;
    it->bdr_main = edge_main;
    it->bdr_cross = edge_cross;
    it->nat = NAN;
    it->nat_w = NAN;
    it->stretched = false;

    /* Margins, from the physical values to the logical start and end of each axis. */
    float ms, me, cs_, ce;
    if (x->row) {
        ms = x->flip_main ? mar.r : mar.l;
        me = x->flip_main ? mar.l : mar.r;
        cs_ = x->cflip ? mar.b : mar.t;
        ce = x->cflip ? mar.t : mar.b;
    } else {
        ms = x->flip_main ? mar.b : mar.t;
        me = x->flip_main ? mar.t : mar.b;
        cs_ = x->cflip ? mar.r : mar.l;
        ce = x->cflip ? mar.l : mar.r;
    }
    it->mas = isnan(ms);
    it->mae = isnan(me);
    it->cas = isnan(cs_);
    it->cae = isnan(ce);
    it->mms = it->mas ? 0 : ms;
    it->mme = it->mae ? 0 : me;
    it->cms = it->cas ? 0 : cs_;
    it->cme = it->cae ? 0 : ce;

    /* Sizes and limits. Percentages resolve against the content box, and heights against the
     * container's height only when it is definite. */
    float pmain = x->row ? x->cw : x->chd;
    float pcross = x->row ? x->chd : x->cw;
    embi_prop_id mainp = x->row ? EMBI_PROP_width : EMBI_PROP_height;
    embi_prop_id crossp = x->row ? EMBI_PROP_height : EMBI_PROP_width;
    embi_prop_id minmp = x->row ? EMBI_PROP_min_width : EMBI_PROP_min_height;
    embi_prop_id maxmp = x->row ? EMBI_PROP_max_width : EMBI_PROP_max_height;
    embi_prop_id mincp = x->row ? EMBI_PROP_min_height : EMBI_PROP_min_width;
    embi_prop_id maxcp = x->row ? EMBI_PROP_max_height : EMBI_PROP_max_width;
    it->spec_main = size_bb(x, k, mainp, pmain, edge_main, bb);
    it->spec_cross = size_bb(x, k, crossp, pcross, edge_cross, bb);
    bool col_table = !x->row && k->kind == EMBI_BK_TABLE && k->tr == EMBI_TR_TABLE && (k->flags & EMBI_BF_ANON) == 0;
    if (col_table && !isnan(it->spec_main)) {
        /* A table's height is that of its table box: the captions add to it, and the content can make it taller
         * (CSS Tables 3 section 3.9.3). Its main size in a column is the height of the whole table, laid out
         * (table-as-item-stretch-cross-size-3/5). */
        embi_lres tr;
        bool fc = !isnan(it->spec_cross);
        float spec = it->spec_main;
        if (lay_item(x, it, x->cw, false, 0, fc, fc ? it->spec_cross : 0, &tr)) {
            /* The table's height property sizes its table box: the captions come on top of it in a column
             * (Chromium; table-as-item-stretch-cross-size-3/5), and taller rows make it taller. */
            float ct = 0, cb = 0;
            embi_frag_caps(tr.frag, &ct, &cb);
            it->spec_main = fmaxf(tr.frag->h, spec + ct + cb);
        }
    }
    /* A percentage that resolves to auto is not auto: such an item does not stretch (section 9.4
     * step 11 looks at the property). */
    it->cross_auto = embi_lay_is_auto(embi_lay_prop_calc(c, k, crossp, &it->cross_expr));
    it->bb = bb;

    /* The automatic minimum (section 4.5) applies to min-main: auto, except to scroll containers. */
    const embi_value *minv = embi_lay_prop(c, k, minmp);
    it->auto_min = embi_lay_is_auto(minv) && !scrolls_in_main(x, k);
    float minm = size_bb(x, k, minmp, pmain, edge_main, bb);
    if (minv != nullptr && (kw_of(minv) == EMBI_KW_min_content || kw_of(minv) == EMBI_KW_max_content)) {
        float mn = 0, mx = 0;
        embi_lay_intrinsic(c, k, &mn, &mx);
        minm = clamp0((kw_of(minv) == EMBI_KW_min_content ? mn : mx) + edge_main);
    }
    if (isnan(minm)) {
        minm = 0;
    }
    const embi_value *maxv = embi_lay_prop(c, k, maxmp);
    float maxm = size_bb(x, k, maxmp, pmain, edge_main, bb);
    if (maxv != nullptr && (kw_of(maxv) == EMBI_KW_min_content || kw_of(maxv) == EMBI_KW_max_content)) {
        float mn = 0, mx = 0;
        embi_lay_intrinsic(c, k, &mn, &mx);
        maxm = clamp0((kw_of(maxv) == EMBI_KW_min_content ? mn : mx) + edge_main);
    }
    if (maxv != nullptr && (kw_of(maxv) == EMBI_KW_min_content || kw_of(maxv) == EMBI_KW_max_content)) {
        const embi_value *mxx = embi_lay_size_expr(c, k, maxmp);
        if (mxx != nullptr && !isinf(maxm)) {
            float r = embi_lay_size_eval(c, k, mxx, bb ? maxm : maxm - edge_main, pmain);
            if (!isnan(r)) {
                maxm = clamp0(bb ? r : r + edge_main);
            }
        }
    }
    if (isnan(maxm)) {
        maxm = INFINITY;
    }
    float minc = size_bb(x, k, mincp, pcross, edge_cross, bb);
    float maxc = size_bb(x, k, maxcp, pcross, edge_cross, bb);
    it->min_cross = isnan(minc) ? edge_cross : fmaxf(minc, edge_cross);
    if (!x->row && k->kind == EMBI_BK_TABLE && k->tr == EMBI_TR_TABLE && (k->flags & EMBI_BF_ANON) == 0) {
        /* A table is never narrower than its min-content width, so it does not stretch below it. */
        float tmn = 0, tmx = 0;
        embi_lay_intrinsic(c, k, &tmn, &tmx);
        if (tmn + edge_cross > it->min_cross) {
            it->min_cross = tmn + edge_cross;
        }
    }
    it->max_cross = isnan(maxc) ? INFINITY : fmaxf(maxc, it->min_cross);

    /* align-self: auto takes the container's align-items, normal stretches. */
    align_v as = align_of(embi_lay_prop(c, k, EMBI_PROP_align_self));
    if (as.pos == EMBI_KW_auto || (as.pos == 0 && !as.baseline)) {
        as = x->ai;
    }
    it->cal = cal_for(x, k, as);
    /* The cross size a preferred aspect ratio transfers from: the specified one, or, for an item that
     * stretches in a single-line container with a definite cross size, the stretched one (section
     * 9.8), within the item's cross limits. */
    it->scross = it->spec_cross;
    if (isnan(it->scross) && !x->wrapping && it->cross_auto && it->cal == AL_STRETCH && !it->cas &&
        !it->cae) {
        float ccross = x->row ? x->chd : x->cw;
        if (!isnan(ccross)) {
            float want = ccross - it->cms - it->cme;
            it->scross = want < it->min_cross ? it->min_cross
                                              : (want > it->max_cross ? it->max_cross : want);
        }
    }

    /* The flex base size (section 9.2 step 3 A to C). */
    const embi_value *bv = embi_lay_prop(c, k, EMBI_PROP_flex_basis);
    float basis = NAN;
    float trans = NAN;
    bool has_trans = isnan(it->spec_main) && transferred_main(x, it, edge_main, edge_cross, &trans);
    it->basis_auto = bv == nullptr || embi_lay_is_auto(bv);
    {
        float rr = 0;
        it->ratio = embi_lay_ratio(c, k, &rr) && rr > 0 ? rr : 0;
    }
    if (bv == nullptr || embi_lay_is_auto(bv)) {
        basis = has_trans ? trans : it->spec_main;
    } else if (bv->kind != EMBI_V_KEYWORD) {
        float len = embi_lay_len(c, k, bv, pmain);
        if (!isnan(len)) {
            basis = clamp0(bb ? len : len + edge_main);
        }
    } else if (kw_of(bv) == EMBI_KW_content) {
        /* flex-basis: content ignores the main size property: a definite cross size still transfers through the
         * ratio (section 9.2 step 3 B; aspect-ratio/flex-aspect-ratio-021). */
        float tr = NAN;
        if (it->ratio > 0 && transferred_main(x, it, edge_main, edge_cross, &tr)) {
            basis = tr;
        }
    }
    /* A replaced box or a box with an aspect ratio gets its width from its layout with no width
     * limit, so its ratio transfers (from a max-height, say) count. Other boxes take their max-content
     * size, which ignores their own min and max (section 9.2 step 3). */
    bool ratio_box = (k->flags & EMBI_BF_REPLACED) != 0 || (k->kind == EMBI_BK_REPLACED) || has_trans;
    float ratio_probe = 0;
    bool has_pref_ratio = embi_lay_ratio(c, k, &ratio_probe) || has_trans;
    if (!ratio_box) {
        ratio_box = has_pref_ratio;
    }
    it->ratio_box = ratio_box;
    if (isnan(basis)) {
        if (x->row) {
            if (ratio_box) {
                basis = natural_width(x, it);
            } else {
                float mn = 0, mx = 0;
                embi_lay_intrinsic(c, k, &mn, &mx);
                basis = mx + edge_main;
                float dw = definite_content_width(x, it);
                if (!isnan(dw) && dw < basis) {
                    basis = dw;
                }
            }
        } else {
            basis = natural_height(x, it);
        }
    }
    it->basis = basis;
    if (isinf(maxm) && maxv != nullptr && kw_of(maxv) == EMBI_KW_auto) {
        /* calc-size(auto, ...) on max-main: the calc-sum sees the flex base size as `size`. */
        const embi_value *mxa = embi_lay_size_expr(c, k, maxmp);
        if (mxa != nullptr) {
            float r = embi_lay_size_eval(c, k, mxa, bb ? basis : basis - edge_main, pmain);
            if (!isnan(r)) {
                maxm = clamp0(bb ? r : r + edge_main);
            }
        }
    }

    /* The minimum, with the automatic one: the content size suggestion, capped by the specified size
     * suggestion and by the max-main size (section 4.5). */
    float minv_used = minm < edge_main ? edge_main : minm;
    if (it->auto_min) {
        float cont;
        if (x->row) {
            /* The min-content size, capped by the natural width (a replaced box's limits transfer
             * into its size, as they do in the natural layout). */
            float mn = 0, mx = 0;
            embi_lay_intrinsic(c, k, &mn, &mx);
            cont = mn + edge_main;
            if (!has_trans) {
                /* The stretched cross size is definite: percentage heights inside the item resolve
                 * against it and can lower its min-content width. */
                float dw = definite_content_width(x, it);
                if (!isnan(dw) && dw < cont) {
                    cont = dw;
                }
            }
            float trans_r = NAN;
            bool repl_k = (k->flags & EMBI_BF_REPLACED) != 0 || k->kind == EMBI_BK_REPLACED;
            if (!has_trans && repl_k && (k->flags2 & EMBI_BF2_CONTROL) == 0 && !isnan(it->spec_main) &&
                transferred_main(x, it, edge_main, edge_cross, &trans_r)) {
                /* A replaced box with a specified width and a definite (or stretched) cross size: its min-content
                 * size is that cross size through the ratio, which the specified size suggestion then caps
                 * (flex-minimum-width-flex-items-013). */
                cont = trans_r;
            } else if (has_trans && repl_k) {
                /* A definite cross size transfers through the ratio into the content size suggestion of a replaced box. */
                cont = trans;
            } else if (has_trans) {
                /* A box with a ratio: the content size suggestion is its min-content size, the transferred size is
                 * not a cap on it when the main size is auto (aspect-ratio/flex-aspect-ratio-002). */
            } else if (it->ratio_box) {
                float nw = natural_width(x, it);
                if (nw < cont) {
                    cont = nw;
                }
            }
        } else {
            cont = natural_height(x, it);
        }
        if (it->ratio_box && !has_trans && it->ratio > 0) {
            /* With a preferred aspect ratio the content size suggestion is clamped by the definite min and max
             * cross sizes converted through the ratio (CSS Flexbox 1 section 4.5; flexbox-min-width-auto-002). */
            float r = x->row ? it->ratio : 1.0f / it->ratio;
            float lo = (it->min_cross - edge_cross) * r + edge_main;
            float hi = isinf(it->max_cross) ? INFINITY : (it->max_cross - edge_cross) * r + edge_main;
            if (cont > hi) {
                cont = hi;
            }
            if (cont < lo) {
                cont = lo;
            }
        }
        float spec_sugg = it->spec_main;
        if (x->row && (k->flags & EMBI_BF_REPLACED) != 0 && (k->flags2 & EMBI_BF2_CONTROL) != 0 &&
            !is_button_input(k) && !isnan(it->spec_main)) {
            /* A compressible replaced element (a form control, CSS Sizing 3 section 5.2.2): the percentage part
             * of its width counts as zero in the specified size suggestion. An img resolves it
             * (flex-aspect-ratio-img-row-017). */
            float z = size_bb(x, k, mainp, 0.0f, edge_main, bb);
            if (!isnan(z)) {
                spec_sugg = z;
            }
        }
        if (has_trans && ((k->flags & EMBI_BF_REPLACED) != 0 || k->kind == EMBI_BK_REPLACED)) {
            spec_sugg = trans;
        }
        if (!isnan(spec_sugg) && spec_sugg < cont) {
            cont = spec_sugg;
        }
        if (cont > maxm) {
            cont = maxm;
        }
        minv_used = clamp0(cont) < edge_main ? edge_main : clamp0(cont);
    }
    /* calc-size() on min-main when the minimum is auto or a content keyword (size_bb reads only lengths):
     * the calc-sum sees that minimum as `size`. */
    if (it->auto_min || (minv != nullptr && (kw_of(minv) == EMBI_KW_min_content || kw_of(minv) == EMBI_KW_max_content))) {
        const embi_value *mnx = embi_lay_size_expr(c, k, minmp);
        if (mnx != nullptr) {
            float r = embi_lay_size_eval(c, k, mnx, bb ? minv_used : minv_used - edge_main, pmain);
            if (!isnan(r)) {
                minv_used = clamp0(bb ? r : r + edge_main);
            }
        }
    }
    if (x->row && k->kind == EMBI_BK_TABLE && k->tr == EMBI_TR_TABLE && (k->flags & EMBI_BF_ANON) == 0) {
        /* A table is never narrower than its min-content width (CSS 2.1 section 17.5.2), whatever min-width says. */
        float tmn = 0, tmx = 0;
        embi_lay_intrinsic(c, k, &tmn, &tmx);
        if (tmn + edge_main > minv_used) {
            minv_used = tmn + edge_main;
        }
    }
    if (col_table) {
        /* A table is never shorter than its content (with its captions), whatever min-height says
         * (table-as-item-min-content-height-1). */
        float nh = natural_height(x, it);
        if (nh > minv_used) {
            minv_used = nh;
        }
    }
    it->min_main = minv_used;
    it->max_main = maxm < minv_used ? minv_used : maxm;

    /* The hypothetical main size: the base size clamped by the limits (the minimum wins). */
    float h = basis < it->min_main ? it->min_main : basis;
    it->hyp = h > it->max_main ? it->max_main : h;

    it->grow = num_of(embi_lay_prop(c, k, EMBI_PROP_flex_grow), 0.0f);
    it->shrink = num_of(embi_lay_prop(c, k, EMBI_PROP_flex_shrink), 1.0f);
    /* In a container with an indefinite main size, the item's main size is still definite when it is its specified
     * size, a size transferred through its aspect ratio, or a definite flex basis it does not grow from (an
     * indefinite container takes the sum of the bases, so nothing shrinks; percentage-heights-006/007/017,
     * flex-minimum-height-flex-items-025, aspect-ratio/flex-aspect-ratio-032). */
    it->main_def = (it->basis_auto && (!isnan(it->spec_main) || has_trans)) ||
                   (!it->basis_auto && bv != nullptr && bv->kind != EMBI_V_KEYWORD && !isnan(basis) && it->grow == 0);
    const embi_value *ov = embi_lay_prop(c, k, EMBI_PROP_order);
    it->order = ov != nullptr && (ov->kind == EMBI_V_INTEGER || ov->kind == EMBI_V_NUMBER) ? (int32_t)ov->num : 0;

    it->frozen = false;
    it->collapsed = !x->no_collapse && is_collapsed(c, k);
    it->coll_main = it->hyp;
    it->strut = NAN;
    if (it->collapsed) {
        /* A strut: no main size and no main margins, the cross size stays. */
        it->basis = it->hyp = it->min_main = it->max_main = 0;
        it->mms = it->mme = 0;
        it->mas = it->mae = false;
        it->grow = it->shrink = 0;
    }
    it->target = it->hyp;
    it->cross = NAN;
}

/* The number of gaps in a line: between the items that are not collapsed (a visibility: collapse strut takes no gap on
 * either side, as in Chromium; WPT gap-collapse). */
static uint32_t line_gaps(const fitem *its, uint32_t cnt)
{
    uint32_t vis = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        vis += its[i].collapsed ? 0u : 1u;
    }
    return vis > 0 ? vis - 1 : 0;
}

/* Resolves the flexible lengths of one line (CSS Flexbox 1 section 9.7). Sets each item's target. */
static void resolve_line(fctx *x, fitem *its, uint32_t cnt, float M, float gap)
{
    (void)x;
    float gapsum = gap * (float)line_gaps(its, cnt);
    float outer = gapsum;
    for (uint32_t i = 0; i < cnt; i++) {
        outer += its[i].hyp + its[i].mms + its[i].mme;
    }
    bool grow = outer < M;

    /* Inflexible items are frozen at their hypothetical size (step 3). */
    for (uint32_t i = 0; i < cnt; i++) {
        fitem *it = &its[i];
        float fac = grow ? it->grow : it->shrink;
        it->frozen = false;
        if (fac == 0 || (grow && it->basis > it->hyp) || (!grow && it->basis < it->hyp)) {
            it->frozen = true;
            it->target = it->hyp;
        }
    }

    /* The initial free space (step 3): frozen items at their target, the others at their base size. */
    float init_free;
    {
        float used = gapsum;
        for (uint32_t i = 0; i < cnt; i++) {
            used += (its[i].frozen ? its[i].target : its[i].basis) + its[i].mms + its[i].mme;
        }
        init_free = M - used;
    }

    for (uint32_t iter = 0; iter <= cnt; iter++) {
        float used = gapsum;
        float sumf = 0, sums = 0;
        bool any = false;
        for (uint32_t i = 0; i < cnt; i++) {
            fitem *it = &its[i];
            used += (it->frozen ? it->target : it->basis) + it->mms + it->mme;
            if (!it->frozen) {
                any = true;
                sumf += grow ? it->grow : it->shrink;
                sums += it->shrink * it->basis;
            }
        }
        if (!any) {
            break;
        }
        float free_space = M - used;
        /* With a flex factor sum below one, only that fraction of the initial free space is handed
         * out (step 4c), when that is less than the remaining free space. */
        if (sumf > 0 && sumf < 1.0f) {
            float frac = init_free * sumf;
            if (fabsf(frac) < fabsf(free_space)) {
                free_space = frac;
            }
        }
        float total = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            fitem *it = &its[i];
            if (it->frozen) {
                continue;
            }
            float t = it->basis;
            if (grow) {
                if (sumf > 0) {
                    t = it->basis + free_space * it->grow / sumf;
                }
            } else if (sums > 0) {
                t = it->basis + free_space * (it->shrink * it->basis) / sums;
            }
            float lo = it->min_main;
            float hi = it->max_main;
            float cl = t > hi ? hi : t;
            if (cl < lo) {
                cl = lo;
            }
            it->target = cl;
            total += cl - t;
        }
        /* Freeze the violations (step 5 of section 9.7): all of them when the total is zero, else
         * those on the side of the total. */
        for (uint32_t i = 0; i < cnt; i++) {
            fitem *it = &its[i];
            if (it->frozen) {
                continue;
            }
            float t = it->basis;
            if (grow) {
                if (sumf > 0) {
                    t = it->basis + free_space * it->grow / sumf;
                }
            } else if (sums > 0) {
                t = it->basis + free_space * (it->shrink * it->basis) / sums;
            }
            float viol = it->target - t;
            if (total == 0 || (total > 0 && viol > 0) || (total < 0 && viol < 0)) {
                it->frozen = true;
            }
        }
    }
    for (uint32_t i = 0; i < cnt; i++) {
        if (!its[i].frozen) {
            its[i].target = its[i].basis;
        }
        if (its[i].target < its[i].bdr_main) {
            its[i].target = its[i].bdr_main;
        }
    }
}

/* Positions one line along the main axis (justify-content and auto margins, section 9.5). Sets each
 * item's mpos. Returns the free space left after the auto margins took their share. */
static void justify_line(fctx *x, fitem *its, uint32_t cnt, float M, float gap, uint16_t jpos, bool safe)
{
    float used = gap * (float)line_gaps(its, cnt);
    uint32_t nauto = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        used += its[i].target + its[i].mms + its[i].mme;
        nauto += (its[i].mas ? 1u : 0u) + (its[i].mae ? 1u : 0u);
    }
    float free_space = M - used;
    if (nauto > 0 && free_space > 0) {
        float share = free_space / (float)nauto;
        for (uint32_t i = 0; i < cnt; i++) {
            if (its[i].mas) {
                its[i].mms = share;
            }
            if (its[i].mae) {
                its[i].mme = share;
            }
        }
        free_space = 0;
    }

    /* The logical end of the line, from the keyword: start and end follow the writing direction,
     * left and right the physical edges (a row only). */
    bool lend = false;
    float lead = 0, between = gap;
    float n = (float)cnt;
    if (safe && free_space < 0 && (jpos == EMBI_KW_flex_end || jpos == EMBI_KW_flex_start || jpos == EMBI_KW_end ||
                                   jpos == EMBI_KW_center || jpos == EMBI_KW_right)) {
        jpos = EMBI_KW_start; /* safe: overflowing items align to the start edge */
    }
    switch (jpos) {
    case EMBI_KW_flex_end:
    case EMBI_KW_end:
        lend = jpos == EMBI_KW_flex_end ? true : !x->reverse;
        break;
    case EMBI_KW_start:
        lend = x->reverse;
        break;
    case EMBI_KW_left:
        lend = x->row ? x->flip_main : x->reverse;
        break;
    case EMBI_KW_right:
        lend = x->row ? !x->flip_main : x->reverse;
        break;
    case EMBI_KW_center:
        lead = free_space / 2;
        break;
    case EMBI_KW_space_between:
        if (free_space > 0 && cnt > 1) {
            between += free_space / (n - 1);
        }
        break;
    case EMBI_KW_space_around:
        if (free_space > 0) {
            between += free_space / n;
            lead = free_space / (2 * n);
        } else {
            lead = 0; /* the fallback is safe center: start when the items overflow */
        }
        break;
    case EMBI_KW_space_evenly:
        if (free_space > 0) {
            between += free_space / (n + 1);
            lead = free_space / (n + 1);
        } else {
            lead = 0; /* the fallback is safe center: start when the items overflow */
        }
        break;
    default:
        break;
    }
    if (lend) {
        lead = free_space;
    }
    float pos = lead;
    bool seen = false; /* an item that is not collapsed came before: the next one that is not starts after a gap */
    for (uint32_t i = 0; i < cnt; i++) {
        if (!its[i].collapsed) {
            pos += seen ? between : 0.0f;
            seen = true;
        }
        its[i].mpos = pos;
        pos += its[i].target + its[i].mms + its[i].mme;
    }
}

/* The fraction of the free space that a justify-content value places a box at, from the logical
 * start (0), the center (0.5) or the logical end (1). Start and end follow the writing direction of
 * the container's start edge, and left and right the physical edges (section 9.5). */
static float frac_of_justify(const fctx *x, uint16_t jpos)
{
    switch (jpos) {
    case EMBI_KW_flex_end:
        return 1.0f;
    case EMBI_KW_end:
        return x->reverse ? 0.0f : 1.0f;
    case EMBI_KW_start:
        return x->reverse ? 1.0f : 0.0f;
    case EMBI_KW_left:
        /* A row's physical left edge; in a column, left and right act as start (the inline start is
         * not the main axis there). */
        if (x->row) {
            return x->flip_main ? 1.0f : 0.0f;
        }
        return x->reverse ? 1.0f : 0.0f;
    case EMBI_KW_right:
        if (x->row) {
            return x->flip_main ? 0.0f : 1.0f;
        }
        return x->reverse ? 1.0f : 0.0f;
    case EMBI_KW_center:
    case EMBI_KW_space_around:
    case EMBI_KW_space_evenly:
        return 0.5f;
    default:
        return 0.0f;
    }
}

/* The fraction along the cross axis of an out-of-flow child: its align-self (auto: the container's
 * align-items), as for a sole item (section 4.1). */
static float cal_frac(const fctx *x, const embi_box *k)
{
    align_v as = align_of(embi_lay_prop(x->c, k, EMBI_PROP_align_self));
    if (as.pos == EMBI_KW_auto || (as.pos == 0 && !as.baseline)) {
        as = x->ai;
    }
    if (as.baseline) {
        /* The fallback alignment of a box with no baseline set is start for first baseline and end for
         * last baseline (CSS Align 3 section 9.3). Both are edges of the writing mode, so wrap-reverse
         * flips them with the other start and end values. */
        as = (align_v){as.last ? EMBI_KW_end : EMBI_KW_start, false, false, false};
    }
    int cal = cal_for(x, k, as);
    if (cal == AL_END) {
        return 1.0f;
    }
    if (cal == AL_CENTER) {
        return 0.5f;
    }
    return 0.0f;
}

/* The cross alignment of a value in a container. start, end, left and right are edges of the writing
 * mode, not of the flex lines, so wrap-reverse does not flip them (only flex-start and flex-end, and
 * the defaults, follow the lines). Baselines along a column's cross axis act as start. */
static int cal_for(const struct fctx *x, const embi_box *k, align_v a)
{
    int cal = cal_of(a);
    if (!a.baseline && !x->row) {
        /* The cross axis of a column is horizontal: start, end, left, right and the self-* values name
         * physical sides through the writing direction (of the container, or of the item for self-*).
         * The logical cross start is the right edge when cflip. */
        const embi_value *dv = embi_lay_prop(x->c, k, EMBI_PROP_direction);
        bool krtl = dv != nullptr && dv->kind == EMBI_V_KEYWORD && dv->kw == EMBI_KW_rtl;
        switch (a.pos) {
        case EMBI_KW_start:
            return x->cflip == x->rtl ? AL_START : AL_END;
        case EMBI_KW_end:
            return x->cflip == x->rtl ? AL_END : AL_START;
        case EMBI_KW_self_start:
            return x->cflip == krtl ? AL_START : AL_END;
        case EMBI_KW_self_end:
            return x->cflip == krtl ? AL_END : AL_START;
        case EMBI_KW_left:
            return x->cflip ? AL_END : AL_START;
        case EMBI_KW_right:
            return x->cflip ? AL_START : AL_END;
        default:
            return cal;
        }
    }
    if (!a.baseline && x->wrap_rev) {
        if (a.pos == EMBI_KW_start || a.pos == EMBI_KW_self_start || a.pos == EMBI_KW_left ||
            a.pos == EMBI_KW_right) {
            cal = AL_END;
        } else if (a.pos == EMBI_KW_end || a.pos == EMBI_KW_self_end) {
            cal = AL_START;
        }
    }
    return cal;
}

/* A baseline of an item from its border-box top, or its border-box bottom when it has none. A scroll
 * container's baseline is clamped to its border box (CSS Align 3 section 9.1: its content may be
 * scrolled far away from it). */
static float item_bl(const fitem *it, bool last)
{
    const embi_frag *f = it->lr.frag;
    float bl = f != nullptr ? (last ? embi_fl(f)->bl_last : f->bl_first) : NAN;
    if (isnan(bl)) {
        return it->cross;
    }
    if (f != nullptr && (it->box->flags & EMBI_BF_SCROLL) != 0) {
        bl = bl < 0 ? 0 : (bl > f->h ? f->h : bl);
    }
    return bl;
}

static float item_baseline(const fitem *it)
{
    return item_bl(it, false);
}

static float item_baseline_last(const fitem *it)
{
    return item_bl(it, true);
}

/* For an item aligned to a baseline (row containers only): the distance s from the margin box's
 * logical cross start to the baseline, and the distance e from the baseline to the logical cross end.
 * The baseline is measured from the physical top, so with a flipped cross axis (wrap-reverse) the
 * logical start is the bottom. */
static void baseline_dists(const fctx *x, const fitem *it, float *s, float *e)
{
    bool last = it->cal == AL_LBASELINE;
    /* In a column the cross axis is the inline axis: the baseline of a box there is its border-box start
     * (first) or end (last) edge, so baseline alignment lines those edges up. */
    float bl = !x->row ? (last ? it->cross : 0.0f) : (last ? item_baseline_last(it) : item_baseline(it));
    float outer = it->cross + it->cms + it->cme;
    float top = bl + (x->cflip ? it->cme : it->cms); /* physical top margin edge to baseline */
    *s = x->cflip ? outer - top : top;
    *e = outer - *s;
}

/* The size of the baseline-sharing groups of a line: the first-baseline group (items aligned to their
 * first baseline) takes max s + max e, and so does the last-baseline group. The largest outer size of
 * the other items is not included. */
static float baseline_groups_size(const fctx *x, const fitem *its, uint32_t cnt)
{
    float size = 0;
    for (int kind = 0; kind < 2; kind++) {
        int al = kind == 0 ? AL_BASELINE : AL_LBASELINE;
        float ms = -INFINITY, me = -INFINITY;
        for (uint32_t i = 0; i < cnt; i++) {
            if (its[i].cal != al) {
                continue;
            }
            float s, e;
            baseline_dists(x, &its[i], &s, &e);
            ms = s > ms ? s : ms;
            me = e > me ? e : me;
        }
        if (ms > -INFINITY && ms + me > size) {
            size = ms + me;
        }
    }
    return size;
}

/* The alignment of the items of one line along the cross axis (section 9.4 step 15). An item that
 * stretches takes the line's cross size (less its margins, within its limits): its cross is set
 * here and the caller lays it out again at that size. Baseline groups: the first-baseline items
 * share a baseline and the one with the largest s sits flush with the cross start; the last-baseline
 * items share one with the largest e flush with the cross end. */
static void align_line(fctx *x, fitem *its, uint32_t cnt, float L)
{
    float ms1 = -INFINITY, me2 = -INFINITY;
    for (uint32_t i = 0; i < cnt; i++) {
        fitem *it = &its[i];
        if (it->cal == AL_BASELINE || it->cal == AL_LBASELINE) {
            float s, e;
            baseline_dists(x, it, &s, &e);
            if (it->cal == AL_BASELINE && s > ms1) {
                ms1 = s;
            }
            if (it->cal == AL_LBASELINE && e > me2) {
                me2 = e;
            }
        }
    }
    for (uint32_t i = 0; i < cnt; i++) {
        fitem *it = &its[i];
        float outer = it->cross + it->cms + it->cme;
        if (it->cas || it->cae) {
            float freec = L - outer;
            if (freec > 0) {
                if (it->cas && it->cae) {
                    it->cms = freec / 2;
                    it->cme = freec / 2;
                } else if (it->cas) {
                    it->cms = freec;
                } else {
                    it->cme = freec;
                }
            }
            it->cpos = 0;
            continue;
        }
        switch (it->cal) {
        case AL_END:
            it->cpos = L - outer;
            break;
        case AL_CENTER:
            it->cpos = (L - outer) / 2;
            break;
        case AL_BASELINE: {
            float s, e;
            baseline_dists(x, it, &s, &e);
            it->cpos = ms1 - s;
            break;
        }
        case AL_LBASELINE: {
            float s, e;
            baseline_dists(x, it, &s, &e);
            it->cpos = (L - me2) - s;
            break;
        }
        case AL_STRETCH:
            it->cpos = 0;
            if (it->cross_auto) {
                float want = L - it->cms - it->cme;
                if (it->cross_expr != nullptr) {
                    /* calc-size(auto, ...) on the cross size: the calc-sum sees the stretched size as `size`. */
                    float r = embi_lay_size_eval(x->c, it->box, it->cross_expr, it->bb ? want : want - it->bdr_cross,
                                                 x->row ? x->chd : x->cw);
                    if (!isnan(r)) {
                        want = clamp0(it->bb ? r : r + it->bdr_cross);
                    }
                }
                if (want < it->min_cross) {
                    want = it->min_cross;
                }
                if (want > it->max_cross) {
                    want = it->max_cross;
                }
                it->cross = want;
                it->stretched = true;
            }
            break;
        default:
            it->cpos = 0;
            break;
        }
    }
}

/* The main size of a column container with an indefinite height: the sum of the outer hypothetical sizes, clamped by
 * the limits (a fieldset caps it at max-height only). */
static float indef_main(const fctx *x, const fitem *its, uint32_t n)
{
    float sumh = x->gap_m * (float)line_gaps(its, n);
    for (uint32_t i = 0; i < n; i++) {
        sumh += its[i].hyp + its[i].mms + its[i].mme;
    }
    return is_fieldset(x->b) || sumh > x->maxh ? (sumh > x->maxh ? x->maxh : sumh) : (sumh < x->minh ? x->minh : sumh);
}

/* Collects the items into lines (CSS Flexbox 1 section 9.3 step 5): lb receives the first item of each line and n
 * after the last one. Returns the number of lines. */
static uint32_t build_lines(const fitem *items, uint32_t n, float M, float gap, bool multi, uint32_t *lb)
{
    uint32_t nl = 0;
    if (n > 0) {
        uint32_t ls = 0;
        float acc = 0;
        bool seen = false; /* the line has an item that is not collapsed (line_gaps) */
        lb[nl++] = 0;
        for (uint32_t i = 0; i < n; i++) {
            float o = items[i].hyp + items[i].mms + items[i].mme;
            float g = seen && !items[i].collapsed ? gap : 0.0f;
            if (multi && i > ls && acc + g + o > M + 0.01f) {
                lb[nl++] = i;
                ls = i;
                acc = o;
                seen = !items[i].collapsed;
            } else {
                acc += g + o;
                seen = seen || !items[i].collapsed;
            }
        }
    }
    lb[nl] = n;
    return nl;
}

/* The height of a multi-line column container with an indefinite height, once its lines are known: its longest
 * line, within its limits (the lines broke at its max-height; Chromium; flex-column-reverse-multiline-item-position). */
static float wrap_main(const fctx *x, const fitem *its, const uint32_t *lb, uint32_t nl, float gap)
{
    float best = 0;
    for (uint32_t k = 0; k < nl; k++) {
        float len = gap * (float)line_gaps(its + lb[k], lb[k + 1] - lb[k]);
        for (uint32_t i = lb[k]; i < lb[k + 1]; i++) {
            len += its[i].hyp + its[i].mms + its[i].mme;
        }
        best = len > best ? len : best;
    }
    best = best > x->maxh ? x->maxh : best;
    return best < x->minh ? x->minh : best;
}

/* Lays out each item at its target main size: its cross size comes from the layout (a strut has its strut size). */
static void measure_items(fctx *x, fitem *items, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        fitem *it = &items[i];
        bool fc = !isnan(it->spec_cross);
        float cbd = fc ? it->spec_cross : 0;
        if (fc) {
            cbd = cbd < it->min_cross ? it->min_cross : (cbd > it->max_cross ? it->max_cross : cbd);
        }
        embi_lres r;
        if (!lay_item(x, it, x->cw, true, it->collapsed ? it->coll_main : it->target, fc, cbd, &r)) {
            it->lr.frag = nullptr;
            it->cross = 0;
            continue;
        }
        it->lr = r;
        it->cross = fc ? cbd : (x->row ? r.frag->h : r.frag->w);
        if (it->collapsed && !isnan(it->strut)) {
            it->cross = it->strut;
        }
    }
}

/* Line cross sizes: the largest outer cross size of the items, with the baseline-aligned items sharing their baseline
 * (section 9.4 step 8). */
static void line_cross_sizes(fctx *x, fitem *items, const uint32_t *lb, uint32_t nl, float *lcross)
{
    for (uint32_t k = 0; k < nl; k++) {
        float maxo = 0;
        for (uint32_t i = lb[k]; i < lb[k + 1]; i++) {
            fitem *it = &items[i];
            float outer = it->cross + it->cms + it->cme;
            if (it->cal != AL_BASELINE && it->cal != AL_LBASELINE && outer > maxo) {
                maxo = outer;
            }
        }
        float gs = baseline_groups_size(x, items + lb[k], lb[k + 1] - lb[k]);
        lcross[k] = fmaxf(maxo, gs);
    }
}

/* The cross size of a row container with an auto height and a preferred aspect ratio: the height the ratio gives its
 * width, within its limits, which is definite for its items (they stretch to it; flex-aspect-ratio-cross-size-001).
 * NaN when that does not apply. Taller lines still make the container taller (embi_flex_content), as the content does
 * a ratio box with min-height: auto (block.c). */
static float ratio_cross(fctx *x)
{
    embi_lctx *c = x->c;
    embi_box *b = x->b;
    float ratio = 0;
    if (!x->row || !isnan(x->chd) || !embi_lay_ratio(c, b, &ratio) || !(ratio > 0) ||
        !embi_lay_is_auto(embi_lay_prop(c, b, EMBI_PROP_height)) ||
        (c->force_box == b && (c->force_hset || c->force_hnone)) || embi_lay_is_table(b)) {
        return NAN;
    }
    embi_edges bdr, pad;
    embi_lay_edges(c, b, 0, &bdr, &pad, nullptr);
    float eh = bdr.l + bdr.r + pad.l + pad.r;
    float ev = bdr.t + bdr.b + pad.t + pad.b;
    bool bb = is_box_sizing_bb(x, b) && !embi_lay_ratio_content(c, b);
    float h = bb ? (x->cw + eh) / ratio - ev : x->cw / ratio;
    h = h > x->maxh ? x->maxh : h;
    h = h < x->minh ? x->minh : h;
    return clamp0(h);
}

/* The strut sizes of the visibility: collapse items (CSS Flexbox 1 section 9.4 step 10 and section 4.4): the layout
 * runs once with them as ordinary items, and each takes the cross size of the line it was in (after align-content:
 * stretch) as its cross size (flexbox-collapsed-item-horiz-002/003, -baseline-001). Sets strut on those items. */
static void strut_sizes(fctx *x, fitem *items, uint32_t n, align_v ac)
{
    fitem *t = embi_calloc(n, sizeof *t);
    uint32_t *lb = embi_calloc(n + 1, sizeof *lb);
    float *lc = embi_calloc(n + 1, sizeof *lc);
    if (t == nullptr || lb == nullptr || lc == nullptr) {
        embi_free(t, n * sizeof *t);
        embi_free(lb, (n + 1) * sizeof *lb);
        embi_free(lc, (n + 1) * sizeof *lc);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        t[i] = items[i];
        if (items[i].collapsed) {
            x->no_collapse = true;
            item_prepare(x, &t[i]);
            x->no_collapse = false;
        }
    }
    float M = x->row ? x->cw : x->chd;
    bool indef = !x->row && isnan(x->chd);
    if (indef) {
        M = indef_main(x, t, n);
    }
    bool multi = x->wrapping && (!indef || x->maxh < INFINITY);
    uint32_t nl = build_lines(t, n, M, x->gap_m, multi, lb);
    if (indef && multi && nl > 1) {
        M = wrap_main(x, t, lb, nl, x->gap_m);
    }
    for (uint32_t k = 0; k < nl; k++) {
        resolve_line(x, t + lb[k], lb[k + 1] - lb[k], M, x->gap_m);
    }
    measure_items(x, t, n);
    line_cross_sizes(x, t, lb, nl, lc);
    float sum = x->gap_c * (float)(nl > 0 ? nl - 1 : 0);
    for (uint32_t k = 0; k < nl; k++) {
        sum += lc[k];
    }
    float X = x->row ? (!isnan(x->chd) ? x->chd : (sum < x->minh ? x->minh : (sum > x->maxh ? x->maxh : sum))) : x->cw;
    if (nl == 1 && !x->wrapping) {
        lc[0] = X;
    } else if (x->wrapping && nl > 0 && X > sum &&
               (ac.baseline || ac.pos == 0 || ac.pos == EMBI_KW_normal || ac.pos == EMBI_KW_stretch)) {
        for (uint32_t k = 0; k < nl; k++) {
            lc[k] += (X - sum) / (float)nl;
        }
    }
    for (uint32_t k = 0; k < nl; k++) {
        for (uint32_t i = lb[k]; i < lb[k + 1]; i++) {
            if (items[i].collapsed) {
                items[i].strut = lc[k];
                /* The strut is the whole line's cross size: no margins around it, and no baseline to share. */
                items[i].cms = items[i].cme = 0;
                items[i].cas = items[i].cae = false;
                items[i].cal = AL_START;
            }
        }
    }
    embi_free(t, n * sizeof *t);
    embi_free(lb, (n + 1) * sizeof *lb);
    embi_free(lc, (n + 1) * sizeof *lc);
}

void embi_flex_content(embi_lctx *c, embi_box *b, embi_frag *f, const embi_flex_in *in,
                       embi_flex_out *out)
{
    out->ch = 0;
    out->bl1 = NAN;
    out->bl2 = NAN;
    out->main_calc = false;

    fctx x;
    memset(&x, 0, sizeof x);
    x.c = c;
    x.b = b;
    x.f = f;
    const embi_value *dv = embi_lay_prop(c, b, EMBI_PROP_flex_direction);
    uint16_t dir = kw_of(dv);
    x.row = dir == 0 || dir == EMBI_KW_row || dir == EMBI_KW_row_reverse;
    x.reverse = dir == EMBI_KW_row_reverse || dir == EMBI_KW_column_reverse;
    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_flex_wrap);
    uint16_t wrap = kw_of(wv);
    bool wrapping = wrap == EMBI_KW_wrap || wrap == EMBI_KW_wrap_reverse;
    x.wrap_rev = wrap == EMBI_KW_wrap_reverse;
    x.wrapping = wrapping;
    const embi_value *dirv = embi_lay_prop(c, b, EMBI_PROP_direction);
    x.rtl = dirv != nullptr && dirv->kind == EMBI_V_KEYWORD && dirv->kw == EMBI_KW_rtl;
    x.flip_main = x.reverse != (x.row && x.rtl);
    x.cflip = x.row ? x.wrap_rev : (x.wrap_rev != x.rtl);
    x.cw = in->cw;
    x.chd = in->ch_def;
    x.minh = in->minh;
    x.maxh = in->maxh;
    x.ox = in->ox;
    x.oy = in->oy;
    float ratio_h = ratio_cross(&x);
    if (!isnan(ratio_h)) {
        x.chd = ratio_h;
    }
    /* The items' percentage heights resolve against the container's definite height in their intrinsic sizes too
     * (intrinsic-percent-replaced-024). */
    embi_ih ih = embi_lay_ih_push(c, x.chd);
    if (x.row) {
        x.gap_m = gap_len(&x, EMBI_PROP_column_gap, x.cw);
        x.gap_c = gap_len(&x, EMBI_PROP_row_gap, x.chd);
    } else {
        x.gap_m = gap_len(&x, EMBI_PROP_row_gap, x.chd);
        x.gap_c = gap_len(&x, EMBI_PROP_column_gap, x.cw);
    }
    x.ai = align_of(embi_lay_prop(c, b, EMBI_PROP_align_items));
    if (x.ai.pos == 0 && !x.ai.baseline) {
        x.ai.pos = EMBI_KW_normal;
    }
    align_v jc = align_of(embi_lay_prop(c, b, EMBI_PROP_justify_content));
    align_v ac = align_of(embi_lay_prop(c, b, EMBI_PROP_align_content));

    /* The items: the in-flow children, sorted by order (a stable insertion sort). */
    uint32_t n = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) == 0) {
            n++;
        }
    }
    uint32_t nalloc = n > 0 ? n : 1;
    fitem *items = embi_calloc(nalloc, sizeof *items);
    uint32_t *lb = embi_calloc(nalloc + 1, sizeof *lb);
    float *lcross = embi_calloc(nalloc + 1, sizeof *lcross);
    if (items == nullptr || lb == nullptr || lcross == nullptr) {
        embi_free(items, nalloc * sizeof *items);
        embi_free(lb, (nalloc + 1) * sizeof *lb);
        embi_free(lcross, (nalloc + 1) * sizeof *lcross);
        embi_lay_ih_pop(c, ih);
        return;
    }
    uint32_t m = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) == 0) {
            items[m++].box = k;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        item_prepare(&x, &items[i]);
    }
    {
        bool sorted = true;
        for (uint32_t i = 1; i < n && sorted; i++) {
            sorted = items[i - 1].order <= items[i].order;
        }
        if (!sorted) {
            /* A stable merge sort of an index array, then one permuted copy. */
            uint32_t *idx = embi_calloc(n, sizeof *idx);
            uint32_t *tmpi = embi_calloc(n, sizeof *tmpi);
            fitem *copy = embi_calloc(n, sizeof *copy);
            if (idx != nullptr && tmpi != nullptr && copy != nullptr) {
                for (uint32_t i = 0; i < n; i++) {
                    idx[i] = i;
                }
                for (uint32_t w = 1; w < n; w *= 2) {
                    for (uint32_t lo = 0; lo < n; lo += 2 * w) {
                        uint32_t mid = lo + w < n ? lo + w : n;
                        uint32_t hi = lo + 2 * w < n ? lo + 2 * w : n;
                        uint32_t p = lo, q = mid, o = lo;
                        while (p < mid && q < hi) {
                            tmpi[o++] = items[idx[q]].order < items[idx[p]].order ? idx[q++] : idx[p++];
                        }
                        while (p < mid) {
                            tmpi[o++] = idx[p++];
                        }
                        while (q < hi) {
                            tmpi[o++] = idx[q++];
                        }
                    }
                    uint32_t *sw = idx;
                    idx = tmpi;
                    tmpi = sw;
                }
                for (uint32_t i = 0; i < n; i++) {
                    copy[i] = items[idx[i]];
                }
                memcpy(items, copy, n * sizeof *items);
            }
            embi_free(idx, n * sizeof *idx);
            embi_free(tmpi, n * sizeof *tmpi);
            embi_free(copy, n * sizeof *copy);
        }
    }

    /* The main size of the container: the content width along a row, the definite height along a
     * column, or else the sum of the hypothetical sizes clamped by the limits (single line). */
    float M = x.row ? x.cw : x.chd;
    float gap = x.gap_m;
    bool indef = !x.row && isnan(x.chd);
    if (indef) {
        M = indef_main(&x, items, n);
    }

    /* The struts of the visibility: collapse items come from a layout where they are not collapsed. */
    for (uint32_t i = 0; i < n; i++) {
        if (items[i].collapsed) {
            strut_sizes(&x, items, n, ac);
            break;
        }
    }

    /* Lines. A single-line container, or one whose main size is not definite, has one line. A column with an
     * indefinite height breaks its lines at its max-height, the available main size, and is then as tall as its
     * longest line. */
    bool multi = wrapping && (!indef || x.maxh < INFINITY);
    uint32_t nl = build_lines(items, n, M, gap, multi, lb);
    if (indef && multi && nl > 1) {
        M = wrap_main(&x, items, lb, nl, gap);
    }

    /* calc-size() on the height of a column (CSS Values 5 section 10.1): the intrinsic main size M (the
     * line sizes, clamped) is the basis; the calc-sum gives the container's main size, which the
     * flexible lengths then resolve against (section 9.7). The lines were built with the indefinite size. */
    bool main_calc = false;
    if (indef && in->main_expr != nullptr) {
        float r = embi_lay_size_eval(c, b, in->main_expr, M + in->main_pad, in->main_pct);
        if (!isnan(r)) {
            float fm = r - in->main_pad;
            fm = fm < 0 ? 0 : fm;
            M = fm < x.minh ? x.minh : (fm > x.maxh ? x.maxh : fm);
            main_calc = true;
        }
    }

    /* Main sizes, line by line. */
    for (uint32_t k = 0; k < nl; k++) {
        resolve_line(&x, items + lb[k], lb[k + 1] - lb[k], M, gap);
    }

    /* Each item is laid out at its target main size: its cross size comes from the layout. */
    measure_items(&x, items, n);

    /* Line cross sizes (section 9.4 step 8). */
    line_cross_sizes(&x, items, lb, nl, lcross);

    /* The container's cross size X: the content height along a row (definite, or the lines clamped
     * by the limits), or the content width along a column. */
    float X;
    if (x.row) {
        if (!isnan(ratio_h)) {
            /* A ratio height: lines taller than it make the container taller when its min-height is auto (the
             * content minimum of a ratio box, CSS Sizing 4 section 5.1; flex-aspect-ratio-040). */
            float sum = x.gap_c * (float)(nl > 0 ? nl - 1 : 0);
            for (uint32_t k = 0; k < nl; k++) {
                sum += lcross[k];
            }
            bool grows = embi_lay_is_auto(embi_lay_prop(c, b, EMBI_PROP_min_height)) && (b->flags & EMBI_BF_SCROLL) == 0;
            X = grows && sum > ratio_h ? (sum > x.maxh ? fmaxf(x.maxh, ratio_h) : sum) : ratio_h;
        } else if (!isnan(x.chd)) {
            X = x.chd;
        } else {
            float sum = x.gap_c * (float)(nl > 0 ? nl - 1 : 0);
            for (uint32_t k = 0; k < nl; k++) {
                sum += lcross[k];
            }
            X = sum < x.minh ? x.minh : (sum > x.maxh ? x.maxh : sum);
        }
    } else {
        X = x.cw;
    }
    /* A single-line container (flex-wrap: nowrap) has one line that is its whole cross size. A
     * multi-line one keeps the line's own size even with a single line (section 9.4 step 8). */
    if (nl == 1 && !wrapping) {
        lcross[0] = X;
    }

    /* align-content: the free cross space between the lines (section 9.4 step 17). A single line
     * takes the whole cross size. */
    float lead_c = 0, between_c = x.gap_c;
    if (wrapping && nl > 0) {
        float sumc = x.gap_c * (float)(nl - 1);
        for (uint32_t k = 0; k < nl; k++) {
            sumc += lcross[k];
        }
        float freec = X - sumc;
        uint16_t acp = ac.pos;
        if (ac.safe && freec < 0 && (acp == EMBI_KW_flex_end || acp == EMBI_KW_flex_start || acp == EMBI_KW_end ||
                                     acp == EMBI_KW_center)) {
            acp = EMBI_KW_start; /* safe: overflowing lines align to the start edge */
        }
        /* start and end are writing-mode edges: with wrap-reverse the cross start is the bottom, so start
         * packs the lines at the physical top (the logical end) and end at the bottom (the logical start). */
        if (x.wrap_rev && acp == EMBI_KW_start) {
            acp = EMBI_KW_flex_end;
        } else if (x.wrap_rev && acp == EMBI_KW_end) {
            acp = EMBI_KW_flex_start;
        }
        float nf = (float)nl;
        if (ac.baseline || acp == 0 || acp == EMBI_KW_normal || acp == EMBI_KW_stretch) {
            if (freec > 0) {
                for (uint32_t k = 0; k < nl; k++) {
                    lcross[k] += freec / nf;
                }
            }
        } else if (acp == EMBI_KW_flex_end || acp == EMBI_KW_end) {
            lead_c = freec;
        } else if (acp == EMBI_KW_center) {
            lead_c = freec / 2;
        } else if (acp == EMBI_KW_space_between) {
            if (freec > 0 && nl > 1) {
                between_c += freec / (nf - 1);
            }
        } else if (acp == EMBI_KW_space_around) {
            if (freec > 0) {
                between_c += freec / nf;
                lead_c = freec / (2 * nf);
            } else {
                lead_c = freec / 2;
            }
        } else if (acp == EMBI_KW_space_evenly) {
            if (freec > 0) {
                between_c += freec / (nf + 1);
                lead_c = freec / (nf + 1);
            } else {
                lead_c = freec / 2;
            }
        }
    }
    {
        float cl = lead_c;
        for (uint32_t k = 0; k < nl; k++) {
            for (uint32_t i = lb[k]; i < lb[k + 1]; i++) {
                items[i].lstart = cl;
            }
            cl += lcross[k] + between_c;
        }
    }

    /* Cross alignment per line. A stretched item whose layout differs from its stretched size is
     * laid out again at that size, so its content sees the used height or width. */
    for (uint32_t k = 0; k < nl; k++) {
        uint32_t cnt = lb[k + 1] - lb[k];
        fitem *its = items + lb[k];
        align_line(&x, its, cnt, lcross[k]);
        for (uint32_t i = 0; i < cnt; i++) {
            fitem *it = &its[i];
            if (it->lr.frag == nullptr || it->collapsed) {
                continue;
            }
            float cur = x.row ? it->lr.frag->h : it->lr.frag->w;
            /* A stretched item is definite in the cross axis, so its content sees the size even when its
             * layout already had it. A plain block whose content does not read a definite height keeps
             * its first layout: along a row only its height changes (W3p). */
            if ((it->stretched || fabsf(cur - it->cross) > 0.001f) && !it->lr.selfc &&
                it->lr.frag->first != nullptr && embi_lay_height_independent(c, it->box) &&
                (x.row || fabsf(cur - it->cross) <= 0.001f)) {
                if (x.row) {
                    it->lr.frag->h = it->cross;
                }
                if (it->stretched && x.row) {
                    it->lr.frag->flags |= EMBI_FF_DEF_H;
                }
                continue;
            }
            if (it->stretched || fabsf(cur - it->cross) > 0.001f) {
                /* An unflexed item with an auto main size and a preferred aspect ratio takes its main size
                 * from its stretched cross size through the ratio (an image stretched to 40px high is 40px
                 * wide too), clamped by its min and max main sizes. */
                if (it->stretched && isnan(it->spec_main) && it->basis_auto && it->grow == 0 &&
                    it->ratio > 0 && it->target == it->hyp) {
                    float cc = it->cross - it->bdr_cross;
                    cc = cc < 0 ? 0 : cc;
                    float tm = (x.row ? cc * it->ratio : cc / it->ratio) + it->bdr_main;
                    if (is_box_sizing_bb(&x, it->box) && !embi_lay_ratio_content(c, it->box)) {
                        tm = x.row ? it->cross * it->ratio : it->cross / it->ratio; /* a border-box ratio */
                        tm = tm < it->bdr_main ? it->bdr_main : tm;
                    }
                    tm = tm < it->min_main ? it->min_main : (tm > it->max_main ? it->max_main : tm);
                    it->target = tm;
                }
                embi_lres r;
                if (lay_item(&x, it, x.cw, true, it->target, true, it->cross, &r)) {
                    it->lr = r;
                }
                if (it->stretched && x.row && it->lr.frag != nullptr) {
                    it->lr.frag->flags |= EMBI_FF_DEF_H;
                }
            }
        }
    }

    /* Main positions per line (justify-content and auto margins). */
    for (uint32_t k = 0; k < nl; k++) {
        justify_line(&x, items + lb[k], lb[k + 1] - lb[k], M, gap, jc.pos, jc.safe);
    }

    /* Physical positions of the items, their margins, and their fragments in order. */
    for (uint32_t i = 0; i < n; i++) {
        fitem *it = &items[i];
        embi_frag *cf = it->lr.frag;
        if (cf == nullptr || it->collapsed) {
            continue;
        }
        float mbs = it->mpos + it->mms;
        float P = x.flip_main ? M - (mbs + it->target) : mbs;
        float cbs = it->lstart + it->cpos + it->cms;
        float C = x.cflip ? X - (cbs + it->cross) : cbs;
        float px = x.row ? P : C;
        float py = x.row ? C : P;
        cf->x = x.ox + px;
        cf->y = x.oy + py;
        /* The used margins, physical: left, top, right, bottom. */
        float ml, mt, mr, mbm;
        if (x.row) {
            ml = x.flip_main ? it->mme : it->mms;
            mr = x.flip_main ? it->mms : it->mme;
            mt = x.cflip ? it->cme : it->cms;
            mbm = x.cflip ? it->cms : it->cme;
        } else {
            mt = x.flip_main ? it->mme : it->mms;
            mbm = x.flip_main ? it->mms : it->mme;
            ml = x.cflip ? it->cme : it->cms;
            mr = x.cflip ? it->cms : it->cme;
        }
        embi_lay_frag_set_mg(c->ld, cf, ml, mt, mr, mbm);
        embi_lay_frag_append(f, cf);
        embi_lay_ov_add_child(f, cf);
    }

    /* The baselines of the container (section 8.5). The first baseline comes from the line at the block
     * start (the first line, or the last one with wrap-reverse; in a column, the line at the inline
     * start), the last baseline from the line at the opposite side. In a row, a line with items aligned to a baseline gives that group's shared baseline
     * (preferably a group of the same kind as the baseline asked for); else the startmost (first) or
     * endmost (last) item of the line gives its own baseline. */
    if (n > 0 && nl > 0) {
        for (int which = 0; which < 2; which++) {
            bool want_first = which == 0;
            uint32_t k = (want_first != x.wrap_rev) ? 0 : nl - 1;
            uint32_t cnt = lb[k + 1] - lb[k];
            fitem *its = items + lb[k];
            fitem *pick = nullptr;
            if (x.row) {
                int pref = want_first ? AL_BASELINE : AL_LBASELINE;
                int other = want_first ? AL_LBASELINE : AL_BASELINE;
                for (uint32_t i = 0; i < cnt && pick == nullptr; i++) {
                    pick = its[i].cal == pref && its[i].lr.frag != nullptr ? &its[i] : nullptr;
                }
                for (uint32_t i = 0; i < cnt && pick == nullptr; i++) {
                    pick = its[i].cal == other && its[i].lr.frag != nullptr ? &its[i] : nullptr;
                }
            }
            bool group = pick != nullptr;
            bool use_last = want_first ? (group && pick->cal == AL_LBASELINE) : !(group && pick->cal == AL_BASELINE);
            if (!group) {
                /* A reversed container starts at the far end of its line. */
                fitem *it = want_first != x.reverse ? &its[0] : &its[cnt - 1];
                pick = it->lr.frag != nullptr ? it : nullptr;
            }
            if (pick != nullptr) {
                float v = pick->lr.frag->y + item_bl(pick, use_last);
                if (want_first) {
                    out->bl1 = v;
                } else {
                    out->bl2 = v;
                }
            }
        }
    }
    out->ch = x.row ? X : M;
    out->main_calc = main_calc && !x.row;

    /* Out-of-flow children: as the sole flex item of the container (section 4.1), at the justify-content
     * fraction along the main axis and the child's align-self fraction along the cross axis, in the
     * content box. position.c places them when their insets are auto. */
    {
        float cont[4] = {x.ox, x.oy, x.ox + x.cw, x.oy + (x.row ? X : M)};
        float mfrac = frac_of_justify(&x, jc.pos);
        for (embi_box *k = b->first; k != nullptr; k = k->next) {
            if ((k->flags & EMBI_BF_ABSPOS) == 0) {
                continue;
            }
            float cfrac = cal_frac(&x, k);
            float frac[2] = {x.row ? mfrac : cfrac, x.row ? cfrac : mfrac};
            bool flip[2] = {x.row ? x.flip_main : x.cflip, x.row ? x.cflip : x.flip_main};
            embi_lay_abspos_flex(c, k, b, cont, frac, flip);
        }
    }

    embi_free(items, nalloc * sizeof *items);
    embi_free(lb, (nalloc + 1) * sizeof *lb);
    embi_free(lcross, (nalloc + 1) * sizeof *lcross);
    embi_lay_ih_pop(c, ih);
}

/* The min-content and max-content sizes of a flex container (CSS Flexbox 1 section 9.9): along a
 * row, the max-content is the sum of the item contributions, the min-content their sum (a single
 * line) or the largest (a multi-line container); along a column, both are the largest contribution. */
static bool is_box_sizing_bb_c(embi_lctx *c, embi_box *k)
{
    const embi_value *bs = embi_lay_prop(c, k, EMBI_PROP_box_sizing);
    return bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
}

/* The inline-axis edges of a row item (border and padding, both sides). */
static float row_edges(const embi_edges *bdr, const embi_edges *pad)
{
    return bdr->l + bdr->r + pad->l + pad->r;
}

/* A px length of a size property, as a border-box size, or NaN when it is not a plain length (auto,
 * a keyword, a percentage). */
static float plain_len_bb(embi_lctx *c, embi_box *k, embi_prop_id p, float edges)
{
    const embi_value *v = embi_lay_prop(c, k, p);
    if (v == nullptr || embi_lay_is_auto(v) || v->kind == EMBI_V_KEYWORD || v->kind == EMBI_V_PERCENT) {
        return NAN;
    }
    float len = embi_lay_len(c, k, v, NAN);
    if (isnan(len)) {
        return NAN;
    }
    return clamp0(is_box_sizing_bb_c(c, k) ? len : len + edges);
}

/* The intrinsic widths of a column container (CSS Flexbox 1 section 9.9.2, with Chromium's treatment
 * of wrapping): a line is as wide as its widest item contribution (items that are baseline-aligned
 * share a baseline at the border-box start (first) or end (last) edge, which can widen the line); the
 * container is the sum of its lines. A wrapping container with a definite height breaks its lines where
 * the items' hypothetical main sizes (flex-basis or height, within min and max height) fill that height. */
static void col_intrinsic(embi_lctx *c, embi_box *b, bool wrapping, float *mn, float *mx)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, b, 0, &bdr, &pad, &mar);
    float vedges = bdr.t + bdr.b + pad.t + pad.b;
    float H = NAN;
    if (wrapping) {
        H = plain_len_bb(c, b, EMBI_PROP_height, vedges);
        if (isnan(H)) {
            /* An auto height breaks the lines at a definite max-height, as the layout does (col-wrap-002). */
            H = plain_len_bb(c, b, EMBI_PROP_max_height, vedges);
        }
        if (!isnan(H)) {
            float lo = plain_len_bb(c, b, EMBI_PROP_min_height, vedges);
            float hi = plain_len_bb(c, b, EMBI_PROP_max_height, vedges);
            if (!isnan(hi) && H > hi) {
                H = hi;
            }
            if (!isnan(lo) && H < lo) {
                H = lo;
            }
            H = clamp0(H - vedges);
        }
    }
    float rgap = 0, cgap = 0;
    {
        const embi_value *g = embi_lay_prop(c, b, EMBI_PROP_row_gap);
        if (g != nullptr && g->kind != EMBI_V_KEYWORD) {
            float v = embi_lay_len(c, b, g, NAN);
            rgap = isnan(v) ? 0 : clamp0(v);
        }
        g = embi_lay_prop(c, b, EMBI_PROP_column_gap);
        if (g != nullptr && g->kind != EMBI_V_KEYWORD) {
            float v = embi_lay_len(c, b, g, NAN);
            cgap = isnan(v) ? 0 : clamp0(v);
        }
    }
    align_v ai = align_of(embi_lay_prop(c, b, EMBI_PROP_align_items));
    float sum[2] = {0, 0};
    float line_max[2] = {0, 0};            /* widest non-baseline contribution of the open line */
    float fs[2] = {-INFINITY, -INFINITY};  /* first-baseline group: max s */
    float fe[2] = {-INFINITY, -INFINITY};  /*   and max e */
    float ls[2] = {-INFINITY, -INFINITY};  /* last-baseline group */
    float le[2] = {-INFINITY, -INFINITY};
    uint32_t lines = 0;
    float acc = 0;
    bool open = false;
    /* The items in order-modified document order (col-wrap-006). */
    uint32_t nk = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        nk++;
    }
    embi_box **ord = nk > 0 ? embi_calloc(nk, sizeof *ord) : nullptr;
    int32_t *okey = nk > 0 ? embi_calloc(nk, sizeof *okey) : nullptr;
    uint32_t no = 0;
    if (ord != nullptr && okey != nullptr) {
        for (embi_box *k = b->first; k != nullptr; k = k->next) {
            const embi_value *ov = embi_lay_prop(c, k, EMBI_PROP_order);
            int32_t key = ov != nullptr && (ov->kind == EMBI_V_INTEGER || ov->kind == EMBI_V_NUMBER) ? (int32_t)ov->num : 0;
            uint32_t j = no++;
            while (j > 0 && okey[j - 1] > key) { /* stable insertion */
                ord[j] = ord[j - 1];
                okey[j] = okey[j - 1];
                j--;
            }
            ord[j] = k;
            okey[j] = key;
        }
    }
    uint32_t oi = 0;
    for (embi_box *k = no > 0 ? ord[0] : b->first;; k = no > 0 ? (++oi < no ? ord[oi] : nullptr) : (k != nullptr ? k->next : nullptr)) {
        bool item = k != nullptr && (k->flags & EMBI_BF_ABSPOS) == 0;
        float hyp = 0;
        if (item) {
            embi_edges kb, kp, km;
            embi_lay_edges(c, k, 0, &kb, &kp, &km);
            float kv = kb.t + kb.b + kp.t + kp.b;
            float mv = (isnan(km.t) ? 0 : km.t) + (isnan(km.b) ? 0 : km.b);
            float h = NAN;
            const embi_value *bv = embi_lay_prop(c, k, EMBI_PROP_flex_basis);
            if (bv != nullptr && bv->kind != EMBI_V_KEYWORD && bv->kind != EMBI_V_PERCENT) {
                float len = embi_lay_len(c, k, bv, NAN);
                if (!isnan(len)) {
                    h = clamp0(is_box_sizing_bb_c(c, k) ? len : len + kv);
                }
            }
            if (isnan(h)) {
                h = plain_len_bb(c, k, EMBI_PROP_height, kv);
            }
            float rr = 0;
            if (isnan(h) && !isnan(H) && !(embi_lay_ratio(c, k, &rr) && rr > 0)) {
                /* An auto height needs the item's content height at its max-content width: lay it out
                 * once (the lines of a wrapping column depend on it). */
                float cm0 = 0, cm1 = 0;
                embi_lay_intrinsic_outer(c, k, &cm0, &cm1);
                float wmeas = cm1 - (isnan(km.l) ? 0 : km.l) - (isnan(km.r) ? 0 : km.r);
                embi_cs mcs;
                memset(&mcs, 0, sizeof mcs);
                mcs.avail_w = wmeas;
                mcs.cb_w = wmeas;
                mcs.cb_h = NAN;
                mcs.fit = EMBI_FIT_SHRINK;
                embi_lres mr;
                memset(&mr, 0, sizeof mr);
                embi_lay_box(c, k, &mcs, &mr);
                if (mr.frag != nullptr) {
                    h = mr.frag->h;
                }
                embi_lay_clear_frags(c, k);
            }
            if (isnan(h) && embi_lay_ratio(c, k, &rr) && rr > 0) {
                /* A preferred aspect ratio gives the height from the width: the specified one, or else
                 * the max-content width (a percentage is unresolvable here). */
                float cm0 = 0, cm1 = 0;
                embi_lay_intrinsic_outer(c, k, &cm0, &cm1);
                float wq = plain_len_bb(c, k, EMBI_PROP_width, 0);
                float wd = !isnan(wq) ? wq : cm1 - (isnan(km.l) ? 0 : km.l) - (isnan(km.r) ? 0 : km.r);
                h = clamp0(wd) / rr;
            }
            if (isnan(h)) {
                h = kv;
            }
            float lo = plain_len_bb(c, k, EMBI_PROP_min_height, kv);
            float hi = plain_len_bb(c, k, EMBI_PROP_max_height, kv);
            if (!isnan(hi) && h > hi) {
                h = hi;
            }
            if (!isnan(lo) && h < lo) {
                h = lo;
            }
            hyp = is_collapsed(c, k) ? 0.0f : h + mv;
        }
        bool close = k == nullptr;
        if (item && open && !isnan(H) && acc + rgap + hyp > H + 0.01f) {
            close = true;
        }
        if (close && open) {
            for (int p = 0; p < 2; p++) {
                float w = line_max[p];
                if (fs[p] > -INFINITY && fs[p] + fe[p] > w) {
                    w = fs[p] + fe[p];
                }
                if (ls[p] > -INFINITY && ls[p] + le[p] > w) {
                    w = ls[p] + le[p];
                }
                sum[p] += w + (lines > 0 ? cgap : 0);
                line_max[p] = 0;
                fs[p] = fe[p] = ls[p] = le[p] = -INFINITY;
            }
            lines++;
            open = false;
            acc = 0;
        }
        if (k == nullptr) {
            break;
        }
        if (!item) {
            continue;
        }
        acc += (open ? rgap : 0) + hyp;
        open = true;
        float cmn = 0, cmx = 0;
        embi_lay_intrinsic_outer(c, k, &cmn, &cmx);
        embi_edges kb, kp, km;
        embi_lay_edges(c, k, 0, &kb, &kp, &km);
        float ml = isnan(km.l) ? 0 : km.l, mr = isnan(km.r) ? 0 : km.r;
        align_v as = align_of(embi_lay_prop(c, k, EMBI_PROP_align_self));
        if (as.pos == EMBI_KW_auto || (as.pos == 0 && !as.baseline)) {
            as = ai;
        }
        for (int p = 0; p < 2; p++) {
            float cw = p == 0 ? cmn : cmx;
            if (as.baseline && !as.last) {
                float s = ml, e = cw - ml;
                fs[p] = s > fs[p] ? s : fs[p];
                fe[p] = e > fe[p] ? e : fe[p];
            } else if (as.baseline && as.last) {
                float s = cw - mr, e = mr;
                ls[p] = s > ls[p] ? s : ls[p];
                le[p] = e > le[p] ? e : le[p];
            } else if (cw > line_max[p]) {
                line_max[p] = cw;
            }
        }
    }
    embi_free(ord, nk * sizeof *ord);
    embi_free(okey, nk * sizeof *okey);
    *mn = sum[0];
    *mx = sum[1];
}

void embi_flex_intrinsic(embi_lctx *c, embi_box *b, float *mn, float *mx)
{
    const embi_value *dv = embi_lay_prop(c, b, EMBI_PROP_flex_direction);
    uint16_t dir = kw_of(dv);
    bool row = dir == 0 || dir == EMBI_KW_row || dir == EMBI_KW_row_reverse;
    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_flex_wrap);
    uint16_t wrap = kw_of(wv);
    bool wrapping = wrap == EMBI_KW_wrap || wrap == EMBI_KW_wrap_reverse;
    float gap = 0;
    if (row) {
        const embi_value *g = embi_lay_prop(c, b, EMBI_PROP_column_gap);
        if (g != nullptr && g->kind != EMBI_V_KEYWORD) {
            float v = embi_lay_len(c, b, g, NAN);
            gap = isnan(v) ? 0 : clamp0(v);
        }
    }
    uint32_t cnt = 0, vis = 0; /* the in-flow items, and those that are not collapsed (line_gaps) */
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if ((k->flags & EMBI_BF_ABSPOS) == 0) {
            cnt++;
            vis += row && !is_collapsed(c, k) ? 1u : 0u;
        }
    }
    if (cnt == 0) {
        *mn = 0;
        *mx = 0;
        return;
    }
    if (!row) {
        col_intrinsic(c, b, wrapping, mn, mx);
        return;
    }

    /* Row containers (CSS Flexbox 1 section 9.9.1, the ideal algorithm with Chromium's treatment of the
     * items that cannot flex). For each item: its outer flex base size, and its min-content and
     * max-content contributions (the outer content size, not smaller than a definite width, clamped by
     * the flex base size as a maximum when it cannot grow and as a minimum when it cannot shrink, then by
     * its min and max main sizes, the minimum being the automatic one when it is auto). The desired flex
     * fraction of an item is (contribution - base) / grow when positive, / (shrink * base) when negative
     * (-infinity when that is zero) and 0 for a zero grow factor. A line's chosen fraction is the
     * greatest of them, divided by the sum of the grow factors when that is below one. With a positive
     * chosen fraction an item is base + grow * fraction; otherwise it is its base size when it cannot
     * shrink and its contribution when it can. The sizes are clamped by the min and max and summed.
     * The multi-line min-content size (9.9.1.3) is the largest min-content contribution. */
    float sum_mx = 0, sum_mn = 0, multi_mn = 0;
    float *base = embi_calloc(cnt, sizeof *base);
    float *grow = embi_calloc(cnt, sizeof *grow);
    float *shr = embi_calloc(cnt, sizeof *shr);
    float *con[2] = {embi_calloc(cnt, sizeof(float)), embi_calloc(cnt, sizeof(float))};
    float *lo = embi_calloc(cnt, sizeof *lo);
    float *hi = embi_calloc(cnt, sizeof *hi);
    float *base_mn = embi_calloc(cnt, sizeof *base_mn);
    if (base == nullptr || grow == nullptr || shr == nullptr || con[0] == nullptr || con[1] == nullptr ||
        lo == nullptr || hi == nullptr || base_mn == nullptr) {
        embi_free(base_mn, cnt * sizeof *base_mn);
        embi_free(base, cnt * sizeof *base);
        embi_free(grow, cnt * sizeof *grow);
        embi_free(shr, cnt * sizeof *shr);
        embi_free(con[0], cnt * sizeof(float));
        embi_free(con[1], cnt * sizeof(float));
        embi_free(lo, cnt * sizeof *lo);
        embi_free(hi, cnt * sizeof *hi);
        *mn = 0;
        *mx = 0;
        return;
    }
    uint32_t i = 0;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->flags & EMBI_BF_ABSPOS) {
            continue;
        }
        if (is_collapsed(c, k)) {
            /* A strut: no main size (all of base, limits and contributions stay zero). */
            i++;
            continue;
        }
        embi_edges bdr, pad, mar;
        embi_lay_edges(c, k, 0, &bdr, &pad, &mar);
        bool bb = is_box_sizing_bb_c(c, k);
        float edges = row_edges(&bdr, &pad);
        float margins = (isnan(mar.l) ? 0 : mar.l) + (isnan(mar.r) ? 0 : mar.r);
        float cmn = 0, cmx = 0;
        embi_lay_intrinsic(c, k, &cmn, &cmx);
        cmn += edges;
        cmx += edges;
        /* The definite width (a border-box size), and the min and max widths. */
        const embi_value *wprop = embi_lay_prop(c, k, EMBI_PROP_width);
        float wbb = NAN;
        if (wprop != nullptr && !embi_lay_is_auto(wprop) && wprop->kind != EMBI_V_KEYWORD && wprop->kind != EMBI_V_PERCENT) {
            float w = embi_lay_len(c, k, wprop, NAN);
            if (!isnan(w)) {
                wbb = clamp0(bb ? w : w + edges);
            }
        }
        const embi_value *bv = embi_lay_prop(c, k, EMBI_PROP_flex_basis);
        float bs = !isnan(wbb) ? wbb : cmx;
        bool content_basis = isnan(wbb); /* the flex base size is the max-content size */
        if (bv != nullptr && bv->kind != EMBI_V_KEYWORD && bv->kind != EMBI_V_PERCENT) {
            float len = embi_lay_len(c, k, bv, NAN);
            if (!isnan(len)) {
                bs = clamp0(bb ? len : len + edges);
                content_basis = false;
            }
        } else if (bv != nullptr && kw_of(bv) == EMBI_KW_content) {
            bs = cmx;
            content_basis = true;
        }
        const embi_value *minv = embi_lay_prop(c, k, EMBI_PROP_min_width);
        const embi_value *maxv = embi_lay_prop(c, k, EMBI_PROP_max_width);
        float minw = 0, maxw = INFINITY;
        if (minv != nullptr && minv->kind != EMBI_V_KEYWORD && minv->kind != EMBI_V_PERCENT) {
            float v = embi_lay_len(c, k, minv, NAN);
            if (!isnan(v)) {
                minw = clamp0(bb ? v : v + edges);
            }
        } else if (embi_lay_is_auto(minv)) {
            const embi_value *ovx = embi_lay_prop(c, k, EMBI_PROP_overflow_x);
            uint16_t okw = kw_of(ovx);
            bool scroll = okw == EMBI_KW_hidden || okw == EMBI_KW_scroll || okw == EMBI_KW_auto;
            minw = scroll ? 0 : (!isnan(wbb) && wbb < cmn ? wbb : cmn);
        }
        if (maxv != nullptr && maxv->kind != EMBI_V_KEYWORD && maxv->kind != EMBI_V_PERCENT) {
            float v = embi_lay_len(c, k, maxv, NAN);
            if (!isnan(v)) {
                maxw = clamp0(bb ? v : v + edges);
            }
        }
        if (minw < edges) {
            minw = edges;
        }
        if (maxw < minw) {
            maxw = minw;
        }
        base[i] = bs + margins;
        grow[i] = num_of(embi_lay_prop(c, k, EMBI_PROP_flex_grow), 0.0f);
        shr[i] = num_of(embi_lay_prop(c, k, EMBI_PROP_flex_shrink), 1.0f);
        lo[i] = minw + margins;
        hi[i] = (isinf(maxw) ? INFINITY : maxw + margins);
        for (int pass = 0; pass < 2; pass++) {
            float start = pass == 0 ? cmx : cmn;
            if (!isnan(wbb) && wbb > start) {
                start = wbb;
            }
            if (grow[i] == 0 && start > bs) {
                start = bs;
            }
            /* A base size that is the item's max-content size does not hold up the min-content size of the
             * container: the item contributes its min-content size there (Chromium; intrinsic-size/row-001,
             * gap-015). A definite base size does (intrinsic-size/row-002). */
            if (shr[i] == 0 && start < bs && !(pass == 1 && content_basis)) {
                start = bs;
            }
            start = start < minw ? minw : (start > maxw ? maxw : start);
            con[pass][i] = start + margins;
        }
        base_mn[i] = content_basis ? con[1][i] : base[i];
        i++;
    }

    for (int pass = 0; pass < 2; pass++) {
        const float *cn = con[pass];
        const float *bq = pass == 0 ? base : base_mn;
        float chosen = -INFINITY;
        float gsum = 0;
        for (uint32_t q = 0; q < cnt; q++) {
            gsum += grow[q];
            float diff = cn[q] - bq[q];
            float d;
            if (diff > 0) {
                d = grow[q] > 0 ? diff / grow[q] : 0.0f;
            } else if (diff < 0) {
                float den = shr[q] * bq[q];
                d = den > 0 ? diff / den : -INFINITY;
            } else {
                d = 0.0f;
            }
            if (d > chosen) {
                chosen = d;
            }
        }
        if (chosen > 0 && gsum < 1.0f) {
            chosen = gsum > 0 ? chosen / gsum : 0.0f;
        }
        float total = gap * (float)(vis > 0 ? vis - 1 : 0);
        for (uint32_t q = 0; q < cnt; q++) {
            float sz;
            if (chosen > 0) {
                sz = bq[q] + grow[q] * chosen;
            } else {
                sz = shr[q] == 0 ? bq[q] : cn[q];
            }
            sz = sz < lo[q] ? lo[q] : (sz > hi[q] ? hi[q] : sz);
            /* In a multi-line container a negative outer size (a negative margin) takes nothing away from the
             * others (row-wrap-002, as Chromium and WebKit); on a single line it does (negative-margins-001). */
            total += wrapping && sz < 0 ? 0 : sz;
        }
        if (pass == 0) {
            sum_mx = total;
        } else {
            sum_mn = total;
        }
    }
    for (uint32_t q = 0; q < cnt; q++) {
        if (con[1][q] > multi_mn) {
            multi_mn = con[1][q];
        }
    }
    embi_free(base, cnt * sizeof *base);
    embi_free(grow, cnt * sizeof *grow);
    embi_free(shr, cnt * sizeof *shr);
    embi_free(con[0], cnt * sizeof(float));
    embi_free(con[1], cnt * sizeof(float));
    embi_free(lo, cnt * sizeof *lo);
    embi_free(hi, cnt * sizeof *hi);
    embi_free(base_mn, cnt * sizeof *base_mn);
    *mx = sum_mx;
    *mn = wrapping ? multi_mn : sum_mn;
}

void embi_lay_flex(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    /* block_pass lays out a flex container's content through embi_flex_content. */
    embi_lay_block(c, b, cs, out);
}
