/*
 * The layout driver (W3a, section 2): the engine, the per-document state, and the layout of the
 * initial containing block. The engine holds the text system, the style engine and the viewport.
 * Each document has two arenas (boxes, fragments) and the list of its shaped runs. A layout resets
 * them. The fragment arena is reset by every layout; the box tree is kept while nothing it was built from
 * changed (W3p, layout_run), so the state of one layout never outlives the next one except for that.
 */
#include "layout_int.h"

#include "css/props.h"

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <string.h>

static void doc_destroy(embi_lay_doc *ld);

static uint64_t doc_key(const embi_document *doc)
{
    return (uint64_t)(uintptr_t)doc;
}

void *embi_lay_pool_alloc(embi_lay_pool *p, size_t sz)
{
    if (sz > EMBI_POOL_MAX) {
        return embi_calloc(1, sz);
    }
    size_t cls = (sz + EMBI_POOL_GRAIN - 1u) / EMBI_POOL_GRAIN;
    size_t rounded = cls * EMBI_POOL_GRAIN;
    void *r = p->freel[cls];
    if (r != nullptr) {
        p->freel[cls] = *(void **)r;
#ifdef EMBI_LAY_ASAN
        ASAN_UNPOISON_MEMORY_REGION(r, rounded);
#endif
        memset(r, 0, rounded);
        return r;
    }
    if (p->left < rounded) {
        embi_vec_reserve(&p->slabs, p->slabs.len + 1u);
        uint8_t *slab = p->slabs.len < p->slabs.cap ? embi_calloc(1, EMBI_POOL_SLAB) : nullptr;
        if (slab == nullptr) {
            return nullptr;
        }
        p->slabs.data[p->slabs.len++] = slab;
        p->bytes += EMBI_POOL_SLAB;
        p->cur = slab;
        p->left = EMBI_POOL_SLAB; /* the tail of the old slab is left unused */
    }
    r = p->cur;
    p->cur += rounded;
    p->left -= rounded;
    return r;
}

void embi_lay_pool_free(embi_lay_pool *p, void *e, size_t sz)
{
    if (e == nullptr) {
        return;
    }
    if (sz > EMBI_POOL_MAX) {
        embi_free(e, sz);
        return;
    }
    size_t cls = (sz + EMBI_POOL_GRAIN - 1u) / EMBI_POOL_GRAIN;
    *(void **)e = p->freel[cls];
    p->freel[cls] = e;
#ifdef EMBI_LAY_ASAN
    ASAN_POISON_MEMORY_REGION((char *)e + sizeof(void *), cls * EMBI_POOL_GRAIN - sizeof(void *));
#endif
}

void embi_lay_pool_destroy(embi_lay_pool *p)
{
    for (uint32_t i = 0; i < p->slabs.len; i++) {
#ifdef EMBI_LAY_ASAN
        ASAN_UNPOISON_MEMORY_REGION(p->slabs.data[i], EMBI_POOL_SLAB);
#endif
        embi_free(p->slabs.data[i], EMBI_POOL_SLAB);
    }
    embi_vec_free(&p->slabs);
    memset(p, 0, sizeof *p);
}

static embi_lay_doc *doc_state(embi_layout_engine *e, const embi_document *doc, bool create)
{
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return (embi_lay_doc *)(uintptr_t)v;
    }
    if (!create) {
        return nullptr;
    }
    embi_lay_doc *ld = embi_calloc(1, sizeof *ld);
    if (ld == nullptr) {
        return nullptr;
    }
    ld->doc = (embi_document *)(uintptr_t)doc;
    embi_arena_init(&ld->ba, 64 * 1024);
    embi_arena_init(&ld->fa, 64 * 1024);
    embi_hashmap_init(&ld->tkeys);
    embi_arena_init(&ld->ta, 64 * 1024);
    embi_hashmap_init(&ld->langs);
    embi_hashmap_init(&ld->pmap);
    embi_hashmap_init(&ld->ecache);
    embi_arena_init(&ld->pa, 64 * 1024);
    embi_hashmap_init(&ld->prmap);
    embi_hashmap_init(&ld->reusedmap);
    embi_hashmap_init(&ld->rebanc);
    embi_arena_init(&ld->ra, 16 * 1024);
    ld->scratch = e->scratch;
    if (!embi_hashmap_put(e->docs, doc_key(doc), (uint64_t)(uintptr_t)ld)) {
        doc_destroy(ld);
        return nullptr;
    }
    return ld;
}

void embi_lay_rewind(embi_arena *a)
{
    for (embi_arena_block *b = a->first; b != nullptr; b = b->next) {
        b->used = 0;
    }
    a->last = a->first;
}

