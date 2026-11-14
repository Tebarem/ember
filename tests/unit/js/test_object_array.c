#include "emb_test.h"

#include "js/object.h"
#include "js/ops.h"

static embi_js_value fn_sum2(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                             int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    int32_t total = 0;
    for (int i = 0; i < argc; i++) {
        total += embi_js_as_int32(argv[i]);
    }
    return embi_js_from_int32(total);
}

static const embi_js_class test_sum_class = {.name = "TestSum", .call = fn_sum2};

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static double length_of(embi_js_runtime *rt, embi_js_object *o)
{
    return embi_js_as_double(embi_js_get(rt, o, key(rt, "length")));
}

static embi_js_object *make_array(embi_js_runtime *rt, int n)
{
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    for (int i = 0; i < n; i++) {
        embi_js_obj_set(rt, a, embi_js_key_from_index((uint32_t)i), embi_js_from_int32(i * 10),
                        embi_js_from_object(a));
    }
    return a;
}

EMB_TEST(array_index_writes_grow_length)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = make_array(rt, 4);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 4);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(9), embi_js_from_int32(1),
                              embi_js_from_object(a)) == EMBI_JS_YES);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 10);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(7)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(setting_length_truncates_elements)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = make_array(rt, 6);
    embi_js_pdesc d = {embi_js_from_int32(2), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                       EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_YES);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 2);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(3)));
    EMB_CHECK(embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(1)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(invalid_length_throws_range_error_and_keeps_array)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = make_array(rt, 3);
    embi_js_pdesc d = {embi_js_from_double(1.5), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                       EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_THREW);
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(rt))); /* a RangeError is pending */
    embi_js_exception_clear(rt);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 3);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(non_configurable_element_stops_length_truncation)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = make_array(rt, 5);
    /* Element 2 becomes non-configurable, so truncation to length 1 stops above it. */
    embi_js_pdesc lock = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                          EMBI_JS_PD_CONFIGURABLE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, embi_js_key_from_index(2), &lock) ==
              EMBI_JS_YES);
    embi_js_pdesc d = {embi_js_from_int32(1), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                       EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_NO);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 3);
    EMB_CHECK(embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(2)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(non_writable_length_blocks_new_indices)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = make_array(rt, 2);
    embi_js_pdesc ro = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                        EMBI_JS_PD_WRITABLE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &ro) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(5), embi_js_from_int32(1),
                              embi_js_from_object(a)) == EMBI_JS_NO);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 2);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(string_wrapper_exposes_code_units_and_length)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_string *s = embi_js_str_new_utf8(rt, "abc", 3);
    embi_js_object *w = embi_js_primitive_wrapper_new(rt, embi_js_from_string(s));
    EMB_CHECK_EQ_INT((int)length_of(rt, w), 3);
    embi_js_value c = embi_js_get(rt, w, embi_js_key_from_index(1));
    EMB_CHECK(embi_js_is_string(c));
    EMB_CHECK_EQ_INT(embi_js_str_at(embi_js_as_string(c), 0), 'b');

    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, w, &keys);
    EMB_REQUIRE(keys.len == 4); /* 0, 1, 2, length */
    EMB_CHECK(embi_js_key_index(keys.data[2]) == 2);
    embi_vec_free(&keys);

    /* Index properties are read-only and non-configurable. */
    embi_js_pdesc d = {embi_js_from_int32(9), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                       EMBI_JS_PD_VALUE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, w, embi_js_key_from_index(0), &d) ==
              EMBI_JS_NO);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bound_function_prepends_arguments_and_fixes_this)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *target = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_sum_class);
    embi_js_value bound_args[2] = {embi_js_from_int32(10), embi_js_from_int32(20)};
    embi_js_object *bound =
        embi_js_bound_function_new(rt, target, EMBI_JS_UNDEFINED, 2, bound_args);
    embi_js_value call_args[1] = {embi_js_from_int32(3)};
    embi_js_value r = embi_js_call(rt, embi_js_from_object(bound), EMBI_JS_UNDEFINED, 1,
                                   call_args);
    EMB_CHECK(embi_js_is_int32(r));
    EMB_CHECK_EQ_INT(embi_js_as_int32(r), 33);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(array_create_rejects_lengths_above_uint32)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    EMB_CHECK(embi_js_array_create_default(rt, 4294967296.0) == nullptr);
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(rt))); /* a RangeError is pending */
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

static void set_index(embi_js_runtime *rt, embi_js_object *o, uint32_t i, int32_t v)
{
    embi_js_obj_set(rt, o, embi_js_key_from_index(i), embi_js_from_int32(v), embi_js_from_object(o));
}

static embi_js_pdesc value_desc(int32_t v)
{
    return (embi_js_pdesc){embi_js_from_int32(v), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                           EMBI_JS_PD_VALUE, 0};
}

