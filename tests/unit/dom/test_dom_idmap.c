#include "emb_test.h"

#include "dom/dom.h"

#define S(lit) EMBI_STR(lit)
#define NAMES 40
#define ELEMENTS 150
#define STEPS 3000

static char g_names[NAMES][8];

static embi_element *make(embi_document *d, embi_str name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, name, &e) == EMBI_DOM_OK);
    return e;
}

static uint32_t next_rand(uint32_t *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (*seed >> 16) & 0x7FFFu;
}

/* First element in tree order under doc whose id attribute equals name. Independent of the id
 * cache and the id map: it reads the attribute value. */
static embi_element *brute_force(embi_document *d, const char *name)
{
    embi_str key = {name, (uint32_t)SDL_strlen(name)};
    for (embi_node *n = d->base.first_child; n; n = embi_next_in_tree(&d->base, n)) {
        if (n->type != EMBI_NODE_ELEMENT) {
            continue;
        }
        const embi_attr *at = embi_el_get_attribute((embi_element *)n, S("id"));
        if (at && embi_dstr_eq_utf8(at->value, key)) {
            return (embi_element *)n;
        }
    }
    return nullptr;
}

static void set_id(embi_document *d, embi_element *el, const char *name)
{
    embi_str s = {name, (uint32_t)SDL_strlen(name)};
    embi_dstr v = embi_dstr_from_utf8(d->slab, s);
    EMB_CHECK(embi_el_set_attribute(el, S("id"), v) == EMBI_DOM_OK);
    embi_dstr_free(v);
}

EMB_TEST(idmap_matches_brute_force_under_churn)
{
    for (int i = 0; i < NAMES; i++) {
        SDL_snprintf(g_names[i], sizeof g_names[i], "k%d", i);
    }
    embi_document *d = embi_document_new(true);
    embi_element *html = make(d, S("html"));
    embi_element *body = make(d, S("body"));
    EMB_CHECK(embi_append_child(&d->base, &html->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&html->base, &body->base) == EMBI_DOM_OK);

    embi_element *els[ELEMENTS];
    for (int i = 0; i < ELEMENTS; i++) {
        els[i] = make(d, S("div"));
    }

    uint32_t seed = 12345u;
    int mismatches = 0;
    for (int step = 0; step < STEPS; step++) {
        uint32_t op = next_rand(&seed) % 10u;
        embi_element *el = els[next_rand(&seed) % ELEMENTS];
        switch (op) {
        case 0:
        case 1:
        case 2:
            if (!el->base.parent) {
                EMB_CHECK(embi_append_child(&body->base, &el->base) == EMBI_DOM_OK);
            }
            break;
        case 3:
            embi_detach(&el->base);
            break;
        case 4:
        case 5:
        case 6:
            set_id(d, el, g_names[next_rand(&seed) % NAMES]);
            break;
        case 7:
            embi_el_remove_attribute(el, S("id"));
            break;
        default: {
            /* Move an attached element to the end, which changes tree order. */
            if (el->base.parent) {
                embi_detach(&el->base);
                EMB_CHECK(embi_append_child(&body->base, &el->base) == EMBI_DOM_OK);
            }
            break;
        }
        }
        for (int k = 0; k < NAMES; k++) {
            embi_str key = {g_names[k], (uint32_t)SDL_strlen(g_names[k])};
            embi_element *got = embi_get_element_by_id(d, key);
            embi_element *want = brute_force(d, g_names[k]);
            if (got != want) {
                mismatches++;
            }
        }
    }
    EMB_CHECK_EQ_INT(mismatches, 0);

    /* Detached elements are never reported. */
    for (int i = 0; i < ELEMENTS; i++) {
        embi_detach(&els[i]->base);
    }
    EMB_CHECK(embi_get_element_by_id(d, S("k1")) == nullptr);
    embi_document_free(d);
}

EMB_TEST(idmap_first_in_tree_order_wins)
{
    embi_document *d = embi_document_new(true);
    embi_element *html = make(d, S("html"));
    EMB_CHECK(embi_append_child(&d->base, &html->base) == EMBI_DOM_OK);
    embi_element *a = make(d, S("a"));
    embi_element *b = make(d, S("b"));
    embi_element *c = make(d, S("c"));
    EMB_CHECK(embi_append_child(&html->base, &b->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&html->base, &c->base) == EMBI_DOM_OK);
    EMB_CHECK(embi_append_child(&html->base, &a->base) == EMBI_DOM_OK);
    set_id(d, c, "dup");
    set_id(d, a, "dup");
    EMB_CHECK(embi_get_element_by_id(d, S("dup")) == c);
    embi_detach(&c->base);
    EMB_CHECK(embi_get_element_by_id(d, S("dup")) == a);
    embi_document_free(d);
}

EMB_TEST(idmap_empty_id_means_no_id)
{
    embi_document *d = embi_document_new(true);
    embi_element *html = make(d, S("html"));
    EMB_CHECK(embi_append_child(&d->base, &html->base) == EMBI_DOM_OK);
    embi_element *a = make(d, S("a"));
    EMB_CHECK(embi_append_child(&html->base, &a->base) == EMBI_DOM_OK);
    set_id(d, a, "");
    EMB_CHECK(embi_el_id(a) == 0);
    EMB_CHECK(embi_get_element_by_id(d, S("")) == nullptr);
    set_id(d, a, "x");
    EMB_CHECK(embi_el_id(a) != 0);
    EMB_CHECK(embi_get_element_by_id(d, S("x")) == a);
    set_id(d, a, "");
    EMB_CHECK(embi_get_element_by_id(d, S("x")) == nullptr);
    EMB_CHECK(embi_get_element_by_id(d, S("")) == nullptr);
    set_id(d, a, "x");
    EMB_CHECK(embi_get_element_by_id(d, S("x")) == a);
    embi_document_free(d);
}

EMB_TEST(equal_node_distinguishes_unknown_namespaces)
{
    embi_document *d = embi_document_new(true);
    embi_element *a = nullptr;
    embi_element *b = nullptr;
    embi_element *c = nullptr;
    EMB_CHECK(embi_dom_create_element_ns(d, true, S("urn:a"), S("x"), &a) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_element_ns(d, true, S("urn:b"), S("x"), &b) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_element_ns(d, true, S("urn:a"), S("x"), &c) == EMBI_DOM_OK);
    EMB_CHECK(!embi_is_equal_node(&a->base, &b->base));
    EMB_CHECK(!embi_is_equal_node(&b->base, &a->base));
    EMB_CHECK(embi_is_equal_node(&a->base, &c->base));
    embi_document_free(d);
}

EMB_TEST(doctype_requires_qname)
{
    embi_document *d = embi_document_new(true);
    embi_doctype *dt = nullptr;
    EMB_CHECK(embi_dom_create_doctype(d, S("html"), S(""), S(""), &dt) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_doctype(d, S(":a"), S(""), S(""), &dt) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_doctype(d, S("a:b:c"), S(""), S(""), &dt) == EMBI_DOM_OK);
    EMB_CHECK(embi_dom_create_doctype(d, S("a:b"), S(""), S(""), &dt) == EMBI_DOM_OK);
    embi_document_free(d);
}

EMB_TEST_MAIN()
