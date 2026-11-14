/*
 * Element (attributes, selectors, HTML fragments, adjacent insertion, geometry) behind the
 * generated glue in gen/dom.c.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "css/selector.h"
#include "html/parser.h"
#include "html/serializer.h"
#include "layout/layout.h"

static const embi_str k_no_str = {"", 0};

static embi_element *el_of(embi_node *n)
{
    return (embi_element *)n;
}

static embi_str ns_arg(embi_bind_ctx *c, embi_bind_nstr a)
{
    return (a.is_null || a.s.len == 0) ? k_no_str : embi_bind_utf8(c, a.s);
}

/* ------------------------------------------------------------------ names */

embi_js_value embi_impl_Element_get_namespaceURI(embi_bind_ctx *c, embi_node *self)
{
    embi_str ns = embi_el_namespace_uri(el_of(self));
    return ns.len ? embi_bind_utf8_value(c, ns) : EMBI_JS_NULL;
}

embi_js_value embi_impl_Element_get_prefix(embi_bind_ctx *c, embi_node *self)
{
    embi_element *e = el_of(self);
    return e->prefix ? embi_bind_utf8_value(c, embi_atom_str(e->prefix)) : EMBI_JS_NULL;
}

embi_dstr embi_impl_Element_get_localName(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_utf8(c, embi_atom_str(self->name));
}

embi_dstr embi_impl_Element_get_tagName(embi_bind_ctx *c, embi_node *self)
{
    embi_element *e = el_of(self);
    embi_str local = embi_atom_str(self->name);
    embi_str prefix = e->prefix ? embi_atom_str(e->prefix) : k_no_str;
    uint32_t n = prefix.len ? prefix.len + 1 + local.len : local.len;
    char *buf = embi_bind_alloc(c, (size_t)n + 1);
    uint32_t o = 0;
    if (prefix.len) {
        SDL_memcpy(buf, prefix.ptr, prefix.len);
        buf[prefix.len] = ':';
        o = prefix.len + 1;
    }
    if (local.len) {
        SDL_memcpy(buf + o, local.ptr, local.len);
    }
    if (self->extra == EMBI_NS_HTML && self->doc->is_html) {
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z') {
                buf[i] = (char)(buf[i] - 'a' + 'A');
            }
        }
    }
    return embi_bind_tmp_utf8(c, (embi_str){buf, n});
}

embi_js_value embi_impl_Element_get_classList(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_tokenlist_new(c, embi_impl_node_rec(c, self), EMBI_STR("class"));
}

embi_js_value embi_impl_Element_get_attributes(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_attrmap_new(c, embi_impl_node_rec(c, self));
}

/* ------------------------------------------------------------------ attributes */

bool embi_impl_Element_hasAttributes(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return embi_el_attr_count(el_of(self)) != 0;
}

embi_bind_seq embi_impl_Element_getAttributeNames(embi_bind_ctx *c, embi_node *self)
{
    embi_element *e = el_of(self);
    uint32_t n = embi_el_attr_count(e);
    embi_dstr *names = (embi_dstr *)embi_bind_alloc(c, (size_t)n * sizeof *names + 1);
    for (uint32_t i = 0; i < n; i++) {
        const embi_attr *a = embi_el_attr_at(e, i);
        embi_str local = embi_atom_str(a->name);
        if (a->prefix) {
            embi_str p = embi_atom_str(a->prefix);
            char *buf = embi_bind_alloc(c, (size_t)p.len + 1 + local.len + 1);
            SDL_memcpy(buf, p.ptr, p.len);
            buf[p.len] = ':';
            SDL_memcpy(buf + p.len + 1, local.ptr, local.len);
            names[i] = embi_bind_tmp_utf8(c, (embi_str){buf, p.len + 1 + local.len});
        } else {
            names[i] = embi_bind_tmp_utf8(c, local);
        }
    }
    return (embi_bind_seq){names, n};
}

embi_js_value embi_impl_Element_getAttribute(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    const embi_attr *a = embi_el_get_attribute(el_of(self), embi_bind_utf8(c, a0));
    return a ? embi_bind_dstr_value(c, a->value) : EMBI_JS_NULL;
}

embi_js_value embi_impl_Element_getAttributeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0, embi_dstr a1)
{
    embi_str ns = ns_arg(c, a0);
    const embi_attr *a = embi_el_get_attribute_ns(el_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1));
    return a ? embi_bind_dstr_value(c, a->value) : EMBI_JS_NULL;
}

