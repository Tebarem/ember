/*
 * Heap allocator and non-moving mark-sweep collector.
 *
 * Small cells come from 16 KB pages, one size class per page, carved from chunks (64 KB, then
 * doubling up to 256 KB). A page with no live cell after a sweep goes back to the page pool and may
 * take any size class. Cells above EMBI_JS_MAX_SMALL are individual allocations kept in a hash map
 * keyed by address. A word on the C stack that equals the address of a live cell's first byte marks
 * that cell (conservative scan). Interior addresses do not count.
 *
 * Collections. Marking runs from the roots: the conservative C-stack scan (from the current frame
 * up to the stack base recorded by the outermost embi_js_enter, or to the OS stack top when GC
 * runs outside one), the atom table, persistent handles, the pending exception, the job queue and
 * the root callbacks. Weak handles and ephemeron entries are cleared after marking when their
 * target is unmarked.
 *
 * Sweeping is lazy. A collection queues pages per size class and frees nothing itself, except
 * large cells, which are swept eagerly. A refill sweeps the queue of its class before it takes a
 * page from the pool, and the next collection finishes whatever is still queued before it marks.
 * The free lists are emptied when a collection starts, and each swept page links all its free
 * cells, so the allocator only ever hands out cells from swept pages.
 *
 * Generational mode (off by default) keeps the mark bit set on survivors (sticky: 1 is old). A
 * minor collection marks from the roots and the remembered set only, so it skips old cells, and
 * it queues only the pages given free cells since the previous collection began. Old unmarked
 * cells on those pages survive, and the next major collection frees them. EMBI_JS_WB records an
 * old cell that stores a young one in the remembered set.
 */
#include "js/heap.h"
#include "js/ic.h"
#include "js/jobs.h"
#include "js/runtime.h"
#include "js/string.h"

#include <SDL3/SDL_timer.h>

#include <stdlib.h>

#define EMBI_NOINLINE __attribute__((noinline))
#define EMBI_NO_ASAN __attribute__((no_sanitize_address))

_Thread_local embi_js_heap *embi_js_wb_heap = nullptr;

static const uint32_t g_class_size[EMBI_JS_NUM_CLASSES] = {
    16,  32,  48,  64,  80,   96,   112,  128,  160,  192,
    224, 256, 320, 384, 448,  512,  768,  1024, 1536, 2048,
};

struct embi_js_chunk {
    uint8_t *base; /* EMBI_JS_PAGE_BYTES aligned start of the first page */
    void *raw;     /* pointer returned by embi_alloc, for freeing */
    size_t raw_size;
    uint32_t npages;   /* pages in this chunk: EMBI_JS_CHUNK_FIRST_PAGES .. EMBI_JS_CHUNK_PAGES */
    uint8_t page_class[EMBI_JS_CHUNK_PAGES];
    uint8_t page_young[EMBI_JS_CHUNK_PAGES]; /* set while the page is in heap.young */
    uint32_t pooled;                         /* pages of this chunk in the page pool */
    bool releasing;                          /* set while release_empty_chunks drops it */
};

static size_t chunk_bytes(const embi_js_chunk *c)
{
    return (size_t)c->npages << EMBI_JS_PAGE_SHIFT;
}

typedef struct large_ref {
    uintptr_t addr;
    size_t size;
} large_ref;

static void collect(embi_js_runtime *rt, bool major, uintptr_t lo);
static void collect_for_alloc(embi_js_runtime *rt);
static void collect_entry(embi_js_runtime *rt, bool major);
static void finish_sweep(embi_js_heap *h);

/* ------------------------------------------------------------------ setup and teardown */

void embi_js_heap_init(embi_js_heap *h)
{
    memset(h, 0, sizeof *h);
    for (uint32_t i = 0; i <= EMBI_JS_MAX_SMALL / 16u; i++) {
        uint32_t want = i * 16u;
        uint32_t c = 0;
        while (g_class_size[c] < want) {
            c++;
        }
        h->class_of[i] = (uint8_t)c;
    }
    h->min_trigger = EMBI_JS_GC_FLOOR_BYTES;
    h->trigger_factor = EMBI_JS_GC_FACTOR_MAX;
    h->threshold = h->min_trigger;
    h->major_limit = h->min_trigger;
    h->next_chunk_pages = EMBI_JS_CHUNK_FIRST_PAGES;
    embi_hashmap_init(&h->large);
}

void embi_js_heap_set_ops(embi_js_heap *h, uint8_t type, embi_js_type_ops ops)
{
    EMBI_ASSERT(type < EMBI_JS_CELL_TYPE_COUNT);
    h->ops[type] = ops;
}

static void chunk_insert(embi_js_heap *h, embi_js_chunk *c)
{
    uint32_t lo = 0;
    uint32_t hi = h->chunks.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if ((uintptr_t)h->chunks.data[mid]->base < (uintptr_t)c->base) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    embi_vec_reserve(&h->chunks, h->chunks.len + 1u);
    memmove(&h->chunks.data[lo + 1u], &h->chunks.data[lo],
            (size_t)(h->chunks.len - lo) * sizeof h->chunks.data[0]);
    h->chunks.data[lo] = c;
    h->chunks.len++;
}

/* Returns the chunk whose address range contains addr, or nullptr. */
static embi_js_chunk *chunk_for(const embi_js_heap *h, uintptr_t addr)
{
    uint32_t lo = 0;
    uint32_t hi = h->chunks.len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if ((uintptr_t)h->chunks.data[mid]->base <= addr) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;
    }
    embi_js_chunk *c = h->chunks.data[lo - 1u];
    return addr - (uintptr_t)c->base < chunk_bytes(c) ? c : nullptr;
}

/* ------------------------------------------------------------------ page pool */

static void pool_push(embi_js_heap *h, embi_js_chunk *c, uint32_t p)
{
    c->page_class[p] = EMBI_JS_NO_CLASS;
    embi_js_page_ref r = {c, p};
    embi_vec_push(&h->free_pages, r);
    c->pooled++;
}

static embi_js_page_ref pool_pop(embi_js_heap *h)
{
    embi_js_page_ref r = embi_vec_pop(&h->free_pages);
    r.chunk->pooled--;
    return r;
}

