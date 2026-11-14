#include "dom/internal.h"
#include "dom/mutation.h"

/* ------------------------------------------------------------------ creation */

/* Inlined so that every caller zeroes a constant size (the element, text and fragment structs
 * are a few stores each) instead of calling memset. */
[[gnu::always_inline]] static inline embi_node *node_alloc(embi_document *doc, uint8_t type,
                                                            size_t size)
{
    embi_node *n = embi_slab_alloc(doc->slab, size);
    SDL_memset(n, 0, size);
    n->type = type;
    n->doc = doc;
    return n;
}

embi_element *embi_new_element(embi_document *doc, uint8_t ns, embi_atom ns_uri, embi_atom prefix,
                               embi_atom local)
{
    embi_element *el = (embi_element *)node_alloc(doc, EMBI_NODE_ELEMENT, sizeof *el);
    el->base.extra = ns;
    el->base.name = local;
    el->prefix = prefix;
    if (ns == EMBI_NS_OTHER) {
        embi_el_attrs_ensure(el)->ns_uri = ns_uri;
    }
    return el;
}

/* Payloads up to this many bytes are stored in the text node's own block. */
#define EMBI_CD_INLINE_MAX 256u

embi_chardata *embi_new_text(embi_document *doc, embi_dstr data)
{
    size_t bytes = (size_t)data.len * (data.wide ? 2u : 1u);
    if (data.len != 0 && bytes <= EMBI_CD_INLINE_MAX) {
        /* The struct is zeroed with a constant size; only the payload size varies. */
        embi_chardata *cd = (embi_chardata *)embi_slab_alloc(doc->slab, sizeof *cd + bytes);
        *cd = (embi_chardata){.base = {.type = EMBI_NODE_TEXT,
                                       .flags = EMBI_NF_INLINE_DATA,
                                       .doc = doc}};
        unsigned char *p = (unsigned char *)cd + sizeof *cd;
        embi_copy_bytes(p, data.ptr, bytes);
        cd->data = (embi_dstr){p, data.len, data.wide};
        return cd;
    }
    embi_chardata *cd = (embi_chardata *)node_alloc(doc, EMBI_NODE_TEXT, sizeof *cd);
    cd->data = embi_dstr_copy(doc->slab, data);
    return cd;
}

embi_chardata *embi_new_text_utf8(embi_document *doc, embi_str text)
{
    bool wide = false;
    bool ascii = embi_bytes_ascii(text.ptr, text.len);
    uint32_t n = ascii ? text.len : embi_utf8_units(text, &wide);
    size_t bytes = (size_t)n * (wide ? 2u : 1u);
    if (n != 0 && bytes <= EMBI_CD_INLINE_MAX) {
        embi_chardata *cd = (embi_chardata *)embi_slab_alloc(doc->slab, sizeof *cd + bytes);
        *cd = (embi_chardata){.base = {.type = EMBI_NODE_TEXT,
                                       .flags = EMBI_NF_INLINE_DATA,
                                       .doc = doc}};
        unsigned char *p = (unsigned char *)cd + sizeof *cd;
        if (ascii) {
            embi_copy_bytes(p, text.ptr, bytes);
        } else {
            embi_utf8_write(p, wide, text);
        }
        cd->data = (embi_dstr){p, n, wide};
        return cd;
    }
    embi_chardata *cd = (embi_chardata *)node_alloc(doc, EMBI_NODE_TEXT, sizeof *cd);
    cd->data = embi_dstr_from_utf8(doc->slab, text);
    return cd;
}

embi_chardata *embi_new_comment(embi_document *doc, embi_dstr data)
{
    embi_chardata *cd = (embi_chardata *)node_alloc(doc, EMBI_NODE_COMMENT, sizeof *cd);
    cd->data = embi_dstr_copy(doc->slab, data);
    return cd;
}

embi_chardata *embi_new_pi(embi_document *doc, embi_atom target, embi_dstr data)
{
    embi_chardata *cd = (embi_chardata *)node_alloc(doc, EMBI_NODE_PI, sizeof *cd);
    cd->base.name = target;
    cd->data = embi_dstr_copy(doc->slab, data);
    return cd;
}

embi_doctype *embi_new_doctype(embi_document *doc, embi_dstr name, embi_dstr public_id,
                               embi_dstr system_id)
{
    embi_doctype *dt = (embi_doctype *)node_alloc(doc, EMBI_NODE_DOCTYPE, sizeof *dt);
    dt->name = embi_dstr_copy(doc->slab, name);
    dt->public_id = embi_dstr_copy(doc->slab, public_id);
    dt->system_id = embi_dstr_copy(doc->slab, system_id);
    return dt;
}

