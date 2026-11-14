/*
 * NodeIterator and TreeWalker (DOM Standard section 6, "Traversal"), W5f. NodeFilter itself is only constants (the
 * generated glue). The filter is any JS value: null, a function, or an object with acceptNode.
 *
 * A traversal object owns a trav record (bo->ptr and bo->owned). The nodes it refers to (root, reference or current) are
 * kept alive by their wrappers, which sit in the traced array bo->ref = [root, filter, current/reference]. A NodeIterator
 * is also listed in ctx->iters (weak) so that the document's pre-remove hook can run the "removing steps"; the release
 * hook unlinks it when the wrapper dies, and embi_bind_free clears the back pointers (trav->ctx).
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/traversal.h"

typedef struct embi_trav {
    embi_bind_ctx *ctx; /* nullptr after embi_bind_free */
    embi_bind_obj *bo;
    embi_node *root;
    embi_node *cur; /* current node / reference node */
    uint32_t what;
    bool before; /* NodeIterator: pointer before reference */
    embi_node *fn; /* NodeIterator: the candidate being filtered (the reference stays the last accepted node until then) */
    bool fbefore;
    bool factive;
    bool active;
    bool iter;
} embi_trav;

enum { SLOT_ROOT = 0, SLOT_FILTER = 1, SLOT_CUR = 2, SLOT_FLIGHT = 3 };
enum { FILTER_ACCEPT = 1, FILTER_REJECT = 2, FILTER_SKIP = 3 };

static void trav_release(void *owned)
{
    embi_trav *t = owned;
    embi_bind_ctx *c = t->ctx;
    if (!c || !t->iter) {
        /* a TreeWalker is not in the context's list, and its ctx is not cleared when the context goes first */
        return;
    }
    for (uint32_t i = 0; i < c->niters; i++) {
        if (c->iters[i] == t) {
            c->iters[i] = c->iters[--c->niters];
            break;
        }
    }
    t->ctx = nullptr;
}

/* Forgets the NodeIterators whose root belongs to a script-made document that the collector found unreachable (its
 * xseen is UINT32_MAX): the document is freed before their wrappers are finalized. */
void embi_impl_trav_drop_dying(embi_bind_ctx *c)
{
    for (uint32_t i = 0; i < c->niters;) {
        embi_trav *t = c->iters[i];
        if (t->root && t->root->doc->script_made && t->root->doc->xseen == UINT32_MAX) {
            c->iters[i] = c->iters[--c->niters];
            t->ctx = nullptr;
            continue;
        }
        i++;
    }
}

void embi_impl_trav_detach_all(embi_bind_ctx *c)
{
    for (uint32_t i = 0; i < c->niters; i++) {
        c->iters[i]->ctx = nullptr;
    }
    embi_free(c->iters, c->cap_iters * sizeof *c->iters);
    c->iters = nullptr;
    c->niters = c->cap_iters = 0;
}

static embi_trav *trav_of(embi_bind_obj *self)
{
    embi_trav *t = embi_bind_stale(self) ? nullptr : self->ptr;
    return (t && t->ctx) ? t : nullptr;
}

static void set_cur(embi_bind_ctx *c, embi_trav *t, embi_node *n)
{
    t->cur = n;
    embi_bind_array_set(c, t->bo->ref, SLOT_CUR, embi_bind_node_value(c, n));
}

static embi_js_value trav_new(embi_bind_ctx *c, bool iter, embi_node *root, uint32_t what, embi_js_value filter)
{
    if (!embi_js_is_undefined(filter) && !embi_js_is_null(filter) && !embi_js_is_object(filter)) {
        return embi_js_throw_type_error(c->rt, "The provided callback is not an object.");
    }
    if (embi_js_is_undefined(filter)) {
        filter = EMBI_JS_NULL;
    }
    embi_trav *t = embi_calloc(1, sizeof *t);
    t->ctx = c;
    t->root = root;
    t->cur = root;
    t->what = what;
    t->before = true;
    t->iter = iter;
    embi_js_value v = embi_bind_other_value(c, iter ? EMBI_IF_NodeIterator : EMBI_IF_TreeWalker, t, t, sizeof *t);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    bo->release = trav_release;
    t->bo = bo;
    (void)embi_bind_pin(c, v);
    embi_js_value arr = embi_bind_array_new(c, 4);
    embi_bind_set_ref(bo, arr);
    embi_bind_array_set(c, arr, SLOT_ROOT, embi_bind_node_value(c, root));
    embi_bind_array_set(c, arr, SLOT_FILTER, filter);
    embi_bind_array_set(c, arr, SLOT_CUR, embi_bind_node_value(c, root));
    if (iter) {
        if (c->niters == c->cap_iters) {
            uint32_t cap = c->cap_iters ? c->cap_iters * 2 : 8;
            c->iters = embi_realloc(c->iters, c->cap_iters * sizeof *c->iters, cap * sizeof *c->iters);
            c->cap_iters = cap;
        }
        c->iters[c->niters++] = t;
    }
    return v;
}

