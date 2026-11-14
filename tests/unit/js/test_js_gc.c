#include "emb_test.h"

#include "js/js.h"

#define TEST_EMBI_NOINLINE __attribute__((noinline))

/* A test cell kind: two value fields that the trace hook marks. It is stored in the SHAPE slot,
 * which the engine does not otherwise use yet. */
typedef struct test_node {
    embi_js_cell hdr;
    embi_js_value next;
    embi_js_value payload;
} test_node;

static int g_node_finalized;

static void node_trace(embi_js_cell *c, embi_js_tracer *t)
{
    test_node *n = (test_node *)c;
    embi_js_mark_value(t, n->next);
    embi_js_mark_value(t, n->payload);
}

static void node_finalize(embi_js_cell *c)
{
    (void)c;
    g_node_finalized++;
}

static void install_node_type(embi_js_runtime *rt)
{
    embi_js_set_type_ops(rt, EMBI_JS_CELL_SHAPE,
                         (embi_js_type_ops){node_trace, node_finalize});
}

static embi_js_value node_new(embi_js_runtime *rt, embi_js_value next, embi_js_value payload)
{
    test_node *n = (test_node *)embi_js_alloc_cell(rt, sizeof *n, EMBI_JS_CELL_SHAPE);
    n->next = next;
    n->payload = payload;
    return embi_js_from_cell(n);
}

/* Overwrites a few kilobytes of dead stack below the caller, so stale pointers left there by
 * earlier calls are not scanned by the next collection. The four arguments are forced into memory
 * so the Windows x64 home slots in the caller's frame are rewritten with zeros too: a callee
 * that spills its register arguments there can otherwise leave a cell pointer behind. */
static EMB_TEST_NOIPA void scrub_stack_impl(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __asm__ volatile("" : : "m"(a), "m"(b), "m"(c), "m"(d) : "memory");
    volatile uint8_t pad[16384];
    for (size_t i = 0; i < sizeof pad; i++) {
        pad[i] = 0;
    }
}

#define scrub_stack() scrub_stack_impl(0, 0, 0, 0)

/* Each test scrubs the stack first, so the frames of its body (which may hold cell pointers) start
 * from zeroed memory, then runs the body in a separate non-inlined function. */
#define GC_TEST(name)                                                                          \
    static TEST_EMBI_NOINLINE void gc_body_##name(void);                                       \
    EMB_TEST(name)                                                                             \
    {                                                                                          \
        scrub_stack();                                                                         \
        gc_body_##name();                                                                      \
    }                                                                                          \
    static TEST_EMBI_NOINLINE void gc_body_##name(void)

static TEST_EMBI_NOINLINE embi_js_value make_string_value(embi_js_runtime *rt, const char *text)
{
    return embi_js_from_string(embi_js_str_new_utf8(rt, text, strlen(text)));
}

static bool string_value_is(embi_js_value v, const char *text)
{
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_as_string(v);
    size_t n = strlen(text);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint16_t)(unsigned char)text[i]) {
            return false;
        }
    }
    return true;
}

static bool is_live_cell(embi_js_runtime *rt, embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_heap_find_cell(&rt->heap, (uintptr_t)v) != nullptr;
}

