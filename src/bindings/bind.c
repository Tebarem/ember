/*
 * Binding runtime (W5a): contexts, wrappers and their lifetime, scopes and the embedder hooks.
 * Conversions and value helpers are in bind_conv.c. See bind.h for the contracts.
 */
#include "bindings/bind.h"
#include "bindings/impl_input.h"
#include "bindings/observers.h"
#include "bindings/gen/types.h"
#include "js/heap.h"
#include "bindings/impl.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "layout/layout.h"
#include "style/style.h"

#include <SDL3/SDL_timer.h>
#include <stdint.h>

/* ------------------------------------------------------------------ the context of a realm */

embi_bind_ctx *embi_bind_ctx_of(embi_js_realm *r)
{
    if (!r || !r->global || r->global->klass != &embi_gen_class_Window) {
        return nullptr;
    }
    embi_bind_obj *bo = r->global->internal;
    return bo ? bo->ctx : nullptr;
}

embi_js_realm *embi_bind_realm(const embi_bind_ctx *c)
{
    return c->realm;
}

embi_document *embi_bind_document(const embi_bind_ctx *c)
{
    return c->doc;
}

embi_js_runtime *embi_bind_runtime(const embi_bind_ctx *c)
{
    return c->rt;
}

void embi_bind_set_host(embi_bind_ctx *c, const embi_bind_host *h)
{
    if (h) {
        c->host = *h;
    } else {
        SDL_memset(&c->host, 0, sizeof c->host);
    }
}

void embi_bind_viewport(embi_bind_ctx *c, double *w, double *h, double *dpr)
{
    double vw = 1024, vh = 768, d = 1;
    if (c->host.viewport) {
        c->host.viewport(c->host.user, &vw, &vh, &d);
    }
    if (w) {
        *w = vw;
    }
    if (h) {
        *h = vh;
    }
    if (dpr) {
        *dpr = d;
    }
}

embi_str embi_bind_language(embi_bind_ctx *c)
{
    if (c->host.language) {
        embi_str s = c->host.language(c->host.user);
        if (s.len) {
            return s;
        }
    }
    return EMBI_STR("en-US");
}

embi_str embi_bind_url(embi_bind_ctx *c)
{
    if (c->host.url) {
        embi_str s = c->host.url(c->host.user);
        if (s.len) {
            return s;
        }
    }
    return EMBI_STR("about:blank");
}

double embi_bind_now_ms(embi_bind_ctx *c)
{
    if (c->host.now_ms) {
        return c->host.now_ms(c->host.user);
    }
    return (double)SDL_GetTicksNS() / 1e6;
}

embi_layout_engine *embi_bind_layout(embi_bind_ctx *c)
{
    if (c->host.sync_sheets != nullptr) {
        c->host.sync_sheets(c->host.user);
    }
    return c->host.layout;
}

embi_style_engine *embi_bind_style(embi_bind_ctx *c)
{
    if (c->host.sync_sheets != nullptr) {
        c->host.sync_sheets(c->host.user);
    }
    return c->host.style;
}

void embi_bind_log(embi_bind_ctx *c, int level, embi_str text)
{
    if (c->host.log) {
        c->host.log(c->host.user, level, text);
    }
    char *z = embi_alloc((size_t)text.len + 1);
    if (text.len) {
        SDL_memcpy(z, text.ptr, text.len);
    }
    z[text.len] = '\0';
    switch (level) {
    case EMBI_BIND_LOG_WARN:
        embi_log_warn("%s", z);
        break;
    case EMBI_BIND_LOG_ERROR:
        embi_log_error("%s", z);
        break;
    case EMBI_BIND_LOG_DEBUG:
        embi_log_debug("%s", z);
        break;
    default:
        embi_log_info("%s", z);
        break;
    }
    embi_free(z, (size_t)text.len + 1);
}

/* ------------------------------------------------------------------ scratch memory */

char *embi_bind_alloc(embi_bind_ctx *c, size_t n)
{
    return embi_arena_alloc(&c->arena, n ? n : 1, 8);
}

embi_js_value embi_bind_pin(embi_bind_ctx *c, embi_js_value v)
{
    if (c->npins == c->cap_pins) {
        uint32_t cap = c->cap_pins ? c->cap_pins * 2 : 8;
        embi_js_value *p = embi_realloc(c->pins, c->cap_pins * sizeof *p, cap * sizeof *p);
        c->pins = p;
        c->cap_pins = cap;
    }
    c->pins[c->npins++] = v;
    return v;
}

embi_bind_scope embi_bind_enter(embi_bind_ctx *c)
{
    embi_bind_scope sc;
    sc.c = c;
    sc.arena = embi_arena_mark(&c->arena);
    sc.pins = c->npins;
    sc.prev_site = c->site;
    sc.early = EMBI_JS_UNDEFINED;
    return sc;
}

embi_js_value embi_bind_leave(embi_bind_scope sc, embi_js_value v)
{
    if (sc.c) {
        embi_arena_reset_to(&sc.c->arena, sc.arena);
        sc.c->npins = sc.pins;
        sc.c->site = sc.prev_site;
    }
    return v;
}

