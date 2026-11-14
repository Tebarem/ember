/*
 * Damage between two display lists (W4d). See damage.h for the rules. The lists are
 * grouped by stacking context (the context of each item is the innermost one whose item range holds it), and each
 * group is compared with the group of the same index in the other list.
 */
#include "paint/damage.h"

#include "paint/layer.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Rects beyond this count are replaced by their bounding box before merging (the merge is quadratic). */
#define DAMAGE_RECTS_MAX 64u

typedef EMBI_VEC(uint32_t) u32vec;

/* The items of a list grouped by stacking context: group g holds order[start[g] .. start[g + 1]), in item order.
 * Group ctxs.len is the items no context covers (only possible for an empty context table). */
typedef struct dgroups {
    u32vec order;
    u32vec start;
    uint32_t groups;
} dgroups;

static void u32_fill(u32vec *v, uint32_t n, uint32_t val)
{
    embi_vec_reserve(v, n);
    for (uint32_t i = 0; i < n; i++) {
        v->data[i] = val;
    }
    v->len = n;
}

static void dgroups_build(dgroups *g, const embi_dl *d)
{
    uint32_t n = d->items.len;
    uint32_t nc = d->ctxs.len;
    g->groups = nc + 1;
    u32vec owner = {nullptr, 0, 0};
    u32_fill(&owner, n, nc);
    /* Contexts are created parent first, so a nested context overwrites its parent's items. */
    for (uint32_t k = 0; k < nc; k++) {
        const embi_dctx *c = &d->ctxs.data[k];
        uint32_t end = c->end < n ? c->end : n;
        for (uint32_t i = c->first; i < end; i++) {
            owner.data[i] = k;
        }
    }
    u32_fill(&g->start, g->groups + 1, 0);
    for (uint32_t i = 0; i < n; i++) {
        g->start.data[owner.data[i] + 1]++;
    }
    for (uint32_t k = 0; k < g->groups; k++) {
        g->start.data[k + 1] += g->start.data[k];
    }
    u32vec fill = {nullptr, 0, 0};
    u32_fill(&fill, g->groups, 0);
    for (uint32_t k = 0; k < g->groups; k++) {
        fill.data[k] = g->start.data[k];
    }
    u32_fill(&g->order, n, 0);
    for (uint32_t i = 0; i < n; i++) {
        g->order.data[fill.data[owner.data[i]]++] = i;
    }
    embi_vec_free(&owner);
    embi_vec_free(&fill);
}

static void dgroups_free(dgroups *g)
{
    embi_vec_free(&g->order);
    embi_vec_free(&g->start);
    g->groups = 0;
}

/* The items of group k (empty when k is past the groups of the list). */
static void dgroup_seg(const dgroups *g, uint32_t k, const uint32_t **seg, uint32_t *n)
{
    if (k >= g->groups) {
        *seg = nullptr;
        *n = 0;
        return;
    }
    *n = g->start.data[k + 1] - g->start.data[k];
    /* No pointer arithmetic on an empty order (its data may be null, which UBSan reports even for an offset of 0). */
    *seg = *n > 0 ? g->order.data + g->start.data[k] : nullptr;
}

/* The output of one compute: the view and the scale, and the rects being collected. */
typedef struct dctx {
    embi_damage *out;
    float scale;
    int32_t vw, vh;
} dctx;

static void push_rect(dctx *c, float x0, float y0, float x1, float y1)
{
    float fx0 = floorf(x0 * c->scale), fy0 = floorf(y0 * c->scale);
    float fx1 = ceilf(x1 * c->scale), fy1 = ceilf(y1 * c->scale);
    if (!(fx1 > fx0) || !(fy1 > fy0)) {
        return;
    }
    embi_drect r;
    r.x0 = fx0 < 0.0f ? 0 : (fx0 > (float)c->vw ? c->vw : (int32_t)fx0);
    r.y0 = fy0 < 0.0f ? 0 : (fy0 > (float)c->vh ? c->vh : (int32_t)fy0);
    r.x1 = fx1 > (float)c->vw ? c->vw : (fx1 < 0.0f ? 0 : (int32_t)fx1);
    r.y1 = fy1 > (float)c->vh ? c->vh : (fy1 < 0.0f ? 0 : (int32_t)fy1);
    if (r.x1 > r.x0 && r.y1 > r.y0) {
        embi_vec_push(&c->out->rects, r);
    }
}

