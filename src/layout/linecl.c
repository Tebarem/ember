/*
 * Line clamping (CSS Overflow 4 section 5, `line-clamp`, `max-lines`, `continue`, `block-ellipsis`; the legacy
 * `-webkit-line-clamp` with `display: -webkit-box` and `-webkit-box-orient: vertical` maps onto the same machinery
 * in the style engine: it computes to a flow-root with continue: collapse).
 *
 * Design. A block container whose continue is collapse or discard is a line-clamp container: an independent
 * formatting context (box.c). Its clamp point is chosen from a normal layout, and the container is laid out a second
 * time with the lines that survive:
 *   1. The container is laid out as usual (embi_lay_block, pass 1) and its fragment tree is scanned for the line boxes
 *      of its block formatting context in block order: lines of its inline formatting contexts and of its in-flow
 *      block-level descendants that do not establish a formatting context of their own (floats, out-of-flow boxes,
 *      atomic and independent boxes are not entered).
 *   2. The clamp opportunities are the start of the container and the points between consecutive lines. max-lines: N
 *      sets a clamp point after the Nth line; max-lines: auto (also the initial value) sets the last opportunity
 *      whose automatic block size (the bottom of that line plus the bottom border and padding of the boxes that contain
 *      it) does not exceed the container's block size limit (the smaller of height and max-height, as content box
 *      sizes; no limit, no auto clamp point). With both, the earlier point wins.
 *   3. When the clamp point leaves lines out, the container is laid out again with embi_clamp set: the inline layout
 *      stops after the last kept line (and ends it with the block overflow ellipsis, inline.c), block flow lays out
 *      nothing after the clamp point, and the boxes it leaves out do not contribute to the height. A box of the
 *      container's formatting context that has already started (an ancestor of the last line) still ends normally, so
 *      its bottom padding and border stay. Floats after the clamp point are not placed; an out-of-flow box in the
 *      content that is left out is still laid out when its containing block lies outside that content (it precedes or
 *      contains the clamp point).
 * The count of lines is carried by the layout context (embi_clamp, layout_int.h) and restored whenever block flow lays
 * a child out again, so a repeated layout counts its lines once. Hit testing and painting read the second layout's
 * fragments, so the content after the clamp point has no boxes.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

/* A fieldset counts no lines directly: its content box and its legend are independent formatting contexts (HTML
 * section 15.3.13), so a clamp on it keeps everything, and its lines do not count for an enclosing clamp. */
static bool is_fieldset_box(const embi_box *b)
{
    if (b->el == nullptr || b->pe != 0) {
        return false;
    }
    embi_str n = embi_atom_str(b->el->base.name);
    return n.len == 8 && memcmp(n.ptr, "fieldset", 8) == 0;
}

/* A clamp opportunity in block order: a line box, or the end of a box of the container's formatting context that is
 * an independent formatting context itself (its lines cannot be counted; the point after it is an opportunity). size
 * is the automatic block size of the container when the clamp point is there, from its border top. */
typedef struct clamp_ev {
    float size;
    bool line;
} clamp_ev;
typedef EMBI_VEC(clamp_ev) ev_vec;

/* A block box open around the event being scanned. */
typedef struct clamp_anc {
    float top, h;   /* its border box in the container's frame */
    float bp, mg;   /* the bottom border and padding, the bottom margin */
    bool fixed;     /* a specified height: the box keeps its size whatever is clamped inside it */
    float minh;     /* its min-height as a border box size (0 when none): clamping inside it cannot shrink it below */
} clamp_anc;
typedef EMBI_VEC(clamp_anc) anc_vec;

/* The bottom border and padding of a block fragment. */
static float bottom_edges(const embi_frag *g)
{
    return embi_frag_bdr(g, 3) + embi_frag_pad(g, 3);
}

/* The space the boxes in st[0, n) add below something that ends at their bottom (the innermost is st[n - 1]): their
 * bottom border and padding, and their bottom margins, which collapse with the margin below a box that has no border
 * or padding at the bottom (CSS 2.1 section 8.3.1). m is the margin of the thing the chain starts at. The container is
 * a BFC root, so the margin that is left counts. */