embi_js_value embi_bind_end(const embi_bind_scope *sc, embi_js_value v)
{
    if (sc->c) {
        embi_arena_reset_to(&sc->c->arena, sc->arena);
        sc->c->npins = sc->pins;
        sc->c->site = sc->prev_site;
    }
    return v;
}

embi_js_value embi_bind_num(double v)
{
    return embi_js_number(v);
}

embi_bind_ctx *embi_bind_begin(embi_js_realm *r, embi_js_value this_v, const embi_bind_member *m,
                               unsigned how, embi_bind_scope *sc, embi_bind_obj **bo)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (!c) {
        sc->c = nullptr;
        sc->early = embi_js_throw_type_error(r->rt, "Illegal invocation");
        return nullptr;
    }
    *sc = embi_bind_enter(c);
    c->site = m;
    unsigned self = how & 7u;
    if (self == EMBI_SELF_NONE) {
        return c;
    }
    if ((how & EMBI_SELF_GLOBAL) && embi_js_is_nullish(this_v)) {
        this_v = embi_js_from_object(r->global);
    }
    embi_bind_obj *o = nullptr;
    if (!embi_bind_this(c, this_v, m->iface, &o)) {
        sc->early = embi_bind_end(sc, EMBI_JS_EXCEPTION);
        return nullptr;
    }
    *bo = o;
    if (self == EMBI_SELF_OBJ ? embi_bind_stale(o) : o->ptr == nullptr) {
        sc->early = embi_bind_end(sc, EMBI_JS_UNDEFINED);
        return nullptr;
    }
    return c;
}

embi_js_value embi_bind_ctor_needs_new(embi_js_realm *r, const embi_bind_member *site)
{
    return embi_js_throw_type_error(r->rt,
                                    "Failed to construct '%s': Please use the 'new' operator, this DOM object constructor cannot be called as a function.",
                                    embi_bind_names[site->iface]);
}

embi_js_value embi_bind_illegal_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)new_target;
    return embi_js_throw_type_error(r->rt, "Illegal constructor");
}

void embi_bind_install_members(embi_bind_ctx *c, embi_js_object *proto, embi_js_object *ctor,
                               const embi_bind_member *rows, size_t n)
{
    embi_js_realm *r = c->realm;
    embi_js_runtime *rt = c->rt;
    for (size_t i = 0; i < n; i++) {
        const embi_bind_member *m = &rows[i];
        bool unforge = (m->flags & EMBI_MF_UNFORGEABLE) != 0;
        switch (m->kind) {
        case EMBI_MEMBER_OP:
        case EMBI_MEMBER_SOP: {
            embi_js_object *f = embi_js_native_function_new_data(r, m->name, m->len, m->fn, 0, (void *)m);
            uint8_t attrs = EMBI_JS_ATTR_ENUMERABLE;
            if (!unforge) {
                attrs |= EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE;
            }
            embi_js_define_raw(rt, m->kind == EMBI_MEMBER_SOP ? ctor : proto, embi_js_key_c(rt, m->name),
                               embi_js_from_object(f), attrs);
            break;
        }
        case EMBI_MEMBER_ATTR:
            embi_js_define_getset_data(r, proto, m->name, m->fn, m->set, 0,
                                       unforge ? EMBI_JS_ATTR_ENUMERABLE
                                               : (EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE),
                                       (void *)m);
            break;
        case EMBI_MEMBER_CONST:
            embi_js_define_raw(rt, proto, embi_js_key_c(rt, m->name), embi_js_number(m->num), EMBI_JS_ATTR_ENUMERABLE);
            embi_js_define_raw(rt, ctor, embi_js_key_c(rt, m->name), embi_js_number(m->num), EMBI_JS_ATTR_ENUMERABLE);
            break;
        case EMBI_MEMBER_ITER:
            embi_bind_install_iterable(c, proto, m->data);
            break;
        case EMBI_MEMBER_ARRAY_ITER:
            embi_bind_install_array_iterable(c, proto, (m->flags & EMBI_MF_SYMBOL_ONLY) != 0);
            break;
        default: /* EMBI_MEMBER_CSSPROPS */
            for (int k = 0; k < embi_css_prop_count; k++) {
                embi_bind_define_css_prop(c, proto, embi_css_prop_names[k], embi_css_prop_camel[k], k, m->fn, m->set);
            }
            break;
        }
    }
}