embi_js_value embi_impl_Document_createNodeIterator(embi_bind_ctx *c, embi_node *self, embi_node *a0, uint32_t a1,
                                                    embi_js_value a2)
{
    (void)self;
    return trav_new(c, true, a0, a1, a2);
}

embi_js_value embi_impl_Document_createTreeWalker(embi_bind_ctx *c, embi_node *self, embi_node *a0, uint32_t a1,
                                                  embi_js_value a2)
{
    (void)self;
    return trav_new(c, false, a0, a1, a2);
}

/* ------------------------------------------------------------------ the filter */

/* Runs "filter node": the whatToShow test, then the user filter. Returns false with the exception pending. */
static embi_dom_err run_filter(embi_bind_ctx *c, embi_trav *t, embi_node *n, int *res)
{
    if (t->active) {
        return embi_impl_throw(c, "InvalidStateError", "The filter is already running.");
    }
    uint32_t bit = n->type >= 1 && n->type <= 32 ? 1u << (n->type - 1u) : 0;
    if (!(t->what & bit)) {
        *res = FILTER_SKIP;
        return EMBI_DOM_OK;
    }
    embi_js_value f = embi_bind_array_get(c, t->bo->ref, SLOT_FILTER);
    if (embi_js_is_null(f) || embi_js_is_undefined(f)) {
        *res = FILTER_ACCEPT;
        return EMBI_DOM_OK;
    }
    embi_js_value fn = f;
    embi_js_value this_v = EMBI_JS_UNDEFINED;
    if (!embi_js_is_callable(f)) {
        fn = embi_js_obj_get(c->rt, embi_js_as_object(f), embi_js_key_c(c->rt, "acceptNode"), f);
        if (fn == EMBI_JS_EXCEPTION) {
            return EMBI_BIND_THREW;
        }
        if (!embi_js_is_callable(fn)) {
            (void)embi_js_throw_type_error(c->rt, "The provided callback has no acceptNode method.");
            return EMBI_BIND_THREW;
        }
        this_v = f;
    }
    embi_js_value arg = embi_bind_node_value(c, n);
    (void)embi_bind_pin(c, arg);
    t->active = true;
    embi_js_value r = embi_js_call(c->rt, fn, this_v, 1, &arg);
    t->active = false;
    if (r == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    uint32_t u = 0;
    if (!embi_js_to_uint32(c->rt, r, &u)) {
        return EMBI_BIND_THREW;
    }
    *res = (int)(u & 0xFFFFu);
    return EMBI_DOM_OK;
}

#define FILTER(n, res)                                                                                                 \
    do {                                                                                                               \
        embi_dom_err fe_ = run_filter(c, t, (n), &(res));                                                              \
        if (fe_ != EMBI_DOM_OK) {                                                                                      \
            return fe_;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------------------------ attributes */

embi_node *embi_impl_TreeWalker_get_root(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_trav *t = trav_of(self);
    return t ? t->root : nullptr;
}

uint32_t embi_impl_TreeWalker_get_whatToShow(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_trav *t = trav_of(self);
    return t ? t->what : 0;
}

embi_js_value embi_impl_TreeWalker_get_filter(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_trav *t = trav_of(self);
    return t ? embi_bind_array_get(c, self->ref, SLOT_FILTER) : EMBI_JS_NULL;
}

embi_node *embi_impl_TreeWalker_get_currentNode(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_trav *t = trav_of(self);
    return t ? t->cur : nullptr;
}

void embi_impl_TreeWalker_set_currentNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0)
{
    embi_trav *t = trav_of(self);
    if (t) {
        set_cur(c, t, a0);
    }
}

embi_node *embi_impl_NodeIterator_get_root(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_TreeWalker_get_root(c, self);
}

embi_node *embi_impl_NodeIterator_get_referenceNode(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_TreeWalker_get_currentNode(c, self);
}

bool embi_impl_NodeIterator_get_pointerBeforeReferenceNode(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_trav *t = trav_of(self);
    return t ? t->before : true;
}

uint32_t embi_impl_NodeIterator_get_whatToShow(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_TreeWalker_get_whatToShow(c, self);
}

embi_js_value embi_impl_NodeIterator_get_filter(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_impl_TreeWalker_get_filter(c, self);
}

void embi_impl_NodeIterator_detach(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self; /* a no-op since the DOM Standard dropped the live link */
}

/* ------------------------------------------------------------------ TreeWalker */

embi_dom_err embi_impl_TreeWalker_parentNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    embi_node *n = t->cur;
    while (n && n != t->root) {
        n = n->parent;
        if (n) {
            int r;
            FILTER(n, r);
            if (r == FILTER_ACCEPT) {
                set_cur(c, t, n);
                *out = n;
                return EMBI_DOM_OK;
            }
        }
    }
    return EMBI_DOM_OK;
}

static embi_dom_err traverse_children(embi_bind_ctx *c, embi_bind_obj *self, bool first, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    embi_node *n = first ? t->cur->first_child : t->cur->last_child;
    while (n) {
        int r;
        FILTER(n, r);
        if (r == FILTER_ACCEPT) {
            set_cur(c, t, n);
            *out = n;
            return EMBI_DOM_OK;
        }
        if (r == FILTER_SKIP) {
            embi_node *child = first ? n->first_child : n->last_child;
            if (child) {
                n = child;
                continue;
            }
        }
        while (n) {
            embi_node *sib = first ? n->next : n->prev;
            if (sib) {
                n = sib;
                break;
            }
            embi_node *p = n->parent;
            if (!p || p == t->root || p == t->cur) {
                return EMBI_DOM_OK;
            }
            n = p;
        }
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_TreeWalker_firstChild(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return traverse_children(c, self, true, out);
}

embi_dom_err embi_impl_TreeWalker_lastChild(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return traverse_children(c, self, false, out);
}

static embi_dom_err traverse_siblings(embi_bind_ctx *c, embi_bind_obj *self, bool next, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    embi_node *n = t->cur;
    if (n == t->root) {
        return EMBI_DOM_OK;
    }
    for (;;) {
        embi_node *sib = next ? n->next : n->prev;
        while (sib) {
            n = sib;
            int r;
            FILTER(n, r);
            if (r == FILTER_ACCEPT) {
                set_cur(c, t, n);
                *out = n;
                return EMBI_DOM_OK;
            }
            sib = next ? n->first_child : n->last_child;
            if (r == FILTER_REJECT || !sib) {
                sib = next ? n->next : n->prev;
            }
        }
        n = n->parent;
        if (!n || n == t->root) {
            return EMBI_DOM_OK;
        }
        int r;
        FILTER(n, r);
        if (r == FILTER_ACCEPT) {
            return EMBI_DOM_OK;
        }
    }
}

embi_dom_err embi_impl_TreeWalker_previousSibling(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return traverse_siblings(c, self, false, out);
}

embi_dom_err embi_impl_TreeWalker_nextSibling(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return traverse_siblings(c, self, true, out);
}

embi_dom_err embi_impl_TreeWalker_previousNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    embi_node *n = t->cur;
    while (n != t->root) {
        embi_node *sib = n->prev;
        while (sib) {
            n = sib;
            int r;
            FILTER(n, r);
            while (r != FILTER_REJECT && n->last_child) {
                n = n->last_child;
                FILTER(n, r);
            }
            if (r == FILTER_ACCEPT) {
                set_cur(c, t, n);
                *out = n;
                return EMBI_DOM_OK;
            }
            sib = n->prev;
        }
        if (n == t->root || !n->parent) {
            return EMBI_DOM_OK;
        }
        n = n->parent;
        int r;
        FILTER(n, r);
        if (r == FILTER_ACCEPT) {
            set_cur(c, t, n);
            *out = n;
            return EMBI_DOM_OK;
        }
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_TreeWalker_nextNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    embi_node *n = t->cur;
    int r = FILTER_ACCEPT;
    for (;;) {
        while (r != FILTER_REJECT && n->first_child) {
            n = n->first_child;
            FILTER(n, r);
            if (r == FILTER_ACCEPT) {
                set_cur(c, t, n);
                *out = n;
                return EMBI_DOM_OK;
            }
        }
        embi_node *sib = nullptr;
        embi_node *tmp = n;
        while (tmp) {
            if (tmp == t->root) {
                return EMBI_DOM_OK;
            }
            sib = tmp->next;
            if (sib) {
                n = sib;
                break;
            }
            tmp = tmp->parent;
        }
        if (!sib) {
            return EMBI_DOM_OK;
        }
        FILTER(n, r);
        if (r == FILTER_ACCEPT) {
            set_cur(c, t, n);
            *out = n;
            return EMBI_DOM_OK;
        }
    }
}

/* ------------------------------------------------------------------ NodeIterator */

static embi_dom_err iter_traverse(embi_bind_ctx *c, embi_bind_obj *self, bool next, embi_node **out)
{
    embi_trav *t = trav_of(self);
    *out = nullptr;
    if (!t) {
        return EMBI_DOM_OK;
    }
    if (t->active) {
        return embi_impl_throw(c, "InvalidStateError", "The filter is already running.");
    }
    embi_node *n = t->cur;
    bool before = t->before;
    embi_node *cand = nullptr;
    for (;;) {
        if (next) {
            if (!before) {
                n = embi_next_in_tree(t->root, n);
                if (!n) {
                    return EMBI_DOM_OK;
                }
            } else {
                before = false;
            }
        } else {
            if (before) {
                n = embi_prev_in_tree(t->root, n);
                if (!n) {
                    return EMBI_DOM_OK;
                }
            } else {
                before = true;
            }
        }
        /* The candidate is the iterator's position in flight: the pre-removing steps retarget it when the filter
         * removes it (or an ancestor), while referenceNode stays the last accepted node until the candidate is. */
        t->fn = n;
        t->fbefore = before;
        t->factive = true;
        embi_bind_array_set(c, t->bo->ref, SLOT_FLIGHT, embi_bind_node_value(c, n));
        cand = n;
        int r = 0;
        embi_dom_err fe = run_filter(c, t, n, &r);
        t->factive = false;
        if (fe != EMBI_DOM_OK) {
            return fe;
        }
        n = t->fn;
        before = t->fbefore;
        if (r == FILTER_ACCEPT) {
            t->before = before;
            set_cur(c, t, n);
            break;
        }
    }
    *out = cand;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_NodeIterator_nextNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return iter_traverse(c, self, true, out);
}

embi_dom_err embi_impl_NodeIterator_previousNode(embi_bind_ctx *c, embi_bind_obj *self, embi_node **out)
{
    return iter_traverse(c, self, false, out);
}

/* The NodeIterator pre-removing steps for one position (a node and its pointer flag) inside the removed node. */
static void retarget(embi_bind_ctx *c, embi_trav *t, embi_node *node, embi_node **pos, bool *before, int slot)
{
    embi_node *ref = nullptr;
    if (*before) {
        embi_node *next = embi_next_skipping_children(t->root, node);
        if (next) {
            ref = next;
        } else {
            *before = false;
        }
    }
    if (!ref) {
        if (node->prev) {
            ref = node->prev;
            while (ref->last_child) {
                ref = ref->last_child;
            }
        } else {
            ref = node->parent;
        }
    }
    if (ref) {
        *pos = ref;
        embi_bind_array_set(c, t->bo->ref, slot, embi_bind_node_value(c, ref));
    }
}

/* The removing steps (DOM Standard "NodeIterator pre-removing steps"): called before node leaves its parent. */
void embi_impl_trav_pre_remove(void *ctx, embi_node *node)
{
    embi_bind_ctx *c = ctx;
    for (uint32_t i = 0; i < c->niters; i++) {
        embi_trav *t = c->iters[i];
        if (node == t->root) {
            continue;
        }
        if (embi_contains(node, t->cur)) {
            retarget(c, t, node, &t->cur, &t->before, SLOT_CUR);
        }
        if (t->factive && embi_contains(node, t->fn)) {
            retarget(c, t, node, &t->fn, &t->fbefore, SLOT_FLIGHT);
        }
    }
}
