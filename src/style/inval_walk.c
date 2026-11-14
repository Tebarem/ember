/*
 * Partial restyle and the change set (W6b): the walk over the marked paths, the classification of a style change
 * (layout, paint, nothing visible), the tracking of old styles for a layout engine, and the verification pass of
 * EMBER_VERIFY_INCREMENTAL.
 */
#include "inval.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

/* ------------------------------------------------------------------ classification */

static bool value_same(const embi_value *x, const embi_value *y)
{
    if (x == y) {
        return true;
    }
    if (x == nullptr || y == nullptr || x->kind != y->kind) {
        return false;
    }
    if (embi_value_identical(x, y)) {
        return true; /* CF1: equal field by field, so their serializations are too */
    }
    if (embi_value_clearly_differs(x, y)) {
        return false; /* the serializations differ, so skip making them (every animated value differs on every frame) */
    }
    /* Complex values are not canonical: the same declared value can be a different object after another cascade. */
    embi_sb a = {0}, b = {0};
    embi_value_serialize(&a, x);
    embi_value_serialize(&b, y);
    bool same = a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
    embi_sb_free(&a);
    embi_sb_free(&b);
    return same;
}

static bool is_none_v(const embi_value *v)
{
    return v == nullptr || v->kind == EMBI_V_NONE || (v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none);
}

static bool translucent(const embi_value *v)
{
    if (v == nullptr || v->kind != EMBI_V_NUMBER) {
        return false;
    }
    return v->num < 1.0;
}

static bool vis_ok(const embi_value *v)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD && (v->kw == EMBI_KW_visible || v->kw == EMBI_KW_hidden);
}

/* The class of the change of one property from value a to value b. */
static uint8_t prop_class(uint16_t p, const embi_value *a, const embi_value *b, uint32_t in_place)
{
    switch (p) {
    case EMBI_PROP_color:
    case EMBI_PROP_background_color:
    case EMBI_PROP_background_image:
    case EMBI_PROP_background_repeat:
    case EMBI_PROP_background_position_x:
    case EMBI_PROP_background_position_y:
    case EMBI_PROP_background_size:
    case EMBI_PROP_background_origin:
    case EMBI_PROP_background_clip:
    case EMBI_PROP_background_attachment:
    case EMBI_PROP_background_blend_mode:
    case EMBI_PROP_border_top_color:
    case EMBI_PROP_border_right_color:
    case EMBI_PROP_border_bottom_color:
    case EMBI_PROP_border_left_color:
    case EMBI_PROP_outline_color:
    case EMBI_PROP_outline_style:
    case EMBI_PROP_outline_width:
    case EMBI_PROP_outline_offset:
    case EMBI_PROP_text_decoration_line:
    case EMBI_PROP_text_decoration_color:
    case EMBI_PROP_text_decoration_style:
    case EMBI_PROP_text_decoration_thickness:
    case EMBI_PROP_text_underline_offset:
    case EMBI_PROP_text_underline_position:
    case EMBI_PROP_caret_color:
    case EMBI_PROP_accent_color:
    case EMBI_PROP_box_shadow:
    case EMBI_PROP_text_shadow:
    case EMBI_PROP_transform_origin:
        return EMBI_SCH_PAINT;
    case EMBI_PROP_cursor:
    case EMBI_PROP_pointer_events:
    case EMBI_PROP_user_select:
        return EMBI_SCH_REFRESH;
    case EMBI_PROP_opacity:
        /* below 1 the box is a stacking context (position.c): crossing 1 changes the fragments' flags */
        return (in_place & EMBI_INPLACE_OPACITY) != 0 || translucent(a) == translucent(b) ? EMBI_SCH_PAINT : EMBI_SCH_LAYOUT;
    case EMBI_PROP_transform:
    case EMBI_PROP_translate:
    case EMBI_PROP_rotate:
    case EMBI_PROP_scale:
        /* layout reads only whether the property is none (stacking context, containing block) */
        return (in_place & EMBI_INPLACE_TRANSFORM) != 0 || is_none_v(a) == is_none_v(b) ? EMBI_SCH_PAINT : EMBI_SCH_LAYOUT;
    case EMBI_PROP_visibility:
        return vis_ok(a) && vis_ok(b) ? EMBI_SCH_PAINT : EMBI_SCH_LAYOUT;
    default:
        return EMBI_SCH_LAYOUT;
    }
}

