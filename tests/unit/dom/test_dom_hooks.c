#include "emb_test.h"

#include "dom/dom.h"

#define S(lit) EMBI_STR(lit)

typedef struct hook_log {
    int children;
    int inserted;
    int removed;
    int chardata;
    int added_seen;
    int removed_seen;
    embi_node *last_parent;
} hook_log;

static void h_children(void *ctx, embi_node *parent, embi_node *first_added, embi_node *last_added,
                       embi_node *first_removed, embi_node *last_removed)
{
    (void)last_added;
    (void)last_removed;
    hook_log *h = ctx;
    h->children++;
    h->last_parent = parent;
    if (first_added) {
        h->added_seen++;
    }
    if (first_removed) {
        h->removed_seen++;
    }
}

static void h_inserted(void *ctx, embi_node *node)
{
    (void)node;
    ((hook_log *)ctx)->inserted++;
}

static void h_removed(void *ctx, embi_node *node)
{
    (void)node;
    ((hook_log *)ctx)->removed++;
}

static void h_chardata(void *ctx, embi_node *node)
{
    (void)node;
    ((hook_log *)ctx)->chardata++;
}

static embi_element *make(embi_document *d, embi_str name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, name, &e) == EMBI_DOM_OK);
    return e;
}

static embi_observer observer_for(hook_log *h)
{
    embi_observer o = {h, h_children, nullptr, h_chardata, h_inserted, h_removed, nullptr};
    return o;
}

EMB_TEST(hooks_report_insert_and_remove)
{
    embi_document *d = embi_document_new(false);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &body->base) == EMBI_DOM_OK);
    hook_log h = {0, 0, 0, 0, 0, 0, nullptr};
    embi_observer o = observer_for(&h);
    embi_document_observe(d, &o);

    embi_element *a = make(d, S("a"));
    EMB_CHECK(embi_append_child(&body->base, &a->base) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(h.children, 1);
    EMB_CHECK_EQ_INT(h.inserted, 1);
    EMB_CHECK(h.last_parent == &body->base);
    EMB_CHECK_EQ_INT(h.added_seen, 1);

    embi_element *b = make(d, S("b"));
    embi_element *c = make(d, S("c"));
    embi_node *frag = embi_new_fragment(d);
    EMB_CHECK(embi_append_child(frag, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(frag, &c->base) == EMBI_DOM_OK);
    int before_children = h.children;
    EMB_CHECK(embi_append_child(&body->base, frag) == EMBI_DOM_OK);
    /* One record for the fragment's children, plus one for the fragment being emptied. */
    EMB_CHECK_EQ_INT(h.children - before_children, 2);
    /* a (1), then b and c into the fragment (2), then the fragment children into body (2). */
    EMB_CHECK_EQ_INT(h.inserted, 5);

    int before_removed = h.removed;
    EMB_CHECK(embi_remove_child(&body->base, &a->base) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(h.removed - before_removed, 1);
    EMB_CHECK(h.removed_seen >= 1);

    embi_document_unobserve(d, &h);
    int frozen = h.children;
    embi_detach(&b->base);
    EMB_CHECK_EQ_INT(h.children, frozen);
    embi_document_free(d);
}

EMB_TEST(hooks_replace_fires_remove_then_insert)
{
    embi_document *d = embi_document_new(false);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &body->base) == EMBI_DOM_OK);
    embi_element *old = make(d, S("old"));
    EMB_CHECK(embi_append_child(&body->base, &old->base) == EMBI_DOM_OK);
    hook_log h = {0, 0, 0, 0, 0, 0, nullptr};
    embi_observer o = observer_for(&h);
    embi_document_observe(d, &o);

    embi_element *fresh = make(d, S("new"));
    EMB_CHECK(embi_replace_child(&body->base, &fresh->base, &old->base) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(h.children, 2);
    EMB_CHECK_EQ_INT(h.inserted, 1);
    EMB_CHECK_EQ_INT(h.removed, 1);
    EMB_CHECK(h.removed_seen == 1);
    EMB_CHECK(h.added_seen == 1);
    embi_document_free(d);
}

EMB_TEST(hooks_report_character_data_and_normalize)
{
    embi_document *d = embi_document_new(false);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &body->base) == EMBI_DOM_OK);
    embi_chardata *t1 = embi_new_text(d, embi_dstr_from_utf8(d->slab, S("x")));
    embi_chardata *t2 = embi_new_text(d, embi_dstr_from_utf8(d->slab, S("y")));
    EMB_CHECK(embi_append_child(&body->base, &t1->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&body->base, &t2->base) == EMBI_DOM_OK);
    hook_log h = {0, 0, 0, 0, 0, 0, nullptr};
    embi_observer o = observer_for(&h);
    embi_document_observe(d, &o);

    embi_dstr v = embi_dstr_from_utf8(d->slab, S("changed"));
    embi_chardata_set(t1, v);
    EMB_CHECK_EQ_INT(h.chardata, 1);
    embi_normalize(&body->base);
    /* The merge changes the first node and removes the second. */
    EMB_CHECK_EQ_INT(h.chardata, 2);
    EMB_CHECK_EQ_INT(h.removed, 1);
    EMB_CHECK(body->base.first_child == &t1->base);
    EMB_CHECK(body->base.first_child->next == nullptr);
    embi_document_free(d);
}

EMB_TEST(hooks_multiple_observers_all_called)
{
    embi_document *d = embi_document_new(false);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &body->base) == EMBI_DOM_OK);
    hook_log first = {0, 0, 0, 0, 0, 0, nullptr};
    hook_log second = {0, 0, 0, 0, 0, 0, nullptr};
    embi_observer o1 = observer_for(&first);
    embi_observer o2 = observer_for(&second);
    embi_document_observe(d, &o1);
    embi_document_observe(d, &o2);
    EMB_CHECK(embi_append_child(&body->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(first.inserted, 1);
    EMB_CHECK_EQ_INT(second.inserted, 1);
    embi_document_unobserve(d, &first);
    EMB_CHECK(embi_append_child(&body->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    EMB_CHECK_EQ_INT(first.inserted, 1);
    EMB_CHECK_EQ_INT(second.inserted, 2);
    embi_document_free(d);
}

EMB_TEST_MAIN()
