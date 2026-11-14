/*
 * Block formatting contexts and normal flow (CSS 2.1 sections 8.3.1, 9.4.1, 10.3.3, 10.4, 10.6.3,
 * 10.7), W3a section 3.
 *
 * Margin collapsing works on a strut per flow. A block's result carries the margins the parent
 * collapses (mt, mb). When a block collapses its top margin with its first child, the child's
 * margins are absorbed into the block's own top margin and the child sits at the top of the content
 * box. The same holds for the bottom (height: auto only). A block with an auto or zero height, no
 * min-height, no padding and no border, and no in-flow content, is self-collapsing: its margins collapse with each other and with its neighbours,
 * and it takes no room. A fragment's own mg[1] and mg[3] are the box's own used margins, never the
 * collapsed ones (those live in embi_lres only).
 *
 * Positions. A child's fragment is placed relative to this block's border-box origin: x is the
 * block's border and padding on the left plus the child's left margin, and y is the border and
 * padding on the top plus the child's border-box top in the content box.
 *
 * Overflow. A fragment's scrollable overflow rectangle is relative to its border-box origin and
 * starts at the padding box. It takes in each child's border box, and the child's own overflow
 * unless the child is a scroll container or clips (then the border box alone). Padding-end is part
 * of the block's overflow (CSS Overflow 3 section 2.2).
 *
 * Scrollbars. A scroll container reserves scrollbar_thickness on the inline end for a vertical bar
 * (narrowing the content box) and on the block end for a horizontal bar (adding to an auto height).
 * overflow: auto reserves a bar on an axis that overflows, found by laying the box out a second
 * time. scrollbar-gutter: stable reserves the vertical gutter always.
 *
 * Floats (W3b, float.c and exclusion.c). Each BFC has an exclusion space of float margin boxes, in
 * the BFC's frame. A box that is not a BFC root lies in its parent's frame: the parent passes the
 * BFC x of its content box and the BFC y of the child's border top, estimated from the margins it
 * can see (top_margins). The estimate is checked after the layout, and the child is laid out again
 * when it was wrong and floats are in play. Clearance moves a box to the bottom of the floats it
 * clears, and a BFC child stays out of the floats (beside them, or below). A BFC root's auto height
 * includes its floats. Absolutely positioned children are out of flow here (see W3d).
 */
#include "layout_int.h"

#include "css/props.h"


#include <math.h>
#include <string.h>

static float clamp0(float v)
{
    return isnan(v) || v < 0 ? 0.0f : v;
}

/* ASCII case-insensitive comparison of a function name with a lowercase literal. */
static bool name_is(embi_atom a, const char *lit)
{
    embi_str s = embi_atom_str(a);
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != lit[i]) {
            return false;
        }
    }
    return true;
}

/* The tier of the fragment of kind (and box b): how many bytes of embi_frag it is allocated with. */
static size_t frag_tier(const embi_box *b, uint8_t kind)
{
    if (kind == EMBI_FK_TEXT) {
        return EMBI_FSZ_TEXT;
    }
    if (kind == EMBI_FK_LINE) {
        return EMBI_FSZ_LINE;
    }
    if (b != nullptr && b->kind == EMBI_BK_INLINE) {
        return EMBI_FSZ_INLINE;
    }
    return EMBI_FSZ_FULL;
}

embi_frag *embi_lay_new_frag(embi_lctx *c, embi_box *b, uint8_t kind)
{
    embi_frag *f;
    size_t tier = frag_tier(b, kind);
    c->ld->frag_allocs++;
    if (b != nullptr && embi_lay_is_table(b)) {
        embi_tfrag *tf = (embi_tfrag *)embi_lay_alloc(&c->ld->fa, sizeof(embi_tfrag), alignof(embi_tfrag));
        memset(tf, 0, sizeof *tf);
        f = &tf->f;
        f->flags = EMBI_FF_TABLE;
    } else if (tier == EMBI_FSZ_FULL && c->ld->free_frag != nullptr) {
        f = c->ld->free_frag;
        c->ld->free_frag = f->next;
        c->ld->frag_reused++;
        memset(f, 0, sizeof *f);
    } else {
#ifdef EMBI_LAY_ASAN
        /* Sanitizer builds allocate the whole record and poison what lies past the tier. */
        f = (embi_frag *)embi_lay_alloc(&c->ld->fa, sizeof(embi_frag), alignof(embi_frag));
        memset(f, 0, sizeof *f);
        if (tier < sizeof(embi_frag)) {
            ASAN_POISON_MEMORY_REGION((char *)f + tier, sizeof(embi_frag) - tier);
        }
#else
        size_t sz = (tier + 7u) & ~(size_t)7u;
        f = (embi_frag *)embi_lay_alloc(&c->ld->fa, sz, alignof(embi_frag));
        memset(f, 0, sz);
#endif
    }
    f->box = b;
    f->kind = kind;
    if (kind == EMBI_FK_BOX && tier == EMBI_FSZ_INLINE) {
        f->inl = 1;
    }
    if (kind != EMBI_FK_TEXT) {
        f->bl_first = NAN;
        f->bl_last = NAN;
    }
    if (b != nullptr && !embi_box_is_text(b) && b->frag == nullptr) {
        b->frag = f;
    }
    return f;
}

void embi_lay_frag_append(embi_frag *parent, embi_frag *child)
{
    child->parent = parent;
    child->next = nullptr;
    if (parent->first == nullptr) {
        parent->first = child;
    } else {
        parent->last->next = child;
    }
    parent->last = child;
}

/* ---- metrics records (embi_fmet) ---- */

/* The interned record equal to *key: fragments with the same border, padding and margins share one (an
 * all-zero key is nullptr). The table is direct-mapped and emptied with the fragment arena, so a record
 * lives as long as the fragments of the layout that made it. */
const embi_fmet *embi_lay_met(embi_lay_doc *ld, const embi_fmet *key)
{
    uint64_t w[6];
    memcpy(w, key, sizeof w);
    if ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5]) == 0) {
        return nullptr;
    }
    uint64_t h = (w[0] * 0x9E3779B97F4A7C15ull) ^ (w[1] * 0xC2B2AE3D27D4EB4Full) ^ (w[2] * 0x165667B19E3779F9ull) ^
                 (w[3] * 0x27D4EB2F165667C5ull) ^ (w[4] * 0x94D049BB133111EBull) ^ (w[5] * 0xBF58476D1CE4E5B9ull);
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    const embi_fmet **slot = &ld->mtab[(uint32_t)(h >> 54) & (EMBI_MET_SLOTS - 1u)];
    if (*slot != nullptr && memcmp(*slot, key, sizeof *key) == 0) {
        return *slot;
    }
    embi_fmet *m = embi_lay_alloc(&ld->fa, sizeof *m, alignof(embi_fmet));
    if (m == nullptr) {
        return nullptr;
    }
    *m = *key;
    *slot = m;
    return m;
}

static void met_get(const embi_frag *f, embi_fmet *out)
{
    if (f->met != nullptr) {
        *out = *f->met;
    } else {
        memset(out, 0, sizeof *out);
    }
}

/* Stores m as the metrics of f: in place for a fragment with a private record, else interned. */
static void met_set(embi_lay_doc *ld, embi_frag *f, const embi_fmet *m)
{
    if ((f->flags & EMBI_FF_EXT) != 0) {
        *(embi_fmet *)(uintptr_t)f->met = *m;
    } else {
        f->met = embi_lay_met(ld, m);
    }
}

void embi_lay_frag_set_edges(embi_lay_doc *ld, embi_frag *f, const embi_edges *bdr, const embi_edges *pad)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX);
    embi_fmet m;
    met_get(f, &m);
    m.bdr[0] = bdr->l;
    m.bdr[1] = bdr->t;
    m.bdr[2] = bdr->r;
    m.bdr[3] = bdr->b;
    m.pad[0] = pad->l;
    m.pad[1] = pad->t;
    m.pad[2] = pad->r;
    m.pad[3] = pad->b;
    met_set(ld, f, &m);
}

void embi_lay_frag_set_mg(embi_lay_doc *ld, embi_frag *f, float l, float t, float r, float b)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX);
    const float cur[4] = {embi_frag_mg(f, 0), embi_frag_mg(f, 1), embi_frag_mg(f, 2), embi_frag_mg(f, 3)};
    const float nv[4] = {l, t, r, b};
    if (memcmp(cur, nv, sizeof cur) == 0) {
        return; /* nothing changes (bit for bit: a missing record reads as +0) */
    }
    embi_fmet m;
    met_get(f, &m);
    m.mg[0] = l;
    m.mg[1] = t;
    m.mg[2] = r;
    m.mg[3] = b;
    met_set(ld, f, &m);
}

embi_fext *embi_lay_frag_ext(embi_lay_doc *ld, embi_frag *f)
{
    EMBI_ASSERT(f->kind == EMBI_FK_BOX);
    if ((f->flags & EMBI_FF_EXT) == 0) {
        embi_fext *x;
        if (ld->free_ext != nullptr) {
            x = ld->free_ext;
            ld->free_ext = (embi_fext *)x->sticky;
        } else {
            x = embi_lay_alloc(&ld->fa, sizeof *x, alignof(embi_fext));
        }
        if (x == nullptr) {
            return nullptr;
        }
        memset(x, 0, sizeof *x);
        if (f->met != nullptr) {
            x->m = *f->met;
        }
        f->met = &x->m;
        f->flags |= EMBI_FF_EXT;
    }
    return (embi_fext *)(uintptr_t)f->met;
}

/* The overflow rectangle of a fragment that does not store one: its border box and, for a line or the
 * piece of an inline box, the border boxes and overflow of its children (those that do not clip them). */
static void ov_children(const embi_frag *f, float out[4])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = f->w;
    out[3] = f->h;
    for (const embi_frag *k = f->first; k != nullptr; k = k->next) {
        float x0 = k->x, y0 = k->y;
        float x1 = k->x + k->w, y1 = k->y + k->h;
        bool clips = k->kind == EMBI_FK_TEXT || (k->flags & (EMBI_FF_SCROLL | EMBI_FF_CLIP)) != 0;
        if (!clips) {
            float co[4];
            embi_frag_ov(k, co);
            x0 = fminf(x0, k->x + co[0]);
            y0 = fminf(y0, k->y + co[1]);
            x1 = fmaxf(x1, k->x + co[2]);
            y1 = fmaxf(y1, k->y + co[3]);
        }
        out[0] = fminf(out[0], x0);
        out[1] = fminf(out[1], y0);
        out[2] = fmaxf(out[2], x1);
        out[3] = fmaxf(out[3], y1);
    }
}

