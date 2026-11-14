#include "dom/internal.h"
#include "dom/mutation.h"

/* ------------------------------------------------------------------ raw links */

void embi_tree_unlink(embi_node *node)
{
    embi_node *p = node->parent;
    if (!p) {
        return;
    }
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        p->first_child = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        p->last_child = node->prev;
    }
    node->parent = nullptr;
    node->prev = nullptr;
    node->next = nullptr;
}

void embi_set_connected_subtree(embi_node *root, bool on)
{
    for (embi_node *x = root; x; x = embi_next_in_tree(root, x)) {
        if (on) {
            x->flags |= EMBI_NF_CONNECTED;
        } else {
            x->flags &= (uint8_t)~EMBI_NF_CONNECTED;
        }
    }
}

void embi_tree_link_chain(embi_node *parent, embi_node *first, embi_node *last, embi_node *before)
{
    embi_node *prev = before ? before->prev : parent->last_child;
    embi_node *n = first;
    for (;;) {
        embi_node *nx = n->next; /* chain link, read before it is rewritten */
        bool is_last = (n == last);
        n->parent = parent;
        n->prev = prev;
        if (prev) {
            prev->next = n;
        } else {
            parent->first_child = n;
        }
        prev = n;
        if (is_last) {
            break;
        }
        n = nx;
    }
    last->next = before;
    if (before) {
        before->prev = last;
    } else {
        parent->last_child = last;
    }
    if (parent->flags & EMBI_NF_CONNECTED) {
        for (embi_node *x = first;; x = x->next) {
            if (x->first_child == nullptr) {
                x->flags |= EMBI_NF_CONNECTED;
            } else {
                embi_set_connected_subtree(x, true);
            }
            if (x == last) {
                break;
            }
        }
    }
}

void embi_tree_append_raw(embi_node *parent, embi_node *child)
{
    embi_tree_link_chain(parent, child, child, nullptr);
}

/* ------------------------------------------------------------------ validity */

/* A host-including inclusive ancestor test (DOM Standard). The template contents fragment of a
 * template counts the template as its host, so walking from p goes up to the root and, at a
 * template contents fragment, continues from its host. The walk ends at a root that has no host. */
static bool is_host_including_ancestor(const embi_node *node, const embi_node *p)
{
    const embi_node *x = p;
    while (x) {
        if (embi_contains(node, x)) {
            return true;
        }
        const embi_node *root = x;
        while (root->parent) {
            root = root->parent;
        }
        x = root->type == EMBI_NODE_FRAGMENT ? ((const embi_fragment *)root)->host : nullptr;
    }
    return false;
}

/* Any child of p of this type other than except. */
static bool has_child(const embi_node *p, uint8_t type, const embi_node *except)
{
    for (const embi_node *c = p->first_child; c; c = c->next) {
        if (c->type == type && c != except) {
            return true;
        }
    }
    return false;
}

static bool doctype_follows(const embi_node *child)
{
    for (const embi_node *c = child->next; c; c = c->next) {
        if (c->type == EMBI_NODE_DOCTYPE) {
            return true;
        }
    }
    return false;
}

static bool element_precedes(const embi_node *child)
{
    for (const embi_node *c = child->prev; c; c = c->prev) {
        if (c->type == EMBI_NODE_ELEMENT) {
            return true;
        }
    }
    return false;
}

/* Ensure pre-insertion validity (replacing == false) or replace validity (replacing == true)
 * of node into parent before child (for replacing, child is the node being replaced). */
