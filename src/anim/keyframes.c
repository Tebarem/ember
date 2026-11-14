/*
 * Keyframes (W6a): keyframe sets built from flat entries (Web Animations and CSS @keyframes),
 * per-property tracks sorted by offset with the implicit 0% and 100% keyframes, the @keyframes
 * table of a sheet list with cascade-layer aware name resolution, and sampling.
 */
#include "anim/anim.h"

#include <math.h>
#include <string.h>

#define EMBI_KF_DEPTH_MAX 64

struct embi_kf_entry {
    double offset;
    embi_value value; /* kind EMBI_V_NONE: the underlying value */
    const embi_easing *easing;
    uint8_t comp; /* 0: the effect's composite; else 1 + embi_composite (the keyframe's own composite) */
};

typedef struct embi_kf_track {
    embi_prop_id prop;
    uint32_t n;
    struct embi_kf_entry *e;
} embi_kf_track;

struct embi_kf_set {
    uint32_t ndropped; /* declarations left out of a collected rule (var(), CSS-wide keywords) */
    uint32_t n;
    embi_kf_track *t;
};

static void *anew(embi_arena *a, size_t n)
{
    void *p = embi_arena_alloc(a, n, 8);
    memset(p, 0, n);
    return p;
}

/* ------------------------------------------------------------------ building */

/* Offsets of the keyframes (Web Animations, computed over the whole list). Entries that share a
 * key are one keyframe. A keyframe's offset is the first explicit offset of its entries. Keyframes
 * without one are spaced evenly between the known ones; the first without one is at 0 and the last
 * at 1. koff[i] is the offset of keyframe i, NaN when unspecified. */
static void fill_keyframe_offsets(double *koff, uint32_t nk)
{
    if (nk == 0) {
        return;
    }
    bool first_nan = isnan(koff[0]);
    bool last_nan = isnan(koff[nk - 1]);
    if (first_nan) {
        koff[0] = 0.0;
    }
    if (last_nan) {
        koff[nk - 1] = 1.0;
    }
    uint32_t i = 0;
    while (i < nk) {
        if (!isnan(koff[i])) {
            i++;
            continue;
        }
        uint32_t run_end = i;
        while (run_end < nk && isnan(koff[run_end])) {
            run_end++;
        }
        double lo = koff[i - 1];
        double hi = koff[run_end];
        uint32_t cnt = run_end - i;
        for (uint32_t k = 0; k < cnt; k++) {
            koff[i + k] = lo + (hi - lo) * (double)(k + 1) / (double)(cnt + 1);
        }
        i = run_end;
    }
}

static void sort_entries(struct embi_kf_entry *e, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        struct embi_kf_entry x = e[i];
        uint32_t j = i;
        while (j > 0 && e[j - 1].offset > x.offset) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = x;
    }
}

embi_kf_set *embi_kf_build(embi_arena *a, const embi_kf_input *in, uint32_t n)
{
    embi_kf_set *s = anew(a, sizeof *s);
    if (n == 0) {
        return s;
    }
    embi_prop_id *props = embi_arena_alloc(a, sizeof(embi_prop_id) * n, 8);
    uint32_t np = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (in[i].prop == EMBI_PROP_NONE) {
            continue;
        }
        bool seen = false;
        for (uint32_t k = 0; k < np && !seen; k++) {
            seen = props[k] == in[i].prop;
        }
        if (!seen) {
            props[np++] = in[i].prop;
        }
    }
    if (np == 0) {
        return s;
    }

    /* Keyframe offsets over the whole list, before the split by property. */
    uint32_t *keys = embi_arena_alloc(a, sizeof(uint32_t) * n, 8);
    double *koff = embi_arena_alloc(a, sizeof(double) * n, 8);
    uint32_t *kidx = embi_arena_alloc(a, sizeof(uint32_t) * n, 8);
    uint32_t nk = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = 0;
        while (k < nk && keys[k] != in[i].key) {
            k++;
        }
        if (k == nk) {
            keys[nk] = in[i].key;
            koff[nk] = NAN;
            nk++;
        }
        if (isnan(koff[k]) && !isnan(in[i].offset)) {
            koff[k] = in[i].offset;
        }
        kidx[i] = k;
    }
    fill_keyframe_offsets(koff, nk);

    s->n = np;
    s->t = anew(a, sizeof(embi_kf_track) * np);
    for (uint32_t k = 0; k < np; k++) {
        /* Entries of this property in input order, then the sort. */
        uint32_t cnt = 0;
        for (uint32_t i = 0; i < n; i++) {
            cnt += in[i].prop == props[k] ? 1u : 0u;
        }
        /* Room for the two implicit keyframes. */
        struct embi_kf_entry *e = embi_arena_alloc(a, sizeof(struct embi_kf_entry) * (cnt + 2), 8);
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (in[i].prop != props[k]) {
                continue;
            }
            e[m].offset = isnan(in[i].offset) ? koff[kidx[i]] : in[i].offset;
            e[m].value = in[i].value;
            e[m].easing = in[i].easing;
            e[m].comp = in[i].comp;
            m++;
        }
        sort_entries(e, m);
        /* Implicit 0% and 100%: the underlying value. */
        if (m > 0 && e[0].offset > 0.0) {
            memmove(&e[1], &e[0], sizeof(struct embi_kf_entry) * m);
            memset(&e[0], 0, sizeof e[0]);
            e[0].offset = 0.0;
            m++;
        }
        if (m > 0 && e[m - 1].offset < 1.0) {
            memset(&e[m], 0, sizeof e[m]);
            e[m].offset = 1.0;
            m++;
        }
        s->t[k].prop = props[k];
        s->t[k].n = m;
        s->t[k].e = e;
    }
    return s;
}

