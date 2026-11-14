/*
 * Style resolution over the tree (W2d): the walk, style sharing, pseudo-element styles, and the
 * accessors used by layout and tests.
 *
 * Sharing. The cache is keyed by the element's identity (tag, namespace, id, classes and every
 * attribute), its parent's style and the identities of its ancestors. Those inputs decide every
 * match of a rule that is position-independent (embi_style_sel_unsafe is false), and the candidate
 * set is itself a function of the identity, so an entry is stored only when every candidate
 * gathered for its element was position-independent. A hit returns the cached style without
 * matching or cascading. Each entry holds one reference on its style. The whole cache is cleared
 * whenever an input outside the key changes (sheets, environment, root font size, detachment).
 *
 * The walk is iterative: the tree depth is limited by the heap, not by the C stack.
 */
#include "style_int.h"
#include "inval.h"

#include <stdatomic.h>

#include <string.h>

/* The per-document state of the engine, or nullptr when the document is not attached. */
embi_docstate *embi_style_ds_find(embi_style_engine *e, embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(&e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return (embi_docstate *)(uintptr_t)v;
    }
    return nullptr;
}

uint64_t embi_style_mix(uint64_t h, uint64_t v)
{
    return embi_hash64(h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)));
}

/* The identity of an element for style sharing. */
uint64_t embi_style_identity_hash(embi_element *el)
{
    uint64_t h = embi_style_mix(0x1d3u, el->base.name);
    h = embi_style_mix(h, el->base.extra);
    h = embi_style_mix(h, el->prefix);
    h = embi_style_mix(h, embi_el_id(el));
    uint32_t nc = 0;
    const embi_atom *cls = embi_el_classes(el, &nc);
    for (uint32_t i = 0; i < nc; i++) {
        h = embi_style_mix(h, cls[i]);
    }
    uint32_t na = embi_el_attr_count(el);
    for (uint32_t i = 0; i < na; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        h = embi_style_mix(h, a->name);
        h = embi_style_mix(h, a->prefix);
        h = embi_style_mix(h, a->ns_uri);
        h = embi_style_mix(h, a->ns);
        h = embi_style_mix(h, embi_dstr_hash(a->value));
    }
    return h;
}

/* ------------------------------------------------------------------ sctx setup */

static const embi_sset *empty_set(void)
{
    static const embi_sset empty;
    return &empty;
}

void embi_style_sc_setup(embi_style_engine *e, const embi_docstate *ds, const embi_style *parent, bool is_root)
{
    embi_sctx *sc = &e->sc;
    sc->eng = e;
    sc->parent = parent;
    sc->init = e->init;
    sc->pcustom = parent != nullptr ? parent->custom : nullptr;
    sc->ccustom = nullptr;
    sc->custom_ready = false;
    sc->root_px = ds != nullptr && ds->root_px > 0 ? ds->root_px : 16.0;
    sc->is_root = is_root;
    sc->ncands = 0;
    sc->ncw = 0;
    sc->unsafe = false;
    sc->nsets = 3;
    embi_style_ua_ensure(e);
    sc->sets[0] = &e->ua;
    sc->sets[1] = &e->user;
    sc->sets[2] = ds != nullptr ? &ds->author : empty_set();
}

/* Gathers every candidate of an element for the given pseudo-element (EMBI_PE_NONE for the
 * element itself). */
void embi_style_gather_all(embi_style_engine *e, embi_sctx *sc, embi_element *el, embi_sel_pseudo pe,
                           const embi_bloom *anc)
{
    sc->el = el;
    for (uint32_t s = 0; s < sc->nsets; s++) {
        if (sc->sets[s]->n == 0) {
            continue;
        }
        if (pe != EMBI_PE_NONE && (sc->sets[s]->pe_mask & (1u << pe)) == 0) {
            continue;
        }
        embi_style_gather(sc, sc->sets[s], el, pe, anc, &e->selctx);
    }
}

/* ------------------------------------------------------------------ element styles */

void embi_style_mark_dirty(embi_style_engine *e, embi_document *doc)
{
    embi_docstate *ds = doc != nullptr ? embi_style_ds_find(e, doc) : nullptr;
    if (ds != nullptr) {
        ds->dirty = true;
        ds->full = true; /* W6b: a change the engine cannot see restyles every element */
    }
}

