#include "emb_test.h"

#include "js/object.h"
#include "js/ops.h"

/* Native test functions: the class hook is the [[Call]] of every object of this class. */
static int g_last_set = -1;

static embi_js_value fn_ret_42(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                               int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_from_int32(42);
}

static embi_js_value fn_record_set(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                   int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    g_last_set = argc > 0 ? embi_js_as_int32(argv[0]) : -2;
    return EMBI_JS_UNDEFINED;
}

static const embi_js_class test_getter_class = {.name = "TestGetter", .call = fn_ret_42};
static const embi_js_class test_setter_class = {.name = "TestSetter", .call = fn_record_set};

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static int32_t get_int(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_value v = embi_js_get(rt, o, k);
    return embi_js_is_int32(v) ? embi_js_as_int32(v) : -999999;
}

static void put_int(embi_js_runtime *rt, embi_js_object *o, const char *name, int32_t v)
{
    embi_js_define_raw(rt, o, key(rt, name), embi_js_from_int32(v), EMBI_JS_ATTR_DEFAULT);
}

static uint32_t key_count(embi_js_runtime *rt, embi_js_object *o)
{
    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, o, &keys);
    uint32_t n = keys.len;
    embi_vec_free(&keys);
    return n;
}

EMB_TEST(shapes_are_shared_between_objects_with_the_same_layout)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_object_new_plain(rt);
    embi_js_object *b = embi_js_object_new_plain(rt);
    put_int(rt, a, "x", 1);
    put_int(rt, a, "y", 2);
    put_int(rt, b, "x", 3);
    put_int(rt, b, "y", 4);
    EMB_CHECK(a->shape == b->shape);
    EMB_CHECK_EQ_INT(get_int(rt, b, key(rt, "y")), 4);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(own_keys_are_indices_then_strings_then_symbols_in_creation_order)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_int(rt, o, "b", 1);
    embi_js_define_raw(rt, o, embi_js_key_from_index(5), embi_js_from_int32(2),
                       EMBI_JS_ATTR_DEFAULT);
    put_int(rt, o, "a", 3);
    embi_js_value sym = embi_js_from_symbol(
        embi_js_symbol_new(rt, embi_js_from_string(embi_js_str_new_utf8(rt, "s", 1))));
    embi_js_define_raw(rt, o, sym, embi_js_from_int32(4), EMBI_JS_ATTR_DEFAULT);

    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, o, &keys);
    EMB_REQUIRE(keys.len == 4);
    EMB_CHECK(embi_js_key_is_index(keys.data[0]) && embi_js_key_index(keys.data[0]) == 5);
    EMB_CHECK(keys.data[1] == key(rt, "b"));
    EMB_CHECK(keys.data[2] == key(rt, "a"));
    EMB_CHECK(keys.data[3] == sym);
    embi_vec_free(&keys);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(deleting_the_last_property_steps_back_and_middle_deletes_keep_order)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_int(rt, o, "a", 1);
    put_int(rt, o, "b", 2);
    put_int(rt, o, "c", 3);
    embi_js_shape *before_c = o->shape->parent;
    EMB_CHECK(embi_js_obj_delete(rt, o, key(rt, "c")) == EMBI_JS_YES);
    EMB_CHECK(o->shape == before_c);
    EMB_CHECK(embi_js_obj_dict(o) == nullptr);

    put_int(rt, o, "d", 4);
    EMB_CHECK(embi_js_obj_delete(rt, o, key(rt, "a")) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_dict(o) != nullptr); /* non-last delete goes to dictionary mode */
    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "a")), -999999);
    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "b")), 2);
    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "d")), 4);

    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, o, &keys);
    EMB_REQUIRE(keys.len == 2);
    EMB_CHECK(keys.data[0] == key(rt, "b"));
    EMB_CHECK(keys.data[1] == key(rt, "d"));
    embi_vec_free(&keys);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(non_writable_property_rejects_set_and_defineproperty_validates)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, key(rt, "fixed"), embi_js_from_int32(1),
                       EMBI_JS_ATTR_ENUMERABLE); /* not writable, not configurable */

    EMB_CHECK(embi_js_obj_set(rt, o, key(rt, "fixed"), embi_js_from_int32(9),
                              embi_js_from_object(o)) == EMBI_JS_NO);
    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "fixed")), 1);

    /* Changing the value of a non-configurable, non-writable property fails. */
    embi_js_pdesc d = {embi_js_from_int32(2), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                       EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, o, key(rt, "fixed"), &d) == EMBI_JS_NO);
    /* Re-defining with the same value is allowed. */
    d.value = embi_js_from_int32(1);
    EMB_CHECK(embi_js_obj_define_own_property(rt, o, key(rt, "fixed"), &d) == EMBI_JS_YES);

    /* A non-extensible object refuses new properties. */
    embi_js_obj_prevent_extensions(rt, o);
    embi_js_pdesc nd = {embi_js_from_int32(5), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                        EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, o, key(rt, "fresh"), &nd) == EMBI_JS_NO);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(accessor_property_calls_getter_and_setter_through_the_prototype_chain)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *getter = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_getter_class);
    embi_js_object *setter = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_setter_class);
    embi_js_object *proto = embi_js_object_new_plain(rt);
    embi_js_define_accessor_raw(rt, proto, key(rt, "acc"), embi_js_from_object(getter),
                                embi_js_from_object(setter), EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_object *o = embi_js_object_new(rt, embi_js_from_object(proto));

    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "acc")), 42);
    g_last_set = -1;
    EMB_CHECK(embi_js_obj_set(rt, o, key(rt, "acc"), embi_js_from_int32(7),
                              embi_js_from_object(o)) == EMBI_JS_YES);
    EMB_CHECK_EQ_INT(g_last_set, 7);
    /* The accessor lives on the prototype, so the receiver gets no own property. */
    EMB_CHECK(!embi_js_obj_has_own_property(rt, o, key(rt, "acc")));
    EMB_CHECK(embi_js_obj_has_property(rt, o, key(rt, "acc")) == EMBI_JS_YES);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(setting_through_an_inherited_data_property_creates_an_own_property)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *proto = embi_js_object_new_plain(rt);
    put_int(rt, proto, "x", 1);
    embi_js_object *o = embi_js_object_new(rt, embi_js_from_object(proto));
    EMB_CHECK(embi_js_obj_set(rt, o, key(rt, "x"), embi_js_from_int32(2),
                              embi_js_from_object(o)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_has_own_property(rt, o, key(rt, "x")));
    EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, "x")), 2);
    EMB_CHECK_EQ_INT(get_int(rt, proto, key(rt, "x")), 1);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(many_properties_switch_to_dictionary_mode_and_stay_correct)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    char name[32];
    for (int i = 0; i < 200; i++) {
        SDL_snprintf(name, sizeof name, "p%d", i);
        put_int(rt, o, name, i);
    }
    EMB_CHECK(embi_js_obj_dict(o) != nullptr);
    EMB_CHECK_EQ_INT(key_count(rt, o), 200);
    for (int i = 0; i < 200; i += 2) {
        SDL_snprintf(name, sizeof name, "p%d", i);
        EMB_CHECK(embi_js_obj_delete(rt, o, key(rt, name)) == EMBI_JS_YES);
    }
    EMB_CHECK_EQ_INT(key_count(rt, o), 100);
    for (int i = 1; i < 200; i += 2) {
        SDL_snprintf(name, sizeof name, "p%d", i);
        EMB_CHECK_EQ_INT(get_int(rt, o, key(rt, name)), i);
    }
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(sparse_elements_store_far_indices_and_iterate_in_order)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_int(rt, o, "z", 0);
    embi_js_define_raw(rt, o, embi_js_key_from_index(1000000), embi_js_from_int32(11),
                       EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_key_from_index(3), embi_js_from_int32(12),
                       EMBI_JS_ATTR_DEFAULT);
    EMB_CHECK(embi_js_obj_sparse(o) != nullptr);
    EMB_CHECK_EQ_INT(get_int(rt, o, embi_js_key_from_index(1000000)), 11);
    EMB_CHECK_EQ_INT(get_int(rt, o, embi_js_key_from_index(3)), 12);

    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, o, &keys);
    EMB_REQUIRE(keys.len == 3);
    EMB_CHECK(embi_js_key_index(keys.data[0]) == 3);
    EMB_CHECK(embi_js_key_index(keys.data[1]) == 1000000);
    EMB_CHECK(keys.data[2] == key(rt, "z"));
    embi_vec_free(&keys);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(dense_elements_hold_holes_and_respect_attributes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_key_from_index(2), embi_js_from_int32(5),
                       EMBI_JS_ATTR_DEFAULT);
    EMB_CHECK(embi_js_obj_sparse(o) == nullptr);
    EMB_CHECK_EQ_INT(key_count(rt, o), 1);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, o, embi_js_key_from_index(0)));
    /* A non-default attribute turns the element into a sparse entry. */
    embi_js_define_raw(rt, o, embi_js_key_from_index(2), embi_js_from_int32(6),
                       EMBI_JS_ATTR_WRITABLE);
    EMB_CHECK(embi_js_obj_sparse(o) != nullptr);
    EMB_CHECK_EQ_INT(get_int(rt, o, embi_js_key_from_index(2)), 6);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(setprototypeof_rejects_cycles_and_immutable_object_prototype)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *base = embi_js_object_new_plain(rt);
    embi_js_object *child = embi_js_object_new(rt, embi_js_from_object(base));
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, base, embi_js_from_object(child)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, child, EMBI_JS_NULL) == EMBI_JS_YES);
    EMB_CHECK(embi_js_is_null(embi_js_obj_proto(child)));

    /* Once registered as Object.prototype, the object refuses a different prototype. */
    rt->protos[EMBI_JS_PROTO_OBJECT] = embi_js_from_object(base);
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, base, embi_js_from_object(child)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, base, embi_js_obj_proto(base)) == EMBI_JS_YES);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(objects_survive_collection_only_while_reachable)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *kept = embi_js_object_new_plain(rt);
    put_int(rt, kept, "v", 123);
    uint32_t handle = embi_js_persistent_new(rt, embi_js_from_object(kept));
    kept = nullptr;

    for (int i = 0; i < 5000; i++) {
        embi_js_object *junk = embi_js_object_new_plain(rt);
        put_int(rt, junk, "w", i);
        embi_js_define_raw(rt, junk, embi_js_key_from_index((uint32_t)i),
                           embi_js_from_int32(i), EMBI_JS_ATTR_DEFAULT);
    }
    embi_js_mem_stats before;
    embi_js_get_mem_stats(rt, &before);
    embi_js_gc(rt);
    embi_js_mem_stats after;
    embi_js_get_mem_stats(rt, &after);
    EMB_CHECK(after.heap_live_bytes < before.heap_live_bytes);

    embi_js_object *back = embi_js_as_object(embi_js_persistent_get(rt, handle));
    EMB_CHECK_EQ_INT(get_int(rt, back, key(rt, "v")), 123);
    embi_js_persistent_free(rt, handle);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(automatic_collections_during_object_churn_keep_the_live_graph)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_set_gc_params(rt, 32 * 1024, 1.0);
    embi_js_object *root = embi_js_object_new_plain(rt); /* 500 props: dictionary mode */
    uint32_t handle = embi_js_persistent_new(rt, embi_js_from_object(root));
    char name[32];
    for (int i = 0; i < 20000; i++) {
        embi_js_object *child = embi_js_object_new_plain(rt);
        put_int(rt, child, "i", i);
        put_int(rt, child, "twice", 2 * i);
        SDL_snprintf(name, sizeof name, "k%d", i % 500);
        embi_js_put(rt, root, key(rt, name), embi_js_from_object(child), true);
    }
    embi_js_mem_stats st;
    embi_js_get_mem_stats(rt, &st);
    EMB_CHECK(st.gc_count > 0);

    root = embi_js_as_object(embi_js_persistent_get(rt, handle));
    EMB_CHECK_EQ_INT(key_count(rt, root), 500);
    for (int j = 0; j < 500; j++) {
        SDL_snprintf(name, sizeof name, "k%d", j);
        embi_js_value c = embi_js_get(rt, root, key(rt, name));
        EMB_REQUIRE(embi_js_is_object(c));
        int32_t i = get_int(rt, embi_js_as_object(c), key(rt, "i"));
        EMB_CHECK(i % 500 == j);
        EMB_CHECK_EQ_INT(get_int(rt, embi_js_as_object(c), key(rt, "twice")), 2 * i);
    }
    embi_js_persistent_free(rt, handle);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(runtime_free_returns_all_object_memory)
{
    size_t base = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    for (int i = 0; i < 300; i++) {
        embi_js_object *o = embi_js_object_new_plain(rt);
        char name[24];
        SDL_snprintf(name, sizeof name, "k%d", i);
        put_int(rt, o, name, i);
        embi_js_define_raw(rt, o, embi_js_key_from_index((uint32_t)i), embi_js_from_int32(i),
                           EMBI_JS_ATTR_DEFAULT);
    }
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
    EMB_CHECK(embi_mem_live() == base);
}