/* Length truncation walks indices from the top and stops at the first non-configurable element. */
EMB_TEST(sparse_array_length_truncation_stops_at_non_configurable_element)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    set_index(rt, a, 5000000u, 1);
    set_index(rt, a, 10, 2);
    set_index(rt, a, 3, 3);
    EMB_CHECK(embi_js_obj_sparse(a) != nullptr);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 5000001);

    embi_js_pdesc lock = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                          EMBI_JS_PD_CONFIGURABLE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, embi_js_key_from_index(10), &lock) ==
              EMBI_JS_YES);
    embi_js_pdesc d = value_desc(1);
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_NO);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 11);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(5000000u)));
    EMB_CHECK(embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(10)));
    EMB_CHECK(embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(3)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* Shrinking a dense array hides the old tail, and growing it again must not revive the values. */
EMB_TEST(dense_array_shrink_then_regrow_leaves_holes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    for (int i = 0; i < 10; i++) {
        set_index(rt, a, (uint32_t)i, i + 100);
    }
    embi_js_pdesc d = value_desc(2);
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_YES);
    d = value_desc(10);
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_YES);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(5)));
    set_index(rt, a, 7, 1);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(5)));
    EMB_CHECK(!embi_js_obj_has_own_property(rt, a, embi_js_key_from_index(3)));
    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_js_obj_own_keys(rt, a, &keys);
    EMB_CHECK_EQ_INT((int)keys.len, 4); /* indices 0, 1 and 7, then length */
    embi_vec_free(&keys);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* Frozen arrays reject element writes and length changes, and report frozen through the test. */
EMB_TEST(frozen_array_and_frozen_object_reject_writes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    for (int i = 0; i < 5; i++) {
        set_index(rt, a, (uint32_t)i, i);
    }
    embi_js_object *g = embi_js_object_new_plain(rt);
    embi_js_define_accessor_raw(rt, g, key(rt, "acc"), EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                                EMBI_JS_ATTR_DEFAULT);
    EMB_CHECK(embi_js_set_integrity(rt, a, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_set_integrity(rt, g, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_test_integrity(rt, a, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_test_integrity(rt, g, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(1), embi_js_from_int32(9),
                              embi_js_from_object(a)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(9), embi_js_from_int32(9),
                              embi_js_from_object(a)) == EMBI_JS_NO);
    embi_js_pdesc d = value_desc(1);
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &d) == EMBI_JS_NO);
    EMB_CHECK_EQ_INT((int)length_of(rt, a), 5);
    embi_js_pdesc cur;
    EMB_CHECK(embi_js_obj_get_own_property(rt, a, key(rt, "length"), &cur) == EMBI_JS_YES);
    EMB_CHECK((cur.attrs & EMBI_JS_ATTR_WRITABLE) == 0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* Array index keys: 2^32 - 2 is the largest index, and 2^32 - 1 is an ordinary string key. */
EMB_TEST(largest_array_index_extends_length_but_max_uint32_is_a_plain_key)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(4294967294u), embi_js_from_int32(1),
                              embi_js_from_object(a)) == EMBI_JS_YES);
    EMB_CHECK((double)length_of(rt, a) == 4294967295.0);
    embi_js_key k = embi_js_to_property_key(rt, embi_js_from_double(4294967295.0));
    EMB_CHECK(!embi_js_key_is_index(k));
    EMB_CHECK(embi_js_obj_set(rt, a, k, embi_js_from_int32(1), embi_js_from_object(a)) ==
              EMBI_JS_YES);
    EMB_CHECK((double)length_of(rt, a) == 4294967295.0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* String wrappers: code-unit indices and length are read-only, and extra properties are not. */
EMB_TEST(string_wrapper_set_and_delete_respect_code_units)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_string *s = embi_js_str_new_utf8(rt, "abc", 3);
    embi_js_object *w = embi_js_primitive_wrapper_new(rt, embi_js_from_string(s));
    embi_js_value self = embi_js_from_object(w);

    EMB_CHECK(embi_js_obj_set(rt, w, embi_js_key_from_index(0), embi_js_from_int32(1), self) ==
              EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set(rt, w, key(rt, "length"), embi_js_from_int32(1), self) ==
              EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set(rt, w, embi_js_key_from_index(5), embi_js_from_int32(7), self) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_get(rt, w, embi_js_key_from_index(5)) == embi_js_from_int32(7));
    EMB_CHECK(embi_js_obj_set(rt, w, key(rt, "extra"), embi_js_from_int32(2), self) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_get(rt, w, key(rt, "extra")) == embi_js_from_int32(2));

    EMB_CHECK(embi_js_obj_delete(rt, w, embi_js_key_from_index(0)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_delete(rt, w, key(rt, "length")) == EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_delete(rt, w, embi_js_key_from_index(9)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_delete(rt, w, key(rt, "extra")) == EMBI_JS_YES);
    EMB_CHECK(!embi_js_obj_has_own_property(rt, w, key(rt, "extra")));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
