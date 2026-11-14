#include "emb_test.h"

#include "js/js.h"

#include <math.h>
#include <stdint.h>

static double bits_to_double(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

EMB_TEST(value_specials_are_distinct_and_not_cells_or_numbers)
{
    embi_js_value specials[] = {EMBI_JS_EMPTY,   EMBI_JS_NULL,      EMBI_JS_FALSE,
                                EMBI_JS_TRUE,    EMBI_JS_UNDEFINED, EMBI_JS_EXCEPTION};
    for (size_t i = 0; i < sizeof specials / sizeof specials[0]; i++) {
        EMB_CHECK(!embi_js_is_cell(specials[i]));
        EMB_CHECK(!embi_js_is_number(specials[i]));
        for (size_t j = 0; j < i; j++) {
            EMB_CHECK(specials[i] != specials[j]);
        }
    }
    EMB_CHECK(embi_js_is_nullish(EMBI_JS_NULL));
    EMB_CHECK(embi_js_is_nullish(EMBI_JS_UNDEFINED));
    EMB_CHECK(!embi_js_is_nullish(EMBI_JS_FALSE));
    EMB_CHECK(embi_js_is_bool(EMBI_JS_TRUE));
    EMB_CHECK(embi_js_as_bool(EMBI_JS_TRUE));
    EMB_CHECK(!embi_js_as_bool(EMBI_JS_FALSE));
    EMB_CHECK(embi_js_from_bool(true) == EMBI_JS_TRUE);
    EMB_CHECK(embi_js_from_bool(false) == EMBI_JS_FALSE);
    EMB_CHECK(embi_js_is_empty(EMBI_JS_EMPTY));
    EMB_CHECK(embi_js_is_exception(EMBI_JS_EXCEPTION));
}

EMB_TEST(value_int32_round_trip_including_extremes)
{
    int32_t cases[] = {0, 1, -1, 42, -42, INT32_MAX, INT32_MIN, 65536, -65536};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_js_value v = embi_js_from_int32(cases[i]);
        EMB_CHECK(embi_js_is_int32(v));
        EMB_CHECK(embi_js_is_number(v));
        EMB_CHECK(!embi_js_is_double(v));
        EMB_CHECK(!embi_js_is_cell(v));
        EMB_CHECK_EQ_INT(embi_js_as_int32(v), cases[i]);
        EMB_CHECK(embi_js_as_double(v) == (double)cases[i]);
    }
}

EMB_TEST(value_double_round_trip_and_canonical_nan)
{
    double cases[] = {0.5, -0.5, 3.141592653589793, 1e300, -1e-300, 5e-324,
                      1.7976931348623157e308, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_js_value v = embi_js_from_double(cases[i]);
        EMB_CHECK(embi_js_is_double(v));
        EMB_CHECK(embi_js_is_number(v));
        EMB_CHECK(!embi_js_is_int32(v));
        EMB_CHECK(embi_js_as_double(v) == cases[i]);
    }
    /* Every NaN payload encodes to the same bits and decodes as a NaN. */
    embi_js_value n1 = embi_js_from_double(bits_to_double(0x7FF0000000000001ull));
    embi_js_value n2 = embi_js_from_double(bits_to_double(0xFFF8000000000123ull));
    EMB_CHECK(n1 == n2);
    EMB_CHECK(embi_js_as_double(n1) != embi_js_as_double(n1));
    /* Negative zero keeps its sign. */
    double nz = embi_js_as_double(embi_js_from_double(-0.0));
    EMB_CHECK(nz == 0.0 && 1.0 / nz < 0);
}

EMB_TEST(value_number_canonicalizes_integers_but_not_negative_zero)
{
    EMB_CHECK(embi_js_is_int32(embi_js_number(7.0)));
    EMB_CHECK_EQ_INT(embi_js_as_int32(embi_js_number(-9.0)), -9);
    EMB_CHECK(embi_js_is_int32(embi_js_number(0.0)));
    EMB_CHECK(embi_js_is_double(embi_js_number(-0.0)));
    EMB_CHECK(embi_js_is_double(embi_js_number(0.5)));
    EMB_CHECK(embi_js_is_double(embi_js_number(2147483648.0)));
    EMB_CHECK(embi_js_is_int32(embi_js_number(-2147483648.0)));
    EMB_CHECK(embi_js_is_double(embi_js_number(NAN)));
}

EMB_TEST(value_cell_pointer_round_trip_and_predicates)
{
    /* A fake cell in ordinary memory: the predicates only look at the address. */
    static embi_js_box fake_box;
    embi_js_value v = embi_js_from_cell(&fake_box);
    EMB_CHECK(embi_js_is_cell(v));
    EMB_CHECK(!embi_js_is_number(v));
    EMB_CHECK(!embi_js_is_nullish(v));
    EMB_CHECK(embi_js_as_cell(v) == &fake_box.hdr);
    EMB_CHECK(embi_js_ptr_decode(v) == (embi_js_cell *)&fake_box);
}

EMB_TEST(value_doubles_never_look_like_cells)
{
    /* +0.0 encodes to 2^49, which is above the pointer range. */
    EMB_CHECK(!embi_js_is_cell(embi_js_from_double(0.0)));
    EMB_CHECK(!embi_js_is_cell(embi_js_from_int32(0)));
    EMB_CHECK(embi_js_is_number(embi_js_from_double(0.0)));
}

EMB_TEST_MAIN()
