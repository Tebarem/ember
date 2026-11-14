/*
 * Property database and single-value parsing (W2c): lookups, keyword and type checks, units,
 * CSS-wide keywords, var(), lists, colors through properties, serialization, initial values and
 * the declaration hook.
 */
#include "emb_test.h"

#include "css/props.h"

#include <string.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

static embi_prop_id prop_of(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

/* Parses value text for a longhand. Returns the count written (0 when invalid). */
static uint32_t parse_for(embi_arena *a, const char *prop, const char *value, embi_longhand *out)
{
    return embi_css_parse_decl(a, prop_of(prop), text_of(a, value), out, EMBI_LONGHAND_MAX);
}

static bool value_is_text(const embi_value *v, const char *want)
{
    embi_sb sb = {0};
    embi_value_serialize(&sb, v);
    bool same = sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return same;
}

EMB_TEST(value_lookup_names_and_flags)
{
    EMB_CHECK_EQ_INT(prop_of("margin-top"), EMBI_PROP_margin_top);
    EMB_CHECK_EQ_INT(prop_of("Margin-TOP"), EMBI_PROP_margin_top);
    EMB_CHECK_EQ_INT(prop_of("no-such-prop"), 0);
    EMB_CHECK_EQ_INT(prop_of("--custom"), 0);
    EMB_CHECK(embi_prop_is_inherited(prop_of("color")));
    EMB_CHECK(!embi_prop_is_inherited(prop_of("width")));
    EMB_CHECK(embi_prop_is_shorthand(prop_of("margin")));
    EMB_CHECK(!embi_prop_is_shorthand(prop_of("margin-left")));
    const embi_prop_info *pi = embi_prop_info_of(prop_of("font-size"));
    EMB_REQUIRE(pi != nullptr);
    EMB_CHECK_EQ_STR(pi->name, "font-size");
    EMB_CHECK_EQ_STR(pi->initial, "medium");
    EMB_CHECK_EQ_STR(pi->group, "font");
    EMB_CHECK(embi_kw_lookup((embi_str){"AUTO", 4}) == EMBI_KW_auto);
    EMB_CHECK(embi_kw_lookup((embi_str){"nonsense-kw", 11}) == EMBI_KW_INVALID);
    EMB_CHECK_EQ_STR(embi_kw_name(EMBI_KW_inline_block), "inline-block");
}

EMB_TEST(value_lengths_and_units)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(parse_for(&a, "width", "10px", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LENGTH);
    EMB_CHECK_EQ_INT(lh[0].value.unit, EMBI_U_PX);
    EMB_CHECK(lh[0].value.num == 10.0);

    EMB_REQUIRE(parse_for(&a, "width", "1in", lh) == 1);
    EMB_CHECK(lh[0].value.num == 96.0);
    EMB_CHECK_EQ_INT(lh[0].value.unit, EMBI_U_PX);

    EMB_REQUIRE(parse_for(&a, "width", "2EM", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.unit, EMBI_U_EM);
    EMB_CHECK(lh[0].value.num == 2.0);

    EMB_REQUIRE(parse_for(&a, "width", "3vmin", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.unit, EMBI_U_VMIN);

    EMB_REQUIRE(parse_for(&a, "width", "50%", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_PERCENT);
    EMB_CHECK(lh[0].value.num == 50.0);

    EMB_REQUIRE(parse_for(&a, "margin-left", "0", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LENGTH);
    EMB_CHECK(lh[0].value.num == 0.0);

    /* Non-negative properties reject negative lengths; margins allow them. */
    EMB_CHECK_EQ_INT(parse_for(&a, "padding-top", "-1px", lh), 0);
    EMB_REQUIRE(parse_for(&a, "margin-top", "-1px", lh) == 1);
    EMB_CHECK(lh[0].value.num == -1.0);

    /* Unitless non-zero is not a length. */
    EMB_CHECK_EQ_INT(parse_for(&a, "width", "5", lh), 0);
    /* Unknown unit. */
    EMB_CHECK_EQ_INT(parse_for(&a, "width", "5furlongs", lh), 0);

    EMB_REQUIRE(parse_for(&a, "transition-duration", "500ms", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LIST);
    EMB_REQUIRE(lh[0].value.list.n == 1);
    EMB_CHECK_EQ_INT(lh[0].value.list.items[0].kind, EMBI_V_TIME);
    EMB_CHECK(lh[0].value.list.items[0].num == 0.5);

    embi_arena_free(&a);
}

EMB_TEST(value_keywords_numbers_and_integers)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(parse_for(&a, "display", "inline-block", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_KEYWORD);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_inline_block);
    EMB_CHECK_EQ_INT(parse_for(&a, "display", "bogus", lh), 0);
    /* A keyword from another property is not valid here. */
    EMB_CHECK_EQ_INT(parse_for(&a, "display", "auto", lh), 0);

    EMB_REQUIRE(parse_for(&a, "opacity", "0.5", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_NUMBER);
    EMB_CHECK(lh[0].value.num == 0.5);
    EMB_REQUIRE(parse_for(&a, "opacity", "150%", lh) == 1);
    EMB_CHECK(lh[0].value.num == 1.0);
    EMB_REQUIRE(parse_for(&a, "opacity", "-2", lh) == 1);
    EMB_CHECK(lh[0].value.num == 0.0);

    EMB_REQUIRE(parse_for(&a, "z-index", "3", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_INTEGER);
    EMB_CHECK_EQ_INT(parse_for(&a, "z-index", "2.5", lh), 0);
    EMB_REQUIRE(parse_for(&a, "z-index", "auto", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_KEYWORD);

    EMB_REQUIRE(parse_for(&a, "line-height", "1.5", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_NUMBER);
    EMB_REQUIRE(parse_for(&a, "font-weight", "bold", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_bold);
    embi_arena_free(&a);
}

EMB_TEST(value_colors_through_properties)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(parse_for(&a, "color", "#f00", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_COLOR);
    EMB_CHECK(lh[0].value.color.c[0] == 1.0);

    EMB_REQUIRE(parse_for(&a, "background-color", "currentcolor", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.color.space, EMBI_CS_CURRENT);

    EMB_REQUIRE(parse_for(&a, "caret-color", "auto", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_KEYWORD);

    EMB_REQUIRE(parse_for(&a, "color", "light-dark(red, blue)", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LIGHT_DARK);
    EMB_CHECK(value_is_text(&lh[0].value.pair[0], "color(srgb 1 0 0)"));
    EMB_CHECK(value_is_text(&lh[0].value.pair[1], "color(srgb 0 0 1)"));

    /* light-dark() is a value-level function, not a <color>. */
    EMB_CHECK(!embi_color_parse(text_of(&a, "light-dark(red, blue)"), &(embi_color){0}));
    EMB_CHECK_EQ_INT(parse_for(&a, "color", "light-dark(red)", lh), 0);
    embi_arena_free(&a);
}

EMB_TEST(value_css_wide_var_and_lists)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(parse_for(&a, "width", "inherit", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_INHERIT);
    EMB_REQUIRE(parse_for(&a, "color", "UNSET", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_UNSET);
    EMB_REQUIRE(parse_for(&a, "display", "revert-layer", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_REVERT_LAYER);
    /* CSS-wide keywords cannot be mixed with other values. */
    EMB_CHECK_EQ_INT(parse_for(&a, "width", "inherit 1px", lh), 0);

    EMB_REQUIRE(parse_for(&a, "width", "calc(var(--w) * 2)", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_VAR);
    EMB_CHECK(embi_css_has_var(text_of(&a, "1px var(--x, 2px)")));
    EMB_CHECK(!embi_css_has_var(text_of(&a, "1px 2px")));

    /* Lists: comma-separated, one item per name. */
    EMB_REQUIRE(parse_for(&a, "transition-property", "opacity, color", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LIST);
    EMB_REQUIRE(lh[0].value.list.n == 2);
    EMB_CHECK(value_is_text(&lh[0].value.list.items[0], "opacity"));
    EMB_CHECK(value_is_text(&lh[0].value.list.items[1], "color"));
    EMB_REQUIRE(parse_for(&a, "transition-property", "none", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_KEYWORD);
    EMB_CHECK_EQ_INT(parse_for(&a, "transition-property", "opacity,", lh), 0);

    /* Unquoted family names with several words form one name. */
    EMB_REQUIRE(parse_for(&a, "font-family", "Helvetica Neue, \"Fira Code\", serif", lh) == 1);
    EMB_REQUIRE(lh[0].value.list.n == 3);
    EMB_CHECK(value_is_text(&lh[0].value.list.items[0], "Helvetica Neue"));
    EMB_CHECK(value_is_text(&lh[0].value.list.items[1], "\"Fira Code\""));
    EMB_CHECK(value_is_text(&lh[0].value.list.items[2], "serif"));
    embi_arena_free(&a);
}

EMB_TEST(value_initial_values)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v = {0};
    EMB_REQUIRE(embi_value_initial(&a, prop_of("width"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_KEYWORD);
    EMB_CHECK_EQ_INT(v.kw, EMBI_KW_auto);
    EMB_REQUIRE(embi_value_initial(&a, prop_of("color"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_COLOR);
    EMB_CHECK_EQ_INT(v.color.space, EMBI_CS_SYSTEM);
    EMB_REQUIRE(embi_value_initial(&a, prop_of("border-top-width"), &v));
    EMB_CHECK_EQ_INT(v.kw, EMBI_KW_medium);
    /* Shorthands have no initial value of their own. */
    EMB_CHECK(!embi_value_initial(&a, prop_of("margin"), &v));
    embi_arena_free(&a);
}

EMB_TEST(value_serialization)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(parse_for(&a, "width", "1.50px", lh) == 1);
    EMB_CHECK(value_is_text(&lh[0].value, "1.5px"));
    EMB_REQUIRE(parse_for(&a, "width", "2cm", lh) == 1);
    EMB_CHECK(value_is_text(&lh[0].value, "75.590551px"));
    EMB_REQUIRE(parse_for(&a, "width", "25%", lh) == 1);
    EMB_CHECK(value_is_text(&lh[0].value, "25%"));
    EMB_REQUIRE(parse_for(&a, "width", "fit-content", lh) == 1);
    EMB_CHECK(value_is_text(&lh[0].value, "fit-content"));
    EMB_REQUIRE(parse_for(&a, "opacity", "-0.0000001", lh) == 1);
    EMB_CHECK(value_is_text(&lh[0].value, "0"));
    embi_arena_free(&a);
}

EMB_TEST(value_decl_hook)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    EMB_CHECK(embi_prop_decl_ok(nullptr, EMBI_STR("width"), text_of(&a, "10px"), false));
    EMB_CHECK(!embi_prop_decl_ok(nullptr, EMBI_STR("width"), text_of(&a, "10"), false));
    EMB_CHECK(!embi_prop_decl_ok(nullptr, EMBI_STR("bogus-prop"), text_of(&a, "1px"), false));
    EMB_CHECK(embi_prop_decl_ok(nullptr, EMBI_STR("--anything"), text_of(&a, "{ }"), false));
    EMB_CHECK(embi_prop_decl_ok(nullptr, EMBI_STR("margin"), text_of(&a, "1px 2px"), true));
    EMB_CHECK(!embi_prop_decl_ok(nullptr, EMBI_STR("margin"), text_of(&a, "1px 2px 3px 4px 5px"),
                                 false));
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
