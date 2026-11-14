/*
 * Borders and outlines (W4c section 3). A border is one BORDER item: the outer edge of the border box with its
 * radii, and each side's width, color and CSS border-style. The renderer draws the styles (W4b); this file
 * only resolves them. An outline is a border drawn outside the border box, offset by outline-offset: its own
 * item, with its rect inflated by the offset and the width, and its radii grown by the same amount.
 */
#include "paint/paint_int.h"

#include <math.h>

#include <stdlib.h>
#include <string.h>

#define BLACK_RGBA 0xFF000000u

/* Per side in top, right, bottom, left order. */
static const embi_prop_id k_bstyle[4] = {
    EMBI_PROP_border_top_style,
    EMBI_PROP_border_right_style,
    EMBI_PROP_border_bottom_style,
    EMBI_PROP_border_left_style,
};
static const embi_prop_id k_bcolor[4] = {
    EMBI_PROP_border_top_color,
    EMBI_PROP_border_right_color,
    EMBI_PROP_border_bottom_color,
    EMBI_PROP_border_left_color,
};
/* The fragment's border index (fragment.h: left 0, top 1, right 2, bottom 3) of each side in top, right, bottom,
 * left order. */
static const int k_bdr_index[4] = {1, 2, 3, 0};

/* The renderer's style of a CSS border-style keyword. none and hidden paint nothing. */
static uint8_t bstyle_of(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW_solid:
        return EMBI_DS_SOLID;
    case EMBI_KW_dotted:
        return EMBI_DS_DOTTED;
    case EMBI_KW_dashed:
        return EMBI_DS_DASHED;
    case EMBI_KW_double:
        return EMBI_DS_DOUBLE;
    case EMBI_KW_groove:
        return EMBI_DS_GROOVE;
    case EMBI_KW_ridge:
        return EMBI_DS_RIDGE;
    case EMBI_KW_inset:
        return EMBI_DS_INSET;
    case EMBI_KW_outset:
        return EMBI_DS_OUTSET;
    default:
        return EMBI_DS_NONE;
    }
}

void embi_pb_border(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                    uint32_t clip)
{
    float w[4];
    uint32_t c[4];
    uint8_t st[4];
    bool any = false;
    for (int i = 0; i < 4; i++) {
        uint8_t bs = bstyle_of(embi_pb_kw(s, k_bstyle[i]));
        float wd = bs == EMBI_DS_NONE ? 0.0f : embi_frag_bdr(f, k_bdr_index[i]);
        w[i] = wd;
        st[i] = bs;
        if (!embi_pb_color(s, embi_style_value(s, k_bcolor[i]), &c[i])) {
            c[i] = BLACK_RGBA;
        }
        if (wd > 0.0f) {
            any = true;
        }
    }
    if (!any) {
        return;
    }
    float radii[4];
    embi_pb_frag_radii(p, f, s, bb[2], bb[3], embi_pb_fontsize(s), radii);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
    memcpy(it->rect, bb, sizeof it->rect);
    memcpy(it->radii, radii, sizeof it->radii);
    memcpy(it->widths, w, sizeof it->widths);
    memcpy(it->colors, c, sizeof it->colors);
    memcpy(it->styles, st, sizeof it->styles);
}