static void *lay_bump(embi_arena_block *b, size_t size, size_t align)
{
    uintptr_t base = (uintptr_t)b->data;
    uintptr_t aligned = (base + b->used + align - 1) & ~(uintptr_t)(align - 1);
    size_t off = (size_t)(aligned - base);
    if (off > b->cap || size > b->cap - off) {
        return nullptr;
    }
    b->used = off + size;
    return b->data + off;
}

#ifdef EMBI_LAY_ASAN
#define LAY_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define LAY_UNPOISON(p, n) ((void)0)
#endif

static void *embi_lay_alloc_raw(embi_arena *a, size_t size, size_t align);

/* Sanitizer builds poison the tail of text boxes in the box arena (box.c): the blocks are reused, so the poison
 * goes with the tree. */
static void lay_unpoison_arena(embi_arena *a)
{
#ifdef EMBI_LAY_ASAN
    for (embi_arena_block *b = a->first; b != nullptr; b = b->next) {
        ASAN_UNPOISON_MEMORY_REGION(b->data, b->cap);
    }
#else
    (void)a;
#endif
}

void *embi_lay_alloc(embi_arena *a, size_t size, size_t align)
{
    void *p = embi_lay_alloc_raw(a, size, align);
    if (p != nullptr) {
        LAY_UNPOISON(p, size);
    }
    return p;
}

static void *embi_lay_alloc_raw(embi_arena *a, size_t size, size_t align)
{
    embi_arena_block *b = a->last;
    if (b != nullptr) {
        void *p = lay_bump(b, size, align);
        if (p != nullptr) {
            return p;
        }
        /* Retained blocks after the tail (a rewound arena) are used before the heap is asked. */
        while (b->next != nullptr) {
            b = b->next;
            a->last = b;
            p = lay_bump(b, size, align);
            if (p != nullptr) {
                return p;
            }
        }
    }
    return embi_arena_alloc(a, size, align);
}

/* Forgets the box tree: the element slots, the arena and what points into it. */
static void doc_drop_tree(embi_lay_doc *ld)
{
    if (ld->root_box != nullptr && ld->doc != nullptr) {
        embi_lay_clear_boxes(ld->doc);
    }
    ld->root_box = nullptr;
    ld->has_images = false;
    ld->has_sticky = false;
    ld->ctr_used = false; /* W6b: what the next build finds (incr.c) */
    ld->ctr_read = false;
    ld->has_contents = false;
    ld->has_fl = false;
    ld->surgeries = 0;
    ld->has_xf = false;
    /* Emptied, not freed: the next tree fills them to about the same size, and growing a map from nothing
     * rehashes it a dozen times. */
    embi_hashmap_clear(&ld->tkeys);
    embi_arena_reset(&ld->ta);
    embi_hashmap_clear(&ld->langs);
    if (ld->pfront != nullptr) {
        memset(ld->pfront, 0, (size_t)ld->pfront_n * sizeof *ld->pfront);
    }
    ld->pfront_cnt = 0;
    embi_hashmap_clear(&ld->ecache);
    memset(ld->efront, 0, sizeof ld->efront);
    memset(ld->tkfront, 0, sizeof ld->tkfront);
    if (ld->stab != nullptr) {
        memset(ld->stab, 0, (size_t)ld->stab_n * sizeof *ld->stab);
    }
    ld->stab_cnt = 0;
    memset(ld->lfront, 0, sizeof ld->lfront);
    embi_arena_reset(&ld->pa);
    lay_unpoison_arena(&ld->ba);
    embi_arena_reset(&ld->ba);
}

/* Resets the fragment arena (every layout). The box arena stays while the tree is kept (W3p), and so do
 * the shaped runs: the layout that follows reuses the ones it needs and, when it rebuilt the tree,
 * drops the rest when it ends (embi_lay_tcache_sweep). */
static void doc_reset(embi_lay_doc *ld)
{
    embi_lay_rewind(&ld->fa);
    ld->free_frag = nullptr;
    ld->free_ext = nullptr;
    memset(ld->mtab, 0, sizeof ld->mtab);
    ld->icb = nullptr;
    ld->tgen++;
    embi_lay_incr_records_reset(ld); /* W6b: the fragments they describe are gone */
}

static void doc_destroy(embi_lay_doc *ld)
{
    doc_reset(ld);
    ld->root_box = nullptr;
    embi_lay_ifc_pool_free(ld);
    embi_hashmap_free(&ld->tkeys);
    embi_arena_free(&ld->ta);
    embi_hashmap_free(&ld->langs);
    embi_free(ld->pfront, (size_t)ld->pfront_n * sizeof *ld->pfront);
    ld->pfront = nullptr;
    ld->pfront_n = 0;
    embi_hashmap_free(&ld->ecache);
    embi_arena_free(&ld->pa);
    embi_lay_tcache_sweep(ld, true);
    embi_lay_pool_destroy(&ld->pool);
    embi_free(ld->tmemo, (size_t)ld->tmemo_n * sizeof *ld->tmemo);
    embi_free(ld->stab, (size_t)ld->stab_n * sizeof *ld->stab);
    embi_hashmap_free(&ld->pmap);
    embi_vec_free(&ld->preps);
    embi_vec_free(&ld->tents);
    embi_vec_free(&ld->pdirty);
    embi_vec_free(&ld->reb);
    lay_unpoison_arena(&ld->ba);
    embi_arena_free(&ld->ba);
    embi_arena_free(&ld->fa);
    embi_lay_incr_records_free(ld);
    embi_free(ld, sizeof *ld);
}

