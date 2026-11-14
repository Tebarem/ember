/*
 * Node, CharacterData, Text, Comment, ProcessingInstruction, DocumentType, DocumentFragment and
 * Document behind the generated glue in gen/dom.c. The ParentNode and ChildNode mixins are
 * instantiated for each interface that includes them (the generated glue calls one function per
 * includer).
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"

static const embi_str k_no_str = {"", 0};

/* ------------------------------------------------------------------ string helpers */

static embi_dstr dview(embi_dstr d, uint32_t off, uint32_t n)
{
    if (n == 0 || d.ptr == nullptr) {
        return embi_dstr_empty();
    }
    if (d.wide) {
        return (embi_dstr){(const uint16_t *)d.ptr + off, n, true};
    }
    return (embi_dstr){(const uint8_t *)d.ptr + off, n, false};
}

/* A canonical DOM string for units [off, off + n) of d, valid until the scope ends. */
static embi_dstr dslice(embi_bind_ctx *c, embi_dstr d, uint32_t off, uint32_t n)
{
    if (n == 0 || d.ptr == nullptr) {
        return embi_dstr_empty();
    }
    if (!d.wide) {
        return dview(d, off, n);
    }
    return embi_bind_tmp_units(c, (const uint16_t *)d.ptr + off, n);
}

/* ------------------------------------------------------------------ Node */

uint16_t embi_impl_Node_get_nodeType(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return (self->type == EMBI_NODE_TEXT && (self->flags & EMBI_NF_CDATA)) ? 4 : self->type;
}

embi_dstr embi_impl_Node_get_nodeName(embi_bind_ctx *c, embi_node *self)
{
    switch (self->type) {
    case EMBI_NODE_ELEMENT:
        return embi_impl_Element_get_tagName(c, self);
    case EMBI_NODE_TEXT:
        return (self->flags & EMBI_NF_CDATA) ? embi_impl_lit("#cdata-section") : embi_impl_lit("#text");
    case EMBI_NODE_COMMENT:
        return embi_impl_lit("#comment");
    case EMBI_NODE_DOCUMENT:
        return embi_impl_lit("#document");
    case EMBI_NODE_FRAGMENT:
        return embi_impl_lit("#document-fragment");
    case EMBI_NODE_PI:
        return embi_bind_tmp_utf8(c, embi_atom_str(self->name));
    case EMBI_NODE_DOCTYPE:
        return ((embi_doctype *)self)->name;
    case EMBI_NODE_ATTR:
        return embi_impl_attr_qname(c, (embi_attr_node *)self);
    default:
        return embi_dstr_empty();
    }
}

embi_dstr embi_impl_Node_get_baseURI(embi_bind_ctx *c, embi_node *self)
{
    embi_document *d = self->type == EMBI_NODE_ATTR ? embi_attr_node_doc((embi_attr_node *)self) : self->doc;
    if (d != c->doc) {
        return embi_impl_lit("about:blank");
    }
    return embi_bind_tmp_utf8(c, embi_bind_url(c));
}

bool embi_impl_Node_get_isConnected(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return (self->flags & EMBI_NF_CONNECTED) != 0 || self->type == EMBI_NODE_DOCUMENT;
}

embi_node *embi_impl_Node_get_ownerDocument(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    if (self->type == EMBI_NODE_ATTR) {
        return &embi_attr_node_doc((embi_attr_node *)self)->base;
    }
    return self->type == EMBI_NODE_DOCUMENT ? nullptr : &self->doc->base;
}

embi_node *embi_impl_Node_getRootNode(embi_bind_ctx *c, embi_node *self, embi_dict_GetRootNodeOptions a0)
{
    (void)c;
    (void)a0;
    return embi_get_root(self);
}

embi_node *embi_impl_Node_get_parentNode(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->parent;
}

embi_node *embi_impl_Node_get_parentElement(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return (self->parent && self->parent->type == EMBI_NODE_ELEMENT) ? self->parent : nullptr;
}

bool embi_impl_Node_hasChildNodes(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->first_child != nullptr;
}

embi_js_value embi_impl_Node_get_childNodes(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_coll_new(c, EMBI_COLL_CHILDNODES, self, k_no_str, k_no_str, false);
}

embi_node *embi_impl_Node_get_firstChild(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->first_child;
}

embi_node *embi_impl_Node_get_lastChild(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->last_child;
}

embi_node *embi_impl_Node_get_previousSibling(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->prev;
}

embi_node *embi_impl_Node_get_nextSibling(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return self->next;
}

static bool is_chardata(const embi_node *n)
{
    return n->type == EMBI_NODE_TEXT || n->type == EMBI_NODE_COMMENT || n->type == EMBI_NODE_PI;
}

embi_js_value embi_impl_Node_get_nodeValue(embi_bind_ctx *c, embi_node *self)
{
    if (is_chardata(self)) {
        return embi_bind_dstr_value(c, ((embi_chardata *)self)->data);
    }
    if (self->type == EMBI_NODE_ATTR) {
        return embi_bind_dstr_value(c, embi_attr_node_value((embi_attr_node *)self));
    }
    return EMBI_JS_NULL;
}

void embi_impl_Node_set_nodeValue(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    (void)c;
    if (is_chardata(self)) {
        embi_chardata_set((embi_chardata *)self, a0.is_null ? embi_dstr_empty() : a0.s);
    } else if (self->type == EMBI_NODE_ATTR) {
        embi_attr_node_set_value((embi_attr_node *)self, a0.is_null ? embi_dstr_empty() : a0.s);
    }
}

