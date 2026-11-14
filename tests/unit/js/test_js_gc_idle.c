#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* CF2: embi_js_gc_idle, the collection an embedder runs in idle time. It collects only when half the budget is spent
 * (minor below half the major limit, major above it), resets what it collected for, does nothing on an idle heap, and
 * never runs during a compile hold or with the stress hook on. Each test runs in its own runtime and scrubs the stack
 * first, so stale words do not keep garbage (as test_js_gc_trigger.c). */

typedef struct idle_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} idle_node;

static void idle_trace(embi_js_cell *c, embi_js_tracer *t)
{
    idle_node *n = (idle_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static embi_js_value idle_new(embi_js_runtime *rt, embi_js_value next)
{
    idle_node *n = (idle_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
    n->next = next;
    n->payload = EMBI_JS_UNDEFINED;
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

/* Allocates unrooted 32-byte nodes until bytes_since_gc reaches `bytes` (no automatic collection may run meanwhile:
 * the caller keeps it under the threshold). */
static EMB_TEST_NOIPA void garbage_to(embi_js_runtime *rt, size_t bytes)
{
    while (rt->heap.bytes_since_gc < bytes) {
        (void)idle_new(rt, EMBI_JS_UNDEFINED);
    }
}

static embi_js_runtime *idle_runtime(void)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){idle_trace, nullptr});
    embi_js_gc(rt);
    return rt;
}

static EMB_TEST_NOIPA void idle_heap_body(void)
{
    embi_js_runtime *rt = idle_runtime();
    EMB_CHECK(rt->heap.generational);
    size_t gc0 = rt->heap.gc_count;
    /* Nothing allocated since the collection: nothing to do, however often it is asked. */
    for (int i = 0; i < 5; i++) {
        EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    }
    /* Just under half the budget: still nothing. */
    garbage_to(rt, rt->heap.threshold / 2u - 64u);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    EMB_CHECK(rt->heap.gc_count == gc0);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_idle_does_nothing_until_half_the_budget_is_spent)
{
    scrub();
    idle_heap_body();
    scrub();
}

static EMB_TEST_NOIPA void minor_body(void)
{
    embi_js_runtime *rt = idle_runtime();
    size_t minor0 = rt->heap.minor_count, major0 = rt->heap.major_count;
    garbage_to(rt, rt->heap.threshold / 2u + 64u);
    EMB_CHECK(rt->heap.bytes_since_gc < rt->heap.threshold); /* the budget alone would not have collected yet */
    EMB_CHECK(rt->heap.old_growth < rt->heap.major_limit / 2u);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 1);
    EMB_CHECK(rt->heap.minor_count == minor0 + 1u);
    EMB_CHECK(rt->heap.major_count == major0);
    EMB_CHECK(rt->heap.bytes_since_gc == 0);
    /* Done: the next idle call has nothing to do. */
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_idle_collects_minor_past_half_the_budget)
{
    scrub();
    minor_body();
    scrub();
}

static EMB_TEST_NOIPA uint32_t rooted_chain(embi_js_runtime *rt, uint32_t count)
{
    embi_js_value head = EMBI_JS_UNDEFINED;
    for (uint32_t i = 0; i < count; i++) {
        head = idle_new(rt, head);
    }
    return embi_js_persistent_new(rt, head);
}

static EMB_TEST_NOIPA void major_body(void)
{
    embi_js_runtime *rt = idle_runtime();
    /* Promote survivors with minor collections until the old generation has grown by half its limit (but not all of
     * it, which would make the next automatic collection major by itself). */
    uint32_t ids[64];
    int n = 0;
    while (rt->heap.old_growth < rt->heap.major_limit / 2u && n < 64) {
        ids[n++] = rooted_chain(rt, 2000);
        embi_js_gc_minor(rt);
    }
    EMB_REQUIRE(rt->heap.old_growth >= rt->heap.major_limit / 2u);
    EMB_CHECK(rt->heap.old_growth < rt->heap.major_limit);
    size_t major0 = rt->heap.major_count;
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 2);
    EMB_CHECK(rt->heap.major_count == major0 + 1u);
    EMB_CHECK(rt->heap.old_growth == 0);
    EMB_CHECK(rt->heap.bytes_since_gc == 0);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    /* The survivors are intact. */
    for (int i = 0; i < n; i++) {
        embi_js_value v = embi_js_persistent_get(rt, ids[i]);
        uint32_t len = 0;
        while (embi_js_is_cell(v)) {
            v = ((idle_node *)embi_js_as_cell(v))->next;
            len++;
        }
        EMB_CHECK_EQ_INT((int)len, 2000);
        embi_js_persistent_free(rt, ids[i]);
    }
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_idle_collects_major_when_the_old_generation_grew_by_half_its_limit)
{
    scrub();
    major_body();
    scrub();
}

static EMB_TEST_NOIPA void blocked_body(void)
{
    embi_js_runtime *rt = idle_runtime();
    garbage_to(rt, rt->heap.threshold / 2u + 64u);
    size_t gc0 = rt->heap.gc_count;
    /* A compile holds collections off: idle time does not run one either. */
    embi_js_gc_hold_begin(rt);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    embi_js_gc_hold_end(rt);
    /* The stress hook owns the schedule of a test that sets it. */
    embi_js_gc_set_stress(rt, 1000000u);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    embi_js_gc_set_stress(rt, 0);
    EMB_CHECK(rt->heap.gc_count == gc0);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 1);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_idle_waits_for_a_compile_hold_and_the_stress_hook)
{
    scrub();
    blocked_body();
    scrub();
}

static EMB_TEST_NOIPA void flat_body(void)
{
    embi_js_runtime *rt = idle_runtime();
    embi_js_gc_set_generational(rt, false);
    embi_js_gc(rt);
    garbage_to(rt, rt->heap.threshold / 2u - 64u);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    garbage_to(rt, rt->heap.threshold / 2u + 64u);
    size_t major0 = rt->heap.major_count;
    /* Not generational: every collection is major. */
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 2);
    EMB_CHECK(rt->heap.major_count == major0 + 1u);
    EMB_CHECK_EQ_INT(embi_js_gc_idle(rt), 0);
    embi_js_runtime_free(rt);
}

EMB_TEST(gc_idle_without_generations_collects_full_past_half_the_budget)
{
    scrub();
    flat_body();
    scrub();
}

EMB_TEST_MAIN()
