/*
 * Floats (CSS 2.1 sections 9.5.1 and 9.5.2, W3b). A float is laid out as a BFC root (its contents
 * never share an exclusion space with the outside), then placed in the exclusion space of the BFC
 * it floats in:
 *
 *   - rule 6 and 7: its margin-box top is not above the top of any float placed before it (the
 *     block flow position it is given is the lower bound, which covers the earlier blocks and
 *     lines);
 *   - clear (CSS 2.1 9.5.2) applies to floats too: the margin box top goes below the floats it clears;
 *   - rules 1 to 5: it is placed at the highest position where the margin box fits in the free
 *     inline interval, at the left edge (left floats) or the right edge (right floats) of that
 *     interval, moving down past the lowest float that blocks it when no position fits.
 *
 * A float that is wider than its containing block and fits nowhere is placed at the edge of its
 * containing block, at the first position with no float in the way (rule 1 and the usual
 * behaviour of browsers for oversized floats).
 *
 * Auto margins of a float are zero (CSS 2.1 section 10.3.5 for floats), so its margin box is its
 * border box plus the specified margins.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

bool embi_lay_float_out_of_flow(const embi_box *b)
{
    return (b->flags & EMBI_BF_FLOAT) != 0;
}

bool embi_lay_float_left(const embi_lctx *c, const embi_box *b)
{
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_float);
    if (v == nullptr || v->kind != EMBI_V_KEYWORD) {
        return false;
    }
    if (v->kw == EMBI_KW_left) {
        return true;
    }
    if (v->kw == EMBI_KW_inline_start) {
        return !embi_lay_cb_rtl(c, b);
    }
    if (v->kw == EMBI_KW_inline_end) {
        return embi_lay_cb_rtl(c, b);
    }
    return false;
}

uint8_t embi_lay_clear_sides(const embi_lctx *c, const embi_box *b)
{
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_clear);
    if (v == nullptr || v->kind != EMBI_V_KEYWORD) {
        return 0;
    }
    bool rtl = embi_lay_cb_rtl(c, b);
    switch (v->kw) {
    case EMBI_KW_left:
        return EMBI_CLR_LEFT;
    case EMBI_KW_right:
        return EMBI_CLR_RIGHT;
    case EMBI_KW_both:
        return EMBI_CLR_LEFT | EMBI_CLR_RIGHT;
    case EMBI_KW_inline_start:
        return rtl ? EMBI_CLR_RIGHT : EMBI_CLR_LEFT;
    case EMBI_KW_inline_end:
        return rtl ? EMBI_CLR_LEFT : EMBI_CLR_RIGHT;
    default:
        return 0;
    }
}

static float nz(float v)
{
    return isnan(v) ? 0.0f : v;
}

embi_frag *embi_lay_float_prepare(embi_lctx *c, embi_box *b, float cx, float cw, float cb_h, embi_float_geom *g)
{
    /* Shrink-to-fit width against the containing block's content width. The float is its own
     * BFC, so it gets no exclusion space and no frame (excl nullptr). */
    embi_cs cs = {cw, cw, cb_h, EMBI_FIT_SHRINK, nullptr, cx, 0.0f, false, 0.0f, 0.0f};
    embi_lres r;
    memset(&r, 0, sizeof r);
    embi_lay_box(c, b, &cs, &r);
    if (r.frag == nullptr) {
        return nullptr;
    }
    g->ml = nz(r.ml);
    g->mr = nz(r.mr);
    g->mt = nz(r.mt);
    g->mb = nz(r.mb);
    g->mw = g->ml + r.frag->w + g->mr;
    g->mh = g->mt + r.frag->h + g->mb;
    g->left = embi_lay_float_left(c, b);
    return r.frag;
}

embi_frag *embi_lay_float(embi_lctx *c, embi_box *b, embi_excl *ex, float cx, float cw, float cb_h,
                          float top, float *bx, float *by)
{
    embi_float_geom g;
    embi_frag *f = embi_lay_float_prepare(c, b, cx, cw, cb_h, &g);
    if (f != nullptr) {
        embi_lay_float_place(c, b, f, &g, ex, cx, cw, top, bx, by);
    }
    return f;
}

void embi_lay_float_place(embi_lctx *c, embi_box *b, const embi_frag *fr, const embi_float_geom *g, embi_excl *ex,
                          float cx, float cw, float top, float *bx, float *by)
{
    (void)fr;
    float ml = g->ml, mt = g->mt, mw = g->mw, mh = g->mh;
    bool left = g->left;

    float y = top;
    float mx = embi_excl_max_top(ex);
    if (mx > y) {
        y = mx;
    }
    /* Clearance (CSS 2.1 9.5.2) for a float: the margin box top goes below the floats on the cleared
     * sides (Chromium does this: a cleared float with margin-top 10 under a 20px float has its border
     * top at 30). With no such float the bound is -INFINITY and y stays. */
    uint8_t clr = embi_lay_clear_sides(c, b);
    if (clr != 0) {
        float cb = embi_excl_clear_y(ex, clr);
        if (cb > y) {
            y = cb;
        }
    }
    float cr = cx + cw;
    float x = cx;
    for (;;) {
        float l = cx, rr = cr;
        embi_excl_band(ex, cx, cr, y, mh, &l, &rr);
        x = left ? l : rr - mw;
        if (rr - l >= mw) {
            break;
        }
        if (mw > cr - cx) {
            /* A float wider than its containing block may stick out of it when no float of its own side is in its way
             * anywhere in the formatting context (rule 7: it may not stick out past the edge when another float is to
             * its start side), but it must not reach a float on the other side (rule 3). The band beside the floats of
             * the whole block formatting context decides (floats-rule3-outside-*, floats-rule7-outside-*). */
            float al, ar;
            embi_excl_band(ex, -INFINITY, INFINITY, y, mh, &al, &ar);
            if (left ? al == -INFINITY : ar == INFINITY) {
                float wl, wr;
                embi_excl_band(ex, left ? cx : -INFINITY, left ? INFINITY : cr, y, mh, &wl, &wr);
                if (wr - wl >= mw) {
                    x = left ? wl : wr - mw;
                    break;
                }
            }
        }
        float nxt = embi_excl_next_y(ex, y, mh);
        if (!isfinite(nxt)) {
            break; /* nothing above blocks it: it stays at the edge of its containing block */
        }
        y = nxt;
    }
    embi_excl_add(ex, x, y, x + mw, y + mh, left);
    *bx = x + ml;
    *by = y + mt;
}
