#include "emb_test.h"

#include "dom/dom.h"
#include "dom/internal.h"

#define S(lit) EMBI_STR(lit)

static embi_element *make(embi_document *d, embi_str name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, name, &e) == EMBI_DOM_OK);
    return e;
}

static embi_dstr v8(embi_document *d, const char *utf8)
{
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    return embi_dstr_from_utf8(d->slab, s);
}

/* Sets an attribute from a temporary value and releases the temporary. */
static void set_attr(embi_document *d, embi_element *el, const char *name, const char *value)
{
    embi_str n = {name, (uint32_t)SDL_strlen(name)};
    embi_dstr v = v8(d, value);
    EMB_CHECK(embi_el_set_attribute(el, n, v) == EMBI_DOM_OK);
    embi_dstr_free(v);
}

static uint32_t child_count_of(const embi_node *n)
{
    uint32_t k = 0;
    for (const embi_node *c = n->first_child; c; c = c->next) {
        k++;
    }
    return k;
}

/* One document, adopted down a chain of three documents and freed in order. */
static void adopt_chain_cycle(void)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_document *c = embi_document_new(true);
    embi_element *html = make(a, S("html"));
    EMB_CHECK(embi_append_child(&a->base, &html->base) == EMBI_DOM_OK);
    embi_element *div = make(a, S("div"));
    EMB_CHECK(embi_append_child(&html->base, &div->base) == EMBI_DOM_OK);
    set_attr(a, div, "title", "moves");
    EMB_CHECK(embi_append_child(&div->base, &make(a, S("span"))->base) == EMBI_DOM_OK);

    EMB_CHECK(embi_adopt_node(b, &div->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_adopt_node(c, &div->base) == EMBI_DOM_OK);
    /* Insert it into a tree of c (inserting under a's html would adopt it back into a, and
     * freeing a would then free it). */
    embi_element *chtml = make(c, S("html"));
    EMB_CHECK(embi_append_child(&c->base, &chtml->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&chtml->base, &div->base) == EMBI_DOM_OK);
    EMB_CHECK(div->base.doc == c);
    embi_document_free(a);
    const embi_attr *at = embi_el_get_attribute(div, S("title"));
    EMB_REQUIRE(at != nullptr);
    EMB_CHECK(embi_dstr_eq_utf8(at->value, S("moves")));
    embi_document_free(c);
    embi_document_free(b);
}

/* Builds a document with about 500 nodes using every node kind, then frees it. The names and
 * values are fixed so that a second run interns nothing new. */
static void build_and_free_document(void)
{
    embi_document *d = embi_document_new(true);
    embi_element *html = make(d, S("html"));
    embi_dstr dt_name = v8(d, "html");
    embi_doctype *dt = embi_new_doctype(d, dt_name, embi_dstr_empty(), embi_dstr_empty());
    embi_dstr_free(dt_name);
    EMB_CHECK(embi_pre_insert(&d->base, &dt->base, nullptr) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&d->base, &html->base) == EMBI_DOM_OK);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&html->base, &body->base) == EMBI_DOM_OK);
    for (int i = 0; i < 100; i++) {
        embi_element *div = make(d, S("div"));
        EMB_CHECK(embi_append_child(&body->base, &div->base) == EMBI_DOM_OK);
        EMB_CHECK(embi_el_set_attribute(div, S("class"), v8(d, "row item")) == EMBI_DOM_OK);
        EMB_CHECK(embi_el_set_attribute(div, S("id"), v8(d, "row")) == EMBI_DOM_OK);
        EMB_CHECK(embi_el_set_attribute(div, S("data-n"), v8(d, "42")) == EMBI_DOM_OK);
        embi_chardata *t = embi_new_text(d, v8(d, "text"));
        EMB_CHECK(embi_append_child(&div->base, &t->base) == EMBI_DOM_OK);
        embi_element *span = make(d, S("span"));
        EMB_CHECK(embi_append_child(&div->base, &span->base) == EMBI_DOM_OK);
        embi_node *frag = embi_new_fragment(d);
        EMB_CHECK(embi_append_child(frag, &make(d, S("b"))->base) == EMBI_DOM_OK);
        EMB_CHECK(embi_append_child(frag, &make(d, S("i"))->base) == EMBI_DOM_OK);
        EMB_CHECK(embi_pre_insert(&span->base, frag, nullptr) == EMBI_DOM_OK);
    }
    embi_element *tpl = make(d, S("template"));
    EMB_CHECK(embi_append_child(&body->base, &tpl->base) == EMBI_DOM_OK);
    embi_node *contents = embi_template_contents(tpl);
    EMB_CHECK(embi_append_child(contents, &make(contents->doc, S("li"))->base) == EMBI_DOM_OK);
    embi_node *copy = embi_clone_node(&body->base, true);
    EMB_CHECK(copy != nullptr);
    embi_destroy_detached(copy);
    embi_normalize(&body->base);
    embi_get_element_by_id(d, S("row"));
    embi_document_free(d);
}