static void new_chunk(embi_js_heap *h)
{
    /* J5m3: a heap starts with small chunks, so a small script's footprint is not padded by a full
     * 256 KB chunk. Each new chunk doubles the page count until EMBI_JS_CHUNK_PAGES. */
    uint32_t np = h->next_chunk_pages;
    if (np < EMBI_JS_CHUNK_PAGES) {
        h->next_chunk_pages = np * 2u;
    }
    size_t raw_size = ((size_t)np << EMBI_JS_PAGE_SHIFT) + EMBI_JS_PAGE_BYTES;
    uint8_t *raw = embi_alloc(raw_size);
    uintptr_t aligned = ((uintptr_t)raw + EMBI_JS_PAGE_BYTES - 1u) & ~((uintptr_t)EMBI_JS_PAGE_BYTES - 1u);
    embi_js_chunk *c = embi_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->base = (uint8_t *)aligned;
    c->raw = raw;
    c->raw_size = raw_size;
    c->npages = np;
    memset(c->page_class, EMBI_JS_NO_CLASS, sizeof c->page_class);
    chunk_insert(h, c);
    for (uint32_t i = np; i-- > 0;) {
        pool_push(h, c, i);
    }
}

/* Drops every chunk whose pages are all in the pool, except one spare. Runs only when the sweep is
 * finished, so no queue or young list still names a page of a dropped chunk. */
static void release_empty_chunks(embi_js_heap *h)
{
    uint32_t full = 0;
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        if (h->chunks.data[ci]->pooled == h->chunks.data[ci]->npages) {
            full++;
        }
    }
    if (full <= 1u) {
        return;
    }
    uint32_t spare = 1u;
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        if (c->pooled == c->npages) {
            if (spare != 0) {
                spare = 0;
            } else {
                c->releasing = true;
            }
        }
    }

    uint32_t w = 0;
    for (uint32_t i = 0; i < h->free_pages.len; i++) {
        if (!h->free_pages.data[i].chunk->releasing) {
            h->free_pages.data[w++] = h->free_pages.data[i];
        }
    }
    h->free_pages.len = w;
    w = 0;
    for (uint32_t i = 0; i < h->young.len; i++) {
        if (!h->young.data[i].chunk->releasing) {
            h->young.data[w++] = h->young.data[i];
        }
    }
    h->young.len = w;

    w = 0;
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        if (!c->releasing) {
            h->chunks.data[w++] = c;
            continue;
        }
        embi_free(c->raw, c->raw_size);
        embi_free(c, sizeof *c);
        h->released_chunks++;
    }
    h->chunks.len = w;
}

void embi_js_heap_destroy(embi_js_heap *h)
{
    if (embi_js_wb_heap == h) {
        embi_js_wb_heap = nullptr;
    }
    /* Finalize everything first, while every cell is still intact. Dead cells on pages still
     * queued for the lazy sweep are not finalized yet, so they are finalized here. */
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        for (uint32_t p = 0; p < c->npages; p++) {
            uint8_t cls = c->page_class[p];
            if (cls == EMBI_JS_NO_CLASS) {
                continue;
            }
            size_t sz = g_class_size[cls];
            uint8_t *page = c->base + (size_t)p * EMBI_JS_PAGE_BYTES;
            for (size_t off = 0; off + sz <= EMBI_JS_PAGE_BYTES; off += sz) {
                embi_js_cell *cell = (embi_js_cell *)(page + off);
                if (cell->type != EMBI_JS_CELL_FREE && h->ops[cell->type].finalize) {
                    h->ops[cell->type].finalize(cell);
                }
            }
        }
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&h->large);
    uint64_t addr = 0;
    uint64_t size = 0;
    while (embi_hashmap_iter_next(&it, &addr, &size)) {
        embi_js_cell *cell = (embi_js_cell *)(uintptr_t)addr;
        if (h->ops[cell->type].finalize) {
            h->ops[cell->type].finalize(cell);
        }
    }

    /* Then release the memory. */
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        embi_free(c->raw, c->raw_size);
        embi_free(c, sizeof *c);
    }
    it = embi_hashmap_iter_begin(&h->large);
    while (embi_hashmap_iter_next(&it, &addr, &size)) {
        embi_free((void *)(uintptr_t)addr, (size_t)size);
    }
    embi_vec_free(&h->chunks);
    embi_vec_free(&h->free_pages);
    for (uint32_t cls = 0; cls < EMBI_JS_NUM_CLASSES; cls++) {
        embi_vec_free(&h->unswept[cls]);
    }
    embi_vec_free(&h->young);
    embi_vec_free(&h->remembered);
    embi_hashmap_free(&h->large);
    memset(h->free_lists, 0, sizeof h->free_lists);
    h->live_bytes = 0;
    h->unswept_total = 0;
}

/* ------------------------------------------------------------------ allocation */

static void page_set_young(embi_js_heap *h, embi_js_chunk *c, uint32_t p)
{
    if (!c->page_young[p]) {
        c->page_young[p] = 1;
        embi_js_page_ref r = {c, p};
        embi_vec_push(&h->young, r);
    }
}

static void sweep_page(embi_js_heap *h, embi_js_chunk *c, uint32_t p, uint8_t cls);

/* Chunks are released once the lazy sweep has drained, not only when a collection finishes it, so
 * an alloc-triggered major still gives back its whole chunks as soon as the queues empty. */
static void release_if_drained(embi_js_heap *h)
{
    if (h->release_pending && h->unswept_total == 0) {
        h->release_pending = false;
        release_empty_chunks(h);
    }
}

/* The pool is empty, so before a new chunk is carved, the queued pages of the other classes are
 * swept (round-robin after cls) until one of them goes back to the pool or every queue is empty.
 * Pages that keep live cells link their free cells onto their own class, which is still useful.
 * cls's own queue is empty when this runs. */
static void sweep_others_for_pool(embi_js_heap *h, uint32_t cls)
{
    uint32_t c = cls;
    while (h->free_pages.len == 0 && h->unswept_total != 0) {
        c = (c + 1u) % EMBI_JS_NUM_CLASSES;
        if (h->unswept[c].len == 0) {
            continue;
        }
        embi_js_page_ref r = embi_vec_pop(&h->unswept[c]);
        h->unswept_total--;
        sweep_page(h, r.chunk, r.index, (uint8_t)c);
    }
}

