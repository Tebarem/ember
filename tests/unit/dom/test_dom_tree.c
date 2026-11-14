#include "emb_test.h"

#include "dom/dom.h"

#define S(lit) EMBI_STR(lit)

static embi_element *make(embi_document *d, embi_str name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, name, &e) == EMBI_DOM_OK);
    return e;
}

static embi_dom_err app(embi_node *p, embi_node *c)
{
    return embi_append_child(p, c);
}

static uint32_t child_count_of(const embi_node *n)
{
    uint32_t k = 0;
    for (const embi_node *c = n->first_child; c; c = c->next) {
        k++;
    }
    return k;
}

static embi_dom_err ins(embi_node *p, embi_node *c, embi_node *before)
{
    return embi_pre_insert(p, c, before);
}

static embi_chardata *text(embi_document *d, const char *utf8)
{
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    embi_dstr v = embi_dstr_from_utf8(d->slab, s);
    embi_chardata *cd = embi_new_text(d, v);
    embi_dstr_free(v);
    return cd;
}

static embi_node *tn(embi_document *d, const char *utf8)
{
    return &text(d, utf8)->base;
}

/* Text content of n as a UTF-8 string in out (NUL terminated when it fits). */
static bool text_of(embi_node *n, char *out, size_t cap)
{
    embi_dbuf b;
    embi_dbuf_init(&b, n->doc->slab);
    bool has = embi_text_content_get(n, &b);
    embi_dstr d = embi_dbuf_finish(&b);
    size_t len = embi_dstr_to_utf8(d, out, cap);
    if (len < cap) {
        out[len] = '\0';
    }
    embi_dstr_free(d);
    embi_dbuf_free(&b);
    return has;
}

static uint32_t child_count(const embi_node *n)
{
    uint32_t k = 0;
    for (const embi_node *c = n->first_child; c; c = c->next) {
        k++;
    }
    return k;
}

EMB_TEST(tree_append_links_siblings_and_parents)
{
    embi_document *d = embi_document_new(true);
    embi_element *html = make(d, S("html"));
    EMB_CHECK(app(&d->base, &html->base) == EMBI_DOM_OK);
    embi_element *head = make(d, S("head"));
    embi_element *body = make(d, S("body"));
    EMB_CHECK(app(&html->base, &head->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&html->base, &body->base) == EMBI_DOM_OK);
    EMB_CHECK(html->base.first_child == &head->base);
    EMB_CHECK(html->base.last_child == &body->base);
    EMB_CHECK(head->base.next == &body->base);
    EMB_CHECK(body->base.prev == &head->base);
    EMB_CHECK(body->base.parent == &html->base);
    EMB_CHECK(html->base.parent == &d->base);
    EMB_CHECK(embi_document_element(d) == html);
    EMB_CHECK(head->base.flags & EMBI_NF_CONNECTED);
    EMB_CHECK(embi_get_root(&body->base) == &d->base);
    embi_document_free(d);
}

