/*
 * Hover chains and the enter and leave sequences (W5d), and click counts: the count grows for presses on the same
 * button, target and place within the time window, and restarts otherwise.
 */
#include "emb_test.h"

#include "dom/dom.h"
#include "platform/platform.h"
#include "pf_test.h"

EMB_TEST(chain_is_the_element_ancestry)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *div = pf_el(doc, &body->base, "div");
    embi_element *span = pf_el(doc, &div->base, "span");
    embi_element *chain[16];
    uint32_t n = embi_pf_chain(&span->base, chain, 16);
    EMB_CHECK_EQ_INT(n, 4); /* span, div, body, html (the document node is not an element) */
    if (n == 4) {
        EMB_CHECK(chain[0] == span);
        EMB_CHECK(chain[1] == div);
        EMB_CHECK(chain[3]->base.name == embi_as_element(doc->base.first_child)->base.name);
    }
    EMB_CHECK_EQ_INT(embi_pf_chain(&span->base, chain, 2), 2); /* capped */
    embi_document_free(doc);
}

EMB_TEST(crossings_leave_inner_enter_outer)
{
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *div = pf_el(doc, &body->base, "div");
    embi_element *span = pf_el(doc, &div->base, "span");
    embi_element *p = pf_el(doc, &body->base, "p");
    embi_element *oc[16], *nc[16];
    uint32_t no = embi_pf_chain(&span->base, oc, 16);
    uint32_t nn = embi_pf_chain(&p->base, nc, 16);
    embi_element *leave[16], *enter[16];
    embi_pf_crossing r = embi_pf_crossings((const embi_element *const *)oc, no, (const embi_element *const *)nc, nn,
                                           leave, enter, 16);
    EMB_CHECK_EQ_INT(r.nleave, 2);
    EMB_CHECK_EQ_INT(r.nenter, 1);
    if (r.nleave == 2) {
        EMB_CHECK(leave[0] == span); /* innermost first */
        EMB_CHECK(leave[1] == div);
    }
    if (r.nenter == 1) {
        EMB_CHECK(enter[0] == p);
    }

    /* Moving into a child: the parent stays, only the child is entered. */
    embi_pf_crossing down = embi_pf_crossings((const embi_element *const *)oc + 2, no - 2,
                                              (const embi_element *const *)oc, no, leave, enter, 16);
    EMB_CHECK_EQ_INT(down.nleave, 0);
    EMB_CHECK_EQ_INT(down.nenter, 2);
    if (down.nenter == 2) {
        EMB_CHECK(enter[0] == div); /* outermost first */
        EMB_CHECK(enter[1] == span);
    }
    embi_document_free(doc);
}

EMB_TEST(click_count_window)
{
    embi_pf_clicks s = {0};
    int target_a = 0, target_b = 0;
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 0.0, 10.0f, 10.0f, 0, &target_a), 1);
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 100.0, 11.0f, 10.0f, 0, &target_a), 2);
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 200.0, 11.0f, 10.0f, 0, &target_a), 3);
    /* Another button restarts. */
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 250.0, 11.0f, 10.0f, 2, &target_a), 1);
    /* Too far away restarts. */
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 300.0, 40.0f, 10.0f, 2, &target_a), 1);
    /* Too late restarts. */
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 1000.0, 40.0f, 10.0f, 2, &target_a), 1);
    /* Another target restarts. */
    EMB_CHECK_EQ_INT(embi_pf_click_press(&s, 1100.0, 40.0f, 10.0f, 2, &target_b), 1);
}

EMB_TEST(deep_chains_stay_aligned_at_the_root)
{
    /* Two leaves of a tree deeper than the chain cap: the chains keep their outermost elements, so the leave and enter
     * lists are the true difference of the two paths (the shared part is not reported). */
    embi_element *body = nullptr;
    embi_document *doc = pf_doc(&body);
    embi_element *cur = body;
    for (uint32_t i = 0; i < EMBI_PF_MAX_DEPTH + 40; i++) {
        cur = pf_el(doc, &cur->base, "div");
    }
    embi_element *a = pf_el(doc, &cur->base, "span");
    embi_element *b = pf_el(doc, &cur->base, "em");
    embi_element *ca[EMBI_PF_MAX_DEPTH], *cb[EMBI_PF_MAX_DEPTH];
    uint32_t na = embi_pf_chain(&a->base, ca, EMBI_PF_MAX_DEPTH);
    uint32_t nb = embi_pf_chain(&b->base, cb, EMBI_PF_MAX_DEPTH);
    EMB_CHECK_EQ_INT(na, EMBI_PF_MAX_DEPTH);
    EMB_CHECK_EQ_INT(nb, EMBI_PF_MAX_DEPTH);
    /* The innermost 41 elements are dropped from both: the outermost cap elements agree entirely. */
    embi_element *leave[8], *enter[8];
    embi_pf_crossing r = embi_pf_crossings((const embi_element *const *)ca, na, (const embi_element *const *)cb, nb,
                                           leave, enter, 8);
    EMB_CHECK_EQ_INT(r.nleave, 0);
    EMB_CHECK_EQ_INT(r.nenter, 0);
    EMB_CHECK(ca[na - 1] == cb[nb - 1]); /* the same root end */
    /* Shallow siblings under a deep common parent still report the real difference. */
    embi_element *sa = pf_el(doc, &body->base, "p");
    embi_element *sb = pf_el(doc, &body->base, "p");
    na = embi_pf_chain(&sa->base, ca, EMBI_PF_MAX_DEPTH);
    nb = embi_pf_chain(&sb->base, cb, EMBI_PF_MAX_DEPTH);
    r = embi_pf_crossings((const embi_element *const *)ca, na, (const embi_element *const *)cb, nb, leave, enter, 8);
    EMB_CHECK_EQ_INT(r.nleave, 1);
    EMB_CHECK_EQ_INT(r.nenter, 1);
    EMB_CHECK(leave[0] == sa && enter[0] == sb);
    embi_document_free(doc);
}

EMB_TEST_MAIN()