EMB_TEST(memory_document_cycles_return_to_baseline)
{
    build_and_free_document();
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        build_and_free_document();
        EMB_CHECK_EQ_INT(embi_mem_live(), base);
    }
}

EMB_TEST(memory_churn_reuses_storage)
{
    embi_document *d = embi_document_new(true);
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &body->base) == EMBI_DOM_OK);
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 20000; i++) {
            embi_element *el = make(d, S("p"));
            EMB_CHECK(embi_append_child(&body->base, &el->base) == EMBI_DOM_OK);
            set_attr(d, el, "title", "t");
            embi_detach(&el->base);
            embi_destroy_detached(&el->base);
        }
    }
    size_t after_warm = embi_mem_live();
    for (int i = 0; i < 20000; i++) {
        embi_element *el = make(d, S("p"));
        EMB_CHECK(embi_append_child(&body->base, &el->base) == EMBI_DOM_OK);
        set_attr(d, el, "title", "t");
        embi_detach(&el->base);
        embi_destroy_detached(&el->base);
    }
    EMB_CHECK_EQ_INT(embi_mem_live(), after_warm);
    EMB_CHECK_EQ_INT(child_count_of(&body->base), 0);
    embi_document_free(d);
}

EMB_TEST(memory_adopt_chain_frees_cleanly)
{
    adopt_chain_cycle();
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        adopt_chain_cycle();
        EMB_CHECK_EQ_INT(embi_mem_live(), base);
    }
}

EMB_TEST(memory_adopt_keeps_template_contents_alive)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_element *tpl = make(a, S("template"));
    EMB_CHECK(embi_append_child(&a->base, &tpl->base) == EMBI_DOM_OK);
    embi_node *contents = embi_template_contents(tpl);
    EMB_CHECK(embi_append_child(contents, &make(contents->doc, S("li"))->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_adopt_node(b, &tpl->base) == EMBI_DOM_OK);
    embi_node *moved = ((embi_element *)tpl)->attrs ? ((embi_element *)tpl)->attrs->content : nullptr;
    EMB_REQUIRE(moved != nullptr);
    EMB_CHECK(moved->doc == embi_document_template_owner(b));
    EMB_CHECK(moved->first_child != nullptr);
    embi_document_free(a);
    EMB_CHECK(moved->first_child != nullptr);
    EMB_CHECK(moved->first_child->type == EMBI_NODE_ELEMENT);
    embi_document_free(b);
}

/* Node x from a lives in a's slab and is moved into b (b keeps a). Node y from b moves into a
 * (a keeps b). Both stay in their new trees, then both documents are freed. */
static void mutual_adopt_cycle(bool free_a_first)
{
    embi_document *a = embi_document_new(true);
    embi_document *b = embi_document_new(true);
    embi_element *ra = make(a, S("div"));
    EMB_CHECK(embi_append_child(&a->base, &ra->base) == EMBI_DOM_OK);
    embi_element *x = make(a, S("span"));
    EMB_CHECK(embi_append_child(&ra->base, &x->base) == EMBI_DOM_OK);
    embi_element *rb = make(b, S("div"));
    EMB_CHECK(embi_append_child(&b->base, &rb->base) == EMBI_DOM_OK);
    embi_element *y = make(b, S("em"));
    EMB_CHECK(embi_append_child(&rb->base, &y->base) == EMBI_DOM_OK);

    EMB_CHECK(embi_adopt_node(b, &x->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&rb->base, &x->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_adopt_node(a, &y->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&ra->base, &y->base) == EMBI_DOM_OK);
    EMB_CHECK(x->base.doc == b);
    EMB_CHECK(y->base.doc == a);

    if (free_a_first) {
        embi_document_free(a);
        embi_document_free(b);
    } else {
        embi_document_free(b);
        embi_document_free(a);
    }
}

EMB_TEST(memory_mutual_adopt_cycle_frees_both)
{
    mutual_adopt_cycle(true);
    mutual_adopt_cycle(false);
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        mutual_adopt_cycle(i % 2 == 0);
        EMB_CHECK_EQ_INT(embi_mem_live(), base);
    }
}