embi_js_object *embi_bind_install_iface(embi_bind_ctx *c, int id)
{
    if (!embi_js_is_empty(c->protos[id])) {
        return embi_js_as_object(c->protos[id]);
    }
    const embi_bind_iface *d = embi_bind_ifaces[id];
    embi_js_realm *r = c->realm;
    embi_js_runtime *rt = c->rt;
    const char *name = embi_bind_names[id];
    uint16_t parent_id = embi_bind_parent[id];
    embi_js_value parent;
    if (parent_id != EMBI_IF_NONE) {
        parent = embi_js_from_object(embi_bind_install_iface(c, parent_id));
    } else if (d->flags & EMBI_IFACE_ERROR) {
        parent = rt->protos[EMBI_JS_PROTO_ERROR];
    } else {
        parent = rt->protos[EMBI_JS_PROTO_OBJECT];
    }
    embi_js_value parent_iface = parent_id != EMBI_IF_NONE ? parent : EMBI_JS_UNDEFINED;
    if (d->flags & EMBI_IFACE_GLOBAL) {
        /* Window.prototype -> WindowProperties -> EventTarget.prototype */
        parent = embi_js_from_object(embi_js_object_new(rt, parent));
    }
    embi_js_object *proto = embi_js_object_new(rt, parent);
    c->protos[id] = embi_js_from_object(proto);
    embi_js_object *ctor_obj =
        embi_js_native_constructor_new(r, name, d->ctor_len, d->ctor ? d->ctor : embi_bind_illegal_ctor, 0);
    embi_js_define_raw(rt, ctor_obj, embi_js_key_c(rt, "prototype"), embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "constructor"), embi_js_from_object(ctor_obj),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_bind_install_members(c, proto, ctor_obj, d->members, d->nmembers);
    embi_bind_finish_iface(c, proto, ctor_obj, name, parent_iface);
    if (!embi_bind_testing[id]) {
        embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor_obj),
                           EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    }
    return proto;
}

embi_js_object *embi_bind_install_ns(embi_bind_ctx *c, int k)
{
    embi_js_runtime *rt = c->rt;
    const embi_bind_iface *d = embi_bind_nsdescs[k];
    /* WebIDL namespace object: its [[Prototype]] is an empty object whose [[Prototype]] is %Object.prototype% (as in the
     * console namespace tests of WPT). */
    embi_js_object *ns = embi_js_object_new(rt, embi_js_from_object(embi_js_object_new_plain(rt)));
    embi_bind_install_members(c, ns, nullptr, d->members, d->nmembers);
    return ns;
}

/* ------------------------------------------------------------------ wrappers */

static embi_bind_obj *bo_new(embi_bind_ctx *c, uint16_t iface, void *ptr)
{
    embi_bind_obj *bo = embi_calloc(1, sizeof *bo);
    bo->ctx = c;
    bo->iface = iface;
    bo->ptr = ptr;
    bo->widx = UINT32_MAX;
    return bo;
}

static embi_js_value wrap_obj(embi_bind_ctx *c, embi_bind_obj *bo)
{
    embi_js_object *o =
        embi_js_object_new_class(c->rt, c->protos[bo->iface], embi_bind_classes[bo->iface]);
    o->internal = bo;
    bo->obj = o;
    EMBI_JS_WB_STRUCT(o);
    if (embi_bind_ifaces[bo->iface] && embi_bind_ifaces[bo->iface]->nunforge) {
        embi_bind_install_members(c, o, nullptr, embi_bind_ifaces[bo->iface]->unforge,
                                  embi_bind_ifaces[bo->iface]->nunforge);
    }
    return embi_js_from_object(o);
}

embi_js_value embi_bind_obj_value(const embi_bind_obj *bo)
{
    return embi_js_from_object(bo->obj);
}

/* The most derived interface of a node. */
static uint16_t iface_of_node(const embi_node *n)
{
    switch (n->type) {
    case EMBI_NODE_ELEMENT:
        return embi_bind_iface_for_tag((uint8_t)n->extra, n->name);
    case EMBI_NODE_ATTR:
        return EMBI_IF_Attr;
    case EMBI_NODE_TEXT:
        return (n->flags & EMBI_NF_CDATA) ? EMBI_IF_CDATASection : EMBI_IF_Text;
    case EMBI_NODE_COMMENT:
        return EMBI_IF_Comment;
    case EMBI_NODE_PI:
        return EMBI_IF_ProcessingInstruction;
    case EMBI_NODE_DOCUMENT:
        return ((const embi_document *)n)->xml_class ? EMBI_IF_XMLDocument : EMBI_IF_Document;
    case EMBI_NODE_DOCTYPE:
        return EMBI_IF_DocumentType;
    case EMBI_NODE_FRAGMENT:
        return EMBI_IF_DocumentFragment;
    default:
        return EMBI_IF_Node;
    }
}

static void list_wrapped(embi_bind_ctx *c, embi_bind_obj *bo)
{
    if (c->nwrapped == c->cap_wrapped) {
        uint32_t cap = c->cap_wrapped ? c->cap_wrapped * 2 : 64;
        embi_bind_obj **p = embi_realloc(c->wrapped, c->cap_wrapped * sizeof *p, cap * sizeof *p);
        c->wrapped = p;
        c->cap_wrapped = cap;
    }
    bo->widx = c->nwrapped;
    c->wrapped[c->nwrapped++] = bo;
}

static void unlist_wrapped(embi_bind_ctx *c, embi_bind_obj *bo)
{
    if (bo->widx == UINT32_MAX) {
        return;
    }
    embi_bind_obj *last = c->wrapped[--c->nwrapped];
    c->wrapped[bo->widx] = last;
    last->widx = bo->widx;
    bo->widx = UINT32_MAX;
}

