#include "emb_test.h"

#include "js/js.h"

/* J5n2: the write barrier target is per thread and follows the innermost runtime entered on the
 * thread, and rests on the runtime that left last. The old scheme restored, at the outermost leave,
 * the target saved at the outermost enter. With two generational runtimes (the target sits on the
 * one created last) that is the wrong heap after entering and leaving the other one: a store made
 * outside an enter, by an old owner of the runtime that just left, went to the other runtime's
 * remembered set, and a minor collection then freed a young cell the owner referenced. Each test
 * below stores into an old owner of one runtime and checks that this runtime's remembered set grew
 * and the other one's did not. Run against the old runtime.c/runtime.h they fail (the first two
 * checks of each scenario see the wrong heap). */

typedef struct wbt_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} wbt_node;

static void wbt_trace(embi_js_cell *c, embi_js_tracer *t)
{
    wbt_node *n = (wbt_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static void install_wbt_type(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE, (embi_js_type_ops){wbt_trace, nullptr});
}

static embi_js_value wbt_new(embi_js_runtime *rt)
{
    wbt_node *n = (wbt_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
    n->next = EMBI_JS_UNDEFINED;
    n->payload = EMBI_JS_UNDEFINED;
    return embi_js_from_cell(n);
}

static EMB_TEST_NOIPA void wbt_scrub_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define wbt_scrub() wbt_scrub_impl(0, 0, 0, 0)

/* Makes an old owner in rt (a rooted cell that a major collection promoted) and returns its id. */
static uint32_t make_old_owner(embi_js_runtime *rt, embi_js_value *owner_out)
{
    embi_js_value owner = wbt_new(rt);
    uint32_t id = embi_js_persistent_new(rt, owner);
    embi_js_gc(rt);
    *owner_out = owner;
    return id;
}

/* Two generational runtimes; rt2 is created last, so the thread's target starts on rt2. */
typedef struct two_rt {
    embi_js_runtime *a, *b;
    uint32_t ida, idb;
    wbt_node *owner_a, *owner_b;
} two_rt;

static void two_rt_open(two_rt *x)
{
    x->a = embi_js_runtime_new();
    install_wbt_type(x->a);
    embi_js_gc_set_generational(x->a, true);
    x->b = embi_js_runtime_new();
    install_wbt_type(x->b);
    embi_js_gc_set_generational(x->b, true);
    embi_js_value v = EMBI_JS_UNDEFINED;
    x->ida = make_old_owner(x->a, &v);
    x->owner_a = (wbt_node *)embi_js_as_cell(embi_js_persistent_get(x->a, x->ida));
    x->idb = make_old_owner(x->b, &v);
    x->owner_b = (wbt_node *)embi_js_as_cell(embi_js_persistent_get(x->b, x->idb));
    EMB_CHECK(x->owner_a->hdr.mark != 0);
    EMB_CHECK(x->owner_b->hdr.mark != 0);
    EMB_CHECK(x->a->heap.remembered.len == 0u);
    EMB_CHECK(x->b->heap.remembered.len == 0u);
}

static void two_rt_close(two_rt *x)
{
    embi_js_persistent_free(x->a, x->ida);
    embi_js_persistent_free(x->b, x->idb);
    embi_js_runtime_free(x->b);
    embi_js_runtime_free(x->a);
}

/* Stores a young cell of rt into owner and runs the barrier, as engine code does. */
static void store_young(embi_js_runtime *rt, wbt_node *owner)
{
    embi_js_value young = wbt_new(rt);
    owner->payload = young;
    EMBI_JS_WB(owner, young);
}

/* Enter and leave the runtime created first; the target must rest on it, not on the later one. */
static EMB_TEST_NOIPA void outside_store_body(void)
{
    two_rt x;
    two_rt_open(&x);
    embi_js_enter(x.a);
    embi_js_leave(x.a);
    store_young(x.a, x.owner_a);
    EMB_CHECK(x.a->heap.remembered.len == 1u); /* the old scheme: 0 here, the entry went to b */
    EMB_CHECK(x.b->heap.remembered.len == 0u);
    wbt_scrub();
    embi_js_gc_minor(x.a);
    EMB_CHECK(x.a->heap.remembered.len == 0u);
    two_rt_close(&x);
}

EMB_TEST(gc_wb_target_rests_on_the_runtime_that_left_last)
{
    wbt_scrub();
    outside_store_body();
    wbt_scrub();
}

/* Nested entries a, then b: leaving b makes a the target again; leaving a leaves the target on a. */
static EMB_TEST_NOIPA void nested_body(void)
{
    two_rt x;
    two_rt_open(&x);
    embi_js_enter(x.a);
    embi_js_enter(x.b);
    store_young(x.b, x.owner_b);
    EMB_CHECK(x.b->heap.remembered.len == 1u);
    EMB_CHECK(x.a->heap.remembered.len == 0u);
    embi_js_leave(x.b);
    store_young(x.a, x.owner_a); /* back inside a only */
    EMB_CHECK(x.a->heap.remembered.len == 1u);
    EMB_CHECK(x.b->heap.remembered.len == 1u);
    embi_js_leave(x.a);
    /* Nothing entered: a left last. The old scheme restored b here (the target saved at a's enter). */
    embi_js_gc(x.a);
    embi_js_gc(x.b);
    EMB_CHECK(x.a->heap.remembered.len == 0u);
    EMB_CHECK(x.b->heap.remembered.len == 0u);
    store_young(x.a, x.owner_a);
    EMB_CHECK(x.a->heap.remembered.len == 1u);
    EMB_CHECK(x.b->heap.remembered.len == 0u);
    wbt_scrub();
    embi_js_gc_minor(x.a);
    two_rt_close(&x);
}

EMB_TEST(gc_wb_target_follows_nested_enter_and_leave)
{
    wbt_scrub();
    nested_body();
    wbt_scrub();
}

/* Runtimes entered and left in stack order across a callback, as host code does when a native
 * function of one view calls into another: a(b()) three times over. */
static void callback_inner(two_rt *x)
{
    embi_js_enter(x->b);
    store_young(x->b, x->owner_b);
    embi_js_leave(x->b);
}

static EMB_TEST_NOIPA void callback_body(void)
{
    two_rt x;
    two_rt_open(&x);
    for (int round = 0; round < 3; round++) {
        embi_js_enter(x.a);
        embi_js_enter(x.a); /* a re-entrant call into the same runtime */
        callback_inner(&x);
        embi_js_leave(x.a);
        size_t bl = x.b->heap.remembered.len;
        store_young(x.a, x.owner_a); /* inside a again after the callback returned */
        EMB_CHECK(x.b->heap.remembered.len == bl);
        embi_js_leave(x.a);
        EMB_CHECK(x.a->heap.remembered.len == 1u);
        EMB_CHECK(x.b->heap.remembered.len == 1u);
        wbt_scrub();
        embi_js_gc(x.a);
        embi_js_gc(x.b);
        EMB_CHECK(x.a->heap.remembered.len == 0u);
        EMB_CHECK(x.b->heap.remembered.len == 0u);
    }
    two_rt_close(&x);
}

EMB_TEST(gc_wb_target_nested_across_callback)
{
    wbt_scrub();
    callback_body();
    wbt_scrub();
}

/* A leave out of order (a leaves while b, entered after it, is still entered) must not corrupt the
 * chain: b stays the target, and after b leaves nothing is entered. */
static EMB_TEST_NOIPA void non_lifo_body(void)
{
    two_rt x;
    two_rt_open(&x);
    embi_js_enter(x.a);
    embi_js_enter(x.b);
    embi_js_leave(x.a);
    store_young(x.b, x.owner_b);
    EMB_CHECK(x.b->heap.remembered.len == 1u);
    EMB_CHECK(x.a->heap.remembered.len == 0u);
    embi_js_leave(x.b);
    embi_js_gc(x.b);
    EMB_CHECK(x.b->heap.remembered.len == 0u);
    /* The chain is empty again: a fresh nested pair behaves as usual. */
    embi_js_enter(x.b);
    embi_js_enter(x.a);
    store_young(x.a, x.owner_a);
    EMB_CHECK(x.a->heap.remembered.len == 1u);
    embi_js_leave(x.a);
    store_young(x.b, x.owner_b);
    EMB_CHECK(x.b->heap.remembered.len == 1u);
    embi_js_leave(x.b);
    wbt_scrub();
    embi_js_gc(x.a);
    embi_js_gc(x.b);
    two_rt_close(&x);
}

EMB_TEST(gc_wb_target_survives_a_non_lifo_leave)
{
    wbt_scrub();
    non_lifo_body();
    wbt_scrub();
}

EMB_TEST_MAIN()