embi_node *embi_new_fragment(embi_document *doc)
{
    return node_alloc(doc, EMBI_NODE_FRAGMENT, sizeof(embi_fragment));
}

embi_node *embi_template_contents(embi_element *el)
{
    embi_attrs *a = embi_el_attrs_ensure(el);
    if (!a->content) {
        embi_document *owner = embi_document_template_owner(el->base.doc);
        a->content = embi_new_fragment(owner);
        ((embi_fragment *)a->content)->host = &el->base;
    }
    return a->content;
}

/* ------------------------------------------------------------------ copies */

/* Copies every attribute of src into dst (dst has none yet). Values are copied into dst's
 * document slab. */
static void copy_attrs(embi_element *dst, const embi_attrs *src)
{
    if (src->len == 0) {
        return;
    }
    embi_attrs *da = embi_el_attrs_reserve(dst, src->len);
    for (uint32_t i = 0; i < src->len; i++) {
        embi_attr at = src->entries[i];
        at.value = embi_dstr_copy(dst->base.doc->slab, at.value);
        da->entries[da->len++] = at;
    }
    embi_el_recompute_id(dst);
}

/* Shallow copy of one node into doc. Returns nullptr for documents. */
static embi_node *clone_shallow(embi_document *doc, const embi_node *src)
{
    switch (src->type) {
    case EMBI_NODE_ELEMENT: {
        const embi_element *s = (const embi_element *)src;
        embi_atom ns_uri = s->attrs ? s->attrs->ns_uri : 0;
        embi_element *el = embi_new_element(doc, (uint8_t)src->extra, ns_uri, s->prefix, src->name);
        if (s->attrs) {
            copy_attrs(el, s->attrs);
        }
        return &el->base;
    }
    case EMBI_NODE_TEXT: {
        embi_node *t = &embi_new_text(doc, ((const embi_chardata *)src)->data)->base;
        t->flags |= (uint8_t)(src->flags & EMBI_NF_CDATA);
        return t;
    }
    case EMBI_NODE_COMMENT:
        return &embi_new_comment(doc, ((const embi_chardata *)src)->data)->base;
    case EMBI_NODE_PI:
        return &embi_new_pi(doc, src->name, ((const embi_chardata *)src)->data)->base;
    case EMBI_NODE_DOCTYPE: {
        const embi_doctype *s = (const embi_doctype *)src;
        return &embi_new_doctype(doc, s->name, s->public_id, s->system_id)->base;
    }
    case EMBI_NODE_FRAGMENT:
        return embi_new_fragment(doc);
    case EMBI_NODE_ATTR: {
        const embi_attr_node *s = (const embi_attr_node *)src;
        return &embi_new_attr_node(doc, s->ns, s->ns_uri, s->prefix, s->local, embi_attr_node_value(s))->base;
    }
    default:
        return nullptr;
    }
}

/* Clones the template contents of src (when it has any) into dst. */
static void clone_template_contents(embi_document *doc, const embi_element *src, embi_element *dst)
{
    if (!src->attrs || !src->attrs->content) {
        return;
    }
    if (src->base.extra != EMBI_NS_HTML || src->base.name != EMBI_ATOM_template) {
        return;
    }
    embi_document *owner = embi_document_template_owner(doc);
    embi_node *c = embi_clone_tree(owner, src->attrs->content, true);
    ((embi_fragment *)c)->host = &dst->base;
    embi_el_attrs_ensure(dst)->content = c;
}

embi_node *embi_clone_tree(embi_document *doc, const embi_node *src, bool deep)
{
    embi_node *root = clone_shallow(doc, src);
    if (!root) {
        return nullptr;
    }
    if (!deep) {
        return root;
    }
    if (src->type == EMBI_NODE_ELEMENT) {
        clone_template_contents(doc, (const embi_element *)src, (embi_element *)root);
    }
    /* Iterative pre-order walk that keeps the source and the copy in step. */
    const embi_node *s = src;
    embi_node *d = root;
    for (;;) {
        if (s->first_child) {
            s = s->first_child;
            embi_node *c = clone_shallow(doc, s);
            embi_tree_append_raw(d, c);
            if (s->type == EMBI_NODE_ELEMENT) {
                clone_template_contents(doc, (const embi_element *)s, (embi_element *)c);
            }
            d = c;
            continue;
        }
        while (s != src && !s->next) {
            s = s->parent;
            d = d->parent;
        }
        if (s == src) {
            break;
        }
        s = s->next;
        embi_node *c = clone_shallow(doc, s);
        embi_tree_append_raw(d->parent, c);
        if (s->type == EMBI_NODE_ELEMENT) {
            clone_template_contents(doc, (const embi_element *)s, (embi_element *)c);
        }
        d = c;
    }
    return root;
}

