/*
 * MutationObserver, ResizeObserver and IntersectionObserver (W6d, observers.webidl).
 *
 * The observer objects keep their C state in the wrapper (ptr and owned are the state struct, freed by the wrapper's
 * finalizer). An observer that observes is pinned (a persistent handle) so that its wrapper, and with it the state, lives
 * while the DOM or the frame can still reach it; the pin is released when the observer stops observing. The state arrays
 * (observations, queued entries) are freed when the observer stops observing, so the finalizer frees the struct only.
 *
 * MutationObserver: the DOM queues records (src/dom/mutation.c); the signal enqueues one microtask per batch, and that
 * microtask builds the MutationRecord objects and calls the callbacks in the order the observers were created.
 *
 * ResizeObserver and IntersectionObserver: the frame calls embi_observers_after_layout (observers.h), which measures the
 * observed boxes with the layout engine and delivers. Both kinds are listed in the document's registry (the ext slot of
 * the mutation state).
 *
 * Records and entries are plain objects behind their wrappers: the getters read their fields.
 */
#include "bindings/impl.h"
#include "bindings/gen/observers.h"
#include "bindings/observers.h"
#include "bindings/svg_geometry.h"
#include "dom/mutation.h"
#include "js/jobs.h"
#include "layout/box.h"
#include "layout/fragment.h"
#include "layout/layout.h"
#include "style/style.h"

#include <stdint.h>

#include <SDL3/SDL_stdinc.h>

/* Kinds of the observer state structs (their first member). */
enum { OBS_RO = 1, OBS_IO = 2 };

/* Box options (ResizeObserverBoxOptions, in the order of the enum). */
enum { BOX_CONTENT = 0, BOX_BORDER = 1, BOX_DEVICE = 2 };

/* The ResizeObserver loop: how many passes at most (the spec's depth loop ends sooner in practice). */
#define RO_MAX_ROUNDS 64u

/* ------------------------------------------------------------------ helpers */

static embi_str cstr(const char *s)
{
    return (embi_str){s, (uint32_t)SDL_strlen(s)};
}

/* A property of a plain object value (undefined when v is not an object). */
static embi_js_value fget(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (!embi_js_is_object(obj)) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
}

/* A field of a wrapper's record object (the wrapper's ref holds it). */
static embi_js_value fld(embi_bind_ctx *c, const embi_bind_obj *self, const char *k)
{
    return embi_js_is_object(self->ref) ? fget(c, self->ref, k) : EMBI_JS_UNDEFINED;
}

static void fset(embi_bind_ctx *c, embi_js_value obj, const char *k, embi_js_value v)
{
    embi_bind_object_set_c(c, obj, k, v);
}

/* A fresh plain object, kept alive until the scope ends. */
static embi_js_value new_fields(embi_bind_ctx *c)
{
    return embi_bind_pin(c, embi_bind_object_new(c));
}

/* A wrapper of iface whose fields are the given object (kept as the wrapper's ref). */
static embi_js_value wrap(embi_bind_ctx *c, uint16_t iface, embi_js_value fields)
{
    embi_js_value v = embi_bind_pin(c, embi_bind_other_value(c, iface, c, nullptr, 0));
    embi_bind_set_ref(embi_bind_obj_of(v), fields);
    return v;
}

/* Pins a value in the persistent handles: the returned token is the handle plus one (never 0). */
static uint32_t pin_value(embi_bind_ctx *c, embi_js_value v)
{
    return embi_js_persistent_new(c->rt, v) + 1u;
}

static void unpin(embi_bind_ctx *c, uint32_t *pin)
{
    if (*pin) {
        embi_js_persistent_free(c->rt, *pin - 1u);
        *pin = 0;
    }
}

static embi_js_value node_or_null(embi_bind_ctx *c, embi_node *n)
{
    return n ? embi_bind_node_value(c, n) : EMBI_JS_NULL;
}

static bool truthy(embi_js_value v)
{
    return embi_js_to_boolean(v);
}

/* True when the option bag has the member (an absent or non-object bag has none). */
static bool opt_present(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (!embi_js_is_object(obj)) {
        return false;
    }
    return !embi_js_is_undefined(fget(c, obj, k));
}

/* ------------------------------------------------------------------ geometry */

typedef struct meas {
    double bw, bh; /* border box size */
    double cw, ch; /* content box size */
    double px, py; /* padding-left and padding-top: the content box origin inside the border box */
    bool vert;     /* the writing mode is vertical: the inline size is the height */
} meas;

static bool svg_rect(embi_bind_ctx *c, embi_node *n, double r[4])
{
    if (!embi_impl_svg_client_bbox(c, n, r)) {
        return false;
    }
    double sx = 0.0, sy = 0.0;
    embi_impl_root_offset(c, &sx, &sy);
    r[0] += sx;
    r[2] += sx;
    r[1] += sy;
    r[3] += sy;
    return true;
}

/* The border box of n in document coordinates (r: x0, y0, x1, y1). False when there is no box. */
static bool border_rect(embi_bind_ctx *c, embi_node *n, double r[4])
{
    embi_layout_engine *e = embi_bind_layout(c);
    embi_rectf rf;
    /* A node outside the document has no box (its layout box can be stale until the next layout). */
    if (!e || !n || !(n->flags & EMBI_NF_CONNECTED) || !embi_is_element(n)) {
        return false;
    }
    if (!embi_layout_border_box_xf(e, embi_as_element(n), &rf)) {
        return svg_rect(c, n, r);
    }
    r[0] = rf.x;
    r[1] = rf.y;
    r[2] = (double)rf.x + rf.w;
    r[3] = (double)rf.y + rf.h;
    return true;
}

static void measure(embi_bind_ctx *c, embi_node *n, meas *m)
{
    SDL_memset(m, 0, sizeof *m);
    embi_layout_engine *e = embi_bind_layout(c);
    embi_rectf rf;
    if (!e || !n || !(n->flags & EMBI_NF_CONNECTED) || !embi_is_element(n)) {
        return;
    }
    if (!embi_layout_border_box(e, embi_as_element(n), &rf)) {
        /* An SVG graphics element has no CSS box: its size is its bounding box (SVG 2, getBBox) and its content rect
         * starts at 0, 0. */
        double b[4] = {0, 0, 0, 0};
        const embi_svg_geometry_ops *g = c->svg_geom;
        if (n->extra == EMBI_NS_SVG && g && g->bbox && g->bbox(c->svg_geom_user, n, b)) {
            m->bw = m->cw = b[2] > 0 ? b[2] : 0;
            m->bh = m->ch = b[3] > 0 ? b[3] : 0;
        }
        return;
    }
    {
        const embi_style *ts = embi_style_get(embi_bind_style(c), embi_as_element(n), EMBI_PE_NONE);
        const embi_value *wm = ts ? embi_style_value(ts, EMBI_PROP_writing_mode) : nullptr;
        if (wm && wm->kind == EMBI_V_KEYWORD) {
            m->vert = wm->kw == EMBI_KW_vertical_rl || wm->kw == EMBI_KW_vertical_lr || wm->kw == EMBI_KW_sideways_rl ||
                      wm->kw == EMBI_KW_sideways_lr;
        }
    }
    /* A non-replaced inline box has no size to report (0 x 0). */
    const embi_box *ebox = embi_as_element(n)->box;
    if (ebox != nullptr && ebox->kind == EMBI_BK_INLINE) {
        return;
    }
    float bdr[4] = {0, 0, 0, 0};
    float pad[4] = {0, 0, 0, 0};
    float mg[4] = {0, 0, 0, 0};
    (void)embi_layout_box_edges(e, embi_as_element(n), bdr, pad, mg);
    m->bw = rf.w;
    m->bh = rf.h;
    double cw = (double)rf.w - bdr[0] - bdr[2] - pad[0] - pad[2];
    double ch = (double)rf.h - bdr[1] - bdr[3] - pad[1] - pad[3];
    m->cw = cw > 0 ? cw : 0;
    m->ch = ch > 0 ? ch : 0;
    m->px = pad[0];
    m->py = pad[1];
}

static uint32_t depth_of(const embi_node *n)
{
    uint32_t d = 0;
    for (const embi_node *x = n->parent; x; x = x->parent) {
        d++;
    }
    return d;
}

/* depth_of(n) <= 2 without walking the whole chain (the document element and body). */
static bool depth_le2(const embi_node *n)
{
    const embi_node *p = n->parent;
    return !p || !p->parent || !p->parent->parent;
}

/* The document position of a fragment's border-box origin (layout coordinates, no scroll offsets applied). */
static void frag_origin(const embi_frag *f, double *x, double *y)
{
    double ox = 0;
    double oy = 0;
    for (const embi_frag *p = f; p; p = p->parent) {
        ox += p->x;
        oy += p->y;
    }
    *x = ox;
    *y = oy;
}

/* The box fragment of el when el has a block-level (non-inline) principal box, else nullptr. */
static const embi_frag *principal_frag(const embi_element *el)
{
    if (!el || !el->box) {
        return nullptr;
    }
    const embi_box *b = el->box;
    if (b->kind == EMBI_BK_INLINE || b->kind == EMBI_BK_TEXT || b->kind == EMBI_BK_MARKER) {
        return nullptr;
    }
    return b->frag;
}