/* Gives the class a free cell. It sweeps queued pages of the class first, so that a page that has
 * free cells is reused before a new one is carved from the pool. */
static void refill_class(embi_js_heap *h, uint32_t cls)
{
    while (h->free_lists[cls] == nullptr && h->unswept[cls].len != 0) {
        embi_js_page_ref r = embi_vec_pop(&h->unswept[cls]);
        h->unswept_total--;
        sweep_page(h, r.chunk, r.index, (uint8_t)cls);
    }
    if (h->free_lists[cls] != nullptr) {
        release_if_drained(h);
        return;
    }
    if (h->free_pages.len == 0) {
        sweep_others_for_pool(h, cls);
    }
    release_if_drained(h);
    if (h->free_pages.len == 0) {
        new_chunk(h);
    }
    embi_js_page_ref pr = pool_pop(h);
    pr.chunk->page_class[pr.index] = (uint8_t)cls;
    size_t sz = g_class_size[cls];
    uint8_t *page = pr.chunk->base + (size_t)pr.index * EMBI_JS_PAGE_BYTES;
    uint32_t n = (uint32_t)(EMBI_JS_PAGE_BYTES / sz);
    for (uint32_t i = n; i-- > 0;) {
        embi_js_free_cell *fc = (embi_js_free_cell *)(page + (size_t)i * sz);
        fc->hdr.type = EMBI_JS_CELL_FREE;
        fc->hdr.mark = 0;
        fc->hdr.flags = 0;
        fc->hdr.aux = 0;
        fc->next = h->free_lists[cls];
        h->free_lists[cls] = fc;
    }
    page_set_young(h, pr.chunk, pr.index);
}

/* The bytes a cell occupies: its size class, or its large allocation. 0 for a non-heap address.
 * The chunk of the previous lookup is tried first, because marking visits cells that sit close
 * together, so the binary search over chunks runs only when the cell leaves that chunk. */
static size_t cell_bytes(embi_js_tracer *t, const embi_js_cell *c)
{
    uintptr_t a = (uintptr_t)c;
    embi_js_chunk *ch = t->chunk_hint;
    if (ch == nullptr || a - (uintptr_t)ch->base >= chunk_bytes(ch)) {
        ch = chunk_for(t->heap, a);
        t->chunk_hint = ch;
    }
    if (ch != nullptr) {
        uint8_t cls = ch->page_class[(a - (uintptr_t)ch->base) >> EMBI_JS_PAGE_SHIFT];
        if (cls != EMBI_JS_NO_CLASS) {
            return g_class_size[cls];
        }
    }
    uint64_t size = 0;
    if (t->heap->large.len != 0 && embi_hashmap_get(&t->heap->large, (uint64_t)a, &size)) {
        return (size_t)size;
    }
    return 0;
}

/* The growth factor for a live heap of the given size, before the cap from embi_js_set_gc_params. */
static double factor_for(size_t live)
{
    if (live <= EMBI_JS_GC_CURVE_LO) {
        return EMBI_JS_GC_FACTOR_MAX;
    }
    if (live >= EMBI_JS_GC_CURVE_HI) {
        return EMBI_JS_GC_FACTOR_MIN;
    }
    double t = (double)(live - EMBI_JS_GC_CURVE_LO) / (double)(EMBI_JS_GC_CURVE_HI - EMBI_JS_GC_CURVE_LO);
    return EMBI_JS_GC_FACTOR_MAX - (EMBI_JS_GC_FACTOR_MAX - EMBI_JS_GC_FACTOR_MIN) * t;
}

/* `shorten` applies the allocation-rate cut (minor budgets of a generational heap only). The allocation budget after a collection that left `live` bytes: the room under the heap limit
 * live * factor (heap.h). A factor at or below 1 leaves no room, so the budget is the floor. */
static size_t gc_budget(const embi_js_heap *h, size_t live, bool shorten)
{
    double f = factor_for(live);
    if (f > h->trigger_factor) {
        f = h->trigger_factor;
    }
    double room = f - 1.0;
    if (room < 0.0) {
        room = 0.0;
    }
    double grown = (double)live * room;
    size_t b = grown >= (double)h->min_trigger ? (size_t)grown : h->min_trigger;
    if (shorten && h->alloc_rate > EMBI_JS_GC_RATE_LO) {
        double s = (h->alloc_rate - EMBI_JS_GC_RATE_LO) / (EMBI_JS_GC_RATE_HI - EMBI_JS_GC_RATE_LO);
        if (s > 1.0) {
            s = 1.0;
        }
        b = (size_t)((double)b * (1.0 - 0.5 * s));
        if (b < h->min_trigger) {
            b = h->min_trigger;
        }
    }
    return b;
}

size_t embi_js_gc_count_unmarked(const embi_js_heap *h)
{
    size_t n = 0;
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        const embi_js_chunk *c = h->chunks.data[ci];
        for (uint32_t p = 0; p < c->npages; p++) {
            uint8_t cls = c->page_class[p];
            if (cls == EMBI_JS_NO_CLASS) {
                continue;
            }
            size_t sz = g_class_size[cls];
            for (size_t off = 0; off + sz <= EMBI_JS_PAGE_BYTES; off += sz) {
                const embi_js_cell *cell = (const embi_js_cell *)(c->base + (size_t)p * EMBI_JS_PAGE_BYTES + off);
                if (cell->type != EMBI_JS_CELL_FREE && cell->mark == 0) {
                    n++;
                }
            }
        }
    }
    return n;
}

void embi_js_gc_retune(embi_js_heap *h)
{
    h->threshold = gc_budget(h, h->live_after_gc, h->generational);
    h->major_limit = gc_budget(h, h->major_live, false);
}

void embi_js_gc_hold_begin(embi_js_runtime *rt)
{
    embi_js_heap *h = &rt->heap;
    h->gc_hold++;
}

size_t embi_js_gc_alloc_mark(const embi_js_runtime *rt)
{
    return rt->heap.bytes_since_gc;
}