embi_dom_err embi_impl_Element_setAttribute(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_dstr a1)
{
    return embi_el_set_attribute(el_of(self), embi_bind_utf8(c, a0), a1);
}

embi_dom_err embi_impl_Element_setAttributeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0,
                                              embi_dstr a1, embi_dstr a2)
{
    embi_str ns = ns_arg(c, a0);
    return embi_el_set_attribute_ns(el_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1), a2);
}

void embi_impl_Element_removeAttribute(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)embi_el_remove_attribute(el_of(self), embi_bind_utf8(c, a0));
}

void embi_impl_Element_removeAttributeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0, embi_dstr a1)
{
    embi_str ns = ns_arg(c, a0);
    (void)embi_el_remove_attribute_ns(el_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1));
}

embi_dom_err embi_impl_Element_toggleAttribute_0(embi_bind_ctx *c, embi_node *self, embi_dstr a0, bool *out)
{
    return embi_el_toggle_attribute(el_of(self), embi_bind_utf8(c, a0), false, false, out);
}

embi_dom_err embi_impl_Element_toggleAttribute_1(embi_bind_ctx *c, embi_node *self, embi_dstr a0, bool a1,
                                                 bool *out)
{
    return embi_el_toggle_attribute(el_of(self), embi_bind_utf8(c, a0), true, a1, out);
}

bool embi_impl_Element_hasAttribute(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return embi_el_has_attribute(el_of(self), embi_bind_utf8(c, a0));
}

bool embi_impl_Element_hasAttributeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0, embi_dstr a1)
{
    embi_str ns = ns_arg(c, a0);
    return embi_el_has_attribute_ns(el_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1));
}

embi_node *embi_impl_Element_getAttributeNode(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    const embi_attr *a = embi_el_get_attribute(el_of(self), embi_bind_utf8(c, a0));
    return a ? &embi_impl_attr_for(c, embi_impl_node_rec(c, self), a)->base : nullptr;
}

embi_node *embi_impl_Element_getAttributeNodeNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0, embi_dstr a1)
{
    embi_str ns = ns_arg(c, a0);
    const embi_attr *a = embi_el_get_attribute_ns(el_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1));
    return a ? &embi_impl_attr_for(c, embi_impl_node_rec(c, self), a)->base : nullptr;
}

embi_dom_err embi_impl_Element_setAttributeNode(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    return embi_impl_set_attr_node(c, self, a0, out);
}

embi_dom_err embi_impl_Element_setAttributeNodeNS(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    return embi_impl_set_attr_node(c, self, a0, out);
}

