/*
 * calc-size() (CSS Values 5 section 10.1, W4g6): the `size` keyword in calc sums, the parse of
 * calc-size(<basis>, <calc-sum>) for the sizing properties, the rejected forms, the serialization
 * and the evaluation of the expression with a given basis size.
 */
#include "emb_test.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

static embi_prop_id prop_of(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

/* Parses `prop: value` and returns the number of longhands (0 when the value is invalid). */
static uint32_t expand(embi_arena *a, const char *prop, const char *value, embi_longhand *out)
{
    return embi_css_parse_decl(a, prop_of(prop), text_of(a, value), out, EMBI_LONGHAND_MAX);
}

static bool valid_width(const char *value)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    bool ok = expand(&a, "width", value, lh) == 1;
    embi_arena_free(&a);
    return ok;
}

/* Serializes the computed value of a property into sb. Returns false when the value is invalid. */
static bool serialize_prop(const char *prop, const char *value, embi_sb *sb)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    bool ok = expand(&a, prop, value, lh) == 1;
    if (ok) {
        embi_value_serialize(sb, &lh[0].value);
    }
    embi_arena_free(&a);
    return ok;
}

static bool serializes_to(const char *prop, const char *value, const char *want)
{
    embi_sb sb = {0};
    bool ok = serialize_prop(prop, value, &sb);
    bool same = ok && sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return same;
}

/* Parses a bare calc-sum (no calc() wrapper) with `size` allowed, into a node and its type. */
static bool sized_of(embi_arena *a, const char *s, const embi_calc **node, uint8_t *type)
{
    embi_css_slice in = text_of(a, s);
    return embi_calc_parse_sized(a, in.p, in.n, node, type);
}

/* ------------------------------------------------------------------ parse */

EMB_TEST(calc_size_parses_for_a_sizing_property_with_each_basis)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "height", "calc-size(auto, min(size, 100px))", lh) == 1);
    const embi_value *v = &lh[0].value;
    EMB_REQUIRE(v->kind == EMBI_V_FUNC && v->func.n == 2);
    EMB_CHECK(v->func.args[0].kind == EMBI_V_KEYWORD && v->func.args[0].kw == EMBI_KW_auto);
    EMB_CHECK(v->func.args[1].kind == EMBI_V_CALC);

    EMB_REQUIRE(expand(&a, "min-width", "calc-size(min-content, size + 1px)", lh) == 1);
    EMB_CHECK(lh[0].value.func.args[0].kind == EMBI_V_KEYWORD &&
              lh[0].value.func.args[0].kw == EMBI_KW_min_content);
    EMB_REQUIRE(expand(&a, "max-height", "calc-size(max-content, size)", lh) == 1);
    EMB_CHECK(lh[0].value.func.args[0].kw == EMBI_KW_max_content);
    EMB_REQUIRE(expand(&a, "max-inline-size", "calc-size(fit-content, size / 2)", lh) == 1);
    EMB_CHECK(lh[0].value.func.args[0].kw == EMBI_KW_fit_content);
    EMB_REQUIRE(expand(&a, "width", "calc-size(any, size)", lh) == 1);
    EMB_CHECK(lh[0].value.func.args[0].kind == EMBI_V_KEYWORD && lh[0].value.func.args[0].kw == EMBI_KW_auto);
    EMB_REQUIRE(expand(&a, "block-size", "calc-size(20px, size * 2)", lh) == 1);
    EMB_CHECK(lh[0].value.func.args[0].kind == EMBI_V_LENGTH && lh[0].value.func.args[0].num == 20.0);
    embi_arena_free(&a);
}

EMB_TEST(calc_size_rejects_invalid_forms)
{
    EMB_CHECK(!valid_width("calc-size(auto)"));
    EMB_CHECK(!valid_width("calc-size(auto, 1px, 2px)"));
    EMB_CHECK(!valid_width("calc-size(bogus, 10px)"));
    EMB_CHECK(!valid_width("calc-size(auto, 1)"));          /* a plain number is not a length */
    EMB_CHECK(!valid_width("calc-size(auto, 1deg)"));       /* nor an angle */
    EMB_CHECK(!valid_width("calc-size(auto, size + 1)"));   /* mixed with a number */
    EMB_CHECK(!valid_width("calc-size(auto, foo(size))"));
    EMB_CHECK(!valid_width("calc-size(auto, )"));
    /* `size` is only a keyword inside calc-size(): plain calc() and min() refuse it. */
    EMB_CHECK(!valid_width("calc(size + 1px)"));
    EMB_CHECK(!valid_width("min(size, 10px)"));
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_CHECK(expand(&a, "margin-top", "calc-size(auto, size)", lh) == 0);
    EMB_CHECK(expand(&a, "z-index", "calc-size(auto, size)", lh) == 0);
    embi_arena_free(&a);
}