/* Whether the element answers a user-action state (hover, active, focus, focus-visible, focus-within) through the selector
 * state hook: it is hovered, pressed or focused, or contains one of those. The sharing key does not carry the state, so such
 * an element is neither looked up in the sharing cache nor stored in it. Siblings share a key only under one parent, so
 * they see the same ancestors' states and the cache stays sound for everything else (W5d2). */
static bool state_relevant(embi_style_engine *e, embi_element *el)
{
    if (e->hooks.state == nullptr) {
        return false;
    }
    if (e->state_idle != nullptr && e->state_idle(e->hooks.user)) {
        return false;
    }
    return e->hooks.state(e->hooks.user, el, EMBI_PC_HOVER) || e->hooks.state(e->hooks.user, el, EMBI_PC_ACTIVE) ||
           e->hooks.state(e->hooks.user, el, EMBI_PC_FOCUS) || e->hooks.state(e->hooks.user, el, EMBI_PC_FOCUS_VISIBLE) ||
           e->hooks.state(e->hooks.user, el, EMBI_PC_FOCUS_WITHIN);
}

static void el_set_style(embi_style_engine *e, embi_element *el, const embi_style *owned)
{
    const embi_style *old = el->style;
    el->style = (void *)owned;
    embi_style_unref(e, old);
}

static void pseudo_store(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                         embi_sel_pseudo pe, const embi_style *owned)
{
    uint64_t key = ((uint64_t)(uintptr_t)el << 4) | pe;
    uint64_t old = 0;
    if (embi_hashmap_get(&ds->pseudo, key, &old)) {
        embi_style_unref(e, (const embi_style *)(uintptr_t)old);
    }
    embi_hashmap_put(&ds->pseudo, key, (uint64_t)(uintptr_t)owned);
}

/* The style of a generated pseudo-element of el, or nullptr when it is not generated. The bloom
 * must hold el's ancestors only. Returns an owned style. */
static const embi_style *compute_pseudo(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                                        const embi_style *el_style, embi_sel_pseudo pe)
{
    embi_style_sc_setup(e, ds, el_style, false);
    embi_sctx *sc = &e->sc;
    embi_style_gather_all(e, sc, el, pe, &e->bloom);
    if (sc->ncands == 0) {
        return nullptr;
    }
    embi_style *s = embi_style_cascade_finish(e, sc);
    if (pe == EMBI_PE_BEFORE || pe == EMBI_PE_AFTER) {
        const embi_value *content = embi_style_value(s, EMBI_PROP_content);
        bool none = content == nullptr ||
                    (content->kind == EMBI_V_KEYWORD &&
                     (content->kw == EMBI_KW_none || content->kw == EMBI_KW_normal));
        if (none) {
            embi_style_unref(e, s);
            return nullptr;
        }
    }
    return s;
}

