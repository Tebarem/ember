#pragma once
/* Helpers for the platform tests: small documents built through the DOM API (no parser, no view). */
#include "dom/dom.h"

#include <string.h>

static inline embi_element *pf_el(embi_document *doc, embi_node *parent, const char *tag)
{
    embi_element *el = nullptr;
    (void)embi_dom_create_element(doc, (embi_str){tag, (uint32_t)strlen(tag)}, &el);
    if (parent != nullptr) {
        (void)embi_append_child(parent, &el->base);
    }
    return el;
}

static inline void pf_attr(embi_element *el, const char *name, const char *value)
{
    embi_dstr v = {value, (uint32_t)strlen(value), false};
    (void)embi_el_set_attribute(el, (embi_str){name, (uint32_t)strlen(name)}, v);
}

/* A document with html > body, returned through body. */
static inline embi_document *pf_doc(embi_element **body_out)
{
    embi_document *doc = embi_document_new(true);
    embi_element *html = pf_el(doc, &doc->base, "html");
    *body_out = pf_el(doc, &html->base, "body");
    return doc;
}
