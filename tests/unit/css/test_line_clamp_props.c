/*
 * CSS Overflow 4 line clamping: parsing and shorthand expansion of max-lines, block-ellipsis, continue,
 * line-clamp, -webkit-line-clamp and -webkit-box-orient, and the -webkit-box display keywords.
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

EMB_TEST(properties_exist)
{
    EMB_CHECK(prop_of("max-lines") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("block-ellipsis") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("continue") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("line-clamp") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("-webkit-line-clamp") != EMBI_PROP_NONE);
    EMB_CHECK(prop_of("-webkit-box-orient") != EMBI_PROP_NONE);
}

EMB_TEST(max_lines_accepts_a_positive_integer_or_auto)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "max-lines", "3", lh) == 1);
    EMB_CHECK(lh[0].value.num == 3);
    EMB_REQUIRE(expand(&a, "max-lines", "auto", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_auto);
    EMB_CHECK(expand(&a, "max-lines", "0", lh) == 0);
    EMB_CHECK(expand(&a, "max-lines", "-2", lh) == 0);
    EMB_CHECK(expand(&a, "max-lines", "1.5", lh) == 0);
    EMB_CHECK(expand(&a, "max-lines", "2px", lh) == 0);
    embi_arena_free(&a);
}

EMB_TEST(line_clamp_shorthand_sets_the_three_longhands)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "line-clamp", "3", lh) == 3);
    EMB_CHECK(find(lh, 3, "max-lines")->num == 3);
    EMB_CHECK_EQ_INT(find(lh, 3, "continue")->kw, EMBI_KW_collapse);
    EMB_CHECK_EQ_INT(find(lh, 3, "block-ellipsis")->kw, EMBI_KW_ellipsis);

    EMB_REQUIRE(expand(&a, "line-clamp", "none", lh) == 3);
    EMB_CHECK_EQ_INT(find(lh, 3, "continue")->kw, EMBI_KW_normal);
    EMB_CHECK_EQ_INT(find(lh, 3, "block-ellipsis")->kw, EMBI_KW_no_ellipsis);

    EMB_REQUIRE(expand(&a, "line-clamp", "auto", lh) == 3);
    EMB_CHECK_EQ_INT(find(lh, 3, "max-lines")->kw, EMBI_KW_auto);
    EMB_CHECK_EQ_INT(find(lh, 3, "continue")->kw, EMBI_KW_collapse);

    EMB_CHECK(expand(&a, "line-clamp", "0", lh) == 0);
    EMB_CHECK(expand(&a, "line-clamp", "foo", lh) == 0);
    embi_arena_free(&a);
}

EMB_TEST(webkit_line_clamp_uses_the_legacy_continue)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "-webkit-line-clamp", "2", lh) == 3);
    EMB_CHECK(find(lh, 3, "max-lines")->num == 2);
    EMB_CHECK_EQ_INT(find(lh, 3, "continue")->kw, EMBI_KW__webkit_legacy);
    EMB_CHECK_EQ_INT(find(lh, 3, "block-ellipsis")->kw, EMBI_KW_ellipsis);
    EMB_CHECK(expand(&a, "-webkit-line-clamp", "0", lh) == 0);
    embi_arena_free(&a);
}

EMB_TEST(block_ellipsis_continue_and_orient_keywords)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "block-ellipsis", "no-ellipsis", lh) == 1);
    EMB_REQUIRE(expand(&a, "block-ellipsis", "auto", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_auto);
    EMB_REQUIRE(expand(&a, "block-ellipsis", "\"..\"", lh) == 1);
    EMB_CHECK(expand(&a, "block-ellipsis", "3", lh) == 0);
    EMB_REQUIRE(expand(&a, "continue", "discard", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_discard);
    EMB_CHECK(expand(&a, "continue", "bogus", lh) == 0);
    EMB_REQUIRE(expand(&a, "-webkit-box-orient", "vertical", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW_vertical);
    EMB_CHECK(expand(&a, "-webkit-box-orient", "diagonal", lh) == 0);
    embi_arena_free(&a);
}

EMB_TEST(display_accepts_the_webkit_box_keywords)
{
    embi_arena a = {0};
    embi_arena_init(&a, 2048);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(expand(&a, "display", "-webkit-box", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW__webkit_box);
    EMB_REQUIRE(expand(&a, "display", "-webkit-inline-box", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kw, EMBI_KW__webkit_inline_box);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