/* Redefining a data property as an accessor keeps E and C and drops the value, and back again
 * drops the accessor and makes the property read-only by default. */
EMB_TEST(accessor_and_data_redefinition_convert_in_place)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_int(rt, o, "x", 1);
    embi_js_object *getter = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_getter_class);
    embi_js_pdesc acc = {EMBI_JS_UNDEFINED, embi_js_from_object(getter), EMBI_JS_UNDEFINED,
                         EMBI_JS_PD_GET, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, o, key(rt, "x"), &acc) == EMBI_JS_YES);
    embi_js_pdesc cur;
    EMB_CHECK(embi_js_obj_get_own_property(rt, o, key(rt, "x"), &cur) == EMBI_JS_YES);
    EMB_CHECK(embi_js_pd_is_accessor(&cur));
    EMB_CHECK((cur.attrs & EMBI_JS_ATTR_ENUMERABLE) != 0);
    EMB_CHECK((cur.attrs & EMBI_JS_ATTR_CONFIGURABLE) != 0);
    EMB_CHECK(get_int(rt, o, key(rt, "x")) == 42);

    embi_js_pdesc data = {embi_js_from_int32(5), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                          EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, o, key(rt, "x"), &data) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_get_own_property(rt, o, key(rt, "x"), &cur) == EMBI_JS_YES);
    EMB_CHECK(!embi_js_pd_is_accessor(&cur));
    EMB_CHECK(cur.value == embi_js_from_int32(5));
    EMB_CHECK((cur.attrs & EMBI_JS_ATTR_WRITABLE) == 0);
    EMB_CHECK(get_int(rt, o, key(rt, "x")) == 5);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* setPrototypeOf rebuilds the shape chain on the new prototype. Values, key order and sharing
 * with ordinarily built shapes must all survive, across collections. */