embi_js_value embi_impl_Node_get_textContent(embi_bind_ctx *c, embi_node *self)
{
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    if (!embi_text_content_get(self, &b)) {
        embi_dbuf_free(&b);
        return EMBI_JS_NULL;
    }
    embi_dstr d = embi_dbuf_finish(&b);
    embi_js_value v = embi_bind_dstr_value(c, d);
    embi_dstr_free(d);
    embi_dbuf_free(&b);
    return v;
}

void embi_impl_Node_set_textContent(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    (void)c;
    if (self->type == EMBI_NODE_DOCUMENT || self->type == EMBI_NODE_DOCTYPE) {
        return;
    }
    (void)embi_text_content_set(self, a0.is_null ? embi_dstr_empty() : a0.s);
}

void embi_impl_Node_normalize(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_normalize(self);
}

embi_dom_err embi_impl_Node_cloneNode(embi_bind_ctx *c, embi_node *self, bool a0, embi_node **out)
{
    if (self->type == EMBI_NODE_DOCUMENT) {
        /* the copy is a document made by script: the context owns and observes it */
        embi_node *copy = embi_clone_node(self, a0);
        embi_bind_add_xdoc(c, (embi_document *)copy);
        *out = copy;
        return EMBI_DOM_OK;
    }
    *out = embi_clone_node(self, a0);
    return EMBI_DOM_OK;
}

bool embi_impl_Node_isEqualNode(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    (void)c;
    return a0 && embi_is_equal_node(self, a0);
}

bool embi_impl_Node_isSameNode(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    (void)c;
    return self == a0;
}

uint16_t embi_impl_Node_compareDocumentPosition(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    (void)c;
    return embi_compare_position(self, a0);
}

bool embi_impl_Node_contains(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    (void)c;
    return a0 && embi_contains(self, a0);
}

/* The element a namespace lookup starts from. */
static embi_element *lookup_start(embi_node *n)
{
    switch (n->type) {
    case EMBI_NODE_ELEMENT:
        return (embi_element *)n;
    case EMBI_NODE_DOCUMENT:
        return (embi_element *)embi_document_element((embi_document *)n);
    case EMBI_NODE_DOCTYPE:
    case EMBI_NODE_FRAGMENT:
        return nullptr;
    case EMBI_NODE_ATTR:
        return ((embi_attr_node *)n)->owner;
    default:
        return (n->parent && n->parent->type == EMBI_NODE_ELEMENT) ? (embi_element *)n->parent : nullptr;
    }
}

/* Locate a namespace: the prefix is nullptr (len 0 with has_prefix false) for the default. */
static bool locate_ns(embi_bind_ctx *c, embi_element *el, bool has_prefix, embi_str prefix, embi_str *uri)
{
    for (embi_node *n = &el->base; n && n->type == EMBI_NODE_ELEMENT; n = n->parent) {
        embi_element *e = (embi_element *)n;
        embi_str ens = embi_el_namespace_uri(e);
        if (ens.len && ((e->prefix == 0 && !has_prefix) ||
                        (e->prefix != 0 && has_prefix &&
                         embi_str_eq(embi_atom_str(e->prefix), prefix)))) {
            *uri = ens;
            return true;
        }
        uint32_t cnt = embi_el_attr_count(e);
        for (uint32_t i = 0; i < cnt; i++) {
            const embi_attr *a = embi_el_attr_at(e, i);
            if (a->ns != EMBI_NS_XMLNS) {
                continue;
            }
            embi_str local = embi_atom_str(a->name);
            bool match = has_prefix ? (a->prefix != 0 && embi_str_eq(local, prefix))
                                    : (a->prefix == 0 && embi_str_eq(local, EMBI_STR("xmlns")));
            if (match) {
                *uri = a->value.len != 0 ? embi_bind_utf8(c, a->value) : k_no_str;
                return a->value.len != 0;
            }
        }
    }
    return false;
}

embi_js_value embi_impl_Node_lookupNamespaceURI(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    embi_element *start = lookup_start(self);
    if (!start) {
        return EMBI_JS_NULL;
    }
    bool has_prefix = !a0.is_null && a0.s.len != 0;
    embi_str prefix = has_prefix ? embi_bind_utf8(c, a0.s) : k_no_str;
    if (has_prefix && embi_str_eq(prefix, EMBI_STR("xml"))) {
        return embi_bind_utf8_value(c, EMBI_STR("http://www.w3.org/XML/1998/namespace"));
    }
    if (has_prefix && embi_str_eq(prefix, EMBI_STR("xmlns"))) {
        return embi_bind_utf8_value(c, EMBI_STR("http://www.w3.org/2000/xmlns/"));
    }
    embi_str uri;
    if (locate_ns(c, start, has_prefix, prefix, &uri) && uri.len) {
        return embi_bind_utf8_value(c, uri);
    }
    return EMBI_JS_NULL;
}

