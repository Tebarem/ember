#include "emb_test.h"

#include <math.h>

#include "js/object.h"
#include "js/ops.h"

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static embi_js_value ret_nine(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                              int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_from_int32(9);
}

static const embi_js_class nine_class = {.name = "Nine", .call = ret_nine};

EMB_TEST(to_length_clamps)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    double d = 1;
    EMB_CHECK(embi_js_to_length(rt, embi_js_from_double(-1.0), &d));
    EMB_CHECK(d == 0.0);
    EMB_CHECK(embi_js_to_length(rt, embi_js_from_double(NAN), &d));
    EMB_CHECK(d == 0.0);
    EMB_CHECK(embi_js_to_length(rt, embi_js_from_double(9007199254740993.0 * 2), &d));
    EMB_CHECK(d == 9007199254740991.0);
    EMB_CHECK(embi_js_to_length(rt, embi_js_from_double(3.7), &d));
    EMB_CHECK(d == 3.0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(to_integer_or_infinity_truncates)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    double d = 1;
    EMB_CHECK(embi_js_to_integer_or_infinity(rt, embi_js_from_double(-2.9), &d));
    EMB_CHECK(d == -2.0);
    EMB_CHECK(embi_js_to_integer_or_infinity(rt, embi_js_from_double(NAN), &d));
    EMB_CHECK(d == 0.0);
    EMB_CHECK(embi_js_to_integer_or_infinity(rt, embi_js_from_double(INFINITY), &d));
    EMB_CHECK(isinf(d) && d > 0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(get_method_and_invoke)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &nine_class);
    EMB_REQUIRE(o != nullptr && f != nullptr);
    EMB_CHECK(embi_js_create_data_property_or_throw(rt, o, key(rt, "nul"), EMBI_JS_NULL) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_create_data_property_or_throw(rt, o, key(rt, "bad"),
                                                    embi_js_from_int32(1)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_create_data_property_or_throw(rt, o, key(rt, "fn"),
                                                    embi_js_from_object(f)) == EMBI_JS_YES);
    embi_js_value ov = embi_js_from_object(o);
    EMB_CHECK(embi_js_get_method(rt, ov, key(rt, "nul")) == EMBI_JS_UNDEFINED);
    EMB_CHECK(embi_js_get_method(rt, ov, key(rt, "missing")) == EMBI_JS_UNDEFINED);
    EMB_CHECK(embi_js_get_method(rt, ov, key(rt, "bad")) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(rt);
    EMB_CHECK(embi_js_get_method(rt, ov, key(rt, "fn")) == embi_js_from_object(f));
    embi_js_value r = embi_js_invoke(rt, ov, key(rt, "fn"), 0, nullptr);
    EMB_CHECK(r == embi_js_from_int32(9));
    EMB_CHECK(embi_js_invoke(rt, ov, key(rt, "bad"), 0, nullptr) == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(proto_from_ctor_falls_back)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *ctor = embi_js_object_new_plain(rt);
    embi_js_object *p = embi_js_object_new_plain(rt);
    EMB_REQUIRE(ctor != nullptr && p != nullptr);
    embi_js_value cv = embi_js_from_object(ctor);
    embi_js_value fb = embi_js_proto_from_ctor(rt, cv, EMBI_JS_PROTO_OBJECT);
    embi_js_value expect = embi_js_is_empty(rt->protos[EMBI_JS_PROTO_OBJECT]) ? EMBI_JS_NULL
                                                                                : rt->protos[EMBI_JS_PROTO_OBJECT];
    EMB_CHECK(fb == expect);
    EMB_CHECK(embi_js_create_data_property_or_throw(rt, ctor, key(rt, "prototype"),
                                                    embi_js_from_int32(3)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_proto_from_ctor(rt, cv, EMBI_JS_PROTO_OBJECT) == fb);
    EMB_CHECK(embi_js_put(rt, ctor, key(rt, "prototype"), embi_js_from_object(p), true) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_proto_from_ctor(rt, cv, EMBI_JS_PROTO_OBJECT) == embi_js_from_object(p));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(delete_or_throw_and_length_of_array_like)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    EMB_REQUIRE(o != nullptr);
    EMB_CHECK(embi_js_create_data_property_or_throw(rt, o, key(rt, "x"), embi_js_from_int32(1)) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_delete_or_throw(rt, o, key(rt, "x")) == EMBI_JS_YES);
    embi_js_pdesc fixed = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                           EMBI_JS_PD_WRITABLE, 0};
    EMB_CHECK(embi_js_define_property_or_throw(rt, o, key(rt, "y"), &fixed) == EMBI_JS_YES);
    EMB_CHECK(embi_js_delete_or_throw(rt, o, key(rt, "y")) == EMBI_JS_THREW);
    embi_js_exception_clear(rt);
    double len = 5;
    EMB_CHECK(embi_js_length_of_array_like(rt, o, &len));
    EMB_CHECK(len == 0.0);
    EMB_CHECK(embi_js_put(rt, o, key(rt, "length"), embi_js_from_int32(4), true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_length_of_array_like(rt, o, &len));
    EMB_CHECK(len == 4.0);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(array_set_fast_path_edge_cases)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    /* non-writable length: index at or past length is rejected */
    embi_js_object *a = embi_js_array_create_default(rt, 2);
    EMB_REQUIRE(a != nullptr);
    embi_js_value av = embi_js_from_object(a);
    embi_js_pdesc ro = {EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED,
                        EMBI_JS_PD_WRITABLE, 0};
    EMB_CHECK(embi_js_obj_define_own_property(rt, a, key(rt, "length"), &ro) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(2), embi_js_from_int32(1), av) ==
              EMBI_JS_NO);
    /* a hole below length is still writable while the length is fixed */
    EMB_CHECK(embi_js_obj_set(rt, a, embi_js_key_from_index(1), embi_js_from_int32(7), av) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_get(rt, a, embi_js_key_from_index(1)) == embi_js_from_int32(7));

    /* non-extensible: existing elements update, a hole cannot be filled */
    embi_js_object *b = embi_js_array_create_default(rt, 3);
    EMB_REQUIRE(b != nullptr);
    embi_js_value bv = embi_js_from_object(b);
    EMB_CHECK(embi_js_obj_set(rt, b, embi_js_key_from_index(0), embi_js_from_int32(1), bv) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_prevent_extensions(rt, b) == EMBI_JS_YES);
    EMB_CHECK(embi_js_obj_set(rt, b, embi_js_key_from_index(0), embi_js_from_int32(2), bv) ==
              EMBI_JS_YES);
    EMB_CHECK(embi_js_get(rt, b, embi_js_key_from_index(0)) == embi_js_from_int32(2));
    EMB_CHECK(embi_js_obj_set(rt, b, embi_js_key_from_index(2), embi_js_from_int32(5), bv) ==
              EMBI_JS_NO);
    EMB_CHECK(embi_js_obj_set(rt, b, embi_js_key_from_index(3), embi_js_from_int32(5), bv) ==
              EMBI_JS_NO);
    EMB_CHECK(embi_js_get(rt, b, embi_js_key_from_index(2)) == EMBI_JS_UNDEFINED);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