static void generate_pseudos(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                             const embi_style *style)
{
    static const embi_sel_pseudo gen_list[] = {EMBI_PE_BEFORE, EMBI_PE_AFTER, EMBI_PE_MARKER, EMBI_PE_FIRST_LETTER,
                                                  EMBI_PE_FIRST_LINE};
    embi_style_ua_ensure(e);
    uint32_t any = e->ua.pe_mask | e->user.pe_mask | ds->author.pe_mask;
    /* With no pseudo-element style stored in this pass, there is no old entry to replace. */
    bool stored = embi_hashmap_len(&ds->pseudo) > 0;
    for (uint32_t i = 0; i < sizeof gen_list / sizeof gen_list[0]; i++) {
        embi_sel_pseudo pe = gen_list[i];
        uint64_t key = ((uint64_t)(uintptr_t)el << 4) | pe;
        uint64_t old = 0;
        bool had = stored && embi_hashmap_get(&ds->pseudo, key, &old);
        const embi_style *ps = nullptr;
        if ((any & (1u << pe)) != 0) {
            bool gen = true;
            if (pe == EMBI_PE_MARKER) {
                const embi_value *disp = embi_style_value(style, EMBI_PROP_display);
                gen = disp != nullptr && disp->kind == EMBI_V_KEYWORD &&
                      (disp->kw == EMBI_KW_list_item || disp->kw == EMBI_KW__internal_inline_list_item ||
                       disp->kw == EMBI_KW__internal_flow_root_list_item ||
                       disp->kw == EMBI_KW__internal_inline_flow_root_list_item);
            }
            if (gen) {
                ps = compute_pseudo(e, ds, el, style, pe);
            }
        }
        /* W6b: a pseudo-element style that is not the same object as before is a change (boxes hold the pointer). The
         * verifying pass compares by content. */
        if (had || ps != nullptr) {
            const embi_style *o = had ? (const embi_style *)(uintptr_t)old : nullptr;
            if (o != ps) {
                if (ds->verifying) {
                    ds->pseudo_changed = ds->pseudo_changed || o == nullptr || ps == nullptr || embi_style_diff(o, ps) != 0;
                } else {
                    ds->pseudo_changed = true;
                    /* CF1: a ::before, ::after or ::marker that stays generated and whose layout properties stay is a
                     * paint change (its boxes take the new style in place); anything else is a layout change */
                    uint8_t k = EMBI_SCH_LAYOUT;
                    if (o != nullptr && ps != nullptr &&
                        (pe == EMBI_PE_BEFORE || pe == EMBI_PE_AFTER || pe == EMBI_PE_MARKER)) {
                        k = embi_style_classify(o, ps);
                        k = k < EMBI_SCH_REFRESH ? (uint8_t)EMBI_SCH_REFRESH : k;
                    }
                    ds->pseudo_cls = k > ds->pseudo_cls ? k : ds->pseudo_cls;
                }
            }
        }
        if (had) {
            if (ds->track && !ds->verifying && (const embi_style *)(uintptr_t)old != ps) {
                /* layout caches may be keyed by the old pseudo style's address: it joins the grave */
                embi_vec_push(&ds->grave, (const embi_style *)(uintptr_t)old);
            } else {
                embi_style_unref(e, (const embi_style *)(uintptr_t)old);
            }
            embi_hashmap_remove(&ds->pseudo, key);
        }
        if (ps != nullptr) {
            pseudo_store(e, ds, el, pe, ps);
        }
    }
}

/* Computes the style of one element (the bloom holds its ancestors) and stores it in the element's
 * style slot. anc is the ancestor identity chain. Returns the borrowed style (owned by the slot). */
const embi_style *embi_style_enter_element(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                                       const embi_style *parent, bool is_root, uint64_t anc,
                                       uint64_t ident)
{
    uint64_t key = embi_style_mix(embi_style_mix(ident, (uint64_t)(uintptr_t)parent), anc);
    uint32_t slot = (uint32_t)(key & (EMBI_STYLE_CACHE_SLOTS - 1));
    const embi_style *style = nullptr;
    bool relevant = !is_root && state_relevant(e, el);

    if (!is_root && !relevant && e->cache[slot].s != nullptr && e->cache[slot].key == key) {
        style = e->cache[slot].s;
        embi_style_ref(style);
        e->cache_hits++;
    } else {
        if (!is_root) {
            e->cache_misses++;
        }
        embi_style_sc_setup(e, ds, parent, is_root);
        embi_sctx *sc = &e->sc;
        embi_style_gather_all(e, sc, el, EMBI_PE_NONE, &e->bloom);
        embi_style_gather_hints(e, sc, el);
        embi_style_gather_attr(e, sc, el);
        embi_style *owned = embi_style_cascade_finish(e, sc);
        style = owned;
        if (!sc->unsafe && !is_root && !relevant) {
            /* The slot takes its own reference. */
            if (e->cache[slot].s != nullptr) {
                embi_style_unref(e, e->cache[slot].s);
            }
            embi_style_ref(owned);
            e->cache[slot].key = key;
            e->cache[slot].s = owned;
        }
    }
    ds->restyled++;
    if (embi_arena_debug_poison_on()) {
        /* WPTASAN test hook: the styles this restyle reads (the old slot, the parent) must not point into memory given back */
        uint32_t k = embi_style_debug_poisoned((const embi_style *)el->style) + embi_style_debug_poisoned(parent);
        e->debug_poison_reads += k;
        embi_arena_debug_poison_note(k);
    }
    if (e->anim.apply != nullptr) {
        /* W6c: the animations overlay their values on the base style; the children and pseudo-elements see the result. */
        style = e->anim.apply(e->anim.user, e, el, (const embi_style *)el->style, style,
                              is_root ? e->init : parent, is_root);
    }
    embi_style_note_change(ds, el, (const embi_style *)el->style, style);
    el_set_style(e, el, style);
    ds->pseudo_changed = false;
    ds->pseudo_cls = 0;
    generate_pseudos(e, ds, el, style);
    if (ds->pseudo_changed) {
        embi_style_note_pseudo(ds, el, ds->pseudo_cls);
    }
    return style;
}