/* The result of walking from a target up its fragment (containing block) chain. */
typedef struct clipwalk {
    double rect[4];      /* the target border box, scrolled and clipped by the ancestors up to the root (frame of the root) */
    bool empty;          /* an ancestor clips the rect to nothing */
    bool root_found;     /* the walk met the root element (always false for the viewport root) */
    double root_rect[4]; /* the root element's rectangle for the intersection: its padding box when it clips, else its border box */
    double shift_total[2]; /* scroll offsets of all the scroll containers on the chain: client = layout - shift */
    double shift_up[2];    /* the part of it above the root element (root frame -> client) */
} clipwalk;

/* Walks the containing-block chain of target: the border box T (layout coordinates) is moved by the scroll offset of each
 * scroll container and clipped by the padding box of each ancestor whose overflow is not visible, up to the root element
 * (an element root) or the document. The document element and body are skipped (their overflow belongs to the viewport). */
static void clip_walk(embi_node *target, const embi_node *root, const double T[4], const double *smv, const bool *smp,
                      clipwalk *w)
{
    SDL_memset(w, 0, sizeof *w);
    SDL_memcpy(w->rect, T, sizeof w->rect);
    /* The starting fragment: the parent of the target's own block fragment, or the fragment of the nearest block-level
     * ancestor for an inline target. */
    const embi_frag *start = nullptr;
    const embi_frag *own = embi_is_element(target) ? principal_frag(embi_as_element(target)) : nullptr;
    if (own) {
        start = own->parent;
    } else {
        for (const embi_node *a = target->parent; a; a = a->parent) {
            if (embi_is_element(a) && (start = principal_frag((const embi_element *)a)) != nullptr) {
                break;
            }
        }
    }
    bool past_root = false;
    for (const embi_frag *p = start; p; p = p->parent) {
        if (p->kind != EMBI_FK_BOX || !p->box || !p->box->el) {
            continue;
        }
        const embi_element *el = p->box->el;
        if (el->box != (const void *)p->box || p->box->frag != p) {
            continue; /* a generated or anonymous box, or a later fragment of the box */
        }
        bool is_root = root && &el->base == root;
        bool sc = (p->flags & EMBI_FF_SCROLL) != 0;
        bool cl = (p->flags & (EMBI_FF_SCROLL | EMBI_FF_CLIP)) != 0;
        if (!is_root && depth_le2(&el->base)) {
            cl = false;
            sc = false;
        }
        double sx = sc ? (double)embi_frag_sx(p) : 0.0;
        double sy = sc ? (double)embi_frag_sy(p) : 0.0;
        w->shift_total[0] += sx;
        w->shift_total[1] += sy;
        if (past_root) {
            w->shift_up[0] += sx;
            w->shift_up[1] += sy;
            continue;
        }
        double ox = 0;
        double oy = 0;
        frag_origin(p, &ox, &oy);
        double px0 = ox + (cl ? embi_frag_bdr(p, 0) : 0.0);
        double py0 = oy + (cl ? embi_frag_bdr(p, 1) : 0.0);
        double px1 = ox + p->w - (cl ? embi_frag_bdr(p, 2) : 0.0);
        double py1 = oy + p->h - (cl ? embi_frag_bdr(p, 3) : 0.0);
        if (sc && !is_root && smv != nullptr) {
            /* scrollMargin grows the clip of each scroll container between the target and the root */
            double cw = px1 - px0;
            double ch = py1 - py0;
            py0 -= smp[0] ? smv[0] * ch / 100.0 : smv[0];
            px1 += smp[1] ? smv[1] * cw / 100.0 : smv[1];
            py1 += smp[2] ? smv[2] * ch / 100.0 : smv[2];
            px0 -= smp[3] ? smv[3] * cw / 100.0 : smv[3];
        }
        w->rect[0] -= sx;
        w->rect[2] -= sx;
        w->rect[1] -= sy;
        w->rect[3] -= sy;
        if (is_root) {
            w->root_found = true;
            w->root_rect[0] = px0;
            w->root_rect[1] = py0;
            w->root_rect[2] = px1;
            w->root_rect[3] = py1;
            past_root = true;
            continue;
        }
        if (cl) {
            if (w->rect[0] < px0) {
                w->rect[0] = px0;
            }
            if (w->rect[1] < py0) {
                w->rect[1] = py0;
            }
            if (w->rect[2] > px1) {
                w->rect[2] = px1;
            }
            if (w->rect[3] > py1) {
                w->rect[3] = py1;
            }
            if (w->rect[0] > w->rect[2] || w->rect[1] > w->rect[3]) {
                w->empty = true;
            }
        }
    }
    /* A position: fixed box of the viewport (the target or a box around it) does not move when the viewport scrolls: its
     * layout rectangle is where it is in the viewport, so in document coordinates it moves with the viewport's offset,
     * like a scroll container with the opposite offset. Nothing above it clips it (its containing block is the viewport). */
    for (const embi_frag *p = own != nullptr ? own : start; p != nullptr; p = p->parent) {
        if (embi_lay_frag_vp_fixed(p)) {
            double vx = (double)embi_frag_sx(p->parent), vy = (double)embi_frag_sy(p->parent);
            w->shift_total[0] -= vx;
            w->shift_total[1] -= vy;
            w->shift_up[0] -= vx;
            w->shift_up[1] -= vy;
            if (!w->root_found) {
                w->rect[0] += vx;
                w->rect[2] += vx;
                w->rect[1] += vy;
                w->rect[3] += vy;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ registry of the frame observers */

/* The ResizeObserver and IntersectionObserver objects that observe something (kept in the mutation state's ext slot). */
typedef struct obs_reg {
    void **obs; /* embi_ro * or embi_io *, by their kind */
    uint32_t n, cap;
    bool io_task; /* an IntersectionObserver delivery task is scheduled */
    uint32_t io_task_id; /* its timer id */
} obs_reg;

static void reg_free(void *p)
{
    obs_reg *r = p;
    if (r->obs) {
        embi_free(r->obs, (size_t)r->cap * sizeof *r->obs);
    }
    embi_free(r, sizeof *r);
}

static obs_reg *reg_get(embi_bind_ctx *c, bool create)
{
    obs_reg *r = embi_mut_ext(c->doc);
    if (!r && create) {
        r = embi_calloc(1, sizeof *r);
        embi_mut_set_ext(c->doc, r, reg_free);
    }
    return r;
}

/* Creation order of the observer objects (all three kinds): the notification order of the spec follows it. */
static uint64_t g_obs_seq;

static uint64_t obs_seq(const void *obs);

/* Adds obs at its place in creation order (the registry is sorted by the creation sequence number). */
static void reg_add(embi_bind_ctx *c, void *obs)
{
    obs_reg *r = reg_get(c, true);
    if (r->n == r->cap) {
        uint32_t nc = r->cap ? r->cap * 2u : 4u;
        r->obs = embi_realloc(r->obs, (size_t)r->cap * sizeof *r->obs, (size_t)nc * sizeof *r->obs);
        r->cap = nc;
    }
    uint32_t at = r->n;
    uint64_t sq = obs_seq(obs);
    while (at > 0 && obs_seq(r->obs[at - 1u]) > sq) {
        r->obs[at] = r->obs[at - 1u];
        at--;
    }
    r->obs[at] = obs;
    r->n++;
}

static void reg_remove(embi_bind_ctx *c, void *obs)
{
    obs_reg *r = reg_get(c, false);
    if (!r) {
        return;
    }
    for (uint32_t i = 0; i < r->n; i++) {
        if (r->obs[i] == obs) {
            for (uint32_t j = i + 1; j < r->n; j++) {
                r->obs[j - 1] = r->obs[j];
            }
            r->n--;
            return;
        }
    }
}

static void release_holds(embi_bind_ctx *c);

/* ------------------------------------------------------------------ MutationObserver */

typedef struct embi_mo_state {
    embi_mo mo; /* the core state (dom/mutation.h): first member, so the wrapper's owned block is this struct */
} embi_mo_state;

/* The MutationRecord object of a queued record. */
static embi_js_value mrec_value(embi_bind_ctx *c, const embi_mrec *r)
{
    embi_js_value f = new_fields(c);
    const char *type = r->type == EMBI_MR_ATTRIBUTES      ? "attributes"
                       : r->type == EMBI_MR_CHARACTER_DATA ? "characterData"
                                                           : "childList";
    fset(c, f, "type", embi_bind_pin(c, embi_bind_utf8_value(c, cstr(type))));
    fset(c, f, "target", node_or_null(c, r->target));
    bool child = r->type == EMBI_MR_CHILD_LIST;
    embi_js_value added = embi_bind_pin(c, embi_impl_nodelist_static(c, r->added, r->nadded));
    embi_js_value removed = embi_bind_pin(c, embi_impl_nodelist_static(c, r->removed, r->nremoved));
    fset(c, f, "addedNodes", added);
    fset(c, f, "removedNodes", removed);
    fset(c, f, "previousSibling", child ? node_or_null(c, r->prev) : EMBI_JS_NULL);
    fset(c, f, "nextSibling", child ? node_or_null(c, r->next) : EMBI_JS_NULL);
    bool attr = r->type == EMBI_MR_ATTRIBUTES;
    fset(c, f, "attributeName", attr ? embi_bind_pin(c, embi_bind_utf8_value(c, embi_atom_str(r->attr_local))) : EMBI_JS_NULL);
    fset(c, f, "attributeNamespace",
         attr && r->attr_ns != EMBI_NS_NULL
             ? embi_bind_pin(c, embi_bind_utf8_value(c, embi_dom_ns_uri(r->attr_ns, r->attr_ns_uri)))
             : EMBI_JS_NULL);
    fset(c, f, "oldValue", r->has_old ? embi_bind_pin(c, embi_bind_dstr_value(c, r->old_value)) : EMBI_JS_NULL);
    return wrap(c, EMBI_IF_MutationRecord, f);
}

/* An array of MutationRecord objects. */
static embi_js_value mrec_array(embi_bind_ctx *c, const embi_mrec *recs, uint32_t n)
{
    embi_js_value arr = embi_bind_pin(c, embi_bind_array_new(c, n));
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value v = embi_bind_pin(c, mrec_value(c, &recs[i]));
        embi_bind_array_set(c, arr, i, v);
    }
    return arr;
}

/* Frees the filter copy of a parsed option bag (observe copies what it keeps). */
static void free_init_opts(embi_mo_opts *o)
{
    if (o->filter) {
        embi_free(o->filter, (size_t)o->nfilter * sizeof *o->filter);
    }
    o->filter = nullptr;
    o->nfilter = 0;
}

/* Reads a MutationObserverInit (any). Returns false with a TypeError pending when it is not valid (the spec's checks). */
static bool parse_mo_init(embi_bind_ctx *c, embi_js_value v, embi_mo_opts *o)
{
    SDL_memset(o, 0, sizeof *o);
    if (!embi_js_is_nullish(v) && !embi_js_is_object(v)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'observe' on 'MutationObserver': parameter 2 is not of type 'MutationObserverInit'.");
        return false;
    }
    bool has_child = opt_present(c, v, "childList");
    bool child = has_child && truthy(fget(c, v, "childList"));
    bool has_attr = opt_present(c, v, "attributes");
    bool attr = has_attr && truthy(fget(c, v, "attributes"));
    bool has_char = opt_present(c, v, "characterData");
    bool chr = has_char && truthy(fget(c, v, "characterData"));
    bool sub = opt_present(c, v, "subtree") && truthy(fget(c, v, "subtree"));
    bool has_aold = opt_present(c, v, "attributeOldValue");
    bool aold = has_aold && truthy(fget(c, v, "attributeOldValue"));
    bool has_cold = opt_present(c, v, "characterDataOldValue");
    bool cold = has_cold && truthy(fget(c, v, "characterDataOldValue"));
    bool has_filter = opt_present(c, v, "attributeFilter");
    /* Defaults: attributes follows attributeOldValue and attributeFilter, characterData follows characterDataOldValue. */
    if (!has_attr && (has_aold || has_filter)) {
        attr = true;
    }
    if (!has_char && has_cold) {
        chr = true;
    }
    if (aold && !attr) {
        (void)embi_js_throw_type_error(c->rt, "The options object must set 'attributes' to true if 'attributeOldValue' is true.");
        return false;
    }
    if (has_filter && !attr) {
        (void)embi_js_throw_type_error(c->rt, "The options object must set 'attributes' to true if 'attributeFilter' is present.");
        return false;
    }
    if (cold && !chr) {
        (void)embi_js_throw_type_error(c->rt, "The options object must set 'characterData' to true if 'characterDataOldValue' is true.");
        return false;
    }
    if (!child && !attr && !chr) {
        (void)embi_js_throw_type_error(c->rt, "The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
        return false;
    }
    o->child_list = child;
    o->attributes = attr;
    o->character_data = chr;
    o->subtree = sub;
    o->attr_old = aold;
    o->char_old = cold;
    o->has_filter = has_filter;
    if (has_filter) {
        embi_js_value arr = fget(c, v, "attributeFilter");
        if (!embi_js_is_object(arr)) {
            (void)embi_js_throw_type_error(c->rt, "Failed to execute 'observe' on 'MutationObserver': The provided value cannot be converted to a sequence.");
            return false;
        }
        uint32_t n = embi_bind_array_len(c, arr);
        if (n) {
            o->filter = embi_alloc((size_t)n * sizeof *o->filter);
            o->nfilter = n;
            for (uint32_t i = 0; i < n; i++) {
                embi_dstr d;
                if (!embi_bind_to_dstr(c, embi_bind_array_get(c, arr, i), &d)) {
                    free_init_opts(o);
                    return false;
                }
                embi_str s = embi_bind_utf8(c, d);
                o->filter[i] = embi_atom_intern(s.ptr, s.len);
            }
        }
    }
    return true;
}

/* The microtask of the mutation observers: delivers the batch that the DOM queued. Called by the engine's job queue. */
static embi_js_value deliver_mutations(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_bind_ctx *c = embi_js_native_data_current(r);
    if (!c) {
        return EMBI_JS_UNDEFINED;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_mo **list = nullptr;
    uint32_t n = embi_mut_take_active(c->doc, &list);
    if (n) {
        /* The observers' wrappers are kept in an array, so the state of every observer in the batch stays alive while
         * the callbacks run (a callback may disconnect a later observer, which releases its pin). */
        embi_js_value wrappers = embi_bind_pin(c, embi_bind_array_new(c, n));
        for (uint32_t i = 0; i < n; i++) {
            embi_bind_array_set(c, wrappers, i, embi_bind_obj_value((embi_bind_obj *)list[i]->user));
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_mo *mo = list[i];
            embi_bind_obj *bo = mo->user;
            embi_mrec *recs = nullptr;
            uint32_t nr = embi_mo_take_records(mo, &recs);
            if (nr == 0) {
                continue;
            }
            embi_js_value args[2];
            args[0] = mrec_array(c, recs, nr);
            args[1] = embi_bind_obj_value(bo);
            embi_mrec_free_all(recs, nr);
            embi_js_value cb = bo->ref;
            embi_js_value res = embi_js_call(c->rt, cb, args[1], 2, args);
            if (res == EMBI_JS_EXCEPTION) {
                embi_js_value err = embi_js_exception_get(c->rt);
                embi_js_exception_clear(c->rt);
                embi_bind_report_error(c, err, nullptr, 0, 0);
            }
        }
        embi_free(list, (size_t)n * sizeof *list);
    }
    release_holds(c);
    return embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* The signal of the mutation state: queues the microtask (one per batch). */
static void mo_signal(void *user)
{
    embi_bind_ctx *c = user;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_object *fn = embi_js_native_function_new_data(c->realm, "notifyMutationObservers", 0, deliver_mutations, 0, c);
    if (fn) {
        embi_js_value f = embi_bind_pin(c, embi_js_from_object(fn));
        embi_js_enqueue_microtask(c->rt, f, EMBI_JS_UNDEFINED);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

embi_js_value embi_impl_MutationObserver_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_mo_state *st = embi_calloc(1, sizeof *st);
    st->mo.seq = ++g_obs_seq;
    embi_js_value v = embi_bind_pin(c, embi_bind_other_value(c, EMBI_IF_MutationObserver, &st->mo, st, sizeof *st));
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_bind_set_ref(bo, a0);
    st->mo.user = bo;
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

/* Returns undefined, or EMBI_JS_EXCEPTION with a TypeError pending (the wrapper passes the value through, so a void
 * member could not report it). */
embi_js_value embi_impl_MutationObserver_observe(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_mo *mo = self->ptr;
    embi_mo_opts o;
    if (!parse_mo_init(c, a1, &o)) {
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    if (embi_mo_observe(mo, c->doc, a0, &o) && mo->pin == 0) {
        mo->pin = pin_value(c, embi_bind_obj_value(self));
    }
    embi_mut_set_signal(c->doc, mo_signal, c);
    free_init_opts(&o);
    return embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_impl_MutationObserver_disconnect(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_mo *mo = self->ptr;
    if (embi_mo_disconnect(mo)) {
        unpin(c, &mo->pin);
    }
    release_holds(c);
}

embi_js_value embi_impl_MutationObserver_takeRecords(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_mrec *recs = nullptr;
    uint32_t n = embi_mo_take_records(self->ptr, &recs);
    embi_js_value arr = mrec_array(c, recs, n);
    embi_mrec_free_all(recs, n);
    release_holds(c);
    return embi_bind_leave(sc, arr);
}

embi_js_value embi_impl_MutationRecord_get_type(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "type");
}

embi_js_value embi_impl_MutationRecord_get_target(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "target");
}

embi_js_value embi_impl_MutationRecord_get_addedNodes(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "addedNodes");
}

embi_js_value embi_impl_MutationRecord_get_removedNodes(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "removedNodes");
}

embi_js_value embi_impl_MutationRecord_get_previousSibling(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "previousSibling");
}

embi_js_value embi_impl_MutationRecord_get_nextSibling(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "nextSibling");
}

embi_js_value embi_impl_MutationRecord_get_attributeName(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "attributeName");
}

embi_js_value embi_impl_MutationRecord_get_attributeNamespace(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "attributeNamespace");
}

embi_js_value embi_impl_MutationRecord_get_oldValue(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "oldValue");
}

/* ------------------------------------------------------------------ ResizeObserver */

typedef struct ro_item {
    embi_node *target;
    uint8_t box;   /* BOX_* */
    bool active;   /* changed and due in the current pass */
    double lw, lh;
    meas m;        /* the measurement of the current pass */
} ro_item;

typedef struct embi_ro {
    uint8_t kind;    /* OBS_RO (first: the registry tells the kinds apart by it) */
    bool registered; /* in the registry and pinned */
    uint32_t pin;
    uint64_t seq;    /* creation order */
    embi_bind_obj *bo;
    ro_item *items;
    uint32_t n, cap;
} embi_ro;

static uint64_t obs_seq(const void *obs);

static void ro_register(embi_bind_ctx *c, embi_ro *ro)
{
    if (!ro->registered) {
        ro->registered = true;
        reg_add(c, ro);
        ro->pin = pin_value(c, embi_bind_obj_value(ro->bo));
    }
}

/* Stops observing: leaves the registry, releases the pin and frees the observations. */
static void ro_release(embi_bind_ctx *c, embi_ro *ro)
{
    if (ro->registered) {
        ro->registered = false;
        reg_remove(c, ro);
        unpin(c, &ro->pin);
    }
    if (ro->items) {
        embi_free(ro->items, (size_t)ro->cap * sizeof *ro->items);
    }
    ro->items = nullptr;
    ro->n = 0;
    ro->cap = 0;
}

static void ro_box_size_phys(const ro_item *it, double dpr, double *w, double *h)
{
    switch (it->box) {
    case BOX_BORDER:
        *w = it->m.bw;
        *h = it->m.bh;
        break;
    case BOX_DEVICE:
        *w = SDL_round(it->m.cw * dpr);
        *h = SDL_round(it->m.ch * dpr);
        break;
    default:
        *w = it->m.cw;
        *h = it->m.ch;
        break;
    }
}

/* The size as (inline, block): the physical sizes swap in a vertical writing mode. */
static void ro_box_size(const ro_item *it, double dpr, double *w, double *h)
{
    ro_box_size_phys(it, dpr, w, h);
    if (it->m.vert) {
        double t = *w;
        *w = *h;
        *h = t;
    }
}

static embi_js_value size_obj(embi_bind_ctx *c, double w, double h)
{
    embi_js_value o = new_fields(c);
    fset(c, o, "inlineSize", embi_js_from_double(w));
    fset(c, o, "blockSize", embi_js_from_double(h));
    return o;
}

static embi_js_value size_list(embi_bind_ctx *c, double w, double h, bool vert)
{
    if (vert) {
        double t = w;
        w = h;
        h = t;
    }
    embi_js_value arr = embi_bind_pin(c, embi_bind_array_new(c, 1));
    embi_bind_array_set(c, arr, 0, size_obj(c, w, h));
    return arr;
}

/* A ResizeObserverEntry for the measurement of it. */
static embi_js_value ro_entry(embi_bind_ctx *c, const ro_item *it, double dpr)
{
    embi_js_value f = new_fields(c);
    fset(c, f, "target", node_or_null(c, it->target));
    fset(c, f, "contentRect", embi_bind_pin(c, embi_impl_rect_new(c, it->m.px, it->m.py, it->m.cw, it->m.ch)));
    fset(c, f, "borderBoxSize", size_list(c, it->m.bw, it->m.bh, it->m.vert));
    fset(c, f, "contentBoxSize", size_list(c, it->m.cw, it->m.ch, it->m.vert));
    fset(c, f, "devicePixelContentBoxSize", size_list(c, SDL_round(it->m.cw * dpr), SDL_round(it->m.ch * dpr), it->m.vert));
    return wrap(c, EMBI_IF_ResizeObserverEntry, f);
}

/* Delivers the active observations of every ResizeObserver: one callback per observer. Returns the shallowest target
 * depth delivered (-1 when none). */
static int ro_deliver(embi_bind_ctx *c, obs_reg *reg, double dpr)
{
    embi_bind_scope sc = embi_bind_enter(c);
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < reg->n; i++) {
        if (*(uint8_t *)reg->obs[i] != OBS_RO) {
            continue;
        }
        const embi_ro *ro = reg->obs[i];
        for (uint32_t j = 0; j < ro->n; j++) {
            if (ro->items[j].active) {
                cnt++;
                break;
            }
        }
    }
    int shallow = INT32_MAX;
    if (cnt) {
        embi_js_value wr = embi_bind_pin(c, embi_bind_array_new(c, cnt));
        embi_js_value ev = embi_bind_pin(c, embi_bind_array_new(c, cnt));
        uint32_t k = 0;
        for (uint32_t i = 0; i < reg->n; i++) {
            if (*(uint8_t *)reg->obs[i] != OBS_RO) {
                continue;
            }
            embi_ro *ro = reg->obs[i];
            uint32_t na = 0;
            for (uint32_t j = 0; j < ro->n; j++) {
                na += ro->items[j].active ? 1u : 0u;
            }
            if (na == 0) {
                continue;
            }
            embi_js_value list = embi_bind_pin(c, embi_bind_array_new(c, na));
            uint32_t q = 0;
            for (uint32_t j = 0; j < ro->n; j++) {
                ro_item *it = &ro->items[j];
                if (!it->active) {
                    continue;
                }
                double w = 0;
                double h = 0;
                ro_box_size(it, dpr, &w, &h);
                it->lw = w;
                it->lh = h;
                it->active = false;
                int d = (int)depth_of(it->target);
                if (d < shallow) {
                    shallow = d;
                }
                embi_bind_array_set(c, list, q++, ro_entry(c, it, dpr));
            }
            embi_bind_array_set(c, wr, k, embi_bind_obj_value(ro->bo));
            embi_bind_array_set(c, ev, k, list);
            k++;
        }
        for (uint32_t i = 0; i < cnt; i++) {
            embi_js_value obs = embi_bind_array_get(c, wr, i);
            embi_bind_obj *bo = embi_bind_obj_of(obs);
            embi_js_value args[2] = {embi_bind_array_get(c, ev, i), obs};
            embi_js_value res = embi_js_call(c->rt, bo->ref, obs, 2, args);
            if (res == EMBI_JS_EXCEPTION) {
                embi_js_value err = embi_js_exception_get(c->rt);
                embi_js_exception_clear(c->rt);
                embi_bind_report_error(c, err, nullptr, 0, 0);
            }
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return shallow == INT32_MAX ? -1 : shallow;
}

/* Restyles and lays the document out again after a callback changed it (the same work as the frame's update: the style
 * engine resolves the document, then the layout engine lays it out). bindings cannot call embi_doc_update (src/embed). */
static void ro_relayout(embi_bind_ctx *c)
{
    embi_style_engine *st = embi_bind_style(c);
    if (st) {
        embi_style_resolve_document(st, c->doc);
    }
    embi_layout_engine *e = embi_bind_layout(c);
    if (e) {
        (void)embi_layout_document(e, c->doc);
    }
}

/* The ResizeObserver step of the frame (the spec's loop): each pass gathers the observations whose box changed and whose
 * target is deeper than the shallowest target the last pass delivered; it delivers them, and when the callbacks changed
 * the DOM the document is laid out again before the next pass. Changes that a pass skips (targets not deeper than that
 * floor) make the loop report an error event. */
static void ro_loop(embi_bind_ctx *c, obs_reg *reg)
{
    double dpr = 1.0;
    double vw = 0;
    double vh = 0;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    int floor_depth = -1;
    bool undelivered = false;
    for (uint32_t round = 0; round < RO_MAX_ROUNDS; round++) {
        uint32_t epoch = c->mut_epoch;
        bool any = false;
        undelivered = false;
        for (uint32_t i = 0; i < reg->n; i++) {
            if (*(uint8_t *)reg->obs[i] != OBS_RO) {
                continue;
            }
            embi_ro *ro = reg->obs[i];
            for (uint32_t j = 0; j < ro->n; j++) {
                ro_item *it = &ro->items[j];
                measure(c, it->target, &it->m);
                double w = 0;
                double h = 0;
                ro_box_size(it, dpr, &w, &h);
                it->active = false;
                if (w == it->lw && h == it->lh) {
                    continue;
                }
                if ((int)depth_of(it->target) > floor_depth) {
                    it->active = true;
                    any = true;
                } else {
                    undelivered = true;
                }
            }
        }
        if (!any) {
            break;
        }
        int shallow = ro_deliver(c, reg, dpr);
        if (shallow >= 0) {
            floor_depth = shallow;
        }
        if (c->mut_epoch != epoch) {
            ro_relayout(c);
        }
    }
    if (undelivered) {
        embi_bind_scope sc = embi_bind_enter(c);
        embi_js_value msg = embi_bind_pin(c, embi_bind_utf8_value(c, cstr("ResizeObserver loop completed with undelivered notifications.")));
        embi_bind_report_error(c, msg, nullptr, 0, 0);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
}

embi_js_value embi_impl_ResizeObserver_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_ro *ro = embi_calloc(1, sizeof *ro);
    ro->kind = OBS_RO;
    ro->seq = ++g_obs_seq;
    embi_js_value v = embi_bind_pin(c, embi_bind_other_value(c, EMBI_IF_ResizeObserver, ro, ro, sizeof *ro));
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_bind_set_ref(bo, a0);
    ro->bo = bo;
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

void embi_impl_ResizeObserver_observe(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0,
                                      embi_dict_ResizeObserverOptions a1)
{
    embi_ro *ro = self->ptr;
    uint8_t box = (uint8_t)a1.box;
    for (uint32_t i = 0; i < ro->n; i++) {
        if (ro->items[i].target == a0) {
            /* The spec removes and adds the observation: the last reported size starts again at 0x0. */
            ro->items[i].box = box;
            ro->items[i].lw = -1; /* the first observation is always reported, even for 0 x 0 */
            ro->items[i].lh = -1;
            return;
        }
    }
    if (ro->n == ro->cap) {
        uint32_t nc = ro->cap ? ro->cap * 2u : 4u;
        ro->items = embi_realloc(ro->items, (size_t)ro->cap * sizeof *ro->items, (size_t)nc * sizeof *ro->items);
        ro->cap = nc;
    }
    ro_item *it = &ro->items[ro->n++];
    SDL_memset(it, 0, sizeof *it);
    it->target = a0;
    it->box = box;
    it->lw = -1;
    it->lh = -1;
    embi_mut_hold(a0);
    ro_register(c, ro);
}

void embi_impl_ResizeObserver_unobserve(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0)
{
    embi_ro *ro = self->ptr;
    for (uint32_t i = 0; i < ro->n; i++) {
        if (ro->items[i].target == a0) {
            SDL_memmove(&ro->items[i], &ro->items[i + 1u], (size_t)(ro->n - i - 1u) * sizeof *ro->items);
            ro->n--;
            break;
        }
    }
    if (ro->n == 0) {
        ro_release(c, ro);
    }
    embi_mut_touch(c->doc);
    release_holds(c);
}

void embi_impl_ResizeObserver_disconnect(embi_bind_ctx *c, embi_bind_obj *self)
{
    ro_release(c, self->ptr);
    embi_mut_touch(c->doc);
    release_holds(c);
}

embi_js_value embi_impl_ResizeObserverEntry_get_target(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "target");
}

embi_js_value embi_impl_ResizeObserverEntry_get_contentRect(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "contentRect");
}

embi_js_value embi_impl_ResizeObserverEntry_get_borderBoxSize(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "borderBoxSize");
}

