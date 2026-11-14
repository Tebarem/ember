/*
 * The cascade (W2d, CSS Cascade 5): gathering the matching declarations of every origin, sorting
 * them by origin and importance, layer, specificity and order of appearance, choosing the winner
 * of each longhand, and computing the custom properties. The element's style is then built from
 * the computed groups (embi_style_finish_groups).
 */
#include "style_int.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ candidates */

static void push_cand(embi_sctx *sc, const embi_cand *c)
{
    if (sc->ncands == sc->capcands) {
        uint32_t cap = sc->capcands == 0 ? 64u : sc->capcands * 2u;
        sc->cands = embi_realloc(sc->cands, sizeof(embi_cand) * sc->capcands,
                                 sizeof(embi_cand) * cap);
        sc->capcands = cap;
    }
    sc->cands[sc->ncands++] = *c;
}

/* Adds one candidate per declaration. layer is the normal layer key; important declarations use
 * its reverse. */
void embi_style_push_decls(embi_sctx *sc, const embi_pdecl *decls, uint32_t n, uint8_t origin,
                           uint16_t layer, embi_sel_spec spec, uint64_t order, bool attr)
{
    for (uint32_t i = 0; i < n; i++) {
        const embi_pdecl *d = &decls[i];
        embi_cand c;
        memset(&c, 0, sizeof c);
        c.important = (d->flags & EMBI_PD_IMPORTANT) != 0;
        c.origin = origin;
        if (c.important) {
            c.oset = origin == EMBI_ORIGIN_UA ? 6 : origin == EMBI_ORIGIN_USER ? 5 : 4;
            c.layer = (uint16_t)(0xFFFFu - layer);
        } else {
            c.oset = origin == EMBI_ORIGIN_UA ? 1 : origin == EMBI_ORIGIN_USER ? 2 : 3;
            c.layer = layer;
        }
        c.attr = attr;
        c.spec = spec;
        c.order = order + i;
        c.d = d;
        push_cand(sc, &c);
    }
}

typedef struct gather_ctx {
    embi_sctx *sc;
    const embi_sset *set;
    embi_element *el;
    embi_sel_pseudo pe;
    const embi_bloom *anc;
    embi_sel_ctx *selctx;
} gather_ctx;

static void gather_visit(void *user, uint64_t payload)
{
    gather_ctx *g = user;
    const embi_sentry *en = &g->set->e[payload];
    /* An entry whose subject is another pseudo-element can never match this one: rejected before
     * the match (and it cannot make the element unsafe, since it never applies to it). */
    if (en->sel->pseudo != g->pe) {
        return;
    }
    if (en->unsafe) {
        g->sc->unsafe = true;
    }
    if (!embi_sel_match(en->sel, g->el, g->pe, g->selctx, g->anc)) {
        return;
    }
    embi_style_push_decls(g->sc, en->decls, en->ndecls, en->origin, en->layer, en->sel->spec,
                          en->order, false);
}

void embi_style_gather(embi_sctx *sc, const embi_sset *set, embi_element *el, embi_sel_pseudo pe,
                       const embi_bloom *anc, embi_sel_ctx *selctx)
{
    gather_ctx g = {.sc = sc, .set = set, .el = el, .pe = pe, .anc = anc, .selctx = selctx};
    embi_sel_index_candidates(&set->index, el, gather_visit, &g);
}

/* Sorts by the cascade order (ascending: the last candidate of a property wins). Insertion sort:
 * the lists are short. */
void embi_style_cands_sort(embi_sctx *sc)
{
    for (uint32_t i = 1; i < sc->ncands; i++) {
        embi_cand key = sc->cands[i];
        uint32_t j = i;
        while (j > 0) {
            const embi_cand *a = &sc->cands[j - 1];
            bool greater;
            if (a->oset != key.oset) {
                greater = a->oset > key.oset;
            } else if (a->attr != key.attr) {
                greater = a->attr > key.attr;
            } else if (a->layer != key.layer) {
                greater = a->layer > key.layer;
            } else {
                int c = embi_sel_spec_cmp(a->spec, key.spec);
                greater = c != 0 ? c > 0 : a->order > key.order;
            }
            if (!greater) {
                break;
            }
            sc->cands[j] = sc->cands[j - 1];
            j--;
        }
        sc->cands[j] = key;
    }
}

