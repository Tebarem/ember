/*
 * NodeList, HTMLCollection, DOMTokenList, NamedNodeMap, Attr, DOMStringMap, DOMRect and
 * DOMRectList. These are views: wrappers whose record names an owner (the element or the root of
 * the collection), so they keep the owner alive and go stale with it.
 *
 * Live collections cache their node list and refresh it when the context's mutation epoch moves.
 * The cached pointers are only read after a refresh, so a node that was freed by a mutation is
 * never touched.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/window.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/string.h"

#include <string.h>

static const embi_str k_no_str = {"", 0};

/* ------------------------------------------------------------------ live collections */

typedef struct coll {
    uint32_t kind;
    uint32_t has_ns;
    uint32_t epoch;
    uint32_t n, cap;
    uint32_t alen, blen;
    uint32_t pad;
    /* alen + blen bytes of arguments, padded to 8, then cap node pointers */
} coll;

static char *coll_args(coll *cl)
{
    return (char *)(cl + 1);
}

static embi_node **coll_items(coll *cl)
{
    return (embi_node **)((char *)(cl + 1) + (((size_t)cl->alen + cl->blen + 7u) & ~(size_t)7u));
}

static size_t coll_bytes(const coll *cl, uint32_t cap)
{
    return sizeof(coll) + (((size_t)cl->alen + cl->blen + 7u) & ~(size_t)7u) + (size_t)cap * sizeof(embi_node *);
}

embi_js_value embi_impl_coll_new(embi_bind_ctx *c, int kind, embi_node *root, embi_str a, embi_str b,
                                 bool has_ns)
{
    embi_bind_obj *owner = embi_impl_node_rec(c, root);
    coll proto = {.kind = (uint32_t)kind, .alen = a.len, .blen = b.len};
    uint32_t cap = 8;
    size_t total = coll_bytes(&proto, cap);
    coll *cl = embi_calloc(1, total);
    *cl = proto;
    cl->has_ns = has_ns;
    cl->epoch = UINT32_MAX;
    cl->cap = cap;
    if (a.len) {
        SDL_memcpy(coll_args(cl), a.ptr, a.len);
    }
    if (b.len) {
        SDL_memcpy(coll_args(cl) + a.len, b.ptr, b.len);
    }
    bool nodelist = kind == EMBI_COLL_CHILDNODES;
    return embi_bind_view_new(c, nodelist ? EMBI_IF_NodeList : EMBI_IF_HTMLCollection, owner,
                              (uint32_t)kind, cl, (uint32_t)total);
}

static bool html_el(const embi_node *n, embi_atom name)
{
    return n->extra == EMBI_NS_HTML && n->name == name;
}

/* Does a qualified name match (HTML elements of HTML documents compare against the lowercased
 * query)? */