GC_TEST(gc_unreachable_cells_are_freed_and_heap_bytes_drop)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    /* A 64 MB floor keeps the automatic trigger out of the way: this test counts the bytes of
     * 4000 strings and expects none of them to be swept before the explicit collection. */
    embi_js_set_gc_params(rt, (size_t)64u << 20, 1.5);
    embi_js_gc(rt);
    embi_js_mem_stats before_alloc;
    embi_js_get_mem_stats(rt, &before_alloc);

    uint8_t text[200];
    for (size_t i = 0; i < sizeof text; i++) {
        text[i] = 'x';
    }
    for (int i = 0; i < 4000; i++) {
        text[0] = (uint8_t)('a' + i % 26);
        (void)embi_js_str_new_latin1(rt, text, sizeof text);
    }
    embi_js_mem_stats full;
    embi_js_get_mem_stats(rt, &full);
    /* 200 bytes plus the 16-byte header lands in the 224-byte class. */
    size_t allocated = full.heap_live_bytes - before_alloc.heap_live_bytes;
    EMB_CHECK(allocated >= 4000u * 224u);

    scrub_stack();
    embi_js_gc(rt);
    embi_js_mem_stats after;
    embi_js_get_mem_stats(rt, &after);
    EMB_CHECK(after.gc_count > full.gc_count);
    EMB_CHECK(full.heap_live_bytes - after.heap_live_bytes >= allocated * 9u / 10u);
    EMB_CHECK(after.heap_live_bytes <= before_alloc.heap_live_bytes + allocated / 10u);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_value_held_only_in_c_local_survives)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_value v = make_string_value(rt, "survivor");
    for (int i = 0; i < 1000; i++) {
        (void)embi_js_str_new_utf8(rt, "garbage", 7);
    }
    scrub_stack();
    embi_js_gc(rt);
    /* Read through a volatile so the compiler cannot reuse a register copy from before the
     * collection. */
    volatile embi_js_value vv = v;
    EMB_CHECK(is_live_cell(rt, vv));
    EMB_CHECK(string_value_is(vv, "survivor"));
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_values_in_locals_survive_repeated_collections)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_value a = make_string_value(rt, "alpha");
    embi_js_value b = embi_js_from_double(0.25);
    embi_js_value c = embi_js_from_int32(9);
    embi_js_value d = node_new(rt, EMBI_JS_UNDEFINED, make_string_value(rt, "delta"));
    for (int i = 0; i < 3; i++) {
        embi_js_gc(rt);
    }
    volatile embi_js_value va = a;
    volatile embi_js_value vd = d;
    EMB_CHECK(string_value_is(va, "alpha"));
    EMB_CHECK(embi_js_as_double(b) == 0.25);
    EMB_CHECK_EQ_INT(embi_js_as_int32(c), 9);
    /* The node was only reachable through this local, and its payload only through the node. */
    EMB_CHECK(is_live_cell(rt, vd));
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_persistent_handle_keeps_value_alive)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    uint32_t id = embi_js_persistent_new(rt, make_string_value(rt, "pinned"));
    for (int i = 0; i < 500; i++) {
        (void)embi_js_str_new_utf8(rt, "noise", 5);
    }
    scrub_stack();
    embi_js_gc(rt);
    embi_js_value v = embi_js_persistent_get(rt, id);
    EMB_CHECK(is_live_cell(rt, v));
    EMB_CHECK(string_value_is(v, "pinned"));

    embi_js_persistent_set(rt, id, EMBI_JS_NULL);
    EMB_CHECK(embi_js_persistent_get(rt, id) == EMBI_JS_NULL);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_persistent_handle_slots_are_reused)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    uint32_t a = embi_js_persistent_new(rt, EMBI_JS_TRUE);
    uint32_t b = embi_js_persistent_new(rt, EMBI_JS_FALSE);
    EMB_CHECK(a != 0 && b != 0 && a != b);
    embi_js_persistent_free(rt, a);
    uint32_t c = embi_js_persistent_new(rt, EMBI_JS_NULL);
    EMB_CHECK_EQ_INT(c, a);
    EMB_CHECK(embi_js_persistent_get(rt, c) == EMBI_JS_NULL);
    EMB_CHECK(embi_js_persistent_get(rt, b) == EMBI_JS_FALSE);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_weak_handles_are_cleared_when_target_dies)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    enum { N = 1000 };
    uint32_t *ids = embi_calloc(N, sizeof *ids);
    for (int i = 0; i < N; i++) {
        ids[i] = embi_js_weak_new(rt, make_string_value(rt, "transient"));
    }
    /* A weak handle to a value that a persistent handle keeps alive must survive. */
    embi_js_value held = make_string_value(rt, "held");
    uint32_t strong = embi_js_persistent_new(rt, held);
    uint32_t weak_held = embi_js_weak_new(rt, held);

    scrub_stack();
    embi_js_gc(rt);

    int cleared = 0;
    for (int i = 0; i < N; i++) {
        if (embi_js_weak_get(rt, ids[i]) == EMBI_JS_EMPTY) {
            cleared++;
        }
    }
    /* A few stale stack words may still name a transient string, so require nearly all. */
    EMB_CHECK(cleared >= N - 10);
    EMB_CHECK(string_value_is(embi_js_weak_get(rt, weak_held), "held"));
    EMB_CHECK(embi_js_persistent_get(rt, strong) == held);

    embi_js_weak_free(rt, weak_held);
    embi_js_persistent_free(rt, strong);
    embi_free(ids, N * sizeof *ids);
    embi_js_runtime_free(rt);
    scrub_stack();
}