void embi_js_gc_forgive_since(embi_js_runtime *rt, size_t mark)
{
    embi_js_heap *h = &rt->heap;
    if (h->bytes_since_gc > mark) {
        h->bytes_since_gc = mark;
    }
}

void embi_js_gc_hold_end(embi_js_runtime *rt)
{
    embi_js_heap *h = &rt->heap;
    EMBI_ASSERT(h->gc_hold > 0);
    /* Clamped: an unbalanced end must not wrap the counter and hold collections off forever. What the
     * compile allocated stays counted, so a collection that became due during the hold runs at the
     * first allocation after it. */
    if (h->gc_hold > 0) {
        h->gc_hold--;
    }
}

embi_js_cell *embi_js_alloc_cell(embi_js_runtime *rt, size_t bytes, uint8_t type)
{
    embi_js_heap *h = &rt->heap;
    EMBI_ASSERT(!h->in_gc);
    EMBI_ASSERT(bytes >= sizeof(embi_js_cell));
    EMBI_ASSERT(type > EMBI_JS_CELL_FREE && type < EMBI_JS_CELL_TYPE_COUNT);

    bool due = (h->gc_hold == 0 || h->threshold < EMBI_JS_GC_HOLD_MIN_BUDGET) &&
               h->bytes_since_gc > h->threshold;
    if (h->stress_every != 0 && --h->stress_left == 0) {
        h->stress_left = h->stress_every;
        due = true;
    }
    if (due) {
        collect_for_alloc(rt);
    }

    size_t sz;
    embi_js_cell *c;
    if (bytes > EMBI_JS_MAX_SMALL) {
        sz = (bytes + 15u) & ~(size_t)15u;
        c = embi_calloc(1, sz);
        embi_hashmap_put(&h->large, (uint64_t)(uintptr_t)c, (uint64_t)sz);
    } else {
        uint32_t cls = h->class_of[(bytes + 15u) / 16u];
        if (!h->free_lists[cls]) {
            refill_class(h, cls);
        }
        embi_js_free_cell *fc = h->free_lists[cls];
        h->free_lists[cls] = fc->next;
        sz = g_class_size[cls];
        memset(fc, 0, bytes);
        c = (embi_js_cell *)fc;
    }
    c->type = type;
    h->live_bytes += sz;
    h->bytes_since_gc += sz;
    return c;
}

embi_js_cell *embi_js_heap_find_cell(const embi_js_heap *h, uintptr_t addr)
{
    if ((addr & 7u) != 0) {
        return nullptr;
    }
    uint64_t size = 0;
    if (h->large.len != 0 && embi_hashmap_get(&h->large, (uint64_t)addr, &size)) {
        return (embi_js_cell *)addr;
    }
    embi_js_chunk *c = chunk_for(h, addr);
    if (!c) {
        return nullptr;
    }
    size_t off = addr - (uintptr_t)c->base;
    uint8_t cls = c->page_class[off >> EMBI_JS_PAGE_SHIFT];
    if (cls == EMBI_JS_NO_CLASS) {
        return nullptr;
    }
    size_t sz = g_class_size[cls];
    size_t in_page = off & (EMBI_JS_PAGE_BYTES - 1u);
    if (in_page % sz != 0 || in_page / sz >= EMBI_JS_PAGE_BYTES / sz) {
        return nullptr;
    }
    embi_js_cell *cell = (embi_js_cell *)addr;
    return cell->type == EMBI_JS_CELL_FREE ? nullptr : cell;
}

void embi_js_heap_chunk_info(const embi_js_heap *h, uint32_t i, uintptr_t *base, uint32_t *npages,
                             uint32_t *pooled)
{
    EMBI_ASSERT(i < h->chunks.len);
    const embi_js_chunk *c = h->chunks.data[i];
    *base = (uintptr_t)c->base;
    *npages = c->npages;
    *pooled = c->pooled;
}

/* ------------------------------------------------------------------ marking */

#ifdef EMBI_JS_GC_VERIFY
static void verify_visit(embi_js_tracer *t, embi_js_cell *c);
#endif

static void mark_cell(embi_js_tracer *t, embi_js_cell *c)
{
#ifdef EMBI_JS_GC_VERIFY
    if (t->seen != nullptr) {
        verify_visit(t, c);
        return;
    }
#endif
    if (c->mark) {
        return;
    }
    c->mark = 1;
    t->marked_bytes += cell_bytes(t, c);
    embi_vec_push(&t->gray, c);
}

void embi_js_mark_cell(embi_js_tracer *t, embi_js_cell *c)
{
    if (c) {
        mark_cell(t, c);
    }
}

void embi_js_mark_value(embi_js_tracer *t, embi_js_value v)
{
    if (embi_js_is_cell(v)) {
        mark_cell(t, embi_js_as_cell(v));
    }
}

static void drain(embi_js_tracer *t)
{
    while (t->gray.len != 0) {
        embi_js_cell *c = embi_vec_pop(&t->gray);
#ifdef EMBI_JS_GC_VERIFY
        t->owner = c;
        t->field = 0;
#endif
        embi_js_trace_fn fn = t->heap->ops[c->type].trace;
        if (fn) {
            fn(c, t);
        }
    }
#ifdef EMBI_JS_GC_VERIFY
    t->owner = nullptr;
#endif
}

/* Reads every aligned word in [lo, hi) and marks the cells whose first byte they name. The
 * volatile loads keep the compiler from dropping or caching the reads. ASan is disabled because
 * the stack holds redzones this reads on purpose. */
EMBI_NO_ASAN
static void scan_words(embi_js_tracer *t, uintptr_t lo, uintptr_t hi)
{
    for (uintptr_t p = lo; hi - p >= sizeof(uint64_t); p += sizeof(uint64_t)) {
        uint64_t w = *(const volatile uint64_t *)p;
        if (embi_js_is_cell(w)) {
            embi_js_cell *c = embi_js_heap_find_cell(t->heap, (uintptr_t)w);
            if (c) {
                mark_cell(t, c);
            }
        }
    }
}

