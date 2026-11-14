/*
 * MutationObserver core (W6d, src/dom/mutation.c): the records that the DOM queues for each kind of change, the transient
 * registrations of removed subtrees, the signal and the held nodes. Checked against the tree operations directly, without
 * script (the bindings' tests cover the JS side).
 */
#include "emb_test.h"

#include "dom/dom.h"
#include "dom/internal.h"
#include "dom/mutation.h"

#include <string.h>

#define S(lit) EMBI_STR(lit)

static embi_element *mk(embi_document *d, const char *name)
{
    embi_element *el = nullptr;
    (void)embi_dom_create_element(d, (embi_str){name, (uint32_t)strlen(name)}, &el);
    return el;
}

/* A document with <div id=root> under it. */
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

EMB_TEST(mut_child_list_append_records_the_added_node)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    EMB_CHECK(embi_mo_observe(&mo, d, &root->base, &o));
    EMB_CHECK(embi_mo_observing(&mo));
    embi_element *a = mk(d, "span");
    (void)embi_append_child(&root->base, &a->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(recs[0].type == EMBI_MR_CHILD_LIST);
    EMB_CHECK(recs[0].target == &root->base);
    EMB_CHECK_EQ_INT(recs[0].nadded, 1);
    EMB_CHECK(recs[0].added[0] == &a->base);
    EMB_CHECK_EQ_INT(recs[0].nremoved, 0);
    EMB_CHECK(recs[0].prev == nullptr && recs[0].next == nullptr);
    embi_mrec_free_all(recs, n);
    /* takeRecords empties the queue. */
    EMB_CHECK_EQ_INT(embi_mo_take_records(&mo, &recs), 0);
    EMB_CHECK(embi_mo_disconnect(&mo));
    EMB_CHECK(!embi_mo_observing(&mo));
    EMB_CHECK(!embi_mo_disconnect(&mo));
    embi_document_free(d);
}

EMB_TEST(mut_remove_records_the_siblings_at_the_removal)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *a = mk(d, "a");
    embi_element *b = mk(d, "b");
    embi_element *c = mk(d, "c");
    (void)embi_append_child(&root->base, &a->base);
    (void)embi_append_child(&root->base, &b->base);
    (void)embi_append_child(&root->base, &c->base);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    (void)embi_remove_child(&root->base, &b->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(recs[0].nremoved, 1);
    EMB_CHECK(recs[0].removed[0] == &b->base);
    EMB_CHECK(recs[0].prev == &a->base);
    EMB_CHECK(recs[0].next == &c->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_no_record_without_a_matching_option)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_element *a = mk(d, "span");
    (void)embi_append_child(&root->base, &a->base);
    EMB_CHECK(embi_mut_peek(d) != nullptr);
    /* childList was not asked for, so the insertion queues nothing. */
    embi_mrec *recs = nullptr;
    EMB_CHECK_EQ_INT(embi_mo_take_records(&mo, &recs), 0);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_subtree_sees_descendants_only_when_asked)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *mid = mk(d, "section");
    embi_element *leaf = mk(d, "p");
    (void)embi_append_child(&root->base, &mid->base);
    (void)embi_append_child(&mid->base, &leaf->base);
    embi_mo shallow = {0};
    embi_mo deep = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&shallow, d, &root->base, &o);
    o.subtree = true;
    (void)embi_mo_observe(&deep, d, &root->base, &o);
    embi_element *x = mk(d, "i");
    (void)embi_append_child(&leaf->base, &x->base);
    embi_mrec *recs = nullptr;
    EMB_CHECK_EQ_INT(embi_mo_take_records(&shallow, &recs), 0);
    uint32_t n = embi_mo_take_records(&deep, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(recs[0].target == &leaf->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&shallow);
    (void)embi_mo_disconnect(&deep);
    embi_document_free(d);
}

