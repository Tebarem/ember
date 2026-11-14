/*
 * Transformed geometry (W6e): getBoundingClientRect, getClientRects and the observers report the rectangles an element
 * paints at, so the transform, translate, rotate and scale of the element's box and of every ancestor box move and
 * grow them. Each rectangle becomes the axis-aligned bounding box of its four corners mapped through the matrices of
 * the boxes on the containing-block chain, innermost first (a matrix already holds the move of its origin, see xform.h).
 * The _xf functions report layout positions (no scroll offsets); the painted ones (embi_layout_painted_box_xf) move them as
 * paint does (the scroll offsets of the containers around them, fixed boxes kept still); the viewport's own offset is
 * left to the callers in both cases. The perspective of a parent and
 * preserve-3d are not modelled: every box maps its plane flat.
 */
#include "layout_int.h"

#include "layout/xform.h"

#include <math.h>
#include <string.h>

static void origin_of(const embi_frag *f, bool painted, float *x, float *y)
{
    if (painted) {
        embi_lay_frag_painted_origin(f, x, y);
        return;
    }
    float ox = 0, oy = 0;
    for (const embi_frag *p = f; p != nullptr; p = p->parent) {
        ox += p->x;
        oy += p->y;
    }
    *x = ox;
    *y = oy;
}

/* The fragment whose box transform comes first for the rectangles of el: its own box fragment, or for an inline the one of the nearest
 * ancestor with a box fragment (a transform does not apply to a non-replaced inline). *own tells which. */
static const embi_frag *start_frag(const embi_element *el)
{
    const embi_box *b = el->box;
    if (b != nullptr && b->kind != EMBI_BK_INLINE && b->kind != EMBI_BK_TEXT && b->kind != EMBI_BK_MARKER) {
        return b->frag;
    }
    for (const embi_node *a = el->base.parent; a != nullptr; a = a->parent) {
        if (!embi_is_element(a)) {
            continue;
        }
        const embi_box *ab = ((const embi_element *)a)->box;
        if (ab != nullptr && ab->kind != EMBI_BK_INLINE && ab->kind != EMBI_BK_TEXT && ab->kind != EMBI_BK_MARKER) {
            return ab->frag;
        }
    }
    return nullptr;
}

/* Maps the rectangle through the transforms from the fragment start up the chain. Returns the bounding box. painted: the
 * rectangle and the boxes of the transforms are at their painted positions (embi_lay_frag_painted_origin). */
static embi_rectf map_rect(const embi_frag *start, bool painted, float vw, float vh, float rem, embi_rectf r)
{
    double px[4] = {r.x, r.x + r.w, r.x + r.w, r.x};
    double py[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    for (const embi_frag *f = start; f != nullptr; f = f->parent) {
        if (f->kind != EMBI_FK_BOX || f->box == nullptr || f->box->el == nullptr || f->box->style == nullptr) {
            continue;
        }
        const embi_element *fe = f->box->el;
        if (fe->box != f->box || f->box->frag != f || !embi_lay_has_transform(f->box->style)) {
            continue;
        }
        float ox, oy;
        origin_of(f, painted, &ox, &oy);
        float bb[4] = {ox, oy, f->w, f->h};
        embi_m4 m;
        if (!embi_lay_box_matrix(f->box->style, bb, vw, vh, rem, &m)) {
            continue;
        }
        for (int i = 0; i < 4; i++) {
            double o[4];
            embi_m4_point(&m, px[i], py[i], 0.0, o);
            if (o[3] > 0.0 && o[3] != 1.0) {
                o[0] /= o[3];
                o[1] /= o[3];
            }
            px[i] = o[0];
            py[i] = o[1];
        }
    }
    double x0 = px[0], x1 = px[0], y0 = py[0], y1 = py[0];
    for (int i = 1; i < 4; i++) {
        x0 = fmin(x0, px[i]);
        x1 = fmax(x1, px[i]);
        y0 = fmin(y0, py[i]);
        y1 = fmax(y1, py[i]);
    }
    return (embi_rectf){(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)};
}

static bool xf_setup(const embi_layout_engine *e, const embi_element *el, const embi_frag **start, float *vw, float *vh,
                     float *rem)
{
    const embi_frag *root = embi_layout_root(e, el->base.doc);
    if (root == nullptr || el->box == nullptr || !embi_lay_doc_has_transform(e, el->base.doc)) {
        return false;
    }
    *start = start_frag(el);
    if (*start == nullptr) {
        return false;
    }
    *vw = root->w;
    *vh = root->h;
    *rem = embi_lay_root_font_px(root);
    return true;
}

static bool box_xf(const embi_layout_engine *e, const embi_element *el, bool painted, embi_rectf *out)
{
    if (!embi_lay_element_box(e, el, painted, out)) {
        return false;
    }
    const embi_frag *start;
    float vw, vh, rem;
    if (xf_setup(e, el, &start, &vw, &vh, &rem)) {
        *out = map_rect(start, painted, vw, vh, rem, *out);
    }
    return true;
}

static uint32_t rects_xf(const embi_layout_engine *e, const embi_element *el, bool painted, embi_rectf *out, uint32_t cap)
{
    uint32_t n = embi_lay_element_rects(e, el, painted, out, cap);
    const embi_frag *start;
    float vw, vh, rem;
    if (n != 0 && xf_setup(e, el, &start, &vw, &vh, &rem)) {
        for (uint32_t i = 0; i < n && i < cap; i++) {
            out[i] = map_rect(start, painted, vw, vh, rem, out[i]);
        }
    }
    return n;
}

bool embi_layout_border_box_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out)
{
    return box_xf(e, el, false, out);
}

uint32_t embi_layout_client_rects_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out, uint32_t cap)
{
    return rects_xf(e, el, false, out, cap);
}

bool embi_layout_painted_box_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out)
{
    return box_xf(e, el, true, out);
}

uint32_t embi_layout_painted_rects_xf(const embi_layout_engine *e, const embi_element *el, embi_rectf *out, uint32_t cap)
{
    return rects_xf(e, el, true, out, cap);
}