static float tail_out(const anc_vec *st, uint32_t n, float m)
{
    float tail = 0;
    for (uint32_t i = n; i > 0; i--) {
        const clamp_anc *a = &st->data[i - 1];
        if (a->bp > 0) {
            tail += m + a->bp;
            m = a->mg;
        } else {
            m = fmaxf(m, a->mg);
        }
    }
    return tail + m;
}

/* The automatic block size at a clamp point after something that ends at bottom with the bottom margin m. A box around
 * it that has a specified height, or whose height limit its content passes (max-height), keeps its size whatever is
 * clamped inside it: the outermost such box gives the size, and the content in it does not add to it. A min-height
 * keeps the box from shrinking below it. */
static float size_at(const anc_vec *st, float bottom, float m)
{
    float sz = bottom + tail_out(st, st->len, m);
    for (uint32_t i = 0; i < st->len; i++) {
        const clamp_anc *a = &st->data[i];
        bool over = bottom > a->top + a->h - a->bp + 0.01f; /* the content passes the box's content edge */
        if (a->fixed || over) {
            sz = a->top + a->h + tail_out(st, i, a->mg);
            break;
        }
    }
    for (uint32_t i = 0; i < st->len; i++) {
        if (st->data[i].minh > 0) {
            float cand = st->data[i].top + st->data[i].minh + tail_out(st, i, st->data[i].mg);
            if (cand > sz) {
                sz = cand;
            }
        }
    }
    return sz;
}

static bool has_flag(const embi_frag *k, uint16_t fl)
{
    return k->box != nullptr && (k->box->flags & fl) != 0;
}

/* True for an in-flow box that is an independent formatting context (or not a plain block): block flow lays it out as
 * a unit of the clamp. */
bool embi_lay_clamp_indep(const embi_box *b)
{
    return (b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) == 0 &&
           ((b->flags & EMBI_BF_BFC) != 0 || b->kind != EMBI_BK_BLOCK || is_fieldset_box(b));
}

/* True when the fragment is a block of the container's formatting context that lines can be counted in. */
static bool counts_in(const embi_frag *k)
{
    const embi_box *b = k->box;
    return b->kind == EMBI_BK_BLOCK &&
           (b->flags & (EMBI_BF_BFC | EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_INLINE_LEVEL)) == 0 && !is_fieldset_box(b);
}

/* The min-height of b as a border box size in px (0 for auto, none and what cannot be resolved here). */
static float min_height_px(const embi_lctx *c, const embi_box *b, const embi_frag *f)
{
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_min_height);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD || v->kind == EMBI_V_PERCENT) {
        return 0.0f;
    }
    float px = embi_lay_len(c, b, v, NAN);
    if (isnan(px) || px <= 0.0f) {
        return 0.0f;
    }
    const embi_value *bs = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    bool bb = bs != nullptr && bs->kind == EMBI_V_KEYWORD && bs->kw == EMBI_KW_border_box;
    return bb ? px : px + embi_frag_bdr(f, 1) + embi_frag_pad(f, 1) + embi_frag_bdr(f, 3) + embi_frag_pad(f, 3);
}

static bool height_fixed(const embi_lctx *c, const embi_box *b)
{
    const embi_value *h = embi_lay_prop(c, b, EMBI_PROP_height);
    return h != nullptr && !(h->kind == EMBI_V_KEYWORD && h->kw == EMBI_KW_auto);
}

/* The events of g in order. gy: the top of g from the container's border top. A plain block with no event inside it
 * (an empty block, one with floats and out-of-flow boxes only) is an event itself. */
static void scan(const embi_lctx *c, const embi_frag *g, float gy, anc_vec *st, ev_vec *out)
{
    for (const embi_frag *k = g->first; k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_LINE) {
            clamp_ev r = {size_at(st, gy + k->y + k->h, 0.0f), true};
            embi_vec_push(out, r);
        } else if (k->kind == EMBI_FK_BOX && k->inl == 0 && k->box != nullptr &&
                   !has_flag(k, EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) {
            if (counts_in(k)) {
                uint32_t n0 = out->len;
                clamp_anc a = {gy + k->y, k->h, bottom_edges(k), embi_frag_mg(k, 3), height_fixed(c, k->box),
                                min_height_px(c, k->box, k)};
                embi_vec_push(st, a);
                scan(c, k, gy + k->y, st, out);
                st->len--;
                if (out->len == n0) {
                    clamp_ev r = {size_at(st, gy + k->y + k->h, embi_frag_mg(k, 3)), false};
                    embi_vec_push(out, r);
                }
            } else if (embi_lay_clamp_indep(k->box)) {
                clamp_ev r = {size_at(st, gy + k->y + k->h, embi_frag_mg(k, 3)), false};
                embi_vec_push(out, r);
            }
        }
    }
}