embi_js_value embi_bind_node_value(embi_bind_ctx *c, embi_node *n)
{
    if (!n) {
        return EMBI_JS_NULL;
    }
    if (n->wrapper) {
        return embi_js_from_object(((embi_bind_obj *)n->wrapper)->obj);
    }
    embi_bind_obj *bo = bo_new(c, iface_of_node(n), n);
    bo->is_node = true;
    embi_js_value v = wrap_obj(c, bo);
    n->wrapper = bo;
    list_wrapped(c, bo);
    return v;
}

embi_js_value embi_bind_other_value(embi_bind_ctx *c, uint16_t iface, void *ptr, void *owned,
                                    uint32_t owned_size)
{
    embi_bind_obj *bo = bo_new(c, iface, ptr);
    bo->owned = owned;
    bo->owned_size = owned_size;
    return wrap_obj(c, bo);
}

embi_js_value embi_bind_view_new(embi_bind_ctx *c, uint16_t iface, embi_bind_obj *owner,
                                 uint32_t aux, void *owned, uint32_t owned_size)
{
    embi_bind_obj *bo = bo_new(c, iface, owner);
    bo->owner = owner;
    bo->aux = aux;
    bo->owned = owned;
    bo->owned_size = owned_size;
    return wrap_obj(c, bo);
}

embi_bind_obj *embi_bind_obj_of(embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        return nullptr;
    }
    embi_js_object *o = embi_js_as_object(v);
    if (o->klass->trace != embi_bind_trace) {
        return nullptr;
    }
    return o->internal;
}

/* True when interface want is have or one of its ancestors. */
bool embi_bind_iface_is(uint16_t have, int want)
{
    for (uint16_t i = have; i != EMBI_IF_NONE && i < EMBI_IF_COUNT; i = embi_bind_parent[i]) {
        if (i == want) {
            return true;
        }
    }
    return false;
}

bool embi_bind_this(embi_bind_ctx *c, embi_js_value this_v, int iface, embi_bind_obj **out)
{
    embi_bind_obj *bo = embi_bind_obj_of(this_v);
    if (!bo || !embi_bind_iface_is(bo->iface, iface)) {
        embi_js_throw_type_error(c->rt, "Illegal invocation");
        return false;
    }
    *out = bo;
    return true;
}

embi_js_value *embi_bind_same(embi_bind_obj *bo, uint32_t slot)
{
    if (slot >= bo->nsame) {
        uint32_t n = slot + 1;
        embi_js_value *p = embi_calloc(n, sizeof *p);
        for (uint32_t i = 0; i < bo->nsame; i++) {
            p[i] = bo->same[i];
        }
        if (bo->same) {
            embi_free(bo->same, bo->nsame * sizeof *p);
        }
        bo->same = p;
        bo->nsame = n;
    }
    return &bo->same[slot];
}

void embi_bind_set_ref(embi_bind_obj *bo, embi_js_value v)
{
    bo->ref = v;
    EMBI_JS_WB_STRUCT(bo->obj);
}

void embi_bind_set_listeners(embi_bind_obj *bo, embi_js_value v)
{
    bo->listeners = v;
    EMBI_JS_WB_STRUCT(bo->obj);
}

/* ------------------------------------------------------------------ GC: roots, trace, finalize */

/* The node that decides how long a tree lives: the root of n's tree, following a template
 * contents fragment to its template element (the template owns its contents). */
static embi_node *life_root(embi_node *n)
{
    for (;;) {
        embi_node *r = n;
        while (r->parent) {
            r = r->parent;
        }
        if (r->type == EMBI_NODE_FRAGMENT && ((embi_fragment *)r)->host) {
            n = ((embi_fragment *)r)->host;
            continue;
        }
        return r;
    }
}

static bool node_rooted(const embi_node *n)
{
    return (n->flags & EMBI_NF_CONNECTED) || n->type == EMBI_NODE_DOCUMENT;
}

/* True for a node whose lifetime is not tied to the page document: a connected node of the page document lives as long
 * as the document, but the nodes of a script-made document live only while the document is reachable. */
static bool gc_rooted(const embi_node *n)
{
    return node_rooted(n) && !n->doc->script_made;
}

/* The script-made document that holds the memory of n (the node document, or for template contents the document of the
 * template), or nullptr. */
static embi_document *owner_xdoc(embi_node *n)
{
    embi_document *d = n->doc;
    if (d->script_made) {
        return d;
    }
    if (d->inert) {
        embi_document *r = life_root(n)->doc;
        if (r && r->script_made) {
            return r;
        }
    }
    return nullptr;
}

typedef struct group_ent {
    embi_node *root;
    embi_bind_obj *bo;
} group_ent;

static int cmp_group(const void *a, const void *b)
{
    const group_ent *x = a;
    const group_ent *y = b;
    uintptr_t rx = (uintptr_t)x->root, ry = (uintptr_t)y->root; /* unrelated pointers: compare as integers */
    return (rx > ry) - (rx < ry);
}

static bool obj_marked(const embi_bind_obj *bo)
{
    return embi_js_as_cell(embi_js_from_object(bo->obj))->mark != 0;
}

/* Ephemeron rule of the wrappers of detached trees: once one wrapper of a tree is marked, every
 * wrapper of the tree is. Returns true when it marked something. */