static bool custom_same(const embi_style *a, const embi_style *b)
{
    const embi_custom_map *x = a->custom, *y = b->custom;
    if (x == y) {
        return true;
    }
    uint32_t nx = x != nullptr ? x->n : 0, ny = y != nullptr ? y->n : 0;
    if (nx != ny) {
        return false;
    }
    for (uint32_t i = 0; i < nx; i++) {
        if (x->e[i].name != y->e[i].name) {
            return false;
        }
        embi_sb p = {0}, q = {0};
        embi_style_custom_text(a, x->e[i].name, &p);
        embi_style_custom_text(b, x->e[i].name, &q);
        bool same = p.len == q.len && (p.len == 0 || memcmp(p.data, q.data, p.len) == 0);
        embi_sb_free(&p);
        embi_sb_free(&q);
        if (!same) {
            return false;
        }
    }
    return true;
}

static uint8_t style_diff(const embi_style *a, const embi_style *b, uint32_t in_place)
{
    if (a == b) {
        return 0;
    }
    if (a == nullptr || b == nullptr) {
        return EMBI_SCH_LAYOUT;
    }
    const embi_layout *L = embi_layout_get();
    uint8_t cls = custom_same(a, b) ? 0 : EMBI_SCH_REFRESH;
    for (uint32_t g = 0; g < L->ngroups; g++) {
        const embi_style_group *ga = a->groups[g], *gb = b->groups[g];
        if (ga == gb) {
            continue;
        }
        for (uint32_t i = 0; i < L->n[g]; i++) {
            const embi_value *va = ga->vals[i], *vb = gb->vals[i];
            if (va == vb || value_same(va, vb)) {
                continue;
            }
            uint8_t c = prop_class(L->props[g][i], va, vb, in_place);
            if (c > cls) {
                cls = c;
                if (cls == EMBI_SCH_LAYOUT) {
                    return cls;
                }
            }
        }
    }
    return cls;
}

uint8_t embi_style_diff(const embi_style *a, const embi_style *b)
{
    return style_diff(a, b, 0);
}

/* W6e: the class of the change when the crossings named by in_place (EMBI_INPLACE_*) are not counted as layout changes: they
 * only flip the stacking-context and containing-block flags of the element's fragment, which layout can redo in place.
 * LAYOUT only when something else needs layout. */
uint8_t embi_style_classify_in_place(const embi_style *old_style, const embi_style *new_style, uint32_t in_place)
{
    if (old_style == new_style) {
        return EMBI_SCH_NONE;
    }
    uint8_t c = style_diff(old_style, new_style, in_place);
    return c != 0 ? c : (uint8_t)EMBI_SCH_REFRESH;
}

uint8_t embi_style_classify(const embi_style *old_style, const embi_style *new_style)
{
    if (old_style == new_style) {
        return EMBI_SCH_NONE;
    }
    uint8_t c = embi_style_diff(old_style, new_style);
    return c != 0 ? c : (uint8_t)EMBI_SCH_REFRESH; /* a different object: the boxes' pointers must still be refreshed */
}

/* ------------------------------------------------------------------ tracking */

#define EMBI_CHG_LIMIT 60000u

static void drop_tracking(embi_style_engine *e, embi_docstate *ds)
{
    for (uint32_t i = 0; i < ds->chg.len; i++) {
        if (ds->chg.data[i].old != nullptr) {
            embi_style_unref(e, ds->chg.data[i].old);
        }
    }
    embi_vec_clear(&ds->chg);
    embi_hashmap_clear(&ds->cmap);
    embi_hashmap_clear(&ds->omap);
}

static embi_style_chg *chg_entry(embi_docstate *ds, embi_element *el, const embi_style *old)
{
    uint64_t key = (uint64_t)(uintptr_t)el;
    uint64_t idx = 0;
    if (embi_hashmap_len(&ds->cmap) > 0 && embi_hashmap_get(&ds->cmap, key, &idx)) {
        return &ds->chg.data[idx - 1];
    }
    if (ds->chg.len >= EMBI_CHG_LIMIT) {
        ds->lost = true;
        drop_tracking(ds->eng, ds);
        return nullptr;
    }
    embi_style_ref(old);
    embi_style_chg c = {el, old, false};
    embi_vec_push(&ds->chg, c);
    embi_hashmap_insert_new(&ds->cmap, key, ds->chg.len); /* absent: the get above missed */
    return &ds->chg.data[ds->chg.len - 1];
}

static void verify_el_fail(embi_element *el, const char *what)
{
    char msg[160];
    embi_str n = embi_atom_str(el->base.name);
    SDL_snprintf(msg, sizeof msg, "<%.*s> %s", (int)(n.len < 40 ? n.len : 40), n.ptr, what);
    embi_incr_verify_fail(msg);
}