void embi_pb_colrules(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf, uint32_t clip)
{
    const embi_mcol *mc = embi_frag_mc(f);
    if (mc == nullptr) {
        return;
    }
    uint8_t bs = bstyle_of(embi_pb_kw(s, EMBI_PROP_column_rule_style));
    if (bs == EMBI_DS_NONE) {
        return;
    }
    float width = 3.0f;
    const embi_value *wv = embi_style_value(s, EMBI_PROP_column_rule_width);
    if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
        width = wv->kw == EMBI_KW_thin ? 1.0f : (wv->kw == EMBI_KW_thick ? 5.0f : 3.0f);
    } else if (wv != nullptr) {
        bool ok = false;
        width = embi_pb_len(p, wv, 0.0f, embi_pb_fontsize(s), &ok);
        if (!ok) {
            width = 3.0f;
        }
        if (width > 0.0f) {
            width = width < 1.0f ? 1.0f : floorf(width); /* the computed width snaps to whole pixels, as a border's */
        }
    }
    if (!(width > 0.0f)) {
        return;
    }
    uint32_t color = BLACK_RGBA;
    if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP_column_rule_color), &color)) {
        color = BLACK_RGBA;
    }
    for (uint32_t i = 0; i < mc->nsets; i++) {
        const embi_mcset *set = &mc->sets[i];
        for (uint32_t j = 0; j + 1u < set->cols; j++) {
            float xa = mc->x0 + (float)j * mc->stride, xb = mc->x0 + (float)(j + 1u) * mc->stride;
            float left = (xa < xb ? xa : xb) + mc->colw + mc->gap * 0.5f - width * 0.5f;
            float x0 = roundf(bb[0] + left), x1 = roundf(bb[0] + left + width);
            if (x1 <= x0) {
                x1 = x0 + 1.0f;
            }
            embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
            float rect[4] = {x0, bb[1] + set->y, x1 - x0, set->h};
            memcpy(it->rect, rect, sizeof it->rect);
            memset(it->radii, 0, sizeof it->radii);
            float wd[4] = {0.0f, 0.0f, 0.0f, x1 - x0};
            memcpy(it->widths, wd, sizeof it->widths);
            for (int s2 = 0; s2 < 4; s2++) {
                it->colors[s2] = color;
                it->styles[s2] = bs;
            }
        }
    }
}

void embi_pb_outline(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                     uint32_t clip)
{
    (void)f;
    uint16_t kw = embi_pb_kw(s, EMBI_PROP_outline_style);
    if (kw == 0 || kw == EMBI_KW_none || kw == EMBI_KW_hidden) {
        return;
    }
    float fs = embi_pb_fontsize(s);
    float width = 0.0f, offset = 0.0f;
    uint32_t color = BLACK_RGBA;
    uint8_t bs = EMBI_DS_SOLID;
    if (kw == EMBI_KW_auto) {
        /* outline-style: auto is a 2 px solid focus ring (the focus color of the embedder). */
        width = 2.0f;
        color = p->cfg->focus_rgba;
    } else {
        bs = bstyle_of(kw);
        if (bs == EMBI_DS_NONE) {
            return;
        }
        const embi_value *wv = embi_style_value(s, EMBI_PROP_outline_width);
        if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
            width = wv->kw == EMBI_KW_thin ? 1.0f : (wv->kw == EMBI_KW_thick ? 5.0f : 3.0f);
        } else {
            bool ok = false;
            width = wv != nullptr ? embi_pb_len(p, wv, 0.0f, fs, &ok) : 3.0f;
            if (!ok) {
                width = 3.0f;
            }
            if (width > 0.0f) {
                width = width < 1.0f ? 1.0f : floorf(width); /* the computed width snaps to whole pixels, as a border's */
            }
        }
        if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP_outline_color), &color)) {
            color = BLACK_RGBA;
        }
    }
    {
        const embi_value *ov = embi_style_value(s, EMBI_PROP_outline_offset);
        if (ov != nullptr && ov->kind == EMBI_V_KEYWORD && ov->kw == EMBI_KW_inset) {
            offset = -width; /* outline-offset: inset puts the outline inside the border edge (CSS UI 4) */
        } else if (ov != nullptr) {
            bool ok = false;
            offset = embi_pb_len(p, ov, 0.0f, fs, &ok);
            if (!ok) {
                offset = 0.0f;
            }
        }
    }
    if (!(width > 0.0f)) {
        return;
    }
    float ex = offset + width;
    float rect[4] = {bb[0] - ex, bb[1] - ex, bb[2] + 2.0f * ex, bb[3] + 2.0f * ex};
    /* A negative offset that reaches past the middle of the box leaves a line as wide as the outline there
     * (WPT css-ui/negative-outline-offset.html). */
    for (int i = 0; i < 2; i++) {
        if (rect[2 + i] < width) {
            rect[i] = bb[i] + bb[2 + i] * 0.5f - width * 0.5f;
            rect[2 + i] = width;
        }
    }
    float radii[4];
    embi_pb_radii(p, s, bb[2], bb[3], fs, radii);
    /* An outline follows the outer edge of a rounded box: a corner grows by ex only when it is rounded (a square
     * corner stays square, as in Chromium). */
    for (int i = 0; i < 4; i++) {
        if (radii[i] > 0.0f) {
            radii[i] += ex;
        }
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    memcpy(it->radii, radii, sizeof it->radii);
    for (int i = 0; i < 4; i++) {
        it->widths[i] = width;
        it->colors[i] = color;
        it->styles[i] = bs;
    }
}

