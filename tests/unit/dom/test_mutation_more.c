/*
 * MutationObserver core (W6d, src/dom/mutation.c), part 2: transient registered observers, the attribute filter with a
 * namespaced attribute, normalize() record order, replaceChild's reference child, insertion without a removal record, and
 * the bounded memory of held nodes across repeated delivery.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "dom/internal.h"
#include "dom/mutation.h"
#include "dom/slab.h"

#include <string.h>

#define S(lit) EMBI_STR(lit)

static embi_element *mk(embi_document *d, const char *name)
{
    embi_element *el = nullptr;
    (void)embi_dom_create_element(d, (embi_str){name, (uint32_t)strlen(name)}, &el);
    return el;
}

static embi_element *page(embi_document *d)
{
    embi_element *root = mk(d, "div");
    (void)embi_append_child(&d->base, &root->base);
    return root;
}

static embi_mo_opts child_opts(void)
{
    embi_mo_opts o = {0};
    o.child_list = true;
    return o;
}

/* Delivers what the DOM queued: takes the active observers (their transient registrations end) and their records, then
 * releases the holds. Returns the number of records taken. */
static uint32_t deliver(embi_document *d, embi_mo *mo)
{
    embi_mo **list = nullptr;
    uint32_t n = embi_mut_take_active(d, &list);
    if (n) {
        embi_free(list, (size_t)n * sizeof *list);
    }
    embi_mrec *recs = nullptr;
    uint32_t nr = embi_mo_take_records(mo, &recs);
    embi_mrec_free_all(recs, nr);
    embi_mut_release(d, nullptr, nullptr);
    return nr;
}

