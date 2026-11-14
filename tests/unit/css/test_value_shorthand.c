/*
 * Shorthand expansion (W2c): positional sides and pairs, any-order shorthands, flex, CSS-wide
 * keywords and var() on shorthands. The compound shorthands (font, background, grid, ...) and the
 * elliptical border-radius are in test_value_typed.c.
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

static uint32_t expand(embi_arena *a, const char *prop, const char *value, embi_longhand *out)
{
    return embi_css_parse_decl(a, prop_of(prop), text_of(a, value), out, EMBI_LONGHAND_MAX);
}

/* Finds the entry for a longhand and returns its value, or nullptr. */
static const embi_value *find(const embi_longhand *lh, uint32_t n, const char *name)
{
    embi_prop_id id = prop_of(name);
    for (uint32_t i = 0; i < n; i++) {
        if (lh[i].prop == id) {
            return &lh[i].value;
        }
    }
    return nullptr;
}

static bool is_px(const embi_value *v, double px)
{
    return v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX && v->num == px;
}

EMB_TEST(shorthand_sides_follow_css_order)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "margin", "1px", lh) == 4);
    EMB_CHECK(is_px(find(lh, 4, "margin-top"), 1));
    EMB_CHECK(is_px(find(lh, 4, "margin-left"), 1));

    EMB_REQUIRE(expand(&a, "margin", "1px 2px", lh) == 4);
    EMB_CHECK(is_px(find(lh, 4, "margin-top"), 1));
    EMB_CHECK(is_px(find(lh, 4, "margin-right"), 2));
    EMB_CHECK(is_px(find(lh, 4, "margin-bottom"), 1));
    EMB_CHECK(is_px(find(lh, 4, "margin-left"), 2));

    EMB_REQUIRE(expand(&a, "padding", "1px 2px 3px", lh) == 4);
    EMB_CHECK(is_px(find(lh, 4, "padding-bottom"), 3));
    EMB_CHECK(is_px(find(lh, 4, "padding-left"), 2));

    EMB_REQUIRE(expand(&a, "padding", "1px 2px 3px 4px", lh) == 4);
    EMB_CHECK(is_px(find(lh, 4, "padding-left"), 4));

    /* margin accepts auto per side. */
    EMB_REQUIRE(expand(&a, "margin", "auto 0", lh) == 4);
    const embi_value *top = find(lh, 4, "margin-top");
    EMB_REQUIRE(top != nullptr);
    EMB_CHECK_EQ_INT(top->kind, EMBI_V_KEYWORD);
    EMB_CHECK_EQ_INT(top->kw, EMBI_KW_auto);

    EMB_CHECK_EQ_INT(expand(&a, "margin", "1px 2px 3px 4px 5px", lh), 0);
    EMB_CHECK_EQ_INT(expand(&a, "margin", "", lh), 0);
    EMB_CHECK_EQ_INT(expand(&a, "margin", "1px, 2px", lh), 0);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_pairs_and_inset)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "margin-block", "1px", lh) == 2);
    EMB_CHECK(is_px(find(lh, 2, "margin-block-start"), 1));
    EMB_CHECK(is_px(find(lh, 2, "margin-block-end"), 1));

    EMB_REQUIRE(expand(&a, "padding-inline", "1px 5px", lh) == 2);
    EMB_CHECK(is_px(find(lh, 2, "padding-inline-end"), 5));

    EMB_REQUIRE(expand(&a, "inset", "0 1px 2px 3px", lh) == 4);
    EMB_CHECK(is_px(find(lh, 4, "left"), 3));

    EMB_REQUIRE(expand(&a, "gap", "1px", lh) == 2);
    EMB_CHECK(is_px(find(lh, 2, "row-gap"), 1));
    EMB_CHECK(is_px(find(lh, 2, "column-gap"), 1));
    EMB_REQUIRE(expand(&a, "gap", "normal 2px", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "row-gap")->kw, EMBI_KW_normal);
    EMB_CHECK(is_px(find(lh, 2, "column-gap"), 2));

    EMB_REQUIRE(expand(&a, "overflow", "hidden scroll", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "overflow-y")->kw, EMBI_KW_scroll);

    EMB_REQUIRE(expand(&a, "place-content", "center", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "align-content")->kw, EMBI_KW_center);
    EMB_CHECK_EQ_INT(find(lh, 2, "justify-content")->kw, EMBI_KW_center);
    EMB_CHECK_EQ_INT(expand(&a, "place-items", "space-between", lh), 0);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_border_any_order_and_defaults)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "border", "1px solid red", lh) == 17);
    EMB_CHECK(is_px(find(lh, 17, "border-top-width"), 1));
    EMB_CHECK(is_px(find(lh, 17, "border-left-width"), 1));
    EMB_CHECK_EQ_INT(find(lh, 17, "border-bottom-style")->kw, EMBI_KW_solid);
    EMB_CHECK_EQ_INT(find(lh, 17, "border-right-color")->color.space, EMBI_CS_SRGB);
    EMB_CHECK(find(lh, 17, "border-right-color")->color.c[0] == 1.0);

    /* Order does not matter, and missing components take their initial values. */
    EMB_REQUIRE(expand(&a, "border", "dashed 2px", lh) == 17);
    EMB_CHECK(is_px(find(lh, 17, "border-top-width"), 2));
    EMB_CHECK_EQ_INT(find(lh, 17, "border-top-color")->color.space, EMBI_CS_CURRENT);

    EMB_REQUIRE(expand(&a, "border", "none", lh) == 17);
    EMB_CHECK_EQ_INT(find(lh, 17, "border-top-style")->kw, EMBI_KW_none);
    EMB_CHECK_EQ_INT(find(lh, 17, "border-top-width")->kw, EMBI_KW_medium);

    /* A group may be set only once. */
    EMB_CHECK_EQ_INT(expand(&a, "border", "1px 2px", lh), 0);

    EMB_REQUIRE(expand(&a, "border-top", "solid", lh) == 3);
    EMB_CHECK_EQ_INT(find(lh, 3, "border-top-style")->kw, EMBI_KW_solid);
    EMB_CHECK_EQ_INT(find(lh, 3, "border-top-width")->kw, EMBI_KW_medium);

    EMB_REQUIRE(expand(&a, "border-width", "thin thick", lh) == 4);
    EMB_CHECK_EQ_INT(find(lh, 4, "border-top-width")->kw, EMBI_KW_thin);
    EMB_CHECK_EQ_INT(find(lh, 4, "border-right-width")->kw, EMBI_KW_thick);

    EMB_REQUIRE(expand(&a, "border-radius", "1px 2px 3px", lh) == 4);
    /* Each corner holds a (horizontal, vertical) pair; without "/" both are equal. */
    const embi_value *br = find(lh, 4, "border-bottom-right-radius");
    const embi_value *bl = find(lh, 4, "border-bottom-left-radius");
    EMB_REQUIRE(br != nullptr && br->kind == EMBI_V_LIST && br->list.n == 2);
    EMB_REQUIRE(bl != nullptr && bl->kind == EMBI_V_LIST && bl->list.n == 2);
    EMB_CHECK(is_px(&br->list.items[0], 3) && is_px(&br->list.items[1], 3));
    EMB_CHECK(is_px(&bl->list.items[0], 2) && is_px(&bl->list.items[1], 2));

    embi_arena_free(&a);
}