static embi_dom_err ensure_validity(const embi_node *parent, const embi_node *node,
                                    const embi_node *child, bool replacing)
{
    if (parent->type != EMBI_NODE_DOCUMENT && parent->type != EMBI_NODE_FRAGMENT &&
        parent->type != EMBI_NODE_ELEMENT) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (is_host_including_ancestor(node, parent)) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (child && child->parent != parent) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    switch (node->type) {
    case EMBI_NODE_ELEMENT:
    case EMBI_NODE_DOCTYPE:
    case EMBI_NODE_FRAGMENT:
    case EMBI_NODE_TEXT:
    case EMBI_NODE_PI:
    case EMBI_NODE_COMMENT:
        break;
    default:
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (node->type == EMBI_NODE_TEXT && parent->type == EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (node->type == EMBI_NODE_DOCTYPE && parent->type != EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    if (parent->type != EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_OK;
    }
    const embi_node *except = replacing ? child : nullptr;
    if (node->type == EMBI_NODE_FRAGMENT) {
        uint32_t elements = 0;
        bool text = false;
        for (const embi_node *c = node->first_child; c; c = c->next) {
            elements += c->type == EMBI_NODE_ELEMENT;
            text = text || c->type == EMBI_NODE_TEXT;
        }
        if (elements > 1 || text) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        if (elements == 1) {
            if (has_child(parent, EMBI_NODE_ELEMENT, except)) {
                return EMBI_DOM_ERR_HIERARCHY;
            }
            if (child && (doctype_follows(child) || (!replacing && child->type == EMBI_NODE_DOCTYPE))) {
                return EMBI_DOM_ERR_HIERARCHY;
            }
        }
        return EMBI_DOM_OK;
    }
    if (node->type == EMBI_NODE_ELEMENT) {
        if (has_child(parent, EMBI_NODE_ELEMENT, except)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        if (child && (doctype_follows(child) || (!replacing && child->type == EMBI_NODE_DOCTYPE))) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        return EMBI_DOM_OK;
    }
    if (node->type == EMBI_NODE_DOCTYPE) {
        if (has_child(parent, EMBI_NODE_DOCTYPE, except)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        if (child && element_precedes(child)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
        if (!replacing && !child && has_child(parent, EMBI_NODE_ELEMENT, nullptr)) {
            return EMBI_DOM_ERR_HIERARCHY;
        }
    }
    return EMBI_DOM_OK;
}

/* Replace all (ParentNode.replaceChildren): validity of node under parent when every child of parent is about to be
 * removed, so a Document's doctype and element do not conflict with the new ones (DOM Standard issue 1045). */
static embi_dom_err replace_all_validity(const embi_node *parent, const embi_node *node)
{
    if (parent->type != EMBI_NODE_DOCUMENT) {
        return ensure_validity(parent, node, nullptr, false);
    }
    if (is_host_including_ancestor(node, parent)) {
        return EMBI_DOM_ERR_HIERARCHY;
    }
    switch (node->type) {
    case EMBI_NODE_ELEMENT:
    case EMBI_NODE_DOCTYPE:
    case EMBI_NODE_PI:
    case EMBI_NODE_COMMENT:
        return EMBI_DOM_OK;
    case EMBI_NODE_FRAGMENT: {
        uint32_t elements = 0;
        for (const embi_node *c = node->first_child; c; c = c->next) {
            if (c->type == EMBI_NODE_TEXT) {
                return EMBI_DOM_ERR_HIERARCHY;
            }
            elements += c->type == EMBI_NODE_ELEMENT;
        }
        return elements > 1 ? EMBI_DOM_ERR_HIERARCHY : EMBI_DOM_OK;
    }
    default:
        return EMBI_DOM_ERR_HIERARCHY;
    }
}

/* ------------------------------------------------------------------ adoption */

/* last caches the previous owner, so a subtree that comes from one source slab (the usual case)
 * costs one pointer compare per payload instead of a scan of dst->kept. */
static void keep_owner(embi_slab *dst, embi_slab **last, const void *p)
{
    if (p) {
        embi_slab *owner = embi_slab_owner(p);
        if (owner != *last) {
            embi_slab_keep(dst, owner);
            *last = owner;
        }
    }
}

/* Moves the subtree rooted at root into doc: node documents change, and doc keeps alive the
 * slabs that the moved memory lives in. Template contents are re-created in the template owner
 * of doc so that they never point at an owner document that may go away. */
static void adopt_subtree(embi_document *doc, embi_node *root)
{
    embi_slab *last = nullptr;
    for (embi_node *x = root; x; x = embi_next_in_tree(root, x)) {
        keep_owner(doc->slab, &last, x);
        x->doc = doc;
        if (x->type == EMBI_NODE_TEXT || x->type == EMBI_NODE_COMMENT || x->type == EMBI_NODE_PI) {
            if (!(x->flags & EMBI_NF_INLINE_DATA)) {
                keep_owner(doc->slab, &last, ((embi_chardata *)x)->data.ptr);
            }
        } else if (x->type == EMBI_NODE_DOCTYPE) {
            embi_doctype *dt = (embi_doctype *)x;
            keep_owner(doc->slab, &last, dt->name.ptr);
            keep_owner(doc->slab, &last, dt->public_id.ptr);
            keep_owner(doc->slab, &last, dt->system_id.ptr);
        } else if (x->type == EMBI_NODE_ELEMENT) {
            embi_element *el = (embi_element *)x;
            if (el->attrs) {
                keep_owner(doc->slab, &last, el->attrs);
                keep_owner(doc->slab, &last, el->attrs->classes);
                for (uint32_t i = 0; i < el->attrs->len; i++) {
                    /* Inline values live inside the block, which is kept above. */
                    if (!embi_attr_value_inline(el->attrs, el->attrs->entries[i].value)) {
                        keep_owner(doc->slab, &last, el->attrs->entries[i].value.ptr);
                    }
                }
                embi_node *content = el->attrs->content;
                if (content && content->doc != embi_document_template_owner(doc)) {
                    embi_node *fresh = embi_clone_tree(embi_document_template_owner(doc), content, true);
                    ((embi_fragment *)fresh)->host = x;
                    el->attrs->content = fresh;
                    embi_destroy_detached(content);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ insert, remove */

static bool detach_ex(embi_node *node, bool record, bool notify);

static bool detach_impl(embi_node *node, bool record)
{
    return detach_ex(node, record, true);
}

/* Inserts node (a single node, or the children of a fragment) before child, without validity
 * checks. The caller has already validated the insertion. record: queue the childList record of the insertion.
 * replaced (when not nullptr) is the node that the insertion replaces (already detached): the same record reports it
 * as removed, with rprev and rnext as its former siblings (replaceChild is one record, DOM Standard). */
static void insert_node(embi_node *parent, embi_node *node, embi_node *child, bool record, embi_node *replaced,
                        embi_node *rprev, embi_node *rnext)
{
    embi_document *doc = parent->doc;
    embi_node *first;
    embi_node *last;
    if (node->type == EMBI_NODE_FRAGMENT) {
        first = node->first_child;
        last = node->last_child;
        if (!first) {
            if (replaced) {
                embi_mut_childlist(parent, nullptr, nullptr, &replaced, 1, rprev, rnext);
            }
            return;
        }
        /* Empty the fragment. The children keep their chain links for the link step below. */
        node->first_child = nullptr;
        node->last_child = nullptr;
        first->prev = nullptr;
        last->next = nullptr;
        for (embi_node *c = first;; c = c->next) {
            c->parent = nullptr;
            if (c == last) {
                break;
            }
        }
        embi_notify_children(node, nullptr, nullptr, first, last);
    } else {
        if (node->parent) {
            /* Adopting the node removes it from its old parent: that removal is recorded (DOM Standard, adopt). */
            detach_impl(node, true);
            if (node->parent) {
                detach_ex(node, true, false); /* a listener of the focus fixup moved it again: take it out, quietly */
            }
            if (child && child->parent != parent) {
                child = nullptr; /* the same listener took the reference child out of parent */
            }
            if (embi_contains(node, parent)) {
                return; /* ... or moved parent into node */
            }
        }
        first = node;
        last = node;
        node->prev = nullptr;
        node->next = nullptr;
    }
    for (embi_node *c = first;; c = c->next) {
        if (c->doc != doc) {
            adopt_subtree(doc, c);
        }
        if (c == last) {
            break;
        }
    }
    bool connected = parent->flags & EMBI_NF_CONNECTED;
    embi_tree_link_chain(parent, first, last, child);
    if (connected) {
        embi_document_idmap_invalidate(doc);
    }
    for (embi_node *c = first;; c = c->next) {
        embi_notify_inserted(c);
        if (c == last) {
            break;
        }
    }
    embi_notify_children(parent, first, last, nullptr, nullptr);
    if (replaced) {
        embi_mut_childlist(parent, first, last, &replaced, 1, rprev, rnext);
    } else if (record) {
        embi_mut_childlist(parent, first, last, nullptr, 0, first->prev, last->next);
    }
}

/* Detaches node from its parent: the tree update and the observers, the transient registrations of its subtree and,
 * when record is set, the childList record of the removal. */
static bool detach_ex(embi_node *node, bool record, bool notify)
{
    embi_node *parent = node->parent;
    if (!parent) {
        return false;
    }
    if (notify) {
        embi_notify_pre_remove(node);
        if (node->parent != parent) {
            return false; /* a listener (the focus fixup's blur) already removed or moved it */
        }
    }
    embi_node *prev = node->prev;
    embi_node *next = node->next;
    bool was_connected = node->flags & EMBI_NF_CONNECTED;
    embi_tree_unlink(node);
    if (was_connected) {
        embi_set_connected_subtree(node, false);
        embi_document_idmap_invalidate(parent->doc);
    }
    embi_notify_removed(node);
    embi_notify_children(parent, nullptr, nullptr, node, node);
    embi_mut_removed(parent, &node, 1);
    if (record) {
        embi_mut_childlist(parent, nullptr, nullptr, &node, 1, prev, next);
    }
    return true;
}

void embi_detach(embi_node *node)
{
    detach_impl(node, true);
}

embi_dom_err embi_pre_insert(embi_node *parent, embi_node *node, embi_node *child)
{
    embi_dom_err err = ensure_validity(parent, node, child, false);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    embi_node *ref = child;
    if (ref == node) {
        ref = node->next;
    }
    insert_node(parent, node, ref, true, nullptr, nullptr, nullptr);
    return EMBI_DOM_OK;
}

embi_dom_err embi_append_child(embi_node *parent, embi_node *node)
{
    return embi_pre_insert(parent, node, nullptr);
}

embi_dom_err embi_replace_child(embi_node *parent, embi_node *node, embi_node *child)
{
    embi_dom_err err = ensure_validity(parent, node, child, true);
    if (err != EMBI_DOM_OK) {
        return err;
    }
    embi_node *ref = child->next;
    if (ref == node) {
        ref = node->next;
    }
    /* The record's siblings: the previous sibling of child, and the reference child (ref) as the next sibling. */
    embi_node *prev = child->prev;
    detach_impl(child, false);
    insert_node(parent, node, ref, false, child, prev, ref);
    return EMBI_DOM_OK;
}

embi_dom_err embi_remove_child(embi_node *parent, embi_node *child)
{
    if (child->parent != parent) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    embi_detach(child);
    return EMBI_DOM_OK;
}

embi_dom_err embi_replace_all(embi_node *parent, embi_node *node)
{
    if (node) {
        embi_dom_err err = replace_all_validity(parent, node);
        if (err != EMBI_DOM_OK) {
            return err;
        }
    }
    /* node may sit inside one of the children that are about to be freed. */
    if (node && node->type != EMBI_NODE_FRAGMENT) {
        detach_impl(node, true);
    }
    if (!embi_mut_watched(parent)) {
        while (parent->first_child) {
            embi_node *child = parent->first_child;
            (void)detach_impl(child, false);
            /* The removed children have no other handle, so free them unless JS wraps them. */
            embi_destroy_if_unowned(child);
        }
        if (node) {
            insert_node(parent, node, nullptr, false, nullptr, nullptr, nullptr);
        }
        return EMBI_DOM_OK;
    }
    /* Observed: one record for all the removed children and the inserted nodes (DOM Standard replace all). The removed
     * children are freed only after the record is queued, which holds them. */
    embi_node *first_add = nullptr;
    embi_node *last_add = nullptr;
    if (node) {
        if (node->type == EMBI_NODE_FRAGMENT) {
            first_add = node->first_child;
            last_add = node->last_child;
        } else {
            first_add = node;
            last_add = node;
        }
    }
    uint32_t n = 0;
    for (embi_node *c = parent->first_child; c; c = c->next) {
        n++;
    }
    embi_node **removed = n ? embi_alloc(n * sizeof *removed) : nullptr;
    uint32_t k = 0;
    while (parent->first_child) {
        embi_node *child = parent->first_child;
        if (detach_impl(child, false)) {
            if (k < n) {
                removed[k++] = child; /* not one a listener of the focus fixup took elsewhere first */
            } else {
                embi_destroy_if_unowned(child); /* one a listener added meanwhile: not in the record */
            }
        }
    }
    if (node) {
        insert_node(parent, node, nullptr, false, nullptr, nullptr, nullptr);
    }
    embi_mut_childlist(parent, first_add, last_add, removed, k, nullptr, nullptr);
    for (uint32_t i = 0; i < k; i++) {
        embi_destroy_if_unowned(removed[i]);
    }
    if (removed) {
        embi_free(removed, n * sizeof *removed);
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_adopt_node(embi_document *doc, embi_node *node)
{
    if (node->type == EMBI_NODE_DOCUMENT) {
        return EMBI_DOM_ERR_NOT_SUPPORTED;
    }
    embi_detach(node);
    if (node->doc != doc) {
        adopt_subtree(doc, node);
    }
    return EMBI_DOM_OK;
}