EMB_TEST(mut_transient_registration_queues_changes_inside_a_removed_subtree)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *x = mk(d, "x");
    embi_element *y = mk(d, "y");
    (void)embi_append_child(&root->base, &x->base);
    (void)embi_append_child(&x->base, &y->base);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    (void)embi_remove_child(&root->base, &x->base);
    /* x is no longer in the tree, but the observer still sees its subtree until it is notified. */
    (void)embi_el_set_attribute(y, S("k"), embi_dstr_from_utf8(d->slab, S("1")));
    /* The notification (take_active, then the records) delivers the record and ends the transient registration. */
    embi_mo **list = nullptr;
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 1);
    embi_free(list, sizeof *list);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(recs[0].type == EMBI_MR_ATTRIBUTES && recs[0].target == &y->base);
    embi_mrec_free_all(recs, n);
    (void)embi_el_set_attribute(y, S("k"), embi_dstr_from_utf8(d->slab, S("2")));
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 0);
    EMB_CHECK_EQ_INT(embi_mo_take_records(&mo, &recs), 0);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_nested_removal_makes_a_transient_registration_from_a_transient_one)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *a = mk(d, "a");
    embi_element *b = mk(d, "b");
    (void)embi_append_child(&root->base, &a->base);
    (void)embi_append_child(&a->base, &b->base);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    /* a leaves the tree (a transient registration on a), then b leaves a in the same task: a's transient registration
     * is in a's registered list, so b gets one too. */
    (void)embi_remove_child(&root->base, &a->base);
    (void)embi_remove_child(&a->base, &b->base);
    (void)embi_el_set_attribute(b, S("k"), embi_dstr_from_utf8(d->slab, S("1")));
    embi_mo **list = nullptr;
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 1);
    embi_free(list, sizeof *list);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(n == 1 && recs[0].type == EMBI_MR_ATTRIBUTES && recs[0].target == &b->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_reobserve_ends_the_transient_registrations_of_the_old_options)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *x = mk(d, "x");
    embi_element *y = mk(d, "y");
    (void)embi_append_child(&root->base, &x->base);
    (void)embi_append_child(&x->base, &y->base);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    (void)embi_remove_child(&root->base, &x->base);
    /* Observing the same target again replaces its options; the transient registration made from the old ones ends
     * (DOM Standard, observe(), step 7.1). The new options do not watch attributes. */
    embi_mo_opts n = {0};
    n.child_list = true;
    (void)embi_mo_observe(&mo, d, &root->base, &n);
    (void)embi_el_set_attribute(y, S("k"), embi_dstr_from_utf8(d->slab, S("1")));
    embi_mrec *recs = nullptr;
    EMB_CHECK_EQ_INT(embi_mo_take_records(&mo, &recs), 0);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_attribute_filter_ignores_a_namespaced_attribute_with_the_same_local_name)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    o.has_filter = true;
    o.nfilter = 1;
    o.filter = embi_alloc(sizeof *o.filter);
    o.filter[0] = embi_atom_intern("title", 5);
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_free(o.filter, sizeof *o.filter);
    /* xlink:title has the local name title but a namespace: the filter (null-namespace names) does not match it. */
    (void)embi_el_set_attribute_ns(root, true, S("http://www.w3.org/1999/xlink"), S("xlink:title"),
                                   embi_dstr_from_utf8(d->slab, S("a")));
    embi_mrec *recs = nullptr;
    EMB_CHECK_EQ_INT(embi_mo_take_records(&mo, &recs), 0);
    (void)embi_el_set_attribute(root, S("title"), embi_dstr_from_utf8(d->slab, S("b")));
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(recs[0].attr_ns == EMBI_NS_NULL);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_normalize_queues_the_data_change_before_the_removals)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_chardata *t1 = embi_new_text_utf8(d, S("a"));
    embi_chardata *t2 = embi_new_text_utf8(d, S("b"));
    (void)embi_append_child(&root->base, &t1->base);
    (void)embi_append_child(&root->base, &t2->base);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.child_list = true;
    o.character_data = true;
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_normalize(&root->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(recs[0].type == EMBI_MR_CHARACTER_DATA && recs[0].target == &t1->base);
    EMB_CHECK(recs[1].type == EMBI_MR_CHILD_LIST && recs[1].nremoved == 1 && recs[1].removed[0] == &t2->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_replace_child_next_sibling_is_the_reference_child)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *x = mk(d, "x");
    embi_element *y = mk(d, "y");
    embi_element *w = mk(d, "w");
    (void)embi_append_child(&root->base, &x->base);
    (void)embi_append_child(&root->base, &y->base);
    (void)embi_append_child(&root->base, &w->base);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    /* y is the next sibling of x, so the reference child is y's next sibling, w. */
    (void)embi_replace_child(&root->base, &y->base, &x->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    /* adopting y first removes it from its old position (a record of its own, observers not suppressed), then comes the replace record */
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(recs[0].nadded == 0 && recs[0].nremoved == 1 && recs[0].removed[0] == &y->base);
    EMB_CHECK(recs[1].nadded == 1 && recs[1].added[0] == &y->base);
    EMB_CHECK(recs[1].nremoved == 1 && recs[1].removed[0] == &x->base);
    EMB_CHECK(recs[1].prev == nullptr);
    EMB_CHECK(recs[1].next == &w->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_insert_of_a_child_of_the_same_parent_queues_the_removal_first)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *x = mk(d, "x");
    embi_element *y = mk(d, "y");
    (void)embi_append_child(&root->base, &x->base);
    (void)embi_append_child(&root->base, &y->base);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    /* Moving x to the end: adopt removes it first (reported, the DOM standard does not suppress that removal), then the insertion. */
    (void)embi_append_child(&root->base, &x->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(recs[0].nremoved == 1 && recs[0].removed[0] == &x->base && recs[0].nadded == 0);
    EMB_CHECK(recs[1].nadded == 1 && recs[1].added[0] == &x->base && recs[1].nremoved == 0);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
}

EMB_TEST(mut_repeated_append_and_remove_keeps_the_memory_bounded)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    /* Each round adds a node, removes it and delivers: the removed node is held until delivery, then freed. */
    for (int i = 0; i < 8; i++) {
        embi_element *el = mk(d, "i");
        (void)embi_append_child(&root->base, &el->base);
        (void)embi_remove_child(&root->base, &el->base);
        EMB_CHECK_EQ_INT(deliver(d, &mo), 2);
    }
    /* Live nodes of the document's slab: a delivered removal frees its node, so the count returns to its value. */
    size_t base = embi_slab_live_blocks(d->slab);
    for (int i = 0; i < 300; i++) {
        embi_element *el = mk(d, "i");
        (void)embi_append_child(&root->base, &el->base);
        (void)embi_remove_child(&root->base, &el->base);
        EMB_CHECK_EQ_INT(deliver(d, &mo), 2);
    }
    EMB_CHECK_EQ_INT((long long)embi_slab_live_blocks(d->slab), (long long)base);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_removed_node_is_freed_once_its_record_is_delivered)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *gone = mk(d, "gone");
    (void)embi_append_child(&root->base, &gone->base);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    size_t before = embi_slab_live_blocks(d->slab);
    (void)embi_remove_child(&root->base, &gone->base);
    /* While the record is pending the removed node is kept (the record refers to it). */
    embi_mo **list = nullptr;
    uint32_t n = embi_mut_take_active(d, &list);
    embi_free(list, (size_t)n * sizeof *list);
    EMB_CHECK_EQ_INT((long long)embi_slab_live_blocks(d->slab), (long long)before);
    embi_mrec *recs = nullptr;
    uint32_t nr = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(nr, 1);
    embi_mrec_free_all(recs, nr);
    embi_mut_release(d, nullptr, nullptr);
    /* After the release the node is freed. */
    EMB_CHECK_EQ_INT((long long)embi_slab_live_blocks(d->slab), (long long)before - 1);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_transient_registrations_are_kept_per_source_registration)
{
    embi_document_free(embi_document_new(true)); /* one-time global allocations (atoms) stay */
    size_t mem0 = embi_mem_live();
    embi_document *d = embi_document_new(true);
    embi_element *a = page(d);
    embi_element *b = mk(d, "b");
    embi_element *c = mk(d, "c");
    (void)embi_append_child(&a->base, &b->base);
    (void)embi_append_child(&b->base, &c->base);
    embi_mo mo = {0};
    embi_mo_opts oa = {0};
    oa.attributes = true;
    oa.subtree = true;
    embi_mo_opts ob = {0};
    ob.child_list = true;
    ob.subtree = true;
    (void)embi_mo_observe(&mo, d, &a->base, &oa);
    (void)embi_mo_observe(&mo, d, &b->base, &ob);
    (void)embi_remove_child(&b->base, &c->base);
    /* One record for the childList removal (b's registration). */
    embi_mo **list = nullptr;
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 1);
    embi_free(list, sizeof *list);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    embi_mrec_free_all(recs, n);
    /* The notification ended the transient registrations; make the removal again and change c before delivery. */
    (void)embi_append_child(&b->base, &c->base);
    n = embi_mo_take_records(&mo, &recs);
    embi_mrec_free_all(recs, n);
    (void)embi_mut_take_active(d, &list);
    if (list) {
        embi_free(list, sizeof *list);
    }
    (void)embi_remove_child(&b->base, &c->base);
    /* The transient registration made from a's registration (attributes) must record this change. */
    (void)embi_el_set_attribute(c, S("k"), embi_dstr_from_utf8(d->slab, S("1")));
    uint32_t na = 0;
    n = embi_mo_take_records(&mo, &recs);
    for (uint32_t i = 0; i < n; i++) {
        na += recs[i].type == EMBI_MR_ATTRIBUTES && recs[i].target == &c->base;
    }
    EMB_CHECK_EQ_INT(na, 1);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The notify step ends the transient registrations of every observer, also of one without records. */