/* Scans the native stack from lo up to the stack base. lo is the low end of the frame collect_entry
 * passes down (see there), so the scan covers that frame, the callee-saved registers it spilled, and
 * every caller frame above it. Nothing below lo is read: collect and the page walks run below it,
 * and the cell pointers they leave behind on the stack are never roots.
 *
 * The upper bound is the stack base recorded by the outermost embi_js_enter, when that base lies
 * above lo. Outside an enter/leave pair, or when the recorded base is unusable, the scan goes up
 * to the OS thread stack top instead. That is wider than necessary but never misses a frame. */
static void scan_stack(embi_js_runtime *rt, embi_js_tracer *t, uintptr_t lo)
{
    uintptr_t hi = 0;
    if (rt->nest > 0 && rt->stack_base > lo) {
        hi = rt->stack_base;
    }
    if (hi == 0) {
        hi = embi_js_stack_top();
    }
    if (hi <= lo) {
        embi_log_warn("gc: native stack top unknown, skipping the conservative scan");
        return;
    }
    scan_words(t, lo, hi);
}

static void mark_roots(embi_js_runtime *rt, embi_js_tracer *t)
{
    embi_js_atoms *a = &rt->atoms;
    for (uint32_t i = 0; i < a->cap; i++) {
        if (a->slots[i]) {
            embi_js_mark_cell(t, &a->slots[i]->hdr);
        }
    }
    for (uint32_t i = 0; i < rt->persistent.slots.len; i++) {
        const embi_js_handle_slot *s = &rt->persistent.slots.data[i];
        if (s->used) {
            embi_js_mark_value(t, s->value);
        }
    }
    for (uint32_t i = rt->job_head; i < rt->jobs.len; i++) {
        embi_js_mark_value(t, rt->jobs.data[i]);
    }
    embi_js_mark_value(t, rt->pending_exception);
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        rt->roots.data[i].fn(t, rt->roots.data[i].user);
    }
}

void embi_js_trace_ephemeron(embi_js_tracer *t, void *data, embi_js_ephemeron_mark_fn mark,
                             embi_js_ephemeron_clear_fn clear)
{
    embi_js_ephemeron e = {data, mark, clear};
    embi_vec_push(&t->ephemerons, e);
}

/* Marks the values of ephemeron entries whose keys are marked, until no table marks anything new.
 * Each pass drains the mark stack first, so keys reached by values are seen by the next pass. */
static void mark_ephemerons(embi_js_tracer *t)
{
    bool again = true;
    while (again) {
        drain(t);
        again = false;
        for (uint32_t i = 0; i < t->ephemerons.len; i++) {
            embi_js_ephemeron *e = &t->ephemerons.data[i];
            if (e->mark(t, e->data)) {
                again = true;
            }
        }
    }
}

/* Removes the ephemeron entries whose keys did not get marked. Runs after the fixed point. */
static void clear_ephemerons(embi_js_tracer *t)
{
    for (uint32_t i = 0; i < t->ephemerons.len; i++) {
        embi_js_ephemeron *e = &t->ephemerons.data[i];
        e->clear(e->data);
    }
}

static void clear_weak(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->weak.slots.len; i++) {
        embi_js_handle_slot *s = &rt->weak.slots.data[i];
        if (s->used && embi_js_is_cell(s->value) && !embi_js_as_cell(s->value)->mark) {
            s->value = EMBI_JS_EMPTY;
        }
    }
}

/* Marks what each remembered old cell holds. A minor collection starts from here, so an old cell
 * that stored a young cell keeps it alive. The mark stack is drained by the next step. The
 * external bytes the old cell reports are not new growth (they were counted when it was made or
 * promoted), so they are discarded here; the young cells it reaches still count when drained. */
static void trace_remembered(embi_js_heap *h, embi_js_tracer *t)
{
    for (uint32_t i = 0; i < h->remembered.len; i++) {
        embi_js_cell *o = h->remembered.data[i];
        embi_js_trace_fn fn = h->ops[o->type].trace;
        if (fn) {
            size_t ext = t->external;
            fn(o, t);
            t->external = ext;
        }
    }
}

/* Empties the remembered set. Runs after marking, before the sweep, so no freed cell stays in it. */
static void clear_remembered(embi_js_heap *h)
{
    for (uint32_t i = 0; i < h->remembered.len; i++) {
        h->remembered.data[i]->flags = (uint16_t)(h->remembered.data[i]->flags & ~EMBI_JS_FLAG_REMEMBERED);
    }
    h->remembered.len = 0;
}

/* Clears every mark, so a major collection in a generational heap marks from scratch. */
static void clear_marks(embi_js_heap *h)
{
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        for (uint32_t p = 0; p < c->npages; p++) {
            uint8_t cls = c->page_class[p];
            if (cls == EMBI_JS_NO_CLASS) {
                continue;
            }
            size_t sz = g_class_size[cls];
            uint8_t *page = c->base + (size_t)p * EMBI_JS_PAGE_BYTES;
            for (size_t off = 0; off + sz <= EMBI_JS_PAGE_BYTES; off += sz) {
                embi_js_cell *cell = (embi_js_cell *)(page + off);
                if (cell->type != EMBI_JS_CELL_FREE) {
                    cell->mark = 0;
                }
            }
        }
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&h->large);
    uint64_t addr = 0;
    uint64_t size = 0;
    while (embi_hashmap_iter_next(&it, &addr, &size)) {
        ((embi_js_cell *)(uintptr_t)addr)->mark = 0;
    }
}

/* ------------------------------------------------------------------ verifier (debug builds) */

#ifdef EMBI_JS_GC_VERIFY

/* The verifier's full mark: a cell is visited once, and nothing in it changes a mark bit. An edge
 * from an old cell (mark set outside this minor) to a cell the minor did not mark is a missing
 * write barrier. */
