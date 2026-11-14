/*
 * Focus order and sequential navigation (W5d): the tabindex order (positive ascending, then the zeros in tree order),
 * the skipped elements (negative tabindex, disabled controls, links without href), Tab and Shift+Tab with wrapping,
 * and the focus-lost test after a removal.
 */
#include "emb_test.h"

#include "dom/dom.h"
#include "platform/platform.h"
#include "pf_test.h"

EMB_TEST(focus_order_mixed_tabindex)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *btn = pf_el(doc, &body->base, "button");
    embi_element *div2 = pf_el(doc, &body->base, "div");
    pf_attr(div2, "tabindex", "2");
    embi_element *input = pf_el(doc, &body->base, "input");
    embi_element *dead = pf_el(doc, &body->base, "a"); /* no href: not focusable */
    embi_element *link = pf_el(doc, &body->base, "a");
    pf_attr(link, "href", "/x");
    embi_element *div1 = pf_el(doc, &body->base, "div");
    pf_attr(div1, "tabindex", "1");
    embi_element *neg = pf_el(doc, &body->base, "div");
    pf_attr(neg, "tabindex", "-1"); /* focusable by script, skipped by Tab */
    embi_element *dis = pf_el(doc, &body->base, "textarea");
    pf_attr(dis, "disabled", "");
    embi_element *span0 = pf_el(doc, &body->base, "span");
    pf_attr(span0, "tabindex", "0");
    (void)dead;

    embi_element **items = nullptr;
    uint32_t n = 0;
    embi_pf_focus_order(&body->base, &items, &n);
    EMB_CHECK_EQ_INT(n, 6);
    if (n == 6) {
        EMB_CHECK(items[0] == div1);
        EMB_CHECK(items[1] == div2);
        EMB_CHECK(items[2] == btn);
        EMB_CHECK(items[3] == input);
        EMB_CHECK(items[4] == link);
        EMB_CHECK(items[5] == span0);
    }
    embi_pf_order_free(items, n);

    EMB_CHECK(embi_pf_tabindex(neg) == -1);
    EMB_CHECK(embi_pf_tabindex(btn) == 0);
    EMB_CHECK(embi_pf_tabindex(div2) == 2);
    EMB_CHECK(!embi_pf_focusable(dis));
    embi_document_free(doc);
}

EMB_TEST(focus_step_wraps_and_starts_at_ends)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *a = pf_el(doc, &body->base, "button");
    embi_element *b = pf_el(doc, &body->base, "input");
    embi_element *c = pf_el(doc, &body->base, "select");

    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, false) == a);
    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, true) == c);
    EMB_CHECK(embi_pf_focus_step(&body->base, &a->base, false) == b);
    EMB_CHECK(embi_pf_focus_step(&body->base, &b->base, false) == c);
    EMB_CHECK(embi_pf_focus_step(&body->base, &c->base, false) == a); /* wraps forward */
    EMB_CHECK(embi_pf_focus_step(&body->base, &a->base, true) == c);  /* wraps backward */
    embi_document_free(doc);
}

EMB_TEST(focus_step_empty_order)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    (void)pf_el(doc, &body->base, "div"); /* not focusable */
    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, false) == nullptr);
    embi_document_free(doc);
}

EMB_TEST(focus_lost_after_removal)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *btn = pf_el(doc, &body->base, "button");
    EMB_CHECK(!embi_pf_focus_lost(&btn->base));
    EMB_CHECK(!embi_pf_focus_lost(nullptr));
    EMB_CHECK(embi_remove_child(&body->base, &btn->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_pf_focus_lost(&btn->base));
    embi_destroy_detached(&btn->base);
    embi_document_free(doc);
}

