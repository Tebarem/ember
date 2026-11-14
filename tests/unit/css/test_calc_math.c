/*
 * calc() and the math functions (W2c): parsing and type checking, constant reduction, evaluation
 * with percentage bases and relative lengths, rounding strategies, trigonometry and the invalid
 * forms the spec rejects.
 */
#include "emb_test.h"

#include "css/props.h"

#include <string.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

/* Parses a math function text and returns the root node and type. */
static bool calc_of(embi_arena *a, const char *s, const embi_calc **node, uint8_t *type)
{
    embi_css_slice in = text_of(a, s);
    for (uint32_t i = 0; i < in.n; i++) {
        if (in.p[i].tok.type == EMBI_CSS_WS) {
            continue;
        }
        return embi_calc_parse_cv(a, &in.p[i], node, type);
    }
    return false;
}

static double value_of(const char *s)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    double v = __builtin_nan("");
    if (calc_of(&a, s, &node, &type)) {
        embi_calc_env env = {0, nullptr, nullptr};
        v = embi_calc_eval(node, &env);
    }
    embi_arena_free(&a);
    return v;
}

static double em_to_px(void *user, uint8_t unit, double value)
{
    (void)user;
    if (unit == EMBI_U_EM) {
        return value * 16.0;
    }
    return __builtin_nan("");
}

EMB_TEST(calc_constant_arithmetic_and_precedence)
{
    EMB_CHECK(value_of("calc(1 + 2 * 3)") == 7.0);
    EMB_CHECK(value_of("calc((1 + 2) * 3)") == 9.0);
    EMB_CHECK(value_of("calc(10 / 4)") == 2.5);
    EMB_CHECK(value_of("calc(1px + 2px)") == 3.0);
    EMB_CHECK(value_of("calc(2in - 96px)") == 96.0);
    EMB_CHECK(value_of("calc(1turn - 90deg)") == 270.0);
    EMB_CHECK(value_of("calc(500ms * 2)") == 1.0);
    EMB_CHECK(value_of("calc(pi * 0)") == 0.0);
    EMB_CHECK(value_of("calc(e - e)") == 0.0);
    EMB_CHECK(value_of("calc(1 - -2)") == 3.0);
}