static bool tree_mark(embi_js_tracer *t, void *data)
{
    embi_bind_ctx *c = data;
    uint32_t n = 0;
    group_ent *g = embi_alloc((size_t)c->nwrapped * sizeof *g + 1);
    for (uint32_t i = 0; i < c->nwrapped; i++) {
        embi_bind_obj *bo = c->wrapped[i];
        if (!bo->ptr || gc_rooted(bo->ptr)) {
            continue;
        }
        g[n].root = life_root(bo->ptr);
        g[n].bo = bo;
        n++;
    }
    bool any = false;
    if (n > 1) {
        SDL_qsort(g, n, sizeof *g, cmp_group);
        uint32_t i = 0;
        while (i < n) {
            uint32_t j = i;
            bool marked = false;
            while (j < n && g[j].root == g[i].root) {
                marked = marked || obj_marked(g[j].bo);
                j++;
            }
            if (marked) {
                for (uint32_t k = i; k < j; k++) {
                    if (!obj_marked(g[k].bo)) {
                        embi_js_mark_value(t, embi_js_from_object(g[k].bo->obj));
                        any = true;
                    }
                }
            }
            i = j;
        }
    }
    embi_free(g, (size_t)c->nwrapped * sizeof *g + 1);
    return any;
}

/* Runs after the marking reached its fixed point and before the sweep: a script-made document none of whose node
 * wrappers got marked is unreachable from script. Its wrappers stop referring to its nodes (they are finalized by the
 * sweep) and the document is freed. A document that has a MutationObserver registration stays until the context goes
 * (the observer records hold plain node pointers). */
static void tree_clear(void *data)
{
    embi_bind_ctx *c = data;
    if (c->nxdocs == 0) {
        return;
    }
    for (uint32_t i = 0; i < c->nwrapped; i++) {
        embi_bind_obj *bo = c->wrapped[i];
        embi_document *x = bo->ptr ? owner_xdoc(bo->ptr) : nullptr;
        if (x) {
            x->xfresh = false;
            if (obj_marked(bo)) {
                x->xseen = c->epoch;
            }
        }
    }
    bool any = false;
    for (uint32_t i = 0; i < c->nxdocs; i++) {
        embi_document *x = c->xdocs[i];
        if (x->xfresh || x->xseen == c->epoch || x->mut != nullptr) {
            continue;
        }
        any = true;
        x->xseen = UINT32_MAX; /* dying */
    }
    if (!any) {
        return;
    }
    for (uint32_t i = 0; i < c->nwrapped; i++) {
        embi_bind_obj *bo = c->wrapped[i];
        embi_document *x = bo->ptr ? owner_xdoc(bo->ptr) : nullptr;
        if (x && x->xseen == UINT32_MAX) {
            if (((embi_node *)bo->ptr)->wrapper == bo) {
                ((embi_node *)bo->ptr)->wrapper = nullptr;
            }
            bo->ptr = nullptr;
        }
    }
    embi_impl_trav_drop_dying(c);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < c->nxdocs; i++) {
        embi_document *x = c->xdocs[i];
        if (x->xseen == UINT32_MAX) {
            embi_document_unobserve(x, c);
            embi_document_free(x);
        } else {
            c->xdocs[keep++] = x;
        }
    }
    c->nxdocs = keep;
}

static void bind_root(embi_js_tracer *t, void *user)
{
    embi_bind_ctx *c = user;
    c->epoch++;
    for (int i = 0; i < EMBI_IF_COUNT; i++) {
        embi_js_mark_value(t, c->protos[i]);
    }
    for (uint32_t i = 0; i < c->npins; i++) {
        embi_js_mark_value(t, c->pins[i]);
    }
    embi_js_mark_value(t, c->iter_proto);
    embi_js_mark_value(t, c->mqls);
    embi_js_mark_value(t, c->win_name);
    for (uint32_t i = 0; i < c->nhist; i++) {
        embi_js_mark_value(t, c->hist[i].state);
    }
    /* Wrappers of connected nodes live as long as the document. */
    for (uint32_t i = 0; i < c->nwrapped; i++) {
        embi_bind_obj *bo = c->wrapped[i];
        if (bo->ptr && gc_rooted(bo->ptr)) {
            embi_js_mark_value(t, embi_js_from_object(bo->obj));
        }
    }
    if (c->nwrapped || c->nxdocs) {
        embi_js_trace_ephemeron(t, c, tree_mark, tree_clear);
    }
}

void embi_bind_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_bind_obj *bo = o->internal;
    if (!bo) {
        return;
    }
    for (uint32_t i = 0; i < bo->nsame; i++) {
        embi_js_mark_value(t, bo->same[i]);
    }
    embi_js_mark_value(t, bo->ref);
    embi_js_mark_value(t, bo->listeners);
    if (bo->owner && bo->owner->obj) {
        embi_js_mark_value(t, embi_js_from_object(bo->owner->obj));
    }
}

/* Detaches the records of every wrapped node of the tree under n (template contents included)
 * from their nodes, ahead of freeing the tree. */
