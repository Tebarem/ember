#include "dom/internal.h"

/* ------------------------------------------------------------------ attribute blocks */

static embi_attrs *attrs_alloc(embi_slab *s, uint32_t cap)
{
    embi_attrs *a = embi_slab_alloc(s, sizeof(embi_attrs) + (size_t)cap * sizeof(embi_attr));
    SDL_memset(a, 0, sizeof(embi_attrs));
    a->cap = cap;
    return a;
}

bool embi_attr_value_inline(const embi_attrs *a, embi_dstr v)
{
    if (v.len == 0 || !(a->flags & EMBI_ATTRS_INLINE_VALUES)) {
        return false;
    }
    const char *base = (const char *)a;
    const char *p = v.ptr;
    return p >= base && p < base + embi_slab_capacity(a);
}

void embi_attr_value_free(const embi_attrs *a, embi_dstr v)
{
    if (v.len == 0 || embi_attr_value_inline(a, v)) {
        return;
    }
    embi_slab_free((void *)v.ptr);
}

embi_attrs *embi_el_attrs_ensure(embi_element *el)
{
    if (!el->attrs) {
        el->attrs = attrs_alloc(el->base.doc->slab, 0);
    }
    return el->attrs;
}

embi_attrs *embi_el_attrs_reserve(embi_element *el, uint32_t need)
{
    embi_attrs *a = el->attrs;
    if (a && a->cap >= need) {
        return a;
    }
    /* Exact size for a first block (the parser reserves the attribute count up front), doubling
     * when a block grows. embi_vec_grow_to's minimum of 8 entries would make every element with
     * one attribute carry a 256-byte array. */
    uint32_t cap = a ? a->cap : 0;
    uint64_t dbl = (uint64_t)cap * 2u;
    uint32_t ncap = need > dbl ? need : (uint32_t)dbl;
    embi_attrs *n = attrs_alloc(el->base.doc->slab, ncap);
    if (a) {
        n->content = a->content;
        n->ns_uri = a->ns_uri;
        n->len = a->len;
        n->nclass = a->nclass;
        n->flags = a->flags & ~EMBI_ATTRS_INLINE_VALUES;
        n->classes = a->classes;
        if (a->len) {
            SDL_memcpy(n->entries, a->entries, (size_t)a->len * sizeof(embi_attr));
            if (a->flags & EMBI_ATTRS_INLINE_VALUES) {
                /* The old block goes away, and its values with it. */
                for (uint32_t i = 0; i < a->len; i++) {
                    embi_dstr v = a->entries[i].value;
                    if (embi_attr_value_inline(a, v)) {
                        n->entries[i].value = embi_dstr_copy(el->base.doc->slab, v);
                    }
                }
            }
        }
        embi_slab_free(a);
    }
    el->attrs = n;
    return n;
}

embi_element *embi_new_element_attrs(embi_document *doc, uint8_t ns, embi_atom local, uint32_t n,
                                     const embi_attr_init *src)
{
    embi_element *el = embi_new_element(doc, ns, 0, 0, local);
    if (n == 0) {
        return el;
    }
    uint32_t units[32];
    bool wides[32];
    if (doc->observers.len != 0 || ns == EMBI_NS_OTHER || n > 32) {
        for (uint32_t i = 0; i < n; i++) {
            embi_dstr v = embi_dstr_from_utf8(doc->slab, src[i].value);
            embi_el_set_attr_atom(el, src[i].name, v);
            embi_dstr_free(v);
        }
        return el;
    }
    size_t tail = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool w = false;
        uint32_t u = embi_utf8_units(src[i].value, &w);
        units[i] = u;
        wides[i] = w;
        if (u != 0) {
            if (w) {
                tail = (tail + 1u) & ~(size_t)1u;
            }
            tail += (size_t)u * (w ? 2u : 1u);
        }
    }
    size_t base = sizeof(embi_attrs) + (size_t)n * sizeof(embi_attr);
    embi_attrs *a = embi_slab_alloc(doc->slab, base + tail);
    SDL_memset(a, 0, sizeof(embi_attrs));
    a->cap = n;
    a->len = n;
    if (tail != 0) {
        a->flags = EMBI_ATTRS_INLINE_VALUES;
    }
    char *p = (char *)a + base;
    bool has_id = false;
    for (uint32_t i = 0; i < n; i++) {
        embi_attr *e = &a->entries[i];
        *e = (embi_attr){.name = src[i].name, .value = embi_dstr_empty()};
        has_id = has_id || src[i].name == EMBI_ATOM_id;
        uint32_t u = units[i];
        if (u != 0) {
            if (wides[i]) {
                p = (char *)(((uintptr_t)p + 1u) & ~(uintptr_t)1u);
            }
            embi_utf8_write(p, wides[i], src[i].value);
            e->value = (embi_dstr){p, u, wides[i]};
            p += (size_t)u * (wides[i] ? 2u : 1u);
        }
    }
    el->attrs = a;
    if (has_id) {
        el->id = EMBI_ID_STALE;
    }
    return el;
}

