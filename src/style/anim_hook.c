/*
 * The style side of the animation layer (W6c, see style.h "animation hooks"): installing the hooks, marking an element for a
 * restyle, computing keyframe values against an element's style, finding the !important declarations of an element, and
 * building the overlay style (the base style with the animated values in place).
 */
#include "inval.h"
#include "style_int.h"

#include <string.h>

void embi_style_set_anim_hooks(embi_style_engine *e, const embi_style_anim_hooks *h)
{
    if (h != nullptr) {
        e->anim = *h;
    } else {
        memset(&e->anim, 0, sizeof e->anim);
    }
    /* Every element's style is computed again through (or without) the hook. */
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->docs);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_docstate *ds = (embi_docstate *)(uintptr_t)v;
        ds->dirty = true;
        ds->full = true;
    }
}

const embi_style *embi_style_anim_cascade_under(embi_style_engine *e, embi_element *el, const embi_style *parent)
{
    embi_docstate *ds = embi_style_ds_find(e, el->base.doc);
    embi_style_sc_setup(e, ds, parent, false);
    embi_sctx *sc = &e->sc;
    embi_style_gather_all(e, sc, el, EMBI_PE_NONE, &e->bloom);
    embi_style_gather_hints(e, sc, el);
    embi_style_gather_attr(e, sc, el);
    return embi_style_cascade_finish(e, sc);
}

void embi_style_mark_element(embi_style_engine *e, embi_element *el)
{
    embi_docstate *ds = el->base.doc != nullptr ? embi_style_ds_find(e, el->base.doc) : nullptr;
    if (ds != nullptr) {
        embi_inval_mark_anim(ds, el);
    }
}

uint8_t embi_style_anim_own_change(const embi_style_engine *e)
{
    return e->own_change;
}

uint64_t embi_style_anim_pass(const embi_style_engine *e)
{
    return e->pass_no;
}

/* ------------------------------------------------------------------ values */

void embi_style_anim_ctx(embi_style_engine *e, embi_element *el, const embi_style *base, const embi_style *parent,
                         bool is_root)
{
    embi_docstate *ds = embi_style_ds_find(e, el->base.doc);
    embi_style_sc_setup(e, ds, parent, is_root);
    embi_sctx *sc = &e->sc;
    sc->gen++;
    sc->dirty = 0;
    sc->ntargets = 0;
    sc->el = el;
    /* The element's own font size (em units), writing mode and color scheme come from its computed style. */
    embi_style_set_ovr(sc, EMBI_PROP_font_size, embi_style_value(base, EMBI_PROP_font_size));
    const embi_value *wm = embi_style_value(base, EMBI_PROP_writing_mode);
    const embi_value *dir = embi_style_value(base, EMBI_PROP_direction);
    sc->wm = wm != nullptr && wm->kind == EMBI_V_KEYWORD ? wm->kw : EMBI_KW_horizontal_tb;
    sc->dir = dir != nullptr && dir->kind == EMBI_V_KEYWORD ? dir->kw : EMBI_KW_ltr;
    sc->dark = embi_style_scheme_dark(sc, embi_style_value(base, EMBI_PROP_color_scheme));
}

const embi_value *embi_style_anim_compute(embi_style_engine *e, embi_prop_id prop, const embi_value *specified)
{
    embi_sctx *sc = &e->sc;
    if (prop == EMBI_PROP_font_size) {
        return embi_style_finalize_value(sc, prop, specified);
    }
    return embi_style_finalize_value(sc, prop, specified);
}

void embi_style_anim_important(embi_style_engine *e, const embi_prop_id *props, uint32_t n, bool *out)
{
    embi_sctx *sc = &e->sc;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = false;
    }
    embi_element *el = sc->el;
    /* The sctx was set up by embi_style_anim_ctx; the candidates are gathered afresh (the element's bloom of ancestors is
     * the walk's). */
    uint32_t keep_gen = sc->gen;
    sc->ncands = 0;
    sc->unsafe = false;
    embi_style_gather_all(e, sc, el, EMBI_PE_NONE, &e->bloom);
    embi_style_gather_hints(e, sc, el);
    embi_style_gather_attr(e, sc, el);
    for (uint32_t c = 0; c < sc->ncands; c++) {
        if (!sc->cands[c].important) {
            continue;
        }
        uint16_t ids[EMBI_LONGHAND_MAX];
        uint32_t k = embi_style_origs(sc->cands[c].d, ids, EMBI_LONGHAND_MAX);
        for (uint32_t j = 0; j < k; j++) {
            uint16_t phys = embi_style_target(sc, ids[j]);
            for (uint32_t i = 0; i < n; i++) {
                if (props[i] == phys || props[i] == ids[j]) {
                    out[i] = true;
                }
            }
        }
    }
    sc->ncands = 0;
    sc->gen = keep_gen;
}

