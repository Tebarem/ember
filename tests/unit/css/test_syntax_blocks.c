/*
 * Block contents (CSS Nesting's declaration-or-rule ambiguity), declaration lists, and the source
 * offsets of syntax items. The fixture suites cover the reference cases; these pin down the cases
 * the reference data does not, such as custom properties with {} values.
 */
#include "emb_test.h"

#include "css/css.h"

#include <SDL3/SDL_timer.h>

#include <stdlib.h>
#include <string.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static bool is_str(embi_str s, const char *lit)
{
    return embi_str_eq(s, S(lit));
}

static embi_css_items block_items(embi_arena *a, const char *css)
{
    embi_css_slice cvs = embi_css_parse_cvs(a, S(css));
    return embi_css_consume_block_contents(a, cvs);
}

EMB_TEST(css_blocks_custom_property_braces_are_declarations)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    embi_css_items it = block_items(&a, "--x:{a:b};");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(is_str(it.p[0].name, "--x"));

    it = block_items(&a, "--x: { } ;");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(is_str(it.p[0].name, "--x"));

    /* A custom property takes any value, so the extra value after the block is fine too. */
    it = block_items(&a, "--x:{a:b} y;");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_DECL);

    it = block_items(&a, "--x:{a:b}; a:hover{}");
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_QUALIFIED);

    embi_arena_free(&a);
}

EMB_TEST(css_blocks_hover_rules_are_qualified)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    /* A {} block after a non-custom value with other values is a rule, not a declaration. */
    embi_css_items it = block_items(&a, "a:hover{}");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_QUALIFIED);
    EMB_CHECK(it.p[0].prelude.n == 3);

    it = block_items(&a, "a:hover {c:1}");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_QUALIFIED);

    /* A plain declaration with no block is still a declaration. */
    it = block_items(&a, "color: red;");
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_DECL);

    /* An invalid declaration with no block is dropped up to its semicolon, and the next item
     * still parses. */
    it = block_items(&a, "color red; b:c{d:e}");
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_ERROR);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_QUALIFIED);

    embi_arena_free(&a);
}

EMB_TEST(css_blocks_declaration_list_has_no_rules)
{
    /* A declaration list (style attributes, @font-face) never reads a rule: the item is an error. */
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_slice cvs = embi_css_parse_cvs(&a, S("a:hover{}; b:c"));
    embi_css_items it = embi_css_consume_declarations(&a, cvs);
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_ERROR);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_DECL);

    embi_css_item one = embi_css_parse_declaration(&a, embi_css_parse_cvs(&a, S("--x:{a:b}")));
    EMB_CHECK(one.kind == EMBI_CSS_ITEM_DECL);
    one = embi_css_parse_declaration(&a, embi_css_parse_cvs(&a, S("color:{a:b} y")));
    EMB_CHECK(one.kind == EMBI_CSS_ITEM_ERROR);

    embi_arena_free(&a);
}

EMB_TEST(css_blocks_declaration_offsets)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    /* The offsets start at the name and end at the last value token, not at the colon or the
     * whitespace around it. */
    embi_css_item one = embi_css_parse_declaration(&a, embi_css_parse_cvs(&a, S(" c : d ")));
    EMB_REQUIRE(one.kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(one.start == 1);
    EMB_CHECK(one.end == 6);

    one = embi_css_parse_declaration(&a, embi_css_parse_cvs(&a, S("a :b")));
    EMB_REQUIRE(one.kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(one.start == 0);
    EMB_CHECK(one.end == 4);

    /* A custom property value ending in a block ends at the closing brace. */
    one = embi_css_parse_declaration(&a, embi_css_parse_cvs(&a, S("--x:{a}")));
    EMB_REQUIRE(one.kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(one.end == 7);

    embi_arena_free(&a);
}

EMB_TEST(css_blocks_rule_offsets_cover_blocks)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    /* A qualified rule runs to its closing brace. */
    embi_css_slice cvs = embi_css_parse_cvs(&a, S("a{b}"));
    embi_css_items it = embi_css_consume_rules(&a, cvs, true);
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_QUALIFIED);
    EMB_CHECK(it.p[0].start == 0);
    EMB_CHECK(it.p[0].end == 4);

    /* An at-rule with a block runs to its closing brace, and one without runs to its semicolon. */
    cvs = embi_css_parse_cvs(&a, S("@media x {a} @import y;"));
    it = embi_css_consume_rules(&a, cvs, true);
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_AT && it.p[0].has_block);
    EMB_CHECK(it.p[0].start == 0 && it.p[0].end == 12);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_AT && !it.p[1].has_block);
    EMB_CHECK(it.p[1].end == 23);

    /* A block left open at EOF ends at the last token. */
    cvs = embi_css_parse_cvs(&a, S("a{b"));
    it = embi_css_consume_rules(&a, cvs, true);
    EMB_REQUIRE(it.n == 1);
    EMB_CHECK(it.p[0].end == 3);

    embi_arena_free(&a);
}

EMB_TEST(css_blocks_invalid_item_stops_at_semicolon)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    embi_css_items it = block_items(&a, "1; color: red");
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_ERROR);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(is_str(it.p[1].name, "color"));

    it = block_items(&a, "*zoom: 1; color: red; &:hover{x:y}");
    EMB_REQUIRE(it.n == 3);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_ERROR);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_DECL);
    EMB_CHECK(it.p[2].kind == EMBI_CSS_ITEM_QUALIFIED);

    it = block_items(&a, ".x; a{}");
    EMB_REQUIRE(it.n == 2);
    EMB_CHECK(it.p[0].kind == EMBI_CSS_ITEM_ERROR);
    EMB_CHECK(it.p[1].kind == EMBI_CSS_ITEM_QUALIFIED);

    embi_arena_free(&a);
}

/* Parses n copies of a nested rule and returns the best of a few runs in nanoseconds. */
static uint64_t time_nested(int n, uint32_t *count)
{
    size_t len = 0;
    char *buf = malloc((size_t)n * 9 + 1);
    if (buf == nullptr) {
        return 0;
    }
    for (int k = 0; k < n; k++) {
        memcpy(buf + len, "a:hover{}", 9);
        len += 9;
    }
    buf[len] = 0;
    uint64_t best = UINT64_MAX;
    for (int rep = 0; rep < 3; rep++) {
        embi_arena a;
        embi_arena_init(&a, 1 << 20);
        uint64_t t0 = SDL_GetTicksNS();
        embi_css_items it = block_items(&a, buf);
        uint64_t dt = SDL_GetTicksNS() - t0;
        *count = it.n;
        embi_arena_free(&a);
        if (dt < best) {
            best = dt;
        }
    }
    free(buf);
    return best;
}

EMB_TEST(css_blocks_many_nested_rules_linear)
{
    enum { N = 20000 };
    uint32_t c1 = 0;
    uint32_t c4 = 0;
    uint64_t t1 = time_nested(N, &c1);
    uint64_t t4 = time_nested(4 * N, &c4);
    EMB_CHECK(c1 == N);
    EMB_CHECK(c4 == 4 * N);
    /* 4x the input: linear is about 4x, quadratic is about 16x. The floor keeps timer noise on
     * a very fast first run from inflating the ratio. */
    uint64_t floor_ns = 2000000;
    if (t1 < floor_ns) {
        t1 = floor_ns;
    }
    EMB_CHECK(t4 < 10 * t1);
}

EMB_TEST_MAIN()