embi_node *embi_clone_node(embi_node *node, bool deep)
{
    if (node->type == EMBI_NODE_DOCUMENT) {
        const embi_document *src = (const embi_document *)node;
        embi_document *d = embi_document_new(src->is_html);
        d->quirks = src->quirks;
        d->ctype = src->ctype;
        d->xml_class = src->xml_class;
        if (deep) {
            for (embi_node *c = node->first_child; c; c = c->next) {
                embi_tree_append_raw(&d->base, embi_clone_tree(d, c, true));
            }
        }
        return &d->base;
    }
    return embi_clone_tree(node->type == EMBI_NODE_ATTR ? embi_attr_node_doc((const embi_attr_node *)node) : node->doc,
                           node, deep);
}

embi_node *embi_import_node(embi_document *doc, embi_node *node, bool deep)
{
    if (node->type == EMBI_NODE_DOCUMENT) {
        return nullptr;
    }
    return embi_clone_tree(doc, node, deep);
}

/* ------------------------------------------------------------------ destruction */

static void destroy_leaf(embi_node *n)
{
    switch (n->type) {
    case EMBI_NODE_ELEMENT: {
        embi_attrs *a = ((embi_element *)n)->attrs;
        if (a) {
            for (uint32_t i = 0; i < a->len; i++) {
                embi_attr_value_free(a, a->entries[i].value);
            }
            if (a->classes) {
                embi_slab_free(a->classes);
            }
            if (a->content) {
                embi_destroy_detached(a->content);
            }
            embi_slab_free(a);
        }
        break;
    }
    case EMBI_NODE_TEXT:
    case EMBI_NODE_COMMENT:
    case EMBI_NODE_PI:
        if (!(n->flags & EMBI_NF_INLINE_DATA)) {
            embi_dstr_free(((embi_chardata *)n)->data);
        }
        break;
    case EMBI_NODE_ATTR:
        embi_dstr_free(((embi_attr_node *)n)->value);
        break;
    case EMBI_NODE_DOCTYPE: {
        embi_doctype *dt = (embi_doctype *)n;
        embi_dstr_free(dt->name);
        embi_dstr_free(dt->public_id);
        embi_dstr_free(dt->system_id);
        break;
    }
    default:
        break;
    }
    embi_slab_free(n);
}

/* True when a node under root is held by a MutationObserver record or observation (dom/mutation.h): such a subtree
 * stays allocated until its document is freed. */
static bool subtree_held(const embi_node *root)
{
    for (const embi_node *x = root; x; x = embi_next_in_tree(root, x)) {
        if (x->flags & EMBI_NF_HELD) {
            return true;
        }
    }
    return false;
}

void embi_destroy_detached(embi_node *root)
{
    /* Only detached, non-document nodes can be freed. Anything else is a caller error. */
    EMBI_ASSERT(root->parent == nullptr && root->type != EMBI_NODE_DOCUMENT);
    if (root->parent || root->type == EMBI_NODE_DOCUMENT) {
        return;
    }
    if (subtree_held(root)) {
        return;
    }
    /* Post-order: descend to a leaf, free it, then continue with the next sibling or the parent. */
    embi_node *n = root;
    for (;;) {
        while (n->first_child) {
            n = n->first_child;
        }
        embi_node *nx = n->next;
        embi_node *p = n->parent;
        embi_tree_unlink(n);
        bool done = (n == root);
        destroy_leaf(n);
        if (done) {
            break;
        }
        n = nx ? nx : p;
    }
}

/* True when no node under root (template contents included) has a wrapper. */
static bool scan_unowned(const embi_node *root)
{
    for (const embi_node *x = root; x; x = embi_next_in_tree(root, x)) {
        if (x->wrapper || (x->flags & EMBI_NF_HELD)) {
            return false;
        }
        if (x->type == EMBI_NODE_ELEMENT) {
            const embi_attrs *a = ((const embi_element *)x)->attrs;
            if (a && a->content && !scan_unowned(a->content)) {
                return false;
            }
        }
    }
    return true;
}

