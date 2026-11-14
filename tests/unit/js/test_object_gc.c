#include "emb_test.h"

#include "js/object.h"
#include "js/ops.h"

/* Storage owned by objects (elements, slots, dictionaries, bound arguments) lives outside the
 * cell. These tests check that such growth drives collections and that the live memory stays
 * bounded, instead of letting a loop grow memory until it runs out. */

#define MIB (1024u * 1024u)

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static embi_js_value from(embi_js_object *o)
{
    return embi_js_from_object(o);
}

static size_t live_above(size_t base)
{
    size_t now = embi_mem_live();
    return now > base ? now - base : 0;
}

EMB_TEST(large_element_arrays_drive_collections_and_bound_live_memory)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    size_t base = embi_mem_live();
    size_t peak = 0;
    for (uint32_t n = 0; n < 60; n++) {
        embi_js_object *a = embi_js_array_create_default(rt, 0);
        for (uint32_t i = 0; i < 100000u; i++) {
            embi_js_obj_set(rt, a, embi_js_key_from_index(i), embi_js_from_int32((int32_t)i),
                            from(a));
        }
        size_t live = live_above(base);
        if (live > peak) {
            peak = live;
        }
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);
    /* Each array holds about 1.2 MB of elements. Without accounting, 60 arrays would stay live. */
    EMB_CHECK(peak < 16u * MIB);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(dictionary_storage_drives_collections_and_bounds_live_memory)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    size_t base = embi_mem_live();
    embi_js_key keys[70];
    for (uint32_t i = 0; i < 70u; i++) {
        char nm[16];
        SDL_snprintf(nm, sizeof nm, "p%u", (unsigned)i);
        keys[i] = key(rt, nm);
    }
    size_t peak = 0;
    for (uint32_t n = 0; n < 3000u; n++) {
        /* 70 named properties: past the shape chain limit, so the object is in dictionary mode. */
        embi_js_object *o = embi_js_object_new_plain(rt);
        for (uint32_t i = 0; i < 70u; i++) {
            embi_js_define_raw(rt, o, keys[i], embi_js_from_int32((int32_t)i),
                               EMBI_JS_ATTR_DEFAULT);
        }
        size_t live = live_above(base);
        if (live > peak) {
            peak = live;
        }
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);
    EMB_CHECK(peak < 8u * MIB);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

static embi_js_value fn_count_args(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                   int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    (void)argv;
    return embi_js_from_int32(argc);
}

static const embi_js_class test_count_class = {.name = "TestCount", .call = fn_count_args};

EMB_TEST(bound_argument_storage_drives_collections)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *target = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_count_class);
    size_t base = embi_mem_live();
    static embi_js_value args[1000];
    for (uint32_t i = 0; i < 1000u; i++) {
        args[i] = embi_js_from_int32((int32_t)i);
    }
    size_t peak = 0;
    for (uint32_t n = 0; n < 3000u; n++) {
        embi_js_object *b = embi_js_bound_function_new(rt, target, EMBI_JS_UNDEFINED, 1000, args);
        EMB_CHECK(embi_js_is_object(from(b)));
        size_t live = live_above(base);
        if (live > peak) {
            peak = live;
        }
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);
    EMB_CHECK(peak < 16u * MIB);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* Object operations running across many collections, with integrity levels, descriptor round
 * trips and enumeration in the same loop. */
EMB_TEST(object_operations_stay_correct_under_collection_pressure)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_set_gc_params(rt, 4096, 0.1);
    for (int n = 0; n < 2000; n++) {
        embi_js_object *a = embi_js_array_create_default(rt, 0);
        for (int i = 0; i < 70; i++) {
            char nm[16];
            SDL_snprintf(nm, sizeof nm, "k%d", i);
            embi_js_put(rt, a, key(rt, nm), from(embi_js_object_new_plain(rt)), true);
        }
        for (int i = 0; i < 20; i++) {
            embi_js_obj_set(rt, a, embi_js_key_from_index((uint32_t)i * 3u),
                            embi_js_from_int32(i), from(a));
        }
        EMB_CHECK(embi_js_set_integrity(rt, a, (n & 1) != 0) == EMBI_JS_YES);
        embi_js_key_vec kv = {nullptr, 0, 0};
        embi_js_enumerable_own_keys(rt, a, &kv);
        EMB_CHECK_EQ_INT((int)kv.len, 90);
        embi_vec_free(&kv);
        embi_js_pdesc d;
        EMB_CHECK(embi_js_obj_get_own_property(rt, a, key(rt, "k3"), &d) == EMBI_JS_YES);
        embi_js_value dv = embi_js_from_property_descriptor(rt, &d);
        EMB_CHECK(embi_js_is_object(dv));
        embi_js_pdesc d2;
        EMB_CHECK(embi_js_to_property_descriptor(rt, dv, &d2));
        EMB_CHECK(d2.has == d.has);
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
