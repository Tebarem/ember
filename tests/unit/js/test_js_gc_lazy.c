#include "emb_test.h"

#include "js/js.h"

/* J5h: lazy sweeping, finalizers that run once, and chunk release. Pages queued by a collection
 * are swept when the allocator needs them, and an explicit collection finishes the sweep. */

typedef struct lazy_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} lazy_node;

static int g_lazy_finalized;

static void lazy_trace(embi_js_cell *c, embi_js_tracer *t)
{
    lazy_node *n = (lazy_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static void lazy_finalize(embi_js_cell *c)
{
    (void)c;
    g_lazy_finalized++;
}

static void install_lazy_type(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){lazy_trace, lazy_finalize});
}

static embi_js_value lazy_new(embi_js_runtime *rt, embi_js_value next, embi_js_value payload)
{
    lazy_node *n = (lazy_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
    n->next = next;
    n->payload = payload;
    return embi_js_from_cell(n);
}

static EMB_TEST_NOIPA void scrub_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub() scrub_impl(0, 0, 0, 0)

/* Unrooted 32-byte nodes. */
static EMB_TEST_NOIPA void garbage(embi_js_runtime *rt, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)lazy_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    }
}

/* A chain of `count` nodes, rooted through the returned persistent id. */
static EMB_TEST_NOIPA uint32_t rooted_chain(embi_js_runtime *rt, uint32_t count)
{
    embi_js_value head = EMBI_JS_UNDEFINED;
    for (uint32_t i = 0; i < count; i++) {
        head = lazy_new(rt, head, embi_js_from_int32((int32_t)i));
    }
    return embi_js_persistent_new(rt, head);
}

/* Walks the chain and checks every payload: rooted_chain makes the head the last node, so the
 * payloads run count-1 down to 0. Returns the number of nodes. */
static EMB_TEST_NOIPA uint32_t walk_chain(embi_js_runtime *rt, uint32_t id, uint32_t count, bool *ok)
{
    embi_js_value v = embi_js_persistent_get(rt, id);
    uint32_t n = 0;
    while (embi_js_is_cell(v)) {
        lazy_node *c = (lazy_node *)embi_js_as_cell(v);
        if (!embi_js_is_int32(c->payload) || embi_js_as_int32(c->payload) != (int32_t)(count - 1u - n)) {
            *ok = false;
        }
        n++;
        v = c->next;
    }
    return n;
}

/* ------------------------------------------------------------------ finalizers run once */

static EMB_TEST_NOIPA void finalized_once_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_lazy_type(rt);
    g_lazy_finalized = 0;
    /* Several automatic collections happen during this loop, each queueing pages for the lazy
     * sweep. A page swept by refill must not be swept again by the next collection. */
    const uint32_t made_a = 60000; /* about 1.9 MB */
    garbage(rt, made_a);
    scrub();
    embi_js_gc(rt);
    int after_a = g_lazy_finalized;
    EMB_CHECK(after_a >= (int)made_a - 64);
    EMB_CHECK(after_a <= (int)made_a);
    EMB_CHECK(rt->heap.unswept_total == 0);

    const uint32_t made_b = 60000;
    garbage(rt, made_b);
    scrub();
    embi_js_gc(rt);
    int after_b = g_lazy_finalized;
    EMB_CHECK(after_b >= (int)(made_a + made_b) - 64);
    EMB_CHECK(after_b <= (int)(made_a + made_b));
    EMB_CHECK(rt->heap.unswept_total == 0);
    embi_js_runtime_free(rt);
    /* Runtime teardown finalizes what is left alive, and nothing twice. */
    EMB_CHECK(g_lazy_finalized <= (int)(made_a + made_b));
}

EMB_TEST(gc_lazy_sweep_finalizes_each_dead_cell_once)
{
    scrub();
    finalized_once_body();
    scrub();
}

/* ------------------------------------------------------------------ the sweep finishes first */