embi_js_value embi_impl_Node_lookupPrefix(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    if (a0.is_null || a0.s.len == 0) {
        return EMBI_JS_NULL;
    }
    embi_element *start = lookup_start(self);
    embi_str want = embi_bind_utf8(c, a0.s);
    for (embi_node *n = start ? &start->base : nullptr; n && n->type == EMBI_NODE_ELEMENT; n = n->parent) {
        embi_element *e = (embi_element *)n;
        if (e->prefix != 0 && embi_str_eq(embi_el_namespace_uri(e), want)) {
            return embi_bind_utf8_value(c, embi_atom_str(e->prefix));
        }
    }
    return EMBI_JS_NULL;
}

bool embi_impl_Node_isDefaultNamespace(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    embi_element *start = lookup_start(self);
    embi_str uri = k_no_str;
    bool found = start && locate_ns(c, start, false, k_no_str, &uri);
    embi_str want = a0.is_null ? k_no_str : embi_bind_utf8(c, a0.s);
    return found ? embi_str_eq(uri, want) : want.len == 0;
}

embi_dom_err embi_impl_Node_insertBefore(embi_bind_ctx *c, embi_node *self, embi_node *a0,
                                         embi_node *a1, embi_node **out)
{
    (void)c;
    embi_dom_err e = embi_pre_insert(self, a0, a1);
    if (e == EMBI_DOM_OK) {
        *out = a0;
    }
    return e;
}

embi_dom_err embi_impl_Node_appendChild(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    (void)c;
    embi_dom_err e = embi_append_child(self, a0);
    if (e == EMBI_DOM_OK) {
        *out = a0;
    }
    return e;
}

embi_dom_err embi_impl_Node_replaceChild(embi_bind_ctx *c, embi_node *self, embi_node *a0,
                                         embi_node *a1, embi_node **out)
{
    (void)c;
    embi_dom_err e = embi_replace_child(self, a0, a1);
    if (e == EMBI_DOM_OK) {
        *out = a1;
    }
    return e;
}

embi_dom_err embi_impl_Node_removeChild(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    (void)c;
    embi_dom_err e = embi_remove_child(self, a0);
    if (e == EMBI_DOM_OK) {
        *out = a0;
    }
    return e;
}

/* ------------------------------------------------------------------ ParentNode and ChildNode */

/* Turns a list of Node or DOMString into one node: the node itself, a Text node, or a fragment of
 * them (nullptr for an empty list). *fresh is set when the result is new (and has to be destroyed
 * when it is not inserted). */
static embi_dom_err nodes_to_node(embi_document *doc, int n, const embi_bind_union *a, embi_node **out,
                                  bool *fresh)
{
    *out = nullptr;
    *fresh = false;
    if (n == 0) {
        return EMBI_DOM_OK;
    }
    if (n == 1 && a[0].kind == 1) {
        *out = a[0].u.node;
        return EMBI_DOM_OK;
    }
    if (n == 1) {
        *out = &embi_new_text(doc, a[0].u.str)->base;
        *fresh = true;
        return EMBI_DOM_OK;
    }
    embi_node *frag = embi_new_fragment(doc);
    for (int i = 0; i < n; i++) {
        embi_node *item = a[i].kind == 1 ? a[i].u.node : &embi_new_text(doc, a[i].u.str)->base;
        embi_dom_err e = embi_append_child(frag, item);
        if (e != EMBI_DOM_OK) {
            if (a[i].kind != 1) {
                embi_destroy_detached(item);
            }
            embi_destroy_detached(frag);
            return e;
        }
    }
    *out = frag;
    *fresh = true;
    return EMBI_DOM_OK;
}

/* Frees a fresh result that was inserted (a fragment is left empty) or not. */
static void release_fresh(embi_node *node, bool fresh)
{
    if (fresh && node && node->parent == nullptr) {
        embi_destroy_detached(node);
    }
}

static embi_dom_err parent_prepend(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK || !node) {
        return e;
    }
    e = embi_pre_insert(self, node, self->first_child);
    release_fresh(node, fresh);
    return e;
}

static embi_dom_err parent_append(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK || !node) {
        return e;
    }
    e = embi_append_child(self, node);
    release_fresh(node, fresh);
    return e;
}

static embi_dom_err parent_replace_children(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK) {
        return e;
    }
    e = embi_replace_all(self, node);
    release_fresh(node, fresh);
    return e;
}

static bool in_list(const embi_node *x, int n, const embi_bind_union *a)
{
    for (int i = 0; i < n; i++) {
        if (a[i].kind == 1 && a[i].u.node == x) {
            return true;
        }
    }
    return false;
}

static embi_dom_err child_before(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *parent = self->parent;
    if (!parent) {
        return EMBI_DOM_OK;
    }
    embi_node *prev = self->prev;
    while (prev && in_list(prev, n, a)) {
        prev = prev->prev;
    }
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK || !node) {
        return e;
    }
    prev = prev ? prev->next : parent->first_child;
    e = embi_pre_insert(parent, node, prev);
    release_fresh(node, fresh);
    return e;
}

static embi_dom_err child_after(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *parent = self->parent;
    if (!parent) {
        return EMBI_DOM_OK;
    }
    embi_node *next = self->next;
    while (next && in_list(next, n, a)) {
        next = next->next;
    }
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK || !node) {
        return e;
    }
    e = embi_pre_insert(parent, node, next);
    release_fresh(node, fresh);
    return e;
}

