/*
 * Hit testing (W5d2): the element under a point, found in the order src/paint emits
 * (CSS 2.1 Appendix E, see paint.c). The topmost box wins, so the walk mirrors the paint walk in reverse:
 *
 *   paint of a stacking context S:  self, negative z, block, float, inline, positioned units, positive z
 *   hit of S:                       positive z, positioned units, inline, float, block, negative z, self
 *   paint of a positioned unit U:   self, block, float, inline, then U's own units
 *   hit of a positioned unit U:     U's own units, inline, float, block, self
 *
 * The positioned units of a scope are the positioned boxes, the z-index 0 contexts and the collapsed tables reached from
 * the scope through its flow boxes; they are visited in document order, reversed. Stacking contexts with a z-index other
 * than 0 belong to the nearest stacking context around them, whatever positioned boxes lie between (paint's collect).
 * A box's clip limits what its contents can hit: the padding box for overflow other than visible and for contain: paint
 * (tables clip between their captions), and the legacy rect() clip of an abspos box, which bounds the box itself too.
 * pointer-events: none removes a box as a target and not its children, and visibility: hidden or collapse does the same.
 *
 * The point is in viewport coordinates. The root's scroll offset is applied by content_of, as paint applies it to the
 * children of the root, so the viewport point is compared with what is painted.
 *
 * embi_layout_hit_test_all (collect mode) runs the same walk without stopping at the first hit: every walk that stops at its
 * first hit in single mode continues, each hit is recorded, and the children of a scope are visited in paint-reverse order
 * so the record is topmost first (flow children are reversed block by block, a 3D context sorts its planes by depth).
 * Single mode is unchanged: it only costs a flag test per scope.
 *
 * The flex and grid order property is applied as paint applies it (children and the document-order keys of units).
 * Transforms (R3c): a stacking context with a transform, or a perspective from its parent, tests the point after the
 * inverse of the projected map of its plane (the homography of layout/xform.h, with the W > 0 side only); a box that
 * hides its back face is skipped when it faces away. A 3D rendering context (a preserve-3d box and the stacking
 * contexts that are its children, recursively) tests every plane with the point of the screen and takes the hit whose
 * plane is nearest the viewer at that point (the largest z / w), later planes winning a tie.
 * Known gaps against paint: rounded corners and overflow-clip-margin do not shape the clip, and the legacy clip takes
 * px lengths only. The gather walk skips a subtree whose scrollable overflow rectangle (and clip) lacks the point; the
 * flow walks prune through the clips only.
 */
#include "layout_int.h"

#include "css/props.h"
#include "layout/clip_path.h"
#include "layout/xform.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The phases of the flow walk (same names and order as paint.c). */
enum {
    HPH_BLOCK = 0,
    HPH_FLOAT,
    HPH_INLINE,
};

typedef struct hit_rect {
    float x0, y0, x1, y1;
} hit_rect;

/* One positioned unit, stacking context or z-ordered context of a scope. */
typedef struct hit_ent {
    const embi_frag *f;
    const embi_node *node;   /* the DOM node that orders it (nullptr when it has none) */
    float x, y;              /* the border box origin in page px */
    hit_rect clip;           /* the clip in effect where it sits */
    int32_t z;
    uint32_t seq;
    uint64_t *path;          /* (order, sibling index) at each level from the root down to node: its document-order key */
    uint32_t plen;           /* in words */
} hit_ent;

typedef struct hit_vec {
    hit_ent *d;
    uint32_t n, cap;
} hit_vec;

/* A box with a perspective while its content is tested: the matrix it gives its children. */
typedef struct hit_pers {
    const embi_element *el;
    embi_m4 m;
} hit_pers;

/* A plane found in a 3D rendering context, to be tested after the plane that holds it. */
typedef struct hit_part {
    const embi_frag *f;
    float kx, ky;
    embi_m4 rel;
} hit_part;

#define HIT_MAX_PERS 32

typedef struct hit_dup {
    const embi_element *el;
    uint32_t pos;
} hit_dup;

/* A 3D rendering context being tested. */
typedef struct hit_c3 {
    struct hit_c3 *prev;
    float sx, sy;               /* the point, in the space the context is composed into */
    const embi_element *cur_el; /* the element of the plane being tested, and whether it preserves 3D */
    bool cur_pres;
    bool discover;              /* the pass that only finds the planes of cur_frag */
    hit_part *parts;
    uint32_t np, cap;
    const embi_element *best;
    double best_z;
} hit_c3;

/* An element hit in collect mode: z and plane are the depth and the rank (in test order, 1 up) of the 3D plane it was hit in
 * (0: not in a 3D context); seq is its array position, set just before a 3D sort. */
typedef struct hit_el {
    const embi_element *el;
    double z;
    uint32_t plane;
    uint32_t seq;
} hit_el;

typedef struct hit_out {
    hit_el *d;
    uint32_t n, cap;
} hit_out;

/* A child of a scope in collect mode: the child and the first output entry of its block. */
typedef struct hit_seg {
    const embi_frag *f;
    uint32_t mark;
} hit_seg;

typedef struct hit_st {
    float px, py;
    const embi_frag *root;
    float vw, vh;               /* the viewport, for viewport units in transforms */
    float rem;                  /* the root font size, for rem units in transforms */
    bool xf;                    /* some box of the document is transformed: scrollable overflow does not bound it */
    bool nocull;                /* discovery pass: every test passes and nothing is hit */
    uint32_t npers;
    hit_pers pers[HIT_MAX_PERS];
    hit_c3 *c3;
    const embi_layout_engine *eng; /* R4c: for the SVG hit hook */
    bool collect;               /* every hit is recorded in out, and no walk stops at its first hit */
    uint32_t planes;            /* collect mode: the planes tested so far (their ranks) */
    hit_out out;
} hit_st;

/* Whether a walk that found best (nullptr: nothing yet) still has to look at the rest: collect mode looks at all. */
#define HIT_OPEN(h, best) ((h)->collect || (best) == nullptr)

static const hit_rect k_all = {-1.0e30f, -1.0e30f, 1.0e30f, 1.0e30f};

static uint16_t style_kw(const embi_style *s, embi_prop_id prop)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, prop) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

static bool inside(const hit_st *h, const hit_rect *r)
{
    return h->nocull || (h->px >= r->x0 && h->px < r->x1 && h->py >= r->y0 && h->py < r->y1);
}

static hit_rect rect_meet(hit_rect a, hit_rect b)
{
    hit_rect r = {fmaxf(a.x0, b.x0), fmaxf(a.y0, b.y0), fminf(a.x1, b.x1), fminf(a.y1, b.y1)};
    return r;
}

/* A stacking context of the tree. The html element is not one when it is a child of the root: paint treats the root
 * and html as one context. */
static bool is_stacking(const hit_st *h, const embi_frag *f)
{
    if (f->kind != EMBI_FK_BOX || f->box == nullptr || (f->flags & EMBI_FF_STACKING) == 0) {
        return false;
    }
    if (f->parent == h->root && f->box->el != nullptr && f->box->el->base.name == EMBI_ATOM_html) {
        return false;
    }
    return true;
}