EMB_TEST(memory_text_content_churn_is_flat)
{
    embi_document *d = embi_document_new(true);
    embi_element *div = make(d, S("div"));
    EMB_CHECK(embi_append_child(&d->base, &div->base) == EMBI_DOM_OK);
    embi_dstr v = v8(d, "some text");
    EMB_CHECK(embi_text_content_set(&div->base, v) == EMBI_DOM_OK);
    size_t warm = embi_slab_live_bytes(d->slab);
    for (int i = 0; i < 10000; i++) {
        EMB_CHECK(embi_text_content_set(&div->base, v) == EMBI_DOM_OK);
    }
    EMB_CHECK_EQ_INT(embi_slab_live_bytes(d->slab), warm);
    embi_dstr_free(v);
    embi_document_free(d);
}

EMB_TEST(memory_replace_all_keeps_wrapped_children)
{
    embi_document *d = embi_document_new(true);
    embi_element *div = make(d, S("div"));
    embi_element *kept = make(d, S("p"));
    int token = 0;
    kept->base.wrapper = &token;
    EMB_CHECK(embi_append_child(&div->base, &kept->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_replace_all(&div->base, nullptr) == EMBI_DOM_OK);
    EMB_CHECK(kept->base.parent == nullptr);
    EMB_CHECK(kept->base.wrapper == &token);
    embi_document_free(d);
}

EMB_TEST(memory_replace_all_with_node_inside_a_child)
{
    embi_document *d = embi_document_new(true);
    embi_element *div = make(d, S("div"));
    embi_element *outer = make(d, S("section"));
    embi_element *inner = make(d, S("p"));
    EMB_CHECK(embi_append_child(&div->base, &outer->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&outer->base, &inner->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_replace_all(&div->base, &inner->base) == EMBI_DOM_OK);
    EMB_CHECK(div->base.first_child == &inner->base && inner->base.next == nullptr);
    embi_document_free(d);
}

EMB_TEST(memory_template_contents_wrappers_keep_the_template)
{
    embi_document *d = embi_document_new(true);
    embi_element *div = make(d, S("div"));
    embi_element *tpl = make(d, S("template"));
    embi_element *inner_tpl = make(d, S("template"));
    EMB_CHECK(embi_append_child(&div->base, &tpl->base) == EMBI_DOM_OK);
    embi_node *contents = embi_template_contents(tpl);
    EMB_CHECK(embi_append_child(contents, &inner_tpl->base) == EMBI_DOM_OK);
    embi_node *inner_contents = embi_template_contents(inner_tpl);
    embi_element *deep = make(inner_contents->doc, S("p"));
    EMB_CHECK(embi_append_child(inner_contents, &deep->base) == EMBI_DOM_OK);

    /* A wrapper two templates deep keeps the whole detached subtree allocated. */
    int token = 0;
    deep->base.wrapper = &token;
    size_t blocks = embi_slab_live_blocks(d->slab);
    EMB_CHECK(embi_replace_all(&div->base, nullptr) == EMBI_DOM_OK);
    EMB_CHECK(tpl->base.parent == nullptr);
    EMB_CHECK_EQ_INT(embi_slab_live_blocks(d->slab), blocks);
    EMB_CHECK(!embi_destroy_if_unowned(&tpl->base));
    EMB_CHECK(deep->base.wrapper == &token);

    /* Without the wrapper the subtree is freed, template contents included. */
    deep->base.wrapper = nullptr;
    EMB_CHECK(embi_destroy_if_unowned(&tpl->base));
    EMB_CHECK(embi_slab_live_blocks(d->slab) < blocks);
    embi_document_free(d);
}

EMB_TEST_MAIN()