void embi_style_note_change(embi_docstate *ds, embi_element *el, const embi_style *old, const embi_style *cur)
{
    if (old == cur || old == nullptr) {
        return;
    }
    if (ds->verifying) {
        if (embi_style_diff(old, cur) != 0) {
            verify_el_fail(el, "style differs between the partial and the full restyle");
        }
        return;
    }
    if (!ds->track || ds->lost) {
        return;
    }
    (void)chg_entry(ds, el, old);
}

void embi_style_note_pseudo(embi_docstate *ds, embi_element *el, uint8_t cls)
{
    if (ds->verifying) {
        verify_el_fail(el, "pseudo-element style differs between the partial and the full restyle");
        return;
    }
    if (!ds->track || ds->lost) {
        return;
    }
    embi_style_chg *c = chg_entry(ds, el, (const embi_style *)el->style);
    if (c != nullptr) {
        c->pseudo = cls > c->pseudo ? cls : c->pseudo; /* CF1: the worst class of the element's pseudo-element changes */
    }
}

/* ------------------------------------------------------------------ the partial walk */

typedef struct pframe {
    embi_element *el;
    const embi_style *style;
    uint64_t child_anc;
    embi_node *next;
    bool kids_force; /* restyle every child */
    bool kids_all;   /* ... and everything below them */
    bool kids_inval; /* WPTASAN: kids_force for an invalidation (not only because this element's style changed) */
    bool sib_force;  /* the siblings after a SIB-marked child */
    bool sib_deep;
    bool next_force; /* the next child (after a NEXT-marked one) */
    bool next_deep;  /* ... and everything below it */
} pframe;

typedef struct pwalk {
    embi_style_engine *e;
    embi_docstate *ds;
    pframe *st;
    uint32_t n, cap;
} pwalk;

static void push_frame(pwalk *w, embi_element *el, const embi_style *style, uint64_t child_anc, bool kids_force,
                       bool kids_all, bool kids_inval)
{
    if (w->n == w->cap) {
        uint32_t ncap = w->cap * 2u;
        w->st = embi_realloc(w->st, sizeof(pframe) * w->cap, sizeof(pframe) * ncap);
        w->cap = ncap;
    }
    pframe *f = &w->st[w->n++];
    f->el = el;
    f->style = style;
    f->child_anc = child_anc;
    f->next = el->base.first_child;
    f->kids_force = kids_force;
    f->kids_all = kids_all;
    f->kids_inval = kids_inval;
    f->sib_force = false;
    f->sib_deep = false;
    f->next_force = false;
    f->next_deep = false;
    embi_bloom_push(&w->e->bloom, el);
}