/* Adds the bounds of item i of d (its clipped bounds) when it has any. The item's own box is grown by one device px first:
 * the antialiased fringe of a rounded or transformed item can touch a pixel just outside the floor/ceil box of its mapped
 * rect. A text item also grows by a quarter of its height: glyphs overhang their fragment (italics, negative bearings,
 * accents). The clips are not grown, so damage never leaves the clip a change is made in. */
static void add_item(dctx *c, const embi_dl *d, uint32_t i)
{
    const embi_ditem *it = &d->items.data[i];
    float pad = 1.0f / (c->scale > 0.0f ? c->scale : 1.0f);
    if (it->kind == EMBI_DI_TEXT) {
        pad += 0.25f * it->rect[3];
    }
    float b[4];
    if (embi_layer_item_bounds_pad(d, it, true, pad, b)) {
        push_rect(c, b[0], b[1], b[2], b[3]);
    }
}

/* Adds the bounds of every drawable item in [lo, hi] of d: the content of a group that changed. */
static void add_range(dctx *c, const embi_dl *d, uint32_t lo, uint32_t hi)
{
    if (d->items.len == 0) {
        return;
    }
    if (hi >= d->items.len) {
        hi = d->items.len - 1;
    }
    for (uint32_t i = lo; i <= hi && i < d->items.len; i++) {
        add_item(c, d, i);
    }
}

/* The index of the marker that pairs with the layer marker at index i of d (its LAYER_END, or its LAYER_BEGIN), or i when
 * it has none. paint.c (finish_layer) sets the aux of both markers of a group: a LAYER_END's aux is its begin and a
 * LAYER_BEGIN's aux is its end. A group that was folded or dropped has no markers left, so there is nothing to pair. The
 * aux is checked before it is trusted; the forward scan is the fallback for a list built by hand. */
static uint32_t layer_partner(const embi_dl *d, uint32_t i)
{
    const embi_ditem *it = &d->items.data[i];
    if (it->kind == EMBI_DI_LAYER_END) {
        return it->aux < d->items.len ? it->aux : i;
    }
    if (it->aux > i && it->aux < d->items.len && d->items.data[it->aux].kind == EMBI_DI_LAYER_END) {
        return it->aux;
    }
    int depth = 0;
    for (uint32_t j = i + 1; j < d->items.len; j++) {
        if (d->items.data[j].kind == EMBI_DI_LAYER_BEGIN) {
            depth++;
        } else if (d->items.data[j].kind == EMBI_DI_LAYER_END) {
            if (depth == 0) {
                return j;
            }
            depth--;
        }
    }
    return i;
}

/* A layer marker at index i of d covers the items between it and its partner. */
static void add_layer(dctx *c, const embi_dl *d, uint32_t i)
{
    uint32_t other = layer_partner(d, i);
    uint32_t lo = other < i ? other : i;
    uint32_t hi = other < i ? i : other;
    add_range(c, d, lo, hi);
}

static bool is_layer(const embi_ditem *it)
{
    return it->kind == EMBI_DI_LAYER_BEGIN || it->kind == EMBI_DI_LAYER_END;
}

/* The bounds of the markers of a group segment (their ranges are damaged when the group changed). */
static void add_markers(dctx *c, const embi_dl *d, const uint32_t *seg, uint32_t n)
{
    for (uint32_t j = 0; j < n; j++) {
        if (is_layer(&d->items.data[seg[j]])) {
            add_layer(c, d, seg[j]);
        }
    }
}

static void add_group_all(dctx *c, const embi_dl *d, const uint32_t *seg, uint32_t n)
{
    for (uint32_t j = 0; j < n; j++) {
        if (!is_layer(&d->items.data[seg[j]])) {
            add_item(c, d, seg[j]);
        }
    }
    add_markers(c, d, seg, n);
}

static bool rect_touch(const embi_drect *a, const embi_drect *b)
{
    return a->x0 <= b->x1 && b->x0 <= a->x1 && a->y0 <= b->y1 && b->y0 <= a->y1;
}

static uint64_t rect_area(const embi_drect *r)
{
    return (uint64_t)(r->x1 - r->x0) * (uint64_t)(r->y1 - r->y0);
}