/* max-lines of b: the number of lines (UINT32_MAX for none) and whether auto is part of it. */
static void max_lines(const embi_lctx *c, const embi_box *b, uint32_t *n, bool *au)
{
    *n = UINT32_MAX;
    *au = false;
    const embi_value *v = embi_lay_prop(c, b, EMBI_PROP_max_lines);
    if (v == nullptr) {
        *au = true;
        return;
    }
    uint32_t cnt = 1;
    const embi_value *items = v;
    if (v->kind == EMBI_V_LIST) {
        items = embi_value_items(v, &cnt);
    }
    for (uint32_t i = 0; i < cnt; i++) {
        if (items[i].kind == EMBI_V_KEYWORD) {
            *au = true;
        } else if (items[i].kind == EMBI_V_INTEGER || items[i].kind == EMBI_V_NUMBER) {
            *n = items[i].num >= 1 ? (uint32_t)items[i].num : 1u;
        }
    }
}

bool embi_lay_clamp_plan(embi_lctx *c, embi_box *b, const embi_frag *f, float limit, float content_top,
                         embi_clamp *plan)
{
    if (is_fieldset_box(b)) {
        return false;
    }
    uint32_t nmax;
    bool au;
    max_lines(c, b, &nmax, &au);
    ev_vec evs;
    anc_vec st;
    memset(&evs, 0, sizeof evs);
    memset(&st, 0, sizeof st);
    scan(c, f, 0.0f, &st, &evs);
    embi_vec_free(&st);
    uint32_t total = evs.len;
    uint32_t keep = total;
    bool point = false; /* a clamp point exists: after the Nth line, or at the last opportunity that fits */
    if (nmax != UINT32_MAX) {
        uint32_t nl = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (evs.data[i].line && ++nl == nmax) {
                keep = i + 1;
                point = true;
                break;
            }
        }
    }
    if (au && isfinite(limit)) {
        uint32_t fit = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (evs.data[i].size - content_top <= limit + 0.01f) {
                fit = i + 1;
            }
        }
        if (fit < total && fit < keep) {
            keep = fit;
            point = true;
        }
    }
    embi_vec_free(&evs);
    if (!point || keep >= total) {
        return false;
    }
    plan->on = true;
    plan->ell = true;
    plan->left = keep;
    return true;
}

/* Out-of-flow descendants of a box the clamp point leaves out (CSS Overflow 4: an absolutely positioned box is shown
 * when its containing block precedes or contains the clamp point): the ones whose containing block is not inside the
 * box are laid out, with the static position of the flow where the clamp point is. */
static bool inside(const embi_box *a, const embi_box *root)
{
    for (; a != nullptr; a = a->parent) {
        if (a == root) {
            return true;
        }
    }
    return false;
}

static void skip_abspos(embi_lctx *c, embi_box *root, embi_box *k, embi_box *sbox, float sx, float sy)
{
    for (embi_box *d = k->first; d != nullptr; d = d->next) {
        if ((d->flags & EMBI_BF_ABSPOS) != 0) {
            const embi_box *cb = embi_lay_abspos_cb(c, d);
            if (cb == nullptr || !inside(cb, root)) {
                embi_lay_abspos_at(c, d, sbox, sx, sy, nullptr);
            }
            continue;
        }
        if (d->kind != EMBI_BK_TEXT) {
            skip_abspos(c, root, d, sbox, sx, sy);
        }
    }
}

void embi_lay_clamp_skip(embi_lctx *c, embi_box *k, embi_box *sbox, float sx, float sy)
{
    skip_abspos(c, k, k, sbox, sx, sy);
}
