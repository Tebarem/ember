#include "emb_test.h"

#include <string.h>

#include "js/object.h"
#include "js/ops.h"

static embi_js_value fn_add(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v, int argc,
                            const embi_js_value *argv)
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

static embi_js_value fn_fail(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v, int argc,
                             const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "boom %d", 7);
}

static const embi_js_class add_class = {.name = "Add", .call = fn_add};
static const embi_js_class fail_class = {.name = "Fail", .call = fn_fail};

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static bool message_is(embi_js_runtime *rt, const char *expected)
{
    embi_js_value ex = embi_js_exception_get(rt);
    if (!embi_js_is_object(ex)) {
        return false;
    }
    embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE));
    if (!embi_js_is_string(m)) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_as_string(m), &sb);
    bool same = sb.len == strlen(expected) && memcmp(sb.data, expected, sb.len) == 0;
    embi_sb_free(&sb);
    return same;
}

EMB_TEST(calling_a_non_function_throws_type_error)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_value r = embi_js_call(rt, embi_js_from_int32(1), EMBI_JS_UNDEFINED, 0, nullptr);
    EMB_CHECK(embi_js_is_exception(r));
    EMB_CHECK(message_is(rt, "not a function"));
    embi_js_exception_clear(rt);

    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &fail_class);
    r = embi_js_call(rt, embi_js_from_object(f), EMBI_JS_UNDEFINED, 0, nullptr);
    EMB_CHECK(embi_js_is_exception(r));
    EMB_CHECK(message_is(rt, "boom 7"));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(create_list_from_array_like_reads_length_and_indices)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    for (int i = 0; i < 3; i++) {
        embi_js_put(rt, a, embi_js_key_from_index((uint32_t)i), embi_js_from_int32(i + 1), true);
    }
    uint32_t n = 0;
    embi_js_varray *list = embi_js_list_from_array_like(rt, embi_js_from_object(a), &n);
    EMB_REQUIRE(list != nullptr);
    EMB_CHECK(n == 3);
    EMB_CHECK_EQ_INT(embi_js_as_int32(list->elems[2]), 3);

    /* Apply: the list feeds a call. */
    embi_js_object *add = embi_js_object_new_class(rt, EMBI_JS_NULL, &add_class);
    embi_js_value r = embi_js_call(rt, embi_js_from_object(add), EMBI_JS_UNDEFINED, (int)n,
                                   list->elems);
    EMB_CHECK_EQ_INT(embi_js_as_int32(r), 6);

    EMB_CHECK(embi_js_list_from_array_like(rt, EMBI_JS_NULL, &n) == nullptr);
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(descriptor_round_trip_and_validation)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_put(rt, o, key(rt, "v"), embi_js_from_int32(5), true);
    embi_js_pdesc cur;
    EMB_CHECK(embi_js_obj_get_own_property(rt, o, key(rt, "v"), &cur) == EMBI_JS_YES);
    EMB_CHECK(cur.has & EMBI_JS_PD_WRITABLE);
    EMB_CHECK(cur.attrs == EMBI_JS_ATTR_DEFAULT);

    embi_js_value desc_obj = embi_js_from_property_descriptor(rt, &cur);
    EMB_REQUIRE(embi_js_is_object(desc_obj));
    embi_js_pdesc back;
    EMB_CHECK(embi_js_to_property_descriptor(rt, desc_obj, &back));
    EMB_CHECK(back.value == embi_js_from_int32(5));
    EMB_CHECK(back.attrs == EMBI_JS_ATTR_DEFAULT);

    /* A descriptor that mixes accessor and data fields is invalid. */
    embi_js_object *bad = embi_js_object_new_plain(rt);
    embi_js_put(rt, bad, embi_js_name_val(rt, EMBI_JS_NAME_VALUE), embi_js_from_int32(1), true);
    embi_js_put(rt, bad, embi_js_name_val(rt, EMBI_JS_NAME_GET), EMBI_JS_UNDEFINED, true);
    EMB_CHECK(!embi_js_to_property_descriptor(rt, embi_js_from_object(bad), &back));
    embi_js_exception_clear(rt);
    EMB_CHECK(!embi_js_to_property_descriptor(rt, embi_js_from_int32(3), &back));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(integrity_levels_freeze_and_seal)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_put(rt, o, key(rt, "a"), embi_js_from_int32(1), true);
    embi_js_put(rt, o, embi_js_key_from_index(0), embi_js_from_int32(2), true);
    EMB_CHECK(embi_js_test_integrity(rt, o, false) == EMBI_JS_NO);
    EMB_CHECK(embi_js_set_integrity(rt, o, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_test_integrity(rt, o, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_put(rt, o, key(rt, "a"), embi_js_from_int32(9), false) == EMBI_JS_NO);
    EMB_CHECK(embi_js_put(rt, o, key(rt, "a"), embi_js_from_int32(9), true) == EMBI_JS_THREW);
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(instanceof_walks_the_prototype_chain)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *ctor = embi_js_object_new_class(rt, EMBI_JS_NULL, &add_class);
    embi_js_object *proto = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE), embi_js_from_object(proto), 0);
    embi_js_object *mid = embi_js_object_new(rt, embi_js_from_object(proto));
    embi_js_object *inst = embi_js_object_new(rt, embi_js_from_object(mid));
    EMB_CHECK(embi_js_instance_of(rt, embi_js_from_object(inst), embi_js_from_object(ctor)) ==
              EMBI_JS_YES);
    embi_js_object *other = embi_js_object_new_plain(rt);
    EMB_CHECK(embi_js_instance_of(rt, embi_js_from_object(other), embi_js_from_object(ctor)) ==
              EMBI_JS_NO);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(get_v_reads_properties_of_primitives_through_their_wrapper)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_value len = embi_js_get_v(rt, embi_js_from_string(embi_js_str_new_utf8(rt, "four", 4)),
                                      embi_js_name_val(rt, EMBI_JS_NAME_LENGTH));
    EMB_CHECK(embi_js_is_int32(len) && embi_js_as_int32(len) == 4);
    embi_js_value undef = embi_js_get_v(rt, EMBI_JS_UNDEFINED, key(rt, "x"));
    EMB_CHECK(embi_js_is_exception(undef));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