embi_layout_engine *embi_layout_engine_create(embi_text *text, embi_style_engine *style,
                                              const embi_layout_config *cfg)
{
    embi_layout_engine *e = embi_calloc(1, sizeof *e);
    if (e == nullptr) {
        return nullptr;
    }
    e->docs = embi_calloc(1, sizeof *e->docs);
    if (e->docs == nullptr) {
        embi_free(e, sizeof *e);
        return nullptr;
    }
    e->text = text;
    e->style = style;
    if (cfg != nullptr) {
        e->image_user = cfg->user;
        e->image_size = cfg->image_size;
        e->scrollbar = cfg->scrollbar_thickness;
    }
    e->vw = 800;
    e->vh = 600;
    embi_hashmap_init(e->docs);
    return e;
}

void embi_layout_engine_destroy(embi_layout_engine *e)
{
    if (e == nullptr) {
        return;
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(e->docs);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        /* The element box slots point into the arenas that are freed here. The documents must
         * outlive the engine (or be forgotten first). */
        embi_lay_doc *ld = (embi_lay_doc *)(uintptr_t)v;
        if (ld->doc != nullptr) {
            embi_lay_clear_boxes(ld->doc);
        }
        doc_destroy(ld);
    }
    embi_hashmap_free(e->docs);
    embi_free(e->docs, sizeof *e->docs);
    embi_free(e, sizeof *e);
}

void embi_layout_set_viewport(embi_layout_engine *e, float w, float h)
{
    e->vw = w;
    e->vh = h;
}

void embi_layout_forget_document(embi_layout_engine *e, embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        embi_hashmap_remove(e->docs, doc_key(doc));
        embi_lay_clear_boxes(doc);
        doc_destroy((embi_lay_doc *)(uintptr_t)v);
    }
}

embi_style_engine *embi_layout_style_engine(const embi_layout_engine *e)
{
    return e->style;
}

embi_text *embi_layout_text(const embi_layout_engine *e)
{
    return e->text;
}

void embi_layout_set_control_hook(embi_layout_engine *e, embi_layout_control_fn fn, void *user)
{
    e->control_fn = fn;
    e->control_user = user;
}

embi_layout_control_fn embi_layout_control_hook(const embi_layout_engine *e, void **user)
{
    *user = e->control_user;
    return e->control_fn;
}

const embi_frag *embi_layout_root(const embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return nullptr;
    }
    return ((embi_lay_doc *)(uintptr_t)v)->icb;
}

bool embi_lay_doc_has_transform(const embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return false;
    }
    return ((embi_lay_doc *)(uintptr_t)v)->has_xf;
}

/* The bytes in use in an arena (its blocks' used bytes, not their capacity). */
static size_t arena_used(const embi_arena *a)
{
    size_t total = 0;
    for (const embi_arena_block *blk = a->first; blk != nullptr; blk = blk->next) {
        total += blk->used;
    }
    return total;
}

/* Counts the boxes reachable from b (b and its siblings and their descendants). */
static uint32_t count_boxes(const embi_box *b)
{
    uint32_t n = 0;
    for (; b != nullptr; b = b->next) {
        n += 1 + (embi_box_is_text(b) ? 0 : count_boxes(b->first));
    }
    return n;
}

static uint32_t count_frags(const embi_frag *f)
{
    uint32_t n = 0;
    for (; f != nullptr; f = f->next) {
        n += 1 + count_frags(embi_frag_first(f));
    }
    return n;
}

void embi_layout_frag_set_scroll(embi_layout_engine *e, const embi_document *doc, embi_frag *f,
                                 float sx, float sy)
{
    embi_lay_doc *ld = doc_state(e, doc, false);
    if (ld == nullptr || f == nullptr) {
        return;
    }
    embi_fext *x = embi_lay_frag_ext(ld, f);
    if (x != nullptr) {
        x->sx = sx;
        x->sy = sy;
        ld->scrolled = ld->scrolled || sx != 0.0f || sy != 0.0f;
    }
}

void embi_layout_stats(const embi_layout_engine *e, const embi_document *doc, uint32_t *boxes,
                       uint32_t *frags, size_t *arena_bytes)
{
    *boxes = 0;
    *frags = 0;
    *arena_bytes = 0;
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return;
    }
    const embi_lay_doc *ld = (const embi_lay_doc *)(uintptr_t)v;
    *arena_bytes = arena_used(&ld->ba) + arena_used(&ld->fa);
    *frags = count_frags(ld->icb);
    embi_element *root = embi_document_element((embi_document *)(uintptr_t)doc);
    if (root != nullptr && root->box != nullptr) {
        *boxes = count_boxes((const embi_box *)root->box);
    }
}

