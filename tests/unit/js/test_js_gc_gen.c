#include "emb_test.h"

#include "js/js.h"

/* J5h: generational mode (sticky marks, minor collections, the remembered set through
 * EMBI_JS_WB). It is off by default; these tests turn it on for their own runtime. In debug builds
 * every minor collection also runs the verifier, which aborts when an old cell holds a young one
 * that the minor did not mark. */

typedef struct gen_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} gen_node;

static int g_gen_finalized;

static void gen_trace(embi_js_cell *c, embi_js_tracer *t)
{
    gen_node *n = (gen_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static void gen_finalize(embi_js_cell *c)
{
    (void)c;
    g_gen_finalized++;
}

static void install_gen_type(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){gen_trace, gen_finalize});
}

static embi_js_value gen_new(embi_js_runtime *rt, embi_js_value next, embi_js_value payload)
{
    gen_node *n = (gen_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
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

static EMB_TEST_NOIPA void garbage(embi_js_runtime *rt, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    }
}

static bool is_marked(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->mark != 0;
}

/* ------------------------------------------------------------------ survivors and sticky marks */

static EMB_TEST_NOIPA void sticky_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value head = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(7));
    uint32_t id = embi_js_persistent_new(rt, head);
    embi_js_gc(rt); /* a major: the rooted cell becomes old, and so sticky-marked */
    EMB_CHECK(is_marked(head));

    garbage(rt, 2000);
    scrub();
    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    embi_js_gc_minor(rt);
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    EMB_CHECK(s1.minor_count == s0.minor_count + 1u);
    EMB_CHECK(s1.major_count == s0.major_count);
    /* The old root kept its sticky mark, and the value is still there. */
    embi_js_value v = embi_js_persistent_get(rt, id);
    EMB_CHECK(is_marked(v));
    EMB_CHECK(embi_js_as_int32(((gen_node *)embi_js_as_cell(v))->payload) == 7);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_minor_keeps_old_survivors_sticky)
{
    scrub();
    sticky_body();
    scrub();
}

/* ------------------------------------------------------------------ the barrier */

/* An old owner stores a young cell. Only the barrier tells the minor collection about it: the
 * young cell is reachable from nothing else. */
static EMB_TEST_NOIPA void barrier_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value owner = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    uint32_t id = embi_js_persistent_new(rt, owner);
    embi_js_gc(rt); /* the owner is old now */

    embi_js_value young = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(42));
    gen_node *o = (gen_node *)embi_js_as_cell(embi_js_persistent_get(rt, id));
    o->payload = young;
    EMBI_JS_WB(o, young);
    EMB_CHECK(rt->heap.remembered.len == 1u);
    young = EMBI_JS_UNDEFINED;
    scrub();

    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    embi_js_gc_minor(rt);
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    EMB_CHECK(s1.remembered_peak >= 1u);
    EMB_CHECK(rt->heap.remembered.len == 0u);
    EMB_CHECK(s1.minor_count == s0.minor_count + 1u);

    /* Reuse the memory to catch a young cell that was freed in error. */
    garbage(rt, 4000);
    scrub();
    o = (gen_node *)embi_js_as_cell(embi_js_persistent_get(rt, id));
    embi_js_value kept = o->payload;
    EMB_CHECK(embi_js_is_cell(kept));
    EMB_CHECK(embi_js_is_cell(kept) && embi_js_as_int32(((gen_node *)embi_js_as_cell(kept))->payload) == 42);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_barrier_keeps_young_cells_held_by_old_owners)
{
    scrub();
    barrier_body();
    scrub();
}

/* The barrier does nothing for a young owner, nor for an old owner that stores an old value. */
static EMB_TEST_NOIPA void barrier_filter_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value a = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    embi_js_value b = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    uint32_t id = embi_js_persistent_new(rt, a);
    /* Both are young: storing one into the other needs no entry. */
    EMBI_JS_WB(embi_js_as_cell(a), b);
    EMB_CHECK(rt->heap.remembered.len == 0u);
    embi_js_gc(rt); /* both become old */
    EMBI_JS_WB(embi_js_as_cell(a), b);
    EMB_CHECK(rt->heap.remembered.len == 0u);
    /* A young value stored twice into the same old owner is remembered once. */
    embi_js_value c = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    EMBI_JS_WB(embi_js_as_cell(a), c);
    EMBI_JS_WB(embi_js_as_cell(a), c);
    EMB_CHECK(rt->heap.remembered.len == 1u);
    embi_js_gc_minor(rt);
    EMB_CHECK(rt->heap.remembered.len == 0u);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_barrier_ignores_young_owners_and_dedupes)
{
    scrub();
    barrier_filter_body();
    scrub();
}