/* Copies an attribute value out as UTF-8 into a fresh buffer (free with embi_free). */
embi_str embi_style_attr_utf8(const embi_dstr *d, size_t *cap_out, char **buf_out)
{
    size_t cap = (size_t)d->len * 3 + 1;
    char *buf = embi_alloc(cap);
    size_t w = embi_dstr_to_utf8(*d, buf, cap);
    *cap_out = cap;
    *buf_out = buf;
    return (embi_str){buf, (uint32_t)w};
}

/* Preparsed declarations of a style attribute, cached by the text. */
void embi_style_gather_attr(embi_style_engine *e, embi_sctx *sc, embi_element *el)
{
    const embi_attr *attr = embi_el_get_attribute(el, EMBI_STR("style"));
    if (attr == nullptr || attr->value.len == 0) {
        return;
    }
    size_t cap = 0;
    char *buf = nullptr;
    embi_str text = embi_style_attr_utf8(&attr->value, &cap, &buf);
    uint64_t key = embi_hash_bytes(text.ptr, text.len, 0x5374u);
    uint64_t found = 0;
    const embi_pdecl *decls = nullptr;
    uint32_t n = 0;
    if (embi_hashmap_get(&e->hints, key, &found) &&
        ((const embi_style_declrec *)(uintptr_t)found)->text.len == text.len &&
        memcmp(((const embi_style_declrec *)(uintptr_t)found)->text.ptr, text.ptr, text.len) == 0) {
        const embi_style_declrec *rec = (const embi_style_declrec *)(uintptr_t)found;
        decls = rec->d;
        n = rec->n;
    } else {
        embi_str kept = embi_style_copy_str(&e->arena, text);
        embi_css_slice cvs = embi_css_parse_cvs(&e->arena, kept);
        embi_css_items items = embi_css_consume_declarations(&e->arena, cvs);
        embi_css_decl *ds = embi_arena_alloc(&e->arena, sizeof(embi_css_decl) * (items.n + 1), 8);
        uint32_t k = 0;
        for (uint32_t i = 0; i < items.n; i++) {
            const embi_css_item *it = &items.p[i];
            if (it->kind != EMBI_CSS_ITEM_DECL) {
                continue;
            }
            ds[k].name = embi_atom_intern(it->name.ptr, it->name.len);
            ds[k].important = it->important;
            ds[k].value = it->prelude;
            ds[k].prop = 0;
            ds[k].order = it->start;
            k++;
        }
        uint32_t cnt = 0;
        decls = embi_style_preparse(e, ds, k, &cnt);
        embi_style_declrec *rec = embi_arena_alloc(&e->arena, sizeof *rec, 8);
        rec->d = decls;
        rec->n = cnt;
        rec->text = kept;
        embi_hashmap_put(&e->hints, key, (uint64_t)(uintptr_t)rec);
        n = cnt;
    }
    embi_free(buf, cap);
    /* Author origin, unlayered, above every author rule by specificity. */
    embi_sel_spec spec = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    embi_style_push_decls(sc, decls, n, EMBI_ORIGIN_AUTHOR, EMBI_LAYER_UNLAYERED, spec,
                          UINT64_MAX - 0x100000u, true);
}

/* ------------------------------------------------------------------ custom properties */

static const embi_sreg *find_reg(const embi_sctx *sc, embi_atom name)
{
    const embi_sreg *found = nullptr;
    for (uint32_t s = 0; s < sc->nsets; s++) {
        const embi_sset *set = sc->sets[s];
        for (uint32_t i = 0; i < set->nregs; i++) {
            if (set->regs[i].name == name) {
                found = &set->regs[i];
            }
        }
    }
    return found;
}

typedef enum custom_result {
    CUST_VALUE,
    CUST_INITIAL,
    CUST_INHERIT,
    CUST_INVALID,
} custom_result;

static embi_css_slice trim_slice(embi_css_slice s)
{
    uint32_t a = 0;
    uint32_t b = s.n;
    while (a < b && s.p[a].tok.type == EMBI_CSS_WS) {
        a++;
    }
    while (b > a && s.p[b - 1].tok.type == EMBI_CSS_WS) {
        b--;
    }
    return embi_css_slice_make(s.p + a, b - a);
}