static void clear_pseudos(embi_style_engine *e, embi_docstate *ds)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&ds->pseudo);
    uint64_t k = 0;
    uint64_t v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_style_unref(e, (const embi_style *)(uintptr_t)v);
    }
    embi_hashmap_clear(&ds->pseudo);
}

typedef struct resolve_frame {
    embi_element *el;
    const embi_style *style;
    uint64_t child_anc;   /* the ancestor chain of this element's children */
    embi_node *next;      /* the next child to visit */
} resolve_frame;

/* Resolves the subtree of one top-level element with an explicit, heap-allocated stack. */
static void resolve_subtree(embi_style_engine *e, embi_docstate *ds, embi_element *root)
{
    uint32_t cap = 64u;
    uint32_t n = 0;
    resolve_frame *st = embi_alloc(sizeof(resolve_frame) * cap);
    uint64_t ident = embi_style_identity_hash(root);
    const embi_style *rs = embi_style_enter_element(e, ds, root, e->init, true, 0, ident);
    embi_bloom_push(&e->bloom, root);
    st[n].el = root;
    st[n].style = rs;
    st[n].child_anc = embi_style_mix(0, ident);
    st[n].next = root->base.first_child;
    n++;
    while (n > 0) {
        resolve_frame *top = &st[n - 1];
        embi_node *c = top->next;
        while (c != nullptr && c->type != EMBI_NODE_ELEMENT) {
            c = c->next;
        }
        if (c == nullptr) {
            embi_bloom_pop(&e->bloom, top->el);
            n--;
            continue;
        }
        top->next = c->next;
        embi_element *ce = (embi_element *)c;
        const embi_style *parent = top->style;
        uint64_t anc = top->child_anc;
        uint64_t cident = embi_style_identity_hash(ce);
        const embi_style *cs = embi_style_enter_element(e, ds, ce, parent, false, anc, cident);
        if (n == cap) {
            uint32_t ncap = cap * 2u;
            st = embi_realloc(st, sizeof(resolve_frame) * cap, sizeof(resolve_frame) * ncap);
            cap = ncap;
        }
        st[n].el = ce;
        st[n].style = cs;
        st[n].child_anc = embi_style_mix(anc, cident);
        st[n].next = ce->base.first_child;
        n++;
        embi_bloom_push(&e->bloom, ce);
    }
    embi_free(st, sizeof(resolve_frame) * cap);
}

/* One full pass over the document's top-level elements. */
static void resolve_pass(embi_style_engine *e, embi_docstate *ds, embi_document *doc)
{
    e->pass_no++;
    e->own_change = EMBI_OWN_ALL;
    embi_style_cache_clear(e);
    embi_sel_ctx_init(&e->selctx, nullptr, e->hooks.state != nullptr ? &e->hooks : nullptr);
    embi_bloom_clear(&e->bloom);
    for (embi_node *c = doc->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            resolve_subtree(e, ds, (embi_element *)c);
        }
    }
    embi_sel_ctx_free(&e->selctx);
}

/* The computed font size of the first top-level element, in px (16 when there is none). */
static double root_font_px(embi_node *first)
{
    for (embi_node *c = first; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            const embi_value *fs = embi_style_value(
                (const embi_style *)((embi_element *)c)->style, EMBI_PROP_font_size);
            if (fs != nullptr && fs->kind == EMBI_V_LENGTH && fs->unit == EMBI_U_PX) {
                return fs->num;
            }
            return 16.0;
        }
    }
    return 16.0;
}