static bool qname_match(const embi_node *n, embi_str q)
{
    const embi_element *e = (const embi_element *)n;
    embi_str local = embi_atom_str(n->name);
    bool lower = n->extra == EMBI_NS_HTML && n->doc->is_html;
    embi_str prefix = e->prefix ? embi_atom_str(e->prefix) : k_no_str;
    uint32_t want = prefix.len ? prefix.len + 1 + local.len : local.len;
    if (q.len != want) {
        return false;
    }
    uint32_t i = 0;
    for (uint32_t k = 0; k < prefix.len + (prefix.len ? 1u : 0u) + local.len; k++, i++) {
        char have = k < prefix.len ? prefix.ptr[k] : (prefix.len && k == prefix.len ? ':' : local.ptr[k - (prefix.len ? prefix.len + 1 : 0)]);
        char ch = q.ptr[i];
        if (lower && ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (have != ch) {
            return false;
        }
    }
    return true;
}

typedef struct class_want {
    embi_str str;
} class_want;

/* The element has the class: exactly, or ASCII case-insensitively in quirks mode. */
static bool has_class_want(embi_element *e, const class_want *w, bool quirks)
{
    uint32_t n = 0;
    const embi_atom *list = embi_el_classes(e, &n);
    for (uint32_t i = 0; i < n; i++) {
        embi_str have = embi_atom_str(list[i]);
        if (quirks ? embi_str_ieq(have, w->str) : embi_str_eq(have, w->str)) {
            return true;
        }
    }
    return false;
}

static bool coll_match(coll *cl, embi_node *n, const class_want *classes, uint32_t nclasses)
{
    if (n->type != EMBI_NODE_ELEMENT) {
        return false;
    }
    embi_element *e = (embi_element *)n;
    switch (cl->kind) {
    case EMBI_COLL_CHILDREN:
    case EMBI_COLL_ALL:
        return true;
    case EMBI_COLL_TAG: {
        embi_str q = {coll_args(cl), cl->alen};
        return embi_str_eq(q, EMBI_STR("*")) || qname_match(n, q);
    }
    case EMBI_COLL_TAG_NS: {
        embi_str ns = {coll_args(cl), cl->alen};
        embi_str local = {coll_args(cl) + cl->alen, cl->blen};
        bool any_ns = cl->has_ns && embi_str_eq(ns, EMBI_STR("*"));
        if (!any_ns) {
            embi_str have = embi_el_namespace_uri(e);
            bool want_null = !cl->has_ns || ns.len == 0;
            if (want_null ? have.len != 0 : !embi_str_eq(have, ns)) {
                return false;
            }
        }
        return embi_str_eq(local, EMBI_STR("*")) || embi_str_eq(local, embi_atom_str(n->name));
    }
    case EMBI_COLL_CLASS:
        for (uint32_t i = 0; i < nclasses; i++) {
            if (!has_class_want(e, &classes[i], n->doc->quirks == EMBI_QUIRKS_FULL)) {
                return false;
            }
        }
        return nclasses != 0;
    case EMBI_COLL_IMAGES:
        return html_el(n, EMBI_ATOM_img);
    case EMBI_COLL_LINKS:
        return (html_el(n, EMBI_ATOM_a) || html_el(n, EMBI_ATOM_area)) && embi_el_has_attribute(e, EMBI_STR("href"));
    case EMBI_COLL_FORMS:
        return html_el(n, EMBI_ATOM_form);
    case EMBI_COLL_SCRIPTS:
        return html_el(n, EMBI_ATOM_script);
    case EMBI_COLL_EMBEDS:
        return html_el(n, EMBI_ATOM_embed);
    case EMBI_COLL_ANCHORS:
        return html_el(n, EMBI_ATOM_a) && embi_el_has_attribute(e, EMBI_STR("name"));
    case EMBI_COLL_TBODIES:
        return html_el(n, EMBI_ATOM_tbody);
    case EMBI_COLL_SECTION_ROWS:
        return html_el(n, EMBI_ATOM_tr);
    case EMBI_COLL_CELLS:
        return html_el(n, EMBI_ATOM_td) || html_el(n, EMBI_ATOM_th);
    default:
        return false;
    }
}

/* Kinds that look at the children of the root only. */
static bool child_walk(uint32_t kind)
{
    return kind == EMBI_COLL_CHILDNODES || kind == EMBI_COLL_CHILDREN || kind == EMBI_COLL_TBODIES ||
           kind == EMBI_COLL_SECTION_ROWS || kind == EMBI_COLL_CELLS;
}

/* Appends n to the cached items, growing the block (which may move: the new block is returned). */
static coll *coll_push(coll *cl, embi_bind_obj *bo, embi_node *n)
{
    if (cl->n == cl->cap) {
        uint32_t ncap = cl->cap * 2;
        size_t old_bytes = coll_bytes(cl, cl->cap);
        size_t new_bytes = coll_bytes(cl, ncap);
        cl = embi_realloc(cl, old_bytes, new_bytes);
        cl->cap = ncap;
        bo->owned = cl;
        bo->owned_size = (uint32_t)new_bytes;
    }
    coll_items(cl)[cl->n++] = n;
    return cl;
}

/* The rows of a table in the order of the HTML Standard: the rows of the first thead, the tr children and the rows of the
 * tbody children in tree order, then the rows of the first tfoot. */
static coll *table_rows(coll *cl, embi_bind_obj *bo, embi_node *table)
{
    embi_node *thead = nullptr, *tfoot = nullptr;
    for (embi_node *k = table->first_child; k; k = k->next) {
        if (html_el(k, EMBI_ATOM_thead) && !thead) {
            thead = k;
        } else if (html_el(k, EMBI_ATOM_tfoot) && !tfoot) {
            tfoot = k;
        }
    }
    for (embi_node *r = thead ? thead->first_child : nullptr; r; r = r->next) {
        if (html_el(r, EMBI_ATOM_tr)) {
            cl = coll_push(cl, bo, r);
        }
    }
    for (embi_node *k = table->first_child; k; k = k->next) {
        if (html_el(k, EMBI_ATOM_tr)) {
            cl = coll_push(cl, bo, k);
        } else if (html_el(k, EMBI_ATOM_tbody)) {
            for (embi_node *r = k->first_child; r; r = r->next) {
                if (html_el(r, EMBI_ATOM_tr)) {
                    cl = coll_push(cl, bo, r);
                }
            }
        }
    }
    for (embi_node *r = tfoot ? tfoot->first_child : nullptr; r; r = r->next) {
        if (html_el(r, EMBI_ATOM_tr)) {
            cl = coll_push(cl, bo, r);
        }
    }
    return cl;
}

static bool is_ascii_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r';
}

/* Refreshes the cached items. Returns the (possibly moved) block. */
static coll *coll_refresh(embi_bind_ctx *c, embi_bind_obj *bo)
{
    coll *cl = bo->owned;
    if (cl->epoch == c->mut_epoch) {
        return cl;
    }
    embi_node *root = embi_bind_owner_node(bo);
    cl->n = 0;
    cl->epoch = c->mut_epoch;
    if (!root) {
        return cl;
    }
    class_want *classes = nullptr;
    uint32_t nclasses = 0;
    if (cl->kind == EMBI_COLL_CLASS) {
        classes = (class_want *)embi_bind_alloc(c, ((size_t)cl->alen / 2 + 1) * sizeof *classes);
        const char *p = coll_args(cl);
        uint32_t i = 0;
        while (i < cl->alen) {
            while (i < cl->alen && is_ascii_ws(p[i])) {
                i++;
            }
            uint32_t s = i;
            while (i < cl->alen && !is_ascii_ws(p[i])) {
                i++;
            }
            if (i > s) {
                char *copy = embi_bind_alloc(c, i - s);
                SDL_memcpy(copy, p + s, i - s);
                classes[nclasses++] = (class_want){{copy, i - s}};
            }
        }
    }
    if (cl->kind == EMBI_COLL_TABLE_ROWS) {
        return table_rows(cl, bo, root);
    }
    for (embi_node *n = child_walk(cl->kind) ? root->first_child : embi_next_in_tree(root, root); n;
         n = child_walk(cl->kind) ? n->next : embi_next_in_tree(root, n)) {
        if (cl->kind != EMBI_COLL_CHILDNODES && !coll_match(cl, n, classes, nclasses)) {
            continue;
        }
        cl = coll_push(cl, bo, n);
    }
    return cl;
}

/* The collection behind a record, refreshed, or nullptr when stale. */
static coll *coll_of(embi_bind_ctx *c, embi_bind_obj *bo)
{
    if (embi_bind_stale(bo) || !bo->owned) {
        return nullptr;
    }
    return coll_refresh(c, bo);
}

/* ------------------------------------------------------------------ static NodeList */