/* A positioned box (position other than static): a unit of its scope, painted outside the flow phases. */
static bool positioned(const embi_frag *f)
{
    if (f->kind != EMBI_FK_BOX || f->box == nullptr) {
        return false;
    }
    uint16_t kw = style_kw(f->box->style, EMBI_PROP_position);
    return kw != 0 && kw != EMBI_KW_static;
}

/* The border box origin of child k of a scope whose content origin is (ox, oy), with its sticky offset. */
static void origin_of(const embi_frag *k, float ox, float oy, float *kx, float *ky)
{
    float dx = 0.0f, dy = 0.0f;
    embi_lay_sticky_offset(k, &dx, &dy);
    *kx = ox + k->x + dx;
    *ky = oy + k->y + dy;
}

/* contain: paint (or strict or content) on a value or a list, as paint.c reads it. */
static bool paint_kw(uint16_t kw)
{
    return kw == EMBI_KW_paint || kw == EMBI_KW_strict || kw == EMBI_KW_content;
}

static bool contains_paint(const embi_style *s)
{
    const embi_value *cv = s != nullptr ? embi_style_value(s, EMBI_PROP_contain) : nullptr;
    if (cv == nullptr) {
        return false;
    }
    if (cv->kind == EMBI_V_KEYWORD) {
        return paint_kw(cv->kw);
    }
    if (cv->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < cv->list.n; i++) {
            const embi_value *item = &cv->list.items[i];
            if (item->kind == EMBI_V_KEYWORD && paint_kw(item->kw)) {
                return true;
            }
        }
    }
    return false;
}

/* The content origin of box k (its border box at kx, ky, moved by its scroll offset) and the clip of its content: the
 * padding box when it clips (overflow other than visible, or contain: paint), intersected with the clip it sits in. A table
 * clips at its table box, which lies between its captions (paint.c enter). Lines and text have no scroll and no clip of
 * their own. Rounded corners and overflow-clip-margin are not applied to the hit clip. */
static void content_of(const embi_frag *k, float kx, float ky, hit_rect clip, float *cx, float *cy, hit_rect *cclip)
{
    *cx = kx;
    *cy = ky;
    *cclip = clip;
    if (k->kind != EMBI_FK_BOX) {
        return;
    }
    *cx -= embi_frag_sx(k);
    *cy -= embi_frag_sy(k);
    if ((k->flags & EMBI_FF_CLIP) != 0 || (k->box != nullptr && contains_paint(k->box->style))) {
        float cap_t = 0.0f, cap_b = 0.0f;
        embi_frag_caps(k, &cap_t, &cap_b);
        float by = ky + cap_t;
        float bh = fmaxf(0.0f, k->h - cap_t - cap_b);
        hit_rect pb = {kx + embi_frag_bdr(k, 0), by + embi_frag_bdr(k, 1), kx + k->w - embi_frag_bdr(k, 2),
                       by + bh - embi_frag_bdr(k, 3)};
        *cclip = rect_meet(clip, pb);
    }
}

/* The clip of an absolutely positioned box with the legacy clip: rect(top, right, bottom, left) in its border box, where
 * auto is the edge (CSS 2.1 11.1.2). Only px lengths are read; relative units and percentages count as auto here (paint.c
 * resolves them). Returns clip when the box has none. */
static hit_rect abs_clip_rect(const embi_frag *k, float kx, float ky, hit_rect clip)
{
    if (k->kind != EMBI_FK_BOX || k->box == nullptr || (k->box->flags & EMBI_BF_ABSPOS) == 0) {
        return clip;
    }
    const embi_value *v = k->box->style != nullptr ? embi_style_value(k->box->style, EMBI_PROP_clip) : nullptr;
    if (v == nullptr || v->kind != EMBI_V_LIST || v->list.n != 4) {
        return clip;
    }
    float side[4] = {0.0f, k->w, k->h, 0.0f}; /* top, right, bottom, left */
    for (int i = 0; i < 4; i++) {
        const embi_value *it = &v->list.items[i];
        if (it->kind == EMBI_V_LENGTH && it->unit == EMBI_U_PX) {
            side[i] = (float)it->num;
        }
    }
    float w = side[1] - side[3], h = side[2] - side[0];
    hit_rect r = {kx + side[3], ky + side[0], kx + side[3] + (w > 0.0f ? w : 0.0f), ky + side[0] + (h > 0.0f ? h : 0.0f)};
    return rect_meet(clip, r);
}

/* The DOM node that orders a fragment: its own element, else that of the nearest ancestor fragment with one. */
static const embi_node *node_of(const embi_frag *f)
{
    for (const embi_frag *g = f; g != nullptr; g = g->parent) {
        if (g->kind == EMBI_FK_BOX && g->box != nullptr && g->box->el != nullptr) {
            return &g->box->el->base;
        }
    }
    return nullptr;
}