static custom_result custom_value(embi_sctx *sc, const embi_pdecl *d, const embi_sreg *reg,
                                  embi_css_slice *out)
{
    embi_css_slice raw = trim_slice(d->raw);
    if (raw.n == 1 && raw.p[0].tok.type == EMBI_CSS_IDENT) {
        embi_str kw = raw.p[0].tok.value;
        if (embi_str_ieq(kw, EMBI_STR("initial"))) {
            return CUST_INITIAL;
        }
        if (embi_str_ieq(kw, EMBI_STR("inherit"))) {
            return CUST_INHERIT;
        }
        if (embi_str_ieq(kw, EMBI_STR("unset"))) {
            return (reg == nullptr || reg->inherits) ? CUST_INHERIT : CUST_INITIAL;
        }
    }
    embi_str name = embi_atom_str(d->name);
    embi_var_env venv = {.user = sc, .lookup = embi_style_var_lookup, .max_tokens = 0};
    embi_css_slice sub;
    if (!embi_css_substitute_vars(&sc->eng->arena, &venv, name, raw, &sub)) {
        return CUST_INVALID;
    }
    if (reg != nullptr) {
        embi_value tmp;
        if (!embi_css_registered_parse(&sc->eng->arena, reg->syntax, sub, &tmp)) {
            return CUST_INVALID;
        }
    }
    *out = sub;
    return CUST_VALUE;
}

typedef embi_cust_entry cust_tmp; /* name and value */

static void cust_upsert(cust_tmp *t, uint32_t *n, embi_atom name, embi_css_slice v)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (t[i].name == name) {
            t[i].val = v;
            return;
        }
    }
    t[*n].name = name;
    t[*n].val = v;
    (*n)++;
}

static void cust_remove(cust_tmp *t, uint32_t *n, embi_atom name)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (t[i].name == name) {
            t[i] = t[*n - 1];
            (*n)--;
            return;
        }
    }
}

static bool cust_has(const cust_tmp *t, uint32_t n, embi_atom name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (t[i].name == name) {
            return true;
        }
    }
    return false;
}

static int cust_cmp(const void *a, const void *b)
{
    const cust_tmp *x = a;
    const cust_tmp *y = b;
    return x->name < y->name ? -1 : x->name > y->name ? 1 : 0;
}

/* Computes the element's custom properties into sc->ccustom. Registered properties (@property)
 * validate their values and fall back to the initial value when they do not inherit. */