embi_js_value embi_impl_nodelist_static(embi_bind_ctx *c, embi_node *const *nodes, uint32_t n)
{
    embi_js_value arr = embi_bind_array_new(c, n);
    embi_bind_pin(c, arr);
    for (uint32_t i = 0; i < n; i++) {
        embi_bind_array_set(c, arr, i, embi_bind_node_value(c, nodes[i]));
    }
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_NodeList, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), arr);
    return v;
}

uint32_t embi_impl_NodeList_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (!self->owned) {
        return embi_js_is_empty(self->ref) ? 0 : embi_bind_array_len(c, self->ref);
    }
    coll *cl = coll_of(c, self);
    return cl ? cl->n : 0;
}

embi_js_value embi_impl_NodeList_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    if (!self->owned) {
        if (embi_js_is_empty(self->ref) || a0 >= embi_bind_array_len(c, self->ref)) {
            return EMBI_JS_UNDEFINED;
        }
        return embi_bind_array_get(c, self->ref, a0);
    }
    coll *cl = coll_of(c, self);
    if (!cl || a0 >= cl->n) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_node_value(c, coll_items(cl)[a0]);
}

uint32_t embi_impl_HTMLCollection_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    coll *cl = coll_of(c, self);
    return cl ? cl->n : 0;
}

embi_js_value embi_impl_HTMLCollection_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    coll *cl = coll_of(c, self);
    if (!cl || a0 >= cl->n) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_node_value(c, coll_items(cl)[a0]);
}

static embi_node *coll_named(coll *cl, embi_str name);

embi_node *embi_impl_HTMLCollection_namedItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    coll *cl = coll_of(c, self);
    if (!cl || a0.len == 0) {
        return nullptr;
    }
    return coll_named(cl, embi_bind_utf8(c, a0));
}

embi_js_value embi_impl_HTMLCollection_named(embi_bind_ctx *c, embi_bind_obj *self, embi_str name)
{
    coll *cl = coll_of(c, self);
    embi_node *n = (cl && name.len) ? coll_named(cl, name) : nullptr;
    return n ? embi_bind_node_value(c, n) : EMBI_JS_UNDEFINED;
}