static void verify_visit(embi_js_tracer *t, embi_js_cell *c)
{
    t->field++;
    if (t->owner != nullptr && t->owner->mark != 0 && c->mark == 0) {
        embi_log_error("gc verify: old cell %p (type %u) holds the young cell %p (type %u) in "
                       "field %u, which the minor collection did not mark: a write barrier is "
                       "missing",
                       (void *)t->owner, (unsigned)t->owner->type, (void *)c, (unsigned)c->type,
                       (unsigned)t->field);
        abort();
    }
    uint64_t seen = 0;
    if (embi_hashmap_get(t->seen, (uint64_t)(uintptr_t)c, &seen)) {
        return;
    }
    embi_hashmap_put(t->seen, (uint64_t)(uintptr_t)c, 1);
    embi_vec_push(&t->gray, c);
}

/* Runs before a minor sweep, after its marking. The ephemeron callbacks read the collector's
 * marks, not this traversal's, so this pass leaves ephemeron values out (it can miss a barrier
 * fault that only reaches a value through a weak table; it never reports a false one). */
static void verify_minor(embi_js_runtime *rt, uintptr_t lo)
{
    embi_hashmap seen;
    embi_hashmap_init(&seen);
    embi_js_tracer v = {.heap = &rt->heap,
                        .gray = {nullptr, 0, 0},
                        .ephemerons = {nullptr, 0, 0},
                        .external = 0,
                        .marked_bytes = 0,
                        .seen = &seen,
                        .owner = nullptr,
                        .field = 0};
    scan_stack(rt, &v, lo);
    mark_roots(rt, &v);
    drain(&v);
    embi_vec_free(&v.gray);
    embi_vec_free(&v.ephemerons);
    embi_hashmap_free(&seen);
}

#endif /* EMBI_JS_GC_VERIFY */

/* ------------------------------------------------------------------ sweeping */

/* Sweeps one size-class page. Dead cells are finalized and turned into FREE cells. Survivors keep
 * their marks in a generational heap (sticky) and lose them otherwise. A page left with no live
 * cell goes back to the pool. Otherwise its free cells are linked onto the class free list (the
 * lists were emptied when the collection started, so every free cell of the page is linked here)
 * and the page is recorded as young, because the allocator can take cells from it. */
static void sweep_page(embi_js_heap *h, embi_js_chunk *c, uint32_t p, uint8_t cls)
{
    size_t sz = g_class_size[cls];
    uint8_t *page = c->base + (size_t)p * EMBI_JS_PAGE_BYTES;
    uint32_t n = (uint32_t)(EMBI_JS_PAGE_BYTES / sz);
    uint32_t live_n = 0;
    uint32_t free_n = 0;

    for (uint32_t i = 0; i < n; i++) {
        embi_js_cell *cell = (embi_js_cell *)(page + (size_t)i * sz);
        if (cell->type == EMBI_JS_CELL_FREE) {
            free_n++;
            continue;
        }
        if (cell->mark) {
            if (!h->generational) {
                cell->mark = 0;
            }
            live_n++;
            continue;
        }
        if (h->ops[cell->type].finalize) {
            h->ops[cell->type].finalize(cell);
        }
        cell->type = EMBI_JS_CELL_FREE;
        cell->flags = 0;
        cell->aux = 0;
        h->live_bytes -= sz;
        free_n++;
    }

    if (live_n == 0) {
        pool_push(h, c, p);
        return;
    }
    if (free_n == 0) {
        return;
    }
    for (uint32_t i = n; i-- > 0;) {
        embi_js_cell *cell = (embi_js_cell *)(page + (size_t)i * sz);
        if (cell->type == EMBI_JS_CELL_FREE) {
            embi_js_free_cell *fc = (embi_js_free_cell *)cell;
            fc->next = h->free_lists[cls];
            h->free_lists[cls] = fc;
        }
    }
    page_set_young(h, c, p);
}

/* Sweeps every queued page now. The pages it links (or pools) are then the young set. */
static void finish_sweep(embi_js_heap *h)
{
    for (uint32_t cls = 0; cls < EMBI_JS_NUM_CLASSES; cls++) {
        while (h->unswept[cls].len != 0) {
            embi_js_page_ref r = embi_vec_pop(&h->unswept[cls]);
            h->unswept_total--;
            sweep_page(h, r.chunk, r.index, (uint8_t)cls);
        }
    }
    if (h->release_pending) {
        h->release_pending = false;
        release_empty_chunks(h);
    }
}

/* Large cells are swept eagerly: each one is its own allocation, so freeing it needs no page. */
static void sweep_large(embi_js_heap *h)
{
    EMBI_VEC(large_ref) dead = {nullptr, 0, 0};
    embi_hashmap_iter it = embi_hashmap_iter_begin(&h->large);
    uint64_t addr = 0;
    uint64_t size = 0;
    while (embi_hashmap_iter_next(&it, &addr, &size)) {
        embi_js_cell *cell = (embi_js_cell *)(uintptr_t)addr;
        if (cell->mark) {
            if (!h->generational) {
                cell->mark = 0;
            }
            continue;
        }
        if (h->ops[cell->type].finalize) {
            h->ops[cell->type].finalize(cell);
        }
        large_ref r = {(uintptr_t)addr, (size_t)size};
        embi_vec_push(&dead, r);
    }
    /* Removing while iterating would invalidate the iterator, so remove afterwards. */
    for (uint32_t i = 0; i < dead.len; i++) {
        embi_hashmap_remove(&h->large, (uint64_t)dead.data[i].addr);
        h->live_bytes -= dead.data[i].size;
        embi_free((void *)dead.data[i].addr, dead.data[i].size);
    }
    embi_vec_free(&dead);
}

/* Moves the young set into the sweep queues (a minor collection), or queues every page (a major
 * one). The young set starts again empty, so it collects the pages the next window allocates in. */
static void queue_sweep(embi_js_heap *h, bool major)
{
    if (major) {
        for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
            embi_js_chunk *c = h->chunks.data[ci];
            for (uint32_t p = 0; p < c->npages; p++) {
                if (c->page_class[p] != EMBI_JS_NO_CLASS) {
                    embi_js_page_ref r = {c, p};
                    embi_vec_push(&h->unswept[c->page_class[p]], r);
                    h->unswept_total++;
                }
            }
        }
    }
    for (uint32_t i = 0; i < h->young.len; i++) {
        embi_js_page_ref r = h->young.data[i];
        r.chunk->page_young[r.index] = 0;
        if (!major) {
            embi_vec_push(&h->unswept[r.chunk->page_class[r.index]], r);
            h->unswept_total++;
        }
    }
    h->young.len = 0;
}