uint32_t embi_kf_prop_count(const embi_kf_set *s)
{
    return s->n;
}

embi_prop_id embi_kf_prop_at(const embi_kf_set *s, uint32_t i)
{
    return s->t[i].prop;
}

bool embi_kf_has_prop(const embi_kf_set *s, embi_prop_id prop)
{
    for (uint32_t k = 0; k < s->n; k++) {
        if (s->t[k].prop == prop) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ sampling */

/* The endpoint value v, with currentcolor resolved when cur is given. buf holds the copy. */
static const embi_value *endpoint(embi_arena *a, const embi_value *v, const embi_color *cur,
                                  embi_value *buf)
{
    if (v == nullptr || cur == nullptr) {
        return v;
    }
    embi_anim_resolve_color(a, v, cur, buf);
    return buf;
}

bool embi_kf_sample(const embi_kf_set *s, embi_prop_id prop, double t, const embi_value *base,
                    const embi_easing *def, const embi_color *current, embi_arena *a,
                    embi_value *out)
{
    return embi_kf_sample_c(s, prop, t, base, def, current, a, EMBI_COMP_REPLACE, out);
}

/* The value of an entry: the underlying value for an implicit keyframe, else the keyframe value composited onto the
 * underlying one when its composite (its own, or the effect's) is add or accumulate. buf holds a composited copy. */
static const embi_value *entry_value(embi_arena *a, embi_prop_id prop, const struct embi_kf_entry *x,
                                     const embi_value *base, uint8_t ecomp, embi_value *buf)
{
    if (x->value.kind == EMBI_V_NONE) {
        return base;
    }
    uint8_t comp = x->comp != 0 ? (uint8_t)(x->comp - 1u) : ecomp;
    if (comp != EMBI_COMP_REPLACE && base != nullptr && base->kind != EMBI_V_NONE &&
        embi_anim_add_scaled(a, prop, base, &x->value, 1.0, buf)) {
        return buf;
    }
    return &x->value;
}

bool embi_kf_sample_c(const embi_kf_set *s, embi_prop_id prop, double t, const embi_value *base,
                      const embi_easing *def, const embi_color *current, embi_arena *a, uint8_t ecomp,
                      embi_value *out)
{
    const embi_kf_track *tr = nullptr;
    for (uint32_t k = 0; k < s->n && tr == nullptr; k++) {
        if (s->t[k].prop == prop) {
            tr = &s->t[k];
        }
    }
    if (tr == nullptr || tr->n == 0) {
        return false;
    }
    const struct embi_kf_entry *e = tr->e;
    uint32_t n = tr->n;
    /* The last entry whose offset is at or before t (entries are sorted). */
    int64_t i = -1;
    for (uint32_t k = 0; k < n && e[k].offset <= t; k++) {
        i = (int64_t)k;
    }
    embi_value buf;
    /* Outside 0..1 the first or last interval is extended (extrapolation). */
    bool extrap = n >= 2 && (t < 0.0 || t > 1.0);
    if (extrap) {
        /* The nearest two entries with distinct offsets, so a shared offset does not give a
         * zero-width interval. */
        if (t < 0.0) {
            i = 0;
            while (i + 2 < (int64_t)n && e[i + 1].offset <= e[i].offset) {
                i++;
            }
        } else {
            i = (int64_t)n - 2;
            while (i > 0 && e[i + 1].offset <= e[i].offset) {
                i--;
            }
        }
    }
    if (!extrap && (i < 0 || i == (int64_t)n - 1)) {
        embi_value cbuf;
        const embi_value *v = entry_value(a, prop, &e[i < 0 ? 0 : i], base, ecomp, &cbuf);
        if (v == nullptr || v->kind == EMBI_V_NONE) {
            return false;
        }
        *out = *endpoint(a, v, current, &buf);
        return true;
    }
    uint32_t j = (uint32_t)i + 1;
    embi_value cbufa, cbufb;
    const embi_value *va = entry_value(a, prop, &e[i], base, ecomp, &cbufa);
    const embi_value *vb = entry_value(a, prop, &e[j], base, ecomp, &cbufb);
    if (va == nullptr) {
        va = vb;
    }
    if (vb == nullptr) {
        vb = va;
    }
    if (va == nullptr || va->kind == EMBI_V_NONE || vb->kind == EMBI_V_NONE) {
        return false;
    }
    embi_value bufa, bufb;
    va = endpoint(a, va, current, &bufa);
    vb = endpoint(a, vb, current, &bufb);
    double span = e[j].offset - e[i].offset;
    double local = span > 0.0 ? (t - e[i].offset) / span : 1.0;
    if (!extrap) {
        local = local < 0.0 ? 0.0 : (local > 1.0 ? 1.0 : local);
    }
    const embi_easing *ease = e[i].easing != nullptr ? e[i].easing : def;
    embi_anim_mix(a, prop, va, vb, embi_easing_eval(ease, local), out);
    return true;
}

/* ------------------------------------------------------------------ @keyframes collection */

typedef struct kf_layer {
    embi_atom name;
    uint32_t first, last, next; /* children, in order of first appearance */
    uint32_t rank;
} kf_layer;

typedef struct kf_rec {
    embi_atom name;
    uint32_t node;
    uint64_t seq;
    const embi_kf_set *set;
    const embi_css_rule *rule;
} kf_rec;

typedef struct kf_walk {
    embi_arena *a;
    const embi_style_env *env;
    kf_layer *nodes;
    uint32_t nn, capn;
    kf_rec *recs;
    uint32_t nr, capr;
    uint64_t seq;
} kf_walk;

static uint32_t layer_new(kf_walk *w, uint32_t parent, embi_atom name)
{
    if (w->nn == w->capn) {
        uint32_t cap = w->capn == 0 ? 16u : w->capn * 2u;
        w->nodes = embi_realloc(w->nodes, sizeof(kf_layer) * w->capn, sizeof(kf_layer) * cap);
        w->capn = cap;
    }
    uint32_t id = w->nn++;
    kf_layer *nd = &w->nodes[id];
    nd->name = name;
    nd->first = nd->last = nd->next = 0;
    nd->rank = 0;
    if (id != parent) {
        kf_layer *p = &w->nodes[parent];
        if (p->first == 0) {
            p->first = id;
        } else {
            w->nodes[p->last].next = id;
        }
        p->last = id;
    }
    return id;
}

/* The child of parent with this name (0 creates an anonymous layer). Node 0 is the root. */
static uint32_t layer_child(kf_walk *w, uint32_t parent, embi_atom name)
{
    if (name != 0) {
        for (uint32_t c = w->nodes[parent].first; c != 0; c = w->nodes[c].next) {
            if (w->nodes[c].name == name) {
                return c;
            }
        }
    }
    return layer_new(w, parent, name);
}

/* A possibly dotted layer name: `a.b` is sublayer b of layer a. */
static uint32_t layer_path(kf_walk *w, uint32_t parent, embi_atom name)
{
    if (name == 0) {
        return layer_child(w, parent, 0);
    }
    embi_str s = embi_atom_str(name);
    uint32_t node = parent;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.len; i++) {
        if (i == s.len || s.ptr[i] == '.') {
            if (i > start) {
                embi_atom seg = embi_atom_intern(s.ptr + start, i - start);
                node = layer_child(w, node, seg);
            }
            start = i + 1;
        }
    }
    return node == parent ? layer_child(w, parent, name) : node;
}

static bool is_keyframe_ident(embi_str s, bool from)
{
    return from ? embi_str_ieq(s, EMBI_STR("from")) : embi_str_ieq(s, EMBI_STR("to"));
}

/* Offsets named by one keyframe selector list: percentages, from and to. Returns the count. */
static uint32_t keyframe_offsets(embi_css_slice sel, double *out, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sel.n; i++) {
        const embi_css_cv *c = &sel.p[i];
        double off;
        if (c->tok.type == EMBI_CSS_PERCENTAGE) {
            /* A percentage outside 0%..100% makes the whole keyframe rule invalid (it is ignored). */
            if (!(c->tok.num >= 0.0 && c->tok.num <= 100.0)) {
                return 0;
            }
            off = c->tok.num / 100.0;
        } else if (c->tok.type == EMBI_CSS_IDENT && is_keyframe_ident(c->tok.value, true)) {
            off = 0.0;
        } else if (c->tok.type == EMBI_CSS_IDENT && is_keyframe_ident(c->tok.value, false)) {
            off = 1.0;
        } else if (c->tok.type == EMBI_CSS_WS || c->tok.type == EMBI_CSS_COMMA) {
            continue;
        } else {
            return 0; /* not a keyframe selector */
        }
        if (n < cap) {
            out[n++] = off;
        }
    }
    return n;
}