EMB_TEST(setprototypeof_rebuilds_the_chain_and_keeps_values_and_sharing)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    put_int(rt, o, "a", 1);
    put_int(rt, o, "b", 2);
    put_int(rt, o, "c", 3);
    embi_js_object *p1 = embi_js_object_new_plain(rt);
    embi_js_object *p2 = embi_js_object_new_plain(rt);
    put_int(rt, p1, "inherited", 10);
    put_int(rt, p2, "inherited", 20);

    EMB_CHECK(embi_js_obj_set_prototype_of(rt, o, embi_js_from_object(p1)) == EMBI_JS_YES);
    EMB_CHECK(get_int(rt, o, key(rt, "inherited")) == 10);
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, o, embi_js_from_object(p2)) == EMBI_JS_YES);
    EMB_CHECK(get_int(rt, o, key(rt, "inherited")) == 20);
    embi_js_gc(rt);
    EMB_CHECK(get_int(rt, o, key(rt, "a")) == 1);
    EMB_CHECK(get_int(rt, o, key(rt, "b")) == 2);
    EMB_CHECK(get_int(rt, o, key(rt, "c")) == 3);
    EMB_CHECK(o->shape->count == 3);
    EMB_CHECK_EQ_INT((int)key_count(rt, o), 3);

    /* Moving to null leaves the chain for the null root. An object built the same way shares
     * its shapes with o. */
    EMB_CHECK(embi_js_obj_set_prototype_of(rt, o, EMBI_JS_NULL) == EMBI_JS_YES);
    EMB_CHECK(get_int(rt, o, key(rt, "inherited")) == -999999);
    embi_js_object *twin = embi_js_object_new_plain(rt); /* null prototype, like o now */
    put_int(rt, twin, "a", 7);
    put_int(rt, twin, "b", 8);
    put_int(rt, twin, "c", 9);
    EMB_CHECK(twin->shape == o->shape);
    embi_js_gc(rt);
    EMB_CHECK(get_int(rt, o, key(rt, "b")) == 2);
    EMB_CHECK(get_int(rt, twin, key(rt, "b")) == 8);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* Two transitions from one node that differ only in attributes for the same key: each gets its
 * own shape, and each object finds the one matching its layout. */
