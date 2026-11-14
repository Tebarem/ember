/*
 * Incremental display list (W6b): after an update that changed only paint properties (the
 * layout kept its fragments, EMBI_LU_PAINT) the stacking contexts that hold the changed elements are built again and
 * spliced into the previous list; the rest of the list is reused as it is.
 *
 * The cache keeps a master list with, for each stacking context, what paint_ctx was called with (paint_int.h
 * embi_pctx_rec). A context is built again into a scratch list that shares the master's tables (clips, transforms, stops,
 * runs, urls: they only grow, an item names an entry by index) and has its own items and contexts, which then replace the
 * context's range: the items after it move, the layer links and the context table are adjusted. The tables are only
 * appended to, so the unreferenced entries of the replaced range stay until the next full build (which comes when the
 * tables have doubled).
 *
 * Anything this cannot do exactly falls back to a full build of the list: a layout that rebuilt its fragments (the
 * serial of embi_layout_paint_dirty), too many dirty elements, the root or body among them (the canvas background), a
 * context that turns empty or non-empty (the layers of its ancestors depend on it), form controls (their runs are owned by
 * the list). With EMBER_VERIFY_INCREMENTAL every list is compared with a full build (embi_dl_equal).
 */
#include "paint/paint_int.h"
#include "paint/scroll.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

struct embi_paint_cache {
    embi_dl master;
    bool valid;
    embi_pctx_vec rec;
    const embi_document *doc;
    uint64_t serial;
    uint32_t base_clips, base_xforms, base_stops, base_runs, base_urls, base_shadows, base_paths;
    float cfg_vw, cfg_vh, cfg_scale;  /* scroll60: what the master was built for */
    uint32_t cfg_focus;
    bool painted;                     /* the last call produced a list for c->doc at c->serial (also when the master is not kept) */
    uint32_t rebuilt;     /* contexts rebuilt by the last call */
    bool last_incr;
    uint64_t n_full, n_incr;
    embi_hashmap ctxmap;  /* fragment -> context index */
    embi_hashmap dirty;   /* element -> 1 */
};

embi_paint_cache *embi_paint_cache_create(void)
{
    embi_paint_cache *c = embi_calloc(1, sizeof *c);
    embi_dl_init(&c->master);
    return c;
}

void embi_paint_cache_free(embi_paint_cache *c)
{
    if (c == nullptr) {
        return;
    }
    embi_dl_free(&c->master);
    embi_vec_free(&c->rec);
    embi_hashmap_free(&c->ctxmap);
    embi_hashmap_free(&c->dirty);
    embi_free(c, sizeof *c);
}

uint32_t embi_paint_cache_rebuilt(const embi_paint_cache *c)
{
    return c->rebuilt;
}

bool embi_paint_cache_last_incremental(const embi_paint_cache *c)
{
    return c->last_incr;
}

uint64_t embi_paint_cache_count(const embi_paint_cache *c, bool incremental)
{
    return incremental ? c->n_incr : c->n_full;
}

/* ------------------------------------------------------------------ equality */

static bool same_f(const float *a, const float *b, size_t n)
{
    return memcmp(a, b, n * sizeof *a) == 0;
}

static bool xform_same(const embi_dl *a, uint32_t ia, const embi_dl *b, uint32_t ib)
{
    if (ia >= a->xforms.len || ib >= b->xforms.len) {
        return ia == ib;
    }
    return same_f(a->xforms.data[ia].m, b->xforms.data[ib].m, 6);
}

static bool clip_same(const embi_dl *a, uint32_t ia, const embi_dl *b, uint32_t ib)
{
    while (ia != 0 || ib != 0) {
        if (ia == 0 || ib == 0 || ia >= a->clips.len || ib >= b->clips.len) {
            return false;
        }
        const embi_dclip *x = &a->clips.data[ia], *y = &b->clips.data[ib];
        if (!same_f(x->rect, y->rect, 4) || !same_f(x->radii, y->radii, 4) || !xform_same(a, x->xform, b, y->xform)) {
            return false;
        }
        ia = x->parent;
        ib = y->parent;
    }
    return true;
}