void embi_frag_ov(const embi_frag *f, float out[4])
{
    if (f->kind == EMBI_FK_TEXT) {
        out[0] = 0;
        out[1] = 0;
        out[2] = f->w;
        out[3] = f->h;
    } else if (f->kind == EMBI_FK_LINE || f->inl != 0) {
        ov_children(f, out);
    } else {
        out[0] = f->ov[0];
        out[1] = f->ov[1];
        out[2] = f->ov[2];
        out[3] = f->ov[3];
    }
}

void embi_lay_ov_add_child(embi_frag *f, const embi_frag *child)
{
    /* A line and the piece of an inline box store no overflow: embi_frag_ov adds their children up. */
    if (f->kind == EMBI_FK_LINE || f->kind == EMBI_FK_TEXT || f->inl != 0) {
        return;
    }
    /* The border box always counts. The child's own overflow counts unless the child clips it. A TEXT
     * fragment has no overflow beyond its border box. */
    bool clips = child->kind == EMBI_FK_TEXT || (child->flags & (EMBI_FF_SCROLL | EMBI_FF_CLIP)) != 0;
    float x0 = child->x, y0 = child->y;
    float x1 = child->x + child->w, y1 = child->y + child->h;
    if (!clips) {
        float co[4];
        embi_frag_ov(child, co);
        x0 = fminf(x0, child->x + co[0]);
        y0 = fminf(y0, child->y + co[1]);
        x1 = fmaxf(x1, child->x + co[2]);
        y1 = fmaxf(y1, child->y + co[3]);
    }
    if (x0 < f->ov[0]) {
        f->ov[0] = x0;
    }
    if (y0 < f->ov[1]) {
        f->ov[1] = y0;
    }
    if (x1 > f->ov[2]) {
        f->ov[2] = x1;
    }
    if (y1 > f->ov[3]) {
        f->ov[3] = y1;
    }
}

void embi_lay_frag_set_size(embi_frag *f, float w, float h)
{
    f->w = w;
    f->h = h;
    if (f->kind == EMBI_FK_BOX && f->inl == 0) {
        f->ov[0] = 0;
        f->ov[1] = 0;
        f->ov[2] = w;
        f->ov[3] = h;
    }
}

/* The keyword that a sizing property is set to, or 0. */
static uint16_t kw_of(const embi_value *v)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* fit-content(<length-percentage>) (CSS Sizing 3 section 5.2.2): min(max-content, max(min-content,
 * arg)). NaN when v is not that function, or its argument is not a length. With border-box sizing
 * the argument is a border-box size. */
static float fit_content_fn(const embi_lctx *c, const embi_box *b, const embi_value *v, float cbw,
                            float edges, bool border_box, float imn, float imx)
{
    if (v == nullptr || v->kind != EMBI_V_FUNC || v->func.n < 1 ||
        !name_is(v->func.name, "fit-content")) {
        return NAN;
    }
    float arg = embi_lay_len(c, b, &v->func.args[0], cbw);
    if (isnan(arg)) {
        return NAN;
    }
    if (border_box) {
        arg = clamp0(arg - edges);
    }
    float r = arg < imn ? imn : arg;
    return r > imx ? imx : r;
}

/* The top margins a box collapses with its parent's top (CSS 2.1 section 8.3.1): its own top margin
 * and, when the box collapses through its top edge, the top margins of its first in-flow child, as
 * block_pass resolves them. A child is placed by this estimate before it is laid out, and the
 * estimate is checked against the layout (see flow_child). */
static void top_margins(const embi_lctx *c, const embi_box *b, float cbw, embi_strut *s)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, b, cbw, &bdr, &pad, &mar);
    embi_strut_add(s, isnan(mar.t) ? 0.0f : mar.t);
    if (b->kind != EMBI_BK_BLOCK || (b->flags & (EMBI_BF_IFC | EMBI_BF_BFC | EMBI_BF_ROOT)) != 0 ||
        bdr.t != 0 || pad.t != 0) {
        return;
    }
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->flags & (EMBI_BF_ABSPOS | EMBI_BF_FLOAT)) {
            continue;
        }
        if ((k->flags & EMBI_BF_INLINE_LEVEL) == 0) {
            top_margins(c, k, cbw, s);
        }
        return;
    }
}

/* The state of block_flow: the children of one block container, laid out in order. The outputs
 * are ch (the content height, with the trailing margin when the bottom does not collapse), top (the
 * margin absorbed by the block's top), bot (the margin left for the block's bottom), ttop (the top
 * margin of the first child when that child is self-collapsing: the block's own position if it
 * collapses through too), bl1 and bl2 (the first and last baselines relative to the block's
 * border-box top) and empty (no in-flow content). */
typedef struct flow_st {
    embi_lctx *c;
    embi_frag *f;      /* the container's fragment */
    float cw;          /* the content width: the children's containing block width */
    float span_w;      /* a multi-column container: the width of a column-span: all child (else NaN) */
    float cb_h;        /* the children's containing block height (percentage base) */
    float ox, oy;      /* the content box origin in the border box */
    float cx, cy;      /* the content box origin in the frame of the BFC */
    embi_excl *ex;     /* the exclusion space of the BFC */
    bool ctop, cbot;   /* the top and bottom margins collapse with the first and last child */
    embi_strut strut;  /* the margins pending below the last placed in-flow box */
    float cur;         /* the bottom of the last placed in-flow box, from the content top */
    bool has;          /* an in-flow box has been placed */
    bool any_selfc;    /* a self-collapsing child has been placed */
    embi_strut top, bot, ttop;
    float ch, bl1, bl2;
    bool strut_clr;    /* the strut belongs to an empty box with clearance: it must not collapse through the bottom */
    bool empty;
} flow_st;

/* Lays out a child box into r. again: the child was laid out before, so its fragments are dropped
 * first. Returns false when the box has no fragment. */
static bool lay_child(embi_lctx *c, embi_box *k, const embi_cs *ccs, embi_lres *r, bool again, uint32_t cl_left)
{
    if (c->cl.on) {
        c->cl.left = cl_left; /* a child laid out again counts its lines again from the same start (linecl.c) */
    }
    if (again) {
        embi_lay_clear_frags(c, k);
    }
    memset(r, 0, sizeof *r);
    if (!again && c->ld->reuse_on && embi_lay_incr_reuse(c, k, ccs, r)) {
        return r->frag != nullptr; /* W6b: a partial relayout keeps the fragments of the children that did not change */
    }
    c->via_flow = true; /* W6b: this call comes from block flow (embi_lay_box reads and clears it) */
    embi_lay_box(c, k, ccs, r);
    return r->frag != nullptr;
}

/* The top border edge of an in-flow child in the content box, from its laid out result. A child
 * that was moved (it clears, or it shifted below floats) sits at est_local, and a child that absorbs
 * the block's top margin sits at the top of the content box. */
static float child_top(const flow_st *s, const embi_lres *r, bool moved, float est_local, bool absorb)
{
    if (moved) {
        return est_local;
    }
    if (absorb) {
        return 0.0f;
    }
    embi_strut u = s->strut;
    embi_strut_merge(&u, r->selfc ? r->top_s : r->mt_s);
    return s->cur + embi_strut_val(&u);
}

/* The state of a block container's children before the first one is placed. */
static flow_st flow_init(embi_lctx *c, embi_frag *f, float cw, float cb_h, float ox, float oy,
                         float cx, float cy, embi_excl *ex, bool ctop, bool cbot)
{
    flow_st s;
    memset(&s, 0, sizeof s);
    s.c = c;
    s.f = f;
    s.cw = cw;
    s.span_w = NAN;
    s.cb_h = cb_h;
    s.ox = ox;
    s.oy = oy;
    s.cx = cx;
    s.cy = cy;
    s.ex = ex;
    s.ctop = ctop;
    s.cbot = cbot;
    s.bl1 = NAN;
    s.bl2 = NAN;
    return s;
}

/* A float (CSS 2.1 section 9.5.1): placed in the exclusion space at the flow position, with margins
 * that do not collapse. It takes no room in the flow. */
static void flow_float(flow_st *s, embi_box *k)
{
    float top = s->cy + s->cur + embi_strut_val(&s->strut);
    float bx = 0, by = 0;
    embi_frag *cf = embi_lay_float(s->c, k, s->ex, s->cx, s->cw, s->cb_h, top, &bx, &by);
    if (cf == nullptr) {
        return;
    }
    cf->x = s->ox + (bx - s->cx);
    cf->y = s->oy + (by - s->cy);
    embi_lay_frag_append(s->f, cf);
    embi_lay_ov_add_child(s->f, cf);
}

/* An in-flow child (CSS 2.1 sections 8.3.1 and 9.5.2). Its flow position is estimated before the
 * layout, with the margins it collapses with. A child with clearance sits at the bottom of the
 * floats it clears (its margins do not collapse with those above). A child that establishes a BFC
 * must not overlap the margin boxes of the floats: it is placed beside them in the free interval,
 * or below them when it fits nowhere (CSS 2.1 section 9.5). When floats are in play, the estimate
 * is checked against the result and the child is laid out again at the position it really has. */
/* The most placement steps a box beside floats takes (each step moves it past a float or narrows its
 * interval). Past this, it goes below all the floats at full width. */
#define EMBI_BFC_PLACE_MAX 64