EMB_TEST(shape_transitions_keep_attribute_variants_of_one_key_apart)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o1 = embi_js_object_new_plain(rt);
    put_int(rt, o1, "a", 1);
    embi_js_shape *s_a = o1->shape;

    embi_js_object *o2 = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o2, key(rt, "a"), embi_js_from_int32(2), EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(o2->shape != s_a);

    embi_js_object *o3 = embi_js_object_new_plain(rt);
    put_int(rt, o3, "a", 3);
    EMB_CHECK(o3->shape == s_a);

    embi_js_object *o4 = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o4, key(rt, "a"), embi_js_from_int32(4), EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(o4->shape == o2->shape);

    embi_js_pdesc d;
    EMB_CHECK(embi_js_obj_get_own_property(rt, o2, key(rt, "a"), &d) == EMBI_JS_YES);
    EMB_CHECK((d.attrs & EMBI_JS_ATTR_ENUMERABLE) == 0);
    EMB_CHECK(get_int(rt, o2, key(rt, "a")) == 2);
    EMB_CHECK(get_int(rt, o4, key(rt, "a")) == 4);
    EMB_CHECK(get_int(rt, o3, key(rt, "a")) == 3);

    /* A second key on the default-attribute "a" node, and the same on a fresh object. */
    put_int(rt, o1, "b", 11);
    embi_js_object *o5 = embi_js_object_new_plain(rt);
    put_int(rt, o5, "a", 5);
    put_int(rt, o5, "b", 15);
    EMB_CHECK(o5->shape == o1->shape);
    EMB_CHECK(get_int(rt, o1, key(rt, "b")) == 11);
    EMB_CHECK(get_int(rt, o5, key(rt, "b")) == 15);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