static bool item_same(const embi_dl *a, const embi_ditem *x, const embi_dl *b, const embi_ditem *y)
{
    if (x->kind != y->kind || x->sub != y->sub || x->flags != y->flags || memcmp(x->styles, y->styles, 4) != 0 ||
        !same_f(x->rect, y->rect, 4) || !same_f(x->radii, y->radii, 4) || !same_f(x->widths, y->widths, 4) ||
        memcmp(x->colors, y->colors, sizeof x->colors) != 0 || !same_f(x->f, y->f, 6)) {
        return false;
    }
    if (!xform_same(a, x->xform, b, y->xform) || !clip_same(a, x->clip, b, y->clip)) {
        return false;
    }
    switch (x->kind) {
    case EMBI_DI_TEXT: {
        const void *ra = x->aux < a->runs.len ? a->runs.data[x->aux] : nullptr;
        const void *rb = y->aux < b->runs.len ? b->runs.data[y->aux] : nullptr;
        return ra == rb && x->aux2 == y->aux2 && x->aux3 == y->aux3;
    }
    case EMBI_DI_IMAGE: {
        embi_str ua = embi_dl_url(a, x->aux), ub = embi_dl_url(b, y->aux);
        return ua.len == ub.len && (ua.len == 0 || memcmp(ua.ptr, ub.ptr, ua.len) == 0) && x->aux2 == y->aux2 &&
               x->aux3 == y->aux3;
    }
    case EMBI_DI_GRADIENT: {
        if (x->aux2 != y->aux2 || x->aux3 != y->aux3 || x->aux + x->aux2 > a->stops.len || y->aux + y->aux2 > b->stops.len) {
            return false;
        }
        for (uint32_t i = 0; i < x->aux2; i++) {
            const embi_dstop *s = &a->stops.data[x->aux + i], *t = &b->stops.data[y->aux + i];
            if (s->offset != t->offset || s->color != t->color) {
                return false;
            }
        }
        return true;
    }
    case EMBI_DI_PATH: {
        /* R4c: the path table of each list holds its own copies; the content hash tells whether they are the same */
        if (x->aux >= a->paths.len || y->aux >= b->paths.len) {
            return x->aux >= a->paths.len && y->aux >= b->paths.len;
        }
        if (a->paths.data[x->aux].hash != b->paths.data[y->aux].hash || x->aux2 != y->aux2 || x->aux3 != y->aux3 ||
            x->aux2 + x->aux3 > a->stops.len || y->aux2 + y->aux3 > b->stops.len) {
            return false;
        }
        for (uint32_t i = 0; i < x->aux3; i++) {
            const embi_dstop *s = &a->stops.data[x->aux2 + i], *t = &b->stops.data[y->aux2 + i];
            if (s->offset != t->offset || s->color != t->color) {
                return false;
            }
        }
        return true;
    }
    case EMBI_DI_SHADOW: {
        /* R3a: the shadow table is shared and appended to, so the same shadow may sit at another index */
        if (x->aux >= a->shadows.len || y->aux >= b->shadows.len) {
            return x->aux >= a->shadows.len && y->aux >= b->shadows.len;
        }
        const embi_dshadow *s = &a->shadows.data[x->aux], *t = &b->shadows.data[y->aux];
        return same_f(s->shape, t->shape, 4) && same_f(s->shape_radii, t->shape_radii, 4) && same_f(s->clip, t->clip, 4) &&
               same_f(s->clip_radii, t->clip_radii, 4) && same_f(&s->sigma, &t->sigma, 1);
    }
    default:
        return x->aux == y->aux && x->aux2 == y->aux2 && x->aux3 == y->aux3;
    }
}

