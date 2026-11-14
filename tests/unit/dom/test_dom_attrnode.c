/*
 * Attr nodes (W5f, src/dom/attrnode.c): the handle of an attribute that script gets from Element.getAttributeNode,
 * createAttribute and setAttributeNode. Ownership, attach and detach, value forwarding and the node document.
 */
#include "emb_test.h"

#include "dom/dom.h"

#define S(lit) EMBI_STR(lit)

static embi_atom atom(const char *s)
{
    return embi_atom_intern(s, (uint32_t)SDL_strlen(s));
}

static embi_dstr v8(embi_document *d, const char *utf8)
{
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    return embi_dstr_from_utf8(d->slab, s);
}

static bool is_value(embi_dstr v, const char *utf8)
{
    embi_str s = {utf8, (uint32_t)SDL_strlen(utf8)};
    return embi_dstr_eq_utf8(v, s);
}

static embi_element *make(embi_document *d, const char *name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, (embi_str){name, (uint32_t)SDL_strlen(name)}, &e) == EMBI_DOM_OK);
    return e;
}

static embi_attr_node *new_attr(embi_document *d, const char *local, const char *value)
{
    embi_atom a = atom(local);
    embi_dstr v = v8(d, value);
    embi_attr_node *an = embi_new_attr_node(d, EMBI_NS_NULL, 0, 0, a, v);
    embi_dstr_free(v);
    return an;
}

EMB_TEST(attrnode_new_is_detached_and_keeps_its_value)
{
    embi_document *w = embi_document_new(true); /* warm-up: interns the atoms */
    (void)new_attr(w, "title", "hello");
    embi_document_free(w);
    size_t base = embi_mem_live();
    embi_document *d = embi_document_new(true);
    embi_attr_node *an = new_attr(d, "title", "hello");
    EMB_CHECK(an->base.type == EMBI_NODE_ATTR);
    EMB_CHECK(an->owner == nullptr);
    EMB_CHECK(an->base.doc == d && embi_attr_node_doc(an) == d);
    EMB_CHECK(is_value(embi_attr_node_value(an), "hello"));
    embi_attr_node_set_value(an, v8(d, "bye"));
    EMB_CHECK(is_value(embi_attr_node_value(an), "bye"));
    embi_document_free(d);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST(attrnode_attach_makes_the_element_attribute_and_forwards_the_value)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, "div");
    embi_attr_node *an = new_attr(d, "title", "one");
    embi_attr_node_attach(an, el);
    EMB_CHECK(an->owner == el);
    const embi_attr *at = embi_el_get_attribute(el, S("title"));
    EMB_CHECK(at != nullptr && is_value(at->value, "one"));
    /* a write through the Attr changes the element's attribute, and the element's value is what the Attr reads */
    embi_attr_node_set_value(an, v8(d, "two"));
    EMB_CHECK(is_value(embi_el_get_attribute(el, S("title"))->value, "two"));
    EMB_CHECK(embi_el_set_attribute(el, S("title"), v8(d, "three")) == EMBI_DOM_OK);
    EMB_CHECK(is_value(embi_attr_node_value(an), "three"));
    EMB_CHECK(embi_attr_node_doc(an) == d);
    embi_document_free(d);
}

EMB_TEST(attrnode_detach_keeps_the_last_value_and_leaves_the_element)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, "div");
    embi_attr_node *an = new_attr(d, "title", "kept");
    embi_attr_node_attach(an, el);
    embi_dstr last = embi_el_get_attribute(el, S("title"))->value;
    embi_attr_node_detach(an, last);
    EMB_CHECK(an->owner == nullptr);
    EMB_CHECK(is_value(embi_attr_node_value(an), "kept"));
    /* the element's attribute can now go without touching the Attr */
    EMB_CHECK(embi_el_remove_attribute(el, S("title")));
    EMB_CHECK(is_value(embi_attr_node_value(an), "kept"));
    EMB_CHECK(!embi_el_has_attribute(el, S("title")));
    embi_document_free(d);
}

EMB_TEST(attrnode_reattach_to_another_element_in_another_document)
{
    embi_document *d1 = embi_document_new(true);
    embi_document *d2 = embi_document_new(true);
    embi_element *a = make(d1, "p");
    embi_element *b = make(d2, "p");
    embi_attr_node *an = new_attr(d1, "lang", "fr");
    embi_attr_node_attach(an, a);
    EMB_CHECK(embi_attr_node_doc(an) == d1);
    embi_attr_node_detach(an, embi_el_get_attribute(a, S("lang"))->value);
    embi_attr_node_attach(an, b);
    EMB_CHECK(an->owner == b);
    /* the node document follows the owner */
    EMB_CHECK(embi_attr_node_doc(an) == d2);
    EMB_CHECK(is_value(embi_el_get_attribute(b, S("lang"))->value, "fr"));
    EMB_CHECK(is_value(embi_el_get_attribute(a, S("lang"))->value, "fr")); /* a keeps its own attribute */
    embi_document_free(d2);
    embi_document_free(d1);
}

EMB_TEST(attrnode_put_attr_replaces_by_namespace_and_local_name)
{
    embi_document *d = embi_document_new(true);
    embi_element *el = make(d, "svg");
    embi_atom href = atom("href");
    embi_atom xlink = atom("xlink");
    embi_atom uri = atom("http://www.w3.org/1999/xlink");
    embi_el_put_attr(el, EMBI_NS_OTHER, uri, xlink, href, v8(d, "#a"));
    embi_el_put_attr(el, EMBI_NS_NULL, 0, 0, href, v8(d, "plain"));
    EMB_CHECK_EQ_INT(el->attrs->len, 2); /* same local name, other namespace: two attributes */
    embi_el_put_attr(el, EMBI_NS_OTHER, uri, xlink, href, v8(d, "#b"));
    EMB_CHECK_EQ_INT(el->attrs->len, 2); /* replaced, not appended */
    embi_document_free(d);
}

EMB_TEST_MAIN()