EMB_TEST(calc_type_rules)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;

    EMB_REQUIRE(calc_of(&a, "calc(1px + 5%)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_LENPCT);

    EMB_REQUIRE(calc_of(&a, "calc(2 * 3px)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_LEN);

    EMB_REQUIRE(calc_of(&a, "calc(10px / 2)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_LEN);

    EMB_REQUIRE(calc_of(&a, "min(1px, 2%, 3em)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_LENPCT);

    EMB_REQUIRE(calc_of(&a, "sin(45deg)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_NUM);

    EMB_REQUIRE(calc_of(&a, "asin(1)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_ANGLE);

    EMB_REQUIRE(calc_of(&a, "atan2(1px, 1px)", &node, &type));
    EMB_CHECK_EQ_INT(type, EMBI_CD_ANGLE);

    /* Invalid: mixed dimensions, length products, number divided by length, bad tokens. */
    EMB_CHECK(!calc_of(&a, "calc(1px + 1)", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc(1px * 2px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc(2 / 1px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc(1px -2px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc(1px +)", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc()", &node, &type));
    EMB_CHECK(!calc_of(&a, "calc(1px + 2px deg)", &node, &type));
    EMB_CHECK(!calc_of(&a, "sqrt(1px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "sin(1px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "clamp(1px, 2px)", &node, &type));
    EMB_CHECK(!calc_of(&a, "round(up, 1px, 2deg)", &node, &type));
    embi_arena_free(&a);
}

EMB_TEST(calc_min_max_clamp)
{
    EMB_CHECK(value_of("min(3px, 1px, 2px)") == 1.0);
    EMB_CHECK(value_of("max(3px, 1px, 2px)") == 3.0);
    EMB_CHECK(value_of("clamp(10px, 5px, 20px)") == 10.0);
    EMB_CHECK(value_of("clamp(1px, 5px, 3px)") == 3.0);
    EMB_CHECK(value_of("clamp(1px, 2px, 3px)") == 2.0);
    /* clamp(MIN, VAL, MAX) is max(MIN, min(VAL, MAX)): a MIN above MAX wins. */
    EMB_CHECK(value_of("clamp(5px, 1px, 3px)") == 5.0);
}

EMB_TEST(calc_percent_and_relative_lengths)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;

    EMB_REQUIRE(calc_of(&a, "calc(50% + 10px)", &node, &type));
    EMB_CHECK(!embi_calc_is_constant(node));
    embi_calc_env env = {200.0, nullptr, nullptr};
    EMB_CHECK(embi_calc_eval(node, &env) == 110.0);
    /* Without a length resolver, relative lengths cannot be evaluated. */
    EMB_REQUIRE(calc_of(&a, "calc(1em + 2px)", &node, &type));
    EMB_CHECK(!embi_calc_is_constant(node));
    EMB_CHECK(embi_calc_eval(node, &env) != embi_calc_eval(node, &env));
    embi_calc_env em_env = {0, em_to_px, nullptr};
    EMB_CHECK(embi_calc_eval(node, &em_env) == 18.0);

    EMB_REQUIRE(calc_of(&a, "min(10px, 5%)", &node, &type));
    EMB_CHECK(embi_calc_eval(node, &env) == 10.0);
    env.pct_base = 100.0;
    EMB_CHECK(embi_calc_eval(node, &env) == 5.0);

    /* A constant reduces to a single value. */
    double reduced = 0;
    EMB_REQUIRE(calc_of(&a, "calc(2px * 3)", &node, &type));
    EMB_CHECK(embi_calc_reduce(&a, node, &reduced));
    EMB_CHECK(reduced == 6.0);
    /* A percentage is not constant, so it does not reduce. */
    EMB_REQUIRE(calc_of(&a, "calc(1% + 1px)", &node, &type));
    EMB_CHECK(!embi_calc_reduce(&a, node, &reduced));
    embi_arena_free(&a);
}

EMB_TEST(calc_rounding_modulo_and_remainder)
{
    EMB_CHECK(value_of("round(nearest, 7px, 5px)") == 5.0);
    EMB_CHECK(value_of("round(up, 7px, 5px)") == 10.0);
    EMB_CHECK(value_of("round(down, 7px, 5px)") == 5.0);
    EMB_CHECK(value_of("round(to-zero, -7px, 5px)") == -5.0);
    EMB_CHECK(value_of("round(7px, 5px)") == 5.0);
    EMB_CHECK(value_of("round(2.5, 1)") == 3.0);
    EMB_CHECK(value_of("round(up, 5px)") == 5.0);
    EMB_CHECK(value_of("mod(-7px, 5px)") == 3.0);
    EMB_CHECK(value_of("mod(7px, -5px)") == -3.0);
    EMB_CHECK(value_of("rem(-7px, 5px)") == -2.0);
    EMB_CHECK(value_of("rem(7px, 5px)") == 2.0);
    EMB_CHECK(value_of("mod(1px, 0px)") != value_of("mod(1px, 0px)")); /* NaN for zero */
    EMB_CHECK(value_of("abs(-3px)") == 3.0);
    EMB_CHECK(value_of("sign(-2)") == -1.0);
    EMB_CHECK(value_of("sign(0)") == 0.0);
}

EMB_TEST(calc_trig_exponential_and_hypot)
{
    EMB_CHECK(__builtin_fabs(value_of("sin(90deg)") - 1.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("cos(0)") - 1.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("tan(45deg)") - 1.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("asin(1)") - 90.0) < 1e-9);
    EMB_CHECK(__builtin_fabs(value_of("acos(0)") - 90.0) < 1e-9);
    EMB_CHECK(__builtin_fabs(value_of("atan(1)") - 45.0) < 1e-9);
    EMB_CHECK(__builtin_fabs(value_of("atan2(1, 1)") - 45.0) < 1e-9);
    EMB_CHECK(value_of("pow(2, 10)") == 1024.0);
    EMB_CHECK(value_of("sqrt(16)") == 4.0);
    EMB_CHECK(value_of("hypot(3px, 4px)") == 5.0);
    EMB_CHECK(__builtin_fabs(value_of("log(e)") - 1.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("log(8, 2)") - 3.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("exp(0)") - 1.0) < 1e-12);
    EMB_CHECK(__builtin_fabs(value_of("calc(pi)") - 3.141592653589793) < 1e-12);
    EMB_CHECK(value_of("calc(infinity)") > 1e300);
    EMB_CHECK(value_of("calc(-infinity)") < -1e300);
    EMB_CHECK(value_of("calc(nan)") != value_of("calc(nan)"));
}

EMB_TEST(calc_reduces_absolute_units)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    EMB_REQUIRE(calc_of(&a, "calc(1cm)", &node, &type));
    double px = 0;
    EMB_CHECK(embi_calc_reduce(&a, node, &px));
    EMB_CHECK(__builtin_fabs(px - 96.0 / 2.54) < 1e-12);
    embi_arena_free(&a);
}

EMB_TEST(calc_mixes_an_angle_with_a_percentage)
{
    /* W4g5: the stops of a conic gradient are angle-percentages (CSS Values 4 section 10.2) */
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    EMB_REQUIRE(calc_of(&a, "calc(10deg + 25%)", &node, &type));
    EMB_CHECK((type & EMBI_CD_ANGLE) != 0);
    EMB_CHECK((type & EMBI_CD_PCT) != 0);
    embi_calc_env env = {360.0, nullptr, nullptr};
    EMB_CHECK(embi_calc_eval(node, &env) == 100.0); /* 25% of a full turn (360) is 90 */
    embi_arena_free(&a);
    /* a length and an angle still do not mix */
    embi_arena b = {0};
    embi_arena_init(&b, 1024);
    EMB_CHECK(!calc_of(&b, "calc(10deg + 5px)", &node, &type));
    embi_arena_free(&b);
}

/* A math function with no argument tokens (M9a fuzz finding: "clamp(" and "clamp(var(--" reached pointer arithmetic on a
 * null argument array) is invalid, not a crash; UBSan builds check the arithmetic. */
EMB_TEST(calc_function_without_arguments_is_invalid)
{
    EMB_CHECK(__builtin_isnan(value_of("clamp(")));
    EMB_CHECK(__builtin_isnan(value_of("min()")));
    EMB_CHECK(__builtin_isnan(value_of("clamp(var(--")));
}

EMB_TEST_MAIN()
