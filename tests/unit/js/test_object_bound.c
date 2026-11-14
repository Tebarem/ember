#include "emb_test.h"

#include "js/object.h"
#include "js/ops.h"

/* Bound function objects: [[Call]], [[Construct]] with new.target, and instanceof through a
 * bound target. */

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static embi_js_value from(embi_js_object *o)
{
    return embi_js_from_object(o);
}

/* Constructor whose [[Construct]] returns new.target itself, so a test can see what the bound
 * function forwarded. */
static embi_js_value ctor_return_new_target(embi_js_runtime *rt, embi_js_object *f, int argc,
                                            const embi_js_value *argv, embi_js_value new_target)
{
    (void)rt;
    (void)f;
    (void)argc;
    (void)argv;
    return new_target;
}

static const embi_js_class test_ctor_class = {
    .name = "TestCtor",
    .construct = ctor_return_new_target,
};

/* Constructor that counts its arguments into an object, and calls return the argument count. */
static embi_js_value ctor_count_args(embi_js_runtime *rt, embi_js_object *f, int argc,
                                     const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    (void)argv;
    (void)new_target;
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, key(rt, "argc"), embi_js_from_int32(argc), EMBI_JS_ATTR_DEFAULT);
    return from(o);
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

static const embi_js_class test_count_class = {
    .name = "TestCount",
    .call = fn_count_args,
    .construct = ctor_count_args,
};

/* Callable and constructible: needed where OrdinaryHasInstance looks at the target. */
static const embi_js_class test_both_class = {
    .name = "TestBoth",
    .call = fn_count_args,
    .construct = ctor_return_new_target,
};

static embi_js_object *make_constructor(embi_js_runtime *rt, const embi_js_class *klass)
{
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, klass);
    f->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    return f;
}

EMB_TEST(bound_function_is_a_constructor_only_when_its_target_is)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *ctor = make_constructor(rt, &test_ctor_class);
    embi_js_object *plain = embi_js_object_new_class(rt, EMBI_JS_NULL, &test_count_class);
    embi_js_object *bc = embi_js_bound_function_new(rt, ctor, EMBI_JS_UNDEFINED, 0, nullptr);
    embi_js_object *bp = embi_js_bound_function_new(rt, plain, EMBI_JS_UNDEFINED, 0, nullptr);
    EMB_CHECK(embi_js_is_constructor(from(bc)));
    EMB_CHECK(!embi_js_is_constructor(from(bp)));
    EMB_CHECK(embi_js_is_callable(from(bp)));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bound_construct_uses_target_constructor)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *f = make_constructor(rt, &test_ctor_class);
    embi_js_object *b = embi_js_bound_function_new(rt, f, EMBI_JS_UNDEFINED, 0, nullptr);
    /* new b() with new.target == b: the target sees new.target == target. */
    embi_js_value r = embi_js_construct(rt, from(b), 0, nullptr, from(b));
    EMB_CHECK(r == from(f));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bound_construct_forwards_other_new_targets)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *f = make_constructor(rt, &test_ctor_class);
    embi_js_object *b = embi_js_bound_function_new(rt, f, EMBI_JS_UNDEFINED, 0, nullptr);
    embi_js_object *other = make_constructor(rt, &test_ctor_class);
    /* Reflect.construct(b, [], other): new.target is other and is passed through. */
    embi_js_value r = embi_js_construct(rt, from(b), 0, nullptr, from(other));
    EMB_CHECK(r == from(other));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bound_arguments_prepend_on_construct_and_call)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *f = make_constructor(rt, &test_count_class);
    embi_js_value bound_args[2] = {embi_js_from_int32(1), embi_js_from_int32(2)};
    embi_js_object *b = embi_js_bound_function_new(rt, f, EMBI_JS_UNDEFINED, 2, bound_args);

    embi_js_value call_args[3] = {embi_js_from_int32(3), embi_js_from_int32(4),
                                  embi_js_from_int32(5)};
    embi_js_value r = embi_js_call(rt, from(b), EMBI_JS_UNDEFINED, 3, call_args);
    EMB_CHECK(embi_js_is_int32(r) && embi_js_as_int32(r) == 5);

    /* Constructing returns the object made by the target, which saw 2 + 3 arguments. */
    embi_js_value obj = embi_js_construct(rt, from(b), 3, call_args, from(b));
    EMB_CHECK(embi_js_is_object(obj));
    embi_js_value argc = embi_js_get(rt, embi_js_as_object(obj), key(rt, "argc"));
    EMB_CHECK(embi_js_is_int32(argc) && embi_js_as_int32(argc) == 5);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

/* A bound function with many arguments uses the heap path for argument buffers. */
EMB_TEST(bound_call_with_many_arguments_uses_heap_buffer)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *f = make_constructor(rt, &test_count_class);
    embi_js_value bound_args[12];
    for (int i = 0; i < 12; i++) {
        bound_args[i] = embi_js_from_int32(i);
    }
    embi_js_object *b = embi_js_bound_function_new(rt, f, EMBI_JS_UNDEFINED, 12, bound_args);
    embi_js_value call_args[5] = {0};
    for (int i = 0; i < 5; i++) {
        call_args[i] = embi_js_from_int32(i);
    }
    embi_js_value r = embi_js_call(rt, from(b), EMBI_JS_UNDEFINED, 5, call_args);
    EMB_CHECK(embi_js_is_int32(r) && embi_js_as_int32(r) == 17);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(instanceof_through_a_bound_function_uses_the_target)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *f = make_constructor(rt, &test_both_class);
    embi_js_object *proto = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, f, key(rt, "prototype"), from(proto), EMBI_JS_ATTR_WRITABLE);
    embi_js_object *b = embi_js_bound_function_new(rt, f, EMBI_JS_UNDEFINED, 0, nullptr);

    embi_js_object *inst = embi_js_object_new(rt, from(proto));
    EMB_CHECK(embi_js_ordinary_has_instance(rt, from(b), from(inst)) == EMBI_JS_YES);
    EMB_CHECK(embi_js_instance_of(rt, from(inst), from(b)) == EMBI_JS_YES);
    embi_js_object *other = embi_js_object_new_plain(rt);
    EMB_CHECK(embi_js_ordinary_has_instance(rt, from(b), from(other)) == EMBI_JS_NO);
    EMB_CHECK(embi_js_instance_of(rt, from(other), from(b)) == EMBI_JS_NO);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
