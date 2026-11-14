/*
 * Documents made by script (W5f): the Document constructor, document.implementation and DOMImplementation
 * (createDocument, createHTMLDocument, createDocumentType, hasFeature). Such documents are owned by the binding
 * context (embi_bind_add_xdoc) and live as long as it does.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"

static embi_document *new_doc(bool is_html, uint8_t ctype, bool xml_class)
{
    embi_document *d = embi_document_new(is_html);
    d->ctype = ctype;
    d->xml_class = xml_class;
    return d;
}

embi_js_value embi_impl_Document_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_document *d = new_doc(false, EMBI_CT_XML, false);
    embi_bind_add_xdoc(c, d);
    embi_js_value v = embi_bind_node_value(c, &d->base);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

/* Only the document of the window has a Location; documents made by script have none. */
embi_js_value embi_impl_Document_get_location(embi_bind_ctx *c, embi_node *self)
{
    if (self != &c->doc->base) {
        return EMBI_JS_NULL;
    }
    return embi_bind_other_value(c, EMBI_IF_Location, c, nullptr, 0);
}

embi_js_value embi_impl_Document_get_implementation(embi_bind_ctx *c, embi_node *self)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_DOMImplementation, self, nullptr, 0);
    /* the object refers to its document by a plain pointer: keep the document's wrapper (and so the document) alive */
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo) {
        embi_bind_set_ref(bo, embi_bind_node_value(c, self));
    }
    return v;
}

static embi_document *impl_doc(embi_bind_obj *self)
{
    return self->ptr;
}

embi_dom_err embi_impl_DOMImplementation_createDocumentType(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0,
                                                           embi_dstr a1, embi_dstr a2, embi_node **out)
{
    embi_doctype *dt = nullptr;
    embi_dom_err e = embi_dom_create_doctype(impl_doc(self), embi_bind_utf8(c, a0), embi_bind_utf8(c, a1),
                                             embi_bind_utf8(c, a2), &dt);
    if (e == EMBI_DOM_OK) {
        *out = &dt->base;
    }
    return e;
}

embi_dom_err embi_impl_DOMImplementation_createDocument(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0,
                                                       embi_dstr a1, embi_node *a2, embi_node **out)
{
    (void)self;
    embi_str ns = (a0.is_null || a0.s.len == 0) ? EMBI_STR("") : embi_bind_utf8(c, a0.s);
    uint8_t ctype = EMBI_CT_XML;
    if (ns.len) {
        uint8_t k = embi_dom_ns_from_uri(ns);
        ctype = k == EMBI_NS_HTML ? EMBI_CT_XHTML : k == EMBI_NS_SVG ? EMBI_CT_SVG : EMBI_CT_XML;
    }
    embi_document *d = new_doc(false, ctype, true);
    embi_element *el = nullptr;
    embi_str qn = embi_bind_utf8(c, a1);
    if (qn.len != 0) {
        embi_dom_err e = embi_dom_create_element_ns(d, ns.len != 0, ns, qn, &el);
        if (e != EMBI_DOM_OK) {
            embi_document_free(d);
            return e;
        }
    }
    embi_bind_add_xdoc(c, d);
    if (a2) {
        (void)embi_append_child(&d->base, a2);
    }
    if (el) {
        (void)embi_append_child(&d->base, &el->base);
    }
    *out = &d->base;
    return EMBI_DOM_OK;
}

static embi_document *html_doc(embi_bind_ctx *c, bool has_title, embi_dstr title)
{
    embi_document *d = new_doc(true, EMBI_CT_HTML, false);
    embi_bind_add_xdoc(c, d);
    embi_dstr nm = embi_dstr_from_utf8(d->slab, EMBI_STR("html"));
    embi_doctype *dt = embi_new_doctype(d, nm, embi_dstr_empty(), embi_dstr_empty());
    embi_dstr_free(nm);
    (void)embi_append_child(&d->base, &dt->base);
    embi_element *html = embi_new_element(d, EMBI_NS_HTML, 0, 0, EMBI_ATOM_html);
    (void)embi_append_child(&d->base, &html->base);
    embi_element *head = embi_new_element(d, EMBI_NS_HTML, 0, 0, EMBI_ATOM_head);
    (void)embi_append_child(&html->base, &head->base);
    if (has_title) {
        embi_element *t = embi_new_element(d, EMBI_NS_HTML, 0, 0, EMBI_ATOM_title);
        (void)embi_append_child(&head->base, &t->base);
        embi_chardata *txt = embi_new_text(d, title);
        (void)embi_append_child(&t->base, &txt->base);
    }
    embi_element *body = embi_new_element(d, EMBI_NS_HTML, 0, 0, EMBI_ATOM_body);
    (void)embi_append_child(&html->base, &body->base);
    return d;
}

embi_node *embi_impl_DOMImplementation_createHTMLDocument_0(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)self;
    return &html_doc(c, false, embi_dstr_empty())->base;
}

embi_node *embi_impl_DOMImplementation_createHTMLDocument_1(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)self;
    return &html_doc(c, true, a0)->base;
}

bool embi_impl_DOMImplementation_hasFeature(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return true;
}

embi_dom_err embi_impl_Document_createAttribute(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_node **out)
{
    embi_attr_node *an = nullptr;
    embi_dom_err e = embi_dom_create_attr((embi_document *)self, embi_bind_utf8(c, a0), &an);
    if (e == EMBI_DOM_OK) {
        *out = &an->base;
    }
    return e;
}

embi_dom_err embi_impl_Document_createAttributeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0, embi_dstr a1,
                                                  embi_node **out)
{
    embi_str ns = (a0.is_null || a0.s.len == 0) ? EMBI_STR("") : embi_bind_utf8(c, a0.s);
    embi_attr_node *an = nullptr;
    embi_dom_err e = embi_dom_create_attr_ns((embi_document *)self, ns.len != 0, ns, embi_bind_utf8(c, a1), &an);
    if (e == EMBI_DOM_OK) {
        *out = &an->base;
    }
    return e;
}