static void flow_child_in(flow_st *s, embi_box *k)
{
    embi_lctx *c = s->c;
    embi_excl *ex = s->ex;
    uint32_t cl0 = c->cl.left;
    bool bfc = (k->flags & EMBI_BF_BFC) != 0;
    uint8_t clr = embi_lay_clear_sides(c, k);
    /* A BFC child beside floats is laid out by the placement loop below, once per step, so it needs
     * no throwaway layout first. */
    bool beside = bfc && ex->rects.len > 0;
    /* The top margins that collapse with the ones above only matter when floats or clearance are in
     * play. Without them a wrong estimate costs nothing: a child that places floats is checked after
     * its layout and laid out again when the estimate missed. */
    embi_strut ts = {0, 0};
    top_margins(c, k, s->cw, &ts);
    embi_strut u = s->strut;
    embi_strut_merge(&u, ts);
    float hyp = s->cur + embi_strut_val(&u);
    bool absorb = !s->has && s->ctop;
    float est_local = absorb ? 0.0f : hyp;
    bool moved = false;
    if (clr != 0 && ex->rects.len > 0) {
        /* The hypothetical border top. An absorbed child's margin is in the parent's top margin, so its
         * border top is the content top (local 0), and any float bottom below that is cleared. */
        float cb = embi_excl_clear_y(ex, clr);
        float hyp_pos = absorb ? 0.0f : hyp;
        if (cb - s->cy > hyp_pos) {
            moved = true;
            est_local = cb - s->cy;
        }
    }

    uint32_t mark = ex->rects.len;
    embi_cs ccs = {s->cw, s->cw, s->cb_h, EMBI_FIT_FILL, ex, s->cx, s->cy + est_local, false, 0.0f, 0.0f};
    embi_lres r;
    memset(&r, 0, sizeof r);
    float L = est_local;
    if (!beside) {
        if (!lay_child(c, k, &ccs, &r, false, cl0)) {
            return;
        }
        if (r.selfc && !moved && clr != 0 && ex->rects.len > 0) {
            /* An empty box that clears is judged by where it would sit without its own margins: its
             * margins collapse together, not with the ones above it, so a large margin does not hide
             * the clearance (Chromium; CSS 2.1 section 8.3.1: a box with clearance is not adjoining
             * through). */
            float cb = embi_excl_clear_y(ex, clr);
            float p0 = absorb ? 0.0f : s->cur + embi_strut_val(&s->strut);
            if (cb - s->cy > p0) {
                moved = true;
                est_local = cb - s->cy;
                ex->rects.len = mark;
                ccs.bfc_y = s->cy + est_local;
                if (!lay_child(c, k, &ccs, &r, true, cl0)) {
                    return;
                }
            }
        }
        L = child_top(s, &r, moved, est_local, absorb);
        if (!bfc && ex->rects.len > 0 && fabsf(s->cy + L - ccs.bfc_y) > 0.001f) {
            /* The estimate missed (a margin collapse it did not see), and floats may have been placed
             * from it: drop them and lay the child out at the position it has. */
            ex->rects.len = mark;
            ccs.bfc_y = s->cy + L;
            if (!lay_child(c, k, &ccs, &r, true, cl0)) {
                return;
            }
            L = child_top(s, &r, moved, est_local, absorb);
        }
    }

    bool forced = false;
    if (beside) {
        /* The free interval [lo, hi] at the top edge narrows the box's containing block (its auto
         * margins and width resolve in it). Only the border box keeps out of the float margin boxes
         * (CSS 2.1 section 9.5): a specified margin on a side that a float intrudes on is absorbed by
         * the float band (the border box starts at max(lo, cb_left + ml)), so the box gets the margin
         * that remains inside the interval. A box that still overlaps a float lower down takes the
         * narrower interval, and a box that fits nowhere moves below the next float. Each step lays
         * the box out once, for the interval and top it has now. */
        embi_edges sbd, spd, smg;
        embi_lay_edges(c, k, s->cw, &sbd, &spd, &smg);
        float cr = s->cx + s->cw;
        float lo = NAN, hi = NAN;
        bool placed = false;
        for (int it = 0; it < EMBI_BFC_PLACE_MAX; it++) {
            float top = s->cy + L;
            if (isnan(lo)) {
                embi_excl_band(ex, s->cx, cr, top, 0.0f, &lo, &hi);
            }
            ccs.bfc_x = lo;
            ccs.avail_w = hi - lo;
            ccs.mg_ovr = true;
            ccs.ml_ovr = smg.l;
            ccs.mr_ovr = smg.r;
            if (!isnan(smg.l)) {
                ccs.ml_ovr = lo > s->cx ? fmaxf(0.0f, smg.l - (lo - s->cx)) : smg.l;
            }
            if (!isnan(smg.r)) {
                ccs.mr_ovr = hi < cr ? fmaxf(0.0f, smg.r - (cr - hi)) : smg.r;
            }
            if (!lay_child(c, k, &ccs, &r, it > 0, cl0)) {
                return;
            }
            /* Once moved below a float, the box stays at that float's bottom (its margins no longer place it). */
            float L2 = forced ? L : child_top(s, &r, moved, est_local, absorb);
            if (fabsf(L2 - L) > 0.001f) {
                /* The child's own margins are not the estimate: the band is taken again at its top. */
                L = L2;
                lo = NAN;
                continue;
            }
            float h = r.frag->h, bw = r.frag->w;
            /* The margin box that has to fit uses the margins as given (a margin that came from the
             * width being over-constrained is not room, and auto margins take what is left). */
            float fml = isnan(ccs.ml_ovr) ? 0.0f : ccs.ml_ovr;
            float fmr = isnan(ccs.mr_ovr) ? 0.0f : ccs.mr_ovr;
            /* An over-constrained box gives up its margin at the inline end (CSS 2.1 section 10.3.3), so that
             * margin is not part of what has to fit. */
            if (embi_lay_cb_rtl(c, k)) {
                fml = 0.0f;
            } else {
                fmr = 0.0f;
            }
            float l2, r2;
            embi_excl_band(ex, s->cx, cr, top, h, &l2, &r2);
            if (l2 > lo || r2 < hi) {
                lo = l2;
                hi = r2;
                continue;
            }
            if (lo + fml + bw + fmr <= hi + 0.01f) {
                placed = true;
                break;
            }
            float nx = embi_excl_next_y(ex, top, h);
            if (!isfinite(nx)) {
                placed = true;
                break;
            }
            L = nx - s->cy;
            forced = true;
            lo = NAN;
        }
        if (!placed) {
            /* The step cap is reached (a page does not get there: each step passes a float or narrows
             * the interval). The box goes below every float, at full width, where nothing is in the
             * way. */
            float below = embi_excl_bottom(ex) - s->cy;
            if (below > L) {
                L = below;
            }
            ccs.bfc_x = s->cx;
            ccs.avail_w = s->cw;
            ccs.mg_ovr = false;
            if (!lay_child(c, k, &ccs, &r, true, cl0)) {
                return;
            }
            forced = true;
        }
    }

    bool moved_all = moved || forced;
    embi_frag *cf = r.frag;
    float ml = isnan(r.ml) ? 0.0f : r.ml;
    float mgr = embi_frag_mg(cf, 2);
    if (bfc && ccs.avail_w < s->cw) {
        /* The box was laid out in a narrower interval. Its used right margin, when auto, is the rest
         * of the containing block (the margin the box would have had in the full width). */
        embi_edges bd, pd, mg;
        embi_lay_edges(c, k, s->cw, &bd, &pd, &mg);
        if (isnan(mg.r)) {
            mgr = s->cw - ml - cf->w;
        }
    }
    cf->x = s->ox + (ccs.bfc_x - s->cx) + ml;
    embi_lay_frag_set_mg(c->ld, cf, ml, embi_frag_mg(cf, 1), mgr, embi_frag_mg(cf, 3));
    cf->y = s->oy + L;

    if (r.selfc && moved) {
        /* An empty box with clearance does not collapse through: its border top sits at the floats'
         * bottom and its margins stay inside the flow (the parent's top and bottom margins get none of
         * them). Its own margins collapse together, and the strut that follows starts at the top
         * margin edge, cur + strut = clearance - top margin + collapsed margin. A margin set with
         * a negative part can leave the top margin edge above the content top, which is fine. */
        embi_lay_frag_append(s->f, cf);
        embi_lay_ov_add_child(s->f, cf);
        s->any_selfc = true;
        s->has = true;
        s->cur = L - embi_strut_val(&r.top_s);
        s->strut_clr = true;
        s->strut = (embi_strut){0, 0};
        embi_strut_merge(&s->strut, r.mt_s);
        return;
    }
    if (r.selfc) {
        /* Self-collapsing: it takes no room, and its margins join the strut for the siblings after
         * it. A child that moved starts a new strut (its top margin is inside the clearance). */
        if (!s->any_selfc && !s->has) {
            embi_strut_merge(&s->ttop, r.top_s);
        }
        s->any_selfc = true;
        if (moved_all) {
            s->strut = (embi_strut){0, 0};
        }
        embi_strut_merge(&s->strut, r.mt_s);
        embi_strut_merge(&s->strut, r.mb_s);
        embi_lay_frag_append(s->f, cf);
        embi_lay_ov_add_child(s->f, cf);
        return;
    }
    if (!moved_all) {
        /* The first real child absorbs the block's top margin (they collapse through). */
        embi_strut_merge(&s->strut, r.mt_s);
        if (absorb) {
            s->top = s->strut;
        }
    }
    s->strut = (embi_strut){0, 0};
    s->strut_clr = false;
    embi_lay_frag_append(s->f, cf);
    embi_lay_ov_add_child(s->f, cf);
    /* A block-level table gives its parent no baseline (WPT tentative/baseline-table.html); an
     * inline-table does, through its atomic inline. */
    bool tbl = cf->box != nullptr && embi_lay_is_table(cf->box) && (cf->box->flags & EMBI_BF_INLINE_LEVEL) == 0;
    const embi_box *pbx = s->f->box;
    bool in_atomic = pbx != nullptr && (pbx->flags & EMBI_BF_INLINE_LEVEL) != 0 && pbx->kind == EMBI_BK_BLOCK;
    if (!tbl && in_atomic && cf->box != nullptr && (cf->box->flags & EMBI_BF_CLIP) != 0 &&
        (cf->box->flags & EMBI_BF_INLINE_LEVEL) == 0) {
        /* A block that clips its overflow has no baseline of its own: its bottom margin edge stands in for it, as for
         * an atomic inline (CSS 2.1 10.8.1; baseline-block-with-overflow-001). */
        float edge = cf->y + cf->h + embi_frag_mg(cf, 3);
        if (isnan(s->bl1)) {
            s->bl1 = edge;
        }
        s->bl2 = edge;
    } else {
        if (!tbl && isnan(s->bl1) && !isnan(cf->bl_first)) {
            s->bl1 = cf->y + cf->bl_first;
        }
        if (!tbl && !isnan(embi_fl(cf)->bl_last)) {
            s->bl2 = cf->y + embi_fl(cf)->bl_last;
        }
    }
    s->cur = L + cf->h;
    embi_strut_merge(&s->strut, r.mb_s);
    s->has = true;
}

static void flow_child(flow_st *s, embi_box *k)
{
    embi_lctx *c = s->c;
    uint32_t cl0 = c->cl.left;
    flow_child_in(s, k);
    if (c->cl.on && cl0 > 0 && (embi_lay_clamp_indep(k) || c->cl.left == cl0)) {
        /* An independent box is one unit of the clamp, and so is a block with no line or unit inside it (linecl.c). */
        c->cl.left = cl0 - 1;
    }
}

/* The in-flow children of a block container, laid out one after another (CSS 2.1 section 9.4.1),
 * and the outputs of the state s. */