/* ------------------------------------------------------------------ collapsed table borders */

typedef struct cellr {
    float x0, y0, x1, y1;
} cellr;

typedef EMBI_VEC(cellr) cellvec;
typedef EMBI_VEC(float) fvec;

/* The cells below f (through its row groups and rows), with their border boxes in page px. */
static void cells_below(const embi_frag *f, float ox, float oy, cellvec *out)
{
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
            continue;
        }
        float kx = ox + k->x, ky = oy + k->y;
        uint8_t tr = k->box->tr;
        if (tr == EMBI_TR_CELL) {
            cellr c = {kx, ky, kx + k->w, ky + k->h};
            embi_vec_push(out, c);
        } else if (tr == EMBI_TR_ROW || tr == EMBI_TR_ROWGROUP || tr == EMBI_TR_THEAD || tr == EMBI_TR_TFOOT) {
            cells_below(k, kx, ky, out);
        }
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* The distinct sorted boundaries of vals (within half a px); false when the count is not want. */
static bool boundaries(fvec *vals, uint32_t want, float *out)
{
    qsort(vals->data, vals->len, sizeof(float), cmp_float);
    uint32_t n = 0;
    for (uint32_t i = 0; i < vals->len; i++) {
        if (n == 0 || vals->data[i] - out[n - 1] > 0.5f) {
            if (n == want) {
                return false;
            }
            out[n++] = vals->data[i];
        }
    }
    return n == want;
}