static void walk_root(pwalk *w, embi_element *root)
{
    embi_style_engine *e = w->e;
    embi_docstate *ds = w->ds;
    uint16_t fl = embi_inval_get(ds, root);
    bool own = (fl & (EMBI_IVD_SELF | EMBI_IVD_DESC)) != 0 || root->style == nullptr;
    bool force = own || (fl & EMBI_IVD_ANIM) != 0;
    if (!force && fl == 0) {
        return;
    }
    uint64_t ident = embi_style_identity_hash(root);
    const embi_style *rs = (const embi_style *)root->style;
    bool changed = false;
    if (force) {
        const embi_style *old = rs;
        e->own_change = own ? EMBI_OWN_SELF : EMBI_OWN_NONE;
        rs = embi_style_enter_element(e, ds, root, e->init, true, 0, ident);
        e->own_change = EMBI_OWN_ALL;
        changed = rs != old;
    }
    bool all = (fl & (EMBI_IVD_DESC | EMBI_IVD_KIDSDEEP)) != 0;
    bool kinval = all || (fl & (EMBI_IVD_KIDS | EMBI_IVD_KIDSDEEP)) != 0;
    bool kids = changed || kinval;
    if (!kids && (fl & EMBI_IVD_SUB) == 0) {
        return;
    }
    w->n = 0;
    push_frame(w, root, rs, embi_style_mix(0, ident), kids, all, kinval);
    while (w->n > 0) {
        pframe *top = &w->st[w->n - 1];
        embi_node *c = top->next;
        while (c != nullptr && c->type != EMBI_NODE_ELEMENT) {
            c = c->next;
        }
        if (c == nullptr) {
            embi_bloom_pop(&e->bloom, top->el);
            w->n--;
            continue;
        }
        top->next = c->next;
        embi_element *ce = (embi_element *)c;
        uint16_t cf = embi_inval_get(ds, ce);
        bool nforce = top->next_force, ndeep = top->next_deep;
        top->next_force = top->next_deep = false;
        bool call = top->kids_all || top->sib_deep || ndeep || (cf & (EMBI_IVD_DESC | EMBI_IVD_KIDSDEEP)) != 0;
        /* WPTASAN: own: the element's own inputs may have changed (not only its parent's style, nor only its animations) */
        bool cown = top->kids_inval || top->sib_force || top->sib_deep || nforce ||
                    (cf & (EMBI_IVD_SELF | EMBI_IVD_DESC)) != 0 || ce->style == nullptr;
        bool cforce = cown || top->kids_force || (cf & EMBI_IVD_ANIM) != 0;
        if ((cf & (EMBI_IVD_NEXT | EMBI_IVD_NEXTDEEP)) != 0) {
            top->next_force = true;
            top->next_deep = (cf & EMBI_IVD_NEXTDEEP) != 0;
        }
        if ((cf & (EMBI_IVD_SIB | EMBI_IVD_SIBDEEP)) != 0) {
            top->sib_force = true;
            if ((cf & EMBI_IVD_SIBDEEP) != 0) {
                top->sib_deep = true;
            }
        }
        if (!cforce && cf == 0) {
            continue;
        }
        const embi_style *parent = top->style;
        uint64_t anc = top->child_anc;
        uint64_t cident = embi_style_identity_hash(ce);
        const embi_style *cs = (const embi_style *)ce->style;
        bool cchanged = false;
        if (cforce) {
            const embi_style *old = cs;
            e->own_change = cown ? EMBI_OWN_SELF : EMBI_OWN_NONE;
            cs = embi_style_enter_element(e, ds, ce, parent, false, anc, cident);
            e->own_change = EMBI_OWN_ALL;
            cchanged = cs != old;
        }
        bool ckinval = call || (cf & (EMBI_IVD_KIDS | EMBI_IVD_KIDSDEEP)) != 0;
        bool ckids = cchanged || ckinval;
        if (!ckids && (cf & EMBI_IVD_SUB) == 0) {
            continue; /* restyled, nothing changed for its children and nothing below is marked */
        }
        push_frame(w, ce, cs, embi_style_mix(anc, cident), ckids, call, ckinval);
    }
}

void embi_style_partial_pass(embi_style_engine *e, embi_docstate *ds, embi_document *doc)
{
    e->pass_no++;
    embi_style_cache_clear(e);
    embi_sel_ctx_init(&e->selctx, nullptr, e->hooks.state != nullptr ? &e->hooks : nullptr);
    embi_bloom_clear(&e->bloom);
    pwalk w;
    w.e = e;
    w.ds = ds;
    w.cap = 64u;
    w.n = 0;
    w.st = embi_alloc(sizeof(pframe) * w.cap);
    for (embi_node *c = doc->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            walk_root(&w, (embi_element *)c);
        }
    }
    embi_free(w.st, sizeof(pframe) * w.cap);
    embi_sel_ctx_free(&e->selctx);
    /* rem is the root's computed font size: when it moved, every length that uses it must be recomputed. */
    double rpx = embi_style_root_font_px(doc);
    double want = rpx > 0 && rpx != 16.0 ? rpx : 16.0;
    if (want != ds->root_px) {
        ds->full = true;
    }
}

void embi_style_verify_pass(embi_style_engine *e, embi_docstate *ds, embi_document *doc)
{
    uint32_t saved = ds->restyled;
    ds->verifying = true;
    embi_style_full_resolve(e, ds, doc);
    ds->verifying = false;
    ds->restyled = saved;
}

/* ------------------------------------------------------------------ the public API */

void embi_style_track(embi_style_engine *e, embi_document *doc, bool on)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr || ds->track == on) {
        return;
    }
    ds->track = on;
    drop_tracking(e, ds);
    if (!on) {
        embi_style_release_grave(e, doc);
    }
    ds->lost = true; /* the consumer starts from a full build */
    ds->from_epoch = ds->epoch;
}

void embi_style_release_grave(embi_style_engine *e, embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < ds->grave.len; i++) {
        embi_style_unref(e, ds->grave.data[i]);
    }
    embi_vec_clear(&ds->grave);
}