embi_js_value embi_impl_ResizeObserverEntry_get_contentBoxSize(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "contentBoxSize");
}

embi_js_value embi_impl_ResizeObserverEntry_get_devicePixelContentBoxSize(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "devicePixelContentBoxSize");
}

/* ------------------------------------------------------------------ IntersectionObserver */

typedef struct io_item {
    embi_node *target;
    bool has_prev; /* a notification was made for this target before */
    bool prev_isi;
    uint32_t prev_ti;
} io_item;

/* A pending IntersectionObserverEntry (rectangles in client coordinates, as the entry reports them). */
typedef struct io_entry {
    embi_node *target;
    double time;
    double bcr[4]; /* boundingClientRect: x, y, w, h */
    double ir[4];  /* intersectionRect */
    double rb[4];  /* rootBounds */
    bool isi;
    double ratio;
} io_entry;

typedef struct embi_io {
    uint8_t kind; /* OBS_IO (first) */
    bool registered;
    uint32_t pin;
    uint64_t seq; /* creation order */
    embi_bind_obj *bo; /* ref: [callback, thresholds (sorted array of numbers), rootMargin (string)] */
    embi_node *root;   /* nullptr: the viewport */
    double margin[4];  /* top, right, bottom, left as written */
    bool margin_pct[4];
    double smargin[4]; /* the scrollMargin the same way */
    bool smargin_pct[4];
    io_item *items;
    uint32_t n, cap;
    io_entry *pend;
    uint32_t npend, cappend;
    uint32_t nthr;  /* the thresholds, parsed once (sorted) */
    double thr[];   /* nthr entries; the struct is allocated with them */
} embi_io;