static void build_custom(embi_sctx *sc)
{
    embi_style_engine *e = sc->eng;
    sc->ncw = 0;
    sc->custom_ready = false;
    for (uint32_t i = 0; i < sc->ncands; i++) {
        const embi_pdecl *d = sc->cands[i].d;
        if ((d->flags & EMBI_PD_CUSTOM) == 0) {
            continue;
        }
        uint32_t k = 0;
        while (k < sc->ncw && sc->cw[k].name != d->name) {
            k++;
        }
        if (k == sc->ncw) {
            if (sc->ncw == sc->capcw) {
                uint32_t cap = sc->capcw == 0 ? 8u : sc->capcw * 2u;
                sc->cw = embi_realloc(sc->cw, sizeof(embi_cust_win) * sc->capcw,
                                      sizeof(embi_cust_win) * cap);
                sc->capcw = cap;
            }
            sc->cw[sc->ncw].name = d->name;
            sc->ncw++;
        }
        sc->cw[k].d = d; /* the last declaration in cascade order wins */
        sc->cw[k].order = i;
    }

    bool non_inherit_regs = false;
    for (uint32_t s = 0; s < sc->nsets; s++) {
        for (uint32_t i = 0; i < sc->sets[s]->nregs; i++) {
            if (!sc->sets[s]->regs[i].inherits) {
                non_inherit_regs = true;
            }
        }
    }
    if (sc->ncw == 0 && !non_inherit_regs) {
        sc->ccustom = sc->pcustom;
        sc->custom_ready = true;
        return;
    }

    uint32_t pn = sc->pcustom != nullptr ? sc->pcustom->n : 0;
    uint32_t cap = pn + sc->ncw + 64;
    cust_tmp *tmp = embi_alloc(sizeof(cust_tmp) * cap);
    uint32_t n = 0;
    for (uint32_t i = 0; i < pn; i++) {
        tmp[n].name = sc->pcustom->e[i].name;
        tmp[n].val = sc->pcustom->e[i].val;
        n++;
    }

    for (uint32_t i = 0; i < sc->ncw; i++) {
        const embi_pdecl *d = sc->cw[i].d;
        embi_atom name = sc->cw[i].name;
        const embi_sreg *reg = find_reg(sc, name);
        embi_css_slice val;
        custom_result r = custom_value(sc, d, reg, &val);
        bool inherits = reg == nullptr || reg->inherits;
        switch (r) {
        case CUST_VALUE:
            cust_upsert(tmp, &n, name, val);
            break;
        case CUST_INITIAL:
            if (reg != nullptr && reg->has_initial) {
                cust_upsert(tmp, &n, name, reg->initial);
            } else {
                cust_remove(tmp, &n, name);
            }
            break;
        case CUST_INHERIT:
            if (!inherits) {
                if (reg->has_initial) {
                    cust_upsert(tmp, &n, name, reg->initial);
                } else {
                    cust_remove(tmp, &n, name);
                }
            } else if (!cust_has(tmp, n, name)) {
                cust_remove(tmp, &n, name);
            }
            break;
        case CUST_INVALID:
            if (!inherits) {
                if (reg->has_initial) {
                    cust_upsert(tmp, &n, name, reg->initial);
                } else {
                    cust_remove(tmp, &n, name);
                }
            } else if (!cust_has(tmp, n, name) && reg != nullptr && reg->has_initial) {
                cust_upsert(tmp, &n, name, reg->initial);
            }
            break;
        }
    }

    /* Registered properties that do not inherit are reset to their initial value on every element
     * that does not set them. */
    for (uint32_t s = 0; s < sc->nsets; s++) {
        for (uint32_t i = 0; i < sc->sets[s]->nregs; i++) {
            const embi_sreg *reg = &sc->sets[s]->regs[i];
            if (reg->inherits) {
                continue;
            }
            bool declared = false;
            for (uint32_t k = 0; k < sc->ncw && !declared; k++) {
                declared = sc->cw[k].name == reg->name;
            }
            if (declared) {
                continue;
            }
            if (reg->has_initial) {
                cust_upsert(tmp, &n, reg->name, reg->initial);
            } else {
                cust_remove(tmp, &n, reg->name);
            }
        }
    }

    qsort(tmp, n, sizeof(cust_tmp), cust_cmp);
    sc->ccustom = embi_style_custom_intern(e, tmp, n); /* CF1: the same map as the last restyle when nothing changed */
    embi_free(tmp, sizeof(cust_tmp) * cap);
    sc->custom_ready = true;
}

/* ------------------------------------------------------------------ winners */

/* Records each candidate's longhands as winners of their targets. only_wm_dir restricts the pass
 * to writing-mode and direction, which the logical mapping depends on. */
static void stamp_pass(embi_sctx *sc, bool only_wm_dir)
{
    for (uint32_t i = 0; i < sc->ncands; i++) {
        uint16_t ids[EMBI_PROP_COUNT];
        uint32_t n = embi_style_origs(sc->cands[i].d, ids, EMBI_PROP_COUNT);
        for (uint32_t k = 0; k < n; k++) {
            uint16_t o = ids[k];
            uint16_t t;
            if (only_wm_dir) {
                if (o != EMBI_PROP_writing_mode && o != EMBI_PROP_direction) {
                    continue;
                }
                t = o;
            } else {
                t = embi_style_target(sc, o);
            }
            if (sc->win_gen[t] != sc->gen) {
                sc->win_gen[t] = sc->gen;
                sc->targets[sc->ntargets++] = t;
            }
            sc->win[t].cand = (int32_t)i;
            sc->win[t].orig = o;
        }
    }
}

/* Builds the element's style from the computed values. Clean groups are shared with the parent
 * (inherited groups) or with the initial style (reset groups). */