/* The flex or grid order property of a box, as paint.c reads it. */
static int64_t order_val(const embi_style *st)
{
    const embi_value *v = st != nullptr ? embi_style_value(st, EMBI_PROP_order) : nullptr;
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

static bool is_ordered(const embi_frag *f)
{
    return f->kind == EMBI_FK_BOX && f->box != nullptr && (f->box->kind == EMBI_BK_FLEX || f->box->kind == EMBI_BK_GRID);
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

typedef struct hit_kid {
    int64_t order;
    uint32_t seq;
    const embi_frag *f;
} hit_kid;

static int cmp_kid(const void *a, const void *b)
{
    const hit_kid *x = a, *y = b;
    if (x->order != y->order) {
        return x->order < y->order ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* The children of a flex or grid container in paint order (order, then tree order), or nullptr when f is not one (or has
 * none): the caller then walks the fragment list. Free with kids_free. */
static hit_kid *kids_of(const embi_frag *f, uint32_t *n)
{
    *n = 0;
    if (!is_ordered(f)) {
        return nullptr;
    }
    uint32_t cnt = 0;
    bool any_order = false;
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        cnt++;
        if (k->kind == EMBI_FK_BOX && k->box != nullptr && order_val(k->box->style) != 0) {
            any_order = true;
        }
    }
    if (cnt == 0 || !any_order) {
        return nullptr;
    }
    hit_kid *ks = embi_alloc(cnt * sizeof *ks);
    uint32_t i = 0;
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next, i++) {
        ks[i].order = (k->kind == EMBI_FK_BOX && k->box != nullptr) ? order_val(k->box->style) : 0;
        ks[i].seq = i;
        ks[i].f = k;
    }
    if (cnt > 1) {
        qsort(ks, cnt, sizeof *ks, cmp_kid);
    }
    *n = cnt;
    return ks;
}

static void kids_free(hit_kid *ks, uint32_t n)
{
    if (ks != nullptr) {
        embi_free(ks, n * sizeof *ks);
    }
}

/* The document-order key of a node: a pair (order, sibling index + 1) per level from the root down, where the order is
 * the child's order property when the parent is a flex or grid container and 0 otherwise (as paint.c's build_key).
 * Computed once per entry before a sort, so the comparator walks no sibling lists. */
static void ent_path(hit_ent *e)
{
    e->path = nullptr;
    e->plen = 0;
    if (e->node == nullptr) {
        return;
    }
    uint32_t depth = 0;
    for (const embi_node *p = e->node; p->parent != nullptr; p = p->parent) {
        depth++;
    }
    if (depth == 0) {
        return;
    }
    uint64_t *path = embi_alloc(2u * depth * sizeof *path);
    const embi_node *p = e->node;
    for (uint32_t i = depth; i > 0; i--) {
        uint32_t idx = 0;
        for (const embi_node *c = p->parent->first_child; c != nullptr && c != p; c = c->next) {
            idx++;
        }
        int64_t o = 0;
        if (node_ordered(p->parent) && p->type == EMBI_NODE_ELEMENT) {
            const embi_box *pb = ((const embi_element *)p)->box;
            o = order_val(pb != nullptr ? pb->style : nullptr);
        }
        path[2 * (i - 1)] = (uint64_t)(o + 4000000000000000LL);
        path[2 * (i - 1) + 1] = (uint64_t)idx + 1u;
        p = p->parent;
    }
    e->path = path;
    e->plen = 2u * depth;
}

static void ents_prepare(hit_vec *v)
{
    for (uint32_t i = 0; i < v->n; i++) {
        ent_path(&v->d[i]);
    }
}

static void ents_unprepare(hit_vec *v)
{
    for (uint32_t i = 0; i < v->n; i++) {
        if (v->d[i].path != nullptr) {
            embi_free(v->d[i].path, v->d[i].plen * sizeof(uint64_t));
            v->d[i].path = nullptr;
            v->d[i].plen = 0;
        }
    }
}

/* Document order of two entries by their keys (an ancestor comes before its descendants), then by gather order. */
static int dom_order(const hit_ent *x, const hit_ent *y)
{
    if (x->node != nullptr && y->node != nullptr && x->node != y->node) {
        uint32_t n = x->plen < y->plen ? x->plen : y->plen;
        for (uint32_t i = 0; i < n; i++) {
            if (x->path[i] != y->path[i]) {
                return x->path[i] < y->path[i] ? -1 : 1;
            }
        }
        if (x->plen != y->plen) {
            return x->plen < y->plen ? -1 : 1;
        }
    }
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* Paint order of the z-ordered contexts of a scope: by z-index, then document order. */
static int cmp_z(const void *a, const void *b)
{
    const hit_ent *x = a, *y = b;
    if (x->z != y->z) {
        return x->z < y->z ? -1 : 1;
    }
    return dom_order(x, y);
}

static int cmp_dom(const void *a, const void *b)
{
    return dom_order(a, b);
}

static void hv_push(hit_vec *v, const embi_frag *f, float x, float y, hit_rect clip, int32_t z)
{
    if (v->n == v->cap) {
        uint32_t ncap = v->cap != 0 ? v->cap * 2u : 8u;
        if (v->d == nullptr) {
            v->d = embi_alloc(ncap * sizeof(hit_ent));
        } else {
            v->d = embi_realloc(v->d, v->cap * sizeof(hit_ent), ncap * sizeof(hit_ent));
        }
        v->cap = ncap;
    }
    hit_ent *e = &v->d[v->n];
    e->f = f;
    e->node = node_of(f);
    e->x = x;
    e->y = y;
    e->clip = clip;
    e->z = z;
    e->path = nullptr;
    e->plen = 0;
    e->seq = v->n;
    v->n++;
}

static void hv_free(hit_vec *v)
{
    if (v->d != nullptr) {
        embi_free(v->d, v->cap * sizeof(hit_ent));
    }
    v->d = nullptr;
    v->n = 0;
    v->cap = 0;
}

/* Collect mode (embi_layout_hit_test_all): every element hit is appended to h->out in the order the walk meets it. The walk
 * then visits the children of a scope in paint-reverse order by putting the blocks of each child in reverse (seg_reverse),
 * and a 3D context orders its planes by depth (out_sort_3d). Single mode never touches out. */
static void out_add(hit_st *h, const embi_element *el)
{
    if (!h->collect) {
        return;
    }
    if (h->out.n == h->out.cap) {
        uint32_t ncap = h->out.cap != 0 ? h->out.cap * 2u : 16u;
        if (h->out.d == nullptr) {
            h->out.d = embi_alloc(ncap * sizeof *h->out.d);
        } else {
            h->out.d = embi_realloc(h->out.d, h->out.cap * sizeof *h->out.d, ncap * sizeof *h->out.d);
        }
        h->out.cap = ncap;
    }
    h->out.d[h->out.n].el = el;
    h->out.d[h->out.n].z = 0.0;
    h->out.d[h->out.n].plane = 0;
    h->out.d[h->out.n].seq = 0;
    h->out.n++;
}

/* Gives the entries from index `from` that no inner plane has tagged yet the depth z and the rank of the plane that holds
 * them. An inner plane tags its own entries first (it is tested inside its parent), so its tags stay. */
static void out_tag(hit_st *h, uint32_t from, double z, uint32_t plane)
{
    if (!(z == z)) {
        z = 0.0; /* NaN: keep the sort a strict order */
    }
    for (uint32_t i = from; i < h->out.n; i++) {
        if (h->out.d[i].plane == 0) {
            h->out.d[i].z = z;
            h->out.d[i].plane = plane;
        }
    }
}

/* Nearest plane first; within a plane the later-tested plane first on a tie (as single mode's z >= best_z); within one
 * plane, the array position, which is the walk's topmost-first order. */
static int cmp_out_z(const void *a, const void *b)
{
    const hit_el *x = a, *y = b;
    if (x->z != y->z) {
        return x->z > y->z ? -1 : 1;
    }
    if (x->plane != y->plane) {
        return x->plane > y->plane ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* Sorts the entries from index `from` (the hits of a 3D rendering context) by plane depth, keeping each plane's walk order. */
static void out_sort_3d(hit_st *h, uint32_t from)
{
    if (h->out.n - from > 1) {
        for (uint32_t i = from; i < h->out.n; i++) {
            h->out.d[i].seq = i;
        }
        qsort(h->out.d + from, h->out.n - from, sizeof *h->out.d, cmp_out_z);
    }
}

/* The children of a scope in paint order (kids_of's order for a flex or grid container, else the fragment list), each as a
 * segment of the output: mark is set when the child is visited, the entry after the last one is segs[n].mark. */
static hit_seg *segs_of(const embi_frag *f, uint32_t *n)
{
    uint32_t kn = 0;
    hit_kid *ks = kids_of(f, &kn);
    uint32_t cnt = kn;
    if (ks == nullptr) {
        cnt = 0;
        for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
            cnt++;
        }
    }
    hit_seg *s = embi_alloc((cnt + 1u) * sizeof *s);
    uint32_t i = 0;
    if (ks != nullptr) {
        for (; i < kn; i++) {
            s[i].f = ks[i].f;
        }
        kids_free(ks, kn);
    } else {
        for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
            s[i++].f = k;
        }
    }
    *n = cnt;
    return s;
}

/* Puts the blocks [s[i].mark, s[i+1].mark) of the output in reverse block order (each block keeps its entries in order):
 * the children are visited in paint order, and the last painted one is the topmost. */
static void seg_reverse(hit_st *h, const hit_seg *s, uint32_t n)
{
    if (n < 2) {
        return;
    }
    uint32_t lo = s[0].mark, hi = s[n].mark;
    if (hi - lo < 2) {
        return;
    }
    size_t bytes = (size_t)(hi - lo) * sizeof *h->out.d;
    hit_el *tmp = embi_alloc(bytes);
    uint32_t pos = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint32_t len = s[i + 1].mark - s[i].mark;
        if (len != 0) {
            memcpy(tmp + pos, h->out.d + s[i].mark, (size_t)len * sizeof *tmp);
        }
        pos += len;
    }
    memcpy(h->out.d + lo, tmp, bytes);
    embi_free(tmp, bytes);
}

/* The element of a box itself (its own background, border or content area), when it can be a target: the point is in
 * its border box and clip, and it has pointer-events other than none and is visible. Children are not looked at. */
/* Set at every document creation, possibly from several threads: stored atomically. */
static _Atomic(embi_layout_svg_hit_fn) g_svg_hit_fn;

void embi_layout_set_svg_hit(embi_layout_svg_hit_fn fn)
{
    atomic_store_explicit(&g_svg_hit_fn, fn, memory_order_release);
}

static const embi_element *self_hit(hit_st *h, const embi_frag *k, float kx, float ky, const hit_rect *clip)
{
    if (h->nocull || k->kind != EMBI_FK_BOX || k->box == nullptr || k->box->el == nullptr || !inside(h, clip)) {
        return nullptr;
    }
    if (h->px < kx || h->px >= kx + k->w || h->py < ky || h->py >= ky + k->h) {
        return nullptr;
    }
    const embi_style *s = k->box->style;
    if (style_kw(s, EMBI_PROP_pointer_events) == EMBI_KW_none) {
        return nullptr;
    }
    uint16_t vis = style_kw(s, EMBI_PROP_visibility);
    if (vis == EMBI_KW_hidden || vis == EMBI_KW_collapse) {
        return nullptr;
    }
    embi_layout_svg_hit_fn svg_hit = atomic_load_explicit(&g_svg_hit_fn, memory_order_acquire);
    if (svg_hit != nullptr && k->box->el->base.extra == EMBI_NS_SVG && k->box->kind == EMBI_BK_REPLACED) {
        /* an svg element's replaced box: the shape under the point, else the svg element itself */
        float bl = embi_frag_bdr(k, 0), bt = embi_frag_bdr(k, 1), br = embi_frag_bdr(k, 2), bb = embi_frag_bdr(k, 3);
        float pl = embi_frag_pad(k, 0), pt = embi_frag_pad(k, 1), pr = embi_frag_pad(k, 2), pbo = embi_frag_pad(k, 3);
        float box[4] = {bl + pl, bt + pt, k->w - bl - br - pl - pr, k->h - bt - bb - pt - pbo};
        if (box[2] > 0.0f && box[3] > 0.0f) {
            const embi_element *r = svg_hit(h->eng, k->box->el, box, h->px - kx - box[0], h->py - ky - box[1]);
            if (r != nullptr) {
                /* the shape, then the svg element around it (painted below the shape) */
                out_add(h, r);
                out_add(h, k->box->el);
                return r;
            }
        }
    }
    out_add(h, k->box->el);
    return k->box->el;
}

static const embi_element *hit_ctx(hit_st *h, const embi_frag *f, float kx, float ky, hit_rect clip);
static const embi_element *hit_unit(hit_st *h, const embi_frag *k, float kx, float ky, hit_rect clip);
static const embi_element *hit_flow(hit_st *h, const embi_frag *f, float cx, float cy, hit_rect cclip, int ph);
static const embi_element *hit_pseudo(hit_st *h, const embi_frag *k, float kx, float ky, hit_rect clip);

/* The children of a line in the inline phase: inline box pieces hit as their own box, atomic inlines as pseudo stacking
 * contexts. Text has no element of its own. Later children are on top. */
static const embi_element *line_child(hit_st *h, const embi_frag *c, float lx, float ly, hit_rect clip)
{
    if (is_stacking(h, c) || c->kind != EMBI_FK_BOX || c->box == nullptr || positioned(c)) {
        return nullptr;
    }
    float kx, ky;
    origin_of(c, lx, ly, &kx, &ky);
    return c->inl ? self_hit(h, c, kx, ky, &clip) : hit_pseudo(h, c, kx, ky, clip);
}

static const embi_element *hit_line(hit_st *h, const embi_frag *line, float lx, float ly, hit_rect clip)
{
    if (!inside(h, &clip)) {
        return nullptr;
    }
    if (h->collect) {
        uint32_t n = 0;
        hit_seg *s = segs_of(line, &n);
        uint32_t start = h->out.n;
        for (uint32_t i = 0; i < n; i++) {
            s[i].mark = h->out.n;
            (void)line_child(h, s[i].f, lx, ly, clip);
        }
        s[n].mark = h->out.n;
        seg_reverse(h, s, n);
        embi_free(s, (n + 1u) * sizeof *s);
        return h->out.n > start ? h->out.d[start].el : nullptr;
    }
    const embi_element *best = nullptr;
    for (const embi_frag *c = embi_frag_first(line); c != nullptr; c = c->next) {
        const embi_element *r = line_child(h, c, lx, ly, clip);
        if (r != nullptr) {
            best = r;
        }
    }
    return best;
}

/* One child of a scope in one phase of the flow walk (paint.c walk_child, reversed). */
static const embi_element *flow_child(hit_st *h, const embi_frag *k, float cx, float cy, hit_rect cclip, int ph)
{
    if (is_stacking(h, k) || k->kind == EMBI_FK_TEXT) {
        return nullptr;
    }
    float kx, ky;
    origin_of(k, cx, cy, &kx, &ky);
    if (k->kind == EMBI_FK_LINE) {
        return ph == HPH_INLINE ? hit_line(h, k, kx, ky, cclip) : nullptr;
    }
    if (k->kind != EMBI_FK_BOX || k->box == nullptr || positioned(k)) {
        return nullptr;
    }
    bool flt = (k->box->flags & EMBI_BF_FLOAT) != 0;
    float ccx, ccy;
    hit_rect kc;
    switch (ph) {
    case HPH_BLOCK: {
        if (flt) {
            return nullptr;
        }
        content_of(k, kx, ky, cclip, &ccx, &ccy, &kc);
        const embi_element *r = hit_flow(h, k, ccx, ccy, kc, HPH_BLOCK);
        if (HIT_OPEN(h, r)) {
            const embi_element *s = self_hit(h, k, kx, ky, &cclip);
            r = r != nullptr ? r : s;
        }
        return r;
    }
    case HPH_FLOAT:
        if (flt) {
            return hit_pseudo(h, k, kx, ky, cclip);
        }
        content_of(k, kx, ky, cclip, &ccx, &ccy, &kc);
        return hit_flow(h, k, ccx, ccy, kc, HPH_FLOAT);
    case HPH_INLINE:
        if (flt) {
            return nullptr;
        }
        content_of(k, kx, ky, cclip, &ccx, &ccy, &kc);
        return hit_flow(h, k, ccx, ccy, kc, HPH_INLINE);
    default:
        return nullptr;
    }
}

/* Collect mode of hit_flow: the children in paint-reverse order (the last painted first), each child's hits kept together.
 * Returns the topmost hit, as single mode does. */
static const embi_element *hit_flow_all(hit_st *h, const embi_frag *f, float cx, float cy, hit_rect cclip, int ph)
{
    uint32_t n = 0;
    hit_seg *s = segs_of(f, &n);
    uint32_t start = h->out.n;
    for (uint32_t i = 0; i < n; i++) {
        s[i].mark = h->out.n;
        (void)flow_child(h, s[i].f, cx, cy, cclip, ph);
    }
    s[n].mark = h->out.n;
    seg_reverse(h, s, n);
    embi_free(s, (n + 1u) * sizeof *s);
    return h->out.n > start ? h->out.d[start].el : nullptr;
}

/* The children of f in one phase of the flow walk. Later siblings are on top, so the last hit wins. */
static const embi_element *hit_flow(hit_st *h, const embi_frag *f, float cx, float cy, hit_rect cclip, int ph)
{
    if (!inside(h, &cclip)) {
        return nullptr;
    }
    if (h->collect) {
        return hit_flow_all(h, f, cx, cy, cclip, ph);
    }
    const embi_element *best = nullptr;
    uint32_t n = 0;
    hit_kid *ks = kids_of(f, &n);
    if (ks != nullptr) {
        for (uint32_t i = 0; i < n; i++) {
            const embi_element *r = flow_child(h, ks[i].f, cx, cy, cclip, ph);
            if (r != nullptr) {
                best = r;
            }
        }
        kids_free(ks, n);
        return best;
    }
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        const embi_element *r = flow_child(h, k, cx, cy, cclip, ph);
        if (r != nullptr) {
            best = r;
        }
    }
    return best;
}

/* An atomic inline or a float: paint_pseudo, hit in reverse (inline, float, block, self). */
static const embi_element *hit_pseudo(hit_st *h, const embi_frag *k, float kx, float ky, hit_rect clip)
{
    if (!inside(h, &clip)) {
        return nullptr;
    }
    float cx, cy;
    hit_rect kc;
    content_of(k, kx, ky, clip, &cx, &cy, &kc);
    const embi_element *r = hit_flow(h, k, cx, cy, kc, HPH_INLINE);
    if (HIT_OPEN(h, r)) {
        const embi_element *f = hit_flow(h, k, cx, cy, kc, HPH_FLOAT);
        r = r != nullptr ? r : f;
    }
    if (HIT_OPEN(h, r)) {
        const embi_element *b = hit_flow(h, k, cx, cy, kc, HPH_BLOCK);
        r = r != nullptr ? r : b;
    }
    if (HIT_OPEN(h, r)) {
        const embi_element *s = self_hit(h, k, kx, ky, &clip);
        r = r != nullptr ? r : s;
    }
    return r;
}

/* Collects the units of the scope whose content is f (origin cx, cy, clip cclip). A stacking child with a z-index other
 * than 0 goes to zs when want_z (it belongs to the enclosing stacking context), a z-index 0 one or a positioned or
 * collapsed box goes to us (a unit of this scope, unless in_unit: then it belongs to a positioned box around it).
 * Positioned boxes are descended for zs but not for us. */
static void gather(hit_st *h, hit_vec *zs, hit_vec *us, const embi_frag *f, float cx, float cy, hit_rect cclip,
                   bool in_unit, bool want_z);

static void gather_child(hit_st *h, hit_vec *zs, hit_vec *us, const embi_frag *k, float ox, float oy, hit_rect clip,
                         bool in_unit, bool want_z)
{
    if (!inside(h, &clip) || k->kind == EMBI_FK_TEXT || (in_unit && !want_z)) {
        return;
    }
    if (k->kind == EMBI_FK_BOX && k->box != nullptr && (k->box->flags2 & EMBI_BF2_SVGFO) != 0) {
        return; /* R4c: a foreignObject is hit through the SVG walk of its svg (embi_layout_hit_svg_fo) */
    }
    float kx, ky;
    origin_of(k, ox, oy, &kx, &ky);
    clip = abs_clip_rect(k, kx, ky, clip); /* the legacy clip of an abspos box bounds the box and its content */
    if (!inside(h, &clip)) {
        return;
    }
    /* A transformed stacking context moves its content: its overflow rectangle is not where the point is tested. */
    bool moved = k->kind == EMBI_FK_BOX && k->box != nullptr && is_stacking(h, k) && embi_lay_has_transform(k->box->style);
    if (k->kind == EMBI_FK_BOX && !moved && !h->nocull && !h->xf) {
        /* Nothing under k can be hit when the point is outside its scrollable overflow rectangle. */
        float ov[4];
        embi_frag_ov(k, ov);
        ov[0] = fminf(ov[0], 0.0f);
        ov[1] = fminf(ov[1], 0.0f);
        ov[2] = fmaxf(ov[2], k->w); /* the border box always counts */
        ov[3] = fmaxf(ov[3], k->h);
        if (h->px < kx + ov[0] || h->px >= kx + ov[2] || h->py < ky + ov[1] || h->py >= ky + ov[3]) {
            return;
        }
    }
    if (is_stacking(h, k)) {
        int32_t z = embi_frag_z_index(k);
        if (z != 0 && want_z) {
            hv_push(zs, k, kx, ky, clip, z);
        } else if (z == 0 && !in_unit) {
            hv_push(us, k, kx, ky, clip, 0);
        }
        return;
    }
    if (k->kind == EMBI_FK_LINE) {
        for (const embi_frag *c = embi_frag_first(k); c != nullptr; c = c->next) {
            gather_child(h, zs, us, c, kx, ky, clip, in_unit, want_z);
        }
        return;
    }
    if (k->kind != EMBI_FK_BOX || k->box == nullptr) {
        return;
    }
    bool unit = positioned(k) || embi_frag_coll(k) != nullptr;
    if (unit && !in_unit) {
        hv_push(us, k, kx, ky, clip, 0);
    }
    float cx, cy;
    hit_rect kc;
    content_of(k, kx, ky, clip, &cx, &cy, &kc);
    gather(h, zs, us, k, cx, cy, kc, in_unit || unit, want_z);
}

static void gather(hit_st *h, hit_vec *zs, hit_vec *us, const embi_frag *f, float cx, float cy, hit_rect cclip,
                   bool in_unit, bool want_z)
{
    uint32_t n = 0;
    hit_kid *ks = kids_of(f, &n);
    if (ks != nullptr) {
        for (uint32_t i = 0; i < n; i++) {
            gather_child(h, zs, us, ks[i].f, cx, cy, cclip, in_unit, want_z);
        }
        kids_free(ks, n);
        return;
    }
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        gather_child(h, zs, us, k, cx, cy, cclip, in_unit, want_z);
    }
}