void embi_layout_cold_stats_get(const embi_layout_engine *e, const embi_document *doc,
                                embi_layout_cold_stats *out)
{
    memset(out, 0, sizeof *out);
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        *out = ((const embi_lay_doc *)(uintptr_t)v)->cold;
    }
}

uint32_t embi_layout_tree_builds(const embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return 0;
    }
    return ((const embi_lay_doc *)(uintptr_t)v)->builds;
}

static size_t map_bytes(const embi_hashmap *m)
{
    return (size_t)m->cap * sizeof(embi_hash_slot);
}

size_t embi_layout_cache_bytes(const embi_layout_engine *e, const embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        return 0;
    }
    const embi_lay_doc *ld = (const embi_lay_doc *)(uintptr_t)v;
    return arena_used(&ld->pa) + (size_t)ld->pfront_n * sizeof *ld->pfront + map_bytes(&ld->tkeys) + map_bytes(&ld->langs) + map_bytes(&ld->ecache) +
           (size_t)ld->tents.cap * sizeof(embi_lay_tent *) + (size_t)ld->tmemo_n * sizeof(embi_lay_tent *) + ld->pool.bytes + (size_t)ld->stab_n * sizeof(embi_lay_ssum) + ld->prep_bytes + map_bytes(&ld->pmap) +
           (size_t)ld->preps.cap * sizeof(void *) +
           embi_lay_ifc_pool_bytes(ld);
}

void embi_layout_alloc_stats(const embi_layout_engine *e, const embi_document *doc, uint32_t *frag_allocs,
                             uint32_t *frag_reused)
{
    *frag_allocs = 0;
    *frag_reused = 0;
    uint64_t v = 0;
    if (embi_hashmap_get(e->docs, doc_key(doc), &v)) {
        const embi_lay_doc *ld = (const embi_lay_doc *)(uintptr_t)v;
        *frag_allocs = ld->frag_allocs;
        *frag_reused = ld->frag_reused;
    }
}

/* The used font size of the root element (the rem base). */
static float root_font_px(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_font_size);
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    return 16.0f;
}

/* True when the element tree of doc is nested deeper than limit (an iterative walk). */
static bool dom_deeper_than(embi_document *doc, uint32_t limit)
{
    embi_element *root = embi_document_element(doc);
    if (root == nullptr) {
        return false;
    }
    embi_node *top = &root->base;
    embi_node *n = top;
    uint32_t d = 1;
    for (;;) {
        if (d > limit) {
            return true;
        }
        if (n->first_child != nullptr) {
            n = n->first_child;
            d++;
            continue;
        }
        while (n != top && n->next == nullptr) {
            n = n->parent;
            d--;
        }
        if (n == top) {
            return false;
        }
        n = n->next;
    }
}

/* Resets what a layout leaves in the boxes of a kept tree: the fragments, and the scrollbar gutters the
 * last layout remembered (a rebuilt tree starts without them, so the result never depends on history). */
void embi_lay_reset_boxes(embi_box *top)
{
    embi_box *b = top;
    for (;;) {
        if (embi_box_is_text(b)) {
            /* no frag, no children, no gutter flags to reset */
        } else {
            b->frag = nullptr;
            b->flags2 = (uint16_t)(b->flags2 & ~(EMBI_BF2_VGUT | EMBI_BF2_HGUT));
            if (b->first != nullptr) {
                b = b->first;
                continue;
            }
        }
        while (b != top && b->next == nullptr) {
            b = b->parent;
        }
        if (b == top) {
            return;
        }
        b = b->next;
    }
}

/* W6e: the styles a box holds (the grave keeps the replaced styles alive because layout caches and boxes that no update
 * refreshed (generated boxes of descendants, the anonymous boxes below them) can still hold one). The set holds the styles
 * of the grave (a flat open-addressing table of pointers, sized for them: a page where every element restyled in the frame
 * has as many as it has boxes, and a hash map grown from empty and filled with every box's style cost more than the layout
 * itself); the walk over the boxes marks the ones some box holds. */
typedef struct grave_set {
    const embi_style **key; /* nullptr: empty */
    bool *held;
    uint32_t mask;
} grave_set;

static uint32_t grave_slot(const grave_set *g, const embi_style *s)
{
    uint64_t h = ((uint64_t)(uintptr_t)s >> 4) * 0x9e3779b97f4a7c15ull;
    uint32_t i = (uint32_t)(h >> 40) & g->mask;
    while (g->key[i] != nullptr && g->key[i] != s) {
        i = (i + 1) & g->mask;
    }
    return i;
}

