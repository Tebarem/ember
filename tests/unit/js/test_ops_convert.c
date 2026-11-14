#include "emb_test.h"

#include <math.h>
#include <string.h>

#include "js/object.h"
#include "js/ops.h"

static embi_js_value text(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_str_new_utf8(rt, s, strlen(s)));
}

/* Compares a string value's UTF-8 form with a C string. */
static bool text_is(embi_js_value v, const char *s)
{
    if (!embi_js_is_string(v)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(v), &sb);
    bool same = sb.len == strlen(s) && memcmp(sb.data ? sb.data : "", s, sb.len) == 0;
    embi_sb_free(&sb);
    return same;
}

static embi_js_value ret_seven(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                               int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_from_int32(7);
}

static const embi_js_class seven_class = {.name = "Seven", .call = ret_seven};

EMB_TEST(to_number_follows_string_to_number_rules)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    double d = 0;
    EMB_CHECK(embi_js_to_number(rt, text(rt, "  12  "), &d));
    EMB_CHECK(d == 12.0);
    EMB_CHECK(embi_js_to_number(rt, text(rt, "0x1F"), &d));
    EMB_CHECK(d == 31.0);
    EMB_CHECK(embi_js_to_number(rt, text(rt, ""), &d));
    EMB_CHECK(d == 0.0);
    EMB_CHECK(embi_js_to_number(rt, text(rt, "abc"), &d));
    EMB_CHECK(d != d);
    EMB_CHECK(embi_js_to_number(rt, EMBI_JS_NULL, &d));
    EMB_CHECK(d == 0.0);
    EMB_CHECK(embi_js_to_number(rt, EMBI_JS_TRUE, &d));
    EMB_CHECK(d == 1.0);
    EMB_CHECK(embi_js_to_number(rt, EMBI_JS_UNDEFINED, &d));
    EMB_CHECK(d != d);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(to_string_formats_numbers_and_specials)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, embi_js_from_int32(1))), "1"));
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, embi_js_from_double(1.5))),
                      "1.5"));
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, embi_js_from_double(-0.0))),
                      "0"));
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, embi_js_from_double(NAN))),
                      "NaN"));
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, EMBI_JS_TRUE)), "true"));
    EMB_CHECK(text_is(embi_js_from_string(embi_js_to_string(rt, EMBI_JS_NULL)), "null"));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(to_int32_and_uint32_wrap_modulo_two_to_the_32)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    int32_t i = 0;
    uint32_t u = 0;
    EMB_CHECK(embi_js_to_int32(rt, embi_js_from_double(4294967301.0), &i));
    EMB_CHECK(i == 5);
    EMB_CHECK(embi_js_to_uint32(rt, embi_js_from_int32(-1), &u));
    EMB_CHECK(u == 0xFFFFFFFFu);
    EMB_CHECK(embi_js_to_int32(rt, embi_js_from_double(-2147483649.0), &i));
    EMB_CHECK(i == 2147483647);
    EMB_CHECK(embi_js_to_int32(rt, embi_js_from_double(1e20), &i));
    EMB_CHECK(i == 1661992960); /* 1e20 mod 2^32 */
    EMB_CHECK(embi_js_to_uint32(rt, embi_js_from_double(NAN), &u));
    EMB_CHECK(u == 0);
    EMB_CHECK(embi_js_to_uint32(rt, embi_js_from_double(-0.5), &u));
    EMB_CHECK(u == 0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(property_keys_canonicalize_numeric_strings)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_key k = embi_js_to_property_key(rt, text(rt, "5"));
    EMB_CHECK(embi_js_key_is_index(k) && embi_js_key_index(k) == 5);
    k = embi_js_to_property_key(rt, embi_js_from_double(5.0));
    EMB_CHECK(embi_js_key_is_index(k) && embi_js_key_index(k) == 5);
    k = embi_js_to_property_key(rt, text(rt, "-1"));
    EMB_CHECK(!embi_js_key_is_index(k) && embi_js_is_string(k));
    k = embi_js_to_property_key(rt, text(rt, "4294967295"));
    EMB_CHECK(!embi_js_key_is_index(k));
    embi_js_value round = embi_js_key_to_value(rt, embi_js_key_from_index(42));
    EMB_CHECK(text_is(round, "42"));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(to_primitive_calls_value_of_then_to_string)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_object *seven = embi_js_object_new_class(rt, EMBI_JS_NULL, &seven_class);
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_VALUE_OF), embi_js_from_object(seven),
                       EMBI_JS_ATTR_HIDDEN);
    double d = 0;
    EMB_CHECK(embi_js_to_number(rt, embi_js_from_object(o), &d));
    EMB_CHECK(d == 7.0);

    /* A valueOf that returns an object falls back to toString. */
    embi_js_object *o2 = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o2, embi_js_name_val(rt, EMBI_JS_NAME_VALUE_OF), embi_js_from_object(o),
                       EMBI_JS_ATTR_HIDDEN);
    embi_js_define_raw(rt, o2, embi_js_name_val(rt, EMBI_JS_NAME_TO_STRING),
                       embi_js_from_object(seven), EMBI_JS_ATTR_HIDDEN);
    embi_js_value prim = embi_js_to_primitive(rt, embi_js_from_object(o2),
                                              EMBI_JS_HINT_DEFAULT);
    EMB_CHECK(embi_js_is_int32(prim) && embi_js_as_int32(prim) == 7);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(equality_follows_same_value_and_loose_rules)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    EMB_CHECK(embi_js_same_value(embi_js_from_double(NAN), embi_js_from_double(NAN)));
    EMB_CHECK(!embi_js_same_value(embi_js_from_double(0.0), embi_js_from_double(-0.0)));
    EMB_CHECK(embi_js_same_value_zero(embi_js_from_double(0.0), embi_js_from_double(-0.0)));
    EMB_CHECK(!embi_js_strict_equal(embi_js_from_double(NAN), embi_js_from_double(NAN)));
    EMB_CHECK(embi_js_strict_equal(text(rt, "ab"), text(rt, "ab")));
    EMB_CHECK(!embi_js_strict_equal(embi_js_from_int32(1), text(rt, "1")));

    EMB_CHECK(embi_js_loose_equal(rt, EMBI_JS_NULL, EMBI_JS_UNDEFINED) == EMBI_JS_YES);
    EMB_CHECK(embi_js_loose_equal(rt, text(rt, "1"), embi_js_from_int32(1)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_loose_equal(rt, EMBI_JS_TRUE, embi_js_from_int32(1)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_loose_equal(rt, EMBI_JS_NULL, embi_js_from_int32(0)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_loose_equal(rt, text(rt, ""), embi_js_from_int32(0)) == EMBI_JS_YES);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(relational_comparison_handles_strings_numbers_and_nan)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    EMB_CHECK(embi_js_less_than(rt, text(rt, "a"), text(rt, "b"), true) == EMBI_JS_CMP_TRUE);
    EMB_CHECK(embi_js_less_than(rt, text(rt, "b"), text(rt, "a"), true) == EMBI_JS_CMP_FALSE);
    EMB_CHECK(embi_js_less_than(rt, embi_js_from_int32(1), text(rt, "2"), true) ==
              EMBI_JS_CMP_TRUE);
    EMB_CHECK(embi_js_less_than(rt, embi_js_from_double(NAN), embi_js_from_int32(1), true) ==
              EMBI_JS_CMP_UNDEFINED);
    EMB_CHECK(embi_js_less_than(rt, embi_js_from_int32(2), embi_js_from_int32(1), false) ==
              EMBI_JS_CMP_FALSE);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(typeof_and_to_boolean)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    EMB_CHECK(text_is(embi_js_typeof(rt, EMBI_JS_NULL), "object"));
    EMB_CHECK(text_is(embi_js_typeof(rt, EMBI_JS_UNDEFINED), "undefined"));
    EMB_CHECK(text_is(embi_js_typeof(rt, embi_js_from_int32(3)), "number"));
    EMB_CHECK(text_is(embi_js_typeof(rt, text(rt, "x")), "string"));
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &seven_class);
    EMB_CHECK(text_is(embi_js_typeof(rt, embi_js_from_object(f)), "function"));
    EMB_CHECK(!embi_js_to_boolean(embi_js_from_double(NAN)));
    EMB_CHECK(!embi_js_to_boolean(text(rt, "")));
    EMB_CHECK(embi_js_to_boolean(text(rt, "0")));
    EMB_CHECK(!embi_js_to_boolean(embi_js_from_int32(0)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(to_object_wraps_primitives_and_rejects_nullish)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *w = embi_js_to_object(rt, embi_js_from_int32(4));
    EMB_REQUIRE(w != nullptr);
    EMB_CHECK(embi_js_wrapper_value(w) == embi_js_from_int32(4));
    EMB_CHECK(embi_js_to_object(rt, EMBI_JS_NULL) == nullptr);
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(rt)));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(wrapper_value_is_undefined_for_non_wrappers)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *plain = embi_js_object_new_plain(rt);
    embi_js_object *arr = embi_js_array_create_default(rt, 0);
    EMB_REQUIRE(plain != nullptr && arr != nullptr);
    embi_js_object *bound = embi_js_bound_function_new(rt, plain, EMBI_JS_UNDEFINED, 0, nullptr);
    EMB_REQUIRE(bound != nullptr);
    EMB_CHECK(embi_js_wrapper_value(plain) == EMBI_JS_UNDEFINED);
    EMB_CHECK(embi_js_wrapper_value(arr) == EMBI_JS_UNDEFINED);
    EMB_CHECK(embi_js_wrapper_value(bound) == EMBI_JS_UNDEFINED);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