static uint64_t obs_seq(const void *obs)
{
    return *(const uint8_t *)obs == OBS_RO ? ((const embi_ro *)obs)->seq : ((const embi_io *)obs)->seq;
}

static void io_register(embi_bind_ctx *c, embi_io *io)
{
    if (!io->registered) {
        io->registered = true;
        reg_add(c, io);
        io->pin = pin_value(c, embi_bind_obj_value(io->bo));
    }
}

/* Leaves the registry once nothing is observed and nothing is pending (pending entries keep it alive until they are
 * delivered or taken). */
static void io_maybe_release(embi_bind_ctx *c, embi_io *io)
{
    if (io->n != 0 || io->npend != 0) {
        return;
    }
    /* Nothing is observed or pending: the entry buffer goes too (it is allocated only while entries are queued). */
    if (io->pend) {
        embi_free(io->pend, (size_t)io->cappend * sizeof *io->pend);
        io->pend = nullptr;
        io->cappend = 0;
    }
    if (!io->registered) {
        return;
    }
    io->registered = false;
    reg_remove(c, io);
    unpin(c, &io->pin);
    if (io->items) {
        embi_free(io->items, (size_t)io->cap * sizeof *io->items);
    }
    io->items = nullptr;
    io->cap = 0;
    io->n = 0;
}