/* Frees the block when nothing in it is still needed. */
static void maybe_release_attrs(embi_element *el)
{
    embi_attrs *a = el->attrs;
    if (a && a->len == 0 && !a->content && a->ns_uri == 0) {
        if (a->classes) {
            embi_slab_free(a->classes);
        }
        embi_slab_free(a);
        el->attrs = nullptr;
    }
}

static void invalidate_classes(embi_attrs *a)
{
    if (a->classes) {
        embi_slab_free(a->classes);
    }
    a->classes = nullptr;
    a->nclass = 0;
    a->flags &= ~EMBI_ATTRS_CLASSES_VALID;
}

/* The id atom is computed on first use: interning the value of every id attribute while parsing
 * puts one atom table insert on the path of each element that has one, and most ids are never
 * looked at. A change only marks the cache stale (and the id map, for a connected element). */
void embi_el_recompute_id(embi_element *el)
{
    el->id = EMBI_ID_STALE;
    if (el->base.flags & EMBI_NF_CONNECTED) {
        embi_document_idmap_invalidate(el->base.doc);
    }
}

static embi_atom compute_id(const embi_element *el)
{
    if (el->attrs) {
        for (uint32_t i = 0; i < el->attrs->len; i++) {
            const embi_attr *at = &el->attrs->entries[i];
            if (at->ns == EMBI_NS_NULL && at->prefix == 0 && at->name == EMBI_ATOM_id) {
                /* An empty id means the element has no ID. */
                return at->value.len ? embi_dstr_atom(at->value) : 0;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ lookup helpers */

static int find_index(const embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local)
{
    const embi_attrs *a = el->attrs;
    if (!a) {
        return -1;
    }
    for (uint32_t i = 0; i < a->len; i++) {
        const embi_attr *at = &a->entries[i];
        if (at->name == local && at->ns == ns && (ns != EMBI_NS_OTHER || at->ns_uri == ns_uri)) {
            return (int)i;
        }
    }
    return -1;
}

const embi_attr *embi_el_find_attr(const embi_element *el, uint8_t ns, embi_atom ns_uri,
                                   embi_atom local)
{
    int i = find_index(el, ns, ns_uri, local);
    return i < 0 ? nullptr : &el->attrs->entries[i];
}

static bool is_ascii_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* Finds the attribute whose qualified name is q: "prefix:local", or just the local name when it
 * has no prefix. Never interns: a name that was never interned cannot match any attribute. A name
 * with no colon can only match an unprefixed attribute. A name with colons matches an attribute
 * whose whole local name contains them (setAttribute) or one split at any colon (setAttributeNS). */
static int find_qname(const embi_element *el, embi_str q)
{
    const embi_attrs *a = el->attrs;
    if (!a || q.len == 0) {
        return -1;
    }
    embi_atom whole = embi_atom_find(q.ptr, q.len);
    uint32_t colon = q.len;
    for (uint32_t i = 0; i < q.len; i++) {
        if (q.ptr[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon == q.len) {
        if (whole == 0) {
            return -1;
        }
        for (uint32_t i = 0; i < a->len; i++) {
            if (a->entries[i].prefix == 0 && a->entries[i].name == whole) {
                return (int)i;
            }
        }
        return -1;
    }
    /* Candidate splits. An attribute's prefix has no colon, so only the first colon can split. */
    embi_atom prefix = 0;
    embi_atom local = 0;
    if (colon > 0 && colon + 1u < q.len) {
        prefix = embi_atom_find(q.ptr, colon);
        local = embi_atom_find(q.ptr + colon + 1u, q.len - colon - 1u);
    }
    for (uint32_t i = 0; i < a->len; i++) {
        const embi_attr *at = &a->entries[i];
        if (at->prefix == 0 ? (whole != 0 && at->name == whole)
                            : (prefix != 0 && local != 0 && at->prefix == prefix && at->name == local)) {
            return (int)i;
        }
    }
    return -1;
}

/* setAttribute and getAttribute lowercase names for HTML elements in HTML documents. Returns the
 * name to use; sb holds the lowered copy when one was made. */
static embi_str fold_name(const embi_element *el, embi_str name, embi_sb *sb)
{
    if (el->base.extra != EMBI_NS_HTML || !el->base.doc->is_html) {
        return name;
    }
    uint32_t upper = 0;
    while (upper < name.len && !(name.ptr[upper] >= 'A' && name.ptr[upper] <= 'Z')) {
        upper++;
    }
    if (upper == name.len) {
        return name; /* already lowercase: no copy */
    }
    for (uint32_t i = 0; i < name.len; i++) {
        char c = name.ptr[i];
        embi_sb_append_char(sb, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
    }
    return (embi_str){sb->data ? sb->data : "", sb->len};
}

/* ------------------------------------------------------------------ core set and remove */

/* Refreshes the id and class caches after the attribute (ns, local) changed. Runs before the
 * observers, so that a callback sees current values. */
static void update_caches(embi_element *el, uint8_t ns, embi_atom local)
{
    if (ns != EMBI_NS_NULL) {
        return;
    }
    if (local == EMBI_ATOM_id) {
        embi_el_recompute_id(el);
    } else if (local == EMBI_ATOM_class && el->attrs) {
        invalidate_classes(el->attrs);
    }
}

/* Replaces the value of entry idx, keeping its namespace, prefix and name. */
static void set_at(embi_element *el, int idx, embi_dstr value)
{
    embi_attr *at = &el->attrs->entries[idx];
    embi_atom local = at->name;
    uint8_t ns = at->ns;
    embi_atom ns_uri = at->ns_uri;
    embi_dstr old = at->value;
    /* Decided before the observers run: a callback may grow or free the block. */
    bool old_inline = embi_attr_value_inline(el->attrs, old);
    at->value = embi_dstr_copy(el->base.doc->slab, value);
    update_caches(el, ns, local);
    embi_notify_attribute(el, ns, ns_uri, local, old, true);
    if (old.len != 0 && !old_inline) {
        embi_slab_free((void *)old.ptr);
    }
}

/* Appends a new attribute. The caller has checked that none with this name exists. */
static void add_at(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                   embi_atom local, embi_dstr value)
{
    embi_attrs *a = embi_el_attrs_reserve(el, (el->attrs ? el->attrs->len : 0) + 1u);
    embi_attr *at = &a->entries[a->len++];
    at->name = local;
    at->prefix = prefix;
    at->ns_uri = ns_uri;
    at->ns = ns;
    at->value = embi_dstr_copy(el->base.doc->slab, value);
    update_caches(el, ns, local);
    embi_notify_attribute(el, ns, ns_uri, local, embi_dstr_empty(), false);
}

static void set_core(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                     embi_atom local, embi_dstr value)
{
    int idx = find_index(el, ns, ns_uri, local);
    if (idx >= 0) {
        set_at(el, idx, value);
    } else {
        add_at(el, ns, ns_uri, prefix, local, value);
    }
}

static bool remove_core(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local)
{
    int idx = find_index(el, ns, ns_uri, local);
    if (idx < 0) {
        return false;
    }
    embi_attrs *a = el->attrs;
    embi_attr removed = a->entries[idx];
    for (uint32_t i = (uint32_t)idx + 1u; i < a->len; i++) {
        a->entries[i - 1u] = a->entries[i];
    }
    a->len--;
    bool removed_inline = embi_attr_value_inline(a, removed.value);
    update_caches(el, ns, local);
    embi_notify_attribute(el, ns, ns_uri, local, removed.value, true);
    if (removed.value.len != 0 && !removed_inline) {
        embi_slab_free((void *)removed.value.ptr);
    }
    maybe_release_attrs(el);
    return true;
}

/* ------------------------------------------------------------------ public API */

embi_dom_err embi_el_set_attribute(embi_element *el, embi_str qname, embi_dstr value)
{
    if (!embi_dom_valid_attr_name(qname)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_str name = fold_name(el, qname, &sb);
    /* The first attribute whose qualified name matches is updated in place, whatever its namespace
     * (setAttribute on an xlink:href attribute changes that attribute). Otherwise a new null
     * namespace attribute is added with the whole name as its local name. */
    int idx = find_qname(el, name);
    if (idx >= 0) {
        set_at(el, idx, value);
    } else {
        add_at(el, EMBI_NS_NULL, 0, 0, embi_atom_intern(name.ptr, name.len), value);
    }
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

embi_dom_err embi_el_set_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri,
                                      embi_str qname, embi_dstr value)
{
    embi_str prefix;
    embi_str local_str;
    if (!embi_dom_split_qname(qname, true, &prefix, &local_str)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    embi_atom uri_atom;
    uint8_t ns = embi_dom_ns_arg(has_ns, ns_uri, &uri_atom);
    embi_dom_err err = embi_dom_check_ns(ns, qname, prefix);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    embi_atom prefix_atom = prefix.len ? embi_atom_intern(prefix.ptr, prefix.len) : 0;
    embi_atom local = embi_atom_intern(local_str.ptr, local_str.len);
    set_core(el, ns, uri_atom, prefix_atom, local, value);
    return EMBI_DOM_OK;
}

void embi_el_put_attr(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix, embi_atom local, embi_dstr value)
{
    int idx = find_index(el, ns, ns == EMBI_NS_OTHER ? ns_uri : 0, local);
    if (idx >= 0) {
        el->attrs->entries[idx].prefix = prefix;
        set_at(el, idx, value);
    } else {
        add_at(el, ns, ns == EMBI_NS_OTHER ? ns_uri : 0, prefix, local, value);
    }
}

void embi_el_set_attr_atom(embi_element *el, embi_atom local, embi_dstr value)
{
    set_core(el, EMBI_NS_NULL, 0, 0, local, value);
}

void embi_el_set_attr_ns_atom(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                              embi_atom local, embi_dstr value)
{
    set_core(el, ns, ns == EMBI_NS_OTHER ? ns_uri : 0, prefix, local, value);
}

uint32_t embi_el_attr_count(const embi_element *el)
{
    return el->attrs ? el->attrs->len : 0;
}

const embi_attr *embi_el_attr_at(const embi_element *el, uint32_t index)
{
    EMBI_ASSERT(el->attrs && index < el->attrs->len);
    return &el->attrs->entries[index];
}

const embi_attr *embi_el_get_attribute(embi_element *el, embi_str qname)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_str name = fold_name(el, qname, &sb);
    int idx = find_qname(el, name);
    embi_sb_free(&sb);
    return idx < 0 ? nullptr : &el->attrs->entries[idx];
}

const embi_attr *embi_el_get_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri,
                                          embi_str local)
{
    embi_atom uri_atom;
    uint8_t ns;
    embi_atom local_atom = embi_atom_find(local.ptr, local.len);
    if (local_atom == 0 || !embi_dom_ns_arg_find(has_ns, ns_uri, &ns, &uri_atom)) {
        return nullptr;
    }
    return embi_el_find_attr(el, ns, uri_atom, local_atom);
}

bool embi_el_has_attribute(embi_element *el, embi_str qname)
{
    return embi_el_get_attribute(el, qname) != nullptr;
}

bool embi_el_has_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri, embi_str local)
{
    return embi_el_get_attribute_ns(el, has_ns, ns_uri, local) != nullptr;
}

bool embi_el_remove_attribute(embi_element *el, embi_str qname)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_str name = fold_name(el, qname, &sb);
    int idx = find_qname(el, name);
    bool removed = false;
    if (idx >= 0) {
        const embi_attr at = el->attrs->entries[idx];
        removed = remove_core(el, at.ns, at.ns_uri, at.name);
    }
    embi_sb_free(&sb);
    return removed;
}

bool embi_el_remove_attribute_ns(embi_element *el, bool has_ns, embi_str ns_uri, embi_str local)
{
    embi_atom uri_atom;
    uint8_t ns;
    embi_atom local_atom = embi_atom_find(local.ptr, local.len);
    if (local_atom == 0 || !embi_dom_ns_arg_find(has_ns, ns_uri, &ns, &uri_atom)) {
        return false;
    }
    return remove_core(el, ns, uri_atom, local_atom);
}

embi_dom_err embi_el_toggle_attribute(embi_element *el, embi_str qname, bool has_force, bool force,
                                      bool *out)
{
    if (!embi_dom_valid_attr_name(qname)) {
        return EMBI_DOM_ERR_INVALID_CHAR;
    }
    bool present = embi_el_has_attribute(el, qname);
    if (present) {
        if (!has_force || !force) {
            embi_el_remove_attribute(el, qname);
            *out = false;
        } else {
            *out = true;
        }
        return EMBI_DOM_OK;
    }
    if (!has_force || force) {
        embi_el_set_attribute(el, qname, embi_dstr_empty());
        *out = true;
    } else {
        *out = false;
    }
    return EMBI_DOM_OK;
}

embi_atom embi_el_id(const embi_element *el)
{
    if (el->id == EMBI_ID_STALE) {
        /* A cache fill: logically const. The DOM is single-threaded (see dom.h). */
        ((embi_element *)el)->id = compute_id(el);
    }
    return el->id;
}

const embi_atom *embi_el_classes(embi_element *el, uint32_t *count)
{
    embi_attrs *a = el->attrs;
    if (!a) {
        *count = 0;
        return nullptr;
    }
    if (!(a->flags & EMBI_ATTRS_CLASSES_VALID)) {
        invalidate_classes(a);
        int idx = find_index(el, EMBI_NS_NULL, 0, EMBI_ATOM_class);
        if (idx >= 0 && a->entries[idx].value.len) {
            embi_sb sb = {nullptr, 0, 0};
            embi_dstr_append_utf8(&sb, a->entries[idx].value);
            EMBI_VEC(embi_atom) toks = {nullptr, 0, 0};
            uint32_t i = 0;
            while (i < sb.len) {
                while (i < sb.len && is_ascii_ws((unsigned char)sb.data[i])) {
                    i++;
                }
                uint32_t start = i;
                while (i < sb.len && !is_ascii_ws((unsigned char)sb.data[i])) {
                    i++;
                }
                if (i > start) {
                    embi_vec_push(&toks, embi_atom_intern(sb.data + start, i - start));
                }
            }
            if (toks.len) {
                a->classes = embi_slab_alloc(el->base.doc->slab, (size_t)toks.len * sizeof(embi_atom));
                SDL_memcpy(a->classes, toks.data, (size_t)toks.len * sizeof(embi_atom));
                a->nclass = toks.len;
            }
            embi_vec_free(&toks);
            embi_sb_free(&sb);
        }
        a->flags |= EMBI_ATTRS_CLASSES_VALID;
    }
    *count = a->nclass;
    return a->classes;
}

bool embi_el_has_class(embi_element *el, embi_atom cls)
{
    uint32_t n = 0;
    const embi_atom *list = embi_el_classes(el, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (list[i] == cls) {
            return true;
        }
    }
    return false;
}

embi_str embi_el_namespace_uri(const embi_element *el)
{
    return embi_dom_ns_uri((uint8_t)el->base.extra, el->attrs ? el->attrs->ns_uri : 0);
}