/* The units of a positioned or collapsed box k (its nested units, painted after its own content). */
static const embi_element *hit_unit(hit_st *h, const embi_frag *k, float kx, float ky, hit_rect clip)
{
    if (!inside(h, &clip)) {
        return nullptr;
    }
    float cx, cy;
    hit_rect kc;
    content_of(k, kx, ky, clip, &cx, &cy, &kc);
    hit_vec zs = {nullptr, 0, 0};
    hit_vec us = {nullptr, 0, 0};
    if (inside(h, &kc)) {
        gather(h, &zs, &us, k, cx, cy, kc, false, false);
    }
    const embi_element *best = nullptr;
    if (us.n > 1) {
        ents_prepare(&us);
        qsort(us.d, us.n, sizeof(hit_ent), cmp_dom);
        ents_unprepare(&us);
    }
    for (uint32_t i = us.n; i > 0 && HIT_OPEN(h, best); i--) {
        const hit_ent *e = &us.d[i - 1];
        const embi_element *r =
            is_stacking(h, e->f) ? hit_ctx(h, e->f, e->x, e->y, e->clip) : hit_unit(h, e->f, e->x, e->y, e->clip);
        best = best != nullptr ? best : r;
    }
    if (HIT_OPEN(h, best) && positioned(k)) {
        const embi_element *r = hit_flow(h, k, cx, cy, kc, HPH_INLINE);
        if (HIT_OPEN(h, r)) {
            const embi_element *x = hit_flow(h, k, cx, cy, kc, HPH_FLOAT);
            r = r != nullptr ? r : x;
        }
        if (HIT_OPEN(h, r)) {
            const embi_element *x = hit_flow(h, k, cx, cy, kc, HPH_BLOCK);
            r = r != nullptr ? r : x;
        }
        if (HIT_OPEN(h, r)) {
            const embi_element *x = self_hit(h, k, kx, ky, &clip);
            r = r != nullptr ? r : x;
        }
        best = best != nullptr ? best : r;
    }
    hv_free(&zs);
    hv_free(&us);
    return best;
}