static embi_dom_err child_replace_with(embi_node *self, int n, const embi_bind_union *a)
{
    embi_node *parent = self->parent;
    if (!parent) {
        return EMBI_DOM_OK;
    }
    embi_node *next = self->next;
    while (next && in_list(next, n, a)) {
        next = next->next;
    }
    embi_node *node;
    bool fresh;
    embi_dom_err e = nodes_to_node(self->doc, n, a, &node, &fresh);
    if (e != EMBI_DOM_OK) {
        return e;
    }
    if (!node) {
        return embi_remove_child(parent, self);
    }
    if (self->parent == parent) {
        e = embi_replace_child(parent, node, self);
    } else {
        e = embi_pre_insert(parent, node, next);
    }
    release_fresh(node, fresh);
    return e;
}

static void child_remove(embi_node *self)
{
    if (self->parent) {
        (void)embi_remove_child(self->parent, self);
    }
}

static embi_node *first_element_child(embi_node *n)
{
    for (embi_node *k = n->first_child; k; k = k->next) {
        if (k->type == EMBI_NODE_ELEMENT) {
            return k;
        }
    }
    return nullptr;
}

static embi_node *last_element_child(embi_node *n)
{
    for (embi_node *k = n->last_child; k; k = k->prev) {
        if (k->type == EMBI_NODE_ELEMENT) {
            return k;
        }
    }
    return nullptr;
}

static uint32_t element_child_count(embi_node *n)
{
    uint32_t cnt = 0;
    for (embi_node *k = n->first_child; k; k = k->next) {
        cnt += k->type == EMBI_NODE_ELEMENT;
    }
    return cnt;
}

static embi_node *prev_element_sibling(embi_node *n)
{
    for (embi_node *k = n->prev; k; k = k->prev) {
        if (k->type == EMBI_NODE_ELEMENT) {
            return k;
        }
    }
    return nullptr;
}

static embi_node *next_element_sibling(embi_node *n)
{
    for (embi_node *k = n->next; k; k = k->next) {
        if (k->type == EMBI_NODE_ELEMENT) {
            return k;
        }
    }
    return nullptr;
}

static embi_node *get_by_id_in(embi_node *root, embi_str id)
{
    if (id.len == 0) {
        return nullptr; /* the empty string never matches an id (DOM: getElementById) */
    }
    for (embi_node *n = embi_next_in_tree(root, root); n; n = embi_next_in_tree(root, n)) {
        if (n->type == EMBI_NODE_ELEMENT) {
            const embi_attr *a = embi_el_get_attribute((embi_element *)n, EMBI_STR("id"));
            if (a && embi_dstr_eq_utf8(a->value, id)) {
                return n;
            }
        }
    }
    return nullptr;
}

/* Node.moveBefore (DOM Standard "move"): validity checks, then the node leaves its parent and goes in before child. The
 * focused element inside the moved node keeps the focus. */
static embi_dom_err parent_move_before(embi_bind_ctx *c, embi_node *self, embi_node *node, embi_node *child)
{
    if (embi_contains(node, self)) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (child && child->parent != self) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    if (node->type != EMBI_NODE_ELEMENT && !is_chardata(node)) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (self->type == EMBI_NODE_DOCUMENT || embi_get_root(node) != embi_get_root(self)) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    embi_node *ref = child == node ? node->next : child;
    embi_node *active = c->active;
    c->moving = true;
    embi_detach(node);
    c->moving = false;
    embi_dom_err e = embi_pre_insert(self, node, ref);
    if (active && embi_contains(node, active) && (node->flags & EMBI_NF_CONNECTED)) {
        c->active = active;
    }
    return e;
}

