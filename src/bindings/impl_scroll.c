/*
 * Scrolling from script (CSSOM View): the viewport's scroll offset as scripts see it, the viewport scrolls of
 * window.scrollTo/scrollBy and the root's scrollTop/scrollLeft, and Element.scrollIntoView.
 *
 * The viewport's offset lives on the root fragment (the view moves it with the wheel, the keys and smooth scrolls too), so
 * the bindings read it through the host's root_offset hook each time; c->scroll_x and scroll_y keep the last value read
 * (and are the offset itself for a context without the hook, as in the binding tests). Every programmatic viewport scroll
 * is clamped to the viewport's scroll range (0 to the root's scrollable overflow less the viewport, as the view clamps it):
 * a page that does not overflow does not move, whatever its overflow property says.
 *
 * scrollIntoView (CSSOM View section 7, "scroll an element into view"): the element's border box, where it is painted, is
 * aligned in the scrollport of each scroll container on its containing-block chain, innermost first, then in the viewport.
 * block (vertical) and inline (horizontal) take start, center, end or nearest; a boolean argument is block start (true) or
 * end (false) with inline nearest. Each container is clamped to its scroll range, and the element's box is measured again
 * after each one moved. A position: fixed box of the viewport (or an element inside one) stops the walk before the viewport,
 * as in browsers: the viewport's scroll does not move it. Known gaps: the axes are the physical ones (writing modes and rtl
 * are not mapped), transforms between the element and a container are ignored (their layout boxes are aligned), smooth
 * behaviour is instant for containers (the viewport follows the root's scroll-behavior as window.scrollTo does), and
 * scroll-margin and scroll-padding are not applied.
 */
#include "bindings/impl.h"

#include "dom/dom.h"
#include "layout/box.h"
#include "layout/fragment.h"
#include "layout/layout.h"
#include "paint/scroll.h"

#include <math.h>

void embi_impl_root_offset(embi_bind_ctx *c, double *x, double *y)
{
    if (c->host.root_offset != nullptr) {
        c->host.root_offset(c->host.user, &c->scroll_x, &c->scroll_y);
    }
    *x = c->scroll_x;
    *y = c->scroll_y;
}

/* The document's layout brought up to date, or nullptr when the context has none. */
static embi_layout_engine *layout_ready(embi_bind_ctx *c)
{
    embi_layout_engine *e = embi_bind_layout(c);
    if (e != nullptr && !embi_layout_document(e, c->doc)) {
        return nullptr;
    }
    return e;
}

/* The largest offsets of the viewport. False when the document has no layout (nothing bounds the offsets but 0). */
static bool root_range(embi_bind_ctx *c, double *mx, double *my)
{
    embi_layout_engine *e = layout_ready(c);
    const embi_frag *root = e != nullptr ? embi_layout_root(e, c->doc) : nullptr;
    if (root == nullptr) {
        return false;
    }
    double vw = 0.0, vh = 0.0;
    embi_bind_viewport(c, &vw, &vh, nullptr);
    float fx = 0.0f, fy = 0.0f;
    embi_scroll_max_root(root, (float)vw, (float)vh, &fx, &fy);
    *mx = fx;
    *my = fy;
    return true;
}

static double clamp_off(double v, double max)
{
    if (!(v > 0.0)) {
        return 0.0; /* negative or NaN */
    }
    return v > max ? max : v;
}

void embi_impl_scroll_root_to(embi_bind_ctx *c, double x, double y)
{
    double mx = INFINITY, my = INFINITY;
    (void)root_range(c, &mx, &my);
    c->scroll_x = clamp_off(x, mx);
    c->scroll_y = clamp_off(y, my);
    if (c->host.scroll != nullptr) {
        c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
    }
    if (c->host.root_offset != nullptr) {
        c->host.root_offset(c->host.user, &c->scroll_x, &c->scroll_y); /* what the view did (a smooth scroll starts) */
    }
}

/* ------------------------------------------------------------------ scrollIntoView */

/* The new offset of one axis that brings the element's edges a0..a1 to pos in the scrollport p0..p1, whose content is at
 * offset cur (CSSOM View, "determine the scroll-into-view position"). */
static double align_axis(int pos, double a0, double a1, double p0, double p1, double cur)
{
    double es = a1 - a0, ps = p1 - p0;
    switch (pos) {
    case EMBI_ENUM_ScrollLogicalPosition_start:
        return cur + (a0 - p0);
    case EMBI_ENUM_ScrollLogicalPosition_end:
        return cur + (a1 - p1);
    case EMBI_ENUM_ScrollLogicalPosition_center:
        return cur + ((a0 + a1) - (p0 + p1)) / 2.0;
    default:
        break;
    }
    /* nearest: a box already inside stays, and so does one that covers the scrollport on both sides */
    if ((a0 >= p0 && a1 <= p1) || (a0 < p0 && a1 > p1)) {
        return cur;
    }
    if ((a0 < p0 && es <= ps) || (a1 > p1 && es > ps)) {
        return cur + (a0 - p0);
    }
    return cur + (a1 - p1);
}