/* Epochs are unique across documents and engines, so a document that is forgotten and attached again
 * never repeats the epoch an older layout state remembers. */
static _Atomic uint64_t g_epoch;

/* The full procedure: the first pass resolves against a 16px rem, a second one against the root's font size when it
 * differs. */
void embi_style_full_resolve(embi_style_engine *e, embi_docstate *ds, embi_document *doc)
{
    /* rem is the root element's computed font size. The root's own font-size is computed against
     * the initial 16px, so the first pass yields that size; a second pass resolves every other
     * length against it when the root's size differs from 16px. */
    ds->root_px = 16.0;
    resolve_pass(e, ds, doc);
    double rpx = root_font_px(doc->base.first_child);
    if (rpx > 0 && rpx != 16.0) {
        ds->root_px = rpx;
        resolve_pass(e, ds, doc);
    }
}

double embi_style_root_font_px(embi_document *doc)
{
    return root_font_px(doc->base.first_child);
}

void embi_style_resolve_document(embi_style_engine *e, embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr) {
        return;
    }
    if (!ds->dirty && ds->resolved) {
        return;
    }
    ds->restyled = 0;
    bool partial = false;
    if (ds->resolved && !ds->full) {
        /* W6b: only the marked paths. The pass leaves ds->full set when it needs the full procedure (the rem base moved). */
        embi_style_partial_pass(e, ds, doc);
        partial = !ds->full;
    }
    if (!partial) {
        embi_style_full_resolve(e, ds, doc);
        ds->full_passes++;
    } else {
        ds->partial_passes++;
        if (ds->restyled == 0) {
            ds->skipped_resolves++;
        }
        if (embi_incr_verify_mode() != 0) {
            embi_style_verify_pass(e, ds, doc);
        }
    }
    ds->dirty = false;
    ds->full = false;
    ds->resolved = true;
    embi_hashmap_clear(&ds->dmap);
    ds->total_restyled += ds->restyled;
    ds->epoch = atomic_fetch_add(&g_epoch, 1) + 1;
}

uint64_t embi_style_document_epoch(embi_style_engine *e, embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    return ds != nullptr ? ds->epoch : 0;
}

/* ------------------------------------------------------------------ on-demand pseudo styles */

const embi_style *embi_style_generated(embi_style_engine *e, embi_element *el, embi_sel_pseudo pe)
{
    if (pe == EMBI_PE_NONE) {
        return el->style;
    }
    if (el->style == nullptr) {
        return nullptr;
    }
    embi_docstate *ds = embi_style_ds_find(e, el->base.doc);
    uint64_t v = 0;
    if (ds != nullptr && embi_hashmap_len(&ds->pseudo) > 0 &&
        embi_hashmap_get(&ds->pseudo, ((uint64_t)(uintptr_t)el << 4) | pe, &v)) {
        return (const embi_style *)(uintptr_t)v;
    }
    return nullptr;
}

static const embi_css_rule *cstyle_in(const embi_sset *set, embi_atom name)
{
    for (uint32_t i = set->ncstyles; i > 0; i--) {
        if (set->cstyles[i - 1]->name == name) {
            return set->cstyles[i - 1];
        }
    }
    return nullptr;
}

const embi_css_rule *embi_style_counter_style(embi_style_engine *e, const embi_document *doc, embi_atom name)
{
    embi_docstate *ds = embi_style_ds_find(e, (embi_document *)(uintptr_t)doc);
    const embi_css_rule *r = ds != nullptr ? cstyle_in(&ds->author, name) : nullptr;
    if (r == nullptr) {
        r = cstyle_in(&e->user, name);
    }
    if (r == nullptr) {
        embi_style_ua_ensure(e);
        r = cstyle_in(&e->ua, name);
    }
    return r;
}

bool embi_style_has_generated(embi_style_engine *e, const embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find(e, (embi_document *)(uintptr_t)doc);
    return ds == nullptr || embi_hashmap_len(&ds->pseudo) > 0;
}