EMB_TEST(focus_step_from_an_element_outside_the_order)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *a = pf_el(doc, &body->base, "button");
    embi_element *neg = pf_el(doc, &body->base, "div");
    pf_attr(neg, "tabindex", "-1"); /* focusable by script, not tabbable */
    embi_element *b = pf_el(doc, &body->base, "input");
    embi_element *c = pf_el(doc, &body->base, "select");
    (void)c;
    /* Tab from the tabindex=-1 element goes to the next tabbable one in tree order, not back to the first. */
    EMB_CHECK(embi_pf_focus_step(&body->base, &neg->base, false) == b);
    /* Shift+Tab goes to the previous one in tree order. */
    EMB_CHECK(embi_pf_focus_step(&body->base, &neg->base, true) == a);
    /* After the last tabbable element, forward wraps to the first; before the first, reverse wraps to the last. */
    embi_element *tail = pf_el(doc, &body->base, "div");
    pf_attr(tail, "tabindex", "-1");
    EMB_CHECK(embi_pf_focus_step(&body->base, &tail->base, false) == a);
    embi_document_free(doc);
}

static bool skip_el(void *user, const embi_element *el)
{
    return el != (const embi_element *)user;
}

EMB_TEST(shift_tab_from_before_the_first_wraps_to_the_last)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *head = pf_el(doc, &body->base, "div");
    pf_attr(head, "tabindex", "-1");
    embi_element *a = pf_el(doc, &body->base, "button");
    embi_element *b = pf_el(doc, &body->base, "button");
    EMB_CHECK(embi_pf_focus_step(&body->base, &head->base, true) == b);
    EMB_CHECK(embi_pf_focus_step(&body->base, &head->base, false) == a);
    embi_document_free(doc);
}

EMB_TEST(focus_step_filter_skips_elements)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *a = pf_el(doc, &body->base, "button");
    embi_element *hidden = pf_el(doc, &body->base, "button");
    embi_element *c = pf_el(doc, &body->base, "button");
    EMB_CHECK(embi_pf_focus_step_filtered(&body->base, &a->base, false, skip_el, hidden) == c);
    EMB_CHECK(embi_pf_focus_step_filtered(&body->base, &c->base, true, skip_el, hidden) == a);
    EMB_CHECK(embi_pf_focus_step_filtered(&body->base, nullptr, false, nullptr, nullptr) == a);
    embi_document_free(doc);
}

EMB_TEST(disabled_fieldset_disables_its_controls)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *fs = pf_el(doc, &body->base, "fieldset");
    pf_attr(fs, "disabled", "");
    embi_element *legend = pf_el(doc, &fs->base, "legend");
    embi_element *in_legend = pf_el(doc, &legend->base, "input");
    embi_element *legend2 = pf_el(doc, &fs->base, "legend");
    embi_element *in_legend2 = pf_el(doc, &legend2->base, "input");
    embi_element *wrap = pf_el(doc, &fs->base, "div");
    embi_element *inside = pf_el(doc, &wrap->base, "button");
    embi_element *outside = pf_el(doc, &body->base, "button");
    EMB_CHECK(!embi_pf_focusable(inside));
    EMB_CHECK(embi_pf_focusable(in_legend)); /* the first legend's controls stay enabled */
    EMB_CHECK(!embi_pf_focusable(in_legend2));
    EMB_CHECK(embi_pf_focusable(outside));
    EMB_CHECK(embi_pf_disabled(inside));
    EMB_CHECK(embi_pf_disabled(in_legend2));
    EMB_CHECK(!embi_pf_disabled(in_legend));
    EMB_CHECK(!embi_pf_disabled(outside));
    EMB_CHECK(!embi_pf_disabled(wrap)); /* not a form control */
    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, false) == in_legend);
    embi_document_free(doc);
}

EMB_TEST(deep_tree_is_collected_without_recursion)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *cur = body;
    for (uint32_t i = 0; i < 20000; i++) {
        cur = pf_el(doc, &cur->base, "div");
    }
    embi_element *btn = pf_el(doc, &cur->base, "button");
    EMB_CHECK(embi_pf_focus_step(&body->base, nullptr, false) == btn);
    embi_element **items = nullptr;
    uint32_t n = 0;
    embi_pf_focus_order(&body->base, &items, &n);
    EMB_CHECK_EQ_INT(n, 1);
    embi_pf_order_free(items, n);
    embi_document_free(doc);
}

EMB_TEST_MAIN()