EMB_TEST(size_keyword_is_a_leaf_only_in_the_sized_parser)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    EMB_REQUIRE(sized_of(&a, "size * 2", &node, &type));
    EMB_CHECK(type == EMBI_CD_LENPCT);
    EMB_CHECK(!embi_calc_is_constant(node));
    /* the public math-function parser still refuses it */
    embi_css_slice in = text_of(&a, "calc(size)");
    const embi_calc *plain = nullptr;
    uint8_t ptype = 0;
    EMB_CHECK(!embi_calc_parse_cv(&a, &in.p[0], &plain, &ptype) || plain == nullptr);
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ serialize */

EMB_TEST(calc_size_serializes_bare_second_argument)
{
    EMB_CHECK(serializes_to("height", "calc-size(auto, min(size, 100px))", "calc-size(auto, min(size, 100px))"));
    EMB_CHECK(serializes_to("min-width", "calc-size(auto, size * 2)", "calc-size(auto, size * 2)"));
    EMB_CHECK(serializes_to("width", "calc-size(10px, size + 5%)", "calc-size(10px, size + 5%)"));
    EMB_CHECK(serializes_to("width", "calc-size(min-content, max(size, 24px))",
                            "calc-size(min-content, max(size, 24px))"));
}

EMB_TEST(calc_values_serialize_their_structure)
{
    EMB_CHECK(serializes_to("width", "calc(10px + 20%)", "calc(10px + 20%)"));
    EMB_CHECK(serializes_to("width", "calc((10px + 20%) * 2)", "calc((10px + 20%) * 2)"));
    EMB_CHECK(serializes_to("width", "calc(100% - (2em + 1px))", "calc(100% - (2em + 1px))"));
    EMB_CHECK(serializes_to("width", "min(1px, 2px, 50%)", "min(1px, 2px, 50%)"));
    EMB_CHECK(serializes_to("width", "clamp(10px, 50%, 300px)", "clamp(10px, 50%, 300px)"));
}

/* ------------------------------------------------------------------ evaluate */

EMB_TEST(size_leaf_takes_the_basis_size)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "height", "calc-size(auto, min(size, 100px))", lh) == 1);
    const embi_calc *node = lh[0].value.func.args[1].calc;
    embi_calc_env env = {0, nullptr, nullptr};
    EMB_CHECK(embi_calc_eval_sized(node, &env, 120.0) == 100.0);
    EMB_CHECK(embi_calc_eval_sized(node, &env, 60.0) == 60.0);
    EMB_CHECK(isnan(embi_calc_eval(node, &env)));          /* no basis: the size is unresolved */
    EMB_CHECK(isnan(embi_calc_eval_sized(node, &env, NAN)));

    EMB_REQUIRE(expand(&a, "min-width", "calc-size(auto, size * 2)", lh) == 1);
    const embi_calc *dbl = lh[0].value.func.args[1].calc;
    EMB_CHECK(embi_calc_eval_sized(dbl, &env, 50.0) == 100.0);
    embi_arena_free(&a);
}

EMB_TEST(size_leaf_resolves_percentages_against_the_base)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    EMB_REQUIRE(sized_of(&a, "size + 50%", &node, &type));
    embi_calc_env env = {200.0, nullptr, nullptr};
    EMB_CHECK(embi_calc_eval_sized(node, &env, 30.0) == 130.0);
    embi_arena_free(&a);
}

EMB_TEST(size_leaf_is_never_constant)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    const embi_calc *node = nullptr;
    uint8_t type = 0;
    EMB_REQUIRE(sized_of(&a, "min(size, 10px)", &node, &type));
    EMB_CHECK(!embi_calc_is_constant(node));
    EMB_REQUIRE(sized_of(&a, "10px * 2", &node, &type));
    EMB_CHECK(embi_calc_is_constant(node));
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