/* ------------------------------------------------------------------ overlay */

static inline embi_prop_id overlay_phys(embi_prop_id p, uint16_t wm, uint16_t dir)
{
    return embi_prop_is_logical(p) ? embi_prop_to_physical(p, wm, dir) : p;
}

embi_style *embi_style_overlay(embi_style_engine *e, const embi_style *base, const embi_prop_id *props,
                               const embi_value *const *vals, uint32_t n)
{
    const embi_layout *L = embi_layout_get();
    uint16_t wm = EMBI_KW_horizontal_tb, dir = EMBI_KW_ltr;
    const embi_value *wv = embi_style_value(base, EMBI_PROP_writing_mode);
    const embi_value *dv = embi_style_value(base, EMBI_PROP_direction);
    if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
        wm = wv->kw;
    }
    if (dv != nullptr && dv->kind == EMBI_V_KEYWORD) {
        dir = dv->kw;
    }
    const embi_style_group *groups[EMBI_STYLE_MAX_GROUPS];
    for (uint32_t g = 0; g < L->ngroups; g++) {
        groups[g] = base->groups[g];
        ((embi_style_group *)groups[g])->refs++;
    }
    /* Only the groups some property lives in are looked at (the ones copied for a few slots each, not every group of the style). */
    bool touched[EMBI_STYLE_MAX_GROUPS] = {false};
    /* No fixed-size scratch for the physical ids: the before-change parent style can carry up to 512 properties. */
    for (uint32_t i = 0; i < n; i++) {
        embi_prop_id p = overlay_phys(props[i], wm, dir);
        if (p != 0 && p < EMBI_PROP_COUNT && L->gid[p] < L->ngroups) {
            touched[L->gid[p]] = true;
        }
    }
    for (uint32_t g = 0; g < L->ngroups; g++) {
        if (!touched[g]) {
            continue;
        }
        const embi_style_group *bg = base->groups[g];
        const embi_value *nv[512];
        if (bg->n > 512) { /* the group has more slots than the scratch: leave it as the base has it */
            continue;
        }
        uint64_t xh = bg->xh;
        bool any = false;
        memcpy(nv, bg->vals, sizeof(void *) * bg->n);
        for (uint32_t i = 0; i < n; i++) {
            embi_prop_id p = overlay_phys(props[i], wm, dir);
            if (p == 0 || p >= EMBI_PROP_COUNT || L->gid[p] != g) {
                continue;
            }
            uint32_t s = L->slot[p];
            if (nv[s] == vals[i]) {
                continue;
            }
            xh ^= embi_style_slot_mix(s, nv[s]) ^ embi_style_slot_mix(s, vals[i]);
            nv[s] = vals[i];
            any = true;
        }
        if (any) {
            const embi_style_group *ng = embi_style_group_intern_xh(e, g, nv, xh);
            embi_style_group *old = (embi_style_group *)groups[g];
            if (--old->refs == 0) {
                /* cannot happen: the base style holds it */
                EMBI_ASSERT(false);
            }
            groups[g] = ng;
        }
    }
    return embi_style_intern(e, groups, base->custom);
}

/* ------------------------------------------------------------------ comparing and holding styles */

void embi_style_retain(const embi_style *s)
{
    embi_style_ref(s);
}

bool embi_style_same_content(const embi_style *a, const embi_style *b)
{
    return embi_style_diff(a, b) == 0;
}

static bool diff_value(const embi_value *x, const embi_value *y)
{
    if (x == y) {
        return false;
    }
    if (x == nullptr || y == nullptr || x->kind != y->kind) {
        return true;
    }
    if (embi_value_identical(x, y)) {
        return false; /* CF1: equal field by field */
    }
    if (embi_value_clearly_differs(x, y)) {
        return true;
    }
    embi_sb p = {0}, q = {0};
    embi_value_serialize(&p, x);
    embi_value_serialize(&q, y);
    bool differ = !(p.len == q.len && (p.len == 0 || memcmp(p.data, q.data, p.len) == 0));
    embi_sb_free(&p);
    embi_sb_free(&q);
    return differ;
}

uint32_t embi_style_diff_props(const embi_style *a, const embi_style *b, embi_prop_id *out, uint32_t cap)
{
    const embi_layout *L = embi_layout_get();
    uint32_t n = 0;
    for (uint32_t g = 0; g < L->ngroups && n < cap; g++) {
        const embi_style_group *ga = a->groups[g], *gb = b->groups[g];
        if (ga == gb) {
            continue;
        }
        for (uint32_t i = 0; i < L->n[g] && n < cap; i++) {
            if (diff_value(ga->vals[i], gb->vals[i])) {
                out[n++] = L->props[g][i];
            }
        }
    }
    return n;
}