static TEST_EMBI_NOINLINE uint32_t build_three_node_chain(embi_js_runtime *rt)
{
    /* The only references to the payload strings are through the node chain. */
    embi_js_value head = EMBI_JS_EMPTY;
    head = node_new(rt, head, make_string_value(rt, "third"));
    head = node_new(rt, head, make_string_value(rt, "second"));
    head = node_new(rt, head, make_string_value(rt, "first"));
    return embi_js_persistent_new(rt, head);
}

static TEST_EMBI_NOINLINE void check_three_node_chain(embi_js_runtime *rt, uint32_t id)
{
    test_node *n = (test_node *)embi_js_as_cell(embi_js_persistent_get(rt, id));
    EMB_CHECK(string_value_is(n->payload, "first"));
    n = (test_node *)embi_js_as_cell(n->next);
    EMB_CHECK(string_value_is(n->payload, "second"));
    n = (test_node *)embi_js_as_cell(n->next);
    EMB_CHECK(string_value_is(n->payload, "third"));
}

static TEST_EMBI_NOINLINE void trace_hook_scenario(embi_js_runtime *rt)
{
    install_node_type(rt);
    g_node_finalized = 0;

    uint32_t id = build_three_node_chain(rt);
    scrub_stack();
    embi_js_gc(rt);
    check_three_node_chain(rt, id);
    EMB_CHECK(g_node_finalized == 0);

    /* Dropping the root frees the whole chain, payloads included, and finalizes each node. */
    embi_js_persistent_free(rt, id);
    scrub_stack();
    embi_js_gc(rt);
    /* Best-effort under ASan: a stale stack slot may still hold the head (see emb_test.h). */
    EMB_CHECK(EMB_TEST_ASAN || g_node_finalized >= 3);
}

GC_TEST(gc_trace_hook_keeps_children_alive)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    trace_hook_scenario(rt);
    embi_js_runtime_free(rt);
}

static const int k_chain_depth = 200000;

static TEST_EMBI_NOINLINE uint32_t build_deep_chain(embi_js_runtime *rt)
{
    embi_js_value head = EMBI_JS_EMPTY;
    for (int i = 0; i < k_chain_depth; i++) {
        head = node_new(rt, head, EMBI_JS_EMPTY);
    }
    return embi_js_persistent_new(rt, head);
}

static TEST_EMBI_NOINLINE int walk_chain(embi_js_runtime *rt, uint32_t id)
{
    int walked = 0;
    embi_js_value cur = embi_js_persistent_get(rt, id);
    while (cur != EMBI_JS_EMPTY) {
        walked++;
        cur = ((test_node *)embi_js_as_cell(cur))->next;
    }
    return walked;
}

static TEST_EMBI_NOINLINE void deep_chain_scenario(embi_js_runtime *rt)
{
    install_node_type(rt);
    g_node_finalized = 0;

    uint32_t id = build_deep_chain(rt);
    scrub_stack();
    embi_js_gc(rt);
    EMB_CHECK_EQ_INT(walk_chain(rt, id), k_chain_depth);

    embi_js_persistent_free(rt, id);
    /* One collection frees the chain: the scan starts at the collection entry's frame, so the
     * stale words of the walk above it are not roots (gc.c collect_entry). */
    scrub_stack();
    embi_js_gc(rt);
    EMB_CHECK(g_node_finalized >= k_chain_depth - 10);
}

GC_TEST(gc_deep_chain_is_marked_without_native_recursion)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    deep_chain_scenario(rt);
    embi_js_runtime_free(rt);
}

GC_TEST(gc_collects_automatically_when_budget_is_spent)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    uint8_t text[100];
    for (size_t i = 0; i < sizeof text; i++) {
        text[i] = 'q';
    }
    for (int i = 0; i < 300000; i++) {
        (void)embi_js_str_new_latin1(rt, text, sizeof text);
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);
    /* 300000 cells of 128 bytes would be about 38 MB without collection. */
    EMB_CHECK(st.heap_live_bytes < (size_t)8 * 1024 * 1024);
    embi_js_runtime_free(rt);
    scrub_stack();
}