/* Adds one declaration at an offset. A later declaration of the same property at the same offset
 * (a repeated selector or a repeated property) replaces the earlier one: the cascade within a
 * keyframe rule takes the last value. */
static void add_input(kf_walk *w, embi_kf_input **in, uint32_t *n, uint32_t *cap, double off,
                      embi_prop_id prop, const embi_value *v, const embi_easing *ease, uint32_t key)
{
    (void)w;
    for (uint32_t i = 0; i < *n; i++) {
        embi_kf_input *x = &(*in)[i];
        if (x->prop == prop && x->offset == off) {
            x->value = *v;
            x->easing = ease;
            return;
        }
    }
    if (*n == *cap) {
        uint32_t nc = *cap == 0 ? 16u : *cap * 2u;
        *in = embi_realloc(*in, sizeof(embi_kf_input) * *cap, sizeof(embi_kf_input) * nc);
        *cap = nc;
    }
    embi_kf_input *x = &(*in)[(*n)++];
    x->offset = off;
    x->prop = prop;
    x->value = *v;
    x->easing = ease;
    x->key = key;
    x->comp = 0;
}

static bool is_custom_name(embi_str s)
{
    return s.len >= 2 && s.ptr[0] == '-' && s.ptr[1] == '-';
}

static bool is_css_wide(uint8_t kind)
{
    return kind == EMBI_V_INITIAL || kind == EMBI_V_INHERIT || kind == EMBI_V_UNSET ||
           kind == EMBI_V_REVERT || kind == EMBI_V_REVERT_LAYER;
}