/* Called after observations or entries were dropped: the nodes they referred to are released when nothing else does. */
static void io_drop_refs(embi_bind_ctx *c)
{
    embi_mut_touch(c->doc);
    release_holds(c);
}

static embi_js_value io_rect(embi_bind_ctx *c, const double r[4])
{
    return embi_bind_pin(c, embi_impl_rect_new(c, r[0], r[1], r[2], r[3]));
}

static embi_js_value io_entry_value(embi_bind_ctx *c, const io_entry *e)
{
    embi_js_value f = new_fields(c);
    fset(c, f, "time", embi_js_from_double(e->time));
    fset(c, f, "rootBounds", io_rect(c, e->rb));
    fset(c, f, "boundingClientRect", io_rect(c, e->bcr));
    fset(c, f, "intersectionRect", io_rect(c, e->ir));
    fset(c, f, "isIntersecting", embi_js_from_bool(e->isi));
    fset(c, f, "intersectionRatio", embi_js_from_double(e->ratio));
    fset(c, f, "target", node_or_null(c, e->target));
    return wrap(c, EMBI_IF_IntersectionObserverEntry, f);
}

/* The index of the first threshold greater than ratio, or the count when there is none (the spec's thresholdIndex). */
static uint32_t thr_index(const embi_io *io, double ratio)
{
    for (uint32_t i = 0; i < io->nthr; i++) {
        if (io->thr[i] > ratio) {
            return i;
        }
    }
    return io->nthr;
}

/* Delivers the pending entries of every IntersectionObserver: one callback per observer. */
static void io_flush(embi_bind_ctx *c, obs_reg *reg)
{
    embi_bind_scope sc = embi_bind_enter(c);
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < reg->n; i++) {
        if (*(uint8_t *)reg->obs[i] == OBS_IO && ((embi_io *)reg->obs[i])->npend) {
            cnt++;
        }
    }
    if (cnt) {
        embi_js_value wr = embi_bind_pin(c, embi_bind_array_new(c, cnt));
        embi_js_value ev = embi_bind_pin(c, embi_bind_array_new(c, cnt));
        uint32_t k = 0;
        for (uint32_t i = 0; i < reg->n; i++) {
            if (*(uint8_t *)reg->obs[i] != OBS_IO) {
                continue;
            }
            embi_io *io = reg->obs[i];
            if (io->npend == 0) {
                continue;
            }
            embi_js_value list = embi_bind_pin(c, embi_bind_array_new(c, io->npend));
            for (uint32_t q = 0; q < io->npend; q++) {
                embi_bind_array_set(c, list, q, io_entry_value(c, &io->pend[q]));
            }
            embi_bind_array_set(c, wr, k, embi_bind_obj_value(io->bo));
            embi_bind_array_set(c, ev, k, list);
            k++;
            embi_free(io->pend, (size_t)io->cappend * sizeof *io->pend);
            io->pend = nullptr;
            io->npend = 0;
            io->cappend = 0;
        }
        for (uint32_t i = 0; i < cnt; i++) {
            embi_js_value obs = embi_bind_array_get(c, wr, i);
            embi_bind_obj *bo = embi_bind_obj_of(obs);
            embi_js_value cb = embi_bind_array_get(c, bo->ref, 0);
            embi_js_value args[2] = {embi_bind_array_get(c, ev, i), obs};
            embi_js_value res = embi_js_call(c->rt, cb, obs, 2, args);
            if (res == EMBI_JS_EXCEPTION) {
                embi_js_value err = embi_js_exception_get(c->rt);
                embi_js_exception_clear(c->rt);
                embi_bind_report_error(c, err, nullptr, 0, 0);
            }
        }
        for (uint32_t i = 0; i < cnt; i++) {
            embi_io *io = embi_bind_obj_of(embi_bind_array_get(c, wr, i))->ptr;
            io_maybe_release(c, io);
        }
    }
    io_drop_refs(c);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static embi_js_value io_task_fn(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    embi_bind_ctx *c = embi_js_native_data_current(r);
    obs_reg *reg = c ? reg_get(c, false) : nullptr;
    if (reg) {
        reg->io_task = false;
        io_flush(c, reg);
    }
    return EMBI_JS_UNDEFINED;
}

