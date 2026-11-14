#include "emb_test.h"

#include "js/js.h"

#define TEST_EMBI_NOINLINE __attribute__((noinline))

static embi_js_value make_text(embi_js_runtime *rt, const char *text)
{
    return embi_js_from_string(embi_js_str_new_utf8(rt, text, strlen(text)));
}

static bool text_is(embi_js_value v, const char *text)
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

/* Zeroes dead stack below the caller and the caller's home slots (see test_js_gc.c). */
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

GC_TEST(runtime_create_and_free_returns_all_memory)
{
    size_t base = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    for (int i = 0; i < 5000; i++) {
        char name[24];
        int n = SDL_snprintf(name, sizeof name, "atom%d", i);
        (void)embi_js_atom_intern_utf8(rt, name, (size_t)n);
    }
    (void)embi_js_persistent_new(rt, make_text(rt, "kept"));
    embi_js_job_push(rt, make_text(rt, "job"));
    (void)embi_js_symbol_new(rt, EMBI_JS_UNDEFINED);
    embi_js_runtime_free(rt);
    EMB_CHECK(embi_mem_live() == base);
    scrub_stack();
}

GC_TEST(runtime_enter_leave_nests_and_records_stack)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK_EQ_INT(rt->nest, 0);
    embi_js_enter(rt);
    embi_js_enter(rt);
    EMB_CHECK_EQ_INT(rt->nest, 2);
    uintptr_t outer = rt->stack_base;
    embi_js_leave(rt);
    embi_js_leave(rt);
    EMB_CHECK_EQ_INT(rt->nest, 0);
    if (embi_js_stack_top() != 0) {
        EMB_CHECK(outer != 0);
        int marker = 0;
        EMB_CHECK(outer > (uintptr_t)&marker);
    }
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(runtime_stack_top_is_above_the_current_frame)
{
    uintptr_t top = embi_js_stack_top();
    if (top == 0) {
        EMB_SKIP("stack bounds unavailable on this platform");
    }
    volatile int here = 0;
    EMB_CHECK(top > (uintptr_t)&here);
    EMB_CHECK(top - (uintptr_t)&here < (uintptr_t)64 * 1024 * 1024);
    scrub_stack();
}

GC_TEST(runtime_job_queue_is_fifo_and_rooted)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_value out = EMBI_JS_EMPTY;
    EMB_CHECK(!embi_js_job_pop(rt, &out));
    embi_js_job_push(rt, make_text(rt, "first"));
    embi_js_job_push(rt, make_text(rt, "second"));
    embi_js_job_push(rt, embi_js_from_int32(3));

    scrub_stack();
    embi_js_gc(rt);

    EMB_CHECK(embi_js_job_pop(rt, &out));
    EMB_CHECK(text_is(out, "first"));
    EMB_CHECK(embi_js_job_pop(rt, &out));
    EMB_CHECK(text_is(out, "second"));
    EMB_CHECK(embi_js_job_pop(rt, &out));
    EMB_CHECK_EQ_INT(embi_js_as_int32(out), 3);
    EMB_CHECK(!embi_js_job_pop(rt, &out));
    embi_js_runtime_free(rt);
    scrub_stack();
}

/* The queue never drains here, so only compaction keeps the backing array bounded. Order must
 * still be FIFO across compactions. */
GC_TEST(runtime_job_queue_stays_bounded_under_steady_load)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    for (int i = 0; i < 8; i++) {
        embi_js_job_push(rt, embi_js_from_int32(i));
    }
    for (int i = 0; i < 100000; i++) {
        embi_js_value out = EMBI_JS_EMPTY;
        EMB_REQUIRE(embi_js_job_pop(rt, &out));
        EMB_CHECK_EQ_INT(embi_js_as_int32(out), i);
        embi_js_job_push(rt, embi_js_from_int32(i + 8));
    }
    EMB_CHECK(rt->jobs.len < 1024u);
    embi_js_value out = EMBI_JS_EMPTY;
    EMB_REQUIRE(embi_js_job_pop(rt, &out));
    EMB_CHECK_EQ_INT(embi_js_as_int32(out), 100000);
    embi_js_runtime_free(rt);
    scrub_stack();
}