static bool grave_style_held(void *user, const embi_style *s)
{
    const grave_set *g = user;
    uint32_t i = grave_slot(g, s);
    return g->key[i] == s && g->held[i];
}

/* Releases the styles of the grave that no box holds any more; the ones some box still holds stay until a later update (or
 * the next full build) lets them go. The walk costs one pass over the box tree, which is what the full build it replaces
 * costs many times over. */
static void release_grave_unused(embi_layout_engine *e, embi_lay_doc *ld, embi_document *doc)
{
    uint32_t n = embi_style_grave_count(e->style, doc);
    if (n == 0) {
        return;
    }
    uint32_t cap = 16;
    while (cap < 2u * n) {
        cap *= 2u;
    }
    grave_set g;
    g.mask = cap - 1u;
    g.key = embi_calloc(cap, sizeof *g.key);
    g.held = embi_calloc(cap, sizeof *g.held);
    for (uint32_t i = 0; i < n; i++) {
        const embi_style *s = embi_style_grave_at(e->style, doc, i);
        g.key[grave_slot(&g, s)] = s;
    }
    const embi_box *b = ld->root_box;
    while (b != nullptr) {
        if (b->style != nullptr) {
            uint32_t i = grave_slot(&g, b->style);
            if (g.key[i] != nullptr) {
                g.held[i] = true;
            }
        }
        const embi_box *first = embi_box_is_text(b) ? nullptr : b->first;
        if (first != nullptr) {
            b = first;
            continue;
        }
        while (b != nullptr && b->next == nullptr) {
            b = b->parent;
        }
        if (b != nullptr) {
            b = b->next;
        }
    }
    (void)embi_style_release_grave_unreferenced(e->style, doc, grave_style_held, &g);
    embi_free(g.key, (size_t)cap * sizeof *g.key);
    embi_free(g.held, (size_t)cap * sizeof *g.held);
}