/* A stacking context (or the root) whose border box is at (kx, ky), in the clip clip, in the space of its own content. */
/* clip-path (R3b): whether the point is inside the clip-path of the context f whose border box is at (kx, ky). A box
 * alone and an unresolvable shape do not clip; the point is tested against the outline of the shape (CSS Masking 1 section
 * 5.1: what is clipped away is not hit). */
static bool clip_path_passes(const hit_st *h, const embi_frag *f, float kx, float ky)
{
    const embi_style *s = f->box != nullptr ? f->box->style : nullptr;
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_clip_path) : nullptr;
    if (v == nullptr || v->kind != EMBI_V_SHAPE || h->nocull) {
        return true;
    }
    float ref[4] = {kx, ky, f->w, f->h};
    float bdr[4], pad[4], mg[4];
    for (int i = 0; i < 4; i++) {
        bdr[i] = embi_frag_bdr(f, i);
        pad[i] = embi_frag_pad(f, i);
        mg[i] = embi_frag_mg(f, i);
    }
    /* The sides are left, top, right, bottom. */
    if (v->kw == EMBI_KW_padding_box || v->kw == EMBI_KW_content_box || v->kw == EMBI_KW_fill_box) {
        ref[0] += bdr[0];
        ref[1] += bdr[1];
        ref[2] -= bdr[0] + bdr[2];
        ref[3] -= bdr[1] + bdr[3];
        if (v->kw != EMBI_KW_padding_box) {
            ref[0] += pad[0];
            ref[1] += pad[1];
            ref[2] -= pad[0] + pad[2];
            ref[3] -= pad[1] + pad[3];
        }
    } else if (v->kw == EMBI_KW_margin_box) {
        ref[0] -= mg[0];
        ref[1] -= mg[1];
        ref[2] += mg[0] + mg[2];
        ref[3] += mg[1] + mg[3];
    }
    if (!(ref[2] > 0.0f) || !(ref[3] > 0.0f)) {
        return false;
    }
    if (v->shape == nullptr) {
        return h->px >= ref[0] && h->px < ref[0] + ref[2] && h->py >= ref[1] && h->py < ref[1] + ref[3];
    }
    const embi_value *fsv = embi_style_value(s, EMBI_PROP_font_size);
    float fs = fsv != nullptr && fsv->kind == EMBI_V_LENGTH && fsv->unit == EMBI_U_PX ? (float)fsv->num : 16.0f;
    embi_lay_curves cv;
    memset(&cv, 0, sizeof cv);
    embi_lay_cp cp;
    bool ok = embi_lay_clip_shape(v->shape, ref, fs, h->vw, h->vh, h->rem, &cp, &cv);
    bool in = !ok || embi_lay_cp_contains(&cp, cv.data, h->px, h->py);
    embi_vec_free(&cv);
    return in;
}