static TEST_EMBI_NOINLINE uint32_t hold_large_string(embi_js_runtime *rt)
{
    uint8_t *big = embi_alloc(100000);
    memset(big, 'L', 100000);
    embi_js_value large = embi_js_from_string(embi_js_str_new_latin1(rt, big, 100000));
    embi_free(big, 100000);
    /* The cell starts at the address and its interior does not count. */
    EMB_CHECK(embi_js_heap_find_cell(&rt->heap, (uintptr_t)large) != nullptr);
    EMB_CHECK(embi_js_heap_find_cell(&rt->heap, (uintptr_t)large + 16u) == nullptr);
    return embi_js_persistent_new(rt, large);
}

/* Checks a rooted large cell after a collection. Kept in its own frame so no pointer to the cell
 * stays in the scenario's frame once it returns, which would keep the cell alive in the final
 * collection. */
static TEST_EMBI_NOINLINE void check_kept_large(embi_js_runtime *rt, uint32_t id, size_t objects)
{
    embi_js_mem_stats kept;
    embi_js_get_mem_stats(rt, &kept);
    EMB_CHECK(kept.large_objects == objects);
    embi_js_value kv = embi_js_persistent_get(rt, id);
    EMB_CHECK(is_live_cell(rt, kv));
    const embi_js_string *ks = embi_js_as_string(kv);
    EMB_CHECK_EQ_INT(embi_js_str_len(ks), 100000);
    EMB_CHECK_EQ_INT(embi_js_str_at(ks, 0), 'L');
    EMB_CHECK_EQ_INT(embi_js_str_at(ks, 99999), 'L');
    EMB_CHECK_EQ_INT(embi_js_str_at(ks, 50000), 'L');
}

static TEST_EMBI_NOINLINE void large_cell_scenario(embi_js_runtime *rt)
{
    embi_js_gc(rt);
    embi_js_mem_stats base;
    embi_js_get_mem_stats(rt, &base);

    uint32_t id = hold_large_string(rt);
    embi_js_mem_stats held;
    embi_js_get_mem_stats(rt, &held);
    EMB_CHECK(held.large_objects == base.large_objects + 1u);
    scrub_stack();
    embi_js_gc(rt);

    check_kept_large(rt, id, base.large_objects + 1u);

    embi_js_persistent_free(rt, id);
    scrub_stack();
    embi_js_gc(rt);
    embi_js_mem_stats gone;
    embi_js_get_mem_stats(rt, &gone);
    EMB_CHECK(gone.large_objects == base.large_objects);
    EMB_CHECK(held.heap_live_bytes - gone.heap_live_bytes >= 100000u);
}

GC_TEST(gc_large_cells_are_tracked_and_freed)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    large_cell_scenario(rt);
    embi_js_runtime_free(rt);
}

static TEST_EMBI_NOINLINE void fill_unrooted_strings(embi_js_runtime *rt, int n)
{
    uint8_t text[100];
    memset(text, 'p', sizeof text);
    for (int i = 0; i < n; i++) {
        (void)embi_js_str_new_latin1(rt, text, sizeof text);
    }
}

static TEST_EMBI_NOINLINE void fill_unrooted_symbols(embi_js_runtime *rt, int n)
{
    for (int i = 0; i < n; i++) {
        (void)embi_js_symbol_new(rt, EMBI_JS_UNDEFINED);
    }
}

/* Fills the heap with 128-byte cells, collects them all, then fills with 32-byte cells. Freed
 * pages go back to the pool and take the new class, so the heap does not grow past the size it
 * reached in the first fill. The collection releases the chunks the pool no longer needs (one
 * spare is kept), so the first fill's peak is what the second fill must fit under. */