/* ------------------------------------------------------------------ minors never free old cells */

static EMB_TEST_NOIPA void old_dead_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value c = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    uint32_t id = embi_js_persistent_new(rt, c);
    embi_js_gc(rt);
    c = EMBI_JS_UNDEFINED;
    embi_js_persistent_free(rt, id); /* unreachable now, but old */
    scrub();

    g_gen_finalized = 0;
    embi_js_gc_minor(rt);
    EMB_CHECK(g_gen_finalized == 0);
    embi_js_gc(rt); /* the major collection frees it */
    /* Under AddressSanitizer a stale stack word can still name the cell, so only the upper bound
     * is exact there (see EMB_TEST_ASAN in emb_test.h). */
    EMB_CHECK(g_gen_finalized <= 1);
    EMB_CHECK(EMB_TEST_ASAN || g_gen_finalized == 1);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_minor_leaves_old_dead_cells_to_the_major)
{
    scrub();
    old_dead_body();
    scrub();
}

static EMB_TEST_NOIPA void young_dead_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_gc(rt);
    scrub();
    g_gen_finalized = 0;
    garbage(rt, 3000);
    scrub();
    embi_js_gc_minor(rt);
    /* The young garbage sits on pages the minor swept, so it is finalized now. */
    EMB_CHECK(g_gen_finalized >= 3000 - 64);
    EMB_CHECK(g_gen_finalized <= 3000);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_minor_frees_dead_young_cells)
{
    scrub();
    young_dead_body();
    scrub();
}

/* ------------------------------------------------------------------ stress across minors */

/* A list grows at its tail, one node per step. The tail is old whenever a collection ran since it
 * was made, so the link from it to the new node goes through the barrier. A small stress interval
 * makes minors and majors interleave with the growth. Debug builds verify every minor. */
static EMB_TEST_NOIPA void stress_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value head = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(0));
    uint32_t id = embi_js_persistent_new(rt, head);
    embi_js_gc(rt);
    embi_js_gc_set_stress(rt, 97);

    const int32_t count = 6000;
    gen_node *tail = (gen_node *)embi_js_as_cell(head);
    for (int32_t i = 1; i < count; i++) {
        embi_js_value n = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(i));
        tail->next = n;
        EMBI_JS_WB(tail, n);
        tail = (gen_node *)embi_js_as_cell(n);
    }
    embi_js_gc_set_stress(rt, 0);
    scrub();

    embi_js_mem_stats s;
    embi_js_get_mem_stats(rt, &s);
    EMB_CHECK(s.minor_count > 0u);

    int32_t seen = 0;
    bool ok = true;
    embi_js_value v = embi_js_persistent_get(rt, id);
    while (embi_js_is_cell(v)) {
        gen_node *c = (gen_node *)embi_js_as_cell(v);
        if (!embi_js_is_int32(c->payload) || embi_js_as_int32(c->payload) != seen) {
            ok = false;
        }
        seen++;
        v = c->next;
    }
    EMB_CHECK(ok);
    EMB_CHECK(seen == count);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_stress_list_survives_minors_and_majors)
{
    scrub();
    stress_body();
    scrub();
}

/* ------------------------------------------------------------------ enabling after a sweep */

/* A child in a different size class from the parents (96 or 112 bytes against 32). */
typedef struct gen_big {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
    uint8_t pad[80];
} gen_big;

static embi_js_value big_new(embi_js_runtime *rt, embi_js_value payload)
{
    gen_big *b = (gen_big *)embi_js_alloc_cell(rt, sizeof *b, EMBI_JS_CELL_SHAPE);
    b->next = EMBI_JS_UNDEFINED;
    b->payload = payload;
    return embi_js_from_cell(b);
}

static EMB_TEST_NOIPA void garbage_big(embi_js_runtime *rt, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)big_new(rt, EMBI_JS_UNDEFINED);
    }
}