/* The set of one @keyframes rule: the keyframes are its style rules. !important declarations and
 * custom properties are ignored; so are non-animatable properties, CSS-wide keywords (initial,
 * inherit, ...) and declarations containing var(): none of those can be resolved without an
 * element's cascade, so W6c must substitute them itself: embi_kf_dropped_count says whether any were left out
 * and embi_kf_table_find_rule returns the winning raw rule to re-walk, then it rebuilds the set
 * through embi_kf_build from its own resolved inputs. */
static uint32_t rule_inputs(kf_walk *w, const embi_css_rule *r, bool keep_wide, embi_kf_input **in_out,
                            uint32_t *ndropped_out)
{
    uint32_t ndropped = 0;
    embi_kf_input *in = nullptr;
    uint32_t n = 0, cap = 0;
    const embi_prop_id timing = embi_prop_lookup(EMBI_STR("animation-timing-function"));
    for (uint32_t k = 0; k < r->nkids; k++) {
        const embi_css_rule *kf = &r->kids[k];
        if (kf->kind != EMBI_CSS_RULE_STYLE) {
            continue;
        }
        double offs[64];
        uint32_t no = keyframe_offsets(kf->prelude, offs, 64);
        if (no == 0) {
            continue;
        }
        /* Each declaration is parsed once; the keyframe's own timing function applies to all of
         * its properties, so the results are kept until the whole block has been read. */
        embi_longhand *parsed = nullptr;
        uint32_t np = 0, pcap = 0;
        const embi_easing *kf_ease = nullptr;
        for (uint32_t d = 0; d < kf->ndecls; d++) {
            const embi_css_decl *dc = &kf->decls[d];
            embi_str nm = embi_atom_str(dc->name);
            embi_prop_id prop = is_custom_name(nm) ? EMBI_PROP_NONE : embi_prop_lookup(nm);
            if (dc->important || prop == EMBI_PROP_NONE) {
                continue;
            }
            if (embi_css_has_var(dc->value)) {
                ndropped++;
                continue;
            }
            embi_longhand lh[EMBI_LONGHAND_MAX];
            uint32_t cnt = embi_css_parse_decl(w->a, prop, dc->value, lh, EMBI_LONGHAND_MAX);
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t items = 0;
                const embi_value *first = embi_value_items(&lh[j].value, &items);
                if (lh[j].prop == timing) {
                    if (items > 0) {
                        (void)embi_easing_of_value(first, &kf_ease);
                    }
                    continue;
                }
                /* CSS-wide keywords never resolve here (the cascade is not available): dropped. */
                if (!embi_anim_animatable(lh[j].prop)) {
                    continue;
                }
                if (is_css_wide(lh[j].value.kind) && !keep_wide) {
                    ndropped++;
                    continue;
                }
                if (np == pcap) {
                    uint32_t nc = pcap == 0 ? 8u : pcap * 2u;
                    parsed = embi_realloc(parsed, sizeof(embi_longhand) * pcap,
                                          sizeof(embi_longhand) * nc);
                    pcap = nc;
                }
                parsed[np++] = lh[j];
            }
        }
        for (uint32_t j = 0; j < np; j++) {
            for (uint32_t o = 0; o < no; o++) {
                add_input(w, &in, &n, &cap, offs[o], parsed[j].prop, &parsed[j].value, kf_ease, k);
            }
        }
        embi_free(parsed, sizeof(embi_longhand) * pcap);
    }
    embi_kf_input *res = nullptr;
    if (n > 0) {
        res = embi_arena_alloc(w->a, sizeof(embi_kf_input) * n, 8);
        memcpy(res, in, sizeof(embi_kf_input) * n);
    }
    embi_free(in, sizeof(embi_kf_input) * cap);
    *in_out = res;
    *ndropped_out = ndropped;
    return n;
}