#define PARENT_NODE_IMPL(I)                                                                            \
    embi_js_value embi_impl_##I##_get_children(embi_bind_ctx *c, embi_node *self)                     \
    {                                                                                                  \
        return embi_impl_coll_new(c, EMBI_COLL_CHILDREN, self, k_no_str, k_no_str, false);             \
    }                                                                                                  \
    embi_node *embi_impl_##I##_get_firstElementChild(embi_bind_ctx *c, embi_node *self)               \
    {                                                                                                  \
        (void)c;                                                                                       \
        return first_element_child(self);                                                              \
    }                                                                                                  \
    embi_node *embi_impl_##I##_get_lastElementChild(embi_bind_ctx *c, embi_node *self)                \
    {                                                                                                  \
        (void)c;                                                                                       \
        return last_element_child(self);                                                               \
    }                                                                                                  \
    uint32_t embi_impl_##I##_get_childElementCount(embi_bind_ctx *c, embi_node *self)                 \
    {                                                                                                  \
        (void)c;                                                                                       \
        return element_child_count(self);                                                              \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_prepend(embi_bind_ctx *c, embi_node *self, int n,                    \
                                         const embi_bind_union *a)                                     \
    {                                                                                                  \
        (void)c;                                                                                       \
        return parent_prepend(self, n, a);                                                             \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_append(embi_bind_ctx *c, embi_node *self, int n,                     \
                                        const embi_bind_union *a)                                      \
    {                                                                                                  \
        (void)c;                                                                                       \
        return parent_append(self, n, a);                                                              \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_replaceChildren(embi_bind_ctx *c, embi_node *self, int n,            \
                                                 const embi_bind_union *a)                             \
    {                                                                                                  \
        (void)c;                                                                                       \
        return parent_replace_children(self, n, a);                                                    \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_moveBefore(embi_bind_ctx *c, embi_node *self, embi_node *a0,         \
                                            embi_node *a1)                                             \
    {                                                                                                  \
        return parent_move_before(c, self, a0, a1);                                                    \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_querySelector(embi_bind_ctx *c, embi_node *self, embi_dstr s,        \
                                               embi_node **out)                                        \
    {                                                                                                  \
        return embi_impl_query_first(c, self, self->type == EMBI_NODE_ELEMENT ? (embi_element *)self   \
                                                                              : nullptr,               \
                                     s, out);                                                          \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_querySelectorAll(embi_bind_ctx *c, embi_node *self, embi_dstr s,     \
                                                  embi_js_value *out)                                  \
    {                                                                                                  \
        return embi_impl_query_all(c, self, self->type == EMBI_NODE_ELEMENT ? (embi_element *)self     \
                                                                            : nullptr,                 \
                                   s, out);                                                            \
    }

#define CHILD_NODE_IMPL(I)                                                                             \
    embi_dom_err embi_impl_##I##_before(embi_bind_ctx *c, embi_node *self, int n,                     \
                                        const embi_bind_union *a)                                      \
    {                                                                                                  \
        (void)c;                                                                                       \
        return child_before(self, n, a);                                                               \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_after(embi_bind_ctx *c, embi_node *self, int n,                      \
                                       const embi_bind_union *a)                                       \
    {                                                                                                  \
        (void)c;                                                                                       \
        return child_after(self, n, a);                                                                \
    }                                                                                                  \
    embi_dom_err embi_impl_##I##_replaceWith(embi_bind_ctx *c, embi_node *self, int n,                \
                                             const embi_bind_union *a)                                 \
    {                                                                                                  \
        (void)c;                                                                                       \
        return child_replace_with(self, n, a);                                                         \
    }                                                                                                  \
    void embi_impl_##I##_remove(embi_bind_ctx *c, embi_node *self)                                    \
    {                                                                                                  \
        (void)c;                                                                                       \
        child_remove(self);                                                                            \
    }

#define NON_DOC_TYPE_CHILD_NODE_IMPL(I)                                                                \
    embi_node *embi_impl_##I##_get_previousElementSibling(embi_bind_ctx *c, embi_node *self)          \
    {                                                                                                  \
        (void)c;                                                                                       \
        return prev_element_sibling(self);                                                             \
    }                                                                                                  \
    embi_node *embi_impl_##I##_get_nextElementSibling(embi_bind_ctx *c, embi_node *self)              \
    {                                                                                                  \
        (void)c;                                                                                       \
        return next_element_sibling(self);                                                             \
    }

PARENT_NODE_IMPL(Document)
PARENT_NODE_IMPL(DocumentFragment)
PARENT_NODE_IMPL(Element)
CHILD_NODE_IMPL(CharacterData)
CHILD_NODE_IMPL(DocumentType)
CHILD_NODE_IMPL(Element)
NON_DOC_TYPE_CHILD_NODE_IMPL(CharacterData)
NON_DOC_TYPE_CHILD_NODE_IMPL(Element)

embi_node *embi_impl_Document_getElementById(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_str id = embi_bind_utf8(c, a0);
    if (id.len == 0) {
        return nullptr;
    }
    embi_element *e = embi_get_element_by_id((embi_document *)self, id);
    return e ? &e->base : nullptr;
}

embi_node *embi_impl_DocumentFragment_getElementById(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return get_by_id_in(self, embi_bind_utf8(c, a0));
}

/* ------------------------------------------------------------------ CharacterData */

embi_dstr embi_impl_CharacterData_get_data(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ((embi_chardata *)self)->data;
}

void embi_impl_CharacterData_set_data(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    embi_chardata_set((embi_chardata *)self, a0);
}

uint32_t embi_impl_CharacterData_get_length(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ((embi_chardata *)self)->data.len;
}

static embi_dom_err index_error(embi_bind_ctx *c)
{
    return embi_impl_throw(c, "IndexSizeError", "The index is not in the allowed range.");
}

embi_dom_err embi_impl_CharacterData_substringData(embi_bind_ctx *c, embi_node *self, uint32_t a0,
                                                   uint32_t a1, embi_dstr *out)
{
    embi_dstr d = ((embi_chardata *)self)->data;
    if (a0 > d.len) {
        return index_error(c);
    }
    uint32_t n = a1 > d.len - a0 ? d.len - a0 : a1;
    *out = dslice(c, d, a0, n);
    return EMBI_DOM_OK;
}

void embi_impl_CharacterData_appendData(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_chardata *cd = (embi_chardata *)self;
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    embi_dbuf_append(&b, cd->data);
    embi_dbuf_append(&b, a0);
    embi_dstr fresh = embi_dbuf_finish(&b);
    embi_chardata_set(cd, fresh);
    embi_dstr_free(fresh);
    embi_dbuf_free(&b);
    (void)c;
}

static embi_dom_err replace_data(embi_bind_ctx *c, embi_node *self, uint32_t off, uint32_t count,
                                 embi_dstr data)
{
    embi_chardata *cd = (embi_chardata *)self;
    embi_dstr old = cd->data;
    if (off > old.len) {
        return index_error(c);
    }
    uint32_t n = count > old.len - off ? old.len - off : count;
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    embi_dbuf_append(&b, dview(old, 0, off));
    embi_dbuf_append(&b, data);
    embi_dbuf_append(&b, dview(old, off + n, old.len - off - n));
    embi_dstr fresh = embi_dbuf_finish(&b);
    embi_chardata_set(cd, fresh);
    embi_dstr_free(fresh);
    embi_dbuf_free(&b);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CharacterData_insertData(embi_bind_ctx *c, embi_node *self, uint32_t a0, embi_dstr a1)
{
    return replace_data(c, self, a0, 0, a1);
}

embi_dom_err embi_impl_CharacterData_deleteData(embi_bind_ctx *c, embi_node *self, uint32_t a0, uint32_t a1)
{
    return replace_data(c, self, a0, a1, embi_dstr_empty());
}

embi_dom_err embi_impl_CharacterData_replaceData(embi_bind_ctx *c, embi_node *self, uint32_t a0,
                                                 uint32_t a1, embi_dstr a2)
{
    return replace_data(c, self, a0, a1, a2);
}

/* ------------------------------------------------------------------ Text, Comment, PI, DocumentType */

embi_js_value embi_impl_Text_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0)
{
    embi_js_value v = embi_bind_node_value(c, &embi_new_text(c->doc, a0)->base);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_js_value embi_impl_Comment_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_dstr a0)
{
    embi_js_value v = embi_bind_node_value(c, &embi_new_comment(c->doc, a0)->base);
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_dom_err embi_impl_Text_splitText(embi_bind_ctx *c, embi_node *self, uint32_t a0, embi_node **out)
{
    embi_chardata *cd = (embi_chardata *)self;
    embi_dstr d = cd->data;
    if (a0 > d.len) {
        return index_error(c);
    }
    embi_dstr tail = dview(d, a0, d.len - a0);
    embi_chardata *nt = embi_new_text(self->doc, tail);
    if (self->parent) {
        (void)embi_pre_insert(self->parent, &nt->base, self->next);
    }
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    embi_dbuf_append(&b, dview(d, 0, a0));
    embi_dstr head = embi_dbuf_finish(&b);
    embi_chardata_set(cd, head);
    embi_dstr_free(head);
    embi_dbuf_free(&b);
    *out = &nt->base;
    return EMBI_DOM_OK;
}

embi_dstr embi_impl_Text_get_wholeText(embi_bind_ctx *c, embi_node *self)
{
    embi_node *first = self;
    while (first->prev && first->prev->type == EMBI_NODE_TEXT) {
        first = first->prev;
    }
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    for (embi_node *n = first; n && n->type == EMBI_NODE_TEXT; n = n->next) {
        embi_dbuf_append(&b, ((embi_chardata *)n)->data);
    }
    embi_dstr d = embi_dbuf_finish(&b);
    embi_dstr copy = embi_bind_tmp_copy(c, d);
    embi_dstr_free(d);
    embi_dbuf_free(&b);
    return copy;
}

embi_dstr embi_impl_ProcessingInstruction_get_target(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_utf8(c, embi_atom_str(self->name));
}

embi_dstr embi_impl_DocumentType_get_name(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ((embi_doctype *)self)->name;
}

embi_dstr embi_impl_DocumentType_get_publicId(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ((embi_doctype *)self)->public_id;
}

embi_dstr embi_impl_DocumentType_get_systemId(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return ((embi_doctype *)self)->system_id;
}

embi_js_value embi_impl_DocumentFragment_constructor(embi_bind_ctx *c, embi_js_value new_target)
{
    embi_js_value v = embi_bind_node_value(c, embi_new_fragment(c->doc));
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

/* ------------------------------------------------------------------ Document */

static embi_document *doc_of(embi_node *self)
{
    return (embi_document *)self;
}

embi_dstr embi_impl_Document_get_URL(embi_bind_ctx *c, embi_node *self)
{
    if (self != &c->doc->base) {
        return embi_impl_lit("about:blank");
    }
    return embi_bind_tmp_utf8(c, embi_bind_url(c));
}

embi_dstr embi_impl_Document_get_documentURI(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_URL(c, self);
}

embi_dstr embi_impl_Document_get_compatMode(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return embi_impl_lit(doc_of(self)->quirks == EMBI_QUIRKS_FULL ? "BackCompat" : "CSS1Compat");
}

embi_dstr embi_impl_Document_get_characterSet(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("UTF-8");
}

embi_dstr embi_impl_Document_get_charset(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_characterSet(c, self);
}

embi_dstr embi_impl_Document_get_inputEncoding(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_characterSet(c, self);
}

embi_dstr embi_impl_Document_get_contentType(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    static const char *const names[] = {"text/html", "application/xml", "application/xhtml+xml", "image/svg+xml"};
    return embi_impl_lit(names[doc_of(self)->ctype]);
}

embi_node *embi_impl_Document_get_doctype(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    for (embi_node *k = self->first_child; k; k = k->next) {
        if (k->type == EMBI_NODE_DOCTYPE) {
            return k;
        }
    }
    return nullptr;
}

embi_node *embi_impl_Document_get_documentElement(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_element *e = embi_document_element(doc_of(self));
    return e ? &e->base : nullptr;
}

/* CSSOM View: the element whose scrollTop and scrollLeft are the viewport's. Ember maps the viewport to the document
 * element in quirks mode too (Element.scrollTop), so that is the answer in both modes. */
embi_node *embi_impl_Document_get_scrollingElement(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_documentElement(c, self);
}

embi_js_value embi_impl_Document_getElementsByTagName(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return embi_impl_coll_new(c, EMBI_COLL_TAG, self, embi_bind_utf8(c, a0), k_no_str, false);
}

embi_js_value embi_impl_Document_getElementsByTagNameNS(embi_bind_ctx *c, embi_node *self,
                                                        embi_bind_nstr a0, embi_dstr a1)
{
    return embi_impl_coll_new(c, EMBI_COLL_TAG_NS, self, a0.is_null ? k_no_str : embi_bind_utf8(c, a0.s),
                              embi_bind_utf8(c, a1), !a0.is_null);
}

embi_js_value embi_impl_Document_getElementsByClassName(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return embi_impl_coll_new(c, EMBI_COLL_CLASS, self, embi_bind_utf8(c, a0), k_no_str, false);
}

embi_dom_err embi_impl_Document_createElement(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_node **out)
{
    embi_element *el = nullptr;
    embi_dom_err e = embi_dom_create_element(doc_of(self), embi_bind_utf8(c, a0), &el);
    if (e == EMBI_DOM_OK) {
        *out = &el->base;
    }
    return e;
}

embi_dom_err embi_impl_Document_createElementNS(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0,
                                                embi_dstr a1, embi_node **out)
{
    embi_element *el = nullptr;
    embi_str ns = (a0.is_null || a0.s.len == 0) ? k_no_str : embi_bind_utf8(c, a0.s);
    embi_dom_err e = embi_dom_create_element_ns(doc_of(self), ns.len != 0, ns, embi_bind_utf8(c, a1), &el);
    if (e == EMBI_DOM_OK) {
        *out = &el->base;
    }
    return e;
}

embi_node *embi_impl_Document_createDocumentFragment(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return embi_new_fragment(doc_of(self));
}

embi_node *embi_impl_Document_createTextNode(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    return &embi_new_text(doc_of(self), a0)->base;
}

embi_node *embi_impl_Document_createComment(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    return &embi_new_comment(doc_of(self), a0)->base;
}

embi_dom_err embi_impl_Document_createCDATASection(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_node **out)
{
    embi_document *d = doc_of(self);
    if (d->is_html) {
        return embi_impl_throw(c, "NotSupportedError", "This operation is not supported for HTML documents.");
    }
    embi_str s = embi_bind_utf8(c, a0);
    for (uint32_t i = 0; i + 2 < s.len; i++) {
        if (s.ptr[i] == ']' && s.ptr[i + 1] == ']' && s.ptr[i + 2] == '>') {
            return embi_impl_throw(c, "InvalidCharacterError", "The data contains ']]>'.");
        }
    }
    embi_node *t = &embi_new_text(d, a0)->base;
    t->flags |= EMBI_NF_CDATA;
    *out = t;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Document_createProcessingInstruction(embi_bind_ctx *c, embi_node *self,
                                                            embi_dstr a0, embi_dstr a1, embi_node **out)
{
    embi_chardata *cd = nullptr;
    embi_dom_err e = embi_dom_create_pi(doc_of(self), embi_bind_utf8(c, a0), a1, &cd);
    if (e == EMBI_DOM_OK) {
        *out = &cd->base;
    }
    return e;
}

embi_dom_err embi_impl_Document_importNode(embi_bind_ctx *c, embi_node *self, embi_node *a0, bool a1,
                                           embi_node **out)
{
    (void)c;
    if (a0->type == EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    embi_node *n = embi_import_node(doc_of(self), a0, a1);
    if (!n) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    *out = n;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Document_adoptNode(embi_bind_ctx *c, embi_node *self, embi_node *a0, embi_node **out)
{
    (void)c;
    if (a0->type == EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    embi_dom_err e = embi_adopt_node(doc_of(self), a0);
    if (e == EMBI_DOM_OK) {
        *out = a0;
    }
    return e;
}

embi_dom_err embi_impl_Document_createEvent(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_js_value *out)
{
    (void)self;
    return embi_impl_create_event(c, a0, out);
}

embi_dstr embi_impl_Document_get_domain(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return embi_dstr_empty();
}

embi_dstr embi_impl_Document_get_referrer(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_domain(c, self);
}

embi_dstr embi_impl_Document_get_cookie(embi_bind_ctx *c, embi_node *self)
{
    if (c->host.cookie_get == nullptr || self->doc != c->doc) {
        return embi_impl_Document_get_domain(c, self);
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_dstr r = embi_dstr_empty();
    if (c->host.cookie_get(c->host.user, &sb)) {
        r = embi_bind_tmp_utf8(c, (embi_str){sb.data != nullptr ? sb.data : "", sb.len});
    }
    embi_sb_free(&sb);
    return r;
}

void embi_impl_Document_set_cookie(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    if (c->host.cookie_set == nullptr || self->doc != c->doc) {
        return;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_dstr_append_utf8(&sb, a0);
    (void)c->host.cookie_set(c->host.user, (embi_str){sb.data != nullptr ? sb.data : "", sb.len});
    embi_sb_free(&sb);
}

embi_dstr embi_impl_Document_get_lastModified(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_Document_get_domain(c, self);
}

embi_dstr embi_impl_Document_get_readyState(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    /* The embedder tracks the parse and load (W5c): 0 loading, 1 interactive, 2 complete. */
    int st = c->host.ready_state != nullptr ? c->host.ready_state(c->host.user) : 2;
    return embi_impl_lit(st <= 0 ? "loading" : (st == 1 ? "interactive" : "complete"));
}

embi_js_value embi_impl_Document_get_defaultView(embi_bind_ctx *c, embi_node *self)
{
    return self == &c->doc->base ? embi_bind_obj_value(c->gbo) : EMBI_JS_NULL;
}

embi_node *embi_impl_Document_get_activeElement(embi_bind_ctx *c, embi_node *self)
{
    if (c->active && (c->active->flags & EMBI_NF_CONNECTED)) {
        return c->active;
    }
    embi_node *body = nullptr;
    (void)embi_impl_Document_get_body(c, self, &body);
    return body ? body : embi_impl_Document_get_documentElement(c, self);
}

bool embi_impl_Document_hasFocus(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return c->active != nullptr;
}

bool embi_impl_Document_get_hidden(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return false;
}

embi_dstr embi_impl_Document_get_visibilityState(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("visible");
}

static bool is_html_el(const embi_node *n, embi_atom name)
{
    return n && n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_HTML && n->name == name;
}

static embi_node *find_title(embi_node *doc)
{
    for (embi_node *n = embi_next_in_tree(doc, doc); n; n = embi_next_in_tree(doc, n)) {
        if (is_html_el(n, EMBI_ATOM_title)) {
            return n;
        }
    }
    return nullptr;
}

embi_dstr embi_impl_Document_get_title(embi_bind_ctx *c, embi_node *self)
{
    embi_node *t = find_title(self);
    if (!t) {
        return embi_dstr_empty();
    }
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    (void)embi_text_content_get(t, &b);
    embi_dstr d = embi_dbuf_finish(&b);
    /* strip and collapse ASCII white space */
    embi_dbuf o;
    embi_dbuf_init(&o, self->doc->slab);
    bool pending = false;
    for (uint32_t i = 0; i < d.len; i++) {
        uint16_t u = embi_dstr_unit(d, i);
        bool ws = u == ' ' || u == '\t' || u == '\n' || u == '\f' || u == '\r';
        if (ws) {
            pending = o.units.len != 0;
        } else {
            if (pending) {
                embi_dbuf_append_unit(&o, ' ');
                pending = false;
            }
            embi_dbuf_append_unit(&o, u);
        }
    }
    embi_dstr r = embi_dbuf_finish(&o);
    embi_dstr out = embi_bind_tmp_copy(c, r);
    embi_dstr_free(r);
    embi_dstr_free(d);
    embi_dbuf_free(&o);
    embi_dbuf_free(&b);
    return out;
}

void embi_impl_Document_set_title(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    embi_node *t = find_title(self);
    if (!t) {
        embi_node *head = embi_impl_Document_get_head(c, self);
        if (!head) {
            return;
        }
        embi_element *el = nullptr;
        if (embi_dom_create_element(doc_of(self), EMBI_STR("title"), &el) != EMBI_DOM_OK) {
            return;
        }
        if (embi_append_child(head, &el->base) != EMBI_DOM_OK) {
            embi_destroy_detached(&el->base);
            return;
        }
        t = &el->base;
    }
    (void)embi_text_content_set(t, a0);
}

embi_node *embi_impl_Document_get_head(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_element *root = embi_document_element(doc_of(self));
    if (!root || !is_html_el(&root->base, EMBI_ATOM_html)) {
        return nullptr;
    }
    for (embi_node *k = root->base.first_child; k; k = k->next) {
        if (is_html_el(k, EMBI_ATOM_head)) {
            return k;
        }
    }
    return nullptr;
}

static embi_node *find_body(embi_node *self)
{
    embi_element *root = embi_document_element(doc_of(self));
    if (!root || !is_html_el(&root->base, EMBI_ATOM_html)) {
        return nullptr;
    }
    for (embi_node *k = root->base.first_child; k; k = k->next) {
        if (is_html_el(k, EMBI_ATOM_body) || is_html_el(k, EMBI_ATOM_frameset)) {
            return k;
        }
    }
    return nullptr;
}

embi_dom_err embi_impl_Document_get_body(embi_bind_ctx *c, embi_node *self, embi_node **out)
{
    (void)c;
    *out = find_body(self);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Document_set_body(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    (void)c;
    if (!a0 || !(is_html_el(a0, EMBI_ATOM_body) || is_html_el(a0, EMBI_ATOM_frameset))) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    embi_node *old = find_body(self);
    if (old == a0) {
        return EMBI_DOM_OK;
    }
    if (old) {
        return embi_replace_child(old->parent, a0, old);
    }
    embi_element *root = embi_document_element(doc_of(self));
    if (!root) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    return embi_append_child(&root->base, a0);
}

#define DOC_COLL(NAME, KIND)                                                                           \
    embi_js_value embi_impl_Document_get_##NAME(embi_bind_ctx *c, embi_node *self)                    \
    {                                                                                                  \
        return embi_impl_coll_new(c, KIND, self, k_no_str, k_no_str, false);                           \
    }

DOC_COLL(images, EMBI_COLL_IMAGES)
DOC_COLL(links, EMBI_COLL_LINKS)
DOC_COLL(forms, EMBI_COLL_FORMS)
DOC_COLL(scripts, EMBI_COLL_SCRIPTS)
DOC_COLL(embeds, EMBI_COLL_EMBEDS)
DOC_COLL(all, EMBI_COLL_ALL)
DOC_COLL(anchors, EMBI_COLL_ANCHORS)