static EMB_TEST_NOIPA void lazy_survivors_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_lazy_type(rt);
    uint32_t id = rooted_chain(rt, 3000);
    /* Enough garbage that automatic collections run, with lazily swept pages in between. */
    garbage(rt, 60000);
    scrub();
    bool ok = true;
    uint32_t n = walk_chain(rt, id, 3000u, &ok);
    EMB_CHECK(n == 3000u);
    EMB_CHECK(ok);
    embi_js_gc(rt);
    ok = true;
    n = walk_chain(rt, id, 3000u, &ok);
    EMB_CHECK(n == 3000u);
    EMB_CHECK(ok);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_lazy_sweep_keeps_rooted_survivors_intact)
{
    scrub();
    lazy_survivors_body();
    scrub();
}

/* ------------------------------------------------------------------ chunk release */

/* Chunks that are entirely in the pool. */
static uint32_t fully_pooled_chunks(embi_js_runtime *rt)
{
    uint32_t n = 0;
    for (uint32_t ci = 0; ci < rt->heap.chunks.len; ci++) {
        uintptr_t base;
        uint32_t np;
        uint32_t pooled;
        embi_js_heap_chunk_info(&rt->heap, ci, &base, &np, &pooled);
        if (pooled == np) {
            n++;
        }
    }
    return n;
}

static EMB_TEST_NOIPA void release_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_lazy_type(rt);
    uint32_t id = rooted_chain(rt, 2000);
    /* A 4 MB floor lets the heap grow to about 16 chunks before the first collection. */
    embi_js_set_gc_params(rt, (size_t)4u << 20, 1.5);
    /* About 8 MB of garbage: the first collection runs halfway through. */
    garbage(rt, 250000);
    scrub();
    embi_js_mem_stats peak;
    embi_js_get_mem_stats(rt, &peak);
    EMB_CHECK(peak.heap_chunks >= 4u);

    embi_js_gc(rt);
    embi_js_mem_stats after;
    embi_js_get_mem_stats(rt, &after);
    EMB_CHECK(after.released_chunks > 0u);
    EMB_CHECK(after.heap_chunks < peak.heap_chunks);
    EMB_CHECK(after.heap_chunks <= 3u);
    /* One spare chunk is kept: the live data fits in one chunk, so a second one stays, pooled. */
    EMB_CHECK(after.heap_chunks >= 2u);
    EMB_CHECK(fully_pooled_chunks(rt) == 1u);

    /* The released memory is usable again, and the rooted chain is unharmed. */
    garbage(rt, 100000);
    scrub();
    bool ok = true;
    uint32_t n = walk_chain(rt, id, 2000u, &ok);
    EMB_CHECK(n == 2000u);
    EMB_CHECK(ok);
    embi_js_gc(rt);
    ok = true;
    n = walk_chain(rt, id, 2000u, &ok);
    EMB_CHECK(n == 2000u);
    EMB_CHECK(ok);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

/* J5m3: a minor collection releases its empty chunks too, not only a major one. The chain is old
 * before the garbage exists, the floor is above the garbage (so no automatic collection runs), and
 * both majors have finished their sweeps, so release_pending is clear when the minor starts. The
 * only release left is the one the minor sweep requests. */
static EMB_TEST_NOIPA void minor_release_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_lazy_type(rt);
    embi_js_gc_set_generational(rt, true);
    uint32_t id = rooted_chain(rt, 2000);
    embi_js_gc(rt);
    embi_js_gc(rt);
    embi_js_set_gc_params(rt, (size_t)16u << 20, 1.5);
    /* About 8 MB of garbage, all young, carved into several chunks. */
    garbage(rt, 250000);
    scrub();
    embi_js_mem_stats before;
    embi_js_get_mem_stats(rt, &before);
    EMB_CHECK(before.heap_chunks >= 4u);
    embi_js_gc_minor(rt);
    embi_js_mem_stats after;
    embi_js_get_mem_stats(rt, &after);
    EMB_CHECK(after.minor_count == before.minor_count + 1u);
    EMB_CHECK(after.major_count == before.major_count);
    EMB_CHECK(after.released_chunks > before.released_chunks);
    EMB_CHECK(after.heap_chunks < before.heap_chunks);
    EMB_CHECK(fully_pooled_chunks(rt) == 1u);
    bool ok = true;
    uint32_t n = walk_chain(rt, id, 2000u, &ok);
    EMB_CHECK(n == 2000u);
    EMB_CHECK(ok);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_minor_collection_releases_empty_chunks)
{
    scrub();
    minor_release_body();
    scrub();
}