static void collapsed_edge(embi_pst *p, const embi_cborder *e, const float rect[4], uint32_t xf, uint32_t clip,
                           bool horizontal)
{
    if (!(e->w > 0.0f) || e->owner == nullptr) {
        return;
    }
    uint8_t bs = bstyle_of(e->style);
    if (bs == EMBI_DS_NONE) {
        return;
    }
    const embi_style *os = e->owner->style;
    uint32_t c = BLACK_RGBA;
    if (os == nullptr || !embi_pb_color(os, embi_style_value(os, k_bcolor[e->side & 3]), &c)) {
        c = BLACK_RGBA;
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    for (int i = 0; i < 4; i++) {
        it->colors[i] = c;
        it->styles[i] = bs;
    }
    /* Only the side of the edge that it is drawn on has a width: the top for a horizontal edge, the left for a
     * vertical one. */
    it->widths[horizontal ? 0 : 3] = e->w;
}

void embi_pb_collapsed(embi_pst *p, const embi_frag *t, float ox, float oy, uint32_t xf, uint32_t clip)
{
    const embi_cgrid *g = embi_frag_coll(t);
    if (g == nullptr || g->rows == 0 || g->cols == 0 || g->h == nullptr || g->v == nullptr) {
        return;
    }
    if (t->box != nullptr && t->box->style != nullptr && embi_pb_hidden(t->box->style)) {
        return; /* a table with visibility: hidden or collapse paints no collapsed borders */
    }
    float *xb = embi_alloc((g->cols + 1) * sizeof(float));
    float *yb = embi_alloc((g->rows + 1) * sizeof(float));
    bool ok = xb != nullptr && yb != nullptr;
    if (ok && g->xb != nullptr && g->yb != nullptr) {
        /* The grid lines come from the layout: rows and columns with no size share theirs. */
        for (uint32_t j = 0; j <= g->cols; j++) {
            xb[j] = ox + g->xb[j];
        }
        for (uint32_t r = 0; r <= g->rows; r++) {
            yb[r] = oy + g->yb[r];
        }
    } else if (ok) {
        /* No lines from the layout: the boundaries of the cells; a table whose boundaries do not match its grid gets no
         * collapsed borders. */
        cellvec cells = {0};
        cells_below(t, ox, oy, &cells);
        fvec xs = {0}, ys = {0};
        for (uint32_t i = 0; i < cells.len; i++) {
            embi_vec_push(&xs, cells.data[i].x0);
            embi_vec_push(&xs, cells.data[i].x1);
            embi_vec_push(&ys, cells.data[i].y0);
            embi_vec_push(&ys, cells.data[i].y1);
        }
        ok = cells.len > 0 && boundaries(&xs, g->cols + 1, xb) && boundaries(&ys, g->rows + 1, yb);
        embi_vec_free(&xs);
        embi_vec_free(&ys);
        embi_vec_free(&cells);
    }
    if (ok) {
        /* The grid lines sit on whole pixels (layout sizes are fractional; the edges are drawn snapped like box borders). */
        for (uint32_t j = 0; j <= g->cols; j++) {
            xb[j] = roundf(xb[j]);
        }
        for (uint32_t r = 0; r <= g->rows; r++) {
            yb[r] = roundf(yb[r]);
        }
        for (uint32_t r = 0; r <= g->rows; r++) {
            for (uint32_t j = 0; j < g->cols; j++) {
                const embi_cborder *e = &g->h[r * g->cols + j];
                /* An edge spans the rest of its width (the larger half) before its grid line and the floor half after. */
                float rect[4] = {xb[j], yb[r] - (e->w - floorf(e->w * 0.5f)), xb[j + 1] - xb[j], e->w};
                /* The outer corners belong to the horizontal edges of the first and last grid lines: they reach over the
                 * halves of the vertical edge that meets them (no other edge covers that square). */
                if (r == 0 || r == g->rows) {
                    uint32_t vr = r == 0 ? 0 : g->rows - 1;
                    if (j == 0 || g->h[r * g->cols + j - 1].w <= 0.0f) {
                        float vw = g->v[vr * (g->cols + 1) + j].w;
                        float ext = g->rtl ? floorf(vw * 0.5f) : vw - floorf(vw * 0.5f);
                        rect[0] -= ext;
                        rect[2] += ext;
                    }
                    if (j + 1 == g->cols || g->h[r * g->cols + j + 1].w <= 0.0f) {
                        float vw = g->v[vr * (g->cols + 1) + j + 1].w;
                        rect[2] += g->rtl ? vw - floorf(vw * 0.5f) : floorf(vw * 0.5f);
                    }
                }
                collapsed_edge(p, e, rect, xf, clip, true);
            }
        }
        for (uint32_t i = 0; i < g->rows; i++) {
            for (uint32_t c = 0; c <= g->cols; c++) {
                const embi_cborder *e = &g->v[i * (g->cols + 1) + c];
                float before = g->rtl ? floorf(e->w * 0.5f) : e->w - floorf(e->w * 0.5f);
                float rect[4] = {xb[c] - before, yb[i], e->w, yb[i + 1] - yb[i]};
                collapsed_edge(p, e, rect, xf, clip, false);
            }
        }
    }
    embi_free(xb, (g->cols + 1) * sizeof(float));
    embi_free(yb, (g->rows + 1) * sizeof(float));
}