bool embi_destroy_if_unowned(embi_node *root)
{
    if (!scan_unowned(root)) {
        return false;
    }
    embi_destroy_detached(root);
    return true;
}

/* ------------------------------------------------------------------ text content */

bool embi_text_content_get(embi_node *node, embi_dbuf *out)
{
    switch (node->type) {
    case EMBI_NODE_DOCUMENT:
    case EMBI_NODE_DOCTYPE:
        return false;
    case EMBI_NODE_TEXT:
    case EMBI_NODE_COMMENT:
    case EMBI_NODE_PI:
        embi_dbuf_append(out, ((embi_chardata *)node)->data);
        return true;
    case EMBI_NODE_ATTR:
        embi_dbuf_append(out, embi_attr_node_value((embi_attr_node *)node));
        return true;
    default:
        for (embi_node *x = node->first_child; x; x = embi_next_in_tree(node, x)) {
            if (x->type == EMBI_NODE_TEXT) {
                embi_dbuf_append(out, ((embi_chardata *)x)->data);
            }
        }
        return true;
    }
}

void embi_chardata_adopt(embi_chardata *cd, embi_dstr fresh)
{
    embi_dstr old = cd->data;
    bool was_inline = (cd->base.flags & EMBI_NF_INLINE_DATA) != 0;
    cd->data = fresh;
    cd->base.flags &= (uint8_t)~EMBI_NF_INLINE_DATA;
    if (!was_inline) {
        embi_dstr_free(old);
    }
}

void embi_chardata_set(embi_chardata *cd, embi_dstr value)
{
    embi_mut_chardata(&cd->base, cd->data, true);
    embi_chardata_adopt(cd, embi_dstr_copy(cd->base.doc->slab, value));
    embi_notify_char_data(&cd->base);
}

embi_dom_err embi_text_content_set(embi_node *node, embi_dstr value)
{
    switch (node->type) {
    case EMBI_NODE_ELEMENT:
    case EMBI_NODE_FRAGMENT: {
        embi_node *text = nullptr;
        if (value.len) {
            text = &embi_new_text(node->doc, value)->base;
        }
        return embi_replace_all(node, text);
    }
    case EMBI_NODE_TEXT:
    case EMBI_NODE_COMMENT:
    case EMBI_NODE_PI:
        embi_chardata_set((embi_chardata *)node, value);
        return EMBI_DOM_OK;
    case EMBI_NODE_ATTR:
        embi_attr_node_set_value((embi_attr_node *)node, value);
        return EMBI_DOM_OK;
    default:
        return EMBI_DOM_OK;
    }
}

/* An exclusive Text node: a Text that is not a CDATASection. */
static bool exclusive_text(const embi_node *n)
{
    return n->type == EMBI_NODE_TEXT && !(n->flags & EMBI_NF_CDATA);
}

void embi_normalize(embi_node *root)
{
    embi_node *n = root->first_child;
    while (n) {
        if (!exclusive_text(n)) {
            n = embi_next_in_tree(root, n);
            continue;
        }
        if (((embi_chardata *)n)->data.len == 0) {
            embi_node *nx = embi_next_in_tree(root, n);
            embi_detach(n);
            embi_destroy_if_unowned(n);
            n = nx;
            continue;
        }
        if (n->next && exclusive_text(n->next)) {
            /* DOM Standard normalize(): the data of n is replaced first (one characterData record), then the contiguous
             * text nodes are removed (their removal records follow). */
            embi_dbuf b;
            embi_dbuf_init(&b, n->doc->slab);
            embi_dbuf_append(&b, ((embi_chardata *)n)->data);
            for (embi_node *sib = n->next; sib && exclusive_text(sib); sib = sib->next) {
                embi_dbuf_append(&b, ((embi_chardata *)sib)->data);
            }
            embi_dstr merged = embi_dbuf_finish(&b);
            embi_dbuf_free(&b);
            embi_mut_chardata(n, ((embi_chardata *)n)->data, true);
            embi_chardata_adopt((embi_chardata *)n, merged);
            embi_notify_char_data(n);
            while (n->next && exclusive_text(n->next)) {
                embi_node *sib = n->next;
                embi_detach(sib);
                embi_destroy_if_unowned(sib);
            }
        }
        n = embi_next_in_tree(root, n);
    }
}