static bool layout_run(embi_layout_engine *e, embi_document *doc, uint32_t max_depth)
{
    embi_lay_doc *ld = doc_state(e, doc, true);
    if (ld == nullptr) {
        return false;
    }
    embi_style_resolve_document(e->style, doc);
    uint64_t epoch = embi_style_document_epoch(e->style, doc);
    /* W6b: what the style engine changed since the last layout decides what is rebuilt (incr.c). */
    int ukind = embi_lay_incr_plan(e, ld, doc);
    ld->last_kind = (uint8_t)ukind;
    ld->last_partial = false;
    if (ukind == EMBI_LU_NONE || ukind == EMBI_LU_PAINT) {
        if (ukind == EMBI_LU_NONE) {
            ld->n_none++;
        } else {
            ld->n_paint++;
        }
        ld->frag_allocs = 0;
        ld->frag_reused = 0;
        if (ukind == EMBI_LU_PAINT) {
            /* W6e: the boxes of the changed elements hold the new styles and the layout caches keyed by style address were
             * emptied by the plan, so the old styles can go now. Without this the grave of a page that restyles 200 elements
             * a frame (a transition on a list) passed EMBI_INCR_GRAVE_MAX every ten frames and the tree was built again. */
            release_grave_unused(e, ld, doc);
        }
        return true; /* the fragments of the last layout stand */
    }
    if (ukind == EMBI_LU_SURGERY) {
        /* W6b: lay out again only the boxes the surgery built and what their size change reaches (incr.c); when that
         * cannot be done the whole kept tree is laid out below */
        ld->frag_allocs = 0;
        ld->frag_reused = 0;
        if (embi_lay_partial(e, ld, doc)) {
            ld->last_partial = true;
            ld->n_surgery++;
            ld->fserial++;
            release_grave_unused(e, ld, doc); /* W6e: as for a paint-only update; the relayout is done */
            return true;
        }
    }
    bool reuse = ld->plan == EMBI_LP_KEEP && ld->root_box != nullptr && ld->doc == doc;
    if (reuse) {
        ld->n_surgery++;
    } else {
        ld->n_full++;
    }
    ld->fserial++;
    doc_reset(ld);
    ld->frag_allocs = 0;
    ld->frag_reused = 0;
    if (!reuse) {
        doc_drop_tree(ld);
        if (ld->flush_text) {
            ld->flush_text = false;
            embi_lay_tcache_sweep(ld, true); /* the tree and the fragments that used the runs are gone */
        }
    }
    ld->doc = doc;
    ld->last_partial = false;

    embi_lctx c;
    memset(&c, 0, sizeof c);
    c.eng = e;
    c.ld = ld;
    c.style = e->style;
    c.text = e->text;
    c.fonts = embi_tshim_fonts(e->text);
    c.initial = embi_style_initial(e->style);
    c.vw = e->vw;
    c.vh = e->vh;
    c.scrollbar = e->scrollbar;
    c.image_size = e->image_size;
    c.image_user = e->image_user;
    c.root_px = 16.0f;
    c.max_depth = max_depth;
    c.no_pseudo = !embi_style_has_generated(e->style, doc);
    embi_hashmap_init(&c.tmodels);

    embi_frag *icb = embi_lay_new_frag(&c, nullptr, EMBI_FK_BOX);
    if (icb == nullptr) {
        embi_table_models_free(&c);
        return false;
    }
    embi_lay_frag_set_size(icb, e->vw, e->vh);
    ld->icb = icb;

    embi_element *root = embi_document_element(doc);
    if (root == nullptr) {
        embi_lay_position_finish(&c, icb);
        embi_ctr_free(&c.ctr);
        embi_table_models_free(&c);
        if (!reuse) {
            embi_lay_tcache_sweep(ld, false);
            embi_lay_incr_built(e, ld, doc);
        }
        return true;
    }
    const embi_style *rs = embi_style_get(e->style, root, EMBI_PE_NONE);
    if (rs != nullptr) {
        c.root_px = root_font_px(rs);
    }
    embi_box *rb;
    if (reuse) {
        rb = ld->root_box;
        embi_lay_reset_boxes(rb);
    } else {
        uint64_t bt0 = SDL_GetPerformanceCounter();
        rb = embi_lay_build_root(&c, root);
        ld->cold.build_ticks += SDL_GetPerformanceCounter() - bt0;
        ld->builds++;
        ld->cold.builds++;
        ld->root_box = rb;
        ld->t_epoch = epoch;
        ld->t_vw = e->vw;
        ld->t_vh = e->vh;
        ld->t_sb = e->scrollbar;
        ld->t_depth = max_depth;
    }
    if (rb != nullptr && (rb->flags & EMBI_BF_ABSPOS) != 0) {
        /* An absolutely positioned root element: its containing block is the initial containing block, its static
         * position the origin (CSS 2.1 10.1, the root cannot be taken out of a flow it is not in). */
        embi_lay_abspos_at(&c, rb, nullptr, 0.0f, 0.0f, nullptr);
    } else if (rb != nullptr) {
        embi_cs cs = {e->vw, e->vw, e->vh, EMBI_FIT_FILL, nullptr, 0, 0, false, 0, 0}; /* W3b: no BFC above */
        embi_lres r;
        memset(&r, 0, sizeof r);
        c.chain_ok = true; /* W6b: the root starts the chain of the partial relayout (incr.c) */
        c.via_flow = true;
        embi_lay_box(&c, rb, &cs, &r);
        c.chain_ok = false;
        if (r.frag != nullptr) {
            /* The root's margins do not collapse with the initial containing block. */
            r.frag->x = r.ml;
            r.frag->y = r.mt;
            r.frag->parent = icb;
            r.frag->next = nullptr;
            icb->first = r.frag;
            icb->last = r.frag;
            embi_lay_ov_add_child(icb, r.frag);
        }
    }
    /* The out-of-flow boxes are placed once the whole tree has its positions (position.c). */
    embi_lay_position_finish(&c, icb);
    embi_ctr_free(&c.ctr);
    embi_table_models_free(&c);
    if (!reuse) {
        embi_lay_tcache_sweep(ld, false);
        embi_lay_incr_built(e, ld, doc);
    } else {
        release_grave_unused(e, ld, doc); /* W6e: the kept tree was laid out over the new styles */
    }
    return true;
}

typedef struct lay_job {
    embi_layout_engine *e;
    embi_document *doc;
    bool ok;
} lay_job;

static int SDLCALL lay_thread(void *ud)
{
    lay_job *j = ud;
    j->ok = layout_run(j->e, j->doc, EMBI_LAY_MAX_DEPTH);
    return 0;
}

/* Scroll offsets across a relayout (W5d2). A layout rebuilds the fragments, and the offsets live in them, so a relayout
 * caused by a state change (a focus, a :hover rule) must not send the scroll containers back to their start. The offsets
 * of the previous layout are kept by element and pseudo box, and the icb (which holds the viewport's offset) by a
 * marker, then given to the new fragments of the same boxes, clamped to the new scroll range. Elements are compared by
 * address and pseudo box only, and no kept element is dereferenced. Known risk (ABA): an element removed between two
 * layouts whose memory is reused by a new element of the same pseudo box inherits the removed one's offset, clamped. The
 * effect is a stale scroll position, never a crash. */
typedef struct lay_keep {
    const void *el;   /* nullptr: the icb (the viewport) */
    uint8_t pe;
    float sx, sy;
} lay_keep;

typedef struct lay_keeps {
    lay_keep *v;
    uint32_t n, cap;
} lay_keeps;

