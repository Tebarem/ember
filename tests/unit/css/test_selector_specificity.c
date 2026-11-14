/*
 * Specificity (Selectors Level 4 section 17): the (a, b, c) table, including the :is(), :not(),
 * :has(), :nth-*(of S) and :where() rules, and the ordering used by the cascade.
 */
#include "emb_test.h"

#include "css/selector.h"

#include <string.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

/* Specificity of the most specific item of the list (a single-item list here). */
static embi_sel_spec spec_of(embi_arena *a, const char *sel)
{
    const embi_sel_list *l = embi_sel_parse_text(a, S(sel), nullptr);
    if (l == nullptr) {
        embi_sel_spec bad = {0xFFFF, 0xFFFF, 0xFFFF};
        return bad;
    }
    embi_sel_spec best = {0, 0, 0};
    for (uint32_t k = 0; k < l->n; k++) {
        embi_sel_spec s = embi_sel_specificity(&l->items[k]);
        if (embi_sel_spec_cmp(s, best) > 0) {
            best = s;
        }
    }
    return best;
}

static void expect_spec(embi_arena *a, const char *sel, uint32_t x, uint32_t y, uint32_t z)
{
    embi_sel_spec s = spec_of(a, sel);
    if (s.a != x || s.b != y || s.c != z) {
        SDL_Log("    %s: got (%u,%u,%u), want (%u,%u,%u)", sel, (unsigned)s.a, (unsigned)s.b,
                (unsigned)s.c, (unsigned)x, (unsigned)y, (unsigned)z);
    }
    EMB_CHECK(s.a == x && s.b == y && s.c == z);
}

EMB_TEST(spec_basic_table)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    expect_spec(&a, "*", 0, 0, 0);
    expect_spec(&a, "li", 0, 0, 1);
    expect_spec(&a, "li::before", 0, 0, 2);
    expect_spec(&a, ".a", 0, 1, 0);
    expect_spec(&a, "#a", 1, 0, 0);
    expect_spec(&a, "#a.b[c]:hover", 1, 3, 0);
    expect_spec(&a, "div > p span", 0, 0, 3);
    expect_spec(&a, "a:not(.b c)", 0, 1, 2);
    expect_spec(&a, "input[type=text]", 0, 1, 1);
    expect_spec(&a, "*|*", 0, 0, 0);
    embi_arena_free(&a);
}

EMB_TEST(spec_logical_pseudo_classes)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    /* :is() and :not() take the most specific argument. */
    expect_spec(&a, ":is(#a, .b)", 1, 0, 0);
    expect_spec(&a, ":not(#a)", 1, 0, 0);
    expect_spec(&a, ":is(li, .b)", 0, 1, 0);
    /* :where() contributes nothing. */
    expect_spec(&a, ":where(#a)", 0, 0, 0);
    expect_spec(&a, "p:where(#a)", 0, 0, 1);
    /* :has() takes its most specific relative selector. */
    expect_spec(&a, ":has(#a)", 1, 0, 0);
    expect_spec(&a, "div:has(> p.x)", 0, 1, 2);
    embi_arena_free(&a);
}

EMB_TEST(spec_nth_with_of)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    /* The pseudo-class counts once in b, plus the most specific argument of "of S". */
    expect_spec(&a, ":nth-child(2n+1)", 0, 1, 0);
    expect_spec(&a, ":nth-last-of-type(odd)", 0, 1, 0);
    expect_spec(&a, ":nth-child(2 of #a.b)", 1, 2, 0);
    expect_spec(&a, "li:nth-child(2 of .x)", 0, 2, 1);
    embi_arena_free(&a);
}

EMB_TEST(spec_ordering)
{
    embi_sel_spec lo = {0, 9, 9};
    embi_sel_spec hi = {1, 0, 0};
    EMB_CHECK(embi_sel_spec_cmp(lo, hi) < 0);
    EMB_CHECK(embi_sel_spec_cmp(hi, lo) > 0);
    EMB_CHECK(embi_sel_spec_cmp(hi, hi) == 0);
    embi_sel_spec b1 = {0, 2, 0};
    embi_sel_spec b2 = {0, 1, 5};
    EMB_CHECK(embi_sel_spec_cmp(b1, b2) > 0);
}

EMB_TEST_MAIN()
