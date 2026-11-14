/*
 * CSS counters (W3a, CSS Lists 3 and CSS Content 3): scoping by marks, applying reset and increment
 * from a computed style, implicit list items, and the counter styles.
 */
#include "layout_harness.h"

#include "layout/layout_int.h"

#include <string.h>

static bool fmt_is(int32_t v, uint16_t kw, const char *want)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_ctr_format(&sb, v, kw);
    bool ok = sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return ok;
}

EMB_TEST(counters_force_then_truncate_restores_the_scope)
{
    embi_ctrs c;
    memset(&c, 0, sizeof c);
    embi_atom n = embi_atom_intern("chapter", 7);
    uint32_t m = embi_ctr_mark(&c);
    embi_ctr_force(&c, n, 7, 1);
    EMB_CHECK_EQ_INT(embi_ctr_value(&c, n), 7);
    embi_ctr_truncate(&c, m);
    EMB_CHECK_EQ_INT(embi_ctr_value(&c, n), 0);
    embi_ctr_free(&c);
}

EMB_TEST(counters_apply_reset_then_increment_from_style)
{
    lh h = lh_make("<body><div id=d style='counter-reset: n 3; counter-increment: n 2'></div></body>",
                   "body { margin: 0 }");
    embi_element *d = lh_id(&h, "d");
    EMB_REQUIRE(d != nullptr);
    const embi_style *s = embi_style_get(h.style, d, EMBI_PE_NONE);
    EMB_REQUIRE(s != nullptr);
    embi_ctrs c;
    memset(&c, 0, sizeof c);
    embi_ctr_apply(&c, s, false, false, 0);
    /* reset to 3, then increment by 2 */
    EMB_CHECK_EQ_INT(embi_ctr_value(&c, embi_atom_intern("n", 1)), 5);
    embi_ctr_free(&c);
    lh_free(&h);
}

EMB_TEST(counters_list_items_count_implicitly)
{
    lh h = lh_make("<body><ol><li id=a></li><li id=b></li></ol></body>", "body { margin: 0 }");
    embi_element *a = lh_id(&h, "a");
    embi_element *b = lh_id(&h, "b");
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(b != nullptr);
    embi_ctrs c;
    memset(&c, 0, sizeof c);
    embi_atom li = embi_atom_intern("list-item", 9);
    embi_ctr_apply(&c, embi_style_get(h.style, a, EMBI_PE_NONE), true, false, 0);
    EMB_CHECK_EQ_INT(embi_ctr_value(&c, li), 1);
    embi_ctr_apply(&c, embi_style_get(h.style, b, EMBI_PE_NONE), true, false, 0);
    EMB_CHECK_EQ_INT(embi_ctr_value(&c, li), 2);
    embi_ctr_free(&c);
    lh_free(&h);
}

EMB_TEST(counters_format_decimal_and_negative)
{
    EMB_CHECK(fmt_is(7, EMBI_KW_decimal, "7"));
    EMB_CHECK(fmt_is(-2, EMBI_KW_decimal, "-2"));
    EMB_CHECK(fmt_is(3, EMBI_KW_decimal_leading_zero, "03"));
}

EMB_TEST(counters_format_roman)
{
    EMB_CHECK(fmt_is(4, EMBI_KW_lower_roman, "iv"));
    EMB_CHECK(fmt_is(1994, EMBI_KW_upper_roman, "MCMXCIV"));
}

EMB_TEST(counters_format_alpha_is_bijective_base_26)
{
    EMB_CHECK(fmt_is(1, EMBI_KW_lower_alpha, "a"));
    EMB_CHECK(fmt_is(26, EMBI_KW_lower_alpha, "z"));
    EMB_CHECK(fmt_is(27, EMBI_KW_lower_alpha, "aa"));
    EMB_CHECK(fmt_is(28, EMBI_KW_upper_alpha, "AB"));
}

/* The text of the ::before box of element id. */
static bool before_is(lh *h, const char *id, const char *want)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr || el->box == nullptr) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    lh_pseudo_text((const embi_box *)el->box, el, EMBI_PE_BEFORE, &sb);
    bool ok = sb.len == strlen(want) && (sb.len == 0 || memcmp(sb.data, want, sb.len) == 0);
    embi_sb_free(&sb);
    return ok;
}

EMB_TEST(counters_reset_scope_includes_following_siblings)
{
    /* CSS Lists 3 4.4: a counter-reset on an element creates a counter whose scope is the element,
     * its descendants, its following siblings and their descendants. */
    lh h = lh_make("<body><div><i style='counter-reset: n 5'></i><b id=x></b>"
                   "<u><b id=y></b></u></div></body>",
                   "b::before { content: counter(n) }");
    EMB_CHECK(before_is(&h, "x", "5"));
    EMB_CHECK(before_is(&h, "y", "5"));
    lh_free(&h);
}

EMB_TEST(counters_scope_ends_with_the_parent)
{
    /* The reset in the first div does not reach the second one: there counter() creates a new
     * counter with value 0. */
    lh h = lh_make("<body><div><i style='counter-reset: n 7'></i><b id=x></b></div>"
                   "<div><b id=y></b></div></body>",
                   "b::before { content: counter(n) }");
    EMB_CHECK(before_is(&h, "x", "7"));
    EMB_CHECK(before_is(&h, "y", "0"));
    lh_free(&h);
}

EMB_TEST(counters_sibling_reset_of_the_same_name_replaces_the_counter)
{
    /* A second reset of the same name on a following sibling creates a new counter in the same
     * scope (CSS Lists 3 4.4.1): an element between the two sees 1, one after both sees 10. */
    lh h = lh_make("<body><div><i style='counter-reset: n 1'></i><b id=a></b>"
                   "<i style='counter-reset: n 10'></i><b id=c></b></div></body>",
                   "b::before { content: counter(n) }");
    EMB_CHECK(before_is(&h, "a", "1"));
    EMB_CHECK(before_is(&h, "c", "10"));
    lh_free(&h);
}

EMB_TEST(counters_function_joins_nested_instances)
{
    /* counters() lists every counter of that name in scope, outermost first. */
    lh h = lh_make("<body><div style='counter-reset: n 1'><p style='counter-reset: n 2'>"
                   "<b id=x></b></p></div></body>",
                   "b::before { content: counters(n, '.') }");
    EMB_CHECK(before_is(&h, "x", "1.2"));
    lh_free(&h);
}

EMB_TEST(counters_increment_and_set_apply_in_order)
{
    lh h = lh_make("<body><div style='counter-reset: n 1'>"
                   "<i style='counter-increment: n 4'></i><b id=a></b>"
                   "<i style='counter-set: n 20'></i><b id=c></b></div></body>",
                   "b::before { content: counter(n) }");
    EMB_CHECK(before_is(&h, "a", "5"));
    EMB_CHECK(before_is(&h, "c", "20"));
    lh_free(&h);
}

EMB_TEST_MAIN()