static const embi_kf_set *build_rule_set(kf_walk *w, const embi_css_rule *r)
{
    embi_kf_input *in = nullptr;
    uint32_t ndropped = 0;
    uint32_t n = rule_inputs(w, r, false, &in, &ndropped);
    embi_kf_set *set = embi_kf_build(w->a, in, n);
    set->ndropped = ndropped;
    return set;
}

uint32_t embi_kf_rule_inputs(embi_arena *a, const embi_css_rule *r, embi_kf_input **out)
{
    kf_walk w;
    memset(&w, 0, sizeof w);
    w.a = a;
    uint32_t dropped = 0;
    return rule_inputs(&w, r, true, out, &dropped);
}

static void walk_rules(kf_walk *w, const embi_css_rule *rules, uint32_t n, uint32_t node,
                       uint32_t depth);

static void walk_rules_for_sheet(kf_walk *w, const embi_css_sheet *sheet, uint32_t node,
                                 uint32_t depth)
{
    walk_rules(w, sheet->rules, sheet->nrules, node, depth + 1);
}

static void walk_rules(kf_walk *w, const embi_css_rule *rules, uint32_t n, uint32_t node,
                       uint32_t depth)
{
    if (depth > EMBI_KF_DEPTH_MAX) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_rule *r = &rules[i];
        switch (r->kind) {
        case EMBI_CSS_RULE_KEYFRAMES: {
            const embi_kf_set *set = build_rule_set(w, r);
            if (w->nr == w->capr) {
                uint32_t nc = w->capr == 0 ? 8u : w->capr * 2u;
                w->recs = embi_realloc(w->recs, sizeof(kf_rec) * w->capr, sizeof(kf_rec) * nc);
                w->capr = nc;
            }
            w->recs[w->nr++] = (kf_rec){r->name, node, w->seq++, set, r};
            break;
        }
        case EMBI_CSS_RULE_MEDIA:
            if (embi_style_media_match(r->prelude, w->env)) {
                walk_rules(w, r->kids, r->nkids, node, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_SUPPORTS:
            if (r->cond) {
                walk_rules(w, r->kids, r->nkids, node, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_LAYER:
            if (r->block) {
                uint32_t child = r->name != 0 ? layer_path(w, node, r->name) : layer_child(w, node, 0);
                walk_rules(w, r->kids, r->nkids, child, depth + 1);
            } else {
                for (uint32_t k = 0; k < r->nlayers; k++) {
                    (void)layer_path(w, node, r->layers[k]);
                }
            }
            break;
        case EMBI_CSS_RULE_IMPORT:
            if (r->sheet != nullptr && r->cond &&
                (r->prelude.n == 0 || embi_style_media_match(r->prelude, w->env))) {
                uint32_t child = node;
                if (r->has_layer) {
                    child = layer_path(w, node, r->name);
                }
                walk_rules_for_sheet(w, r->sheet, child, depth);
            }
            break;
        default:
            break;
        }
    }
}

/* Cascade order of layers: a layer's sublayers come before its own rules, siblings by their first
 * appearance; unlayered rules come last. */
static uint32_t assign_ranks(kf_walk *w, uint32_t node, uint32_t next)
{
    for (uint32_t c = w->nodes[node].first; c != 0; c = w->nodes[c].next) {
        next = assign_ranks(w, c, next);
    }
    if (node != 0) {
        w->nodes[node].rank = ++next;
    }
    return next;
}

void embi_kf_table_collect(embi_arena *a, embi_kf_table *t, embi_css_sheet *const *sheets,
                           uint32_t n, const embi_style_env *env)
{
    kf_walk w;
    memset(&w, 0, sizeof w);
    w.a = a;
    w.env = env;
    w.nodes = embi_calloc(16, sizeof(kf_layer));
    w.capn = 16;
    w.nn = 1; /* node 0: the root, unlayered */
    for (uint32_t i = 0; i < n; i++) {
        walk_rules_for_sheet(&w, sheets[i], 0, 0);
    }
    (void)assign_ranks(&w, 0, 0);
    w.nodes[0].rank = UINT32_MAX;

    for (uint32_t i = 0; i < w.nr; i++) {
        const kf_rec *rec = &w.recs[i];
        uint32_t rank = w.nodes[rec->node].rank;
        int found = -1;
        for (uint32_t k = 0; k < t->n && found < 0; k++) {
            if (t->e[k].name == rec->name) {
                found = (int)k;
            }
        }
        if (found < 0) {
            if (t->n == t->cap) {
                uint32_t nc = t->cap == 0 ? 8u : t->cap * 2u;
                t->e = embi_realloc(t->e, sizeof(embi_kf_named) * t->cap, sizeof(embi_kf_named) * nc);
                t->cap = nc;
            }
            t->e[t->n++] = (embi_kf_named){rec->name, rec->set, rank, rec->seq, rec->rule};
            continue;
        }
        embi_kf_named *cur = &t->e[found];
        if (rank > cur->rank || (rank == cur->rank && rec->seq > cur->seq)) {
            cur->set = rec->set;
            cur->rule = rec->rule;
            cur->rank = rank;
            cur->seq = rec->seq;
        }
    }
    embi_free(w.nodes, sizeof(kf_layer) * w.capn);
    embi_free(w.recs, sizeof(kf_rec) * w.capr);
}

const embi_kf_set *embi_kf_table_find(const embi_kf_table *t, embi_atom name)
{
    for (uint32_t k = 0; k < t->n; k++) {
        if (t->e[k].name == name) {
            return t->e[k].set;
        }
    }
    return nullptr;
}

const embi_css_rule *embi_kf_table_find_rule(const embi_kf_table *t, embi_atom name)
{
    for (uint32_t k = 0; k < t->n; k++) {
        if (t->e[k].name == name) {
            return t->e[k].rule;
        }
    }
    return nullptr;
}

uint32_t embi_kf_dropped_count(const embi_kf_set *s)
{
    return s != nullptr ? s->ndropped : 0;
}

void embi_kf_table_free(embi_kf_table *t)
{
    embi_free(t->e, sizeof(embi_kf_named) * t->cap);
    memset(t, 0, sizeof *t);
}