embi_style *embi_style_finish_groups(embi_style_engine *e, embi_sctx *sc)
{
    const embi_layout *L = embi_layout_get();
    const embi_style_group *groups[EMBI_STYLE_MAX_GROUPS];
    for (uint32_t g = 0; g < L->ngroups; g++) {
        const embi_style *base = L->inherited[g] && sc->parent != nullptr ? sc->parent : sc->init;
        const embi_style_group *bg = base != nullptr ? base->groups[g] : nullptr;
        uint32_t n = L->n[g];
        bool differs = false;
        if ((sc->dirty & (1ull << g)) != 0 && bg != nullptr && bg->n == n) {
            /* A group whose written values all equal its base is the base group. */
            for (uint32_t s = 0; s < n && !differs; s++) {
                uint16_t p = L->props[g][s];
                differs = sc->ovr_gen[p] == sc->gen && sc->ovr[p] != bg->vals[s];
            }
        }
        if ((sc->dirty & (1ull << g)) != 0 && (differs || bg == nullptr || bg->n != n)) {
            const embi_value *vals[512];
            uint64_t xh = 0;
            if (bg != nullptr && bg->n == n) {
                /* The base group differs from the element only in the slots the cascade set. */
                memcpy(vals, bg->vals, sizeof(void *) * n);
                xh = bg->xh;
                for (uint32_t s = 0; s < n; s++) {
                    uint16_t p = L->props[g][s];
                    if (sc->ovr_gen[p] == sc->gen && sc->ovr[p] != vals[s]) {
                        xh ^= embi_style_slot_mix(s, vals[s]) ^ embi_style_slot_mix(s, sc->ovr[p]);
                        vals[s] = sc->ovr[p];
                    }
                }
            } else {
                for (uint32_t s = 0; s < n; s++) {
                    uint16_t p = L->props[g][s];
                    if (sc->ovr_gen[p] == sc->gen) {
                        vals[s] = sc->ovr[p];
                    } else {
                        vals[s] = L->inherited[g] ? embi_style_parent_value(sc, p)
                                                  : embi_style_initial_value(sc, p);
                    }
                    xh ^= embi_style_slot_mix(s, vals[s]);
                }
            }
            groups[g] = embi_style_group_intern_xh(e, g, vals, xh);
        } else {
            embi_style_group *shared = (embi_style_group *)bg;
            shared->refs++;
            groups[g] = shared;
        }
    }
    return embi_style_intern(e, groups, sc->ccustom);
}

/* The cascade and computation of one element. The caller gathered the candidates (and the
 * presentational hints and style attribute). Returns the element's style, owned by the caller. */
embi_style *embi_style_cascade_finish(embi_style_engine *e, embi_sctx *sc)
{
    sc->gen++;
    sc->dirty = 0;
    sc->ntargets = 0;
    sc->dark = false;
    sc->wm = EMBI_KW_horizontal_tb;
    sc->dir = EMBI_KW_ltr;
    embi_style_cands_sort(sc);
    build_custom(sc);

    /* Pass A: writing mode and direction. */
    stamp_pass(sc, true);
    embi_style_compute_target(sc, EMBI_PROP_writing_mode);
    embi_style_compute_target(sc, EMBI_PROP_direction);
    {
        const embi_value *wm = embi_style_elem_value(sc, EMBI_PROP_writing_mode);
        const embi_value *dir = embi_style_elem_value(sc, EMBI_PROP_direction);
        if (wm->kind == EMBI_V_KEYWORD) {
            sc->wm = wm->kw;
        }
        if (dir->kind == EMBI_V_KEYWORD) {
            sc->dir = dir->kw;
        }
    }

    /* Pass B: every longhand, logical ones mapped to their physical property. The last candidate
     * in cascade order wins. */
    stamp_pass(sc, false);

    /* Font size and the used color scheme come before any value that depends on them. */
    embi_style_compute_target(sc, EMBI_PROP_font_size);
    /* The face the font-relative units (ex, cap, ch, ic) are measured in is the one these pick. */
    embi_style_compute_target(sc, EMBI_PROP_font_family);
    embi_style_compute_target(sc, EMBI_PROP_font_weight);
    embi_style_compute_target(sc, EMBI_PROP_font_style);
    embi_style_compute_target(sc, EMBI_PROP_font_stretch);
    embi_style_compute_target(sc, EMBI_PROP_color_scheme);
    sc->dark = embi_style_scheme_dark(sc, embi_style_elem_value(sc, EMBI_PROP_color_scheme));

    for (uint32_t i = 0; i < sc->ntargets; i++) {
        embi_style_compute_target(sc, sc->targets[i]);
    }

    embi_style_fix_display(sc);
    embi_style_fix_widths(sc);
    embi_style_fix_text_align(sc);
    return embi_style_finish_groups(e, sc);
}