static void release_tree_wrappers(embi_node *n)
{
    for (embi_node *x = n; x; x = embi_next_in_tree(n, x)) {
        if (x->wrapper) {
            embi_bind_obj *w = x->wrapper;
            w->ptr = nullptr;
            x->wrapper = nullptr;
        }
        if (x->type == EMBI_NODE_ELEMENT) {
            embi_element *el = (embi_element *)x;
            if (el->attrs && el->attrs->content) {
                release_tree_wrappers(el->attrs->content);
            }
        }
    }
}

void embi_bind_finalize(embi_js_object *o)
{
    embi_bind_obj *bo = o->internal;
    if (!bo) {
        return;
    }
    o->internal = nullptr;
    embi_bind_ctx *c = bo->ctx;
    if (bo->is_node && c) {
        unlist_wrapped(c, bo);
        if (bo->ptr) {
            embi_node *n = bo->ptr;
            bo->ptr = nullptr;
            if (n->wrapper == bo) {
                n->wrapper = nullptr;
            }
            embi_node *root = life_root(n);
            /* A detached tree whose last reachable wrapper died is owned by JS: free it. The
             * other wrappers of the tree died in the same collection (the ephemeron rule) and
             * stop referring to it first. */
            if (!node_rooted(root) && root->parent == nullptr &&
                root->type != EMBI_NODE_DOCUMENT) {
                release_tree_wrappers(root);
                embi_destroy_detached(root);
            }
        }
    }
    if (bo->release != nullptr && bo->owned != nullptr) {
        bo->release(bo->owned);
    }
    if (bo->owned) {
        embi_free(bo->owned, bo->owned_size);
    }
    if (bo->same) {
        embi_free(bo->same, bo->nsame * sizeof *bo->same);
    }
    embi_free(bo, sizeof *bo);
}

/* ------------------------------------------------------------------ DOM mutation epoch */

static void pre_remove(void *ctx, embi_node *node)
{
    embi_impl_input_pre_remove(ctx, node); /* first: its listeners may still change the tree */
    embi_impl_trav_pre_remove(ctx, node);
}

static void obs_children(void *ctx, embi_node *parent, embi_node *fa, embi_node *la,
                         embi_node *fr, embi_node *lr)
{
    (void)parent;
    (void)fa;
    (void)la;
    (void)lr;
    embi_bind_ctx *c = ctx;
    c->mut_epoch++;
    if (c->active && fr && !(c->active->flags & EMBI_NF_CONNECTED)) {
        c->active = nullptr;
    }
}

static void obs_attr(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local,
                     embi_dstr old_value, bool had_old)
{
    embi_bind_ctx *c = ctx;
    c->mut_epoch++;
    embi_impl_attr_changed(c, el, ns, ns_uri, local, old_value, had_old);
    embi_impl_canvas_attr_changed(c, el, local);
}

static void obs_chardata(void *ctx, embi_node *node)
{
    (void)node;
    ((embi_bind_ctx *)ctx)->mut_epoch++;
}

/* ------------------------------------------------------------------ contexts */

/* X.prototype[@@unscopables] for the interfaces with [Unscopable] members (DOM: the ChildNode and ParentNode methods are
 * not found by a with statement, nor by an event handler's scope chain). Element has all of them; Document and
 * DocumentFragment only the ParentNode ones. */
static void install_unscopables_for(embi_bind_ctx *c, uint16_t iface, const char *const *names, size_t n)
{
    embi_js_runtime *rt = c->rt;
    if (!embi_js_is_object(c->protos[iface])) {
        return;
    }
    embi_js_object *u = embi_js_object_new(rt, EMBI_JS_NULL);
    const uint8_t wec = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE;
    for (size_t i = 0; i < n; i++) {
        embi_js_define_raw(rt, u, embi_js_key_c(rt, names[i]), embi_js_from_bool(true), wec);
    }
    embi_js_define_raw(rt, embi_js_as_object(c->protos[iface]), embi_js_wk_sym(rt, EMBI_JS_WK_UNSCOPABLES),
                       embi_js_from_object(u), EMBI_JS_ATTR_CONFIGURABLE);
}

static void install_unscopables(embi_bind_ctx *c)
{
    static const char *const element[] = {"after", "append", "before", "prepend", "remove", "replaceChildren",
                                          "replaceWith"};
    static const char *const parent[] = {"append", "prepend", "replaceChildren"};
    install_unscopables_for(c, EMBI_IF_Element, element, sizeof element / sizeof element[0]);
    install_unscopables_for(c, EMBI_IF_Document, parent, sizeof parent / sizeof parent[0]);
    install_unscopables_for(c, EMBI_IF_DocumentFragment, parent, sizeof parent / sizeof parent[0]);
}