bool embi_dl_equal(const embi_dl *a, const embi_dl *b)
{
    if (a->items.len != b->items.len || a->ctxs.len != b->ctxs.len) {
        return false;
    }
    for (uint32_t i = 0; i < a->ctxs.len; i++) {
        const embi_dctx *x = &a->ctxs.data[i], *y = &b->ctxs.data[i];
        if (x->first != y->first || x->end != y->end || x->parent != y->parent) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a->items.len; i++) {
        if (!item_same(a, &a->items.data[i], b, &b->items.data[i])) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ copies */

#define COPY_VEC(dst, src, f)                                                                                   \
    do {                                                                                                        \
        embi_vec_reserve(&(dst)->f, (src)->f.len);                                                              \
        if ((src)->f.len > 0) {                                                                                 \
            memcpy((dst)->f.data, (src)->f.data, (size_t)(src)->f.len * sizeof((src)->f.data[0]));              \
        }                                                                                                       \
        (dst)->f.len = (src)->f.len;                                                                            \
    } while (0)

static void dl_copy(embi_dl *dst, const embi_dl *src)
{
    embi_dl_reset(dst);
    COPY_VEC(dst, src, items);
    COPY_VEC(dst, src, clips);
    COPY_VEC(dst, src, xforms);
    COPY_VEC(dst, src, scrolls);
    dst->scroll_rigid = src->scroll_rigid;
    COPY_VEC(dst, src, stops);
    COPY_VEC(dst, src, ctxs);
    COPY_VEC(dst, src, runs);
    COPY_VEC(dst, src, urls);
    COPY_VEC(dst, src, urltext);
    COPY_VEC(dst, src, shadows); /* R3a */
    COPY_VEC(dst, src, fx);
    dst->fx_items = src->fx_items;
    COPY_VEC(dst, src, planes); /* R3c */
    COPY_VEC(dst, src, cpaths); /* R3b */
    COPY_VEC(dst, src, curves);
    embi_dl_copy_paths(dst, src); /* R4c: the copy shares the paths */
}

/* ------------------------------------------------------------------ the full build */

static bool full_build(embi_paint_cache *c, const embi_layout_engine *e, const embi_document *doc, const embi_paint_cfg *cfg,
                       uint64_t serial)
{
    embi_dl *m = &c->master;
    embi_dl_reset(m);
    embi_vec_clear(&c->rec);
    c->doc = doc;
    c->serial = serial;
    c->valid = false;
    c->painted = false;
    c->cfg_vw = cfg != nullptr ? cfg->viewport_w : 0.0f;
    c->cfg_vh = cfg != nullptr ? cfg->viewport_h : 0.0f;
    c->cfg_scale = cfg != nullptr ? cfg->scale : 0.0f;
    c->cfg_focus = cfg != nullptr ? cfg->focus_rgba : 0u;
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.rec = &c->rec;
    uint32_t vclip;
    if (!embi_paint_begin(&p, m, e, doc, cfg, true, &vclip)) {
        return false;
    }
    embi_paint_reenter(&p, p.root, 0.0f, 0.0f, 0, vclip, 0);
    embi_pb_control_popup(&p); /* W5e: the open select's list over everything, as embi_paint_document does */
    bool popup = p.popup;
    embi_paint_end(&p);
    c->base_clips = m->clips.len;
    c->base_xforms = m->xforms.len;
    c->base_stops = m->stops.len;
    c->base_runs = m->runs.len;
    c->base_urls = m->urls.len;
    c->base_shadows = m->shadows.len;
    c->base_paths = m->paths.len;
    /* An open popup sits at the end of the list: no context is rebuilt in place. A canvas background inside the root group
     * (R3b) is rebuilt with the whole list. */
    c->valid = m->own_runs.len == 0 && !popup && !p.bg_deferred;
    return true;
}

/* ------------------------------------------------------------------ rebuilding contexts */

typedef struct dirty_walk {
    EMBI_VEC(uint32_t) stack;   /* the context of each open fragment */
    EMBI_VEC(uint32_t) found;   /* context indices with a dirty element */
} dirty_walk;

/* The contexts that hold a fragment of a dirty element: the nearest context root at or above the fragment. */
static void find_dirty_ctxs(embi_paint_cache *c, const embi_frag *root, dirty_walk *w)
{
    const embi_frag *f = root;
    while (f != nullptr) {
        uint64_t v = 0;
        uint32_t ctx = w->stack.len > 0 ? w->stack.data[w->stack.len - 1] : 0;
        if (embi_hashmap_get(&c->ctxmap, (uint64_t)(uintptr_t)f, &v)) {
            ctx = (uint32_t)v;
        }
        /* The element the fragment paints for: the nearest element of its box and the boxes above it. A text run belongs to
         * the inline box around it (the line fragment that holds it belongs to the block), so the fragments are not asked. */
        const embi_element *el = nullptr;
        for (const embi_box *bx = f->box; bx != nullptr; bx = bx->parent) {
            if (bx->el != nullptr) {
                el = bx->el;
                break;
            }
        }
        if (el != nullptr && embi_hashmap_get(&c->dirty, (uint64_t)(uintptr_t)el, &v)) {
            embi_vec_push(&w->found, ctx);
        }
        const embi_frag *first = embi_frag_first(f);
        if (first != nullptr) {
            embi_vec_push(&w->stack, ctx);
            f = first;
            continue;
        }
        for (;;) {
            if (f == root) {
                f = nullptr;
                break;
            }
            if (f->next != nullptr) {
                f = f->next;
                break;
            }
            f = f->parent;
            (void)embi_vec_pop(&w->stack);
        }
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Builds context ci again and replaces its range. False (the master is then useless: the caller builds it again) when
 * the context cannot be replaced in place. */
static bool rebuild_ctx(embi_paint_cache *c, const embi_layout_engine *e, const embi_document *doc, const embi_paint_cfg *cfg,
                        uint32_t ci)
{
    embi_dl *m = &c->master;
    embi_pctx_rec r = c->rec.data[ci];
    uint32_t sub_end = r.sub_end;
    uint32_t i0 = m->ctxs.data[ci].first, i1 = m->ctxs.data[ci].end;

    embi_dl s = *m;
    memset(&s.items, 0, sizeof s.items);
    memset(&s.ctxs, 0, sizeof s.ctxs);
    embi_pctx_vec srec = {0};
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.rec = &srec;
    uint32_t vclip;
    bool ok = embi_paint_begin(&p, &s, e, doc, cfg, false, &vclip);
    if (ok) {
        embi_paint_reenter(&p, r.f, r.kx, r.ky, r.xf_parent, r.clip_parent, 0);
        embi_paint_end(&p);
    }
    /* the tables grew in the scratch list: the master takes them back */
    m->clips = s.clips;
    m->xforms = s.xforms;
    m->scrolls = s.scrolls; /* scroll60: containers inside the rebuilt context are added (the old entries stay, unreferenced) */
    m->scroll_rigid = s.scroll_rigid;
    m->stops = s.stops;
    m->runs = s.runs;
    m->own_runs = s.own_runs;
    m->urls = s.urls;
    m->urltext = s.urltext;
    m->shadows = s.shadows; /* R3a */
    m->fx = s.fx;
    m->cpaths = s.cpaths; /* R3b (a list with composite groups is never rebuilt in part; fx_items says so) */
    m->curves = s.curves;
    m->planes = s.planes; /* R3c (a list with planes is never rebuilt in part; the table is still the scratch's) */
    m->paths = s.paths;   /* R4c: the path table is shared like the others; the replaced range's paths stay until the next full build */
    uint32_t new_n = s.items.len, old_n = i1 - i0;
    uint32_t ncx = s.ctxs.len, ocx = sub_end - ci;
    if (!ok || m->own_runs.len != 0 || ncx == 0 || srec.len != ncx || (new_n == 0) != (old_n == 0)) {
        embi_vec_free(&s.items);
        embi_vec_free(&s.ctxs);
        embi_vec_free(&srec);
        return false;
    }
    int64_t idelta = (int64_t)new_n - (int64_t)old_n;
    int64_t cdelta = (int64_t)ncx - (int64_t)ocx;

    /* the layer links of the items outside the range that point past it */
    for (uint32_t k = 0; k < m->items.len; k++) {
        if (k >= i0 && k < i1) {
            k = i1 - 1;
            continue;
        }
        embi_ditem *it = &m->items.data[k];
        if ((it->kind == EMBI_DI_LAYER_BEGIN || it->kind == EMBI_DI_LAYER_END) && it->aux >= i1) {
            it->aux = (uint32_t)((int64_t)it->aux + idelta);
        }
    }
    for (uint32_t k = 0; k < new_n; k++) {
        embi_ditem *it = &s.items.data[k];
        if (it->kind == EMBI_DI_LAYER_BEGIN || it->kind == EMBI_DI_LAYER_END) {
            it->aux += i0;
        }
    }
    /* the items */
    uint32_t tail = m->items.len - i1;
    embi_vec_reserve(&m->items, (uint32_t)((int64_t)m->items.len + (idelta > 0 ? idelta : 0)));
    if (tail > 0) {
        memmove(&m->items.data[i0 + new_n], &m->items.data[i1], (size_t)tail * sizeof(embi_ditem));
    }
    if (new_n > 0) {
        memcpy(&m->items.data[i0], s.items.data, (size_t)new_n * sizeof(embi_ditem));
    }
    m->items.len = (uint32_t)((int64_t)m->items.len + idelta);

    /* the contexts: the ancestors' ends, the contexts after, the replaced entries */
    for (uint32_t k = 0; k < ci; k++) {
        if (c->rec.data[k].sub_end > ci) {
            m->ctxs.data[k].end = (uint32_t)((int64_t)m->ctxs.data[k].end + idelta);
            c->rec.data[k].sub_end = (uint32_t)((int64_t)c->rec.data[k].sub_end + cdelta);
        }
    }
    for (uint32_t k = sub_end; k < m->ctxs.len; k++) {
        embi_dctx *x = &m->ctxs.data[k];
        x->first = (uint32_t)((int64_t)x->first + idelta);
        x->end = (uint32_t)((int64_t)x->end + idelta);
        embi_pctx_rec *q = &c->rec.data[k];
        if (x->parent >= sub_end) {
            x->parent = (uint32_t)((int64_t)x->parent + cdelta);
        }
        if (q->parent_ctx >= sub_end) {
            q->parent_ctx = (uint32_t)((int64_t)q->parent_ctx + cdelta);
        }
        q->sub_end = (uint32_t)((int64_t)q->sub_end + cdelta);
    }
    uint32_t ctail = m->ctxs.len - sub_end;
    embi_vec_reserve(&m->ctxs, (uint32_t)((int64_t)m->ctxs.len + (cdelta > 0 ? cdelta : 0)));
    embi_vec_reserve(&c->rec, (uint32_t)((int64_t)c->rec.len + (cdelta > 0 ? cdelta : 0)));
    if (ctail > 0) {
        memmove(&m->ctxs.data[ci + ncx], &m->ctxs.data[sub_end], (size_t)ctail * sizeof(embi_dctx));
        memmove(&c->rec.data[ci + ncx], &c->rec.data[sub_end], (size_t)ctail * sizeof(embi_pctx_rec));
    }
    for (uint32_t k = 0; k < ncx; k++) {
        embi_dctx x = s.ctxs.data[k];
        x.first += i0;
        x.end += i0;
        x.parent = k == 0 ? r.parent_ctx : x.parent + ci;
        m->ctxs.data[ci + k] = x;
        embi_pctx_rec q = srec.data[k];
        q.parent_ctx = k == 0 ? r.parent_ctx : q.parent_ctx + ci;
        q.sub_end += ci;
        c->rec.data[ci + k] = q;
    }
    m->ctxs.len = (uint32_t)((int64_t)m->ctxs.len + cdelta);
    c->rec.len = m->ctxs.len;
    embi_vec_free(&s.items);
    embi_vec_free(&s.ctxs);
    embi_vec_free(&srec);
    return true;
}

static bool incremental(embi_paint_cache *c, const embi_layout_engine *e, const embi_document *doc, const embi_paint_cfg *cfg,
                        embi_element *const *els, uint32_t nels)
{
    embi_dl *m = &c->master;
    if (m->clips.len > 2u * c->base_clips + 256u || m->xforms.len > 2u * c->base_xforms + 256u ||
        m->stops.len > 2u * c->base_stops + 1024u || m->runs.len > 2u * c->base_runs + 1024u ||
        m->urls.len > 2u * c->base_urls + 256u || m->paths.len > 2u * c->base_paths + 256u || m->shadows.len > 2u * c->base_shadows + 256u || m->ctxs.len != c->rec.len ||
        m->planes.len != 0 /* R3c: contexts painted inside 3D planes are not re-entered alone */ ||
        m->fx_items != 0 /* R3a: filter groups and backdrops are rebuilt with the whole list */) {
        return false;
    }
    c->rebuilt = 0;
    /* The offsets the master was built for are those of the fragments now (a scroll goes through embi_paint_document_scroll). */
    for (uint32_t i = 0; i < m->scrolls.len; i++) {
        const embi_dscroll *n = &m->scrolls.data[i];
        const embi_frag *f = (const embi_frag *)(uintptr_t)n->key;
        if (embi_scroll_quantize(embi_frag_sx(f), c->cfg_scale) != n->sx ||
            embi_scroll_quantize(embi_frag_sy(f), c->cfg_scale) != n->sy) {
            return false;
        }
    }
    if (nels == 0) {
        return true;
    }
    embi_hashmap_clear(&c->dirty);
    for (uint32_t i = 0; i < nels; i++) {
        const embi_element *el = els[i];
        if (embi_pb_named(el, "html") || embi_pb_named(el, "body")) {
            return false; /* the canvas background may come from them */
        }
        (void)embi_hashmap_put(&c->dirty, (uint64_t)(uintptr_t)el, 1);
    }
    const embi_frag *root = embi_layout_root(e, doc);
    if (root == nullptr || c->rec.len == 0 || c->rec.data[0].f != root) {
        return false;
    }
    embi_hashmap_clear(&c->ctxmap);
    for (uint32_t k = 0; k < c->rec.len; k++) {
        (void)embi_hashmap_put(&c->ctxmap, (uint64_t)(uintptr_t)c->rec.data[k].f, k);
    }
    dirty_walk w;
    memset(&w, 0, sizeof w);
    find_dirty_ctxs(c, root, &w);
    bool ok = true;
    if (w.found.len > 0) {
        qsort(w.found.data, w.found.len, sizeof(uint32_t), cmp_u32);
        /* the outermost of the dirty contexts only: a context rebuilds its nested ones */
        EMBI_VEC(uint32_t) todo = {0};
        uint32_t reach = 0;
        for (uint32_t i = 0; i < w.found.len; i++) {
            uint32_t ci = w.found.data[i];
            if (todo.len > 0 && ci < reach) {
                continue;
            }
            embi_vec_push(&todo, ci);
            reach = c->rec.data[ci].sub_end;
        }
        /* from the last to the first, so that a rebuilt context does not move the indices still to do */
        for (uint32_t i = todo.len; i > 0 && ok; i--) {
            ok = rebuild_ctx(c, e, doc, cfg, todo.data[i - 1]);
            c->rebuilt++;
        }
        embi_vec_free(&todo);
    }
    embi_vec_free(&w.stack);
    embi_vec_free(&w.found);
    return ok;
}

int embi_paint_document_scroll(embi_paint_cache *c, embi_dl *list, uint32_t content_len, embi_layout_engine *e,
                               const embi_document *doc, const embi_paint_cfg *cfg, embi_scroll_damage *dmg)
{
    uint64_t serial = 0;
    embi_element *const *els = nullptr;
    uint32_t nels = 0;
    bool lay_ok = embi_layout_paint_dirty(e, doc, &serial, &els, &nels);
    if (!lay_ok || nels != 0 || !c->painted || c->doc != doc || c->serial != serial || cfg == nullptr ||
        cfg->viewport_w != c->cfg_vw || cfg->viewport_h != c->cfg_vh || cfg->scale != c->cfg_scale ||
        cfg->focus_rgba != c->cfg_focus || content_len > list->items.len) {
        return 0;
    }
    uint32_t n = list->scrolls.len;
    float *off = SDL_malloc(((size_t)n * 2 + 2) * sizeof *off);
    if (off == nullptr) {
        return 0;
    }
    bool moved = false;
    for (uint32_t i = 0; i < n; i++) {
        const embi_frag *f = (const embi_frag *)(uintptr_t)list->scrolls.data[i].key;
        off[2 * i] = embi_scroll_quantize(embi_frag_sx(f), c->cfg_scale);
        off[2 * i + 1] = embi_scroll_quantize(embi_frag_sy(f), c->cfg_scale);
        moved = moved || off[2 * i] != list->scrolls.data[i].sx || off[2 * i + 1] != list->scrolls.data[i].sy;
    }
    int r = 1;
    if (moved) {
        r = embi_dl_scroll_retarget(list, off, dmg) ? 2 : 0;
        if (r == 2 && c->valid && c->master.scrolls.len == n) {
            /* the cached master follows, so that a later incremental paint builds on the offsets that are on screen */
            if (!embi_dl_scroll_retarget(&c->master, off, nullptr)) {
                c->valid = false;
            }
        }
        if (r == 2) {
            list->items.len = content_len; /* the scrollbars after the content are made again by the caller */
        }
    }
    SDL_free(off);
    return r;
}

void embi_paint_document_incr(embi_paint_cache *c, embi_dl *out, embi_layout_engine *e, const embi_document *doc,
                              const embi_paint_cfg *cfg, bool allow)
{
    uint64_t serial = 0;
    embi_element *const *els = nullptr;
    uint32_t nels = 0;
    bool lay_ok = embi_layout_paint_dirty(e, doc, &serial, &els, &nels);
    bool used = false;
    c->rebuilt = 0;
    if (allow && lay_ok && c->valid && c->doc == doc && c->serial == serial) {
        used = incremental(c, e, doc, cfg, els, nels);
    }
    if (!used) {
        c->rebuilt = 0;
        if (!full_build(c, e, doc, cfg, serial)) {
            embi_dl_reset(out);
            embi_dl_reset(&c->master);
            c->valid = false;
            embi_layout_paint_clean(e, doc);
            c->last_incr = false;
            return;
        }
        c->n_full++;
    } else {
        c->n_incr++;
    }
    c->last_incr = used;
    c->painted = true;
    c->doc = doc;
    c->serial = serial;
    embi_layout_paint_clean(e, doc);
    if (c->master.own_runs.len > 0) {
        /* the list owns runs (form controls): it cannot be copied, nor reused */
        embi_dl tmp = *out;
        *out = c->master;
        c->master = tmp;
        embi_dl_reset(&c->master);
        c->valid = false;
        return;
    }
    dl_copy(out, &c->master);
    if (used && embi_incr_verify_mode() != 0) {
        embi_dl fresh;
        memset(&fresh, 0, sizeof fresh);
        embi_paint_document(&fresh, e, doc, cfg);
        if (fresh.own_runs.len == 0 && !embi_dl_equal(out, &fresh)) {
            embi_incr_verify_fail("paint: an incrementally rebuilt display list differs from a full build");
        }
        embi_dl_free(&fresh);
    }
}
