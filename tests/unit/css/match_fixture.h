#pragma once
/*
 * Shared DOM fixture for the selector matching tests (header-only, test-only). The main tree is
 * parsed from inline HTML with the W1 parser, so attribute-name lowercasing, tag case folding, the
 * quirks decision and the html-namespace flags all come from the real tree builder. The SVG element
 * is added by hand, because the parser nests SVG under <svg>. The tree:
 *
 *   html[lang=en-US]
 *     body[dir=rtl][id=main][class="a b"]
 *       div#d1.x
 *         p.y[title="hello world"] "text"
 *         span[lang=fr-CA] "s"
 *       ul
 *         li "1"  li.sel "2"  li "3"
 *       i (empty)
 *       input[type=checkbox][checked][disabled]
 *       a[href=x] "link"
 *       custom-el
 *       rect (SVG namespace, hand-built, last child of body)
 */
#include "emb_test.h"

#include "css/selector.h"
#include "dom/dom.h"
#include "html/parser.h"

#include <stdlib.h>
#include <string.h>

#define MF_SVG "http://www.w3.org/2000/svg"
#define MF_XHTML "http://www.w3.org/1999/xhtml"

static inline embi_str ms(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static inline embi_element *mf_el(embi_document *d, const char *name)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element(d, ms(name), &e) == EMBI_DOM_OK);
    return e;
}

static inline embi_element *mf_el_ns(embi_document *d, const char *uri, const char *qname)
{
    embi_element *e = nullptr;
    EMB_CHECK(embi_dom_create_element_ns(d, true, ms(uri), ms(qname), &e) == EMBI_DOM_OK);
    return e;
}

static inline void mf_attr(embi_element *e, const char *name, const char *value)
{
    EMB_CHECK(embi_el_set_attribute(e, ms(name), embi_dstr_from_utf8(e->base.doc->slab, ms(value))) ==
              EMBI_DOM_OK);
}

static inline void mf_app(embi_node *parent, embi_node *child)
{
    EMB_CHECK(embi_append_child(parent, child) == EMBI_DOM_OK);
}

static inline void mf_text(embi_node *parent, const char *s)
{
    embi_chardata *t = embi_new_text(parent->doc, embi_dstr_from_utf8(parent->doc->slab, ms(s)));
    mf_app(parent, &t->base);
}

typedef struct mf_tree {
    embi_document *doc;
    embi_element *html;
    embi_element *body;
    embi_element *div;
    embi_element *p;
    embi_element *span;
    embi_element *ul;
    embi_element *li1;
    embi_element *li2;
    embi_element *li3;
    embi_element *i;
    embi_element *input;
    embi_element *a;
    embi_element *rect;
    embi_element *custom;
} mf_tree;

/* Parses html into a fresh HTML document with the real parser. */
static inline embi_document *mf_parse(const char *html)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return doc;
}

/* The first element with this (lowercase) tag name in document order, or nullptr. */
static inline embi_element *mf_find(embi_document *doc, const char *name)
{
    embi_atom want = embi_atom_intern(name, (uint32_t)strlen(name));
    for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n != nullptr;
         n = embi_next_in_tree(&doc->base, n)) {
        if (n->type == EMBI_NODE_ELEMENT && n->name == want) {
            return (embi_element *)n;
        }
    }
    return nullptr;
}

/* The first element with this id, or nullptr. */
static inline embi_element *mf_by_id(embi_document *doc, const char *id)
{
    embi_atom want = embi_atom_intern(id, (uint32_t)strlen(id));
    for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n != nullptr;
         n = embi_next_in_tree(&doc->base, n)) {
        if (n->type == EMBI_NODE_ELEMENT && embi_el_id((embi_element *)n) == want) {
            return (embi_element *)n;
        }
    }
    return nullptr;
}

#define MF_BODY_HTML                                                                                  "<html lang=en-US><body dir=rtl id=main class=\"a b\">"                                          "<div id=d1 class=x><p class=y title=\"hello world\">text</p><span lang=fr-CA>s</span></div>"     "<ul><li>1</li><li class=sel>2</li><li>3</li></ul><i></i>"                                        "<input type=checkbox checked disabled><a href=x>link</a><custom-el></custom-el>"                 "</body></html>"

/* A fixture that is not what the tests expect is a test bug: stop instead of dereferencing null. */
static inline void mf_need(bool ok)
{
    if (!ok) {
        emb_test_report_fail(__FILE__, __LINE__, "fixture tree is malformed");
        abort();
    }
}

static inline mf_tree mf_from_doc(embi_document *doc)
{
    mf_tree t = {0};
    t.doc = doc;
    t.html = mf_find(doc, "html");
    t.body = mf_find(doc, "body");
    t.div = mf_find(doc, "div");
    t.p = mf_find(doc, "p");
    t.span = mf_find(doc, "span");
    t.ul = mf_find(doc, "ul");
    mf_need(t.html && t.body && t.div && t.p && t.span && t.ul);
    t.li1 = (embi_element *)t.ul->base.first_child;
    t.li2 = (embi_element *)t.li1->base.next;
    t.li3 = (embi_element *)t.li2->base.next;
    t.i = mf_find(doc, "i");
    t.input = mf_find(doc, "input");
    t.a = mf_find(doc, "a");
    t.custom = mf_find(doc, "custom-el");
    mf_need(t.i && t.input && t.a && t.custom);
    t.rect = mf_el_ns(doc, MF_SVG, "rect");
    mf_app(&t.body->base, &t.rect->base);
    return t;
}

/* The standards-mode fixture (with a doctype). */
static inline mf_tree mf_build(void)
{
    return mf_from_doc(mf_parse("<!DOCTYPE html>" MF_BODY_HTML));
}

/* The same tree without a doctype: the parser puts the document in quirks mode. */
static inline mf_tree mf_build_quirks(void)
{
    return mf_from_doc(mf_parse(MF_BODY_HTML));
}

static inline void mf_free(mf_tree *t)
{
    embi_document_free(t->doc);
    t->doc = nullptr;
}

/* Does the element match the selector list (non-pseudo items), with an optional scope and env? */
static inline bool mf_matches_in(embi_element *e, const char *sel, const embi_sel_env *env,
                                 embi_element *scope, const embi_sel_hooks *hooks)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = embi_sel_parse_text(&a, ms(sel), env);
    if (l == nullptr) {
        emb_test_report_fail(__FILE__, __LINE__, "selector failed to parse");
        embi_arena_free(&a);
        return false;
    }
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, scope, hooks);
    bool r = embi_sel_match_list(l, e, &ctx);
    embi_sel_ctx_free(&ctx);
    embi_arena_free(&a);
    return r;
}

static inline bool mf_matches(embi_element *e, const char *sel)
{
    return mf_matches_in(e, sel, nullptr, nullptr, nullptr);
}

/* Matches a pseudo-element selector against e for the given pseudo-element. */
static inline bool mf_matches_pe(embi_element *e, const char *sel, embi_sel_pseudo pe)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = embi_sel_parse_text(&a, ms(sel), nullptr);
    if (l == nullptr) {
        emb_test_report_fail(__FILE__, __LINE__, "selector failed to parse");
        embi_arena_free(&a);
        return false;
    }
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, nullptr, nullptr);
    bool r = false;
    for (uint32_t k = 0; k < l->n && !r; k++) {
        r = embi_sel_match(&l->items[k], e, pe, &ctx, nullptr);
    }
    embi_sel_ctx_free(&ctx);
    embi_arena_free(&a);
    return r;
}