static void block_flow(flow_st *s, embi_box *b)
{
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        if (k->flags & EMBI_BF_ABSPOS) {
            /* Out of flow (position.c): the static position is the hypothetical position of the box,
             * at the content start in the inline direction and after the previous boxes and their
             * margins in the block direction (CSS 2.1 10.6.4). The box is placed after the layout. */
            const embi_value *dv = embi_lay_prop(s->c, b, EMBI_PROP_direction);
            bool rtl = dv != nullptr && dv->kind == EMBI_V_KEYWORD && dv->kw == EMBI_KW_rtl;
            float sx = rtl ? s->ox + s->cw : s->ox;
            float sy = s->oy + ((!s->has && s->ctop) ? 0.0f : s->cur + embi_strut_val(&s->strut));
            embi_lay_abspos_at(s->c, k, b, sx, sy, nullptr);
            continue;
        }
        if (s->c->cl.on && s->c->cl.left == 0) {
            /* Past the clamp point: nothing of the content is laid out (linecl.c). */
            float sy = s->oy + ((!s->has && s->ctop) ? 0.0f : s->cur + embi_strut_val(&s->strut));
            embi_lay_clamp_skip(s->c, k, b, s->ox, sy);
            continue;
        }
        if (embi_lay_float_out_of_flow(k)) {
            flow_float(s, k);
        } else if (!isnan(s->span_w) && embi_lay_is_spanner(s->c, k)) {
            float cw0 = s->cw;
            s->cw = s->span_w;
            flow_child(s, k);
            s->cw = cw0;
        } else {
            flow_child(s, k);
        }
    }

    s->empty = !s->has;
    if (!s->has) {
        /* Only self-collapsing children (or none). Their margins collapse through the block on the
         * edge that is open: with the top margin when the top is open, else with the bottom margin
         * when the bottom is open, else they separate the block's edges (content height). */
        s->bot = (embi_strut){0, 0};
        s->ch = 0;
        if (s->ctop) {
            s->top = s->strut;
        } else if (s->cbot) {
            s->bot = s->strut;
        } else {
            s->ch = embi_strut_val(&s->strut);
        }
        return;
    }
    if (s->cbot && !s->strut_clr) {
        s->bot = s->strut;
        s->ch = s->cur;
    } else {
        /* CSS 2.1 8.3.1: a last child's bottom margin does not collapse with the parent's when it
         * collapses with a top margin that has clearance. */
        s->bot = (embi_strut){0, 0};
        s->ch = s->cur + embi_strut_val(&s->strut);
    }
}

/* True for a size that depends on the content: a keyword other than auto, none and stretch, or a
 * function (fit-content()). */
static bool needs_intrinsic(const embi_value *v)
{
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_FUNC) {
        return true;
    }
    return v->kind == EMBI_V_KEYWORD && v->kw != EMBI_KW_auto && v->kw != EMBI_KW_none &&
           v->kw != EMBI_KW_stretch;
}

/* An HTML button element: even when it is block-level its auto width is shrink-to-fit (HTML Standard
 * section 15.5.3, the button layout). */
static bool is_html_button(const embi_box *b)
{
    return (b->flags2 & EMBI_BF2_BUTTON) != 0;
}

/* Whether the height a ratio gives a box with an auto height counts as definite for its content: blocks and grids, and
 * wrapping flex containers whose main axis is the block axis (flex-aspect-ratio-007: the lines break at the ratio height; a
 * container without wrap, or a row one, may grow with its content past it: flex-aspect-ratio-040, -041). */
static bool ratio_h_definite(const embi_lctx *c, const embi_box *b)
{
    if (b->kind == EMBI_BK_BLOCK || b->kind == EMBI_BK_GRID) {
        return true;
    }
    if (b->kind == EMBI_BK_FLEX) {
        const embi_value *fd = embi_lay_prop(c, b, EMBI_PROP_flex_direction);
        const embi_value *fw = embi_lay_prop(c, b, EMBI_PROP_flex_wrap);
        return fd != nullptr && fd->kind == EMBI_V_KEYWORD && (fd->kw == EMBI_KW_column || fd->kw == EMBI_KW_column_reverse) &&
               fw != nullptr && fw->kind == EMBI_V_KEYWORD && fw->kw != EMBI_KW_nowrap;
    }
    return false;
}

/* The used content width of a block (CSS 2.1 section 10.3.3, and 10.4 for the min and max). The
 * result is the content width, plus any scrollbar gutter the caller adds separately. A width that
 * transfers through the aspect ratio from a definite height is used for shrink-to-fit boxes
 * (CSS Sizing 4 section 5). */
static float block_width(embi_lctx *c, embi_box *b, const embi_cs *cs, float edges_h, float edges_v,
                         float *ml, float *mr, bool border_box, float hspec, bool has_ratio,
                         float ratio, bool ratio_bb, float sbv, float minh, float maxh)
{
    float cbw = cs->cb_w;
    /* A definite height inside its min-height and max-height is the one the ratio transfers from; an auto height
     * transfers its min-height and max-height as a minimum and maximum width (CSS Sizing 4 section 5.1). */
    if (has_ratio && !isnan(hspec)) {
        hspec = hspec < minh ? minh : (hspec > maxh ? maxh : hspec);
    }
    float tmin_w = 0, tmax_w = INFINITY;
    if (has_ratio && isnan(hspec) && !embi_lay_is_table(b)) {
        if (minh > 0) {
            tmin_w = clamp0(ratio_bb ? (minh + edges_v) * ratio - edges_h : minh * ratio);
        }
        if (isfinite(maxh)) {
            tmax_w = clamp0(ratio_bb ? (maxh + edges_v) * ratio - edges_h : maxh * ratio);
        }
    }
    if (c->force_box == b && c->force_wset) {
        /* A flex item laid out at its used main or cross size (W3e): the size is final. */
        return clamp0(c->force_w - edges_h);
    }
    const embi_value *wv = embi_lay_prop(c, b, EMBI_PROP_width);
    uint16_t wkw = kw_of(wv);
    const embi_value *minv = embi_lay_prop(c, b, EMBI_PROP_min_width);
    const embi_value *maxv = embi_lay_prop(c, b, EMBI_PROP_max_width);
    /* The content sizes are only needed for shrink-to-fit and for keyword and fit-content() sizes.
     * A fill-the-container box with plain lengths never measures its content here. */
    float imn = 0, imx = 0;
    bool have_intr = false; /* imn and imx hold the content sizes */
    bool button = is_html_button(b);
    /* A table is shrink-to-fit when its width is auto, and never narrower than its min-content width
     * (CSS Tables 3 section 3.5). */
    bool table = embi_lay_is_table(b);
    if (cs->fit == EMBI_FIT_SHRINK || button || table || needs_intrinsic(wv) || needs_intrinsic(minv) ||
        needs_intrinsic(maxv)) {
        /* The box's percentage height resolves against its containing block's height in its intrinsic sizes too, and
         * a definite one is the basis of its children's (intrinsic-percent-replaced-015, aspect-ratio/intrinsic-size-006). */
        embi_ih ih = embi_lay_ih_push(c, cs->cb_h);
        embi_lay_intrinsic(c, b, &imn, &imx);
        embi_lay_ih_pop(c, ih);
        have_intr = true;
        if (table && embi_table_in_cell(b)) {
            embi_table_intrinsic_own(c, b, &imn, &imx);
        }
        /* The scrollbar gutter that is always there is part of the intrinsic size (CSS Overflow 3; WPT
         * css-grid compute-intrinsic-widths-scrollbar-001.html): block_pass takes it from the content
         * width again. */
        imn += sbv;
        imx += sbv;
    }
    /* A ratio box with a definite height: its min-content and max-content widths are the width the ratio transfers
     * (CSS Sizing 4 section 5.1), so the content keywords of width, min-width and max-width give that; the content's
     * own min-content size stays the automatic minimum (aspect-ratio/intrinsic-size-010, -011). */
    float content_imn = imn;
    bool ratio_def = has_ratio && !isnan(hspec) && !table;
    if (ratio_def) {
        imn = imx = clamp0(ratio_bb ? (hspec + edges_v) * ratio - edges_h : hspec * ratio);
    }
    float ml0 = isnan(*ml) ? 0 : *ml;
    float mr0 = isnan(*mr) ? 0 : *mr;
    float avail_content = cs->avail_w - edges_h - ml0 - mr0;
    float cw = NAN;
    bool width_auto = false;
    bool width_was_auto = false;
    bool ratio_w = false; /* the width came from the definite height through the ratio */
    bool fill_ml_auto = false, fill_mr_auto = false; /* auto margins zeroed for the fill width (below) */

    float fc = fit_content_fn(c, b, wv, cbw, edges_h, border_box, imn, imx);
    if (table && wkw == EMBI_KW_stretch) {
        /* A table with width: stretch fills the available width, never below its min-content width. */
        cw = avail_content > imn ? avail_content : imn;
    } else if (wv == nullptr || embi_lay_is_auto(wv) || wkw == EMBI_KW_stretch) {
        width_auto = true;
    } else if ((wkw == EMBI_KW_min_content || wkw == EMBI_KW_max_content || wkw == EMBI_KW_fit_content) && ratio_def) {
        /* A ratio box with a definite block size: the intrinsic inline size is the one the ratio transfers, and its
         * min-width: auto is the content's (ratio_w). */
        cw = ratio_bb ? (hspec + edges_v) * ratio - edges_h : hspec * ratio;
        ratio_w = true;
    } else if (wkw == EMBI_KW_min_content) {
        cw = imn;
    } else if (wkw == EMBI_KW_max_content) {
        cw = table ? embi_table_max_content_plain(c, b) : imx;
    } else if (wkw == EMBI_KW_fit_content) {
        float fit = avail_content;
        cw = imx < (imn > fit ? imn : fit) ? imx : (imn > fit ? imn : fit);
    } else if (!isnan(fc)) {
        cw = fc;
    } else {
        float len = embi_lay_len(c, b, wv, cbw);
        if (isnan(len)) {
            width_auto = true;
        } else {
            cw = border_box ? len - edges_h : len;
            cw = clamp0(cw);
        }
    }

    width_was_auto = width_auto;
    if (width_auto && has_ratio && !isnan(hspec)) {
        /* The width follows the definite height through the ratio, for a block that would stretch too (CSS Sizing 4
         * section 5.1: a ratio box with a definite block size has no stretch-fit inline size). */
        cw = ratio_bb ? (hspec + edges_v) * ratio - edges_h : hspec * ratio;
        width_auto = false;
        ratio_w = true;
    }
    if (table && !width_auto && cw < imn) {
        cw = imn;
    }
    if (width_auto && wkw == EMBI_KW_stretch && cs->fit == EMBI_FIT_SHRINK && !button && !table) {
        /* width: stretch in a shrink-to-fit context (a float, an absolutely positioned box, an inline-block) fills the
         * available space instead of fitting the content (CSS Sizing 4 section 6.1; stretch-float, stretch/abspos-1). */
        cw = clamp0(avail_content);
        width_auto = false;
    }
    if (width_auto) {
        if (cs->fit == EMBI_FIT_SHRINK || button || table) {
            float fit = avail_content;
            if (fit < imn) {
                fit = imn;
            }
            cw = fit < imx ? fit : imx;
            if (table && (b->tflags & EMBI_TF_FILL) != 0) {
                cw = fit; /* unbounded max-content: the table fills the available width */
            }
        } else {
            /* Auto margins are 0 for the fill width; when min-width or max-width changes it below, the rules run again
             * with that width (CSS 2.1 section 10.4), so the auto margins go back to auto and the caller resolves them
             * (body { max-width: 560px; margin: 0 auto } is centred). */
            fill_ml_auto = isnan(*ml);
            fill_mr_auto = isnan(*mr);
            if (fill_ml_auto) {
                *ml = 0;
            }
            if (fill_mr_auto) {
                *mr = 0;
            }
            cw = cs->avail_w - edges_h - *ml - *mr;
        }
    }
    cw = clamp0(cw);
    const float fill_cw = cw;
    /* calc-size(): the calc-sum sees the basis width as `size`, then min-width and max-width apply. */
    const embi_value *cwx = embi_lay_size_expr(c, b, EMBI_PROP_width);
    if (cwx != nullptr) {
        float r = embi_lay_size_eval(c, b, cwx, border_box ? cw + edges_h : cw, cbw);
        if (!isnan(r)) {
            cw = clamp0(border_box ? r - edges_h : r);
        }
    }

    /* min-width and max-width: the minimum wins over the maximum. */
    float minw = 0;
    float maxw = INFINITY;
    float auto_min = 0; /* the content-based automatic minimum, which a definite max-width overrides */
    uint16_t mkw = kw_of(minv);
    float mfc = fit_content_fn(c, b, minv, cbw, edges_h, border_box, imn, imx);
    if (mkw == EMBI_KW_min_content) {
        minw = imn;
    } else if (mkw == EMBI_KW_max_content) {
        minw = imx;
    } else if (mkw == EMBI_KW_fit_content) {
        float lo = imn > avail_content ? imn : avail_content;
        minw = imx < lo ? imx : lo;
    } else if (!isnan(mfc)) {
        minw = mfc;
    } else if (!embi_lay_is_auto(minv)) {
        float v = embi_lay_len(c, b, minv, cbw);
        if (!isnan(v)) {
            minw = clamp0(border_box ? v - edges_h : v);
        }
    } else if (ratio_w && !table && (b->flags & EMBI_BF_CLIP) == 0) {
        /* min-width: auto of a ratio box whose width follows its height: the content size is the automatic minimum
         * (CSS Sizing 4 section 5.2; block-aspect-ratio-015). */
        if (!have_intr) {
            float cimx = 0;
            embi_ih ih = embi_lay_ih_push(c, cs->cb_h);
            embi_lay_intrinsic(c, b, &content_imn, &cimx);
            embi_lay_ih_pop(c, ih);
            content_imn += sbv;
        }
        auto_min = content_imn;
    }
    const embi_value *mnx = embi_lay_size_expr(c, b, EMBI_PROP_min_width);
    if (mnx != nullptr) {
        float r = embi_lay_size_eval(c, b, mnx, border_box ? minw + edges_h : minw, cbw);
        if (!isnan(r)) {
            minw = clamp0(border_box ? r - edges_h : r);
        }
    }
    uint16_t xkw = kw_of(maxv);
    float xfc = fit_content_fn(c, b, maxv, cbw, edges_h, border_box, imn, imx);
    if (xkw == EMBI_KW_min_content) {
        maxw = imn;
    } else if (xkw == EMBI_KW_max_content) {
        maxw = imx;
    } else if (xkw == EMBI_KW_fit_content) {
        float lo = imn > avail_content ? imn : avail_content;
        maxw = imx < lo ? imx : lo;
    } else if (!isnan(xfc)) {
        maxw = xfc;
    } else if (maxv != nullptr && !(maxv->kind == EMBI_V_KEYWORD && maxv->kw == EMBI_KW_none)) {
        float v = embi_lay_len(c, b, maxv, cbw);
        if (!isnan(v)) {
            maxw = clamp0(border_box ? v - edges_h : v);
        }
    }
    const embi_value *mxx = embi_lay_size_expr(c, b, EMBI_PROP_max_width);
    if (mxx != nullptr && !isinf(maxw)) {
        float r = embi_lay_size_eval(c, b, mxx, border_box ? maxw + edges_h : maxw, cbw);
        if (!isnan(r)) {
            maxw = clamp0(border_box ? r - edges_h : r);
        }
    } else if (mxx != nullptr && isinf(maxw) && kw_of(maxv) == EMBI_KW_auto) {
        /* calc-size(auto, ...) on max-width: the calc-sum sees the width the box has before the limits as `size`. */
        float r = embi_lay_size_eval(c, b, mxx, border_box ? cw + edges_h : cw, cbw);
        if (!isnan(r)) {
            maxw = clamp0(border_box ? r - edges_h : r);
        }
    }
    if (auto_min > minw) {
        minw = auto_min < maxw ? auto_min : maxw;
    }
    /* The transferred limits apply to an auto width and to a content-sized one (block-aspect-ratio-021). */
    bool kw_width = wkw == EMBI_KW_min_content || wkw == EMBI_KW_max_content || wkw == EMBI_KW_fit_content;
    if (tmax_w < INFINITY && (width_was_auto || kw_width) && tmax_w < maxw) {
        maxw = tmax_w < minw ? minw : tmax_w; /* an explicit minimum wins over the transferred maximum */
    }
    if (tmin_w > minw && (width_was_auto || kw_width)) {
        minw = tmin_w < maxw ? tmin_w : maxw;
    }
    if (table && maxw < imn) {
        /* A table's max-width never narrows it below its min-content width (CSS Tables 3 section 3.5). */
        maxw = imn;
    }
    if (cw > maxw) {
        cw = maxw;
    }
    if (cw < minw) {
        cw = minw;
    }
    if (cw != fill_cw) {
        if (fill_ml_auto) {
            *ml = NAN;
        }
        if (fill_mr_auto) {
            *mr = NAN;
        }
    }
    return cw;
}