/* ------------------------------------------------------------------ queries */

embi_node *embi_get_root(embi_node *node)
{
    while (node->parent) {
        node = node->parent;
    }
    return node;
}

embi_node *embi_next_skipping_children(const embi_node *root, const embi_node *node)
{
    const embi_node *x = node;
    while (x != root) {
        if (x->next) {
            return x->next;
        }
        x = x->parent;
        if (!x) {
            return nullptr;
        }
    }
    return nullptr;
}

embi_node *embi_next_in_tree(const embi_node *root, const embi_node *node)
{
    if (node->first_child) {
        return node->first_child;
    }
    return embi_next_skipping_children(root, node);
}

embi_node *embi_prev_in_tree(const embi_node *root, const embi_node *node)
{
    if (node == root) {
        return nullptr;
    }
    embi_node *x = node->prev;
    if (!x) {
        return node->parent;
    }
    while (x->last_child) {
        x = x->last_child;
    }
    return x;
}

bool embi_contains(const embi_node *a, const embi_node *b)
{
    for (const embi_node *x = b; x; x = x->parent) {
        if (x == a) {
            return true;
        }
    }
    return false;
}

/* compareDocumentPosition with an Attr on either side (DOM Standard: node1 is the argument b, node2 the receiver a). */
static uint16_t compare_attr_position(const embi_node *a, const embi_node *b)
{
    const embi_attr_node *attr1 = b->type == EMBI_NODE_ATTR ? (const embi_attr_node *)b : nullptr;
    const embi_attr_node *attr2 = a->type == EMBI_NODE_ATTR ? (const embi_attr_node *)a : nullptr;
    const embi_node *n1 = attr1 ? (const embi_node *)attr1->owner : b;
    const embi_node *n2 = attr2 ? (const embi_node *)attr2->owner : a;
    if (attr1 && attr2 && n1 && n1 == n2) {
        const embi_element *el = (const embi_element *)n2;
        for (uint32_t i = 0; el->attrs && i < el->attrs->len; i++) {
            const embi_attr *e = &el->attrs->entries[i];
            if (e->ns == attr1->ns && e->ns_uri == attr1->ns_uri && e->name == attr1->local) {
                return EMBI_POS_IMPL_SPECIFIC | EMBI_POS_PRECEDING;
            }
            if (e->ns == attr2->ns && e->ns_uri == attr2->ns_uri && e->name == attr2->local) {
                return EMBI_POS_IMPL_SPECIFIC | EMBI_POS_FOLLOWING;
            }
        }
    }
    if (!n1 || !n2 || embi_get_root((embi_node *)n1) != embi_get_root((embi_node *)n2)) {
        uint16_t side = (uintptr_t)a < (uintptr_t)b ? EMBI_POS_FOLLOWING : EMBI_POS_PRECEDING;
        return EMBI_POS_DISCONNECTED | EMBI_POS_IMPL_SPECIFIC | side;
    }
    if ((n1 != n2 && embi_contains(n1, n2) && !attr1) || (n1 == n2 && attr2)) {
        return EMBI_POS_CONTAINS | EMBI_POS_PRECEDING;
    }
    if ((n1 != n2 && embi_contains(n2, n1) && !attr2) || (n1 == n2 && attr1)) {
        return EMBI_POS_CONTAINED_BY | EMBI_POS_FOLLOWING;
    }
    uint16_t r = embi_compare_position(n2, n1);
    return r & (EMBI_POS_PRECEDING | EMBI_POS_FOLLOWING);
}

uint16_t embi_compare_position(const embi_node *a, const embi_node *b)
{
    if (a == b) {
        return 0;
    }
    if (a->type == EMBI_NODE_ATTR || b->type == EMBI_NODE_ATTR) {
        return compare_attr_position(a, b);
    }
    if (embi_get_root((embi_node *)a) != embi_get_root((embi_node *)b)) {
        uint16_t side = (uintptr_t)a < (uintptr_t)b ? EMBI_POS_FOLLOWING : EMBI_POS_PRECEDING;
        return EMBI_POS_DISCONNECTED | EMBI_POS_IMPL_SPECIFIC | side;
    }
    if (embi_contains(a, b)) {
        return EMBI_POS_CONTAINED_BY | EMBI_POS_FOLLOWING;
    }
    if (embi_contains(b, a)) {
        return EMBI_POS_CONTAINS | EMBI_POS_PRECEDING;
    }
    /* Bring both to the same depth, then find the children of the common ancestor. */
    const embi_node *x = a;
    const embi_node *y = b;
    int dx = 0;
    int dy = 0;
    for (const embi_node *t = a; t->parent; t = t->parent) {
        dx++;
    }
    for (const embi_node *t = b; t->parent; t = t->parent) {
        dy++;
    }
    while (dx > dy) {
        x = x->parent;
        dx--;
    }
    while (dy > dx) {
        y = y->parent;
        dy--;
    }
    while (x->parent != y->parent) {
        x = x->parent;
        y = y->parent;
    }
    /* x and y are siblings. b follows a when y is reachable from x going forward. */
    for (const embi_node *s = x; s; s = s->next) {
        if (s == y) {
            return EMBI_POS_FOLLOWING;
        }
    }
    return EMBI_POS_PRECEDING;
}