static embi_js_value g_root_slot;
static int g_root_calls;

static void mark_root_slot(embi_js_tracer *t, void *user)
{
    (void)user;
    g_root_calls++;
    embi_js_mark_value(t, g_root_slot);
}

GC_TEST(runtime_root_callbacks_keep_values_alive)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    g_root_slot = make_text(rt, "from a root");
    g_root_calls = 0;
    embi_js_root_add(rt, mark_root_slot, nullptr);

    scrub_stack();
    embi_js_gc(rt);
    EMB_CHECK(g_root_calls >= 1);
    EMB_CHECK(text_is(g_root_slot, "from a root"));

    embi_js_root_remove(rt, mark_root_slot, nullptr);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(runtime_pending_exception_is_rooted_and_clears)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK(embi_js_is_undefined(embi_js_exception_get(rt)));
    embi_js_exception_set(rt, make_text(rt, "boom"));
    scrub_stack();
    embi_js_gc(rt);
    EMB_CHECK(text_is(embi_js_exception_get(rt), "boom"));
    embi_js_exception_clear(rt);
    EMB_CHECK(embi_js_is_undefined(embi_js_exception_get(rt)));
    EMB_CHECK(EMBI_JS_EXCEPTION != EMBI_JS_EMPTY);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(runtime_mem_stats_track_handles_atoms_and_collections)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK_EQ_INT(st.persistent_handles, 0);
    EMB_CHECK_EQ_INT(st.weak_handles, 0);
    EMB_CHECK_EQ_INT(st.atoms, 0);

    uint32_t p = embi_js_persistent_new(rt, EMBI_JS_TRUE);
    uint32_t w = embi_js_weak_new(rt, EMBI_JS_TRUE);
    (void)embi_js_atom_intern_utf8(rt, "a", 1);
    (void)embi_js_atom_intern_utf8(rt, "b", 1);
    embi_js_gc(rt);
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK_EQ_INT(st.persistent_handles, 1);
    EMB_CHECK_EQ_INT(st.weak_handles, 1);
    EMB_CHECK_EQ_INT(st.atoms, 2);
    EMB_CHECK(st.gc_count >= 1);
    EMB_CHECK(st.heap_chunks >= 1);
    EMB_CHECK(st.heap_live_bytes > 0);
    /* Immediates are never cleared by the collector. */
    EMB_CHECK(embi_js_weak_get(rt, w) == EMBI_JS_TRUE);
    embi_js_persistent_free(rt, p);
    embi_js_weak_free(rt, w);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(runtime_gc_params_control_the_trigger)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_set_gc_params(rt, 4096, 0.0);
    uint8_t text[64];
    for (size_t i = 0; i < sizeof text; i++) {
        text[i] = 'g';
    }
    for (int i = 0; i < 2000; i++) {
        (void)embi_js_str_new_latin1(rt, text, sizeof text);
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    /* 2000 cells of 80 bytes is 160 KB, collected every 4 KB. */
    EMB_CHECK(st.gc_count >= 20);
    EMB_CHECK(st.heap_live_bytes < 64 * 1024);
    embi_js_set_gc_params(rt, (size_t)1 << 20, 1.0);
    embi_js_runtime_free(rt);
    scrub_stack();
}

GC_TEST(runtime_empty_collection_is_harmless)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    for (int i = 0; i < 3; i++) {
        embi_js_gc(rt);
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK_EQ_INT(st.gc_count, 3);
    EMB_CHECK_EQ_INT(st.heap_live_bytes, 0);
    embi_js_runtime_free(rt);
    scrub_stack();
}

EMB_TEST_MAIN()