static void keeps_push(lay_keeps *k, const void *el, uint8_t pe, float sx, float sy)
{
    if (k->n == k->cap) {
        uint32_t ncap = k->cap != 0 ? k->cap * 2u : 16u;
        lay_keep *nv = SDL_malloc(ncap * sizeof *nv);
        if (nv == nullptr) {
            return;
        }
        if (k->v != nullptr) {
            SDL_memcpy(nv, k->v, k->n * sizeof *nv);
            SDL_free(k->v);
        }
        k->v = nv;
        k->cap = ncap;
    }
    k->v[k->n++] = (lay_keep){el, pe, sx, sy};
}

static void keeps_collect(lay_keeps *k, const embi_frag *f)
{
    for (const embi_frag *c = embi_frag_first(f); c != nullptr; c = c->next) {
        if (c->kind == EMBI_FK_BOX) {
            const embi_fext *x = embi_frag_ext(c);
            if (x != nullptr && (x->sx != 0.0f || x->sy != 0.0f) && c->box != nullptr && c->box->el != nullptr) {
                keeps_push(k, c->box->el, c->box->pe, x->sx, x->sy);
            }
        }
        if (c->kind != EMBI_FK_TEXT) {
            keeps_collect(k, c);
        }
    }
}

static void keeps_apply(embi_layout_engine *e, embi_document *doc, embi_lay_doc *ld, const lay_keeps *k, embi_frag *f,
                        bool top)
{
    if (top) {
        for (uint32_t i = 0; i < k->n; i++) {
            if (k->v[i].el == nullptr) {
                embi_fext *x = embi_lay_frag_ext(ld, f);
                if (x != nullptr) {
                    /* Clamp to the new scroll range, as a browser does after layout (embi_scroll_max_root). */
                    float ov[4];
                    embi_frag_ov(f, ov);
                    float mx = fmaxf(0.0f, ov[2] - f->w), my = fmaxf(0.0f, ov[3] - f->h);
                    x->sx = k->v[i].sx < 0.0f ? 0.0f : (k->v[i].sx > mx ? mx : k->v[i].sx);
                    x->sy = k->v[i].sy < 0.0f ? 0.0f : (k->v[i].sy > my ? my : k->v[i].sy);
                    ld->scrolled = ld->scrolled || x->sx != 0.0f || x->sy != 0.0f;
                }
            }
        }
    }
    for (embi_frag *c = embi_frag_first(f); c != nullptr; c = c->next) {
        if (c->kind == EMBI_FK_BOX && c->box != nullptr && c->box->el != nullptr) {
            for (uint32_t i = 0; i < k->n; i++) {
                if (k->v[i].el == c->box->el && k->v[i].pe == c->box->pe) {
                    float mx = 0.0f, my = 0.0f, cw = 0.0f, ch = 0.0f, sw = 0.0f, sh = 0.0f;
                    if (embi_layout_scroll_size(e, c->box->el, &sw, &sh) && embi_layout_client_size(e, c->box->el, &cw, &ch)) {
                        mx = sw > cw ? sw - cw : 0.0f;
                        my = sh > ch ? sh - ch : 0.0f;
                    }
                    float sx = k->v[i].sx < 0.0f ? 0.0f : (k->v[i].sx > mx ? mx : k->v[i].sx);
                    float sy = k->v[i].sy < 0.0f ? 0.0f : (k->v[i].sy > my ? my : k->v[i].sy);
                    embi_fext *x = embi_lay_frag_ext(ld, c);
                    if (x != nullptr) {
                        x->sx = sx;
                        x->sy = sy;
                        ld->scrolled = ld->scrolled || sx != 0.0f || sy != 0.0f;
                    }
                    break;
                }
            }
        }
        if (c->kind != EMBI_FK_TEXT) {
            keeps_apply(e, doc, ld, k, c, false);
        }
    }
}

static bool layout_document_run(embi_layout_engine *e, embi_document *doc);

/* EMBER_VERIFY_INCREMENTAL (W6b): after an update that did not build the tree again, the document is laid out from scratch by
 * a throwaway engine (no change set, no records, its own box and fragment arenas) and the two fragment trees are compared.
 * The real layout state is not touched: its fragments, records, dirty list and serial stay what the update made them, so the
 * incremental paths that follow (the paint cache) are verified in the state they really run in. The element slots of the
 * boxes (embi_element.box) are the one thing both engines write: they are saved and put back. */