/* ------------------------------------------------------------------ collection */

static void note_alloc_rate(embi_js_heap *h, uint64_t now)
{
    if (h->last_end_ns != 0 && now > h->last_end_ns) {
        double ms = (double)(now - h->last_end_ns) / 1e6;
        double rate = (double)h->bytes_since_gc / ms;
        h->alloc_rate = h->alloc_rate == 0.0 ? rate : 0.75 * h->alloc_rate + 0.25 * rate;
    }
}

/* Collection entry. Its frame is the lowest frame the conservative scan reads. The scan starts at lo,
 * the lower of this frame's frame address and the address of spill (its local buffer), so it covers
 * this frame and every caller above it, and nothing deeper.
 *
 * Layout assumption (not tied to one ABI): the callee-saved registers the caller keeps are pushed by
 * this function's prologue (__builtin_unwind_init forces that), into the frame between the frame
 * address and the locals on x86-64, and between the frame record and the locals on AArch64 GCC, where
 * the locals sit above the callee-save area. Taking the lower of the two bounds covers both layouts:
 * the frame record is below the saves on AArch64, and spill is below them on x86-64. The Win64
 * xmm6..xmm15 are copied into spill. spill is zeroed first, so no stale word of an earlier frame lies
 * in the scanned part of this frame. Test: test_js_gc_precise keeps a cell only in a register of the
 * caller across a collection. */
static EMBI_NOINLINE void collect_entry(embi_js_runtime *rt, bool major)
{
    volatile uint64_t spill[20];
    __builtin_unwind_init();
    for (size_t i = 0; i < 20; i++) {
        spill[i] = 0;
    }
#if defined(_WIN32) && defined(__x86_64__)
    /* Win64 keeps xmm6..xmm15 callee-saved, so a pointer can live only in one of them. */
    __asm__ volatile("movdqu %%xmm6, 0(%0)\n\t"
                     "movdqu %%xmm7, 16(%0)\n\t"
                     "movdqu %%xmm8, 32(%0)\n\t"
                     "movdqu %%xmm9, 48(%0)\n\t"
                     "movdqu %%xmm10, 64(%0)\n\t"
                     "movdqu %%xmm11, 80(%0)\n\t"
                     "movdqu %%xmm12, 96(%0)\n\t"
                     "movdqu %%xmm13, 112(%0)\n\t"
                     "movdqu %%xmm14, 128(%0)\n\t"
                     "movdqu %%xmm15, 144(%0)\n\t"
                     :
                     : "r"(spill)
                     : "memory");
#endif
    uintptr_t lo = (uintptr_t)spill;
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    if (fp < lo) {
        lo = fp;
    }
    collect(rt, major, lo);
}

/* A generational heap collects minor until old growth reaches the major limit. Otherwise every
 * collection is major. lo bounds the conservative stack scan (see collect_entry). */
static EMBI_NOINLINE void collect(embi_js_runtime *rt, bool major, uintptr_t lo)
{
    embi_js_heap *h = &rt->heap;
    note_alloc_rate(h, SDL_GetTicksNS());
    /* The previous sweep must finish first: its marks are still set on the pages it has not
     * reached, and its free cells are not linked yet. */
    finish_sweep(h);
    if (!h->generational) {
        major = true;
    }

    embi_js_tracer t = {.heap = h,
                        .gray = {nullptr, 0, 0},
                        .ephemerons = {nullptr, 0, 0},
                        .external = 0,
                        .marked_bytes = 0};
#ifdef EMBI_JS_GC_VERIFY
    t.seen = nullptr;
    t.owner = nullptr;
    t.field = 0;
#endif
    h->in_gc = true;

    if (major && h->generational) {
        clear_marks(h);
    }
    /* Stub cache entries name shapes, and an unmarked shape may be freed below: drop them all. */
    embi_js_stub_cache_clear(rt);
    scan_stack(rt, &t, lo);
    mark_roots(rt, &t);
    if (!major) {
        trace_remembered(h, &t);
    }
    mark_ephemerons(&t);
#ifdef EMBI_JS_GC_VERIFY
    if (!major) {
        verify_minor(rt, lo);
    }
#endif
    clear_ephemerons(&t);
    clear_weak(rt);
    clear_remembered(h);

    /* The free lists are rebuilt from the pages that get swept. Every free cell of a page sits on
     * a list only after that page is swept, so nothing allocates from a page with unswept dead
     * cells. */
    memset(h->free_lists, 0, sizeof h->free_lists);
    queue_sweep(h, major);
    sweep_large(h);
    embi_vec_free(&t.gray);
    embi_vec_free(&t.ephemerons);

    size_t marked = t.marked_bytes + t.external;
    h->in_gc = false;
    h->bytes_since_gc = 0;
    h->gc_count++;
    if (major) {
        h->major_count++;
        h->major_live = marked;
        h->old_growth = 0;
        h->live_after_gc = marked;
        h->major_limit = gc_budget(h, marked, false);
        h->release_pending = true;
    } else {
        h->minor_count++;
        h->old_growth += marked;
        /* J5m3: a minor sweep can empty whole chunks too, so it releases them as a major does. */
        h->release_pending = true;
        h->live_after_gc = h->major_live + h->old_growth;
    }
    h->threshold = gc_budget(h, h->live_after_gc, h->generational);
    h->last_end_ns = SDL_GetTicksNS();
}

/* Collects for an allocation that found the budget spent. Minor or major, by the policy. */
static void collect_for_alloc(embi_js_runtime *rt)
{
    embi_js_heap *h = &rt->heap;
    bool major = !h->generational || h->old_growth >= h->major_limit;
    collect_entry(rt, major);
    /* Outside the collection, so the cleanup jobs of dead FinalizationRegistry targets may
     * allocate (jobs.h). */
    embi_js_weakref_after_gc(rt);
}

void embi_js_gc(embi_js_runtime *rt)
{
    if (rt->heap.in_gc) {
        return;
    }
    collect_entry(rt, true);
    finish_sweep(&rt->heap);
    /* Outside the collection, so the cleanup jobs of dead FinalizationRegistry targets may
     * allocate (jobs.h). */
    embi_js_weakref_after_gc(rt);
}