/* Puts the fragments of the tree rooted at f on the free list of the document. */
static void frag_release(embi_lay_doc *ld, embi_frag *f)
{
    if (ld->reuse_on && embi_hashmap_len(&ld->reusedmap) != 0) {
        embi_hashmap_remove(&ld->reusedmap, (uint64_t)(uintptr_t)f); /* the address is about to be recycled */
    }
    for (embi_frag *k = embi_frag_first(f); k != nullptr;) {
        embi_frag *nx = k->next;
        frag_release(ld, k);
        k = nx;
    }
    embi_fext *x = embi_frag_ext(f);
    if (x != nullptr) {
        /* The side record is linked through its sticky pointer while it is free. */
        x->sticky = (struct embi_sticky *)ld->free_ext;
        ld->free_ext = x;
    }
    if ((f->flags & EMBI_FF_TABLE) == 0 && frag_tier(f->box, f->kind) == EMBI_FSZ_FULL) {
        f->next = ld->free_frag;
        ld->free_frag = f;
    }
}

static void clear_boxes(embi_box *b)
{
    if (embi_box_is_text(b)) {
        return; /* the fields of a text run share the bytes of frag and first */
    }
    b->frag = nullptr;
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        clear_boxes(k);
    }
}

/* Clears the fragment of a box and of its descendants (before a second layout of the box). The box's
 * previous fragment tree goes to the free list when nothing attaches it (its root has no parent), so a
 * pass that is thrown away costs no new fragments. */
void embi_lay_clear_frags(embi_lctx *c, embi_box *b)
{
    embi_frag *of = b->frag;
    clear_boxes(b);
    if (of != nullptr && of->parent == nullptr && of->box == b) {
        frag_release(c->ld, of);
    }
}

/* True when the layout of b does not read a definite height of its containing block (see
 * layout_int.h). */
static bool hi_value_depends(const embi_value *v)
{
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_PERCENT || v->kind == EMBI_V_CALC || v->kind == EMBI_V_FUNC) {
        return true;
    }
    return v->kind == EMBI_V_KEYWORD && v->kw != EMBI_KW_auto && v->kw != EMBI_KW_none;
}

static bool hi_box_depends(const embi_lctx *c, const embi_box *k)
{
    if ((k->flags & (EMBI_BF_ABSPOS | EMBI_BF_CENTER | EMBI_BF_REPLACED | EMBI_BF_HAS_RATIO)) != 0 ||
        k->kind == EMBI_BK_REPLACED || k->tr != EMBI_TR_NONE || (k->flags2 & EMBI_BF2_CONTROL) != 0) {
        return true;
    }
    if (k->kind == EMBI_BK_TEXT || k->kind == EMBI_BK_BR || k->kind == EMBI_BK_MARKER) {
        return false;
    }
    if (hi_value_depends(embi_lay_prop(c, k, EMBI_PROP_height)) ||
        hi_value_depends(embi_lay_prop(c, k, EMBI_PROP_min_height)) ||
        hi_value_depends(embi_lay_prop(c, k, EMBI_PROP_max_height))) {
        return true;
    }
    const embi_value *ct = embi_lay_prop(c, k, EMBI_PROP_container_type);
    if (ct != nullptr && ct->kind == EMBI_V_KEYWORD && ct->kw != EMBI_KW_normal) {
        return true;
    }
    const embi_value *ar = embi_lay_prop(c, k, EMBI_PROP_aspect_ratio);
    return ar != nullptr && !(ar->kind == EMBI_V_KEYWORD && ar->kw == EMBI_KW_auto);
}

void embi_lay_release_frags(embi_lay_doc *ld, embi_frag *f)
{
    frag_release(ld, f);
}

bool embi_lay_height_independent(const embi_lctx *c, embi_box *b)
{
    if ((b->flags2 & EMBI_BF2_HIKNOWN) != 0) {
        return (b->flags2 & EMBI_BF2_HINDEP) != 0;
    }
    bool dep = b->kind != EMBI_BK_BLOCK || (b->flags & (EMBI_BF_SCROLL | EMBI_BF_FLOAT)) != 0;
    const embi_box *k = b;
    while (!dep && k != nullptr) {
        if (hi_box_depends(c, k)) {
            dep = true;
            break;
        }
        if (!embi_box_is_text(k) && k->first != nullptr) {
            k = k->first;
            continue;
        }
        while (k != b && k->next == nullptr) {
            k = k->parent;
        }
        if (k == b) {
            break;
        }
        k = k->next;
    }
    b->flags2 = (uint16_t)(b->flags2 | EMBI_BF2_HIKNOWN | (dep ? 0 : EMBI_BF2_HINDEP));
    return !dep;
}

/* One layout of a block with the scrollbar gutters sbv (inline end) and sbh (block end). ovx and
 * ovy are set when the contents overflow the padding box in that direction. */