static bool shallow_equal(const embi_node *a, const embi_node *b)
{
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case EMBI_NODE_ELEMENT: {
        const embi_element *ea = (const embi_element *)a;
        const embi_element *eb = (const embi_element *)b;
        if (a->extra != b->extra || a->name != b->name || ea->prefix != eb->prefix) {
            return false;
        }
        if (a->extra == EMBI_NS_OTHER &&
            (ea->attrs ? ea->attrs->ns_uri : 0) != (eb->attrs ? eb->attrs->ns_uri : 0)) {
            return false;
        }
        uint32_t na = ea->attrs ? ea->attrs->len : 0;
        uint32_t nb = eb->attrs ? eb->attrs->len : 0;
        if (na != nb) {
            return false;
        }
        /* Each attribute of a must have an identical counterpart in b with the same namespace,
         * local name and value. The prefix is not compared: it does not affect attribute equality
         * (xlink:href and foo:href in one namespace are equal). Names are unique per element, so
         * the first match decides. */
        for (uint32_t i = 0; i < na; i++) {
            const embi_attr *pa = &ea->attrs->entries[i];
            bool found = false;
            for (uint32_t j = 0; j < nb && !found; j++) {
                const embi_attr *pb = &eb->attrs->entries[j];
                if (pa->name == pb->name && pa->ns == pb->ns && pa->ns_uri == pb->ns_uri) {
                    found = embi_dstr_eq(pa->value, pb->value);
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }
    case EMBI_NODE_TEXT:
    case EMBI_NODE_COMMENT:
        return embi_dstr_eq(((const embi_chardata *)a)->data, ((const embi_chardata *)b)->data);
    case EMBI_NODE_PI:
        return a->name == b->name &&
               embi_dstr_eq(((const embi_chardata *)a)->data, ((const embi_chardata *)b)->data);
    case EMBI_NODE_ATTR: {
        const embi_attr_node *aa = (const embi_attr_node *)a;
        const embi_attr_node *ab = (const embi_attr_node *)b;
        return aa->local == ab->local && aa->ns == ab->ns && aa->ns_uri == ab->ns_uri &&
               embi_dstr_eq(embi_attr_node_value(aa), embi_attr_node_value(ab));
    }
    case EMBI_NODE_DOCTYPE: {
        const embi_doctype *da = (const embi_doctype *)a;
        const embi_doctype *db = (const embi_doctype *)b;
        return embi_dstr_eq(da->name, db->name) && embi_dstr_eq(da->public_id, db->public_id) &&
               embi_dstr_eq(da->system_id, db->system_id);
    }
    default:
        return true;
    }
}

bool embi_is_equal_node(const embi_node *a, const embi_node *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b || !shallow_equal(a, b)) {
        return false;
    }
    /* Walk both trees in step, in pre-order below a and b. Every sibling run must end at the same
     * time in both trees: when x has no next sibling, y must have none either, before climbing. */
    const embi_node *x = a;
    const embi_node *y = b;
    for (;;) {
        if (x->first_child || y->first_child) {
            if (!x->first_child || !y->first_child) {
                return false;
            }
            x = x->first_child;
            y = y->first_child;
        } else {
            for (;;) {
                if (x == a) {
                    return true;
                }
                if (x->next) {
                    if (!y->next) {
                        return false;
                    }
                    x = x->next;
                    y = y->next;
                    break;
                }
                if (y->next) {
                    return false;
                }
                x = x->parent;
                y = y->parent;
            }
        }
        if (!shallow_equal(x, y)) {
            return false;
        }
    }
}