/* A non-generational collection leaves the survivors on the queued pages still marked, while the
 * children on the pages already swept are cleared. Enabling generational mode then must not
 * leave those stale marks behind: the parents would look old, their children young, and the next
 * minor would free the children (the verifier aborts on that in debug builds). The parents' class
 * keeps its queue because nothing allocates from it after the last automatic collection. */
static EMB_TEST_NOIPA void enable_after_sweep_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_gc_set_generational(rt, false); /* runtimes start generational (J5k): these start from the off state */
    install_gen_type(rt);
    const int32_t count = 2000;
    embi_js_value head = EMBI_JS_UNDEFINED;
    for (int32_t i = 0; i < count; i++) {
        embi_js_value child = big_new(rt, embi_js_from_int32(i));
        head = gen_new(rt, head, child);
    }
    uint32_t id = embi_js_persistent_new(rt, head);
    garbage_big(rt, 8000); /* several automatic collections, the last one not yet swept */
    scrub();
    uint32_t pcls = rt->heap.class_of[(sizeof(gen_node) + 15u) / 16u];
    EMB_CHECK(rt->heap.unswept[pcls].len > 0u);
    EMB_CHECK(rt->heap.unswept_total > 0u);

    embi_js_gc_set_generational(rt, true);
    for (int round = 0; round < 4; round++) {
        garbage_big(rt, 3000);
        garbage(rt, 2000);
        scrub();
        embi_js_gc_minor(rt);
    }

    /* Walk the two-level structure: every child must still hold its payload. */
    bool ok = true;
    int32_t n = 0;
    embi_js_value v = embi_js_persistent_get(rt, id);
    while (embi_js_is_cell(v)) {
        gen_node *p = (gen_node *)embi_js_as_cell(v);
        embi_js_value child = p->payload;
        if (!embi_js_is_cell(child)) {
            ok = false;
        } else {
            gen_big *b = (gen_big *)embi_js_as_cell(child);
            if (!embi_js_is_int32(b->payload) || embi_js_as_int32(b->payload) != count - 1 - n) {
                ok = false;
            }
        }
        n++;
        v = p->next;
    }
    EMB_CHECK(ok);
    EMB_CHECK(n == count);
    embi_js_gc(rt);
    n = 0;
    v = embi_js_persistent_get(rt, id);
    while (embi_js_is_cell(v)) {
        gen_node *p = (gen_node *)embi_js_as_cell(v);
        gen_big *b = (gen_big *)embi_js_as_cell(p->payload);
        EMB_CHECK(embi_js_as_int32(b->payload) == count - 1 - n);
        n++;
        v = p->next;
    }
    EMB_CHECK(n == count);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_enabled_after_a_sweep_keeps_children)
{
    scrub();
    enable_after_sweep_body();
    scrub();
}

/* Garbage that died before generational mode was enabled sits on pages that were full and have
 * already been reset out of the young set by a collection. Enabling must make every page young
 * again, or the first minor frees the children of such a zombie (different class, own page) and
 * leaves the zombie's pointers dangling, or never sweeps them at all. */
static EMB_TEST_NOIPA uint32_t build_rooted_chain(embi_js_runtime *rt, int32_t count)
{
    embi_js_value head = EMBI_JS_UNDEFINED;
    for (int32_t i = 0; i < count; i++) {
        embi_js_value child = big_new(rt, embi_js_from_int32(i));
        head = gen_new(rt, head, child);
    }
    return embi_js_persistent_new(rt, head);
}

static EMB_TEST_NOIPA void enable_with_garbage_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_gc_set_generational(rt, false); /* runtimes start generational (J5k): these start from the off state */
    install_gen_type(rt);
    const int32_t count = 3000;
    uint32_t id = build_rooted_chain(rt, count);
    embi_js_gc(rt); /* every page is full and swept: queue_sweep resets the young set */
    embi_js_persistent_free(rt, id); /* the whole chain is dead now, on non-young pages */
    scrub();
    EMB_CHECK(rt->heap.young.len < 4u); /* the setup: the dead cells are not on young pages */
    size_t before = rt->heap.live_bytes;
    g_gen_finalized = 0;

    embi_js_gc_set_generational(rt, true);
    embi_js_gc_minor(rt);
    embi_js_gc_minor(rt);
    EMB_CHECK(rt->heap.live_bytes < before);
    /* A stale conservative stack slot may keep a few cells; ASan builds are not exact. */
    EMB_CHECK(EMB_TEST_ASAN || g_gen_finalized >= 2 * count - 64);
    /* Every dead cell must be swept: none stays live-but-unmarked on an old page. */
    size_t zombies = embi_js_gc_count_unmarked(&rt->heap);
    EMB_CHECK(zombies == 0);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_enabled_over_dead_cells_sweeps_them)
{
    scrub();
    enable_with_garbage_body();
    scrub();
}