/* Delivers the pending entries as a task (the timer hooks of the context run it in the event loop). Without timer hooks
 * they are delivered now. */
static void io_schedule(embi_bind_ctx *c, obs_reg *reg)
{
    if (reg->io_task) {
        return;
    }
    if (c->timers.add) {
        embi_bind_scope sc = embi_bind_enter(c);
        embi_js_object *fn = embi_js_native_function_new_data(c->realm, "deliverIntersectionRecords", 0, io_task_fn,
                                                              0, c);
        embi_js_value rec = embi_bind_pin(c, embi_bind_array_new(c, 1));
        if (fn) {
            embi_bind_array_set(c, rec, 0, embi_js_from_object(fn));
        }
        uint32_t pid = embi_js_persistent_new(c->rt, rec);
        uint32_t id = c->timers.add(c->timers.user, 0.0, false, (void *)(uintptr_t)(pid + 1u));
        if (id) {
            reg->io_task = true;
            reg->io_task_id = id;
            (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            return;
        }
        embi_js_persistent_free(c->rt, pid);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    io_flush(c, reg);
}

/* Computes the intersection of every observed target with its root for this frame, and queues the entries of the targets
 * whose state changed (isIntersecting or the threshold index). */
static void io_update(embi_bind_ctx *c, obs_reg *reg)
{
    embi_bind_scope sc = embi_bind_enter(c);
    double dpr = 1.0;
    double vw = 0;
    double vh = 0;
    embi_bind_viewport(c, &vw, &vh, &dpr);
    double sx = 0.0;
    double sy = 0.0;
    embi_impl_root_offset(c, &sx, &sy);
    for (uint32_t i = 0; i < reg->n; i++) {
        if (*(uint8_t *)reg->obs[i] != OBS_IO) {
            continue;
        }
        embi_io *io = reg->obs[i];
        /* The root rectangle in document coordinates: the viewport, or the root element's border box. */
        double R[4] = {sx, sy, sx + vw, sy + vh};
        /* An element root without a box (display:none, or removed from the document) intersects nothing: an empty
         * root rectangle would still touch a target at the origin. */
        bool root_ok = true;
        if (io->root) {
            double rr[4] = {0, 0, 0, 0};
            if (border_rect(c, io->root, rr)) {
                SDL_memcpy(R, rr, sizeof R);
            } else {
                SDL_memset(R, 0, sizeof R);
                root_ok = false;
            }
        }
        double R0[4] = {R[0], R[1], R[2], R[3]};
        for (uint32_t j = 0; j < io->n; j++) {
            io_item *it = &io->items[j];
            if (!io->root && it->target->doc != c->doc) {
                continue; /* a target of another (detached) document is never intersecting and never notified */
            }
            double T[4] = {0, 0, 0, 0};
            bool has = border_rect(c, it->target, T);
            clipwalk w;
            SDL_memset(&w, 0, sizeof w);
            SDL_memcpy(w.rect, T, sizeof w.rect);
            if (has) {
                clip_walk(it->target, io->root, T, io->smargin, io->smargin_pct, &w);
            }
            /* The root rectangle: the viewport, or the root element's padding box when it clips (else its border
             * box), with the root margin added. */
            if (io->root && has && w.root_found) {
                SDL_memcpy(R, w.root_rect, sizeof R);
            } else {
                SDL_memcpy(R, R0, sizeof R);
            }
            double rw = R[2] - R[0];
            double rh = R[3] - R[1];
            double top = io->margin_pct[0] ? io->margin[0] * rh / 100.0 : io->margin[0];
            double right = io->margin_pct[1] ? io->margin[1] * rw / 100.0 : io->margin[1];
            double bottom = io->margin_pct[2] ? io->margin[2] * rh / 100.0 : io->margin[2];
            double left = io->margin_pct[3] ? io->margin[3] * rw / 100.0 : io->margin[3];
            R[0] -= left;
            R[1] -= top;
            R[2] += right;
            R[3] += bottom;
            double ox = sx + (io->root ? w.shift_up[0] : 0.0);
            double oy = sy + (io->root ? w.shift_up[1] : 0.0);
            const double *I = w.rect;
            double ix0 = I[0] > R[0] ? I[0] : R[0];
            double iy0 = I[1] > R[1] ? I[1] : R[1];
            double ix1 = I[2] < R[2] ? I[2] : R[2];
            double iy1 = I[3] < R[3] ? I[3] : R[3];
            bool isi = has && root_ok && !w.empty && (!io->root || w.root_found) && ix0 <= ix1 && iy0 <= iy1;
            double ratio = 0;
            if (isi) {
                double tarea = (T[2] - T[0]) * (T[3] - T[1]);
                if (tarea > 0) {
                    ratio = (ix1 - ix0) * (iy1 - iy0) / tarea;
                    if (ratio > 1 - 1e-6) {
                        ratio = 1; /* the rectangles are single precision floats */
                    }
                } else {
                    ratio = 1;
                }
            }
            uint32_t ti = thr_index(io, ratio);
            bool notify = !it->has_prev || isi != it->prev_isi || ti != it->prev_ti;
            it->has_prev = true;
            it->prev_isi = isi;
            it->prev_ti = ti;
            if (!notify) {
                continue;
            }
            if (io->npend == io->cappend) {
                uint32_t nc = io->cappend ? io->cappend * 2u : 4u;
                io->pend = embi_realloc(io->pend, (size_t)io->cappend * sizeof *io->pend,
                                        (size_t)nc * sizeof *io->pend);
                io->cappend = nc;
            }
            io_entry *e = &io->pend[io->npend++];
            e->target = it->target;
            e->time = embi_bind_now_ms(c);
            e->bcr[0] = has ? T[0] - sx - w.shift_total[0] : 0;
            e->bcr[1] = has ? T[1] - sy - w.shift_total[1] : 0;
            e->bcr[2] = has ? T[2] - T[0] : 0;
            e->bcr[3] = has ? T[3] - T[1] : 0;
            e->rb[0] = has ? R[0] - ox : 0;
            e->rb[1] = has ? R[1] - oy : 0;
            e->rb[2] = has ? R[2] - R[0] : 0;
            e->rb[3] = has ? R[3] - R[1] : 0;
            e->isi = isi;
            e->ratio = ratio;
            if (isi) {
                e->ir[0] = ix0 - ox;
                e->ir[1] = iy0 - oy;
                e->ir[2] = ix1 - ix0;
                e->ir[3] = iy1 - iy0;
            } else {
                e->ir[0] = 0;
                e->ir[1] = 0;
                e->ir[2] = 0;
                e->ir[3] = 0;
            }
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    bool pending = false;
    for (uint32_t i = 0; i < reg->n; i++) {
        if (*(uint8_t *)reg->obs[i] == OBS_IO && ((embi_io *)reg->obs[i])->npend) {
            pending = true;
            break;
        }
    }
    if (pending) {
        io_schedule(c, reg);
    }
}

/* Parses one rootMargin length: a number with px or %. */
static bool parse_margin_len(embi_str t, double *v, bool *pct)
{
    if (t.len == 0) {
        return false;
    }
    uint32_t i = 0;
    bool neg = false;
    if (t.ptr[i] == '+' || t.ptr[i] == '-') {
        neg = t.ptr[i] == '-';
        i++;
    }
    double val = 0;
    bool digits = false;
    while (i < t.len && t.ptr[i] >= '0' && t.ptr[i] <= '9') {
        val = val * 10 + (t.ptr[i] - '0');
        digits = true;
        i++;
    }
    if (i < t.len && t.ptr[i] == '.') {
        double scale = 0.1;
        i++;
        while (i < t.len && t.ptr[i] >= '0' && t.ptr[i] <= '9') {
            val += (t.ptr[i] - '0') * scale;
            scale *= 0.1;
            digits = true;
            i++;
        }
    }
    if (!digits) {
        return false;
    }
    if (i + 2 == t.len && t.ptr[i] == 'p' && t.ptr[i + 1] == 'x') {
        *pct = false;
    } else if (i + 1 == t.len && t.ptr[i] == '%') {
        *pct = true;
    } else {
        /* the other absolute units, converted to px and truncated, as the serialization shows them */
        char u[3] = {0, 0, 0};
        uint32_t ul = t.len - i;
        if (ul == 0 || ul > 2) {
            return false;
        }
        for (uint32_t k = 0; k < ul; k++) {
            char ch = t.ptr[i + k];
            u[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
        }
        double f;
        if (ul == 2 && u[0] == 'c' && u[1] == 'm') {
            f = 96.0 / 2.54;
        } else if (ul == 2 && u[0] == 'm' && u[1] == 'm') {
            f = 96.0 / 25.4;
        } else if (ul == 1 && u[0] == 'q') {
            f = 96.0 / 101.6;
        } else if (ul == 2 && u[0] == 'i' && u[1] == 'n') {
            f = 96.0;
        } else if (ul == 2 && u[0] == 'p' && u[1] == 't') {
            f = 96.0 / 72.0;
        } else if (ul == 2 && u[0] == 'p' && u[1] == 'c') {
            f = 16.0;
        } else {
            return false;
        }
        val = SDL_trunc(val * f + 1e-9);
        *pct = false;
    }
    *v = neg ? -val : val;
    return true;
}

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* Parses the rootMargin text into the four margins of io (top, right, bottom, left) and writes its serialized form
 * (four values) into out. */
static bool parse_root_margin(embi_str s, double margin[4], bool margin_pct[4], char out[160], uint32_t *outlen)
{
    embi_str tok[4];
    uint32_t nt = 0;
    uint32_t i = 0;
    while (i < s.len) {
        while (i < s.len && is_space(s.ptr[i])) {
            i++;
        }
        if (i >= s.len) {
            break;
        }
        uint32_t b = i;
        while (i < s.len && !is_space(s.ptr[i])) {
            i++;
        }
        if (nt == 4) {
            return false;
        }
        tok[nt++] = (embi_str){s.ptr + b, i - b};
    }
    if (nt == 0) {
        /* an empty rootMargin is the default, 0px */
        tok[nt++] = cstr("0px");
    }
    /* CSS expansion: one value for all sides, two for vertical and horizontal, three for top, sides, bottom, four in
     * order. */
    int idx[4];
    if (nt == 1) {
        idx[0] = idx[1] = idx[2] = idx[3] = 0;
    } else if (nt == 2) {
        idx[0] = idx[2] = 0;
        idx[1] = idx[3] = 1;
    } else if (nt == 3) {
        idx[0] = 0;
        idx[1] = idx[3] = 1;
        idx[2] = 2;
    } else {
        idx[0] = 0;
        idx[1] = 1;
        idx[2] = 2;
        idx[3] = 3;
    }
    double v[4];
    bool p[4];
    for (int k = 0; k < 4; k++) {
        if (!parse_margin_len(tok[idx[k]], &v[k], &p[k])) {
            return false;
        }
        margin[k] = v[k];
        margin_pct[k] = p[k];
    }
    uint32_t len = 0;
    for (int k = 0; k < 4; k++) {
        char part[40];
        int m = SDL_snprintf(part, sizeof part, "%s%g%s", k ? " " : "", v[k], p[k] ? "%" : "px");
        if (m < 0 || len + (uint32_t)m >= 160) {
            return false;
        }
        SDL_memcpy(out + len, part, (size_t)m);
        len += (uint32_t)m;
    }
    out[len] = '\0';
    *outlen = len;
    return true;
}

/* Reads the threshold member (a number, or a sequence of numbers in [0, 1]) into a new sorted array (*out, *nv entries;
 * the caller frees it). Returns false with an exception pending. */
static bool parse_thresholds(embi_bind_ctx *c, embi_js_value tv, double **out, uint32_t *nv)
{
    bool is_list = embi_js_is_object(tv) && embi_js_is_array(tv);
    uint32_t len = 0;
    if (is_list) {
        len = embi_bind_array_len(c, tv);
    } else if (!embi_js_is_undefined(tv)) {
        len = 1;
    }
    uint32_t cap = len ? len : 1u;
    double *vals = embi_alloc((size_t)cap * sizeof *vals);
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        double d = 0;
        embi_js_value item = is_list ? embi_bind_array_get(c, tv, i) : tv;
        if (!embi_js_to_number(c->rt, item, &d)) {
            embi_free(vals, (size_t)cap * sizeof *vals);
            return false;
        }
        if (SDL_isnan(d) || SDL_isinf(d)) {
            embi_free(vals, (size_t)cap * sizeof *vals);
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'IntersectionObserver': The provided double value is non-finite.");
            return false;
        }
        if (!(d >= 0 && d <= 1)) {
            embi_free(vals, (size_t)cap * sizeof *vals);
            (void)embi_js_throw_range_error(c->rt, "Failed to construct 'IntersectionObserver': Threshold values must be between 0 and 1.");
            return false;
        }
        vals[n++] = d;
    }
    if (n == 0) {
        vals[n++] = 0.0;
    }
    for (uint32_t i = 1; i < n; i++) {
        double x = vals[i];
        uint32_t j = i;
        while (j > 0 && vals[j - 1] > x) {
            vals[j] = vals[j - 1];
            j--;
        }
        vals[j] = x;
    }
    *out = vals;
    *nv = n;
    return true;
}

/* Reads the rootMargin and threshold members of an IntersectionObserverInit and builds the observer's state. Returns the
 * state with the wrapper's ref array set, or nullptr with the exception pending. */
static embi_js_value io_construct(embi_bind_ctx *c, embi_js_value a0, embi_js_value a1, embi_js_value new_target)
{
    embi_io *io = embi_calloc(1, sizeof *io);
    io->kind = OBS_IO;
    io->seq = ++g_obs_seq;
    /* root: null or undefined is the viewport; otherwise an Element. */
    embi_js_value rootv = fget(c, a1, "root");
    if (!embi_js_is_nullish(rootv)) {
        embi_bind_obj *rb = embi_bind_obj_of(rootv);
        if (!rb || !rb->is_node || !rb->ptr || !embi_is_element(rb->ptr)) {
            embi_free(io, sizeof *io);
            (void)embi_js_throw_type_error(c->rt, "Failed to construct 'IntersectionObserver': Failed to read the 'root' property from 'IntersectionObserverInit': member is not of type Element.");
            return EMBI_JS_EXCEPTION;
        }
        io->root = rb->ptr;
    }
    /* rootMargin: default "0px". */
    char mtext[160];
    uint32_t mlen = 0;
    embi_js_value mv = fget(c, a1, "rootMargin");
    if (embi_js_is_undefined(mv)) {
        (void)parse_root_margin(cstr("0px"), io->margin, io->margin_pct, mtext, &mlen);
    } else {
        embi_dstr md;
        if (!embi_bind_to_dstr(c, mv, &md)) {
            embi_free(io, sizeof *io);
            return EMBI_JS_EXCEPTION;
        }
        if (!parse_root_margin(embi_bind_utf8(c, md), io->margin, io->margin_pct, mtext, &mlen)) {
            embi_free(io, sizeof *io);
            (void)embi_bind_throw_named(c, "SyntaxError", "Failed to construct 'IntersectionObserver': rootMargin must be specified in pixels or percent.");
            return EMBI_JS_EXCEPTION;
        }
    }
    /* scrollMargin: the same grammar, default "0px". */
    char smtext[160];
    uint32_t smlen = 0;
    embi_js_value smv = fget(c, a1, "scrollMargin");
    if (embi_js_is_undefined(smv)) {
        (void)parse_root_margin(cstr("0px"), io->smargin, io->smargin_pct, smtext, &smlen);
    } else {
        embi_dstr md;
        if (!embi_bind_to_dstr(c, smv, &md)) {
            embi_free(io, sizeof *io);
            return EMBI_JS_EXCEPTION;
        }
        if (!parse_root_margin(embi_bind_utf8(c, md), io->smargin, io->smargin_pct, smtext, &smlen)) {
            embi_free(io, sizeof *io);
            (void)embi_bind_throw_named(c, "SyntaxError", "Failed to construct 'IntersectionObserver': scrollMargin must be specified in pixels or percent.");
            return EMBI_JS_EXCEPTION;
        }
    }
    /* threshold: a number or a list of numbers in [0, 1], sorted; default [0]. */
    embi_js_value tv = fget(c, a1, "threshold");
    double *vals = nullptr;
    uint32_t nv = 0;
    if (!parse_thresholds(c, tv, &vals, &nv)) {
        embi_free(io, sizeof *io);
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value thr = embi_bind_pin(c, embi_bind_array_new(c, nv));
    for (uint32_t i = 0; i < nv; i++) {
        embi_bind_array_set(c, thr, i, embi_js_from_double(vals[i]));
    }
    /* Move the state into a block that also holds the parsed thresholds. */
    size_t iosz = sizeof *io + (size_t)nv * sizeof(double);
    embi_io *io2 = embi_calloc(1, iosz);
    SDL_memcpy(io2, io, sizeof *io);
    io2->nthr = nv;
    SDL_memcpy(io2->thr, vals, (size_t)nv * sizeof(double));
    embi_free(io, sizeof *io);
    io = io2;
    embi_free(vals, (size_t)nv * sizeof *vals);
    /* The ref array: callback, thresholds, rootMargin, and the root element (or null). Keeping the root's value here keeps
     * its wrapper alive, and a node with a wrapper is never freed while detached, so the observer's root stays valid. */
    (void)embi_js_set_integrity(c->rt, embi_js_as_object(thr), true); /* thresholds is a FrozenArray */
    embi_js_value refs = embi_bind_pin(c, embi_bind_array_new(c, 5));
    embi_bind_array_set(c, refs, 0, a0);
    embi_bind_array_set(c, refs, 1, thr);
    embi_bind_array_set(c, refs, 2, embi_bind_pin(c, embi_bind_utf8_value(c, (embi_str){mtext, mlen})));
    embi_bind_array_set(c, refs, 3, embi_js_is_nullish(rootv) ? EMBI_JS_NULL : rootv);
    embi_bind_array_set(c, refs, 4, embi_bind_pin(c, embi_bind_utf8_value(c, (embi_str){smtext, smlen})));
    embi_js_value v = embi_bind_pin(c, embi_bind_other_value(c, EMBI_IF_IntersectionObserver, io, io, (uint32_t)iosz));
    embi_bind_obj *bo = embi_bind_obj_of(v);
    embi_bind_set_ref(bo, refs);
    io->bo = bo;
    embi_bind_adopt_new_target(c, v, new_target);
    return v;
}

embi_js_value embi_impl_IntersectionObserver_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0,
                                                         embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    if (!embi_js_is_callable(a0)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'IntersectionObserver': The callback provided as parameter 1 is not a function.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_js_value v = io_construct(c, a0, a1, new_target);
    return embi_bind_leave(sc, v);
}

void embi_impl_IntersectionObserver_observe(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0)
{
    embi_io *io = self->ptr;
    for (uint32_t i = 0; i < io->n; i++) {
        if (io->items[i].target == a0) {
            return;
        }
    }
    if (io->n == io->cap) {
        uint32_t nc = io->cap ? io->cap * 2u : 4u;
        io->items = embi_realloc(io->items, (size_t)io->cap * sizeof *io->items, (size_t)nc * sizeof *io->items);
        io->cap = nc;
    }
    io_item *it = &io->items[io->n++];
    SDL_memset(it, 0, sizeof *it);
    it->target = a0;
    embi_mut_hold(a0);
    io_register(c, io);
}

void embi_impl_IntersectionObserver_unobserve(embi_bind_ctx *c, embi_bind_obj *self, embi_node *a0)
{
    embi_io *io = self->ptr;
    for (uint32_t i = 0; i < io->n; i++) {
        if (io->items[i].target == a0) {
            SDL_memmove(&io->items[i], &io->items[i + 1u], (size_t)(io->n - i - 1u) * sizeof *io->items);
            io->n--;
            break;
        }
    }
    /* The queued entries of the target go with it (the entries keep their order). */
    uint32_t w = 0;
    for (uint32_t i = 0; i < io->npend; i++) {
        if (io->pend[i].target != a0) {
            io->pend[w++] = io->pend[i];
        }
    }
    io->npend = w;
    io_maybe_release(c, io);
    io_drop_refs(c);
}

void embi_impl_IntersectionObserver_disconnect(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_io *io = self->ptr;
    io->n = 0;
    io->npend = 0; /* like unobserve: the queued entries go with the observations */
    io_maybe_release(c, io);
    io_drop_refs(c);
}

embi_js_value embi_impl_IntersectionObserver_takeRecords(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_io *io = self->ptr;
    embi_js_value arr = embi_bind_pin(c, embi_bind_array_new(c, io->npend));
    for (uint32_t i = 0; i < io->npend; i++) {
        embi_bind_array_set(c, arr, i, io_entry_value(c, &io->pend[i]));
    }
    if (io->pend) {
        embi_free(io->pend, (size_t)io->cappend * sizeof *io->pend);
    }
    io->pend = nullptr;
    io->npend = 0;
    io->cappend = 0;
    io_maybe_release(c, io);
    io_drop_refs(c);
    return embi_bind_leave(sc, arr);
}

embi_js_value embi_impl_IntersectionObserver_get_root(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_io *io = self->ptr;
    return node_or_null(c, io->root);
}

embi_dstr embi_impl_IntersectionObserver_get_rootMargin(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_dstr d = embi_dstr_empty();
    (void)embi_bind_to_dstr(c, embi_bind_array_get(c, self->ref, 2), &d);
    return d;
}

embi_dstr embi_impl_IntersectionObserver_get_scrollMargin(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_dstr d = embi_dstr_empty();
    (void)embi_bind_to_dstr(c, embi_bind_array_get(c, self->ref, 4), &d);
    return d;
}

embi_js_value embi_impl_IntersectionObserver_get_thresholds(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_array_get(c, self->ref, 1);
}

embi_js_value embi_impl_IntersectionObserverEntry_get_time(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "time");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_rootBounds(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "rootBounds");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_boundingClientRect(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "boundingClientRect");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_intersectionRect(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "intersectionRect");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_isIntersecting(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "isIntersecting");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_intersectionRatio(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "intersectionRatio");
}

embi_js_value embi_impl_IntersectionObserverEntry_get_target(embi_bind_ctx *c, embi_bind_obj *self)
{
    return fld(c, self, "target");
}

/* ------------------------------------------------------------------ the frame step (observers.h) */

static bool mo_of_ctx(embi_mo *mo, void *ctx)
{
    const embi_bind_obj *bo = mo->user;
    if (bo && bo->ctx == ctx) {
        unpin(ctx, &mo->pin);
        return true;
    }
    return false;
}

/* Reports the references of the ResizeObserver and IntersectionObserver observations to embi_mut_release: their targets,
 * and the targets of the queued IntersectionObserver entries. */
static void refs_obs(void *ctx, embi_mut_mark_fn mark, void *mctx)
{
    obs_reg *reg = reg_get(ctx, false);
    if (!reg) {
        return;
    }
    for (uint32_t i = 0; i < reg->n; i++) {
        if (*(uint8_t *)reg->obs[i] == OBS_RO) {
            const embi_ro *ro = reg->obs[i];
            for (uint32_t j = 0; j < ro->n; j++) {
                mark(mctx, ro->items[j].target);
            }
        } else {
            const embi_io *io = reg->obs[i];
            for (uint32_t j = 0; j < io->n; j++) {
                mark(mctx, io->items[j].target);
            }
            for (uint32_t j = 0; j < io->npend; j++) {
                mark(mctx, io->pend[j].target);
            }
        }
    }
}

static void release_holds(embi_bind_ctx *c)
{
    embi_mut_release(c->doc, refs_obs, c);
}

void embi_observers_release(embi_bind_ctx *c)
{
    embi_mut_detach_if(c->doc, mo_of_ctx, c);
    embi_mut_set_signal(c->doc, nullptr, nullptr);
    /* Resize and intersection observers still observing: their arrays are freed here, since the wrapper's finalizer
     * frees the struct only. Each one leaves the registry, so the loop shrinks it. */
    obs_reg *reg = reg_get(c, false);
    while (reg && reg->n) {
        uint32_t before = reg->n;
        void *o = reg->obs[reg->n - 1];
        if (*(uint8_t *)o == OBS_IO) {
            embi_io *io = o;
            io->n = 0;
            io->npend = 0;
            if (io->pend) {
                embi_free(io->pend, (size_t)io->cappend * sizeof *io->pend);
            }
            io->pend = nullptr;
            io->cappend = 0;
            io_maybe_release(c, io);
        } else {
            ro_release(c, o);
        }
        if (reg->n == before) {
            reg_remove(c, o);
        }
    }
    embi_mut_touch(c->doc);
    release_holds(c);
}

void embi_observers_after_layout(embi_bind_ctx *c)
{
    obs_reg *reg = reg_get(c, false);
    if (!reg || reg->n == 0) {
        return;
    }
    /* A delivery task still marked as scheduled at the start of a frame did not run in the pump before it: its timer was
     * dropped (the loop released the payload), so it is scheduled again below when entries are pending. A late task that
     * fires anyway finds nothing to deliver or delivers entries it was meant for. */
    if (reg->io_task && c->timers.clear) {
        /* Cancel and re-add the timer: when the loop no longer knows it (it dropped it), clear finds nothing. */
        void *payload = c->timers.clear(c->timers.user, reg->io_task_id);
        if (payload) {
            uint32_t id = c->timers.add ? c->timers.add(c->timers.user, 0.0, false, payload) : 0;
            if (id) {
                reg->io_task_id = id;
            } else {
                embi_bind_release_timer(c, payload);
                reg->io_task = false;
            }
        } else {
            reg->io_task = false;
        }
    } else if (!c->timers.clear) {
        reg->io_task = false;
    }
    ro_loop(c, reg);
    io_update(c, reg);
}