const embi_style *embi_style_get(embi_style_engine *e, embi_element *el, embi_sel_pseudo pe)
{
    if (pe == EMBI_PE_NONE) {
        return el->style;
    }
    if (el->style == nullptr) {
        return nullptr;
    }
    embi_document *doc = el->base.doc;
    embi_docstate *ds = embi_style_ds_find(e, doc);
    uint64_t key = ((uint64_t)(uintptr_t)el << 4) | pe;
    uint64_t v = 0;
    if (ds != nullptr && embi_hashmap_get(&ds->pseudo, key, &v)) {
        return (const embi_style *)(uintptr_t)v;
    }
    if (ds == nullptr) {
        return nullptr; /* the document is not attached: nothing owns a new style */
    }
    /* Not generated during the walk: compute it now, with the element's ancestors in the bloom. */
    embi_bloom_clear(&e->bloom);
    embi_sel_ctx_init(&e->selctx, nullptr, e->hooks.state != nullptr ? &e->hooks : nullptr);
    uint32_t depth = 0;
    for (embi_node *p = el->base.parent; p != nullptr; p = p->parent) {
        if (p->type == EMBI_NODE_ELEMENT) {
            depth++;
        }
    }
    embi_element **chain = depth > 0 ? embi_alloc(sizeof(embi_element *) * depth) : nullptr;
    uint32_t k = 0;
    for (embi_node *p = el->base.parent; p != nullptr; p = p->parent) {
        if (p->type == EMBI_NODE_ELEMENT) {
            chain[k++] = (embi_element *)p;
        }
    }
    for (uint32_t i = depth; i > 0; i--) {
        embi_bloom_push(&e->bloom, chain[i - 1]);
    }
    const embi_style *ps = compute_pseudo(e, ds, el, el->style, pe);
    for (uint32_t i = 0; i < depth; i++) {
        embi_bloom_pop(&e->bloom, chain[i]);
    }
    if (depth > 0) {
        embi_free(chain, sizeof(embi_element *) * depth);
    }
    embi_sel_ctx_free(&e->selctx);
    if (ps != nullptr) {
        embi_hashmap_put(&ds->pseudo, key, (uint64_t)(uintptr_t)ps);
    }
    return ps;
}

/* ------------------------------------------------------------------ releasing styles */

/* Releases the styles of one element and of its pseudo-elements. */
static void release_element(embi_style_engine *e, embi_docstate *ds, embi_element *el)
{
    embi_style_purge_element(ds, el);
    if (e->anim.gone != nullptr) {
        e->anim.gone(e->anim.user, el);
    }
    /* W6b: a detached element has no box; the slot would point into the layout's arena. */
    el->box = nullptr;
    if (el->style != nullptr) {
        embi_style_unref(e, (const embi_style *)el->style);
        el->style = nullptr;
    }
    for (uint32_t pe = 1; pe < 16; pe++) {
        uint64_t key = ((uint64_t)(uintptr_t)el << 4) | pe;
        uint64_t v = 0;
        if (embi_hashmap_get(&ds->pseudo, key, &v)) {
            embi_hashmap_remove(&ds->pseudo, key);
            embi_style_unref(e, (const embi_style *)(uintptr_t)v);
        }
    }
}

void embi_style_release_subtree(embi_style_engine *e, embi_docstate *ds, embi_node *root)
{
    if (ds == nullptr || root == nullptr) {
        return;
    }
    for (embi_node *n = root; n != nullptr; n = embi_next_in_tree(root, n)) {
        if (n->type == EMBI_NODE_ELEMENT) {
            release_element(e, ds, (embi_element *)n);
        }
    }
    embi_style_cache_clear(e);
}

void embi_style_forget_document_walk(embi_style_engine *e, embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr) {
        return;
    }
    clear_pseudos(e, ds);
    for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n != nullptr;
         n = embi_next_in_tree(&doc->base, n)) {
        if (n->type == EMBI_NODE_ELEMENT) {
            embi_element *el = (embi_element *)n;
            if (el->style != nullptr) {
                embi_style_unref(e, (const embi_style *)el->style);
                el->style = nullptr;
            }
        }
    }
}