static embi_drect bbox_of(const embi_drect *r, uint32_t n)
{
    embi_drect b = r[0];
    for (uint32_t i = 1; i < n; i++) {
        b.x0 = b.x0 < r[i].x0 ? b.x0 : r[i].x0;
        b.y0 = b.y0 < r[i].y0 ? b.y0 : r[i].y0;
        b.x1 = b.x1 > r[i].x1 ? b.x1 : r[i].x1;
        b.y1 = b.y1 > r[i].y1 ? b.y1 : r[i].y1;
    }
    return b;
}

/* Merges the collected rects (see damage.h) and sets full, any and area. */
static void finish(embi_damage *out, int32_t vw, int32_t vh)
{
    uint64_t view_area = (uint64_t)vw * (uint64_t)vh;
    out->full = false;
    out->area = 0;
    /* Overlapping rects join until none overlap; past DAMAGE_RECTS_MAX they become their bounding box. */
    embi_drect *r = out->rects.data;
    uint32_t n = out->rects.len;
    if (n > DAMAGE_RECTS_MAX) {
        r[0] = bbox_of(r, n);
        n = 1;
    }
    bool merged = true;
    while (merged && n > 1) {
        merged = false;
        for (uint32_t i = 0; i < n && !merged; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                if (rect_touch(&r[i], &r[j])) {
                    r[i].x0 = r[i].x0 < r[j].x0 ? r[i].x0 : r[j].x0;
                    r[i].y0 = r[i].y0 < r[j].y0 ? r[i].y0 : r[j].y0;
                    r[i].x1 = r[i].x1 > r[j].x1 ? r[i].x1 : r[j].x1;
                    r[i].y1 = r[i].y1 > r[j].y1 ? r[i].y1 : r[j].y1;
                    r[j] = r[n - 1];
                    n--;
                    merged = true;
                    break;
                }
            }
        }
    }
    /* More than half the view damaged: one rect, the bounding box. */
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += rect_area(&r[i]);
    }
    if (n > 1 && sum * 2 > view_area) {
        r[0] = bbox_of(r, n);
        n = 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        out->area += rect_area(&r[i]);
    }
    out->rects.len = n;
    if (out->area * 10 > view_area * 6) {
        out->full = true;
        out->rects.len = 0;
        out->area = view_area;
    }
    out->any = out->full || out->rects.len > 0;
}

void embi_damage_init(embi_damage *d)
{
    memset(d, 0, sizeof *d);
}

void embi_damage_free(embi_damage *d)
{
    embi_vec_free(&d->rects);
    d->full = false;
    d->any = false;
    d->area = 0;
    d->changed = 0;
}

/* ------------------------------------------------------------------ alignment */

typedef EMBI_VEC(uint64_t) u64vec;
typedef EMBI_VEC(int32_t) i32vec;

/* Scratch arrays reused across the groups of one compute. */
typedef struct dscratch {
    u64vec ha, hb, sp, sc; /* group item hashes of both sides; context signatures of both sides */
    i32vec ma, mb, cp, cc; /* match results for items and for contexts */
} dscratch;

static void u64_fill(u64vec *v, uint32_t n)
{
    embi_vec_reserve(v, n);
    v->len = n;
}

static void i32_fill(i32vec *v, uint32_t n)
{
    embi_vec_reserve(v, n);
    for (uint32_t i = 0; i < n; i++) {
        v->data[i] = -1;
    }
    v->len = n;
}

/* Table cells above which the longest common subsequence is not computed (the unmatched middle is then damaged). */
#define LCS_CELLS_MAX (1u << 20)

/* Aligns the sequences a (na) and b (nb) by value: ma[i] is the index in b matched with a[i], or -1; mb is the inverse.
 * A longest common subsequence, after trimming the common prefix and suffix; when the differing middle is too large for
 * the table it stays unmatched (and is damaged whole by the caller). */