/* ------------------------------------------------------------------ weak handles across minors */

/* WeakRef and WeakMap sit on the same two collector hooks as weak handles (clear_weak after the
 * mark phase, ephemerons through the mark stack), so a handle test pins the generational rules
 * J5k depends on: a young target dies in a minor, a target kept alive only through the remembered
 * set survives, and an old dead target keeps its sticky mark until the next major. */
static EMB_TEST_NOIPA void weak_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value owner = gen_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    uint32_t oid = embi_js_persistent_new(rt, owner);
    embi_js_value olddead = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(5));
    uint32_t odid = embi_js_persistent_new(rt, olddead);
    uint32_t w_old = embi_js_weak_new(rt, olddead);
    embi_js_gc(rt); /* owner and olddead are old */
    embi_js_persistent_free(rt, odid);
    olddead = EMBI_JS_UNDEFINED;

    embi_js_value kept = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(1));
    embi_js_value lost = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(2));
    uint32_t w_kept = embi_js_weak_new(rt, kept);
    uint32_t w_lost = embi_js_weak_new(rt, lost);
    gen_node *o = (gen_node *)embi_js_as_cell(embi_js_persistent_get(rt, oid));
    o->payload = kept;
    EMBI_JS_WB(o, kept);
    kept = EMBI_JS_UNDEFINED;
    lost = EMBI_JS_UNDEFINED;
    scrub();

    embi_js_gc_minor(rt);
    EMB_CHECK(embi_js_is_cell(embi_js_weak_get(rt, w_kept))); /* held by the old owner */
    EMB_CHECK(embi_js_as_int32(((gen_node *)embi_js_as_cell(embi_js_weak_get(rt, w_kept)))->payload) == 1);
    EMB_CHECK(embi_js_weak_get(rt, w_lost) == EMBI_JS_EMPTY); /* a dead young target is cleared */
    /* The dead old target is not collected by a minor: its sticky mark keeps the handle. */
    EMB_CHECK(embi_js_is_cell(embi_js_weak_get(rt, w_old)));
    embi_js_gc(rt); /* a major clears it, the owner's child stays */
    EMB_CHECK(embi_js_weak_get(rt, w_old) == EMBI_JS_EMPTY);
    EMB_CHECK(embi_js_is_cell(embi_js_weak_get(rt, w_kept)));
    embi_js_weak_free(rt, w_old);
    embi_js_weak_free(rt, w_kept);
    embi_js_weak_free(rt, w_lost);
    embi_js_persistent_free(rt, oid);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_generational_weak_handles_across_minors)
{
    scrub();
    weak_body();
    scrub();
}

/* ------------------------------------------------------------------ switching the mode off */

EMB_TEST(gc_generational_off_clears_sticky_marks)
{
    scrub();
    embi_js_runtime *rt = embi_js_runtime_new();
    install_gen_type(rt);
    embi_js_gc_set_generational(rt, true);
    embi_js_value v = gen_new(rt, EMBI_JS_UNDEFINED, embi_js_from_int32(9));
    uint32_t id = embi_js_persistent_new(rt, v);
    embi_js_gc(rt);
    EMB_CHECK(is_marked(embi_js_persistent_get(rt, id)));
    embi_js_gc_set_generational(rt, false);
    EMB_CHECK(!is_marked(embi_js_persistent_get(rt, id)));
    EMB_CHECK(embi_js_wb_heap == nullptr);
    embi_js_gc(rt);
    embi_js_gc_minor(rt); /* off: a full collection, counted as a major */
    EMB_CHECK(embi_js_as_int32(((gen_node *)embi_js_as_cell(embi_js_persistent_get(rt, id)))->payload) == 9);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub();
}

EMB_TEST_MAIN()