EMB_TEST(gc_chunks_are_released_after_a_major_and_the_heap_still_works)
{
    scrub();
    release_body();
    scrub();
}

/* ------------------------------------------------------------------ chunk sizes and lookup */

enum { CHAIN_NODES = 40000 };
static uintptr_t g_chain_addr[CHAIN_NODES];

/* J5m3: chunks start at 4 pages (64 KB) and double to 16 pages (256 KB). No collection runs (the
 * floor is far above the data), so nothing is released, and the chunks in creation order are 4, 8,
 * 16, 16, ... pages. chunk_info lists them by address, so the test counts sizes instead of reading
 * them by index. Every cell of the chain is found by heap_find_cell inside one of the chunks. */
static EMB_TEST_NOIPA void chunk_growth_body(void)
{
    size_t mem0 = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    install_lazy_type(rt);
    embi_js_set_gc_params(rt, (size_t)64u << 20, 1.5);
    uint32_t id = rooted_chain(rt, CHAIN_NODES);

    uint32_t n = 0;
    embi_js_value v = embi_js_persistent_get(rt, id);
    while (embi_js_is_cell(v) && n < CHAIN_NODES) {
        g_chain_addr[n++] = (uintptr_t)embi_js_as_cell(v);
        v = ((lazy_node *)embi_js_as_cell(v))->next;
    }
    EMB_CHECK(n == CHAIN_NODES);

    uint32_t nch = rt->heap.chunks.len;
    EMB_CHECK(nch >= 3u);
    EMB_CHECK(nch <= 64u);
    uint32_t c4 = 0;
    uint32_t c8 = 0;
    uint32_t c16 = 0;
    uint32_t other = 0;
    for (uint32_t ci = 0; ci < nch; ci++) {
        uintptr_t base;
        uint32_t np;
        uint32_t pooled;
        embi_js_heap_chunk_info(&rt->heap, ci, &base, &np, &pooled);
        if (np == 4u) {
            c4++;
        } else if (np == 8u) {
            c8++;
        } else if (np == 16u) {
            c16++;
        } else {
            other++;
        }
    }
    EMB_CHECK(other == 0u);
    EMB_CHECK(c4 == 1u);
    EMB_CHECK(c8 == 1u);
    EMB_CHECK(c16 == nch - 2u);

    uint32_t hits[64] = {0};
    for (uint32_t k = 0; k < CHAIN_NODES; k++) {
        uintptr_t a = g_chain_addr[k];
        EMB_CHECK((uintptr_t)embi_js_heap_find_cell(&rt->heap, a) == a);
        for (uint32_t ci = 0; ci < nch; ci++) {
            uintptr_t base;
            uint32_t np;
            uint32_t pooled;
            embi_js_heap_chunk_info(&rt->heap, ci, &base, &np, &pooled);
            if (a - base < ((size_t)np << EMBI_JS_PAGE_SHIFT)) {
                hits[ci]++;
                break;
            }
        }
    }
    uint32_t chunks_hit = 0;
    for (uint32_t ci = 0; ci < nch; ci++) {
        if (hits[ci] != 0u) {
            chunks_hit++;
        }
    }
    EMB_CHECK(chunks_hit >= 3u);

    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(gc_chunks_double_from_64kb_to_256kb_and_lookup_works)
{
    scrub();
    chunk_growth_body();
    scrub();
}

EMB_TEST_MAIN()