/* ------------------------------------------------------------------ serialization */

static void serialize_tokens(embi_sb *sb, embi_css_slice s);

static void serialize_cv(embi_sb *sb, const embi_css_cv *c)
{
    const embi_css_token *t = &c->tok;
    switch (t->type) {
    case EMBI_CSS_WS:
        embi_sb_append_char(sb, ' ');
        return;
    case EMBI_CSS_IDENT:
        embi_sb_append(sb, t->value);
        return;
    case EMBI_CSS_FUNCTION:
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, '(');
        serialize_tokens(sb, embi_css_slice_make(c->kids, c->nkids));
        embi_sb_append_char(sb, ')');
        return;
    case EMBI_CSS_AT_KEYWORD:
        embi_sb_append_char(sb, '@');
        embi_sb_append(sb, t->value);
        return;
    case EMBI_CSS_HASH:
        embi_sb_append_char(sb, '#');
        embi_sb_append(sb, t->value);
        return;
    case EMBI_CSS_STRING:
        embi_sb_append_char(sb, '"');
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, '"');
        return;
    case EMBI_CSS_URL:
        embi_sb_append(sb, EMBI_STR("url("));
        embi_sb_append(sb, t->value);
        embi_sb_append_char(sb, ')');
        return;
    case EMBI_CSS_NUMBER:
    case EMBI_CSS_PERCENTAGE:
        embi_sb_append(sb, t->repr);
        return;
    case EMBI_CSS_DIMENSION:
        /* repr is the number's source text; value is the unit (css.h). */
        embi_sb_append(sb, t->repr);
        embi_sb_append(sb, t->value);
        return;
    case EMBI_CSS_DELIM:
        embi_sb_append_cp(sb, t->cp);
        return;
    case EMBI_CSS_COMMA:
        embi_sb_append_char(sb, ',');
        return;
    case EMBI_CSS_COLON:
        embi_sb_append_char(sb, ':');
        return;
    case EMBI_CSS_SEMICOLON:
        embi_sb_append_char(sb, ';');
        return;
    case EMBI_CSS_LPAREN:
    case EMBI_CSS_LBRACKET:
    case EMBI_CSS_LBRACE: {
        char open = t->type == EMBI_CSS_LPAREN ? '(' : t->type == EMBI_CSS_LBRACKET ? '[' : '{';
        char close = t->type == EMBI_CSS_LPAREN ? ')' : t->type == EMBI_CSS_LBRACKET ? ']' : '}';
        embi_sb_append_char(sb, open);
        serialize_tokens(sb, embi_css_slice_make(c->kids, c->nkids));
        embi_sb_append_char(sb, close);
        return;
    }
    default:
        embi_sb_append(sb, t->value);
        return;
    }
}

static void serialize_tokens(embi_sb *sb, embi_css_slice s)
{
    for (uint32_t i = 0; i < s.n; i++) {
        serialize_cv(sb, &s.p[i]);
    }
}

void embi_style_value_serialize(embi_sb *sb, const embi_value *v)
{
    embi_value_serialize(sb, v);
}

/* ------------------------------------------------------------------ accessors */

uint32_t embi_style_group_of(embi_prop_id prop)
{
    const embi_layout *L = embi_layout_get();
    if (prop == 0 || prop >= EMBI_PROP_COUNT) {
        return UINT32_MAX;
    }
    return L->gid[prop] == EMBI_STYLE_NO_GROUP ? UINT32_MAX : L->gid[prop];
}

/* The stored (physical) property a style reads for prop: logical ones map through the style's
 * writing mode and direction. Returns 0 for shorthands and unknown ids. */
static uint16_t stored_prop(const embi_style *s, embi_prop_id prop)
{
    if (prop == 0 || prop >= EMBI_PROP_COUNT || embi_prop_is_shorthand(prop)) {
        return 0;
    }
    if (embi_prop_is_logical(prop)) {
        uint16_t wm = EMBI_KW_horizontal_tb;
        uint16_t dir = EMBI_KW_ltr;
        const embi_value *wv = embi_style_value(s, EMBI_PROP_writing_mode);
        const embi_value *dv = embi_style_value(s, EMBI_PROP_direction);
        if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
            wm = wv->kw;
        }
        if (dv != nullptr && dv->kind == EMBI_V_KEYWORD) {
            dir = dv->kw;
        }
        return (uint16_t)embi_prop_to_physical(prop, wm, dir);
    }
    return (uint16_t)prop;
}