EMB_TEST(shorthand_outline_text_decoration_list_style)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "outline", "2px dotted", lh) == 3);
    EMB_CHECK(is_px(find(lh, 3, "outline-width"), 2));
    EMB_CHECK_EQ_INT(find(lh, 3, "outline-style")->kw, EMBI_KW_dotted);

    EMB_REQUIRE(expand(&a, "text-decoration", "underline wavy red 2px", lh) == 4);
    EMB_CHECK_EQ_INT(find(lh, 4, "text-decoration-line")->kw, EMBI_KW_underline);
    EMB_CHECK_EQ_INT(find(lh, 4, "text-decoration-style")->kw, EMBI_KW_wavy);
    EMB_CHECK(is_px(find(lh, 4, "text-decoration-thickness"), 2));

    EMB_REQUIRE(expand(&a, "list-style", "square inside", lh) == 3);
    EMB_CHECK_EQ_INT(find(lh, 3, "list-style-type")->kw, EMBI_KW_square);
    EMB_CHECK_EQ_INT(find(lh, 3, "list-style-position")->kw, EMBI_KW_inside);
    EMB_CHECK_EQ_INT(find(lh, 3, "list-style-image")->kw, EMBI_KW_none);
    EMB_CHECK_EQ_INT(expand(&a, "list-style", "square circle", lh), 0);

    EMB_REQUIRE(expand(&a, "flex-flow", "column wrap", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "flex-direction")->kw, EMBI_KW_column);
    EMB_CHECK_EQ_INT(find(lh, 2, "flex-wrap")->kw, EMBI_KW_wrap);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_flex_rules)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "flex", "1", lh) == 3);
    EMB_CHECK(find(lh, 3, "flex-grow")->num == 1.0);
    EMB_CHECK(find(lh, 3, "flex-shrink")->num == 1.0);
    EMB_CHECK_EQ_INT(find(lh, 3, "flex-basis")->kind, EMBI_V_PERCENT);

    EMB_REQUIRE(expand(&a, "flex", "2 3 10px", lh) == 3);
    EMB_CHECK(find(lh, 3, "flex-shrink")->num == 3.0);
    EMB_CHECK(is_px(find(lh, 3, "flex-basis"), 10));

    EMB_REQUIRE(expand(&a, "flex", "none", lh) == 3);
    EMB_CHECK(find(lh, 3, "flex-grow")->num == 0.0);
    EMB_CHECK_EQ_INT(find(lh, 3, "flex-basis")->kw, EMBI_KW_auto);

    EMB_REQUIRE(expand(&a, "flex", "300px", lh) == 3);
    EMB_CHECK(find(lh, 3, "flex-grow")->num == 1.0);
    EMB_CHECK(is_px(find(lh, 3, "flex-basis"), 300));

    EMB_CHECK_EQ_INT(expand(&a, "flex", "1 2 3 4", lh), 0);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_css_wide_and_var)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];

    EMB_REQUIRE(expand(&a, "border", "inherit", lh) == 17);
    for (uint32_t i = 0; i < 17; i++) {
        EMB_CHECK_EQ_INT(lh[i].value.kind, EMBI_V_INHERIT);
    }

    EMB_REQUIRE(expand(&a, "margin", "var(--m)", lh) == 4);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_VAR);
    EMB_CHECK_EQ_INT(lh[3].value.kind, EMBI_V_VAR);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_real_expansions)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    /* text-wrap: any order of mode and style. */
    EMB_REQUIRE(expand(&a, "text-wrap", "balance nowrap", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "text-wrap-mode")->kind, EMBI_V_KEYWORD);
    EMB_CHECK_EQ_INT(find(lh, 2, "text-wrap-style")->kind, EMBI_V_KEYWORD);
    /* columns: width and count in any order. */
    EMB_REQUIRE(expand(&a, "columns", "3 200px", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "column-count")->kind, EMBI_V_INTEGER);
    EMB_CHECK(is_px(find(lh, 2, "column-width"), 200));
    /* border-block-width: one value sets both logical sides. */
    EMB_REQUIRE(expand(&a, "border-block-width", "4px", lh) == 2);
    EMB_CHECK(is_px(find(lh, 2, "border-block-start-width"), 4));
    EMB_CHECK(is_px(find(lh, 2, "border-block-end-width"), 4));
    /* border-block: components set every group, the unset groups take their initial values. */
    EMB_REQUIRE(expand(&a, "border-block", "2px dashed", lh) == 6);
    EMB_CHECK(is_px(find(lh, 6, "border-block-end-width"), 2));
    EMB_CHECK_EQ_INT(find(lh, 6, "border-block-end-style")->kw, EMBI_KW_dashed);
    /* overscroll-behavior pairs. */
    EMB_REQUIRE(expand(&a, "overscroll-behavior", "contain", lh) == 2);
    EMB_CHECK_EQ_INT(find(lh, 2, "overscroll-behavior-y")->kw, EMBI_KW_contain);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_grid_gap_names_are_aliases_of_the_gap_properties)
{
    /* grid-gap, grid-row-gap and grid-column-gap are the legacy names of gap, row-gap and column-gap. */
    EMB_CHECK_EQ_INT(prop_of("grid-gap"), prop_of("gap"));
    EMB_CHECK_EQ_INT(prop_of("grid-row-gap"), prop_of("row-gap"));
    EMB_CHECK_EQ_INT(prop_of("grid-column-gap"), prop_of("column-gap"));
    EMB_CHECK(prop_of("grid-gap") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("GRID-GAP") == prop_of("gap"));
    /* The two-value form expands to row-gap then column-gap. */
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "grid-gap", "3px 7px", lh) == 2);
    EMB_CHECK(is_px(find(lh, 2, "row-gap"), 3));
    EMB_CHECK(is_px(find(lh, 2, "column-gap"), 7));
    EMB_REQUIRE(expand(&a, "grid-row-gap", "5px", lh) == 1);
    EMB_CHECK(is_px(find(lh, 1, "row-gap"), 5));
    EMB_REQUIRE(expand(&a, "grid-column-gap", "6px", lh) == 1);
    EMB_CHECK(is_px(find(lh, 1, "column-gap"), 6));
    /* Other grid- properties are not aliased. */
    EMB_CHECK(prop_of("grid-template-columns") != prop_of("column-gap"));
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