EMB_TEST(tree_hierarchy_errors_for_bad_parents_and_cycles)
{
    embi_document *d = embi_document_new(false);
    embi_element *div = make(d, S("div"));
    embi_element *span = make(d, S("span"));
    embi_chardata *t = text(d, "x");
    embi_doctype *dt = nullptr;
    EMB_CHECK(embi_dom_create_doctype(d, S("html"), S(""), S(""), &dt) == EMBI_DOM_OK);
    EMB_CHECK(app(&d->base, &div->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, &span->base) == EMBI_DOM_OK);

    EMB_CHECK(app(&t->base, &span->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(&div->base, &div->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(&span->base, &div->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(&div->base, &dt->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(&div->base, &d->base) == EMBI_DOM_ERR_HIERARCHY);

    embi_element *other = make(d, S("p"));
    EMB_CHECK(ins(&div->base, &other->base, &t->base) == EMBI_DOM_ERR_NOT_FOUND);
    EMB_CHECK(embi_remove_child(&div->base, &other->base) == EMBI_DOM_ERR_NOT_FOUND);

    /* Text may not be a child of a document. */
    EMB_CHECK(app(&d->base, &t->base) == EMBI_DOM_ERR_HIERARCHY);
    embi_document_free(d);
}

EMB_TEST(tree_document_child_rules)
{
    embi_document *d = embi_document_new(true);
    embi_doctype *dt = nullptr;
    EMB_CHECK(embi_dom_create_doctype(d, S("html"), S(""), S(""), &dt) == EMBI_DOM_OK);
    embi_element *html = make(d, S("html"));
    embi_element *second = make(d, S("body"));

    EMB_CHECK(app(&d->base, &html->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&d->base, &second->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(&d->base, &dt->base) == EMBI_DOM_ERR_HIERARCHY);

    /* A doctype goes before the document element, and only once. */
    EMB_CHECK(ins(&d->base, &dt->base, &html->base) == EMBI_DOM_OK);
    embi_doctype *dt2 = nullptr;
    EMB_CHECK(embi_dom_create_doctype(d, S("x"), S(""), S(""), &dt2) == EMBI_DOM_OK);
    EMB_CHECK(ins(&d->base, &dt2->base, &html->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(child_count(&d->base) == 2);
    embi_document_free(d);
}

EMB_TEST(tree_document_fragment_rules)
{
    embi_document *d = embi_document_new(true);
    embi_node *frag = embi_new_fragment(d);
    embi_element *a = make(d, S("a"));
    embi_element *b = make(d, S("b"));
    EMB_CHECK(app(frag, &a->base) == EMBI_DOM_OK);
    EMB_CHECK(app(frag, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&d->base, frag) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(child_count(frag) == 2);

    embi_node *frag2 = embi_new_fragment(d);
    embi_element *c = make(d, S("c"));
    EMB_CHECK(app(frag2, &c->base) == EMBI_DOM_OK);
    EMB_CHECK(app(frag2, tn(d, "t")) == EMBI_DOM_OK);
    EMB_CHECK(app(&d->base, frag2) == EMBI_DOM_ERR_HIERARCHY);

    embi_node *frag3 = embi_new_fragment(d);
    embi_element *e = make(d, S("e"));
    EMB_CHECK(app(frag3, &e->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&d->base, frag3) == EMBI_DOM_OK);
    EMB_CHECK(child_count(frag3) == 0);
    EMB_CHECK(embi_document_element(d) == e);
    embi_document_free(d);
}

EMB_TEST(tree_insert_before_and_replace_and_remove)
{
    embi_document *d = embi_document_new(false);
    embi_element *ul = make(d, S("ul"));
    embi_element *a = make(d, S("a"));
    embi_element *c = make(d, S("c"));
    EMB_CHECK(app(&ul->base, &a->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&ul->base, &c->base) == EMBI_DOM_OK);
    embi_element *b = make(d, S("b"));
    EMB_CHECK(ins(&ul->base, &b->base, &c->base) == EMBI_DOM_OK);
    EMB_CHECK(a->base.next == &b->base);
    EMB_CHECK(b->base.next == &c->base);

    /* Inserting a node before itself moves it to where it would have been. */
    EMB_CHECK(ins(&ul->base, &b->base, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(a->base.next == &b->base);
    EMB_CHECK(b->base.next == &c->base);

    embi_element *x = make(d, S("x"));
    EMB_CHECK(embi_replace_child(&ul->base, &x->base, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(a->base.next == &x->base);
    EMB_CHECK(x->base.next == &c->base);
    EMB_CHECK(b->base.parent == nullptr);
    EMB_CHECK(embi_replace_child(&ul->base, &x->base, &b->base) == EMBI_DOM_ERR_NOT_FOUND);

    EMB_CHECK(embi_remove_child(&ul->base, &x->base) == EMBI_DOM_OK);
    EMB_CHECK(x->base.parent == nullptr);
    EMB_CHECK(child_count(&ul->base) == 2);

    /* Moving a node that is already a child keeps the tree consistent. */
    EMB_CHECK(app(&ul->base, &a->base) == EMBI_DOM_OK);
    EMB_CHECK(ul->base.first_child == &c->base);
    EMB_CHECK(ul->base.last_child == &a->base);
    EMB_CHECK(child_count(&ul->base) == 2);

    embi_detach(&a->base);
    embi_detach(&a->base);
    EMB_CHECK(child_count(&ul->base) == 1);
    embi_document_free(d);
}

EMB_TEST(tree_fragment_insert_moves_children)
{
    embi_document *d = embi_document_new(false);
    embi_element *div = make(d, S("div"));
    embi_element *end = make(d, S("end"));
    EMB_CHECK(app(&div->base, &end->base) == EMBI_DOM_OK);
    embi_node *frag = embi_new_fragment(d);
    embi_element *p = make(d, S("p"));
    embi_element *q = make(d, S("q"));
    embi_element *r = make(d, S("r"));
    EMB_CHECK(app(frag, &p->base) == EMBI_DOM_OK);
    EMB_CHECK(app(frag, &q->base) == EMBI_DOM_OK);
    EMB_CHECK(app(frag, &r->base) == EMBI_DOM_OK);
    EMB_CHECK(ins(&div->base, frag, &end->base) == EMBI_DOM_OK);
    EMB_CHECK(child_count(frag) == 0);
    EMB_CHECK(div->base.first_child == &p->base);
    EMB_CHECK(p->base.next == &q->base);
    EMB_CHECK(q->base.next == &r->base);
    EMB_CHECK(r->base.next == &end->base);
    EMB_CHECK(p->base.parent == &div->base);
    embi_document_free(d);
}

EMB_TEST(tree_replace_all_clears_children)
{
    embi_document *d = embi_document_new(false);
    embi_element *div = make(d, S("div"));
    EMB_CHECK(app(&div->base, &make(d, S("a"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, tn(d, "x")) == EMBI_DOM_OK);
    EMB_CHECK(embi_replace_all(&div->base, nullptr) == EMBI_DOM_OK);
    EMB_CHECK(child_count(&div->base) == 0);
    EMB_CHECK(embi_replace_all(&div->base, tn(d, "y")) == EMBI_DOM_OK);
    EMB_CHECK(child_count(&div->base) == 1);
    embi_document_free(d);
}

EMB_TEST(tree_adopt_moves_subtree_and_keeps_memory)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_element *div = make(a, S("div"));
    embi_element *span = make(a, S("span"));
    EMB_CHECK(app(&div->base, &span->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(span, S("title"), embi_dstr_from_utf8(a->slab, S("kept"))) ==
              EMBI_DOM_OK);
    embi_element *body = make(b, S("body"));
    EMB_CHECK(app(&b->base, &body->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&body->base, &div->base) == EMBI_DOM_OK);
    EMB_CHECK(div->base.doc == b);
    EMB_CHECK(span->base.doc == b);
    EMB_CHECK(div->base.parent == &body->base);

    /* The adopted nodes live in a's slab. Freeing a must not free them. */
    embi_document_free(a);
    const embi_attr *at = embi_el_get_attribute(span, S("title"));
    EMB_REQUIRE(at != nullptr);
    EMB_CHECK(embi_dstr_eq_utf8(at->value, S("kept")));
    EMB_CHECK(embi_get_root(&span->base) == &b->base);
    embi_document_free(b);
}

EMB_TEST(tree_adopt_node_detaches_then_moves)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_element *html = make(a, S("html"));
    EMB_CHECK(app(&a->base, &html->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_adopt_node(b, &html->base) == EMBI_DOM_OK);
    EMB_CHECK(html->base.parent == nullptr);
    EMB_CHECK(html->base.doc == b);
    EMB_CHECK(embi_adopt_node(b, &b->base) == EMBI_DOM_ERR_NOT_SUPPORTED);
    embi_document_free(a);
    EMB_CHECK(app(&b->base, &html->base) == EMBI_DOM_OK);
    embi_document_free(b);
}

EMB_TEST(tree_text_content_get_and_set)
{
    embi_document *d = embi_document_new(false);
    embi_element *div = make(d, S("div"));
    embi_element *span = make(d, S("span"));
    EMB_CHECK(app(&div->base, tn(d, "a")) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, &span->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&span->base, tn(d, "b")) == EMBI_DOM_OK);
    EMB_CHECK(app(&span->base, &embi_new_comment(d, embi_dstr_empty())->base) == EMBI_DOM_OK);
    char buf[64];
    EMB_CHECK(text_of(&div->base, buf, sizeof buf));
    EMB_CHECK(SDL_strcmp(buf, "ab") == 0);

    embi_dstr v = embi_dstr_from_utf8(d->slab, S("hi\xE2\x82\xAC"));
    EMB_CHECK(embi_text_content_set(&div->base, v) == EMBI_DOM_OK);
    embi_dstr_free(v);
    EMB_CHECK(child_count(&div->base) == 1);
    EMB_CHECK(text_of(&div->base, buf, sizeof buf));
    EMB_CHECK(embi_dstr_eq_utf8(embi_dstr_from_utf8(d->slab, S("hi\xE2\x82\xAC")),
                                S("hi\xE2\x82\xAC")));
    EMB_CHECK(SDL_strcmp(buf, "hi\xE2\x82\xAC") == 0);

    EMB_CHECK(embi_text_content_set(&div->base, embi_dstr_empty()) == EMBI_DOM_OK);
    EMB_CHECK(child_count(&div->base) == 0);

    EMB_CHECK(!text_of(&d->base, buf, sizeof buf));
    embi_document_free(d);
}

EMB_TEST(tree_normalize_merges_and_drops_empty_text)
{
    embi_document *d = embi_document_new(false);
    embi_element *div = make(d, S("div"));
    EMB_CHECK(app(&div->base, tn(d, "a")) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, tn(d, "")) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, tn(d, "b")) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, &make(d, S("i"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, tn(d, "")) == EMBI_DOM_OK);
    EMB_CHECK(app(&div->base, tn(d, "c")) == EMBI_DOM_OK);
    embi_normalize(&div->base);
    EMB_CHECK(child_count(&div->base) == 3);
    char buf[16];
    EMB_CHECK(text_of(div->base.first_child, buf, sizeof buf));
    EMB_CHECK(SDL_strcmp(buf, "ab") == 0);
    EMB_CHECK(div->base.first_child->next->type == EMBI_NODE_ELEMENT);
    EMB_CHECK(text_of(div->base.last_child, buf, sizeof buf));
    EMB_CHECK(SDL_strcmp(buf, "c") == 0);
    embi_document_free(d);
}

static embi_element *build_sample(embi_document *d)
{
    embi_element *root = make(d, S("section"));
    embi_element *p = make(d, S("p"));
    EMB_CHECK(app(&root->base, &p->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(p, S("class"), embi_dstr_from_utf8(d->slab, S("a b"))) ==
              EMBI_DOM_OK);
    EMB_CHECK(app(&p->base, tn(d, "hello")) == EMBI_DOM_OK);
    embi_element *img = make(d, S("img"));
    EMB_CHECK(app(&root->base, &img->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(img, S("src"), embi_dstr_from_utf8(d->slab, S("x.png"))) ==
              EMBI_DOM_OK);
    return root;
}

EMB_TEST(tree_clone_deep_and_shallow)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = build_sample(d);
    embi_node *shallow = embi_clone_node(&root->base, false);
    EMB_REQUIRE(shallow != nullptr);
    EMB_CHECK(shallow->parent == nullptr);
    EMB_CHECK(shallow->first_child == nullptr);
    EMB_CHECK(shallow->doc == d);
    EMB_CHECK(!embi_is_equal_node(shallow, &root->base));

    embi_node *deep = embi_clone_node(&root->base, true);
    EMB_REQUIRE(deep != nullptr);
    EMB_CHECK(deep != &root->base);
    EMB_CHECK(embi_is_equal_node(deep, &root->base));
    EMB_CHECK(child_count(deep) == 2);
    EMB_CHECK(deep->first_child->first_child->type == EMBI_NODE_TEXT);
    EMB_CHECK(deep->first_child->first_child != root->base.first_child->first_child);

    /* Changing the copy leaves the original alone. */
    EMB_CHECK(embi_el_set_attribute((embi_element *)deep->first_child, S("class"),
                                    embi_dstr_from_utf8(d->slab, S("z"))) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(deep, &root->base));
    const embi_attr *orig = embi_el_get_attribute((embi_element *)root->base.first_child, S("class"));
    EMB_REQUIRE(orig != nullptr);
    EMB_CHECK(embi_dstr_eq_utf8(orig->value, S("a b")));

    embi_destroy_detached(deep);
    embi_destroy_detached(shallow);
    embi_document_free(d);
}

EMB_TEST(tree_clone_document_and_import)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = build_sample(d);
    EMB_CHECK(app(&d->base, &root->base) == EMBI_DOM_OK);

    embi_node *copy = embi_clone_node(&d->base, true);
    EMB_REQUIRE(copy != nullptr);
    EMB_CHECK(copy->type == EMBI_NODE_DOCUMENT);
    EMB_CHECK(copy != &d->base);
    EMB_CHECK(child_count(copy) == 1);
    EMB_CHECK(embi_is_equal_node(copy, &d->base));
    embi_document *d2 = (embi_document *)copy;

    embi_document *other = embi_document_new(true);
    EMB_CHECK(embi_import_node(other, &d->base, true) == nullptr);
    embi_node *imported = embi_import_node(other, &root->base, true);
    EMB_REQUIRE(imported != nullptr);
    EMB_CHECK(imported->doc == other);
    EMB_CHECK(embi_is_equal_node(imported, &root->base));
    embi_document_free(other);
    embi_document_free(d2);
    embi_document_free(d);
}

EMB_TEST(tree_clone_template_contents_are_copied)
{
    embi_document *d = embi_document_new(true);
    embi_element *tpl = make(d, S("template"));
    embi_node *contents = embi_template_contents(tpl);
    EMB_CHECK(contents->doc != d);
    EMB_CHECK(contents->doc->inert);
    EMB_CHECK(app(contents, &make(contents->doc, S("li"))->base) == EMBI_DOM_OK);
    EMB_CHECK(child_count(contents) == 1);

    embi_node *copy = embi_clone_node(&tpl->base, true);
    EMB_REQUIRE(copy != nullptr);
    embi_node *copy_contents = ((embi_element *)copy)->attrs->content;
    EMB_REQUIRE(copy_contents != nullptr);
    EMB_CHECK(copy_contents != contents);
    EMB_CHECK(child_count(copy_contents) == 1);
    EMB_CHECK(embi_is_equal_node(copy_contents, contents));

    embi_node *shallow = embi_clone_node(&tpl->base, false);
    EMB_CHECK(((embi_element *)shallow)->attrs == nullptr);

    embi_destroy_detached(copy);
    embi_destroy_detached(shallow);
    embi_document_free(d);
}

EMB_TEST(tree_compare_position_and_contains)
{
    embi_document *d = embi_document_new(false);
    embi_element *root = make(d, S("root"));
    embi_element *a = make(d, S("a"));
    embi_element *b = make(d, S("b"));
    embi_element *a1 = make(d, S("a1"));
    EMB_CHECK(app(&root->base, &a->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&root->base, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&a->base, &a1->base) == EMBI_DOM_OK);

    EMB_CHECK(embi_compare_position(&a->base, &a->base) == 0);
    EMB_CHECK(embi_compare_position(&root->base, &a1->base) ==
              (EMBI_POS_CONTAINED_BY | EMBI_POS_FOLLOWING));
    EMB_CHECK(embi_compare_position(&a1->base, &root->base) ==
              (EMBI_POS_CONTAINS | EMBI_POS_PRECEDING));
    EMB_CHECK(embi_compare_position(&a->base, &b->base) == EMBI_POS_FOLLOWING);
    EMB_CHECK(embi_compare_position(&b->base, &a->base) == EMBI_POS_PRECEDING);
    EMB_CHECK(embi_compare_position(&a1->base, &b->base) == EMBI_POS_FOLLOWING);
    EMB_CHECK(embi_compare_position(&b->base, &a1->base) == EMBI_POS_PRECEDING);

    embi_document *other = embi_document_new(false);
    embi_element *lone = make(other, S("lone"));
    uint16_t pos = embi_compare_position(&lone->base, &a->base);
    EMB_CHECK((pos & EMBI_POS_DISCONNECTED) != 0);
    EMB_CHECK((pos & EMBI_POS_IMPL_SPECIFIC) != 0);

    EMB_CHECK(embi_contains(&root->base, &a1->base));
    EMB_CHECK(embi_contains(&a->base, &a->base));
    EMB_CHECK(!embi_contains(&a->base, &b->base));
    EMB_CHECK(!embi_contains(&a1->base, &root->base));
    embi_document_free(other);
    embi_document_free(d);
}

EMB_TEST(tree_is_equal_node_structure_and_attributes)
{
    embi_document *d = embi_document_new(true);
    embi_element *x = build_sample(d);
    embi_element *y = build_sample(d);
    EMB_CHECK(embi_is_equal_node(&x->base, &y->base));

    /* Attribute order does not matter. */
    embi_element *e1 = make(d, S("e"));
    embi_element *e2 = make(d, S("e"));
    EMB_CHECK(embi_el_set_attribute(e1, S("a"), embi_dstr_from_utf8(d->slab, S("1"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(e1, S("b"), embi_dstr_from_utf8(d->slab, S("2"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(e2, S("b"), embi_dstr_from_utf8(d->slab, S("2"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute(e2, S("a"), embi_dstr_from_utf8(d->slab, S("1"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_is_equal_node(&e1->base, &e2->base));
    EMB_CHECK(embi_el_set_attribute(e2, S("a"), embi_dstr_from_utf8(d->slab, S("9"))) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&e1->base, &e2->base));

    /* Child order matters. */
    embi_element *p1 = make(d, S("p"));
    embi_element *p2 = make(d, S("p"));
    EMB_CHECK(app(&p1->base, &make(d, S("a"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&p1->base, &make(d, S("b"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&p2->base, &make(d, S("b"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&p2->base, &make(d, S("a"))->base) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&p1->base, &p2->base));

    /* Different element names never match. */
    EMB_CHECK(!embi_is_equal_node(&make(d, S("q"))->base, &make(d, S("r"))->base));
    embi_document_free(d);
}

/* isEqualNode must reject extra trailing children at any depth, checked in both argument orders. */
EMB_TEST(tree_is_equal_node_extra_trailing_children)
{
    embi_document *d = embi_document_new(true);
    /* Depth 1: b has an extra trailing child under the root. */
    embi_element *a1 = make(d, S("div"));
    EMB_CHECK(app(&a1->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    embi_element *b1 = make(d, S("div"));
    EMB_CHECK(app(&b1->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&b1->base, &make(d, S("span"))->base) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&a1->base, &b1->base));
    EMB_CHECK(!embi_is_equal_node(&b1->base, &a1->base));

    /* Depth 2: b has an extra trailing child under the first child. */
    embi_element *a2 = make(d, S("div"));
    embi_element *pa = make(d, S("p"));
    EMB_CHECK(app(&a2->base, &pa->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&pa->base, &make(d, S("i"))->base) == EMBI_DOM_OK);
    embi_element *b2 = make(d, S("div"));
    embi_element *pb = make(d, S("p"));
    EMB_CHECK(app(&b2->base, &pb->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&pb->base, &make(d, S("i"))->base) == EMBI_DOM_OK);
    EMB_CHECK(app(&pb->base, &make(d, S("s"))->base) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&a2->base, &b2->base));
    EMB_CHECK(!embi_is_equal_node(&b2->base, &a2->base));

    /* Identical structure is still equal. */
    embi_element *a3 = make(d, S("div"));
    EMB_CHECK(app(&a3->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    embi_element *b3 = make(d, S("div"));
    EMB_CHECK(app(&b3->base, &make(d, S("p"))->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_is_equal_node(&a3->base, &b3->base));
    EMB_CHECK(embi_is_equal_node(&b3->base, &a3->base));
    embi_document_free(d);
}

/* Attributes that differ only in prefix (same namespace, local name and value) are equal. */
EMB_TEST(tree_is_equal_node_ignores_attribute_prefix)
{
    embi_document *d = embi_document_new(true);
    embi_element *e1 = make(d, S("a"));
    embi_element *e2 = make(d, S("a"));
    EMB_CHECK(embi_el_set_attribute_ns(e1, true, S("http://www.w3.org/1999/xlink"), S("xlink:href"),
                                       embi_dstr_from_utf8(d->slab, S("x"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_el_set_attribute_ns(e2, true, S("http://www.w3.org/1999/xlink"), S("foo:href"),
                                       embi_dstr_from_utf8(d->slab, S("x"))) == EMBI_DOM_OK);
    EMB_CHECK(embi_is_equal_node(&e1->base, &e2->base));
    EMB_CHECK(embi_is_equal_node(&e2->base, &e1->base));
    EMB_CHECK(embi_el_set_attribute_ns(e2, true, S("http://www.w3.org/1999/xlink"), S("foo:href"),
                                       embi_dstr_from_utf8(d->slab, S("y"))) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&e1->base, &e2->base));
    embi_document_free(d);
}

/* The template element is the host of its contents, so it counts as an ancestor of them. Putting
 * the template, or any of its ancestors, into its own contents is a hierarchy error. */
EMB_TEST(tree_template_host_counts_as_ancestor)
{
    embi_document *d = embi_document_new(true);
    embi_element *root = make(d, S("div"));
    EMB_CHECK(app(&d->base, &root->base) == EMBI_DOM_OK);
    embi_element *tpl = make(d, S("template"));
    EMB_CHECK(app(&root->base, &tpl->base) == EMBI_DOM_OK);
    embi_node *contents = embi_template_contents(tpl);
    EMB_CHECK(app(contents, &tpl->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(contents, &root->base) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(ins(contents, &tpl->base, nullptr) == EMBI_DOM_ERR_HIERARCHY);
    EMB_CHECK(app(contents, &make(contents->doc, S("li"))->base) == EMBI_DOM_OK);
    /* Nothing moved. */
    EMB_CHECK(tpl->base.parent == &root->base);
    EMB_CHECK(root->base.parent == &d->base);
    EMB_CHECK_EQ_INT(child_count_of(contents), 1);
    embi_document_free(d);
}

EMB_TEST(tree_processing_instruction_and_doctype_validation)
{
    embi_document *d = embi_document_new(false);
    embi_chardata *pi = nullptr;
    EMB_CHECK(embi_dom_create_pi(d, S("xml-stylesheet"), embi_dstr_from_utf8(d->slab, S("a=b")),
                                 &pi) == EMBI_DOM_OK);
    EMB_CHECK(pi->base.type == EMBI_NODE_PI);
    EMB_CHECK(pi->base.name == embi_atom_intern("xml-stylesheet", 14));
    EMB_CHECK(embi_dom_create_pi(d, S("1bad"), embi_dstr_empty(), &pi) ==
              EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_dom_create_pi(d, S("ok"), embi_dstr_from_utf8(d->slab, S("x?>y")), &pi) ==
              EMBI_DOM_ERR_INVALID_CHAR);

    embi_doctype *dt = nullptr;
    EMB_CHECK(embi_dom_create_doctype(d, S("a b"), S(""), S(""), &dt) == EMBI_DOM_ERR_INVALID_CHAR);
    EMB_CHECK(embi_dom_create_doctype(d, S("html"), S("-//W3C//DTD"), S("x.dtd"), &dt) ==
              EMBI_DOM_OK);
    EMB_CHECK(embi_dstr_eq_utf8(dt->public_id, S("-//W3C//DTD")));
    EMB_CHECK(embi_dstr_eq_utf8(dt->system_id, S("x.dtd")));
    embi_document_free(d);
}

EMB_TEST(tree_next_in_tree_visits_subtree_only)
{
    embi_document *d = embi_document_new(false);
    embi_element *root = build_sample(d);
    EMB_CHECK(app(&d->base, &root->base) == EMBI_DOM_OK);
    uint32_t count = 0;
    for (embi_node *n = root->base.first_child; n; n = embi_next_in_tree(&root->base, n)) {
        count++;
    }
    /* p, text "hello", img. */
    EMB_CHECK_EQ_INT(count, 3);
    EMB_CHECK(embi_next_in_tree(&root->base, root->base.last_child) == nullptr);
    embi_document_free(d);
}

/* ---- Document validity matrix (DOM Standard "ensure pre-insertion validity" and "replace") ---- */

typedef enum { SH_DT_HTML, SH_HTML, SH_COMMENT_DT_HTML, SH_COUNT } vshape;
typedef enum {
    NK_ELEMENT, NK_DOCTYPE, NK_TEXT, NK_COMMENT, NK_PI,
    NK_FRAG0, NK_FRAG1, NK_FRAG2, NK_FRAG_TEXT, NK_FRAG1_TEXT, NK_FRAG_COMMENT_PI,
    NK_COUNT
} vnode;
typedef enum { CH_NULL, CH_DOCTYPE, CH_HTML, CH_COMMENT, CH_STRAY, CH_COUNT } vchild;

static const char *const shape_names[] = {"[doctype,html]", "[html]", "[comment,doctype,html]"};
static const char *const node_names[] = {"element", "doctype", "text", "comment", "PI", "frag0",
                                         "frag1", "frag2", "frag(text)", "frag(el,text)",
                                         "frag(comment,PI)"};
static const char *const child_names[] = {"null", "doctype", "html", "comment", "stray"};

typedef struct vctx {
    embi_document *d;
    embi_node *kids[3]; /* document children in order */
    uint32_t nkids;
    embi_node *doctype; /* document children by kind, nullptr when absent */
    embi_node *html;
    embi_node *comment;
} vctx;

static void v_build(vctx *c, vshape shape)
{
    c->d = embi_document_new(true);
    c->nkids = 0;
    c->doctype = c->html = c->comment = nullptr;
    embi_doctype *dt = nullptr;
    EMB_CHECK(embi_dom_create_doctype(c->d, S("html"), S(""), S(""), &dt) == EMBI_DOM_OK);
    embi_element *html = make(c->d, S("html"));
    embi_chardata *cm = embi_new_comment(c->d, embi_dstr_empty());
    if (shape == SH_COMMENT_DT_HTML) {
        c->comment = &cm->base;
        c->kids[c->nkids++] = c->comment;
    }
    if (shape != SH_HTML) {
        c->doctype = &dt->base;
        c->kids[c->nkids++] = c->doctype;
    }
    c->html = &html->base;
    c->kids[c->nkids++] = c->html;
    for (uint32_t i = 0; i < c->nkids; i++) {
        EMB_CHECK(app(&c->d->base, c->kids[i]) == EMBI_DOM_OK);
    }
}

static embi_node *v_make_node(vctx *c, vnode k)
{
    embi_document *d = c->d;
    embi_node *f = nullptr;
    switch (k) {
    case NK_ELEMENT:
        return &make(d, S("div"))->base;
    case NK_DOCTYPE: {
        embi_doctype *dt = nullptr;
        EMB_CHECK(embi_dom_create_doctype(d, S("x"), S(""), S(""), &dt) == EMBI_DOM_OK);
        return &dt->base;
    }
    case NK_TEXT:
        return tn(d, "t");
    case NK_COMMENT:
        return &embi_new_comment(d, embi_dstr_empty())->base;
    case NK_PI: {
        embi_chardata *pi = nullptr;
        EMB_CHECK(embi_dom_create_pi(d, S("pi"), embi_dstr_empty(), &pi) == EMBI_DOM_OK);
        return &pi->base;
    }
    case NK_FRAG0:
        return embi_new_fragment(d);
    case NK_FRAG1:
        f = embi_new_fragment(d);
        EMB_CHECK(app(f, &make(d, S("a"))->base) == EMBI_DOM_OK);
        return f;
    case NK_FRAG2:
        f = embi_new_fragment(d);
        EMB_CHECK(app(f, &make(d, S("a"))->base) == EMBI_DOM_OK);
        EMB_CHECK(app(f, &make(d, S("b"))->base) == EMBI_DOM_OK);
        return f;
    case NK_FRAG_TEXT:
        f = embi_new_fragment(d);
        EMB_CHECK(app(f, tn(d, "t")) == EMBI_DOM_OK);
        return f;
    case NK_FRAG1_TEXT:
        f = embi_new_fragment(d);
        EMB_CHECK(app(f, &make(d, S("a"))->base) == EMBI_DOM_OK);
        EMB_CHECK(app(f, tn(d, "t")) == EMBI_DOM_OK);
        return f;
    case NK_FRAG_COMMENT_PI: {
        embi_chardata *pi = nullptr;
        f = embi_new_fragment(d);
        EMB_CHECK(app(f, &embi_new_comment(d, embi_dstr_empty())->base) == EMBI_DOM_OK);
        EMB_CHECK(embi_dom_create_pi(d, S("pi"), embi_dstr_empty(), &pi) == EMBI_DOM_OK);
        EMB_CHECK(app(f, &pi->base) == EMBI_DOM_OK);
        return f;
    }
    default:
        return nullptr;
    }
}

static embi_node *v_child(vctx *c, vchild k)
{
    switch (k) {
    case CH_DOCTYPE:
        return c->doctype;
    case CH_HTML:
        return c->html;
    case CH_COMMENT:
        return c->comment;
    case CH_STRAY:
        return &make(c->d, S("stray"))->base; /* detached: parent is not the document */
    default:
        return nullptr;
    }
}

/* The spec algorithm, written step by step from the DOM Standard text (parent is a Document;
 * the parent type, host-including ancestor and node type checks hold for every test shape).
 * `child` is nullptr only for pre-insert. */
static embi_dom_err v_oracle(const vctx *c, const embi_node *node, const embi_node *child,
                             bool replace)
{
    if (child && child->parent != &c->d->base) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    if (node->type == EMBI_NODE_TEXT) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    bool has_other_element = false;  /* parent has an element child [that is not child] */
    bool has_other_doctype = false;  /* parent has a doctype child [that is not child] */
    bool has_element = false;        /* parent has an element child */
    bool doctype_after_child = false;
    bool element_before_child = false;
    bool child_is_doctype = child && child->type == EMBI_NODE_DOCTYPE;
    bool seen_child = false;
    for (const embi_node *x = c->d->base.first_child; x; x = x->next) {
        bool excluded = replace && x == child;
        if (x->type == EMBI_NODE_ELEMENT) {
            has_element = true;
            has_other_element = has_other_element || !excluded;
            element_before_child = element_before_child || (child && !seen_child && x != child);
        }
        if (x->type == EMBI_NODE_DOCTYPE) {
            has_other_doctype = has_other_doctype || !excluded;
            doctype_after_child = doctype_after_child || (child && seen_child && x != child);
        }
        seen_child = seen_child || x == child;
    }
    if (node->type == EMBI_NODE_FRAGMENT) {
        uint32_t elements = 0;
        for (const embi_node *x = node->first_child; x; x = x->next) {
            elements += x->type == EMBI_NODE_ELEMENT;
            if (x->type == EMBI_NODE_TEXT) {
                return EMBI_DOM_ERR_HIERARCHY;
            }
        }
        if (elements > 1) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        if (elements == 1 && (has_other_element || doctype_after_child ||
                              (!replace && child_is_doctype))) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        return EMBI_DOM_OK;
    }
    if (node->type == EMBI_NODE_ELEMENT) {
        if (has_other_element || doctype_after_child || (!replace && child_is_doctype)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        return EMBI_DOM_OK;
    }
    if (node->type == EMBI_NODE_DOCTYPE) {
        if (has_other_doctype || (child && element_before_child) || (!child && has_element)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
    }
    return EMBI_DOM_OK;
}

static bool v_same_children(const embi_node *p, embi_node *const *want, uint32_t n)
{
    uint32_t i = 0;
    const embi_node *prev = nullptr;
    for (const embi_node *x = p->first_child; x; x = x->next, i++) {
        if (i >= n || x != want[i] || x->prev != prev || x->parent != p) {
            return false;
        }
        prev = x;
    }
    return i == n && p->last_child == prev;
}

static void v_run(vshape shape, vnode nk, vchild ck, bool replace)
{
    if (replace && ck == CH_NULL) {
        return;
    }
    vctx c;
    v_build(&c, shape);
    embi_node *child = v_child(&c, ck);
    if (!child && ck != CH_NULL) {
        embi_document_free(c.d);
        return; /* this shape has no such child */
    }
    embi_node *node = v_make_node(&c, nk);
    EMB_REQUIRE(node != nullptr);

    embi_node *frag_kids[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t nfrag = 0;
    for (embi_node *x = node->first_child; x && nfrag < 4; x = x->next) {
        frag_kids[nfrag++] = x;
    }
    embi_dom_err want = v_oracle(&c, node, child, replace);
    embi_dom_err got = replace ? embi_replace_child(&c.d->base, node, child)
                               : embi_pre_insert(&c.d->base, node, child);
    if (got != want) {
        SDL_Log("validity matrix: doc %s, %s node %s, child %s: got %d, want %d",
                shape_names[shape], replace ? "replace with" : "insert", node_names[nk],
                child_names[ck], (int)got, (int)want);
        EMB_CHECK(got == want);
    }
    if (got != EMBI_DOM_OK) {
        bool same = v_same_children(&c.d->base, c.kids, c.nkids) && node->parent == nullptr &&
                    v_same_children(node, frag_kids, nfrag);
        if (ck == CH_STRAY) {
            same = same && child->parent == nullptr;
        }
        if (!same) {
            SDL_Log("validity matrix: tree changed after rejected %s (doc %s, node %s, child %s)",
                    replace ? "replace" : "insert", shape_names[shape], node_names[nk],
                    child_names[ck]);
            EMB_CHECK(same);
        }
    } else {
        /* A success must leave a valid document: at most one element and one doctype, the
         * doctype before the element, no text. */
        uint32_t els = 0;
        uint32_t dts = 0;
        bool bad = false;
        for (const embi_node *x = c.d->base.first_child; x; x = x->next) {
            els += x->type == EMBI_NODE_ELEMENT;
            if (x->type == EMBI_NODE_DOCTYPE) {
                dts++;
                bad = bad || els != 0;
            }
            bad = bad || x->type == EMBI_NODE_TEXT || x->type == EMBI_NODE_FRAGMENT;
        }
        EMB_CHECK(!bad && els <= 1 && dts <= 1);
        if (node->type != EMBI_NODE_FRAGMENT) {
            EMB_CHECK(node->parent == &c.d->base);
        } else {
            EMB_CHECK(node->first_child == nullptr);
        }
        if (replace && child != node) {
            EMB_CHECK(child->parent == nullptr);
        }
    }
    embi_document_free(c.d);
}

EMB_TEST(tree_document_validity_matrix)
{
    for (int s = 0; s < SH_COUNT; s++) {
        for (int n = 0; n < NK_COUNT; n++) {
            for (int ch = 0; ch < CH_COUNT; ch++) {
                v_run((vshape)s, (vnode)n, (vchild)ch, false);
                v_run((vshape)s, (vnode)n, (vchild)ch, true);
            }
        }
    }
}

/* Hand-derived expectations, independent of the oracle above. */
EMB_TEST(tree_document_validity_known_cases)
{
    static const struct {
        vshape shape;
        vnode node;
        vchild child;
        bool replace;
        embi_dom_err want;
    } rows[] = {
        /* [doctype, html] */
        {SH_DT_HTML, NK_ELEMENT, CH_NULL, false, EMBI_DOM_ERR_HIERARCHY},   /* second element */
        {SH_DT_HTML, NK_ELEMENT, CH_HTML, true, EMBI_DOM_OK},               /* html -> div */
        {SH_DT_HTML, NK_ELEMENT, CH_DOCTYPE, true, EMBI_DOM_ERR_HIERARCHY}, /* html stays */
        {SH_DT_HTML, NK_DOCTYPE, CH_HTML, false, EMBI_DOM_ERR_HIERARCHY},   /* second doctype */
        {SH_DT_HTML, NK_DOCTYPE, CH_DOCTYPE, true, EMBI_DOM_OK},            /* doctype -> doctype */
        {SH_DT_HTML, NK_DOCTYPE, CH_HTML, true, EMBI_DOM_ERR_HIERARCHY},    /* other doctype stays */
        {SH_DT_HTML, NK_TEXT, CH_NULL, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_DT_HTML, NK_TEXT, CH_HTML, true, EMBI_DOM_ERR_HIERARCHY},
        {SH_DT_HTML, NK_COMMENT, CH_NULL, false, EMBI_DOM_OK},
        {SH_DT_HTML, NK_COMMENT, CH_DOCTYPE, false, EMBI_DOM_OK},
        {SH_DT_HTML, NK_PI, CH_HTML, true, EMBI_DOM_OK},
        {SH_DT_HTML, NK_FRAG0, CH_NULL, false, EMBI_DOM_OK},
        {SH_DT_HTML, NK_FRAG1, CH_HTML, true, EMBI_DOM_OK},
        {SH_DT_HTML, NK_FRAG1, CH_HTML, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_DT_HTML, NK_FRAG2, CH_HTML, true, EMBI_DOM_ERR_HIERARCHY},
        {SH_DT_HTML, NK_FRAG_TEXT, CH_NULL, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_DT_HTML, NK_FRAG_COMMENT_PI, CH_HTML, false, EMBI_DOM_OK},
        {SH_DT_HTML, NK_ELEMENT, CH_STRAY, false, EMBI_DOM_ERR_NOT_FOUND},
        {SH_DT_HTML, NK_TEXT, CH_STRAY, true, EMBI_DOM_ERR_NOT_FOUND},
        /* [html] */
        {SH_HTML, NK_DOCTYPE, CH_NULL, false, EMBI_DOM_ERR_HIERARCHY},      /* after the element */
        {SH_HTML, NK_DOCTYPE, CH_HTML, false, EMBI_DOM_OK},                 /* before the element */
        {SH_HTML, NK_DOCTYPE, CH_HTML, true, EMBI_DOM_OK},                  /* replaces the element */
        {SH_HTML, NK_ELEMENT, CH_HTML, true, EMBI_DOM_OK},
        {SH_HTML, NK_ELEMENT, CH_HTML, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_HTML, NK_FRAG1, CH_NULL, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_HTML, NK_FRAG0, CH_HTML, true, EMBI_DOM_OK},
        {SH_HTML, NK_FRAG1_TEXT, CH_HTML, true, EMBI_DOM_ERR_HIERARCHY},
        /* [comment, doctype, html] */
        {SH_COMMENT_DT_HTML, NK_DOCTYPE, CH_COMMENT, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_COMMENT_DT_HTML, NK_DOCTYPE, CH_COMMENT, true, EMBI_DOM_ERR_HIERARCHY},
        {SH_COMMENT_DT_HTML, NK_COMMENT, CH_COMMENT, true, EMBI_DOM_OK},
        {SH_COMMENT_DT_HTML, NK_ELEMENT, CH_COMMENT, false, EMBI_DOM_ERR_HIERARCHY},
        {SH_COMMENT_DT_HTML, NK_ELEMENT, CH_DOCTYPE, true, EMBI_DOM_ERR_HIERARCHY},
        {SH_COMMENT_DT_HTML, NK_FRAG1, CH_COMMENT, true, EMBI_DOM_ERR_HIERARCHY},
        {SH_COMMENT_DT_HTML, NK_FRAG0, CH_NULL, false, EMBI_DOM_OK},
        {SH_COMMENT_DT_HTML, NK_FRAG_COMMENT_PI, CH_COMMENT, false, EMBI_DOM_OK},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        vctx c;
        v_build(&c, rows[i].shape);
        embi_node *child = v_child(&c, rows[i].child);
        embi_node *node = v_make_node(&c, rows[i].node);
        EMB_REQUIRE(node != nullptr);
        embi_dom_err got = rows[i].replace ? embi_replace_child(&c.d->base, node, child)
                                           : embi_pre_insert(&c.d->base, node, child);
        if (got != rows[i].want) {
            SDL_Log("known case %u failed: got %d, want %d", (unsigned)i, (int)got, (int)rows[i].want);
        }
        EMB_CHECK(got == rows[i].want);
        embi_document_free(c.d);
    }
}

EMB_TEST(tree_prev_in_tree_and_next_skipping_children)
{
    embi_document *d = embi_document_new(false);
    embi_element *root = build_sample(d); /* section(p("hello"), img) */
    EMB_CHECK(app(&d->base, &root->base) == EMBI_DOM_OK);
    embi_node *r = &root->base;
    embi_node *p = r->first_child;
    embi_node *hello = p->first_child;
    embi_node *img = p->next;
    EMB_CHECK(embi_prev_in_tree(r, r) == nullptr);
    EMB_CHECK(embi_prev_in_tree(r, p) == r);
    EMB_CHECK(embi_prev_in_tree(r, hello) == p);
    EMB_CHECK(embi_prev_in_tree(r, img) == hello); /* deepest last descendant of the sibling */
    EMB_CHECK(embi_prev_in_tree(&d->base, r) == &d->base);
    EMB_CHECK(embi_prev_in_tree(&d->base, &d->base) == nullptr);
    /* Walking backwards visits exactly what the forward walk visits. */
    embi_node *fwd[8];
    uint32_t n = 0;
    for (embi_node *x = r; x; x = embi_next_in_tree(r, x)) {
        fwd[n++] = x;
    }
    EMB_CHECK_EQ_INT(n, 4);
    uint32_t i = n;
    for (embi_node *x = img; x; x = embi_prev_in_tree(r, x)) {
        EMB_CHECK(i > 0 && x == fwd[--i]);
    }
    EMB_CHECK_EQ_INT(i, 0);

    EMB_CHECK(embi_next_skipping_children(r, p) == img);
    EMB_CHECK(embi_next_skipping_children(r, hello) == img);
    EMB_CHECK(embi_next_skipping_children(r, img) == nullptr);
    EMB_CHECK(embi_next_skipping_children(r, r) == nullptr);
    EMB_CHECK(embi_next_skipping_children(p, p) == nullptr);
    EMB_CHECK(embi_next_skipping_children(p, hello) == nullptr);
    embi_document_free(d);
}

EMB_TEST_MAIN()