GC_TEST(gc_empty_pages_are_reused_by_another_size_class)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    fill_unrooted_strings(rt, 20000);
    embi_js_mem_stats peak;
    embi_js_get_mem_stats(rt, &peak);
    scrub_stack();
    embi_js_gc(rt);
    embi_js_mem_stats after_first;
    embi_js_get_mem_stats(rt, &after_first);
    /* A stale stack word may keep a cell alive, so allow a few cells. */
    EMB_CHECK(after_first.heap_live_bytes < 4096u);
    EMB_CHECK(after_first.released_chunks > 0u);
    EMB_CHECK(after_first.heap_chunks < peak.heap_chunks);
    EMB_CHECK(after_first.heap_chunks >= 1u);

    fill_unrooted_symbols(rt, 20000);
    embi_js_mem_stats after_second;
    embi_js_get_mem_stats(rt, &after_second);
    /* The second fill runs under the budget floor (512 KB, two chunks) with its own collections,
     * and the pool is refilled from the other classes' queues before any new chunk, so it stays
     * within the budget's chunks plus the spare, not at the first fill's peak. */
    EMB_CHECK(after_second.heap_chunks <= after_first.heap_chunks + 3u);
    EMB_CHECK(after_second.heap_chunks <= peak.heap_chunks);

    scrub_stack();
    embi_js_gc(rt);
    embi_js_mem_stats final;
    embi_js_get_mem_stats(rt, &final);
    EMB_CHECK(final.heap_live_bytes < 4096u);
    embi_js_runtime_free(rt);
    scrub_stack();
}

/* Many independent unrooted nodes: one stale stack word can keep at most one of them alive. */
static TEST_EMBI_NOINLINE void make_unrooted_nodes(embi_js_runtime *rt, int n)
{
    for (int i = 0; i < n; i++) {
        (void)node_new(rt, EMBI_JS_EMPTY, EMBI_JS_EMPTY);
    }
}

GC_TEST(gc_unrooted_nodes_are_finalized)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_node_type(rt);
    g_node_finalized = 0;
    make_unrooted_nodes(rt, 2000);
    scrub_stack();
    embi_js_gc(rt);
    EMB_CHECK(g_node_finalized >= 1990);
    embi_js_runtime_free(rt);
}

GC_TEST(gc_runtime_free_finalizes_surviving_cells)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    install_node_type(rt);
    g_node_finalized = 0;
    uint32_t id = embi_js_persistent_new(rt, node_new(rt, EMBI_JS_EMPTY, EMBI_JS_TRUE));
    (void)id;
    embi_js_runtime_free(rt);
    EMB_CHECK_EQ_INT(g_node_finalized, 1);
    scrub_stack();
}

GC_TEST(gc_symbols_and_boxes_are_traced)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_symbol *sym = embi_js_symbol_new(rt, make_string_value(rt, "desc"));
    embi_js_box *box = embi_js_box_new(rt, embi_js_from_symbol(sym));
    uint32_t id = embi_js_persistent_new(rt, embi_js_from_cell(box));
    scrub_stack();
    embi_js_gc(rt);
    embi_js_box *kept = (embi_js_box *)embi_js_as_cell(embi_js_persistent_get(rt, id));
    EMB_CHECK(embi_js_is_symbol(kept->value));
    EMB_CHECK(string_value_is(embi_js_as_symbol(kept->value)->description, "desc"));
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(gc_varray_grow_keeps_elements_and_fills_holes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_varray *a = embi_js_varray_new(rt, 2);
    a->elems[0] = embi_js_from_int32(1);
    a->elems[1] = make_string_value(rt, "two");
    uint32_t id = embi_js_persistent_new(rt, embi_js_from_cell(a));
    embi_js_varray *b = embi_js_varray_grow(rt, a, 4);
    embi_js_persistent_set(rt, id, embi_js_from_cell(b));
    scrub_stack();
    embi_js_gc(rt);
    embi_js_varray *kept = (embi_js_varray *)embi_js_as_cell(embi_js_persistent_get(rt, id));
    EMB_CHECK_EQ_INT(kept->hdr.aux, 4);
    EMB_CHECK_EQ_INT(embi_js_as_int32(kept->elems[0]), 1);
    EMB_CHECK(string_value_is(kept->elems[1], "two"));
    EMB_CHECK(kept->elems[2] == EMBI_JS_EMPTY);
    EMB_CHECK(kept->elems[3] == EMBI_JS_EMPTY);
    embi_js_persistent_free(rt, id);
    embi_js_runtime_free(rt);
    scrub_stack();
}

EMB_TEST_MAIN()