static embi_node *coll_named(coll *cl, embi_str name)
{
    for (uint32_t i = 0; i < cl->n; i++) {
        embi_element *e = (embi_element *)coll_items(cl)[i];
        const embi_attr *id = embi_el_get_attribute(e, EMBI_STR("id"));
        if (id && embi_dstr_eq_utf8(id->value, name)) {
            return &e->base;
        }
    }
    for (uint32_t i = 0; i < cl->n; i++) {
        embi_element *e = (embi_element *)coll_items(cl)[i];
        if (e->base.extra != EMBI_NS_HTML) {
            continue;
        }
        const embi_attr *nm = embi_el_get_attribute(e, EMBI_STR("name"));
        if (nm && embi_dstr_eq_utf8(nm->value, name)) {
            return &e->base;
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ DOMTokenList */

static embi_element *tl_element(embi_bind_obj *self)
{
    embi_node *n = embi_bind_owner_node(self);
    return (embi_element *)n;
}

static embi_str tl_attr(embi_bind_obj *self)
{
    return (embi_str){(const char *)self->owned, self->owned_size ? self->owned_size - 1 : 0};
}

embi_js_value embi_impl_tokenlist_new(embi_bind_ctx *c, embi_bind_obj *el, embi_str attr)
{
    char *name = embi_alloc((size_t)attr.len + 1);
    SDL_memcpy(name, attr.ptr, attr.len);
    name[attr.len] = '\0';
    return embi_bind_view_new(c, EMBI_IF_DOMTokenList, el, 0, name, attr.len + 1);
}

typedef struct tokens {
    embi_str *items;
    uint32_t n;
} tokens;

/* The ordered set of tokens of the attribute value (scratch memory). */
static tokens tl_parse(embi_bind_ctx *c, embi_bind_obj *self)
{
    tokens t = {nullptr, 0};
    embi_element *el = tl_element(self);
    if (!el) {
        return t;
    }
    const embi_attr *a = embi_el_get_attribute(el, tl_attr(self));
    if (!a || a->value.len == 0) {
        return t;
    }
    embi_str v = embi_bind_utf8(c, a->value);
    t.items = (embi_str *)embi_bind_alloc(c, (size_t)(v.len / 2 + 1) * sizeof(embi_str));
    uint32_t i = 0;
    while (i < v.len) {
        while (i < v.len && is_ascii_ws(v.ptr[i])) {
            i++;
        }
        uint32_t s = i;
        while (i < v.len && !is_ascii_ws(v.ptr[i])) {
            i++;
        }
        if (i > s) {
            embi_str tok = {v.ptr + s, i - s};
            bool dup = false;
            for (uint32_t k = 0; k < t.n; k++) {
                if (embi_str_eq(t.items[k], tok)) {
                    dup = true;
                }
            }
            if (!dup) {
                t.items[t.n++] = tok;
            }
        }
    }
    return t;
}

static void tl_write(embi_bind_ctx *c, embi_bind_obj *self, const tokens *t)
{
    embi_element *el = tl_element(self);
    if (!el) {
        return;
    }
    embi_str name = tl_attr(self);
    if (t->n == 0 && !embi_el_has_attribute(el, name)) {
        return;
    }
    embi_sb sb = {0};
    for (uint32_t i = 0; i < t->n; i++) {
        if (i) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_sb_append(&sb, t->items[i]);
    }
    embi_dstr d = embi_dstr_empty();
    if (sb.len) {
        d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    }
    (void)embi_el_set_attribute(el, name, d);
    embi_sb_free(&sb);
}

static embi_dom_err tl_check(embi_bind_ctx *c, embi_str tok)
{
    if (tok.len == 0) {
        return embi_impl_throw(c, "SyntaxError", "The token provided must not be empty.");
    }
    for (uint32_t i = 0; i < tok.len; i++) {
        if (is_ascii_ws(tok.ptr[i])) {
            return embi_impl_throw(c, "InvalidCharacterError",
                                   "The token provided contains HTML space characters, which are not valid in tokens.");
        }
    }
    return EMBI_DOM_OK;
}

static int tl_find(const tokens *t, embi_str tok)
{
    for (uint32_t i = 0; i < t->n; i++) {
        if (embi_str_eq(t->items[i], tok)) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t embi_impl_DOMTokenList_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    return tl_parse(c, self).n;
}

embi_js_value embi_impl_DOMTokenList_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    tokens t = tl_parse(c, self);
    return a0 < t.n ? embi_bind_utf8_value(c, t.items[a0]) : EMBI_JS_UNDEFINED;
}

bool embi_impl_DOMTokenList_contains(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    tokens t = tl_parse(c, self);
    return tl_find(&t, embi_bind_utf8(c, a0)) >= 0;
}

embi_dom_err embi_impl_DOMTokenList_add(embi_bind_ctx *c, embi_bind_obj *self, int a0n, const embi_dstr *a0p)
{
    for (int i = 0; i < a0n; i++) {
        embi_dom_err e = tl_check(c, embi_bind_utf8(c, a0p[i]));
        if (e != EMBI_DOM_OK) {
            return e;
        }
    }
    tokens t = tl_parse(c, self);
    embi_str *items = (embi_str *)embi_bind_alloc(c, (size_t)(t.n + (uint32_t)a0n + 1) * sizeof(embi_str));
    for (uint32_t i = 0; i < t.n; i++) {
        items[i] = t.items[i];
    }
    tokens nt = {items, t.n};
    for (int i = 0; i < a0n; i++) {
        embi_str tok = embi_bind_utf8(c, a0p[i]);
        if (tl_find(&nt, tok) < 0) {
            nt.items[nt.n++] = tok;
        }
    }
    tl_write(c, self, &nt);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_DOMTokenList_remove(embi_bind_ctx *c, embi_bind_obj *self, int a0n, const embi_dstr *a0p)
{
    for (int i = 0; i < a0n; i++) {
        embi_dom_err e = tl_check(c, embi_bind_utf8(c, a0p[i]));
        if (e != EMBI_DOM_OK) {
            return e;
        }
    }
    tokens t = tl_parse(c, self);
    tokens nt = {(embi_str *)embi_bind_alloc(c, (size_t)(t.n + 1) * sizeof(embi_str)), 0};
    for (uint32_t i = 0; i < t.n; i++) {
        bool drop = false;
        for (int k = 0; k < a0n; k++) {
            drop = drop || embi_str_eq(t.items[i], embi_bind_utf8(c, a0p[k]));
        }
        if (!drop) {
            nt.items[nt.n++] = t.items[i];
        }
    }
    tl_write(c, self, &nt);
    return EMBI_DOM_OK;
}

static embi_dom_err tl_toggle(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr tokd, bool has_force,
                              bool force, bool *out)
{
    embi_str tok = embi_bind_utf8(c, tokd);
    embi_dom_err e = tl_check(c, tok);
    if (e != EMBI_DOM_OK) {
        return e;
    }
    tokens t = tl_parse(c, self);
    int at = tl_find(&t, tok);
    if (at >= 0) {
        if (!has_force || !force) {
            tokens nt = {(embi_str *)embi_bind_alloc(c, (size_t)(t.n + 1) * sizeof(embi_str)), 0};
            for (uint32_t i = 0; i < t.n; i++) {
                if ((int)i != at) {
                    nt.items[nt.n++] = t.items[i];
                }
            }
            tl_write(c, self, &nt);
            *out = false;
            return EMBI_DOM_OK;
        }
        *out = true;
        return EMBI_DOM_OK;
    }
    if (has_force && !force) {
        *out = false;
        return EMBI_DOM_OK;
    }
    embi_str *items = (embi_str *)embi_bind_alloc(c, (size_t)(t.n + 2) * sizeof(embi_str));
    for (uint32_t i = 0; i < t.n; i++) {
        items[i] = t.items[i];
    }
    items[t.n] = tok;
    tokens nt = {items, t.n + 1};
    tl_write(c, self, &nt);
    *out = true;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_DOMTokenList_toggle_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool *out)
{
    return tl_toggle(c, self, a0, false, false, out);
}

embi_dom_err embi_impl_DOMTokenList_toggle_1(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool a1,
                                             bool *out)
{
    return tl_toggle(c, self, a0, true, a1, out);
}

embi_dom_err embi_impl_DOMTokenList_replace(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0,
                                            embi_dstr a1, bool *out)
{
    embi_str old = embi_bind_utf8(c, a0), nw = embi_bind_utf8(c, a1);
    /* both tokens are checked for emptiness before either is checked for whitespace */
    embi_dom_err e = EMBI_DOM_OK;
    if (old.len == 0 || nw.len == 0) {
        e = tl_check(c, old.len == 0 ? old : nw);
    } else {
        e = tl_check(c, old);
        if (e == EMBI_DOM_OK) {
            e = tl_check(c, nw);
        }
    }
    if (e != EMBI_DOM_OK) {
        return e;
    }
    tokens t = tl_parse(c, self);
    int at = tl_find(&t, old);
    if (at < 0) {
        *out = false;
        return EMBI_DOM_OK;
    }
    tokens nt = {(embi_str *)embi_bind_alloc(c, (size_t)(t.n + 1) * sizeof(embi_str)), 0};
    bool placed = false;
    for (uint32_t i = 0; i < t.n; i++) {
        embi_str cur = t.items[i];
        if ((int)i == at) {
            if (tl_find(&nt, nw) < 0 && !placed) {
                nt.items[nt.n++] = nw;
            }
            placed = true;
        } else if (embi_str_eq(cur, nw)) {
            if (!placed) {
                nt.items[nt.n++] = nw;
                placed = true;
            }
        } else {
            nt.items[nt.n++] = cur;
        }
    }
    tl_write(c, self, &nt);
    *out = true;
    return EMBI_DOM_OK;
}

/* DOMTokenList.supports (DOM 7.1). The supported tokens are the HTML list of the attribute: rel on link (W7d: the
 * module preloads and the link kinds Ember knows of; a page's modulepreload polyfill then leaves the work to the loader).
 * Any other attribute has none, which is the TypeError. */
embi_dom_err embi_impl_DOMTokenList_supports(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, bool *out)
{
    static const char *const link_rel[] = {"alternate", "dns-prefetch", "icon", "modulepreload", "pingback",
                                           "preconnect", "prefetch", "preload", "search", "stylesheet"};
    embi_element *el = tl_element(self);
    embi_str attr = tl_attr(self);
    bool is_link_rel = el != nullptr && embi_str_eq(attr, EMBI_STR("rel")) &&
                       embi_str_ieq(embi_atom_str(el->base.name), EMBI_STR("link"));
    if (!is_link_rel) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'supports' on 'DOMTokenList': DOMTokenList has no supported tokens.");
        return EMBI_BIND_THREW;
    }
    embi_str want = embi_bind_utf8(c, a0);
    *out = false;
    for (size_t i = 0; i < sizeof link_rel / sizeof link_rel[0]; i++) {
        if (embi_str_ieq(want, (embi_str){link_rel[i], (uint32_t)strlen(link_rel[i])})) {
            *out = true;
        }
    }
    return EMBI_DOM_OK;
}

embi_dstr embi_impl_DOMTokenList_get_value(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_element *el = tl_element(self);
    const embi_attr *a = el ? embi_el_get_attribute(el, tl_attr(self)) : nullptr;
    return a ? a->value : embi_dstr_empty();
}

void embi_impl_DOMTokenList_set_value(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    (void)c;
    embi_element *el = tl_element(self);
    if (el) {
        (void)embi_el_set_attribute(el, tl_attr(self), a0);
    }
}

embi_dstr embi_impl_DOMTokenList_stringifier(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_DOMTokenList_get_value(c, self);
}

/* ------------------------------------------------------------------ Attr and NamedNodeMap */

/* An Attr is an embi_attr_node (dom/dom.h). The Attr objects of an element's attributes are cached in an array in the
 * element record's ref slot (so that the same attribute gives the same object), and each keeps its element alive. */

embi_dstr embi_impl_attr_qname(embi_bind_ctx *c, const embi_attr_node *an)
{
    embi_str local = embi_atom_str(an->local);
    if (!an->prefix) {
        return embi_bind_tmp_utf8(c, local);
    }
    embi_str p = embi_atom_str(an->prefix);
    char *buf = embi_bind_alloc(c, (size_t)p.len + 1 + local.len + 1);
    SDL_memcpy(buf, p.ptr, p.len);
    buf[p.len] = ':';
    SDL_memcpy(buf + p.len + 1, local.ptr, local.len);
    return embi_bind_tmp_utf8(c, (embi_str){buf, p.len + 1 + local.len});
}

static embi_attr_node *attr_of_value(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    return (bo && bo->iface == EMBI_IF_Attr && bo->ptr) ? bo->ptr : nullptr;
}

/* The Attr object of el's attribute (ns, ns_uri, local), or nullptr when none was made. */
static embi_attr_node *attr_cached(embi_bind_ctx *c, embi_bind_obj *el, uint8_t ns, embi_atom ns_uri, embi_atom local)
{
    if (embi_js_is_empty(el->ref)) {
        return nullptr;
    }
    uint32_t n = embi_bind_array_len(c, el->ref);
    for (uint32_t i = 0; i < n; i++) {
        embi_attr_node *an = attr_of_value(embi_bind_array_get(c, el->ref, i));
        if (an && an->owner == (embi_element *)el->ptr && an->local == local && an->ns == ns &&
            (ns != EMBI_NS_OTHER || an->ns_uri == ns_uri)) {
            return an;
        }
    }
    return nullptr;
}

/* Records an attached Attr object in the element's cache and ties its lifetime to the element. */
static void attr_cache_add(embi_bind_ctx *c, embi_bind_obj *el, embi_attr_node *an)
{
    if (embi_js_is_empty(el->ref)) {
        embi_bind_set_ref(el, embi_bind_array_new(c, 0));
    }
    embi_js_value v = embi_bind_node_value(c, &an->base);
    ((embi_bind_obj *)an->base.wrapper)->owner = el;
    uint32_t n = embi_bind_array_len(c, el->ref);
    for (uint32_t i = 0; i < n; i++) {
        embi_attr_node *old = attr_of_value(embi_bind_array_get(c, el->ref, i));
        if (old == an) {
            return;
        }
        if (old && old->owner == nullptr) { /* a detached one: its slot is free */
            embi_bind_array_set(c, el->ref, i, v);
            return;
        }
    }
    embi_bind_array_set(c, el->ref, n, v);
}

embi_attr_node *embi_impl_attr_for(embi_bind_ctx *c, embi_bind_obj *el, const embi_attr *a)
{
    embi_attr_node *an = attr_cached(c, el, a->ns, a->ns_uri, a->name);
    if (an) {
        return an;
    }
    embi_element *e = el->ptr;
    an = embi_new_attr_node(e->base.doc, a->ns, a->ns_uri, a->prefix, a->name, embi_dstr_empty());
    an->owner = e;
    attr_cache_add(c, el, an);
    return an;
}

embi_js_value embi_impl_attr_new(embi_bind_ctx *c, embi_bind_obj *el, const embi_attr *a)
{
    return embi_bind_node_value(c, &embi_impl_attr_for(c, el, a)->base);
}

/* The attribute (ns, ns_uri, local) of el has changed; if it is gone, its Attr object is detached with the old value. */
void embi_impl_attr_changed(embi_bind_ctx *c, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local,
                            embi_dstr old_value, bool had_old)
{
    embi_bind_obj *bo = el->base.wrapper;
    if (!had_old || !bo || embi_js_is_empty(bo->ref) || embi_el_find_attr(el, ns, ns_uri, local)) {
        return;
    }
    embi_attr_node *an = attr_cached(c, bo, ns, ns_uri, local);
    if (an) {
        embi_attr_node_detach(an, old_value);
        ((embi_bind_obj *)an->base.wrapper)->owner = nullptr;
    }
}

embi_dstr embi_impl_Attr_get_localName(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_utf8(c, embi_atom_str(((embi_attr_node *)self)->local));
}

embi_js_value embi_impl_Attr_get_namespaceURI(embi_bind_ctx *c, embi_node *self)
{
    const embi_attr_node *an = (embi_attr_node *)self;
    if (an->ns == EMBI_NS_NULL) {
        return EMBI_JS_NULL;
    }
    return embi_bind_utf8_value(c, embi_dom_ns_uri(an->ns, an->ns_uri));
}

embi_js_value embi_impl_Attr_get_prefix(embi_bind_ctx *c, embi_node *self)
{
    const embi_attr_node *an = (embi_attr_node *)self;
    return an->prefix ? embi_bind_utf8_value(c, embi_atom_str(an->prefix)) : EMBI_JS_NULL;
}

embi_dstr embi_impl_Attr_get_name(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_attr_qname(c, (embi_attr_node *)self);
}

embi_dstr embi_impl_Attr_get_value(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_copy(c, embi_attr_node_value((embi_attr_node *)self));
}

void embi_impl_Attr_set_value(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    embi_attr_node_set_value((embi_attr_node *)self, a0);
}

embi_node *embi_impl_Attr_get_ownerElement(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_element *o = ((embi_attr_node *)self)->owner;
    return o ? &o->base : nullptr;
}

bool embi_impl_Attr_get_specified(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    (void)self;
    return true;
}

/* "Set an attribute" (DOM Standard): attr goes onto element self; returns the Attr it replaced, or nullptr. */
embi_dom_err embi_impl_set_attr_node(embi_bind_ctx *c, embi_node *self, embi_node *attr_n, embi_node **out)
{
    embi_attr_node *an = (embi_attr_node *)attr_n;
    embi_element *el = (embi_element *)self;
    *out = nullptr;
    if (an->owner && an->owner != el) {
        return embi_impl_throw(c, "InUseAttributeError", "The attribute is in use by another element.");
    }
    embi_bind_obj *eb = embi_impl_node_rec(c, self);
    const embi_attr *old = embi_el_find_attr(el, an->ns, an->ns_uri, an->local);
    if (an->owner == el) {
        *out = attr_n;
        return EMBI_DOM_OK;
    }
    embi_attr_node *oldn = nullptr;
    if (old) {
        oldn = embi_impl_attr_for(c, eb, old);
        (void)embi_bind_pin(c, embi_bind_node_value(c, &oldn->base));
        embi_attr_node_detach(oldn, old->value);
        ((embi_bind_obj *)oldn->base.wrapper)->owner = nullptr;
    }
    (void)embi_bind_node_value(c, attr_n);
    embi_attr_node_attach(an, el);
    attr_cache_add(c, eb, an);
    *out = oldn ? &oldn->base : nullptr;
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_attrmap_new(embi_bind_ctx *c, embi_bind_obj *el)
{
    return embi_bind_view_new(c, EMBI_IF_NamedNodeMap, el, 0, nullptr, 0);
}

static embi_bind_obj *map_el_rec(embi_bind_obj *self)
{
    return embi_bind_stale(self) ? nullptr : self->owner;
}

uint32_t embi_impl_NamedNodeMap_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_node *n = embi_bind_owner_node(self);
    return n ? embi_el_attr_count((embi_element *)n) : 0;
}

embi_js_value embi_impl_NamedNodeMap_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    embi_node *n = embi_bind_owner_node(self);
    if (!n || a0 >= embi_el_attr_count((embi_element *)n)) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_impl_attr_new(c, map_el_rec(self), embi_el_attr_at((embi_element *)n, a0));
}

embi_node *embi_impl_NamedNodeMap_getNamedItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_node *n = embi_bind_owner_node(self);
    return n ? embi_impl_Element_getAttributeNode(c, n, a0) : nullptr;
}

embi_node *embi_impl_NamedNodeMap_getNamedItemNS(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0,
                                                 embi_dstr a1)
{
    embi_node *n = embi_bind_owner_node(self);
    return n ? embi_impl_Element_getAttributeNodeNS(c, n, a0, a1) : nullptr;
}

embi_dom_err embi_impl_NamedNodeMap_setNamedItem(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0, embi_node **out)
{
    embi_node *n = embi_bind_owner_node(self);
    return n ? embi_impl_set_attr_node(c, n, a0, out) : EMBI_DOM_ERR_NOT_FOUND;
}

embi_dom_err embi_impl_NamedNodeMap_setNamedItemNS(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0,
                                                   embi_node **out)
{
    return embi_impl_NamedNodeMap_setNamedItem(c, self, a0, out);
}

embi_dom_err embi_impl_NamedNodeMap_removeNamedItem(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0,
                                                    embi_node **out)
{
    embi_node *n = embi_bind_owner_node(self);
    if (!n) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    const embi_attr *a = embi_el_get_attribute((embi_element *)n, embi_bind_utf8(c, a0));
    if (!a) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    embi_attr_node *an = embi_impl_attr_for(c, map_el_rec(self), a);
    (void)embi_bind_pin(c, embi_bind_node_value(c, &an->base));
    (void)embi_el_remove_attribute((embi_element *)n, embi_bind_utf8(c, a0));
    *out = &an->base;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_NamedNodeMap_removeNamedItemNS(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_nstr a0,
                                                      embi_dstr a1, embi_node **out)
{
    embi_node *n = embi_bind_owner_node(self);
    if (!n) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    embi_str ns = (a0.is_null || a0.s.len == 0) ? k_no_str : embi_bind_utf8(c, a0.s);
    const embi_attr *a = embi_el_get_attribute_ns((embi_element *)n, ns.len != 0, ns, embi_bind_utf8(c, a1));
    if (!a) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    embi_attr_node *an = embi_impl_attr_for(c, map_el_rec(self), a);
    (void)embi_bind_pin(c, embi_bind_node_value(c, &an->base));
    (void)embi_el_remove_attribute_ns((embi_element *)n, ns.len != 0, ns, embi_bind_utf8(c, a1));
    *out = &an->base;
    return EMBI_DOM_OK;
}

embi_js_value embi_impl_NamedNodeMap_named(embi_bind_ctx *c, embi_bind_obj *self, embi_str name)
{
    embi_node *n = embi_bind_owner_node(self);
    if (!n) {
        return EMBI_JS_UNDEFINED;
    }
    const embi_attr *a = embi_el_get_attribute((embi_element *)n, name);
    return a ? embi_impl_attr_new(c, map_el_rec(self), a) : EMBI_JS_UNDEFINED;
}

/* The supported property names of a legacy platform object, appended without duplicates; a name that a prototype
 * member shadows is not visible. */
typedef struct keyset {
    embi_str *items;
    uint32_t n;
} keyset;

static void keyset_add(embi_js_runtime *rt, embi_js_object *o, embi_bind_ctx *c, keyset *ks, embi_str name,
                       embi_js_key_vec *out)
{
    if (name.len == 0) {
        return;
    }
    for (uint32_t i = 0; i < ks->n; i++) {
        if (embi_str_eq(ks->items[i], name)) {
            return;
        }
    }
    ks->items[ks->n++] = name;
    embi_js_key k = embi_bind_key_from_utf8(c, name);
    if (!embi_js_key_is_index(k) && !embi_bind_proto_has(rt, o, k)) {
        embi_vec_push(out, k);
    }
}

void embi_impl_HTMLCollection_named_keys(embi_js_runtime *rt, embi_js_object *o, embi_bind_ctx *c, embi_bind_obj *self,
                                         embi_js_key_vec *out)
{
    coll *cl = coll_of(c, self);
    if (!cl) {
        return;
    }
    keyset ks = {(embi_str *)embi_bind_alloc(c, ((size_t)cl->n * 2 + 1) * sizeof(embi_str)), 0};
    for (uint32_t i = 0; i < cl->n; i++) {
        embi_element *e = (embi_element *)coll_items(cl)[i];
        const embi_attr *id = embi_el_get_attribute(e, EMBI_STR("id"));
        if (id) {
            keyset_add(rt, o, c, &ks, embi_bind_utf8(c, id->value), out);
        }
        if (e->base.extra == EMBI_NS_HTML) {
            const embi_attr *nm = embi_el_get_attribute(e, EMBI_STR("name"));
            if (nm) {
                keyset_add(rt, o, c, &ks, embi_bind_utf8(c, nm->value), out);
            }
        }
    }
}

void embi_impl_NamedNodeMap_named_keys(embi_js_runtime *rt, embi_js_object *o, embi_bind_ctx *c, embi_bind_obj *self,
                                       embi_js_key_vec *out)
{
    embi_node *n = embi_bind_owner_node(self);
    if (!n) {
        return;
    }
    embi_element *el = (embi_element *)n;
    uint32_t cnt = embi_el_attr_count(el);
    keyset ks = {(embi_str *)embi_bind_alloc(c, ((size_t)cnt + 1) * sizeof(embi_str)), 0};
    bool lower_only = el->base.extra == EMBI_NS_HTML && n->doc->is_html;
    for (uint32_t i = 0; i < cnt; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        embi_str local = embi_atom_str(a->name);
        embi_str prefix = a->prefix ? embi_atom_str(a->prefix) : k_no_str;
        char *buf = embi_bind_alloc(c, (size_t)local.len + prefix.len + 2);
        uint32_t w = 0;
        if (prefix.len) {
            memcpy(buf, prefix.ptr, prefix.len);
            w = prefix.len;
            buf[w++] = ':';
        }
        memcpy(buf + w, local.ptr, local.len);
        w += local.len;
        bool ok = true;
        for (uint32_t k = 0; k < w && lower_only; k++) {
            ok = ok && !(buf[k] >= 'A' && buf[k] <= 'Z');
        }
        if (ok) {
            keyset_add(rt, o, c, &ks, (embi_str){buf, w}, out);
        }
    }
}

/* ------------------------------------------------------------------ DOMStringMap */

embi_js_value embi_impl_dataset_new(embi_bind_ctx *c, embi_bind_obj *el)
{
    return embi_bind_view_new(c, EMBI_IF_DOMStringMap, el, 0, nullptr, 0);
}

/* data-foo-bar for the property fooBar. Returns false when the name cannot be a data attribute
 * (a dash followed by a lowercase letter). */
static bool dataset_attr_name(embi_str prop, embi_sb *out)
{
    embi_sb_append(out, EMBI_STR("data-"));
    for (uint32_t i = 0; i < prop.len; i++) {
        char ch = prop.ptr[i];
        if (ch == '-' && i + 1 < prop.len && prop.ptr[i + 1] >= 'a' && prop.ptr[i + 1] <= 'z') {
            return false;
        }
        if (ch >= 'A' && ch <= 'Z') {
            embi_sb_append_char(out, '-');
            embi_sb_append_char(out, (char)(ch - 'A' + 'a'));
        } else {
            embi_sb_append_char(out, ch);
        }
    }
    return true;
}

int embi_impl_DOMStringMap_named_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    embi_bind_obj *bo = o->internal;
    if (bo && bo->ctx && !embi_bind_stale(bo) && embi_js_is_string(k)) {
        embi_bind_ctx *c = bo->ctx;
        embi_bind_scope sc = embi_bind_enter(c);
        embi_str prop = embi_bind_key_utf8(c, k);
        embi_sb name = {0};
        embi_node *n = embi_bind_owner_node(bo);
        const embi_attr *a = nullptr;
        if (n && dataset_attr_name(prop, &name)) {
            a = embi_el_get_attribute((embi_element *)n, (embi_str){name.data, name.len});
        }
        embi_sb_free(&name);
        if (a) {
            out->value = embi_bind_dstr_value(c, a->value);
            out->get = EMBI_JS_UNDEFINED;
            out->set = EMBI_JS_UNDEFINED;
            out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
            out->attrs = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
            embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return EMBI_JS_YES;
        }
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return embi_bind_own_named(rt, o, k, out);
}

int embi_impl_DOMStringMap_named_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                            const embi_js_pdesc *d)
{
    embi_bind_obj *bo = o->internal;
    if (!bo || !bo->ctx || embi_bind_stale(bo) || !embi_js_is_string(k)) {
        return EMBI_JS_NO;
    }
    if ((d->has & (EMBI_JS_PD_GET | EMBI_JS_PD_SET)) || !(d->has & EMBI_JS_PD_VALUE)) {
        return EMBI_JS_NO;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_dstr value;
    if (!embi_bind_to_dstr(c, d->value, &value)) {
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return EMBI_JS_THREW;
    }
    embi_str prop = embi_bind_key_utf8(c, k);
    embi_sb name = {0};
    int result = EMBI_JS_YES;
    if (!dataset_attr_name(prop, &name)) {
        (void)embi_bind_throw_named(c, "SyntaxError",
                                    "A property name containing a dash followed by a lowercase letter cannot be a data attribute.");
        result = EMBI_JS_THREW;
    } else {
        embi_node *n = embi_bind_owner_node(bo);
        embi_dom_err e = n ? embi_el_set_attribute((embi_element *)n, (embi_str){name.data, name.len}, value)
                           : EMBI_DOM_OK;
        if (e != EMBI_DOM_OK) {
            (void)embi_bind_throw_named(c, "InvalidCharacterError", "The property name is not a valid attribute name.");
            result = EMBI_JS_THREW;
        }
    }
    embi_sb_free(&name);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    (void)rt;
    return result;
}

int embi_impl_DOMStringMap_named_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    (void)rt;
    embi_bind_obj *bo = o->internal;
    if (!bo || !bo->ctx || embi_bind_stale(bo) || !embi_js_is_string(k)) {
        return EMBI_JS_YES;
    }
    embi_bind_ctx *c = bo->ctx;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str prop = embi_bind_key_utf8(c, k);
    embi_sb name = {0};
    embi_node *n = embi_bind_owner_node(bo);
    if (n && dataset_attr_name(prop, &name)) {
        (void)embi_el_remove_attribute((embi_element *)n, (embi_str){name.data, name.len});
    }
    embi_sb_free(&name);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return EMBI_JS_YES;
}

int embi_impl_DOMStringMap_named_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    embi_bind_obj *bo = o->internal;
    if (bo && bo->ctx && !embi_bind_stale(bo)) {
        embi_bind_ctx *c = bo->ctx;
        embi_node *n = embi_bind_owner_node(bo);
        if (n) {
            embi_bind_scope sc = embi_bind_enter(c);
            embi_element *el = (embi_element *)n;
            uint32_t cnt = embi_el_attr_count(el);
            for (uint32_t i = 0; i < cnt; i++) {
                const embi_attr *a = embi_el_attr_at(el, i);
                if (a->ns != EMBI_NS_NULL) {
                    continue;
                }
                embi_str nm = embi_atom_str(a->name);
                if (nm.len < 5 || SDL_strncmp(nm.ptr, "data-", 5) != 0) {
                    continue;
                }
                bool ok = true;
                for (uint32_t k = 5; k < nm.len; k++) {
                    ok = ok && !(nm.ptr[k] >= 'A' && nm.ptr[k] <= 'Z');
                }
                if (!ok) {
                    continue;
                }
                char *buf = embi_bind_alloc(c, nm.len);
                uint32_t w = 0;
                for (uint32_t k = 5; k < nm.len; k++) {
                    if (nm.ptr[k] == '-' && k + 1 < nm.len && nm.ptr[k + 1] >= 'a' && nm.ptr[k + 1] <= 'z') {
                        buf[w++] = (char)(nm.ptr[k + 1] - 'a' + 'A');
                        k++;
                    } else {
                        buf[w++] = nm.ptr[k];
                    }
                }
                embi_vec_push(out, embi_bind_key_from_utf8(c, (embi_str){buf, w}));
            }
            embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        }
    }
    return embi_bind_own_keys_ordinary(rt, o, out);
}

/* ------------------------------------------------------------------ DOMRect and DOMRectList */

/* A DOMRect is an sv payload (impl_svg_val.c: embi_impl_rect_new, the accessors), shared with SVGRect. */

embi_js_value embi_impl_rectlist_new(embi_bind_ctx *c, const double *rects, uint32_t n)
{
    embi_js_value arr = embi_bind_array_new(c, n);
    embi_bind_pin(c, arr);
    for (uint32_t i = 0; i < n; i++) {
        embi_bind_array_set(c, arr, i,
                            embi_impl_rect_new(c, rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]));
    }
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_DOMRectList, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), arr);
    return v;
}

uint32_t embi_impl_DOMRectList_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_js_is_empty(self->ref) ? 0 : embi_bind_array_len(c, self->ref);
}

embi_js_value embi_impl_DOMRectList_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    if (embi_js_is_empty(self->ref) || a0 >= embi_bind_array_len(c, self->ref)) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_array_get(c, self->ref, a0);
}