EMB_TEST(mut_attribute_records_old_value_and_filter)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.attributes = true;
    o.attr_old = true;
    o.has_filter = true;
    o.nfilter = 1;
    o.filter = embi_alloc(sizeof *o.filter);
    o.filter[0] = embi_atom_intern("title", 5);
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    /* The observation keeps its own copy of the filter. */
    embi_free(o.filter, sizeof *o.filter);
    (void)embi_el_set_attribute(root, S("title"), embi_dstr_from_utf8(d->slab, S("a")));
    (void)embi_el_set_attribute(root, S("title"), embi_dstr_from_utf8(d->slab, S("b")));
    (void)embi_el_set_attribute(root, S("class"), embi_dstr_from_utf8(d->slab, S("x")));
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(recs[0].type == EMBI_MR_ATTRIBUTES);
    EMB_CHECK(recs[0].attr_local == embi_atom_intern("title", 5));
    EMB_CHECK(!recs[0].has_old);
    EMB_CHECK(recs[1].has_old);
    EMB_CHECK_EQ_INT(recs[1].old_value.len, 1);
    EMB_CHECK(embi_dstr_unit(recs[1].old_value, 0) == 'a');
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_character_data_records_the_old_value)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_chardata *t = embi_new_text_utf8(d, S("one"));
    (void)embi_append_child(&root->base, &t->base);
    embi_mo mo = {0};
    embi_mo_opts o = {0};
    o.character_data = true;
    o.char_old = true;
    o.subtree = true;
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_chardata_set(t, embi_dstr_from_utf8(d->slab, S("two")));
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(recs[0].type == EMBI_MR_CHARACTER_DATA);
    EMB_CHECK(recs[0].target == &t->base);
    EMB_CHECK(recs[0].has_old);
    EMB_CHECK_EQ_INT(recs[0].old_value.len, 3);
    EMB_CHECK(embi_dstr_unit(recs[0].old_value, 0) == 'o');
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_replace_all_is_one_record_with_removed_and_added)
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
    embi_element *z = mk(d, "z");
    (void)embi_replace_all(&root->base, &z->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(recs[0].nremoved, 2);
    EMB_CHECK(recs[0].removed[0] == &x->base && recs[0].removed[1] == &y->base);
    EMB_CHECK_EQ_INT(recs[0].nadded, 1);
    EMB_CHECK(recs[0].added[0] == &z->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_replace_child_is_one_record)
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
    embi_element *n1 = mk(d, "n");
    (void)embi_replace_child(&root->base, &n1->base, &y->base);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(recs[0].nadded, 1);
    EMB_CHECK(recs[0].added[0] == &n1->base);
    EMB_CHECK_EQ_INT(recs[0].nremoved, 1);
    EMB_CHECK(recs[0].removed[0] == &y->base);
    EMB_CHECK(recs[0].prev == &x->base);
    EMB_CHECK(recs[0].next == &w->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_fragment_insert_records_its_children_once)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_node *frag = embi_new_fragment(d);
    embi_element *a = mk(d, "a");
    embi_element *b = mk(d, "b");
    (void)embi_append_child(frag, &a->base);
    (void)embi_append_child(frag, &b->base);
    (void)embi_append_child(&root->base, frag);
    embi_mrec *recs = nullptr;
    /* The appends into the fragment are not changes of root; inserting the fragment is one record with both children. */
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(recs[0].nadded, 2);
    EMB_CHECK(recs[0].added[0] == &a->base && recs[0].added[1] == &b->base);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

static int g_signals;

static void count_signal(void *user)
{
    (void)user;
    g_signals++;
}

EMB_TEST(mut_signal_once_per_batch_and_take_active_rearms)
{
    g_signals = 0;
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_mut_set_signal(d, count_signal, nullptr);
    embi_element *a = mk(d, "a");
    embi_element *b = mk(d, "b");
    (void)embi_append_child(&root->base, &a->base);
    (void)embi_append_child(&root->base, &b->base);
    EMB_CHECK_EQ_INT(g_signals, 1);
    embi_mo **list = nullptr;
    uint32_t n = embi_mut_take_active(d, &list);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK(list[0] == &mo);
    embi_free(list, sizeof *list);
    EMB_CHECK_EQ_INT(embi_mut_take_active(d, &list), 0);
    /* A new batch after the notification signals again. */
    embi_element *c = mk(d, "c");
    (void)embi_append_child(&root->base, &c->base);
    EMB_CHECK_EQ_INT(g_signals, 2);
    embi_mrec *recs = nullptr;
    uint32_t nrec = embi_mo_take_records(&mo, &recs);
    embi_mrec_free_all(recs, nrec);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_held_record_nodes_survive_replace_all)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_element *keep = mk(d, "keep");
    embi_element *gone = mk(d, "gone");
    (void)embi_append_child(&root->base, &keep->base);
    (void)embi_append_child(&root->base, &gone->base);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    (void)embi_remove_child(&root->base, &gone->base);
    /* gone has no wrapper: without the hold, replaceAll below would free it and the record would dangle. */
    (void)embi_replace_all(&root->base, nullptr);
    EMB_CHECK(embi_destroy_if_unowned(&gone->base) == false);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(&mo, &recs);
    EMB_CHECK(n >= 2);
    bool seen = false;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t k = 0; k < recs[i].nremoved; k++) {
            if (recs[i].removed[k] == &gone->base) {
                seen = true;
            }
        }
    }
    EMB_CHECK(seen);
    embi_mrec_free_all(recs, n);
    (void)embi_mo_disconnect(&mo);
    embi_document_free(d);
}

EMB_TEST(mut_document_free_while_observing_is_clean)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = page(d);
    embi_mo mo = {0};
    embi_mo_opts o = child_opts();
    (void)embi_mo_observe(&mo, d, &root->base, &o);
    embi_element *a = mk(d, "a");
    (void)embi_append_child(&root->base, &a->base);
    /* The document goes away while observed: the records and the observation arrays are released with it. */
    embi_document_free(d);
    EMB_CHECK(!embi_mo_observing(&mo));
}

EMB_TEST_MAIN()