const embi_value *embi_style_value(const embi_style *s, embi_prop_id prop)
{
    if (s == nullptr) {
        return nullptr;
    }
    const embi_layout *L = embi_layout_get();
    /* W3p: a stored (physical) property has a group; shorthands and logical properties have none, so the
     * common case is two table reads and the mapping through the writing mode stays on the slow path. */
    if (prop < EMBI_PROP_COUNT && L->gid[prop] != EMBI_STYLE_NO_GROUP) {
        return s->groups[L->gid[prop]]->vals[L->slot[prop]];
    }
    uint16_t p = stored_prop(s, prop);
    if (p == 0) {
        return nullptr;
    }
    uint16_t g = L->gid[p];
    if (g == EMBI_STYLE_NO_GROUP) {
        return nullptr;
    }
    return s->groups[g]->vals[L->slot[p]];
}

const void *embi_style_group_key(const embi_style *s, embi_prop_id prop)
{
    const embi_layout *L = embi_layout_get();
    if (s == nullptr || prop >= EMBI_PROP_COUNT || L->gid[prop] == EMBI_STYLE_NO_GROUP) {
        return nullptr;
    }
    return s->groups[L->gid[prop]];
}

const void *embi_style_custom_key(const embi_style *s)
{
    return s != nullptr ? s->custom : nullptr;
}

bool embi_style_value_text(const embi_style *s, embi_prop_id prop, embi_sb *sb)
{
    const embi_value *v = embi_style_value(s, prop);
    if (v == nullptr) {
        return false;
    }
    embi_value_serialize(sb, v);
    return true;
}

bool embi_style_custom_text(const embi_style *s, embi_atom name, embi_sb *sb)
{
    if (s == nullptr || s->custom == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < s->custom->n; i++) {
        if (s->custom->e[i].name == name) {
            serialize_tokens(sb, s->custom->e[i].val);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ R4c: a style under another parent */

const embi_style *embi_style_compute_under(embi_style_engine *e, embi_element *el, const embi_style *parent)
{
    embi_docstate *ds = embi_style_ds_find(e, el->base.doc);
    if (ds == nullptr || parent == nullptr) {
        return nullptr;
    }
    embi_bloom_clear(&e->bloom);
    embi_sel_ctx_init(&e->selctx, nullptr, e->hooks.state != nullptr ? &e->hooks : nullptr);
    uint32_t depth = 0;
    for (embi_node *p = el->base.parent; p != nullptr; p = p->parent) {
        if (p->type == EMBI_NODE_ELEMENT) {
            depth++;
        }
    }
    embi_element **chain = depth > 0 ? embi_alloc(sizeof(embi_element *) * depth) : nullptr;
    uint32_t k = 0;
    for (embi_node *p = el->base.parent; p != nullptr; p = p->parent) {
        if (p->type == EMBI_NODE_ELEMENT) {
            chain[k++] = (embi_element *)p;
        }
    }
    for (uint32_t i = depth; i > 0; i--) {
        embi_bloom_push(&e->bloom, chain[i - 1]);
    }
    embi_style_sc_setup(e, ds, parent, false);
    embi_sctx *sc = &e->sc;
    embi_style_gather_all(e, sc, el, EMBI_PE_NONE, &e->bloom);
    embi_style_gather_hints(e, sc, el);
    embi_style_gather_attr(e, sc, el);
    const embi_style *owned = embi_style_cascade_finish(e, sc);
    for (uint32_t i = 0; i < depth; i++) {
        embi_bloom_pop(&e->bloom, chain[i]);
    }
    if (depth > 0) {
        embi_free(chain, sizeof(embi_element *) * depth);
    }
    embi_sel_ctx_free(&e->selctx);
    return owned;
}

void embi_style_release(embi_style_engine *e, const embi_style *s)
{
    if (s != nullptr) {
        embi_style_unref(e, s);
    }
}