static void lcs_match(const uint64_t *a, uint32_t na, const uint64_t *b, uint32_t nb, int32_t *ma, int32_t *mb)
{
    for (uint32_t i = 0; i < na; i++) {
        ma[i] = -1;
    }
    for (uint32_t j = 0; j < nb; j++) {
        mb[j] = -1;
    }
    uint32_t pre = 0;
    while (pre < na && pre < nb && a[pre] == b[pre]) {
        ma[pre] = (int32_t)pre;
        mb[pre] = (int32_t)pre;
        pre++;
    }
    uint32_t suf = 0;
    while (suf < na - pre && suf < nb - pre && a[na - 1 - suf] == b[nb - 1 - suf]) {
        ma[na - 1 - suf] = (int32_t)(nb - 1 - suf);
        mb[nb - 1 - suf] = (int32_t)(na - 1 - suf);
        suf++;
    }
    uint32_t m = na - pre - suf, n = nb - pre - suf;
    if (m == 0 || n == 0 || (uint64_t)(m + 1) * (uint64_t)(n + 1) > LCS_CELLS_MAX) {
        return;
    }
    /* t[i][j]: the common subsequence length of a[pre + i ..) and b[pre + j ..); at most min(m, n) <= 1024. */
    uint32_t w = n + 1;
    size_t cells = (size_t)(m + 1) * w;
    uint16_t *t = embi_calloc(cells, sizeof *t);
    if (t == nullptr) {
        return;
    }
    for (uint32_t i = m; i-- > 0;) {
        for (uint32_t j = n; j-- > 0;) {
            if (a[pre + i] == b[pre + j]) {
                t[(size_t)i * w + j] = (uint16_t)(t[(size_t)(i + 1) * w + j + 1] + 1);
            } else {
                uint16_t x = t[(size_t)(i + 1) * w + j], y = t[(size_t)i * w + j + 1];
                t[(size_t)i * w + j] = x > y ? x : y;
            }
        }
    }
    uint32_t i = 0, j = 0;
    while (i < m && j < n) {
        if (a[pre + i] == b[pre + j]) {
            ma[pre + i] = (int32_t)(pre + j);
            mb[pre + j] = (int32_t)(pre + i);
            i++;
            j++;
        } else if (t[(size_t)(i + 1) * w + j] >= t[(size_t)i * w + j + 1]) {
            i++;
        } else {
            j++;
        }
    }
    embi_free(t, cells * sizeof *t);
}

/* Damages the bounds of an item that has no counterpart in the other list. A layer marker damages the whole group it
 * opens or closes. */
static void add_unmatched(dctx *c, const embi_dl *d, uint32_t i)
{
    if (is_layer(&d->items.data[i])) {
        add_layer(c, d, i);
    } else {
        add_item(c, d, i);
    }
}

/* Compares one pair of groups (the item indices sa of prev and sb of cur, in item order) by aligning their item hashes.
 * hp and hc are the per-item hashes of the two lists. */
static void diff_group(dctx *c, dscratch *sc, const embi_dl *prev, const uint64_t *hp, const uint32_t *sa, uint32_t na,
                       const embi_dl *cur, const uint64_t *hc, const uint32_t *sb, uint32_t nb)
{
    if (na == 0 && nb == 0) {
        return;
    }
    u64_fill(&sc->ha, na);
    u64_fill(&sc->hb, nb);
    i32_fill(&sc->ma, na);
    i32_fill(&sc->mb, nb);
    for (uint32_t j = 0; j < na; j++) {
        sc->ha.data[j] = hp[sa[j]];
    }
    for (uint32_t j = 0; j < nb; j++) {
        sc->hb.data[j] = hc[sb[j]];
    }
    lcs_match(sc->ha.data, na, sc->hb.data, nb, sc->ma.data, sc->mb.data);
    uint32_t ua = 0, ub = 0;
    for (uint32_t j = 0; j < na; j++) {
        if (sc->ma.data[j] < 0) {
            ua++;
            add_unmatched(c, prev, sa[j]);
        }
    }
    for (uint32_t j = 0; j < nb; j++) {
        if (sc->mb.data[j] < 0) {
            ub++;
            add_unmatched(c, cur, sb[j]);
        }
    }
    c->out->changed += ua > ub ? ua : ub;
}

static void list_hashes(const embi_dl *d, u64vec *h)
{
    u64_fill(h, d->items.len);
    for (uint32_t i = 0; i < d->items.len; i++) {
        h->data[i] = embi_layer_item_hash(d, i, 0.0f, 0.0f);
    }
}

static uint64_t group_signature(const uint64_t *h, const uint32_t *seg, uint32_t n)
{
    uint64_t sig = 0xC2B2AE3D27D4EB4Full ^ n;
    for (uint32_t j = 0; j < n; j++) {
        sig = embi_hash64(sig ^ (h[seg[j]] + 0x9E3779B97F4A7C15ull + (sig << 6) + (sig >> 2)));
    }
    return sig;
}

/* Whether two item arrays are equal field by field: the byte after styles[] is padding with no defined value, so it is
 * skipped (memcmp over the whole record would see differences that are not there). */