static const embi_element *hit_ctx_body(hit_st *h, const embi_frag *f, float kx, float ky, hit_rect clip)
{
    if (!inside(h, &clip) || (f != h->root && !clip_path_passes(h, f, kx, ky))) {
        return nullptr;
    }
    float cx, cy;
    hit_rect cclip;
    content_of(f, kx, ky, clip, &cx, &cy, &cclip);
    hit_vec zs = {nullptr, 0, 0};
    hit_vec us = {nullptr, 0, 0};
    if (inside(h, &cclip)) {
        gather(h, &zs, &us, f, cx, cy, cclip, false, true);
    }
    if (zs.n > 1) {
        ents_prepare(&zs);
        qsort(zs.d, zs.n, sizeof(hit_ent), cmp_z);
        ents_unprepare(&zs);
    }
    if (us.n > 1) {
        ents_prepare(&us);
        qsort(us.d, us.n, sizeof(hit_ent), cmp_dom);
        ents_unprepare(&us);
    }
    const embi_element *best = nullptr;
    /* Step 7: positive z-index, topmost first. */
    uint32_t i = zs.n;
    for (; i > 0 && zs.d[i - 1].z > 0 && HIT_OPEN(h, best); i--) {
        const hit_ent *e = &zs.d[i - 1];
        const embi_element *r = hit_ctx(h, e->f, e->x, e->y, e->clip);
        best = best != nullptr ? best : r;
    }
    /* Step 6: the positioned units and z-index 0 contexts. */
    for (uint32_t j = us.n; j > 0 && HIT_OPEN(h, best); j--) {
        const hit_ent *e = &us.d[j - 1];
        const embi_element *r =
            is_stacking(h, e->f) ? hit_ctx(h, e->f, e->x, e->y, e->clip) : hit_unit(h, e->f, e->x, e->y, e->clip);
        best = best != nullptr ? best : r;
    }
    /* Steps 5, 4 and 3: the flow phases, inline on top. */
    if (inside(h, &cclip)) {
        if (HIT_OPEN(h, best)) {
            const embi_element *r = hit_flow(h, f, cx, cy, cclip, HPH_INLINE);
            best = best != nullptr ? best : r;
        }
        if (HIT_OPEN(h, best)) {
            const embi_element *r = hit_flow(h, f, cx, cy, cclip, HPH_FLOAT);
            best = best != nullptr ? best : r;
        }
        if (HIT_OPEN(h, best)) {
            const embi_element *r = hit_flow(h, f, cx, cy, cclip, HPH_BLOCK);
            best = best != nullptr ? best : r;
        }
    }
    /* Step 2: negative z-index, the least negative first. */
    uint32_t neg = 0;
    while (neg < zs.n && zs.d[neg].z < 0) {
        neg++;
    }
    for (uint32_t k = neg; k > 0 && HIT_OPEN(h, best); k--) {
        const hit_ent *e = &zs.d[k - 1];
        const embi_element *r = hit_ctx(h, e->f, e->x, e->y, e->clip);
        best = best != nullptr ? best : r;
    }
    /* Step 1: the context's own box. */
    if (HIT_OPEN(h, best)) {
        const embi_element *r = self_hit(h, f, kx, ky, &clip);
        best = best != nullptr ? best : r;
    }
    hv_free(&zs);
    hv_free(&us);
    return best;
}

