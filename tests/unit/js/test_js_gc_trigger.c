#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* J5h: the allocation budget, the allocation-rate shortening, the stress hook and the stats. Each
 * test runs in its own runtime and scrubs the stack first, so stale words do not keep garbage. */

typedef struct trig_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} trig_node;

static void trig_trace(embi_js_cell *c, embi_js_tracer *t)
{
    trig_node *n = (trig_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static void install_trig_type(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){trig_trace, nullptr});
}

static embi_js_value trig_new(embi_js_runtime *rt, embi_js_value next, embi_js_value payload)
{
    trig_node *n = (trig_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
    n->next = next;
    n->payload = payload;
    return embi_js_from_cell(n);
}

/* Overwrites dead stack below the caller, so stale cell pointers are not scanned. */
static EMB_TEST_NOIPA void scrub_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub() scrub_impl(0, 0, 0, 0)

/* Allocates `count` unrooted nodes of 32 bytes each (the 32-byte class). */
static EMB_TEST_NOIPA void garbage(embi_js_runtime *rt, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        (void)trig_new(rt, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED);
    }
}

/* Builds a rooted list of `count` nodes and returns its head through a persistent id. */
static EMB_TEST_NOIPA uint32_t live_chain(embi_js_runtime *rt, uint32_t count)
{
    embi_js_value head = EMBI_JS_UNDEFINED;
    for (uint32_t i = 0; i < count; i++) {
        head = trig_new(rt, head, embi_js_from_int32((int32_t)i));
    }
    return embi_js_persistent_new(rt, head);
}

static size_t budget_floor(void)
{
    return EMBI_JS_GC_FLOOR_BYTES;
}

/* ------------------------------------------------------------------ budget */

static EMB_TEST_NOIPA void floor_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    embi_js_gc(rt);
    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    /* Well under the 256 KB floor: no automatic collection yet. */
    garbage(rt, 2000); /* 64 KB */
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    EMB_CHECK(s1.gc_count == s0.gc_count);
    /* Past the floor: the budget is spent and a collection runs. */
    garbage(rt, 40000); /* 1.28 MB */
    embi_js_mem_stats s2;
    embi_js_get_mem_stats(rt, &s2);
    EMB_CHECK(s2.gc_count > s1.gc_count);
    EMB_CHECK(rt->heap.min_trigger == budget_floor());
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_budget_floor_delays_the_first_automatic_collection)
{
    scrub();
    floor_body();
    scrub();
}

/* True when a and b differ by at most tol bytes. */
static bool near_bytes(size_t a, size_t b, size_t tol)
{
    size_t d = a > b ? a - b : b - a;
    return d <= tol;
}

static EMB_TEST_NOIPA void threshold_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    uint32_t id = live_chain(rt, 150000); /* about 4.8 MB of live cells */
    embi_js_gc(rt);
    size_t live = rt->heap.live_after_gc;
    EMB_CHECK(live >= (size_t)4u << 20);
    /* A cap of 1.0 leaves no room above the live heap, so the budget is the floor. */
    embi_js_set_gc_params(rt, budget_floor(), 1.0);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(rt->heap.threshold == budget_floor());
    /* With the default factor (J5n2: 1.25 at every size), the room is a quarter of the live heap. */
    embi_js_set_gc_params(rt, budget_floor(), EMBI_JS_GC_FACTOR_MAX);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(near_bytes(rt->heap.threshold, (size_t)((double)live * (EMBI_JS_GC_FACTOR_MAX - 1.0)), 2u));
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_budget_follows_live_size_and_factor_cap)
{
    scrub();
    threshold_body();
    scrub();
}

/* The curve of heap.h: the factor is EMBI_JS_GC_FACTOR_MAX up to 16 MB, falls linearly to
 * EMBI_JS_GC_FACTOR_MIN at 64 MB and stays there (J5n2: both are 1.25, so the curve is flat). The
 * budget is the room under the heap limit, live * (factor - 1), with the floor under it. The live
 * sizes are set directly, so no real heap of that size is needed. */
static EMB_TEST_NOIPA void curve_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    embi_js_set_gc_params(rt, budget_floor(), EMBI_JS_GC_FACTOR_MAX);
    rt->heap.alloc_rate = 0.0;

    static const struct {
        size_t live;
        double factor;
    } cases[] = {
        {(size_t)16u << 20, EMBI_JS_GC_FACTOR_MAX},
        {(size_t)40u << 20, (EMBI_JS_GC_FACTOR_MAX + EMBI_JS_GC_FACTOR_MIN) / 2.0},
        {(size_t)64u << 20, EMBI_JS_GC_FACTOR_MIN},
        {(size_t)200u << 20, EMBI_JS_GC_FACTOR_MIN},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t live = cases[i].live;
        rt->heap.live_after_gc = live;
        rt->heap.major_live = live;
        embi_js_gc_retune(&rt->heap);
        size_t want = (size_t)((double)live * (cases[i].factor - 1.0));
        EMB_CHECK(near_bytes(rt->heap.threshold, want, 2u));
        EMB_CHECK(rt->heap.threshold >= budget_floor());
    }

    /* A cap below the curve wins: at 40 MB the curve gives the factor above, the cap 1.1 gives room 0.1. */
    rt->heap.live_after_gc = (size_t)40u << 20;
    embi_js_set_gc_params(rt, budget_floor(), 1.1);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(near_bytes(rt->heap.threshold, (size_t)((double)((size_t)40u << 20) * 0.1), 2u));

    /* The floor wins when the room is smaller than it: 16 MB at a cap of 1.01 leaves 160 KB, under
     * the 256 KB floor (J5m3). */
    rt->heap.live_after_gc = (size_t)16u << 20;
    embi_js_set_gc_params(rt, budget_floor(), 1.01);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(rt->heap.threshold == budget_floor());

    /* A factor at or below 1 never goes negative: with no floor the budget is zero. */
    embi_js_set_gc_params(rt, 0u, 0.0);
    EMB_CHECK(rt->heap.threshold == 0u);
    embi_js_set_gc_params(rt, budget_floor(), EMBI_JS_GC_FACTOR_MAX);
    rt->heap.live_after_gc = 0;
    rt->heap.major_live = 0;
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_budget_curve_interpolates_between_16_and_64_mb)
{
    scrub();
    curve_body();
    scrub();
}