EMB_TEST(mut_notification_ends_transient_registrations_of_observers_without_records)
{
    embi_document_free(embi_document_new(true)); /* one-time global allocations (atoms) stay */
    size_t mem0 = embi_mem_live();
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *other = mk(d, "o");
    embi_element *x = mk(d, "x");
    embi_element *y = mk(d, "y");
    (void)embi_append_child(&root->base, &other->base);
    (void)embi_append_child(&root->base, &x->base);
    (void)embi_append_child(&x->base, &y->base);
    embi_mo A = {0};
    embi_mo B = {0};
    embi_mo_opts oa = {0};
    oa.attributes = true;
    oa.subtree = true;
    embi_mo_opts ob = child_opts();
    A.seq = 1;
    B.seq = 2;
    (void)embi_mo_observe(&A, d, &root->base, &oa);
    (void)embi_mo_observe(&B, d, &other->base, &ob);
    (void)embi_remove_child(&root->base, &x->base); /* transient registration of A on x; no record for A */
    (void)embi_append_child(&other->base, &mk(d, "z")->base); /* B gets a record */
    embi_mo **list = nullptr;
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 1);
    embi_free(list, sizeof *list);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&B, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    embi_mrec_free_all(recs, n);
    /* A's transient registration on x ended with the notification, so this change is not recorded for A. */
    (void)embi_el_set_attribute(y, S("k"), embi_dstr_from_utf8(d->slab, S("1")));
    EMB_CHECK_EQ_INT(embi_mo_take_records(&A, &recs), 0);
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 0);
    (void)embi_mo_disconnect(&A);
    (void)embi_mo_disconnect(&B);
    embi_mut_release(d, nullptr, nullptr);
    embi_document_free(d);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