/* ------------------------------------------------------------------ transforms */

/* hit_ctx_body with the perspective of f on the stack while its content is tested. */
static const embi_element *hit_ctx_pers(hit_st *h, const embi_frag *f, float kx, float ky, hit_rect clip)
{
    const embi_style *s = f->box != nullptr ? f->box->style : nullptr;
    embi_m4 pm;
    float bb[4] = {kx, ky, f->w, f->h};
    bool has = s != nullptr && f != h->root && f->box->el != nullptr && h->npers < HIT_MAX_PERS &&
               embi_lay_perspective_matrix(s, bb, h->vw, h->vh, h->rem, &pm);
    if (has) {
        h->pers[h->npers].el = f->box->el;
        h->pers[h->npers].m = pm;
        h->npers++;
    }
    const embi_element *r = hit_ctx_body(h, f, kx, ky, clip);
    if (has) {
        h->npers--;
    }
    return r;
}

/* The point (px, py) of the space a plane is composed into, mapped into the plane's own space through the inverse of
 * its homography. False when the plane is singular or the point lies on the side behind the viewer. */
static bool unmap_point(const embi_m4 *t, float px, float py, double *x, double *y)
{
    double hh[9], inv[9];
    embi_m4_plane(t, hh);
    if (!embi_h3_invert(hh, inv)) {
        return false;
    }
    double w = inv[6] * px + inv[7] * py + inv[8];
    if (!(fabs(w) > 1e-12)) {
        return false;
    }
    double lx = (inv[0] * px + inv[1] * py + inv[2]) / w;
    double ly = (inv[3] * px + inv[4] * py + inv[5]) / w;
    if (!isfinite(lx) || !isfinite(ly)) {
        return false;
    }
    if (!(hh[6] * lx + hh[7] * ly + hh[8] > 0.0)) {
        return false;
    }
    *x = lx;
    *y = ly;
    return true;
}

static void c3_push(hit_c3 *c, const embi_frag *f, float kx, float ky, const embi_m4 *rel)
{
    if (c->np == c->cap) {
        uint32_t ncap = c->cap != 0 ? c->cap * 2u : 8u;
        if (c->parts == nullptr) {
            c->parts = embi_alloc(ncap * sizeof *c->parts);
        } else {
            c->parts = embi_realloc(c->parts, c->cap * sizeof *c->parts, ncap * sizeof *c->parts);
        }
        c->cap = ncap;
    }
    c->parts[c->np].f = f;
    c->parts[c->np].kx = kx;
    c->parts[c->np].ky = ky;
    c->parts[c->np].rel = *rel;
    c->np++;
}

/* Tests the plane of f in a 3D rendering context (t is its matrix into the space of the context's point), then the planes
 * that are its children. */
static void hit_plane(hit_st *h, hit_c3 *c, const embi_frag *f, float kx, float ky, const embi_m4 *t)
{
    const embi_style *s = f->box != nullptr ? f->box->style : nullptr;
    uint32_t rank = h->collect ? ++h->planes : 0; /* collect mode: planes are ranked in the order they are tested */
    const embi_element *saved_el = c->cur_el;
    bool saved_pres = c->cur_pres;
    c->cur_el = f->box != nullptr ? f->box->el : nullptr;
    c->cur_pres = embi_lay_preserve3d_used(s);
    uint32_t mark = c->np;
    float spx = h->px, spy = h->py;

    /* Finding the planes of f needs a walk that prunes nothing and hits nothing. */
    bool saved_nocull = h->nocull;
    c->discover = true;
    h->nocull = true;
    (void)hit_ctx_pers(h, f, kx, ky, k_all);
    h->nocull = saved_nocull;
    c->discover = false;

    double hh[9];
    embi_m4_plane(t, hh);
    double x, y;
    if (!(embi_lay_backface_hidden(s) && embi_h3_det(hh) < 0.0) && unmap_point(t, c->sx, c->sy, &x, &y)) {
        h->px = (float)x;
        h->py = (float)y;
        uint32_t first = h->out.n;
        const embi_element *r = hit_ctx_pers(h, f, kx, ky, k_all);
        if (r != nullptr || h->collect) {
            double o[4];
            embi_m4_point(t, x, y, 0.0, o);
            double z = o[2] / (o[3] > 1e-9 ? o[3] : 1e-9);
            if (h->collect) {
                /* the hits of this plane are at its depth; the rank counts planes in test order (later planes win ties) */
                out_tag(h, first, z, rank);
            }
            if (r != nullptr && (c->best == nullptr || z >= c->best_z)) {
                c->best = r;
                c->best_z = z;
            }
        }
    }
    h->px = spx;
    h->py = spy;

    /* The planes found were met top first; they paint in the reverse order. */
    uint32_t n = c->np - mark;
    if (n > 0) {
        hit_part *kids = embi_alloc(n * sizeof *kids);
        for (uint32_t i = 0; i < n; i++) {
            kids[i] = c->parts[mark + (n - 1 - i)];
        }
        c->np = mark;
        for (uint32_t i = 0; i < n; i++) {
            embi_m4 t2;
            embi_m4_mul(t, &kids[i].rel, &t2);
            hit_plane(h, c, kids[i].f, kids[i].kx, kids[i].ky, &t2);
        }
        embi_free(kids, n * sizeof *kids);
    }
    c->cur_el = saved_el;
    c->cur_pres = saved_pres;
}