static void verify_against_full(embi_layout_engine *e, embi_document *doc)
{
    embi_lay_doc *ld = doc_state(e, doc, false);
    if (ld == nullptr || ld->icb == nullptr || ld->last_kind == EMBI_LU_FULL || ld->last_kind == EMBI_LU_ABORT) {
        return;
    }
    uint8_t kind = ld->last_kind;
    bool partial = ld->last_partial;
    EMBI_VEC(embi_element *) els = {0};
    EMBI_VEC(void *) boxes = {0};
    embi_node *top = &doc->base;
    for (embi_node *n = embi_next_in_tree(top, top); n != nullptr; n = embi_next_in_tree(top, n)) {
        if (embi_is_element(n)) {
            embi_element *el = embi_as_element(n);
            embi_vec_push(&els, el);
            embi_vec_push(&boxes, el->box);
        }
    }
    embi_layout_config cfg = {e->image_user, e->image_size, e->scrollbar};
    embi_layout_engine *t = embi_layout_engine_create(e->text, e->style, &cfg);
    if (t != nullptr) {
        t->scratch = true;
        t->vw = e->vw;
        t->vh = e->vh;
        t->images_stable = e->images_stable;
        t->control_fn = e->control_fn;
        t->control_user = e->control_user;
        bool ok = layout_document_run(t, doc);
        const embi_frag *ref = embi_layout_root(t, doc);
        if (ok && ref != nullptr) {
            embi_lay_snap *a = embi_lay_snapshot(ld->icb);
            embi_lay_snap *b = embi_lay_snapshot(ref);
            if (!embi_lay_snapshot_equal(a, b)) {
                embi_incr_verify_fail(kind == EMBI_LU_SURGERY
                                          ? (partial ? "layout: a partial relayout differs from a full build"
                                                     : "layout: a surgery update differs from a full build")
                                          : "layout: a paint-only update differs from a full build");
            }
            embi_lay_snapshot_free(a);
            embi_lay_snapshot_free(b);
        }
        embi_layout_engine_destroy(t);
    }
    for (uint32_t i = 0; i < els.len; i++) {
        els.data[i]->box = boxes.data[i];
    }
    embi_vec_free(&els);
    embi_vec_free(&boxes);
}

bool embi_layout_document(embi_layout_engine *e, embi_document *doc)
{
    lay_keeps keep = {nullptr, 0, 0};
    embi_lay_doc *old = doc_state(e, doc, false);
    bool was_scrolled = old != nullptr && old->scrolled;
    if (old != nullptr && old->icb != nullptr && old->scrolled) {
        const embi_frag *oi = old->icb;
        float isx = embi_frag_sx(oi), isy = embi_frag_sy(oi);
        if (isx != 0.0f || isy != 0.0f) {
            keeps_push(&keep, nullptr, 0, isx, isy);
        }
        keeps_collect(&keep, oi);
        old->scrolled = false; /* the flag describes the new layout: keeps_apply sets it again for each offset it gives */
    }
    bool ok = layout_document_run(e, doc);
    {
        embi_lay_doc *ld = doc_state(e, doc, false);
        if (ld != nullptr && !ok) {
            ld->last_kind = EMBI_LU_ABORT; /* no layout ran in this call: what the last one did does not describe it */
        }
        if (ld != nullptr && (ld->last_kind == EMBI_LU_NONE || ld->last_kind == EMBI_LU_PAINT)) {
            /* W6b: the fragments were not rebuilt, the offsets are still in them */
            ld->scrolled = ld->scrolled || was_scrolled;
        } else if (keep.n > 0 && ld != nullptr && ld->icb != nullptr) {
            keeps_apply(e, doc, ld, &keep, ld->icb, true);
        }
    }
    if (ok && embi_incr_verify_mode() != 0) {
        verify_against_full(e, doc);
    }
    SDL_free(keep.v);
    return ok;
}

static bool layout_document_run(embi_layout_engine *e, embi_document *doc)
{
    /* The DOM depth only changes with the DOM, which changes the style epoch: walk the 10k elements of a big
     * page once per epoch, not on every layout. */
    embi_style_resolve_document(e->style, doc);
    uint64_t epoch = embi_style_document_epoch(e->style, doc);
    embi_lay_doc *ld = epoch != 0 ? doc_state(e, doc, true) : nullptr;
    bool deep;
    if (ld != nullptr && ld->deep_epoch == epoch) {
        deep = ld->deep;
    } else {
        deep = dom_deeper_than(doc, EMBI_LAY_INLINE_DEPTH);
        if (ld != nullptr) {
            ld->deep = deep;
            ld->deep_epoch = epoch;
        }
    }
    if (!deep) {
        return layout_run(e, doc, EMBI_LAY_MAX_DEPTH);
    }
    /* A deep page needs more stack than the caller is known to have: lay it out on a thread that has it. */
    lay_job job = {e, doc, false};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Thread *t = nullptr;
    if (props != 0) {
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)lay_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "ember-layout");
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &job);
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, (Sint64)EMBI_LAY_STACK_BYTES);
        t = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (t == nullptr) {
        /* No silent fallback: a lower cap here would make the result depend on the environment. */
        embi_log_error("layout: cannot start the %u MiB layout thread a page nested deeper than %u needs",
                       (unsigned)(EMBI_LAY_STACK_BYTES >> 20), EMBI_LAY_INLINE_DEPTH);
        return false;
    }
    SDL_WaitThread(t, nullptr);
    return job.ok;
}