static bool items_equal(const embi_ditem *a, const embi_ditem *b, uint32_t n)
{
    const size_t pad = offsetof(embi_ditem, styles) + sizeof a->styles;
    static_assert(offsetof(embi_ditem, xform) > offsetof(embi_ditem, styles) + sizeof(((embi_ditem *)0)->styles),
                  "the padding byte after styles");
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *pa = (const unsigned char *)&a[i], *pb = (const unsigned char *)&b[i];
        if (memcmp(pa, pb, pad) != 0 ||
            memcmp(pa + offsetof(embi_ditem, xform), pb + offsetof(embi_ditem, xform),
                   sizeof(embi_ditem) - offsetof(embi_ditem, xform)) != 0) {
            return false;
        }
    }
    return true;
}

/* Whether two display lists hold the same items, clips, transforms, planes, stops and runs. The other records have no
 * padding, so they compare byte for byte. */
static bool lists_equal(const embi_dl *a, const embi_dl *b)
{
#define SAME(v, T) ((a)->v.len == (b)->v.len && ((a)->v.len == 0 || memcmp((a)->v.data, (b)->v.data, (a)->v.len * sizeof(T)) == 0))
    return a->items.len == b->items.len && items_equal(a->items.data, b->items.data, a->items.len) &&
           SAME(clips, embi_dclip) && SAME(xforms, embi_dxform) && SAME(planes, embi_dplane) && SAME(stops, embi_dstop) &&
           SAME(runs, const void *) && a->urltext.len == b->urltext.len &&
           (a->urltext.len == 0 || memcmp(a->urltext.data, b->urltext.data, a->urltext.len) == 0);
#undef SAME
}

void embi_damage_set_full(embi_damage *out, uint32_t view_w, uint32_t view_h, uint32_t changed)
{
    out->rects.len = 0;
    out->changed = changed;
    out->area = (uint64_t)view_w * (uint64_t)view_h;
    out->full = out->area > 0;
    out->any = out->area > 0;
}

void embi_damage_set_page_rects(embi_damage *out, const float (*r)[4], uint32_t n, float scale, uint32_t view_w, uint32_t view_h)
{
    out->rects.len = 0;
    out->changed = n;
    out->full = false;
    out->any = false;
    out->area = 0;
    dctx c = {out, scale, (int32_t)view_w, (int32_t)view_h};
    for (uint32_t i = 0; i < n; i++) {
        push_rect(&c, r[i][0] - 1.0f / scale, r[i][1] - 1.0f / scale, r[i][2] + 1.0f / scale, r[i][3] + 1.0f / scale);
    }
    finish(out, (int32_t)view_w, (int32_t)view_h);
}