static EMB_TEST_NOIPA void rate_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_gc_set_generational(rt, false); /* runtimes start generational (J5k) */
    install_trig_type(rt);
    uint32_t id = live_chain(rt, 150000);
    embi_js_gc(rt);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    size_t calm = rt->heap.threshold;
    /* Generational off: the rate never shortens the budget, since there are no minors. */
    rt->heap.alloc_rate = EMBI_JS_GC_RATE_HI * 4.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(rt->heap.threshold == calm);
    embi_js_gc_set_generational(rt, true);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    calm = rt->heap.threshold;
    size_t calm_major = rt->heap.major_limit;
    /* A fast allocator (well above EMBI_JS_GC_RATE_HI) gets half the budget. */
    rt->heap.alloc_rate = EMBI_JS_GC_RATE_HI * 4.0;
    embi_js_gc_retune(&rt->heap);
    size_t fast = rt->heap.threshold;
    EMB_CHECK(fast < calm);
    EMB_CHECK(fast + 2u >= calm / 2u);
    EMB_CHECK(fast >= budget_floor());
    EMB_CHECK(rt->heap.major_limit == calm_major);
    rt->heap.alloc_rate = 0.0;
    embi_js_gc_retune(&rt->heap);
    EMB_CHECK(rt->heap.threshold == calm);
    embi_js_gc_set_generational(rt, false);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_allocation_rate_halves_the_budget_at_high_rates)
{
    scrub();
    rate_body();
    scrub();
}

static EMB_TEST_NOIPA void zero_params_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    embi_js_gc(rt);
    embi_js_set_gc_params(rt, 0, 0.0);
    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    garbage(rt, 50);
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    /* A zero budget collects before every allocation after the first byte. */
    EMB_CHECK(s1.gc_count - s0.gc_count >= 45u);
    embi_js_set_gc_params(rt, EMBI_JS_GC_FLOOR_BYTES, EMBI_JS_GC_FACTOR_MAX);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_zero_budget_collects_on_nearly_every_allocation)
{
    scrub();
    zero_params_body();
    scrub();
}

/* ------------------------------------------------------------------ stress and stats */

static EMB_TEST_NOIPA void stress_body(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    embi_js_gc(rt);
    embi_js_gc_set_stress(rt, 10);
    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    garbage(rt, 1000);
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    size_t made = s1.gc_count - s0.gc_count;
    EMB_CHECK(made >= 95u && made <= 105u);
    embi_js_gc_set_stress(rt, 0);
    garbage(rt, 50);
    embi_js_mem_stats s2;
    embi_js_get_mem_stats(rt, &s2);
    EMB_CHECK(s2.gc_count == s1.gc_count);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_stress_hook_collects_every_nth_allocation)
{
    scrub();
    stress_body();
    scrub();
}

EMB_TEST(gc_stats_count_majors_and_leave_minors_at_zero_when_generational_is_off)
{
    scrub();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_gc_set_generational(rt, false); /* runtimes start generational (J5k) */
    install_trig_type(rt);
    embi_js_mem_stats s0;
    embi_js_get_mem_stats(rt, &s0);
    embi_js_gc(rt);
    embi_js_gc_minor(rt); /* generational is off: a full collection */
    embi_js_mem_stats s1;
    embi_js_get_mem_stats(rt, &s1);
    EMB_CHECK(s1.major_count == s0.major_count + 2u);
    EMB_CHECK(s1.minor_count == 0u);
    EMB_CHECK(s1.gc_count == s0.gc_count + 2u);
    EMB_CHECK(s1.remembered_peak == 0u);
    embi_js_runtime_free(rt);
    scrub();
}

/* Every chunk the runtime took is returned when it is freed, so the allocator sees no leak. */
EMB_TEST(gc_runtime_round_trip_returns_all_heap_memory)
{
    scrub();
    size_t before = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    install_trig_type(rt);
    garbage(rt, 20000);
    embi_js_gc(rt);
    embi_js_runtime_free(rt);
    EMB_CHECK(embi_mem_live() == before);
    scrub();
}

EMB_TEST_MAIN()
