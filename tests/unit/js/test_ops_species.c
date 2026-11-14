#include "emb_test.h"

#include "js/object.h"
#include "js/ops.h"

/* SpeciesConstructor, ArraySpeciesCreate, EnumerableOwnProperties and integrity levels. */

static embi_js_key key(embi_js_runtime *rt, const char *s)
{
    return embi_js_key_c(rt, s);
}

static embi_js_value from(embi_js_object *o)
{
    return embi_js_from_object(o);
}

static const embi_js_class test_result_class = {.name = "TestResult"};

static embi_js_value species_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                       const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    (void)argc;
    (void)argv;
    (void)new_target;
    return from(embi_js_object_new_class(rt, EMBI_JS_NULL, &test_result_class));
}

static const embi_js_class test_species_class = {
    .name = "TestSpecies",
    .construct = species_construct,
};

static embi_js_object *make_constructor(embi_js_runtime *rt, const embi_js_class *klass)
{
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, klass);
    f->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    return f;
}

EMB_TEST(species_constructor_defaults_and_reads_the_species_property)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *dflt = make_constructor(rt, &test_species_class);
    embi_js_object *o = embi_js_object_new_plain(rt);

    /* No constructor property: the default. */
    EMB_CHECK(embi_js_species_constructor(rt, o, from(dflt)) == from(dflt));

    /* constructor is an object whose @@species is a constructor: that one is used. */
    embi_js_object *c = embi_js_object_new_plain(rt);
    embi_js_object *s = make_constructor(rt, &test_species_class);
    embi_js_define_raw(rt, c, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES), from(s),
                       EMBI_JS_ATTR_HIDDEN);
    embi_js_define_raw(rt, o, key(rt, "constructor"), from(c), EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(embi_js_species_constructor(rt, o, from(dflt)) == from(s));

    /* @@species undefined: the default again. */
    embi_js_define_raw(rt, c, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES), EMBI_JS_UNDEFINED,
                       EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(embi_js_species_constructor(rt, o, from(dflt)) == from(dflt));

    /* constructor is a number: TypeError. */
    embi_js_define_raw(rt, o, key(rt, "constructor"), embi_js_from_int32(3), EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(embi_js_species_constructor(rt, o, from(dflt)) == EMBI_JS_EXCEPTION);
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(rt)));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(array_species_create_uses_species_and_falls_back_to_a_plain_array)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);

    /* A plain array with the default constructor makes an ordinary array. */
    embi_js_object *a = embi_js_array_create_default(rt, 0);
    embi_js_object *r1 = embi_js_array_species_create(rt, a, 4.0);
    EMB_REQUIRE(r1 != nullptr);
    EMB_CHECK(embi_js_is_array(from(r1)));
    EMB_CHECK(embi_js_as_double(embi_js_get(rt, r1, key(rt, "length"))) == 4.0);

    /* A constructor whose @@species is a constructor: that constructor builds the result. */
    embi_js_object *ctor = make_constructor(rt, &test_species_class);
    embi_js_define_raw(rt, ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES), from(ctor),
                       EMBI_JS_ATTR_HIDDEN);
    embi_js_define_raw(rt, a, key(rt, "constructor"), from(ctor), EMBI_JS_ATTR_HIDDEN);
    embi_js_object *r2 = embi_js_array_species_create(rt, a, 2.0);
    EMB_REQUIRE(r2 != nullptr);
    EMB_CHECK(r2->klass == &test_result_class);

    /* Not an array: the species lookup is skipped and a plain array comes back. */
    embi_js_object *plain = embi_js_object_new_plain(rt);
    embi_js_object *r3 = embi_js_array_species_create(rt, plain, 1.0);
    EMB_REQUIRE(r3 != nullptr);
    EMB_CHECK(embi_js_is_array(from(r3)));

    /* A non-constructor species throws TypeError. */
    embi_js_define_raw(rt, ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES), embi_js_from_int32(1),
                       EMBI_JS_ATTR_HIDDEN);
    EMB_CHECK(embi_js_array_species_create(rt, a, 2.0) == nullptr);
    EMB_CHECK(embi_js_is_object(embi_js_exception_get(rt)));
    embi_js_exception_clear(rt);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(enumerable_own_keys_skip_hidden_properties_and_symbols)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, key(rt, "a"), embi_js_from_int32(1), EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, key(rt, "hidden"), embi_js_from_int32(2), EMBI_JS_ATTR_HIDDEN);
    embi_js_symbol *sym = embi_js_symbol_new(rt, EMBI_JS_UNDEFINED);
    embi_js_define_raw(rt, o, embi_js_from_symbol(sym), embi_js_from_int32(3),
                       EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_key_from_index(3), embi_js_from_int32(4),
                       EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_key_from_index(1), embi_js_from_int32(5),
                       EMBI_JS_ATTR_HIDDEN);

    embi_js_key_vec keys = {nullptr, 0, 0};
    EMB_CHECK(embi_js_enumerable_own_keys(rt, o, &keys) == EMBI_JS_YES);
    EMB_REQUIRE(keys.len == 2);
    EMB_CHECK(keys.data[0] == embi_js_key_from_index(3));
    EMB_CHECK(keys.data[1] == key(rt, "a"));
    embi_vec_free(&keys);
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(integrity_levels_seal_and_freeze_plain_objects)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_enter(rt);
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, key(rt, "x"), embi_js_from_int32(1), EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, key(rt, "y"), embi_js_from_int32(2), EMBI_JS_ATTR_DEFAULT);

    EMB_CHECK(embi_js_test_integrity(rt, o, false) == EMBI_JS_NO);
    EMB_CHECK(embi_js_set_integrity(rt, o, false) == EMBI_JS_YES);
    EMB_CHECK(!embi_js_obj_is_extensible(o));
    EMB_CHECK(embi_js_test_integrity(rt, o, false) == EMBI_JS_YES);
    EMB_CHECK(embi_js_test_integrity(rt, o, true) == EMBI_JS_NO);

    EMB_CHECK(embi_js_set_integrity(rt, o, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_test_integrity(rt, o, true) == EMBI_JS_YES);
    EMB_CHECK(embi_js_put(rt, o, key(rt, "x"), embi_js_from_int32(9), false) == EMBI_JS_NO);
    EMB_CHECK(embi_js_get(rt, o, key(rt, "x")) == embi_js_from_int32(1));
    embi_js_leave(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