embi_dom_err embi_impl_Element_removeAttributeNode(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    (void)c;
    embi_attr_node *an = (embi_attr_node *)a0;
    embi_element *el = el_of(self);
    if (an->owner != el) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    embi_str ln = embi_atom_str(an->local);
    if (an->ns == EMBI_NS_NULL) {
        (void)embi_el_remove_attribute_ns(el, false, EMBI_STR(""), ln);
    } else {
        (void)embi_el_remove_attribute_ns(el, true, embi_dom_ns_uri(an->ns, an->ns_uri), ln);
    }
    *out = a0;
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ selectors */

static const embi_sel_list *parse_selectors(embi_bind_ctx *c, embi_dstr s, embi_dom_err *err)
{
    embi_str text = embi_bind_utf8(c, s);
    const embi_sel_list *l = embi_sel_parse_text(&c->arena, text, nullptr);
    if (!l) {
        char *msg = embi_bind_alloc(c, (size_t)text.len + 48);
        SDL_snprintf(msg, (size_t)text.len + 48, "'%.*s' is not a valid selector.", (int)text.len, text.ptr);
        *err = embi_impl_throw(c, "SyntaxError", msg);
    }
    return l;
}

embi_dom_err embi_impl_query_first(embi_bind_ctx *c, embi_node *root, embi_element *scope,
                                   embi_dstr selectors, embi_node **out)
{
    embi_dom_err err = EMBI_DOM_OK;
    const embi_sel_list *l = parse_selectors(c, selectors, &err);
    if (!l) {
        return err;
    }
    embi_sel_ctx sc;
    embi_sel_ctx_init(&sc, scope, nullptr);
    *out = nullptr;
    for (embi_node *n = embi_next_in_tree(root, root); n; n = embi_next_in_tree(root, n)) {
        if (n->type == EMBI_NODE_ELEMENT && embi_sel_match_list(l, (embi_element *)n, &sc)) {
            *out = n;
            break;
        }
    }
    embi_sel_ctx_free(&sc);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_query_all(embi_bind_ctx *c, embi_node *root, embi_element *scope,
                                 embi_dstr selectors, embi_js_value *out)
{
    embi_dom_err err = EMBI_DOM_OK;
    const embi_sel_list *l = parse_selectors(c, selectors, &err);
    if (!l) {
        return err;
    }
    embi_sel_ctx sc;
    embi_sel_ctx_init(&sc, scope, nullptr);
    embi_node **found = nullptr;
    uint32_t n = 0, cap = 0;
    for (embi_node *k = embi_next_in_tree(root, root); k; k = embi_next_in_tree(root, k)) {
        if (k->type == EMBI_NODE_ELEMENT && embi_sel_match_list(l, (embi_element *)k, &sc)) {
            if (n == cap) {
                uint32_t ncap = cap ? cap * 2 : 16;
                embi_node **nf = (embi_node **)embi_bind_alloc(c, (size_t)ncap * sizeof *nf);
                if (n) {
                    SDL_memcpy(nf, found, (size_t)n * sizeof *nf);
                }
                found = nf;
                cap = ncap;
            }
            found[n++] = k;
        }
    }
    embi_sel_ctx_free(&sc);
    *out = embi_impl_nodelist_static(c, found, n);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_matches(embi_bind_ctx *c, embi_element *el, embi_dstr selectors, bool *out)
{
    embi_dom_err err = EMBI_DOM_OK;
    const embi_sel_list *l = parse_selectors(c, selectors, &err);
    if (!l) {
        return err;
    }
    embi_sel_ctx sc;
    embi_sel_ctx_init(&sc, el, nullptr);
    *out = embi_sel_match_list(l, el, &sc);
    embi_sel_ctx_free(&sc);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_closest(embi_bind_ctx *c, embi_element *el, embi_dstr selectors, embi_node **out)
{
    embi_dom_err err = EMBI_DOM_OK;
    const embi_sel_list *l = parse_selectors(c, selectors, &err);
    if (!l) {
        return err;
    }
    embi_sel_ctx sc;
    embi_sel_ctx_init(&sc, el, nullptr);
    *out = nullptr;
    for (embi_node *n = &el->base; n && n->type == EMBI_NODE_ELEMENT; n = n->parent) {
        if (embi_sel_match_list(l, (embi_element *)n, &sc)) {
            *out = n;
            break;
        }
    }
    embi_sel_ctx_free(&sc);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Element_closest(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_node **out)
{
    return embi_impl_closest(c, el_of(self), a0, out);
}

embi_dom_err embi_impl_Element_matches(embi_bind_ctx *c, embi_node *self, embi_dstr a0, bool *out)
{
    return embi_impl_matches(c, el_of(self), a0, out);
}

embi_dom_err embi_impl_Element_webkitMatchesSelector(embi_bind_ctx *c, embi_node *self, embi_dstr a0, bool *out)
{
    return embi_impl_matches(c, el_of(self), a0, out);
}

/* ------------------------------------------------------------------ collections */

embi_js_value embi_impl_Element_getElementsByTagName(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return embi_impl_coll_new(c, EMBI_COLL_TAG, self, embi_bind_utf8(c, a0), k_no_str, false);
}

embi_js_value embi_impl_Element_getElementsByTagNameNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0,
                                                       embi_dstr a1)
{
    return embi_impl_coll_new(c, EMBI_COLL_TAG_NS, self, a0.is_null ? k_no_str : embi_bind_utf8(c, a0.s),
                              embi_bind_utf8(c, a1), !a0.is_null);
}

embi_js_value embi_impl_Element_getElementsByClassName(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return embi_impl_coll_new(c, EMBI_COLL_CLASS, self, embi_bind_utf8(c, a0), k_no_str, false);
}

/* ------------------------------------------------------------------ HTML fragments */

/* The context element for parsing relative to node (a fragment parent or a missing element
 * uses a temporary body element, returned in *temp). */
static embi_element *fragment_context(embi_bind_ctx *c, embi_node *node, embi_node **temp)
{
    (void)c;
    *temp = nullptr;
    if (node->type == EMBI_NODE_ELEMENT &&
        !(node->doc->is_html && node->extra == EMBI_NS_HTML && node->name == EMBI_ATOM_html)) {
        return (embi_element *)node;
    }
    embi_element *body = nullptr;
    if (embi_dom_create_element(node->doc, EMBI_STR("body"), &body) != EMBI_DOM_OK) {
        return nullptr;
    }
    *temp = &body->base;
    return body;
}

static embi_node *parse_html(embi_bind_ctx *c, embi_element *context, embi_dstr html)
{
    embi_html_parser_config cfg = {.scripting = true};
    return embi_html_parse_fragment(context, embi_bind_utf8(c, html), &cfg);
}

static embi_dstr serialize(embi_bind_ctx *c, embi_node *n, bool children)
{
    embi_sb sb = {0};
    if (children) {
        embi_html_serialize_children(n, true, &sb);
    } else {
        embi_html_serialize_node(n, true, &sb);
    }
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
    return d;
}

embi_dom_err embi_impl_Element_get_innerHTML(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    *out = serialize(c, self, true);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Element_set_innerHTML(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_node *target = self;
    if (self->extra == EMBI_NS_HTML && self->name == EMBI_ATOM_template) {
        target = embi_template_contents(el_of(self));
    }
    if (a0.len == 0) {
        return embi_replace_all(target, nullptr);
    }
    embi_node *frag = parse_html(c, el_of(self), a0);
    if (!frag) {
        return embi_impl_throw(c, "SyntaxError", "The HTML could not be parsed.");
    }
    embi_dom_err e = embi_replace_all(target, frag);
    embi_destroy_detached(frag);
    return e;
}

embi_dom_err embi_impl_Element_get_outerHTML(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    *out = serialize(c, self, false);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Element_set_outerHTML(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_node *parent = self->parent;
    if (!parent) {
        return EMBI_DOM_OK;
    }
    if (parent->type == EMBI_NODE_DOCUMENT) {
        return embi_impl_throw(c, "NoModificationAllowedError",
                               "Modifications are not allowed for this document.");
    }
    embi_node *temp;
    embi_element *ctx = fragment_context(c, parent, &temp);
    if (!ctx) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    embi_node *frag = parse_html(c, ctx, a0);
    if (!frag) {
        if (temp) {
            embi_destroy_detached(temp);
        }
        return embi_impl_throw(c, "SyntaxError", "The HTML could not be parsed.");
    }
    embi_dom_err e = embi_replace_child(parent, frag, self);
    embi_destroy_detached(frag);
    if (temp) {
        embi_destroy_detached(temp);
    }
    return e;
}

/* The insertion point of insertAdjacent*: false when there is none (beforebegin without a
 * parent). */
static embi_dom_err adjacent_point(embi_bind_ctx *c, embi_node *self, embi_dstr where, embi_node **parent,
                                   embi_node **before, bool *ok)
{
    embi_str w = embi_bind_utf8(c, where);
    *ok = true;
    if (embi_str_ieq(w, EMBI_STR("beforebegin"))) {
        *parent = self->parent;
        *before = self;
        *ok = *parent != nullptr;
    } else if (embi_str_ieq(w, EMBI_STR("afterbegin"))) {
        *parent = self;
        *before = self->first_child;
    } else if (embi_str_ieq(w, EMBI_STR("beforeend"))) {
        *parent = self;
        *before = nullptr;
    } else if (embi_str_ieq(w, EMBI_STR("afterend"))) {
        *parent = self->parent;
        *before = self->next;
        *ok = *parent != nullptr;
    } else {
        return embi_impl_throw(c, "SyntaxError",
                               "The value provided is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'.");
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Element_insertAdjacentElement(embi_bind_ctx *c, embi_node *self, embi_dstr a0,
                                                     embi_node *a1, embi_node **out)
{
    embi_node *parent = nullptr, *before = nullptr;
    bool ok;
    embi_dom_err e = adjacent_point(c, self, a0, &parent, &before, &ok);
    if (e != EMBI_DOM_OK) {
        return e;
    }
    *out = nullptr;
    if (!ok) {
        return EMBI_DOM_OK;
    }
    e = embi_pre_insert(parent, a1, before);
    if (e == EMBI_DOM_OK) {
        *out = a1;
    }
    return e;
}

embi_dom_err embi_impl_Element_insertAdjacentText(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_dstr a1)
{
    embi_node *parent = nullptr, *before = nullptr;
    bool ok;
    embi_dom_err e = adjacent_point(c, self, a0, &parent, &before, &ok);
    if (e != EMBI_DOM_OK || !ok) {
        return e;
    }
    embi_chardata *t = embi_new_text(self->doc, a1);
    e = embi_pre_insert(parent, &t->base, before);
    if (e != EMBI_DOM_OK) {
        embi_destroy_detached(&t->base);
    }
    return e;
}

embi_dom_err embi_impl_Element_insertAdjacentHTML(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_dstr a1)
{
    embi_node *parent = nullptr, *before = nullptr;
    bool ok;
    embi_dom_err e = adjacent_point(c, self, a0, &parent, &before, &ok);
    if (e != EMBI_DOM_OK) {
        return e;
    }
    if (!ok) {
        return embi_impl_throw(c, "NoModificationAllowedError", "The element has no parent.");
    }
    embi_node *context_node = (parent == self) ? self : parent;
    if (context_node->type == EMBI_NODE_DOCUMENT) {
        return embi_impl_throw(c, "NoModificationAllowedError",
                               "Modifications are not allowed for this document.");
    }
    embi_node *temp;
    embi_element *ctx = fragment_context(c, context_node, &temp);
    if (!ctx) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    embi_node *frag = parse_html(c, ctx, a1);
    if (!frag) {
        if (temp) {
            embi_destroy_detached(temp);
        }
        return embi_impl_throw(c, "SyntaxError", "The HTML could not be parsed.");
    }
    embi_node *dest = parent;
    if (dest->type == EMBI_NODE_ELEMENT && dest->extra == EMBI_NS_HTML && dest->name == EMBI_ATOM_template) {
        dest = embi_template_contents(el_of(dest));
        before = nullptr;
    }
    e = embi_pre_insert(dest, frag, before);
    embi_destroy_detached(frag);
    if (temp) {
        embi_destroy_detached(temp);
    }
    return e;
}

/* ------------------------------------------------------------------ geometry */

static embi_layout_engine *layout_ready(embi_bind_ctx *c)
{
    embi_layout_engine *e = embi_bind_layout(c);
    if (e && !embi_layout_document(e, c->doc)) {
        return nullptr;
    }
    return e;
}

embi_js_value embi_impl_Element_getClientRects(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    embi_rectf rects[64];
    uint32_t n = e ? embi_layout_client_rects_xf(e, el_of(self), rects, 64) : 0;
    double sr[4];
    if (n == 0 && embi_impl_svg_client_bbox(c, self, sr)) {
        rects[0] = (embi_rectf){(float)(sr[0] + c->scroll_x), (float)(sr[1] + c->scroll_y), (float)(sr[2] - sr[0]),
                                (float)(sr[3] - sr[1])};
        n = 1;
    }
    double *d = (double *)embi_bind_alloc(c, (size_t)n * 4 * sizeof *d + 8);
    for (uint32_t i = 0; i < n; i++) {
        d[i * 4 + 0] = (double)rects[i].x - c->scroll_x;
        d[i * 4 + 1] = (double)rects[i].y - c->scroll_y;
        d[i * 4 + 2] = (double)rects[i].w;
        d[i * 4 + 3] = (double)rects[i].h;
    }
    return embi_impl_rectlist_new(c, d, n);
}

embi_js_value embi_impl_Element_getBoundingClientRect(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    embi_rectf r = {0, 0, 0, 0};
    if (!e || !embi_layout_border_box_xf(e, el_of(self), &r)) {
        double sr[4];
        if (embi_impl_svg_client_bbox(c, self, sr)) {
            return embi_impl_rect_new(c, sr[0], sr[1], sr[2] - sr[0], sr[3] - sr[1]);
        }
        r = (embi_rectf){0, 0, 0, 0};
        return embi_impl_rect_new(c, 0, 0, 0, 0);
    }
    return embi_impl_rect_new(c, (double)r.x - c->scroll_x, (double)r.y - c->scroll_y, (double)r.w, (double)r.h);
}

void embi_impl_Element_scrollIntoView(embi_bind_ctx *c, embi_node *self, embi_bind_union a0)
{
    /* Scrolls the viewport so that the element's top is at the top (block "start" by default). */
    (void)a0;
    embi_layout_engine *e = layout_ready(c);
    embi_rectf r;
    if (!e || !embi_layout_border_box(e, el_of(self), &r)) {
        return;
    }
    c->scroll_y = (double)r.y;
    if (c->host.scroll) {
        c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
    }
}

static bool is_scrolling_element(embi_bind_ctx *c, embi_node *self)
{
    embi_element *root = embi_document_element(c->doc);
    return root && (&root->base == self);
}

/* The scroll offsets of a scroll container element other than the viewport's (the host reads them from its fragment). The
 * layout is brought up to date first, so an offset set right after a style change lands on the new fragment. */
static bool el_scroll(embi_bind_ctx *c, embi_node *self, bool set, double *x, double *y)
{
    if (self == nullptr || self->type != EMBI_NODE_ELEMENT || c->host.scroll_el == nullptr || layout_ready(c) == nullptr) {
        return false;
    }
    return c->host.scroll_el(c->host.user, (embi_element *)self, set, x, y);
}

double embi_impl_Element_get_scrollTop(embi_bind_ctx *c, embi_node *self)
{
    double x = 0.0, y = 0.0;
    if (!is_scrolling_element(c, self) && el_scroll(c, self, false, &x, &y)) {
        return y;
    }
    return is_scrolling_element(c, self) ? c->scroll_y : 0.0;
}

void embi_impl_Element_set_scrollTop(embi_bind_ctx *c, embi_node *self, double a0)
{
    if (is_scrolling_element(c, self)) {
        c->scroll_y = (a0 != a0 || a0 < 0) ? 0 : a0;
        if (c->host.scroll) {
            c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
        }
        return;
    }
    double x = 0.0, y = 0.0;
    if (el_scroll(c, self, false, &x, &y)) {
        y = (a0 != a0 || a0 < 0) ? 0 : a0;
        (void)el_scroll(c, self, true, &x, &y);
    }
}

double embi_impl_Element_get_scrollLeft(embi_bind_ctx *c, embi_node *self)
{
    double x = 0.0, y = 0.0;
    if (!is_scrolling_element(c, self) && el_scroll(c, self, false, &x, &y)) {
        return x;
    }
    return is_scrolling_element(c, self) ? c->scroll_x : 0.0;
}

void embi_impl_Element_set_scrollLeft(embi_bind_ctx *c, embi_node *self, double a0)
{
    if (is_scrolling_element(c, self)) {
        c->scroll_x = (a0 != a0 || a0 < 0) ? 0 : a0;
        if (c->host.scroll) {
            c->host.scroll(c->host.user, c->scroll_x, c->scroll_y);
        }
        return;
    }
    double x = 0.0, y = 0.0;
    if (el_scroll(c, self, false, &x, &y)) {
        x = (a0 != a0 || a0 < 0) ? 0 : a0;
        (void)el_scroll(c, self, true, &x, &y);
    }
}

int32_t embi_impl_Element_get_scrollWidth(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float w = 0, h = 0;
    if (e && embi_layout_scroll_size(e, el_of(self), &w, &h)) {
        return (int32_t)(w + 0.5f);
    }
    return 0;
}

int32_t embi_impl_Element_get_scrollHeight(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float w = 0, h = 0;
    if (e && embi_layout_scroll_size(e, el_of(self), &w, &h)) {
        return (int32_t)(h + 0.5f);
    }
    return 0;
}

int32_t embi_impl_Element_get_clientTop(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float b[4] = {0}, p[4] = {0}, m[4] = {0};
    if (e && embi_layout_box_edges(e, el_of(self), b, p, m)) {
        return (int32_t)(b[1] + 0.5f);
    }
    return 0;
}

int32_t embi_impl_Element_get_clientLeft(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float b[4] = {0}, p[4] = {0}, m[4] = {0};
    if (e && embi_layout_box_edges(e, el_of(self), b, p, m)) {
        return (int32_t)(b[0] + 0.5f);
    }
    return 0;
}

int32_t embi_impl_Element_get_clientWidth(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float w = 0, h = 0;
    if (e && embi_layout_client_size(e, el_of(self), &w, &h)) {
        return (int32_t)(w + 0.5f);
    }
    return 0;
}

int32_t embi_impl_Element_get_clientHeight(embi_bind_ctx *c, embi_node *self)
{
    embi_layout_engine *e = layout_ready(c);
    float w = 0, h = 0;
    if (e && embi_layout_client_size(e, el_of(self), &w, &h)) {
        return (int32_t)(h + 0.5f);
    }
    return 0;
}