uint32_t embi_style_release_grave_unreferenced(embi_style_engine *e, embi_document *doc,
                                               bool (*referenced)(void *user, const embi_style *s), void *user)
{
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr) {
        return 0;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ds->grave.len; i++) {
        const embi_style *s = ds->grave.data[i];
        if (referenced(user, s)) {
            ds->grave.data[kept++] = s;
        } else {
            embi_style_unref(e, s);
        }
    }
    ds->grave.len = kept;
    return kept;
}

uint32_t embi_style_grave_count(const embi_style_engine *e, const embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find((embi_style_engine *)(uintptr_t)e, (embi_document *)(uintptr_t)doc);
    return ds != nullptr ? ds->grave.len : 0;
}

const embi_style *embi_style_grave_at(const embi_style_engine *e, const embi_document *doc, uint32_t i)
{
    embi_docstate *ds = embi_style_ds_find((embi_style_engine *)(uintptr_t)e, (embi_document *)(uintptr_t)doc);
    return ds != nullptr && i < ds->grave.len ? ds->grave.data[i] : nullptr;
}

uint32_t embi_style_restyled_count(const embi_style_engine *e, const embi_document *doc)
{
    embi_docstate *ds = embi_style_ds_find((embi_style_engine *)(uintptr_t)e, (embi_document *)(uintptr_t)doc);
    return ds != nullptr ? ds->restyled : 0;
}

void embi_style_restyle_stats_get(const embi_style_engine *e, const embi_document *doc, embi_style_restyle_stats *out)
{
    memset(out, 0, sizeof *out);
    embi_docstate *ds = embi_style_ds_find((embi_style_engine *)(uintptr_t)e, (embi_document *)(uintptr_t)doc);
    if (ds == nullptr) {
        return;
    }
    out->last_restyled = ds->restyled;
    out->total_restyled = ds->total_restyled;
    out->partial_passes = ds->partial_passes;
    out->full_passes = ds->full_passes;
    out->skipped = ds->skipped_resolves;
    out->last_layout = ds->cls_layout;
    out->last_paint = ds->cls_paint;
    out->last_refresh = ds->cls_refresh;
}

void embi_style_take_delta(embi_style_engine *e, embi_document *doc, embi_style_delta *out)
{
    memset(out, 0, sizeof *out);
    embi_docstate *ds = embi_style_ds_find(e, doc);
    if (ds == nullptr) {
        out->lost = true;
        return;
    }
    embi_style_resolve_document(e, doc);
    out->from_epoch = ds->from_epoch;
    out->to_epoch = ds->epoch;
    ds->from_epoch = ds->epoch;
    ds->cls_layout = ds->cls_paint = ds->cls_refresh = 0;
    if (!ds->track || ds->lost) {
        out->lost = true;
        ds->lost = false;
        drop_tracking(e, ds);
        return;
    }
    embi_vec_clear(&ds->outc);
    embi_vec_clear(&ds->outo);
    for (uint32_t i = 0; i < ds->chg.len; i++) {
        embi_style_chg *c = &ds->chg.data[i];
        if (c->el == nullptr || c->old == nullptr) {
            continue;
        }
        const embi_style *cur = (const embi_style *)c->el->style;
        if (cur == nullptr || (cur == c->old && !c->pseudo)) {
            embi_style_unref(e, c->old);
            c->old = nullptr;
            continue;
        }
        uint8_t cls = cur != c->old ? embi_style_classify(c->old, cur) : (uint8_t)EMBI_SCH_NONE;
        cls = c->pseudo > cls ? c->pseudo : cls; /* CF1: was EMBI_SCH_LAYOUT for any pseudo-element change */
        embi_style_change ch = {c->el, c->old, cls, c->pseudo != 0};
        embi_vec_push(&ds->outc, ch);
        embi_vec_push(&ds->grave, c->old); /* the reference moves to the grave */
        c->old = nullptr;
        if (cls == EMBI_SCH_LAYOUT) {
            ds->cls_layout++;
        } else if (cls == EMBI_SCH_PAINT) {
            ds->cls_paint++;
        } else {
            ds->cls_refresh++;
        }
    }
    embi_vec_clear(&ds->chg);
    embi_hashmap_clear(&ds->cmap);
    embi_hashmap_iter it = embi_hashmap_iter_begin(&ds->omap);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_style_origin o = {(embi_element *)(uintptr_t)k, (uint8_t)v};
        embi_vec_push(&ds->outo, o);
    }
    embi_hashmap_clear(&ds->omap);
    out->changes = ds->outc.data;
    out->nchanges = ds->outc.len;
    out->origins = ds->outo.data;
    out->norigins = ds->outo.len;
}