/* The fragment whose containing-block chain the element sits on: its own box fragment, or for an inline (which has no
 * fragment of its own) that of its nearest block-level ancestor, which is then a container of it too (*inclusive). */
static const embi_frag *chain_start(const embi_element *el, bool *inclusive)
{
    *inclusive = false;
    const embi_box *b = el->box;
    if (b != nullptr && b->kind != EMBI_BK_INLINE && b->kind != EMBI_BK_TEXT && b->kind != EMBI_BK_MARKER) {
        return b->frag;
    }
    *inclusive = true;
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

/* The element whose scroll container f is (its principal box fragment), or nullptr. */
static embi_element *scroller_el(const embi_frag *f)
{
    if (f->kind != EMBI_FK_BOX || (f->flags & EMBI_FF_SCROLL) == 0 || f->box == nullptr || f->box->el == nullptr) {
        return nullptr;
    }
    embi_element *el = (embi_element *)(uintptr_t)f->box->el;
    return el->box == (const void *)f->box && f->box->frag == f ? el : nullptr;
}

/* Sets the offsets of scroll container f (element sel). The host clamps them and tells where they ended; without a host
 * the layout's offset is set directly (clamped here). */
static void scroll_el_to(embi_bind_ctx *c, embi_layout_engine *e, embi_frag *f, embi_element *sel, double x, double y)
{
    if (c->host.scroll_el != nullptr) {
        (void)c->host.scroll_el(c->host.user, sel, true, &x, &y);
        return;
    }
    float mx = 0.0f, my = 0.0f;
    embi_scroll_max(f, &mx, &my);
    embi_layout_frag_set_scroll(e, c->doc, f, (float)clamp_off(x, mx), (float)clamp_off(y, my));
}

void embi_impl_scroll_into_view(embi_bind_ctx *c, embi_element *el, int block, int inl)
{
    embi_layout_engine *e = layout_ready(c);
    embi_rectf t;
    if (e == nullptr || el == nullptr || !embi_layout_painted_box(e, el, &t)) {
        return; /* no box: nothing to show */
    }
    bool inclusive = false;
    const embi_frag *start = chain_start(el, &inclusive);
    if (start == nullptr) {
        return;
    }
    bool fixed = false; /* a fixed box of the viewport was passed: the viewport's scroll does not move the element */
    for (const embi_frag *p = inclusive ? start : start->parent, *prev = inclusive ? nullptr : start; p != nullptr;
         prev = p, p = p->parent) {
        fixed = fixed || (prev != nullptr && embi_lay_frag_vp_fixed(prev));
        if (p->parent == nullptr) {
            if (fixed) {
                break;
            }
            /* the viewport: its scrollport is the viewport at the current offset, in document coordinates */
            double sx = 0.0, sy = 0.0, vw = 0.0, vh = 0.0;
            embi_impl_root_offset(c, &sx, &sy);
            embi_bind_viewport(c, &vw, &vh, nullptr);
            double nx = align_axis(inl, t.x, (double)t.x + t.w, sx, sx + vw, sx);
            double ny = align_axis(block, t.y, (double)t.y + t.h, sy, sy + vh, sy);
            if (nx != sx || ny != sy) {
                embi_impl_scroll_root_to(c, nx, ny);
            }
            break;
        }
        embi_element *sel = scroller_el(p);
        embi_rectf b;
        if (sel == nullptr || !embi_layout_painted_box(e, sel, &b)) {
            continue;
        }
        /* the scrollport: the padding box less the scrollbar gutters, where the container is painted */
        double x0 = (double)b.x + embi_frag_bdr(p, 0), y0 = (double)b.y + embi_frag_bdr(p, 1);
        double x1 = (double)b.x + b.w - embi_frag_bdr(p, 2) - embi_frag_scr(p, 0);
        double y1 = (double)b.y + b.h - embi_frag_bdr(p, 3) - embi_frag_scr(p, 1);
        double cx = embi_frag_sx(p), cy = embi_frag_sy(p);
        double nx = align_axis(inl, t.x, (double)t.x + t.w, x0, x1, cx);
        double ny = align_axis(block, t.y, (double)t.y + t.h, y0, y1, cy);
        if (nx != cx || ny != cy) {
            scroll_el_to(c, e, (embi_frag *)(uintptr_t)p, sel, nx, ny);
            if (!embi_layout_painted_box(e, el, &t)) {
                return;
            }
        }
    }
}