int embi_js_gc_idle(embi_js_runtime *rt)
{
    embi_js_heap *h = &rt->heap;
    if (h->in_gc || h->gc_hold != 0 || h->stress_every != 0 || (h->bytes_since_gc == 0 && h->old_growth == 0)) {
        return 0; /* nothing allocated since the last collection: also keeps tiny test thresholds from collecting each frame */
    }
    /* The collection the next allocations would make anyway once half its budget is spent: a major one when the old
     * generation has grown by half its limit (non-generational: when half the budget is allocated), else a minor one. */
    bool major = h->generational ? h->old_growth >= h->major_limit / 2u : h->bytes_since_gc >= h->threshold / 2u;
    if (major) {
        embi_js_gc(rt);
        return 2;
    }
    if (h->generational && h->bytes_since_gc >= h->threshold / 2u) {
        embi_js_gc_minor(rt);
        return 1;
    }
    return 0;
}

void embi_js_gc_minor(embi_js_runtime *rt)
{
    if (rt->heap.in_gc) {
        return;
    }
    collect_entry(rt, false);
    finish_sweep(&rt->heap);
    embi_js_weakref_after_gc(rt);
}

void embi_js_gc_set_generational(embi_js_runtime *rt, bool on)
{
    embi_js_heap *h = &rt->heap;
    EMBI_ASSERT(!h->in_gc);
    if (on) {
        if (!h->generational) {
            /* A non-generational collection leaves marks behind: survivors on the pages it has
             * not swept yet still carry mark 1, while swept survivors were cleared. Sticky marks
             * need every mark clear at this point, otherwise the stale ones look old and their
             * young children are freed by the next minor. Finishing the sweep clears the swept
             * survivors' marks and links their free cells; clear_marks covers the rest. */
            finish_sweep(h);
            clear_remembered(h);
            clear_marks(h);
            /* Every existing cell is young now: a minor sweeps only the young pages, and cells
             * that died before this point sit on pages that were full (so not young). Left
             * unswept they would survive as zombies pointing at children the first minor frees. */
            for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
                embi_js_chunk *c = h->chunks.data[ci];
                for (uint32_t p = 0; p < c->npages; p++) {
                    if (c->page_class[p] != EMBI_JS_NO_CLASS) {
                        page_set_young(h, c, p);
                    }
                }
            }
            h->generational = true;
            embi_js_gc_retune(h);
        }
        embi_js_wb_heap = h;
        return;
    }
    /* Off: the sticky marks must go, because a non-generational sweep expects them clear. */
    finish_sweep(h);
    clear_remembered(h);
    clear_marks(h);
    h->generational = false;
    if (embi_js_wb_heap == h) {
        embi_js_wb_heap = nullptr;
    }
}

void embi_js_gc_set_stress(embi_js_runtime *rt, uint32_t every)
{
    rt->heap.stress_every = every;
    rt->heap.stress_left = every;
}

/* Puts an old owner on the remembered set once: its trace runs in the next minor. */
static void wb_remember(embi_js_heap *h, embi_js_cell *owner)
{
    if ((owner->flags & EMBI_JS_FLAG_REMEMBERED) != 0) {
        return;
    }
    owner->flags = (uint16_t)(owner->flags | EMBI_JS_FLAG_REMEMBERED);
    embi_vec_push(&h->remembered, owner);
    if (h->remembered.len > h->remembered_peak) {
        h->remembered_peak = h->remembered.len;
    }
}

void embi_js_wb_slow(embi_js_heap *h, embi_js_cell *owner, uint64_t bits)
{
    if (owner == nullptr || owner->mark == 0 || !embi_js_is_cell(bits)) {
        return;
    }
    embi_js_cell *v = embi_js_ptr_decode(bits);
    if (v->mark != 0) {
        return;
    }
    wb_remember(h, owner);
}

void embi_js_wb_struct_slow(embi_js_heap *h, embi_js_cell *owner)
{
    if (owner == nullptr || owner->mark == 0) {
        return;
    }
    wb_remember(h, owner);
}

/* J5n2 diagnostic: the non-free cells by type after the sweep is finished (call it after
 * embi_js_gc). count[t] and bytes[t] (size-class bytes; large cells use their own size) are indexed
 * by cell type. Used by bench_js_suite (EMBER_CENSUS=1) to see what a workload leaves alive. */
void embi_js_gc_census(embi_js_heap *h, size_t count[EMBI_JS_CELL_TYPE_COUNT],
                       size_t bytes[EMBI_JS_CELL_TYPE_COUNT],
                       void (*visit)(const embi_js_cell *, void *), void *user)
{
    finish_sweep(h);
    for (uint32_t ci = 0; ci < h->chunks.len; ci++) {
        embi_js_chunk *c = h->chunks.data[ci];
        for (uint32_t p = 0; p < c->npages; p++) {
            uint8_t cls = c->page_class[p];
            if (cls == EMBI_JS_NO_CLASS) {
                continue;
            }
            size_t sz = g_class_size[cls];
            uint8_t *page = c->base + (size_t)p * EMBI_JS_PAGE_BYTES;
            for (size_t off = 0; off + sz <= EMBI_JS_PAGE_BYTES; off += sz) {
                const embi_js_cell *cell = (const embi_js_cell *)(page + off);
                if (cell->type != EMBI_JS_CELL_FREE && cell->type < EMBI_JS_CELL_TYPE_COUNT) {
                    count[cell->type]++;
                    bytes[cell->type] += sz;
                    if (visit) {
                        visit(cell, user);
                    }
                }
            }
        }
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&h->large);
    uint64_t addr = 0;
    uint64_t size = 0;
    while (embi_hashmap_iter_next(&it, &addr, &size)) {
        const embi_js_cell *cell = (const embi_js_cell *)(uintptr_t)addr;
        if (cell->type < EMBI_JS_CELL_TYPE_COUNT) {
            count[cell->type]++;
            bytes[cell->type] += (size_t)size;
            if (visit) {
                visit(cell, user);
            }
        }
    }
}