void embi_damage_compute(embi_damage *out, const embi_dl *prev, const embi_dl *cur, float scale, uint32_t view_w,
                         uint32_t view_h, bool force_full)
{
    out->rects.len = 0;
    out->changed = 0;
    out->full = false;
    out->any = false;
    out->area = 0;
    int32_t vw = (int32_t)view_w, vh = (int32_t)view_h;
    uint64_t view_area = (uint64_t)view_w * (uint64_t)view_h;
    if (!force_full && prev != nullptr && (cur->planes.len > 0 || prev->planes.len > 0)) {
        /* A 3D plane (R3c) holds items in its own space, so their bounds say nothing about the pixels they change: a list
         * with planes is compared whole, and any difference is a full redraw. */
        if (!lists_equal(prev, cur)) {
            force_full = true;
        } else {
            return;
        }
    }
    if (force_full || prev == nullptr) {
        out->full = true;
        out->any = view_area > 0;
        out->area = view_area;
        return;
    }
    dctx c = {out, scale, vw, vh};
    dgroups gp, gc;
    memset(&gp, 0, sizeof gp);
    memset(&gc, 0, sizeof gc);
    dgroups_build(&gp, prev);
    dgroups_build(&gc, cur);
    dscratch sc;
    memset(&sc, 0, sizeof sc);
    u64vec hp = {nullptr, 0, 0}, hc = {nullptr, 0, 0};
    list_hashes(prev, &hp);
    list_hashes(cur, &hc);
    /* The stacking contexts are aligned by their content signature first: inserting or removing one context shifts the
     * indices of the rest, which must not damage them. Contexts that differ between two anchors are paired by position
     * and compared item by item (items aligned by hash too); the leftovers are damaged whole. The last group of each
     * list (the items no context covers) pairs with its counterpart. */
    uint32_t np = gp.groups - 1, nc = gc.groups - 1;
    u64_fill(&sc.sp, np);
    u64_fill(&sc.sc, nc);
    i32_fill(&sc.cp, np);
    i32_fill(&sc.cc, nc);
    for (uint32_t k = 0; k < np; k++) {
        const uint32_t *seg;
        uint32_t n;
        dgroup_seg(&gp, k, &seg, &n);
        sc.sp.data[k] = group_signature(hp.data, seg, n);
    }
    for (uint32_t k = 0; k < nc; k++) {
        const uint32_t *seg;
        uint32_t n;
        dgroup_seg(&gc, k, &seg, &n);
        sc.sc.data[k] = group_signature(hc.data, seg, n);
    }
    lcs_match(sc.sp.data, np, sc.sc.data, nc, sc.cp.data, sc.cc.data);
    uint32_t i = 0, j = 0;
    while (i < np || j < nc) {
        uint32_t i0 = i, j0 = j;
        while (i < np && sc.cp.data[i] < 0) {
            i++;
        }
        while (j < nc && sc.cc.data[j] < 0) {
            j++;
        }
        uint32_t rp = i - i0, rc = j - j0;
        uint32_t both = rp < rc ? rp : rc;
        for (uint32_t k = 0; k < both; k++) {
            const uint32_t *sa, *sb;
            uint32_t na, nb;
            dgroup_seg(&gp, i0 + k, &sa, &na);
            dgroup_seg(&gc, j0 + k, &sb, &nb);
            diff_group(&c, &sc, prev, hp.data, sa, na, cur, hc.data, sb, nb);
        }
        for (uint32_t k = both; k < rp; k++) {
            const uint32_t *sa;
            uint32_t na;
            dgroup_seg(&gp, i0 + k, &sa, &na);
            out->changed += na;
            add_group_all(&c, prev, sa, na);
        }
        for (uint32_t k = both; k < rc; k++) {
            const uint32_t *sb;
            uint32_t nb;
            dgroup_seg(&gc, j0 + k, &sb, &nb);
            out->changed += nb;
            add_group_all(&c, cur, sb, nb);
        }
        if (i < np && j < nc) { /* an anchor: two identical contexts */
            i++;
            j++;
        } else if (i == i0 && j == j0) {
            break;
        }
    }
    const uint32_t *sa, *sb;
    uint32_t na, nb;
    dgroup_seg(&gp, np, &sa, &na);
    dgroup_seg(&gc, nc, &sb, &nb);
    diff_group(&c, &sc, prev, hp.data, sa, na, cur, hc.data, sb, nb);
    embi_vec_free(&hp);
    embi_vec_free(&hc);
    embi_vec_free(&sc.ha);
    embi_vec_free(&sc.hb);
    embi_vec_free(&sc.sp);
    embi_vec_free(&sc.sc);
    embi_vec_free(&sc.ma);
    embi_vec_free(&sc.mb);
    embi_vec_free(&sc.cp);
    embi_vec_free(&sc.cc);
    dgroups_free(&gp);
    dgroups_free(&gc);
    finish(out, vw, vh);
    /* Filters and backdrop filters (R3a) read pixels beyond the item that changed (a blur spreads, a backdrop depends on
     * everything under it), so any change on a page that has them redraws the whole view. Pages without them are
     * unaffected. */
    if (out->any && !out->full && (prev->fx_items > 0 || cur->fx_items > 0)) {
        out->full = true;
        out->rects.len = 0;
        out->area = view_area;
    }
}

void embi_damage_add_images(embi_damage *d, const embi_dl *cur, float scale, uint32_t view_w, uint32_t view_h,
                            bool (*match)(void *user, embi_str url), void *user)
{
    if (d->full || cur == nullptr || match == nullptr) {
        return;
    }
    dctx c = {d, scale, (int32_t)view_w, (int32_t)view_h};
    uint32_t before = d->rects.len;
    for (uint32_t i = 0; i < cur->items.len; i++) {
        const embi_ditem *it = &cur->items.data[i];
        if (it->kind == EMBI_DI_IMAGE && match(user, embi_dl_url(cur, it->aux))) {
            add_item(&c, cur, i);
            d->changed++;
        }
    }
    if (d->rects.len != before) {
        finish(d, (int32_t)view_w, (int32_t)view_h);
    }
}