static void install_global_names(embi_bind_ctx *c)
{
    embi_js_realm *r = c->realm;
    embi_js_runtime *rt = c->rt;
    for (int k = 0; k < EMBI_NS_COUNT; k++) {
        embi_js_object *ns = embi_bind_install_ns(c, k);
        embi_bind_finish_ns(c, ns, embi_bind_ns_names[k]);
        embi_js_define_raw(rt, r->global, embi_js_key_c(rt, embi_bind_ns_names[k]),
                           embi_js_from_object(ns),
                           EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    }
}

embi_bind_ctx *embi_bind_new_detached(embi_js_runtime *rt)
{
    embi_bind_ctx *c = embi_calloc(1, sizeof *c);
    c->rt = rt;
    c->protos = embi_calloc(EMBI_IF_COUNT, sizeof *c->protos);
    embi_arena_init(&c->arena, 4096);
    embi_js_root_add(rt, bind_root, c);

    embi_bind_obj *gbo = bo_new(c, EMBI_IF_Window, c);
    c->gbo = gbo;
    c->realm = embi_js_realm_new_ex(rt, &embi_gen_class_Window, gbo);
    if (!c->realm) {
        embi_js_root_remove(rt, bind_root, c);
        embi_arena_free(&c->arena);
        embi_free(c->protos, EMBI_IF_COUNT * sizeof *c->protos);
        embi_free(gbo, sizeof *gbo);
        embi_free(c, sizeof *c);
        return nullptr;
    }
    gbo->obj = c->realm->global;
    for (int i = 0; i < EMBI_IF_COUNT; i++) {
        if (embi_bind_ifaces[i]) {
            (void)embi_bind_install_iface(c, i);
        }
    }
    embi_impl_window_install_named(c);
    embi_js_realm_set_global_proto(c->realm, embi_js_as_object(c->protos[EMBI_IF_Window]));
    embi_impl_html_install_image(c);
    if (embi_bind_ifaces[EMBI_IF_Window] && embi_bind_ifaces[EMBI_IF_Window]->nunforge) {
        embi_bind_install_members(c, c->realm->global, nullptr, embi_bind_ifaces[EMBI_IF_Window]->unforge,
                                  embi_bind_ifaces[EMBI_IF_Window]->nunforge);
    }
    install_global_names(c);
    install_unscopables(c);
    return c;
}

void embi_bind_attach_doc(embi_bind_ctx *c, embi_document *doc)
{
    c->doc = doc;
    embi_bind_observe_doc(c, doc);
}

embi_bind_ctx *embi_bind_new(embi_js_runtime *rt, embi_document *doc)
{
    embi_bind_ctx *c = embi_bind_new_detached(rt);
    if (c != nullptr) {
        embi_bind_attach_doc(c, doc);
    }
    return c;
}

void embi_bind_observe_doc(embi_bind_ctx *c, embi_document *doc)
{
    embi_observer obs = {.ctx = c,
                         .children_changed = obs_children,
                         .attribute_changed = obs_attr,
                         .character_data_changed = obs_chardata,
                         .pre_remove = pre_remove};
    embi_document_observe(doc, &obs);
}

void embi_bind_add_xdoc(embi_bind_ctx *c, embi_document *doc)
{
    if (c->nxdocs == c->cap_xdocs) {
        uint32_t cap = c->cap_xdocs ? c->cap_xdocs * 2 : 8;
        c->xdocs = embi_realloc(c->xdocs, c->cap_xdocs * sizeof *c->xdocs, cap * sizeof *c->xdocs);
        c->cap_xdocs = cap;
    }
    c->xdocs[c->nxdocs++] = doc;
    doc->script_made = true;
    doc->xfresh = true;
    embi_bind_observe_doc(c, doc);
}

void embi_bind_expose_testing(embi_bind_ctx *c)
{
    if (c->testing) {
        return;
    }
    c->testing = true;
    embi_js_runtime *rt = c->rt;
    for (int i = 0; i < EMBI_IF_COUNT; i++) {
        if (embi_bind_testing[i] && embi_bind_ifaces[i]) {
            embi_js_value ctor = embi_js_obj_get(rt, embi_js_as_object(c->protos[i]),
                                                 embi_js_key_c(rt, "constructor"), c->protos[i]);
            embi_js_define_raw(rt, c->realm->global, embi_js_key_c(rt, embi_bind_names[i]), ctor,
                               EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
        }
    }
}

void embi_bind_free(embi_bind_ctx *c)
{
    /* The loader state of this context goes first: it is keyed by the context's address, which a later context can reuse. */
    embi_loader_detach(c);
    embi_impl_frames_release(c); /* IF1: the remote window proxies */
    if (c->doc != nullptr) { /* the canvas and observer records hang off the document; a context never attached has none */
        embi_impl_canvas_release(c);
        embi_observers_release(c);
        embi_document_unobserve(c->doc, c);
    }
    /* Every node wrapper becomes stale: it stops referring to its node and to this context. The
     * detached trees they own are freed with the document. */
    for (uint32_t i = 0; i < c->nwrapped; i++) {
        embi_bind_obj *bo = c->wrapped[i];
        if (bo->ptr) {
            embi_node *n = bo->ptr;
            if (n->wrapper == bo) {
                n->wrapper = nullptr;
            }
        }
        bo->ptr = nullptr;
        bo->ctx = nullptr;
        bo->widx = UINT32_MAX;
    }
    c->gbo->ptr = nullptr;
    c->gbo->ctx = nullptr;
    if (c->adopted != 0) {
        embi_js_persistent_free(c->rt, c->adopted - 1u);
        c->adopted = 0;
    }
    if (c->css_sheet_fn != 0) {
        embi_js_persistent_free(c->rt, c->css_sheet_fn - 1u);
        c->css_sheet_fn = 0;
    }
    if (c->anim_exports != 0) {
        embi_js_persistent_free(c->rt, c->anim_exports - 1u);
        c->anim_exports = 0;
    }
    embi_js_root_remove(c->rt, bind_root, c);
    embi_js_realm_free(c->realm);
    c->realm = nullptr;
    embi_impl_trav_detach_all(c);
    for (uint32_t i = 0; i < c->nxdocs; i++) {
        embi_document_unobserve(c->xdocs[i], c);
        embi_document_free(c->xdocs[i]);
    }
    embi_free(c->xdocs, c->cap_xdocs * sizeof *c->xdocs);
    for (uint32_t i = 0; i < c->ncounters; i++) {
        embi_free(c->counters[i].label, c->counters[i].len ? c->counters[i].len : 1);
    }
    embi_free(c->counters, c->cap_counters * sizeof *c->counters);
    embi_impl_style_cache_free(c);
    for (uint32_t i = 0; i < c->nhist; i++) {
        embi_free(c->hist[i].url, c->hist[i].url_len + 1u);
        embi_sb_free(&c->hist[i].bytes);
    }
    embi_free(c->hist, c->cap_hist * sizeof *c->hist);
    embi_arena_free(&c->arena);
    embi_free(c->wrapped, c->cap_wrapped * sizeof *c->wrapped);
    embi_free(c->pins, c->cap_pins * sizeof *c->pins);
    embi_free(c->protos, EMBI_IF_COUNT * sizeof *c->protos);
    embi_free(c, sizeof *c);
}

/* ------------------------------------------------------------------ console counters and timers */

/* One entry per label (count and timer share it), in a growable array. An entry is dropped when it
 * holds neither a count nor a running timer. */
static embi_bind_counter *counter_find(embi_bind_ctx *c, embi_str label, bool create)
{
    for (uint32_t i = 0; i < c->ncounters; i++) {
        embi_bind_counter *k = &c->counters[i];
        if (k->len == label.len &&
            (label.len == 0 || SDL_memcmp(k->label, label.ptr, label.len) == 0)) {
            return k;
        }
    }
    if (!create) {
        return nullptr;
    }
    if (c->ncounters == c->cap_counters) {
        uint32_t cap = c->cap_counters ? c->cap_counters * 2 : 8;
        c->counters = embi_realloc(c->counters, c->cap_counters * sizeof *c->counters, cap * sizeof *c->counters);
        c->cap_counters = cap;
    }
    embi_bind_counter *k = &c->counters[c->ncounters++];
    SDL_memset(k, 0, sizeof *k);
    k->label = embi_alloc(label.len ? label.len : 1);
    if (label.len) {
        SDL_memcpy(k->label, label.ptr, label.len);
    }
    k->len = label.len;
    return k;
}

static void counter_prune(embi_bind_ctx *c, embi_bind_counter *k)
{
    if (k->n != 0 || k->timing) {
        return;
    }
    embi_free(k->label, k->len ? k->len : 1);
    uint32_t i = (uint32_t)(k - c->counters);
    c->ncounters--;
    if (i != c->ncounters) {
        c->counters[i] = c->counters[c->ncounters];
    }
}

uint32_t embi_bind_console_count(embi_bind_ctx *c, embi_str label)
{
    embi_bind_counter *k = counter_find(c, label, true);
    return ++k->n;
}

bool embi_bind_console_count_reset(embi_bind_ctx *c, embi_str label)
{
    embi_bind_counter *k = counter_find(c, label, false);
    if (!k || k->n == 0) {
        return false;
    }
    k->n = 0;
    counter_prune(c, k);
    return true;
}

void embi_bind_timer_start(embi_bind_ctx *c, embi_str label)
{
    embi_bind_counter *k = counter_find(c, label, true);
    if (!k->timing) {
        k->timing = true;
        k->start = embi_bind_now_ms(c);
    } else if (k->n == 0) {
        counter_prune(c, k);
    }
}

double embi_bind_timer_end(embi_bind_ctx *c, embi_str label, bool stop)
{
    embi_bind_counter *k = counter_find(c, label, false);
    if (!k || !k->timing) {
        return -1;
    }
    double d = embi_bind_now_ms(c) - k->start;
    if (stop) {
        k->timing = false;
        counter_prune(c, k);
    }
    return d;
}

embi_js_value embi_bind_eval_asset(embi_bind_ctx *c, const unsigned char *data, size_t size, const char *name)
{
#ifdef EMBER_LZ_ASSETS
    size_t raw = 0;
    uint8_t *src = embi_lz_unpack(data, size, &raw);
    if (src == nullptr) {
        return embi_js_throw_range_error(c->rt, "embedded script %s is damaged", name);
    }
    embi_js_value fn = embi_js_eval(c->realm, (const char *)src, raw, name, 0);
    embi_free(src, raw + 1);
    return fn;
#else
    return embi_js_eval(c->realm, (const char *)data, size - 1, name, 0);
#endif
}