/* A stacking context (or the root) whose border box is at (kx, ky), in the clip clip. */
static const embi_element *hit_ctx(hit_st *h, const embi_frag *f, float kx, float ky, hit_rect clip)
{
    const embi_style *s = f->box != nullptr ? f->box->style : nullptr;
    if (s == nullptr || f == h->root) {
        return hit_ctx_pers(h, f, kx, ky, clip);
    }
    bool own = embi_lay_has_transform(s);
    bool pres = embi_lay_preserve3d_used(s);
    const embi_element *par = embi_lay_parent_el(f->box);
    bool part = h->c3 != nullptr && h->c3->cur_pres && par != nullptr && par == h->c3->cur_el;
    if (h->nocull && !part) {
        /* A discovery pass only looks for the planes of the context being tested, and those are direct children: a
         * context below one that is not a plane of it (a nested preserve-3d box, say) has none of them. */
        return nullptr;
    }
    if (!own && !pres && !part) {
        return hit_ctx_pers(h, f, kx, ky, clip);
    }
    float bb[4] = {kx, ky, f->w, f->h};
    embi_m4 om, rel;
    embi_lay_box_matrix(s, bb, h->vw, h->vh, h->rem, &om);
    if (!embi_m4_invertible(&om)) {
        return nullptr;
    }
    rel = om;
    for (uint32_t i = h->npers; par != nullptr && i-- > 0;) {
        if (h->pers[i].el == par) {
            embi_m4_mul(&h->pers[i].m, &om, &rel);
            break;
        }
    }
    if (part) {
        if (h->c3->discover) {
            c3_push(h->c3, f, kx, ky, &rel);
        }
        return nullptr;
    }
    if (!inside(h, &clip)) {
        return nullptr;
    }
    if (pres) {
        hit_c3 c;
        memset(&c, 0, sizeof c);
        c.prev = h->c3;
        c.sx = h->px;
        c.sy = h->py;
        h->c3 = &c;
        uint32_t mark = h->out.n;
        hit_plane(h, &c, f, kx, ky, &rel);
        h->c3 = c.prev;
        if (h->collect) {
            out_sort_3d(h, mark);
        }
        if (c.parts != nullptr) {
            embi_free(c.parts, c.cap * sizeof *c.parts);
        }
        return c.best;
    }
    double hh[9];
    embi_m4_plane(&rel, hh);
    double x, y;
    if ((embi_lay_backface_hidden(s) && embi_h3_det(hh) < 0.0) || !unmap_point(&rel, h->px, h->py, &x, &y)) {
        return nullptr;
    }
    float spx = h->px, spy = h->py;
    h->px = (float)x;
    h->py = (float)y;
    const embi_element *r = hit_ctx_pers(h, f, kx, ky, k_all);
    h->px = spx;
    h->py = spy;
    return r;
}

const embi_element *embi_layout_hit_svg_fo(const embi_layout_engine *e, const embi_element *fo, float x, float y)
{
    if (e == nullptr || fo == nullptr || fo->box == nullptr) {
        return nullptr;
    }
    const embi_box *b = (const embi_box *)fo->box;
    const embi_frag *f = b->frag;
    if ((b->flags2 & EMBI_BF2_SVGFO) == 0 || f == nullptr || f->box != b) {
        return nullptr;
    }
    const embi_frag *root = embi_layout_root(e, fo->base.doc);
    if (root == nullptr) {
        return nullptr;
    }
    hit_st h;
    memset(&h, 0, sizeof h);
    h.px = x;
    h.py = y;
    h.root = root;
    h.eng = e;
    h.vw = root->w;
    h.vh = root->h;
    h.rem = embi_lay_root_font_px(root);
    h.xf = embi_lay_doc_has_transform(e, fo->base.doc);
    /* the box is a context of its own; its transform is the SVG walk's business (the point is already in its space) */
    return hit_ctx_pers(&h, f, 0.0f, 0.0f, k_all);
}

embi_element *embi_layout_hit_test(const embi_layout_engine *e, const embi_document *doc, float x, float y)
{
    const embi_frag *root = embi_layout_root(e, doc);
    if (root == nullptr) {
        return nullptr;
    }
    hit_st h;
    memset(&h, 0, sizeof h);
    h.px = x;
    h.py = y;
    h.root = root;
    h.eng = e;
    h.vw = root->w;
    h.vh = root->h;
    h.rem = embi_lay_root_font_px(root);
    h.xf = embi_lay_doc_has_transform(e, doc);
    return (embi_element *)(uintptr_t)hit_ctx(&h, root, root->x, root->y, k_all);
}

static int cmp_dup(const void *a, const void *b)
{
    const hit_dup *x = a, *y = b;
    if (x->el != y->el) {
        return (uintptr_t)x->el < (uintptr_t)y->el ? -1 : 1;
    }
    return x->pos < y->pos ? -1 : (x->pos > y->pos ? 1 : 0);
}

void embi_layout_hit_test_all(const embi_layout_engine *e, const embi_document *doc, float x, float y, embi_element ***out,
                              uint32_t *n)
{
    *out = nullptr;
    *n = 0;
    const embi_frag *root = embi_layout_root(e, doc);
    if (root == nullptr) {
        return;
    }
    hit_st h;
    memset(&h, 0, sizeof h);
    h.px = x;
    h.py = y;
    h.root = root;
    h.eng = e;
    h.vw = root->w;
    h.vh = root->h;
    h.rem = embi_lay_root_font_px(root);
    h.xf = embi_lay_doc_has_transform(e, doc);
    h.collect = true;
    (void)hit_ctx(&h, root, root->x, root->y, k_all);
    uint32_t m = h.out.n;
    if (m > 0) {
        /* An element met twice (a plane hit at two depths, say) keeps its first place. */
        hit_dup *d = embi_alloc(m * sizeof *d);
        uint8_t *dup = embi_alloc(m);
        for (uint32_t i = 0; i < m; i++) {
            d[i].el = h.out.d[i].el;
            d[i].pos = i;
            dup[i] = 0;
        }
        qsort(d, m, sizeof *d, cmp_dup);
        for (uint32_t i = 1; i < m; i++) {
            if (d[i].el == d[i - 1].el) {
                dup[d[i].pos] = 1;
            }
        }
        embi_element **res = embi_alloc(m * sizeof *res);
        uint32_t k = 0;
        for (uint32_t i = 0; i < m; i++) {
            if (dup[i] == 0) {
                res[k++] = (embi_element *)(uintptr_t)h.out.d[i].el;
            }
        }
        embi_free(d, m * sizeof *d);
        embi_free(dup, m);
        if (k < m) {
            res = embi_realloc(res, m * sizeof *res, k * sizeof *res);
        }
        *out = res;
        *n = k;
    }
    if (h.out.d != nullptr) {
        embi_free(h.out.d, h.out.cap * sizeof *h.out.d);
    }
}