static void block_pass(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out, float sbv,
                       float sbh, bool *ovx, bool *ovy)
{
    bool root = (b->flags & EMBI_BF_ROOT) != 0;
    bool bfc = (b->flags & EMBI_BF_BFC) != 0 || root;
    const embi_value *dir = embi_lay_prop(c, b, EMBI_PROP_direction);
    bool rtl = dir != nullptr && dir->kind == EMBI_V_KEYWORD && dir->kw == EMBI_KW_rtl;
    const embi_value *bsv = embi_lay_prop(c, b, EMBI_PROP_box_sizing);
    bool border_box = bsv != nullptr && bsv->kind == EMBI_V_KEYWORD && bsv->kw == EMBI_KW_border_box;

    embi_edges bdr, pad, mar;
    embi_lay_edges(c, b, cs->cb_w, &bdr, &pad, &mar);
    float edges_h = bdr.l + bdr.r + pad.l + pad.r;
    float edges_v = bdr.t + bdr.b + pad.t + pad.b;
    if (cs->mg_ovr) {
        mar.l = cs->ml_ovr;
        mar.r = cs->mr_ovr;
    }
    float ml = mar.l, mr = mar.r;
    float mt0 = isnan(mar.t) ? 0 : mar.t;
    float mb0 = isnan(mar.b) ? 0 : mar.b;

    /* Vertical sizing inputs (content box). */
    const embi_value *hv = embi_lay_prop(c, b, EMBI_PROP_height);
    /* calc-size() on height: a definite basis is applied here; an auto basis is applied to the content
     * height below (CSS Values 5 section 10.1). */
    const embi_value *hx = embi_lay_size_expr(c, b, EMBI_PROP_height);
    float hspec = NAN;
    if (!embi_lay_is_auto(hv) && kw_of(hv) == 0) {
        hspec = embi_lay_len(c, b, hv, cs->cb_h);
        if (!isnan(hspec)) {
            hspec = clamp0(border_box ? hspec - edges_v : hspec);
            if (hx != nullptr) {
                float r = embi_lay_size_eval(c, b, hx, border_box ? hspec + edges_v : hspec, cs->cb_h);
                if (!isnan(r)) {
                    hspec = clamp0(border_box ? r - edges_v : r);
                }
            }
        }
    } else if (kw_of(hv) == EMBI_KW_stretch && !isnan(cs->cb_h)) {
        /* height: stretch fills the containing block's height; the margins do not count (CSS Sizing 4 section 6.1,
         * csswg-drafts 11044; a table: CSS Tables 3 section 3.6). */
        hspec = clamp0(cs->cb_h - edges_v);
    }
    if (c->force_box == b && c->force_hset) {
        /* A flex item laid out at its used main or cross size (W3e): a definite height. */
        hspec = clamp0(c->force_h - edges_v);
    } else if (c->force_box == b && c->force_hnone) {
        /* A flex item measured for its content height: its own height does not count (W3e). */
        hspec = NAN;
    }
    float minh = 0;
    float maxh = INFINITY;
    const embi_value *minhv = embi_lay_prop(c, b, EMBI_PROP_min_height);
    bool min_auto = embi_lay_is_auto(minhv);
    /* min-content, max-content and fit-content as a minimum or maximum height mean the content height
     * (CSS Sizing 3 section 4: in the block axis they behave as auto). */
    uint16_t mnk = kw_of(minhv);
    bool min_kw_content = mnk == EMBI_KW_min_content || mnk == EMBI_KW_max_content || mnk == EMBI_KW_fit_content;
    if (!embi_lay_is_auto(minhv) && kw_of(minhv) == 0) {
        /* A percentage minimum against an indefinite height counts 0 (CSS 2.1 10.7), also inside calc(). */
        float v = embi_lay_len(c, b, minhv, isnan(cs->cb_h) ? 0.0f : cs->cb_h);
        if (!isnan(v)) {
            minh = clamp0(border_box ? v - edges_v : v);
        }
    }
    if (kw_of(minhv) == EMBI_KW_stretch && !isnan(cs->cb_h)) {
        minh = clamp0(cs->cb_h - edges_v);
    }
    {
        /* calc-size() on min-height: the basis (auto is 0 here) is the size the calc-sum sees. */
        const embi_value *mnhx = embi_lay_size_expr(c, b, EMBI_PROP_min_height);
        if (mnhx != nullptr) {
            float r = embi_lay_size_eval(c, b, mnhx, border_box ? minh + edges_v : minh,
                                         isnan(cs->cb_h) ? 0.0f : cs->cb_h);
            if (!isnan(r)) {
                minh = clamp0(border_box ? r - edges_v : r);
            }
        }
    }
    const embi_value *maxhv = embi_lay_prop(c, b, EMBI_PROP_max_height);
    uint16_t mxk = kw_of(maxhv);
    bool max_kw_content = mxk == EMBI_KW_min_content || mxk == EMBI_KW_max_content || mxk == EMBI_KW_fit_content;
    if (maxhv != nullptr && kw_of(maxhv) == 0) {
        float v = embi_lay_len(c, b, maxhv, cs->cb_h);
        if (!isnan(v)) {
            maxh = clamp0(border_box ? v - edges_v : v);
        }
    }
    if (kw_of(maxhv) == EMBI_KW_stretch && !isnan(cs->cb_h)) {
        maxh = clamp0(cs->cb_h - edges_v);
    }

    if (c->force_box == b && c->force_hnone && c->hraw_box == b) {
        /* The flex base size of a column item is its content height: its min-height and max-height clamp the
         * hypothetical size only (CSS Flexbox 1 section 9.2 step 3; table-as-item-min-height-1). */
        minh = 0;
        maxh = INFINITY;
    }
    if (b->tr == EMBI_TR_CELL) {
        /* max-height does not apply to table cells (CSS 2.1 section 17.5.3: undefined; browsers ignore it, WPT
         * css-tables/table-cell-overflow-explicit-height-002.html); min-height does. */
        maxh = INFINITY;
        max_kw_content = false;
    }

    const embi_value *mxhx = embi_lay_size_expr(c, b, EMBI_PROP_max_height);
    bool max_auto_calc = false; /* calc-size(auto, ...) on max-height: applied to the used height below */
    if (mxhx != nullptr) {
        if (!isinf(maxh)) {
            float r = embi_lay_size_eval(c, b, mxhx, border_box ? maxh + edges_v : maxh, cs->cb_h);
            if (!isnan(r)) {
                maxh = clamp0(border_box ? r - edges_v : r);
            }
        } else {
            max_auto_calc = kw_of(maxhv) == EMBI_KW_auto;
        }
    }
    /* The content height that min-height or max-height: min-content, max-content or fit-content stands
     * for is the height the box has with an auto height: a definite height does not give it, so the
     * box is measured once more with its own height ignored. */
    float auto_ch = NAN;
    if ((min_kw_content || max_kw_content) && !isnan(hspec) && !(c->force_box == b && c->force_hnone)) {
        embi_box *sbx = c->force_box;
        bool shn = c->force_hnone, shs = c->force_hset;
        c->force_box = b;
        c->force_hnone = true;
        c->force_hset = false;
        embi_lres tmp;
        memset(&tmp, 0, sizeof tmp);
        embi_lay_box(c, b, cs, &tmp);
        if (tmp.frag != nullptr) {
            auto_ch = clamp0(tmp.frag->h - edges_v);
        }
        embi_lay_clear_frags(c, b);
        c->force_box = sbx;
        c->force_hnone = shn;
        c->force_hset = shs;
        if (!isnan(auto_ch)) {
            if (min_kw_content && auto_ch > minh) {
                minh = auto_ch;
            }
            if (max_kw_content && auto_ch < maxh) {
                maxh = auto_ch;
            }
        }
    }

    float ratio = 0;
    bool has_ratio = embi_lay_ratio(c, b, &ratio);
    bool ratio_bb = border_box && !embi_lay_ratio_content(c, b);

    float cw_total = block_width(c, b, cs, edges_h, edges_v, &ml, &mr, border_box, hspec, has_ratio,
                                 ratio, ratio_bb, sbv, minh, maxh);
    /* Horizontal position: the margins resolve against the available width (10.3.3). */
    {
        float bw = cw_total + edges_h;
        float rest_l = ml, rest_r = mr;
        if (isnan(rest_l)) {
            rest_l = 0;
        }
        if (isnan(rest_r)) {
            rest_r = 0;
        }
        if (cs->fit == EMBI_FIT_SHRINK &&
            (!embi_lay_is_table(b) || (b->flags & (EMBI_BF_INLINE_LEVEL | EMBI_BF_FLOAT)) != 0)) {
            ml = rest_l;
            mr = rest_r;
        } else {
            embi_lay_hmargins(cs->avail_w, bw, &ml, &mr, embi_lay_cb_rtl(c, b));
        }
    }
    /* The scrollbar takes its room from the content box, so the content is narrower by sbv. */
    float cw = clamp0(cw_total - sbv);

    bool collapse_top = !bfc && bdr.t == 0 && pad.t == 0;
    /* A max-height never stops the last child's bottom margin from collapsing with the box's (margin-collapse-038), and
     * a min-height does not while it is below the content height (margin-collapse-025, -037); when it is in effect, the
     * margin is put back inside the box below. */
    bool mm_plain = !min_kw_content && !max_kw_content && !max_auto_calc && hx == nullptr;
    bool collapse_bot = !bfc && bdr.b == 0 && pad.b == 0 && isnan(hspec) && (minh == 0 || mm_plain) && !has_ratio;
    bool mm_opt = collapse_bot && minh != 0;
    /* A zero or auto height with no min-height, padding or border can collapse through (CSS 2.1
     * section 8.3.1). The bottom margin collapses with the last child only for height: auto. */
    bool self_ok = !bfc && bdr.t == 0 && bdr.b == 0 && pad.t == 0 && pad.b == 0 &&
                   (isnan(hspec) || hspec == 0) && minh == 0 && !has_ratio && hx == nullptr;
    float child_cb_h = NAN;
    if (!isnan(hspec)) {
        child_cb_h = hspec < minh ? minh : (hspec > maxh ? maxh : hspec);
        /* A horizontal scrollbar takes its room from the content box: percentages of the children
         * resolve against what is left. */
        child_cb_h = clamp0(child_cb_h - sbh);
        if (c->force_box == b && c->force_hset && c->force_hsoft) {
            child_cb_h = NAN;
        }
    }

    if (isnan(child_cb_h) && isnan(hspec) && has_ratio && ratio_h_definite(c, b) && !(c->force_box == b && c->force_hnone)) {
        /* The height that follows the width through the ratio is definite for the percentages of the children
         * (CSS Sizing 4 section 5.1; aspect-ratio/percentage-resolution-001). */
        float rh = ratio_bb ? (cw_total + edges_h) / ratio - edges_v : cw_total / ratio;
        child_cb_h = clamp0((rh < minh ? minh : (rh > maxh ? maxh : rh)) - sbh);
    }

    embi_frag *f = embi_lay_new_frag(c, b, EMBI_FK_BOX);
    embi_lay_frag_set_edges(c->ld, f, &bdr, &pad);
    f->w = cw_total + edges_h;
    if (b->tr == EMBI_TR_CELL && c->force_box == b && c->force_wset && c->force_w < f->w) {
        /* A table cell is as wide as its column even when that is less than its border and padding
         * (Chromium; WPT tentative/table-width-redistribution-fixed-padding.html). */
        f->w = clamp0(c->force_w);
    }
    embi_lay_frag_set_size(f, f->w, 0);
    /* The scrollable overflow starts at the padding box (CSS Overflow 3 section 2.2). */
    embi_fo(f)->ov[0] = bdr.l;
    embi_fo(f)->ov[1] = bdr.t;
    embi_fo(f)->ov[2] = f->w - bdr.r;
    embi_fo(f)->ov[3] = 0;
    if (b->flags & EMBI_BF_SCROLL) {
        f->flags |= EMBI_FF_SCROLL;
    }
    if (b->flags & EMBI_BF_CLIP) {
        f->flags |= EMBI_FF_CLIP;
    }
    if (sbv != 0 || sbh != 0) {
        embi_fext *x = embi_lay_frag_ext(c->ld, f);
        if (x != nullptr) {
            x->scr[0] = sbv;
            x->scr[1] = sbh;
        }
    }

    /* The inline start of the content: a vertical bar sits at the inline end, on the left in rtl. */
    float ox = bdr.l + pad.l + (rtl ? sbv : 0);
    float oy = bdr.t + pad.t;

    /* The frame of the BFC (W3b). A BFC root, or a box with no exclusion space to share, starts a
     * frame of its own at its border-box top-left. Any other box lies in its parent's frame, at the
     * BFC x of its containing block plus its left margin, and at the BFC y its parent estimated. */
    bool own_frame = bfc || cs->excl == nullptr;
    embi_excl own;
    embi_excl_init(&own);
    embi_excl *ex = own_frame ? &own : cs->excl;
    float fx = own_frame ? 0.0f : cs->bfc_x + ml;
    float fy = own_frame ? 0.0f : cs->bfc_y;
    float CX = fx + ox;
    float CY = fy + oy;

    /* A container-type box is a container for the cq* units of its descendants (CSS Containment 3
     * section 4). Its size is known now for the inline size, and for the block size when it is
     * definite. */
    const embi_value *ct = embi_lay_prop(c, b, EMBI_PROP_container_type);
    bool is_container = ct != nullptr && ct->kind == EMBI_V_KEYWORD &&
                        (ct->kw == EMBI_KW_size || ct->kw == EMBI_KW_inline_size);
    bool pushed = false;
    if (is_container && c->ncq < EMBI_CQ_DEPTH) {
        c->cq[c->ncq].w = cw;
        c->cq[c->ncq].h = ct->kw == EMBI_KW_size ? child_cb_h : NAN;
        c->ncq++;
        pushed = true;
    }

    /* A multi-column container lays its content out in one column of the column width, and cuts it into columns after (multicol.c). */
    embi_mc_in mci;
    bool mc = (b->tflags & EMBI_TF_MC) != 0 && b->kind == EMBI_BK_BLOCK;
    float cw_in = cw;
    if (mc) {
        memset(&mci, 0, sizeof mci);
        embi_lay_mc_geom(c, b, cw, &mci.g);
        cw_in = mci.g.colw;
    }
    float ch = 0;
    float bl1 = NAN, bl2 = NAN;
    bool flex_calc = false; /* the flex container applied calc-size() to its main size */
    bool empty = true;
    embi_strut top = {0, 0};
    embi_strut bot = {0, 0};
    embi_strut ttop = {0, 0};

    if (b->kind == EMBI_BK_FLEX || b->kind == EMBI_BK_GRID || embi_lay_is_table(b)) {
        /* The items of a flex container are laid out by flex.c (W3e), those of a grid container by
         * grid.c (W3f), the rows, cells and captions of a table by table.c (W3g). */
        const embi_value *fx_expr = isnan(hspec) ? hx : nullptr;
        embi_flex_in fin = {cw, child_cb_h, minh, maxh, ox, oy, fx_expr, border_box ? edges_v : 0.0f,
                            isnan(cs->cb_h) ? NAN : cs->cb_h};
        embi_flex_out fo = {0, NAN, NAN, false};
        if (b->kind == EMBI_BK_FLEX) {
            embi_flex_content(c, b, f, &fin, &fo);
        } else if (b->kind == EMBI_BK_GRID) {
            embi_grid_content(c, b, f, &fin, &fo);
        } else {
            embi_table_content(c, b, f, &fin, &fo);
        }
        ch = fo.ch;
        bl1 = fo.bl1;
        bl2 = fo.bl2;
        flex_calc = fo.main_calc;
        empty = false;
    } else if (b->flags & EMBI_BF_IFC) {
        /* The floats among the inline content are placed in the line they fall in (W3c). */
        embi_ifc_frame ifr = {ex, CX, CY, ox, oy, child_cb_h};
        float ilh = 0, ibl1 = NAN, ibl2 = NAN;
        /* The out-of-flow children of the inline content get the content start as their static position
         * first; the lines then record the position they fall at (position.c, inline.c). */
        embi_lay_abspos_ifc(c, b, rtl ? ox + cw : ox, oy);
        bool has = embi_lay_ifc(c, b, f, &ifr, cw_in, &ilh, &ibl1, &ibl2);
        /* The lines and what sticks out of them (an atomic inline wider than the box, nowrap text) are scrollable overflow. */
        for (embi_frag *lk = f->first; lk != nullptr; lk = lk->next) {
            if (lk->kind == EMBI_FK_LINE) {
                embi_lay_ov_add_child(f, lk);
            }
        }
        empty = !has;
        ch = has ? ilh : 0;
        if (has) {
            bl1 = oy + ibl1;
            bl2 = oy + ibl2;
        }
    } else {
        flow_st fl = flow_init(c, f, cw_in, child_cb_h, ox, oy, CX, CY, ex, collapse_top, collapse_bot);
        if (mc) {
            fl.span_w = cw;
        }
        block_flow(&fl, b);
        ch = fl.ch;
        bl1 = fl.bl1;
        bl2 = fl.bl2;
        empty = fl.empty;
        top = fl.top;
        bot = fl.bot;
        ttop = fl.ttop;
    }
    if (pushed) {
        c->ncq--;
    }
    if (mc && f->first != nullptr) {
        const embi_value *cfv = embi_lay_prop(c, b, EMBI_PROP_column_fill);
        mci.rtl = rtl;
        mci.fill_auto = cfv != nullptr && cfv->kind == EMBI_V_KEYWORD && cfv->kw == EMBI_KW_auto;
        mci.definite = !isnan(hspec);
        mci.ox = ox;
        mci.oy = oy;
        mci.ch = ch;
        mci.cap = mci.definite ? (hspec < minh ? minh : (hspec > maxh ? maxh : hspec)) : maxh;
        mci.bdr = bdr;
        ch = embi_lay_multicol(c, b, f, &mci);
        if (!isnan(bl1)) {
            bl2 = bl1;
        }
    }

    /* The padding-end edges are part of the scrollable overflow (in the inline and block axes). */
    float pe_x = ox + cw + pad.r;
    float pe_y = oy + ch + pad.b;
    if (pe_x > embi_fo(f)->ov[2]) {
        embi_fo(f)->ov[2] = pe_x;
    }
    if (pe_y > embi_fo(f)->ov[3]) {
        embi_fo(f)->ov[3] = pe_y;
    }

    bool selfc = empty && self_ok;
    float used = 0;
    if (selfc) {
        embi_strut s = {0, 0};
        embi_strut_add(&s, mt0);
        embi_strut_add(&s, mb0);
        embi_strut_merge(&s, top);
        embi_strut_merge(&s, bot);
        out->mt_s = s;
        out->mb_s = (embi_strut){0, 0};
        out->mt = embi_strut_val(&s);
        out->mb = 0;
        /* Where the box itself sits: its own top margin and its first child's. */
        embi_strut t = {0, 0};
        embi_strut_add(&t, mt0);
        embi_strut_merge(&t, ttop);
        out->top_s = t;
        out->selfc = true;
        f->h = edges_v;
        bl1 = NAN;
        bl2 = NAN;
    } else {
        used = isnan(hspec) ? (embi_lay_block_size_contained(c, b) ? 0.0f : ch) : hspec;
        if (!isnan(hspec) && embi_lay_is_table(b) && ch > used) {
            used = ch; /* a table's height is a minimum: its rows never overflow it (CSS 2.1 section 17.5.3) */
        }
        if (isnan(hspec) && has_ratio) {
            /* An auto height follows the width through the ratio (CSS Sizing 4 section 5). */
            used = ratio_bb ? (cw_total + edges_h) / ratio - edges_v : cw_total / ratio;
            /* min-height: auto is the content size for a ratio box that is not a scroll container
             * (CSS Sizing 4 section 5.1): taller content makes the box taller. */
            if (min_auto && (b->flags & EMBI_BF_SCROLL) == 0 && ch > used) {
                used = ch;
            }
        }
        if (bfc && isnan(hspec) && !has_ratio && !embi_lay_ssum_of(c, (b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr ? c->initial : b->style)->clamp) {
            /* The auto height of a BFC root contains its floats (CSS 2.1 section 10.6.7). A height
             * that comes from the aspect ratio is not an auto height of this kind. */
            float fb = embi_excl_bottom(ex) - CY;
            if (fb > used) {
                used = fb;
            }
        }
        used = clamp0(used);
        if (hx != nullptr && isnan(hspec) && !flex_calc) {
            /* calc-size(auto, ...): the calc-sum sees the content height as `size`. */
            float r = embi_lay_size_eval(c, b, hx, border_box ? used + edges_v : used, cs->cb_h);
            if (!isnan(r)) {
                used = clamp0(border_box ? r - edges_v : r);
            }
        }
        float hi_calc = INFINITY;
        if (max_auto_calc) {
            /* calc-size(auto, ...) on max-height: the calc-sum sees the height the box has before the limits. */
            float r = embi_lay_size_eval(c, b, mxhx, border_box ? used + edges_v : used, cs->cb_h);
            if (!isnan(r)) {
                hi_calc = clamp0(border_box ? r - edges_v : r);
            }
        }
        {
            float cc = isnan(auto_ch) ? ch : auto_ch;
            if (isnan(hspec) && has_ratio && !embi_lay_is_table(b)) {
                /* The min-content height of a ratio box is the height its ratio gives (CSS Sizing 4 section 5.1;
                 * aspect-ratio/intrinsic-size-010). */
                cc = clamp0(ratio_bb ? (cw_total + edges_h) / ratio - edges_v : cw_total / ratio);
            }
            float lo = min_kw_content && cc > minh ? cc : minh;
            float hi = max_kw_content && cc < maxh ? cc : maxh;
            if (embi_lay_is_table(b) && hi < ch) {
                hi = ch; /* a max-height does not shrink a table under its rows (CSS Tables 3 section 3.7) */
            }
            if (hi_calc < hi) {
                hi = hi_calc;
            }
            hi = hi < lo ? lo : hi; /* min-height wins over max-height */
            float used_pre = used;
            used = used < lo ? lo : (used > hi ? hi : used);
            if (mm_opt && used_pre < lo) {
                /* The limit is in effect: the box is as tall as the limit says, and the last child's bottom margin
                 * stays inside the content (it does not collapse with this box's). */
                float with_m = clamp0(used_pre + embi_strut_val(&bot));
                used = with_m < lo ? lo : (with_m > hi ? hi : with_m);
                collapse_bot = false;
            }
        }
        embi_strut ms = {0, 0};
        embi_strut_add(&ms, mt0);
        if (collapse_top) {
            embi_strut_merge(&ms, top);
        }
        out->mt_s = ms;
        out->mt = embi_strut_val(&ms);
        embi_strut bs = {0, 0};
        embi_strut_add(&bs, mb0);
        if (collapse_bot) {
            embi_strut_merge(&bs, bot);
        }
        out->mb_s = bs;
        out->mb = embi_strut_val(&bs);
        out->top_s = (embi_strut){0, 0};
        out->selfc = false;
        /* An auto height grows by the block-end gutter. A fixed height keeps its content box. */
        f->h = used + edges_v + (isnan(hspec) ? sbh : 0);
    }

    /* align-content on a block container moves its content in the block axis (CSS Box Alignment 3 section 6.2). */
    float ac_free = 0;
    if (!selfc && !(b->flags & EMBI_BF_CENTER) && used > ch && b->kind == EMBI_BK_BLOCK && !embi_lay_is_table(b)) {
        const embi_value *acv = embi_lay_prop(c, b, EMBI_PROP_align_content);
        bool in_fg = b->parent != nullptr && (b->parent->kind == EMBI_BK_FLEX || b->parent->kind == EMBI_BK_GRID);
        bool plain = !in_fg;
        if (plain && acv != nullptr && acv->kind == EMBI_V_KEYWORD) {
            if (acv->kw == EMBI_KW_center) {
                ac_free = (used - ch) / 2;
            } else if (acv->kw == EMBI_KW_end) {
                ac_free = used - ch;
            }
        }
    }
    /* Centered content (buttons): the content sits in the middle of a fixed height. */
    if (((b->flags & EMBI_BF_CENTER) && !selfc && !isnan(hspec)) || ac_free > 0) {
        float free = ac_free > 0 ? ac_free : (used - ch) / 2;
        if (free > 0) {
            for (embi_frag *k = f->first; k != nullptr; k = k->next) {
                k->y += free;
            }
            if (!isnan(bl1)) {
                bl1 += free;
            }
            if (!isnan(bl2)) {
                bl2 += free;
            }
            /* The overflow is recomputed from the moved children. */
            embi_fo(f)->ov[0] = bdr.l;
            embi_fo(f)->ov[1] = bdr.t;
            embi_fo(f)->ov[2] = f->w - bdr.r;
            embi_fo(f)->ov[3] = f->h - bdr.b;
            for (embi_frag *k = f->first; k != nullptr; k = k->next) {
                embi_lay_ov_add_child(f, k);
            }
        }
    }
    f->bl_first = bl1;
    embi_fl(f)->bl_last = bl2;

    /* The box's own margins (the fragment keeps them; the collapsed ones are in out). */
    embi_lay_frag_set_mg(c->ld, f, ml, mt0, mr, mb0);

    /* Overflow of the padding box in each direction, for overflow: auto. */
    *ovx = embi_fo(f)->ov[2] > f->w - bdr.r + 0.5f;
    *ovy = embi_fo(f)->ov[3] > f->h - bdr.b + 0.5f;

    {
        /* The block size limit of a line-clamp container's auto clamp point (linecl.c): content box sizes. */
        float lim = INFINITY;
        if (!isnan(hspec)) {
            lim = hspec;
        }
        if (maxh < lim) {
            lim = maxh;
        }
        if (minh > lim && !isinf(lim)) {
            lim = minh; /* the block size constraint is the larger of min-height and max-height */
        }
        c->cl_limit = lim;
    }
    embi_excl_free(&own);
    out->frag = f;
    out->ml = ml;
    out->mr = mr;
}

/* Lays out a block: the scrollbar gutters are reserved for scroll containers, and overflow: auto
 * is laid out again with a gutter on each axis that overflows. The box remembers the gutters its
 * last layout needed (EMBI_BF2_VGUT, HGUT), so when an ancestor lays it out again the first pass
 * already has the right gutters and nested overflow: auto boxes cost a pass each, not two. */
static void block_gutters(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    /* overflow-x and -y come from the style summary (an anonymous box has the initial style's); scrollbar-gutter only
     * matters on a scroll container (W3q: three style reads per block became one table probe). */
    const embi_lay_ssum *osm = embi_lay_ssum_of(c, (b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr ? c->initial : b->style);
    uint16_t ox = osm->ox, oy = osm->oy;
    bool stable = false;
    if ((b->flags & EMBI_BF_SCROLL) != 0) {
        const embi_value *sg = embi_lay_prop(c, b, EMBI_PROP_scrollbar_gutter);
        stable = sg != nullptr && sg->kind == EMBI_V_KEYWORD && sg->kw == EMBI_KW_stable;
    }
    float t = c->scrollbar > 0 ? c->scrollbar : 0;
    bool auto_x = (b->flags & EMBI_BF_SCROLL) != 0 && ox == EMBI_KW_auto;
    bool auto_y = (b->flags & EMBI_BF_SCROLL) != 0 && oy == EMBI_KW_auto;
    /* The gutters that are there whatever the content: overflow: scroll, and scrollbar-gutter:
     * stable on a scroll container (not on overflow: clip). */
    float base_v = (oy == EMBI_KW_scroll || (stable && (b->flags & EMBI_BF_SCROLL) != 0)) ? t : 0;
    float base_h = ox == EMBI_KW_scroll ? t : 0;
    bool gv = auto_y && (b->flags2 & EMBI_BF2_VGUT) != 0;
    bool gh = auto_x && (b->flags2 & EMBI_BF2_HGUT) != 0;
    float sbv = gv ? t : base_v;
    float sbh = gh ? t : base_h;
    bool ovx = false, ovy = false;
    embi_clamp cl0 = c->cl;
    block_pass(c, b, cs, out, sbv, sbh, &ovx, &ovy);
    if (auto_x || auto_y) {
        float sbv2 = auto_y && ovy ? t : base_v;
        float sbh2 = auto_x && ovx ? t : base_h;
        if (sbv2 != sbv || sbh2 != sbh) {
            embi_lay_clear_frags(c, b);
            c->cl = cl0;
            block_pass(c, b, cs, out, sbv2, sbh2, &ovx, &ovy);
            sbv = sbv2;
            sbh = sbh2;
        }
        b->flags2 = (uint16_t)((b->flags2 & ~(EMBI_BF2_VGUT | EMBI_BF2_HGUT)) |
                               (auto_y && sbv > base_v ? EMBI_BF2_VGUT : 0) |
                               (auto_x && sbh > base_h ? EMBI_BF2_HGUT : 0));
    }
}

/* A block container: a line-clamp container (continue: collapse or discard) is laid out once to find its clamp point
 * and, when that leaves lines out, again with only the lines before it (linecl.c). */
void embi_lay_block(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    const embi_style *st = (b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr ? c->initial : b->style;
    if (!embi_lay_ssum_of(c, st)->clamp) {
        block_gutters(c, b, cs, out);
        return;
    }
    embi_clamp saved = c->cl;
    memset(&c->cl, 0, sizeof c->cl);
    block_gutters(c, b, cs, out);
    embi_clamp plan;
    memset(&plan, 0, sizeof plan);
    if (out->frag != nullptr &&
        embi_lay_clamp_plan(c, b, out->frag, c->cl_limit, embi_frag_bdr(out->frag, 1) + embi_frag_pad(out->frag, 1), &plan)) {
        embi_lay_clear_frags(c, b);
        c->cl = plan;
        block_gutters(c, b, cs, out);
    }
    c->cl = saved;
}

/* Dispatch to the formatting context of the box (CSS Display 3 outer and inner display). */
static void lay_box_dispatch(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    switch (b->kind) {
    case EMBI_BK_REPLACED:
        embi_lay_replaced(c, b, cs, out);
        return;
    case EMBI_BK_FLEX:
        embi_lay_flex(c, b, cs, out);
        return;
    case EMBI_BK_GRID:
        embi_lay_grid(c, b, cs, out);
        return;
    case EMBI_BK_TABLE:
        embi_lay_table(c, b, cs, out);
        return;
    default:
        embi_lay_block(c, b, cs, out);
        return;
    }
}

void embi_lay_box(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out)
{
    /* The intrinsic sizes this layout asks for have their own percentage basis (embi_lctx.ih_on). */
    embi_ih ih = embi_lay_ih_push(c, NAN);
    /* W6b: a plain block laid out by block flow on a chain of such boxes is recorded for the partial relayout (incr.c).
     * CF1: so is a flex or grid container there, whose children are not (the chain ends at it). */
    bool via = c->via_flow;
    c->via_flow = false;
    bool saved = c->chain_ok;
    bool rec = saved && via && embi_lay_incr_chain_ok(c, b, cs);
    c->chain_ok = rec && b->kind == EMBI_BK_BLOCK;
    uint32_t n0 = cs->excl != nullptr ? cs->excl->rects.len : 0;
    uint32_t pos0 = rec ? embi_lay_pos_count(c) : 0;
    if (c->ld->reuse_on && !embi_box_is_text(b)) {
        embi_lay_incr_stale(c->ld, b); /* laid out again: what the last layout left in it is stale */
    }
    /* A box that establishes its own formatting context starts no line count of the enclosing line clamp (linecl.c). */
    bool iso = c->cl.on && ((b->flags & EMBI_BF_BFC) != 0 || b->kind != EMBI_BK_BLOCK || embi_lay_clamp_indep(b));
    embi_clamp cl_saved = c->cl;
    if (iso) {
        c->cl.on = false;
    }
    lay_box_dispatch(c, b, cs, out);
    if (iso) {
        c->cl = cl_saved;
    }
    c->chain_ok = saved;
    if (rec && out->frag != nullptr) {
        embi_lay_incr_record(c, b, cs, out, n0, pos0);
    } else if (saved) {
        embi_lay_incr_forget(c->ld, b);
    }
    embi_lay_ih_pop(c, ih);
}
