/*
 * Flattening of sheets into rule entries (W2d). Each style rule becomes one entry per complex
 * selector it compiles to, with its declarations preparsed. Conditional rules (@media, @supports,
 * @import conditions) are evaluated here, and @layer assigns each entry a layer rank. CSS Nesting
 * is flattened by compiling every nested selector against its parent's compiled list (the `&`
 * becomes an :is() of the parent), so the entries are standalone.
 */
#include "style_int.h"

#include <string.h>

/* ------------------------------------------------------------------ flatten context */

/* One layer of the cascade-layer tree. Node 0 is the root (no layer). Children are kept in the
 * order they first appear. */
typedef struct embi_lnode {
    embi_atom name;      /* 0 for an anonymous layer */
    uint32_t first_child, last_child, next;
    uint32_t rank;       /* the cascade rank, assigned after the whole set is flattened */
} embi_lnode;

typedef struct embi_flat {
    embi_style_engine *eng;
    embi_sset *set;
    uint8_t origin;
    const embi_style_env *env;
    uint64_t seq;
    embi_sel_nsmap *ns;
    uint32_t nns, capns;
    embi_hashmap layers; /* (parent node << 32 | name atom) -> node */
    embi_lnode *nodes;
    uint32_t nnodes, capnodes;
} embi_flat;

/* ------------------------------------------------------------------ small helpers */

static bool contains_amp(embi_css_slice s)
{
    for (uint32_t i = 0; i < s.n; i++) {
        const embi_css_cv *c = &s.p[i];
        if (c->tok.type == EMBI_CSS_DELIM && c->tok.cp == '&') {
            return true;
        }
        if (c->nkids > 0 && contains_amp(embi_css_slice_make(c->kids, c->nkids))) {
            return true;
        }
    }
    return false;
}

/* Nested selectors without `&` get an implicit `& ` prefix (CSS Nesting 1). */
static embi_css_slice with_implicit_amp(embi_arena *a, embi_css_slice s)
{
    embi_css_cv *cv = embi_arena_alloc(a, sizeof(embi_css_cv) * (size_t)(s.n + 2), 8);
    memset(cv, 0, sizeof(embi_css_cv) * (size_t)(s.n + 2));
    cv[0].tok.type = EMBI_CSS_DELIM;
    cv[0].tok.cp = '&';
    cv[0].tok.start = 0;
    cv[0].tok.end = 1;
    cv[0].end = 1;
    cv[1].tok.type = EMBI_CSS_WS;
    cv[1].tok.start = 1;
    cv[1].tok.end = 2;
    cv[1].end = 2;
    for (uint32_t i = 0; i < s.n; i++) {
        cv[i + 2] = s.p[i];
    }
    return embi_css_slice_make(cv, s.n + 2);
}

/* The layer named name (0: a new anonymous layer) inside parent. A named layer is created once
 * per parent. Returns its node index (at least 1). */
static uint32_t layer_child(embi_flat *f, uint32_t parent, embi_atom name)
{
    uint64_t key = ((uint64_t)parent << 32) | name;
    uint64_t found = 0;
    if (name != 0 && embi_hashmap_get(&f->layers, key, &found)) {
        return (uint32_t)found;
    }
    if (f->nnodes == f->capnodes) {
        uint32_t cap = f->capnodes == 0 ? 16u : f->capnodes * 2u;
        f->nodes = embi_realloc(f->nodes, sizeof(embi_lnode) * f->capnodes,
                                sizeof(embi_lnode) * cap);
        f->capnodes = cap;
    }
    uint32_t id = f->nnodes++;
    embi_lnode *nd = &f->nodes[id];
    memset(nd, 0, sizeof *nd);
    nd->name = name;
    if (f->nodes[parent].last_child != 0) {
        f->nodes[f->nodes[parent].last_child].next = id;
    } else {
        f->nodes[parent].first_child = id;
    }
    f->nodes[parent].last_child = id;
    if (name != 0) {
        embi_hashmap_put(&f->layers, key, id);
    }
    return id;
}

/* The layer node for a possibly dotted name (`a.b` is sublayer b of layer a), walked down from
 * parent. An anonymous layer (name 0) is a fresh child. */
static uint32_t layer_path(embi_flat *f, uint32_t parent, embi_atom name)
{
    if (name == 0) {
        return layer_child(f, parent, 0);
    }
    embi_str s = embi_atom_str(name);
    uint32_t node = parent;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.len; i++) {
        if (i == s.len || s.ptr[i] == '.') {
            if (i > start) {
                embi_atom seg = embi_atom_intern(s.ptr + start, i - start);
                node = layer_child(f, node, seg);
            }
            start = i + 1;
        }
    }
    return node == parent ? layer_child(f, parent, name) : node;
}

/* Cascade 5 layer order: a layer's sublayers come before the layer's own rules, and siblings are
 * ordered by their first appearance. Ranks are assigned in that depth-first order. */
static uint32_t assign_ranks(embi_flat *f, uint32_t node, uint32_t next_rank)
{
    for (uint32_t c = f->nodes[node].first_child; c != 0; c = f->nodes[c].next) {
        next_rank = assign_ranks(f, c, next_rank);
    }
    if (node != 0) {
        f->nodes[node].rank = ++next_rank;
    }
    return next_rank;
}

/* ------------------------------------------------------------------ declarations */

/* Preparses a declaration block. The result lives in the engine arena. */
static const embi_pdecl *preparse(embi_flat *f, const embi_css_decl *decls, uint32_t n,
                                  uint32_t *count)
{
    embi_style_engine *e = f->eng;
    embi_pdecl *out = embi_arena_alloc(&e->arena, sizeof(embi_pdecl) * (size_t)(n + 1), 8);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_decl *d = &decls[i];
        embi_str nm = embi_atom_str(d->name);
        embi_pdecl pd;
        memset(&pd, 0, sizeof pd);
        if (d->important) {
            pd.flags |= EMBI_PD_IMPORTANT;
        }
        if (embi_str_is_custom(nm)) {
            pd.flags |= EMBI_PD_CUSTOM;
            pd.name = d->name;
            pd.raw = d->value;
            out[k++] = pd;
            continue;
        }
        embi_prop_id prop = embi_prop_lookup(nm);
        if (prop == EMBI_PROP_NONE) {
            int wk = embi_str_ieq(nm, EMBI_STR("all")) ? embi_css_all_keyword(d->value) : 0;
            if (wk != 0) {
                embi_value kv = {0};
                kv.kind = (uint8_t)wk;
                embi_plh *pl = embi_arena_alloc(&e->arena, sizeof(embi_plh), 8);
                pl[0].prop = 0;
                pl[0].val = embi_style_canon(e, &kv);
                pd.flags |= EMBI_PD_ALL;
                pd.lh = pl;
                pd.nlh = 1;
                out[k++] = pd;
            }
            continue;
        }
        pd.prop = prop;
        if (embi_css_has_attr(d->value)) {
            if (f->set != nullptr) {
                f->set->has_attrfn = true; /* W6b: a changed attribute can change the style (inval.c) */
            } else {
                e->attrfn_inline = true;
            }
        }
        if (embi_css_has_var(d->value) || (prop != EMBI_PROP_content && embi_css_has_attr(d->value))) {
            pd.flags |= EMBI_PD_HAS_VAR;
            pd.raw = d->value;
            out[k++] = pd;
            continue;
        }
        embi_longhand lh[EMBI_LONGHAND_MAX];
        uint32_t cnt = embi_css_parse_decl(&e->arena, prop, d->value, lh, EMBI_LONGHAND_MAX);
        if (cnt == 0) {
            continue; /* invalid for its property: dropped */
        }
        embi_plh *pl = embi_arena_alloc(&e->arena, sizeof(embi_plh) * cnt, 8);
        for (uint32_t j = 0; j < cnt; j++) {
            pl[j].prop = lh[j].prop;
            pl[j].val = embi_style_canon(e, &lh[j].value);
        }
        pd.lh = pl;
        pd.nlh = cnt;
        out[k++] = pd;
    }
    *count = k;
    return out;
}

/* ------------------------------------------------------------------ entries */

static void add_entry(embi_flat *f, const embi_sel_complex *sel, const embi_pdecl *pds,
                      uint32_t nd, uint64_t order, uint16_t layer)
{
    embi_sset *set = f->set;
    if (set->n == set->cap) {
        uint32_t cap = set->cap == 0 ? 64u : set->cap * 2u;
        embi_sentry *ne = embi_realloc(set->e, sizeof(embi_sentry) * set->cap,
                                       sizeof(embi_sentry) * cap);
        set->e = ne;
        set->cap = cap;
    }
    embi_sentry *en = &set->e[set->n];
    en->sel = sel;
    en->decls = pds;
    en->ndecls = nd;
    en->order = order;
    en->layer = layer;
    en->origin = f->origin;
    en->unsafe = embi_style_sel_unsafe(sel);
    if (f->origin != EMBI_ORIGIN_UA && embi_style_sel_state(sel)) {
        f->eng->uses_state = true;
    }
    if (sel->pseudo != EMBI_PE_NONE) {
        set->pe_mask |= 1u << sel->pseudo;
    }
    embi_sel_index_add(&set->index, sel, set->n);
    set->n++;
}

/* ------------------------------------------------------------------ rules */

static void flatten_rules(embi_flat *f, const embi_css_rule *rules, uint32_t n, uint32_t path,
                          uint16_t layer, const embi_sel_list *nest, uint32_t depth);

static uint64_t rcache_key(const void *rule, const void *nest)
{
    return embi_hash64((uint64_t)(uintptr_t)rule * 0x9e3779b97f4a7c15ull ^
                       embi_hash64((uint64_t)(uintptr_t)nest + 0x1234567u));
}

static const embi_rcache_rec *rcache_find(embi_flat *f, const void *rule, const embi_sel_list *nest)
{
    uint64_t found = 0;
    if (embi_hashmap_get(&f->eng->rcache, rcache_key(rule, nest), &found)) {
        const embi_rcache_rec *rec = (const embi_rcache_rec *)(uintptr_t)found;
        if (rec->rule == rule && rec->nest == nest) {
            return rec;
        }
    }
    return nullptr;
}

/* Remembers a rule's parsed form. On a hash collision the record is simply not kept (the rule is
 * parsed again on the next flatten), so a lookup never returns a wrong rule. */
static const embi_rcache_rec *rcache_put(embi_flat *f, const embi_css_rule *rule,
                                         const embi_sel_list *nest, const embi_sel_list *list,
                                         const embi_pdecl *pds, uint32_t nd)
{
    embi_rcache_rec *rec = embi_arena_alloc(&f->eng->arena, sizeof *rec, 8);
    rec->rule = rule;
    rec->nest = nest;
    rec->list = list;
    rec->pds = pds;
    rec->nd = nd;
    uint64_t key = rcache_key(rule, nest);
    uint64_t found = 0;
    if (!embi_hashmap_get(&f->eng->rcache, key, &found)) {
        embi_hashmap_put(&f->eng->rcache, key, (uint64_t)(uintptr_t)rec);
    }
    return rec;
}

/* Sheets are immutable and retained by the engine, so a rule's selectors and declarations are
 * compiled once per engine: flattening again (a new media environment, another sheet set) reuses
 * them and allocates nothing in the engine arena. */
static const embi_rcache_rec *rule_compiled(embi_flat *f, const embi_css_rule *r,
                                            const embi_sel_list *nest)
{
    const embi_rcache_rec *rec = rcache_find(f, r, nest);
    if (rec != nullptr) {
        return rec;
    }
    embi_css_slice use = r->prelude;
    if (nest != nullptr && !contains_amp(r->prelude)) {
        use = with_implicit_amp(&f->eng->arena, r->prelude);
    }
    embi_sel_env senv = {.ns = f->ns, .nns = f->nns, .nest_parent = nest};
    const embi_sel_list *list = embi_sel_parse(&f->eng->arena, use, &senv);
    uint32_t nd = 0;
    const embi_pdecl *pds = nullptr;
    if (list != nullptr) {
        pds = preparse(f, r->decls, r->ndecls, &nd);
    }
    return rcache_put(f, r, nest, list, pds, nd);
}

static void flatten_style(embi_flat *f, const embi_css_rule *r, uint32_t path, uint16_t layer,
                          const embi_sel_list *nest, uint32_t depth)
{
    const embi_rcache_rec *rec = rule_compiled(f, r, nest);
    const embi_sel_list *list = rec->list;
    if (list == nullptr) {
        return; /* an invalid selector list drops the rule and its nested rules */
    }
    if (rec->nd > 0) {
        uint64_t order = (f->seq << 32) | r->order;
        for (uint32_t i = 0; i < list->n; i++) {
            add_entry(f, &list->items[i], rec->pds, rec->nd, order, layer);
        }
    }
    if (r->nkids > 0) {
        flatten_rules(f, r->kids, r->nkids, path, layer, list, depth + 1);
    }
}

static void register_property(embi_flat *f, const embi_css_rule *r)
{
    embi_sset *set = f->set;
    embi_sreg reg;
    memset(&reg, 0, sizeof reg);
    reg.name = r->name;
    reg.inherits = true;
    for (uint32_t i = 0; i < r->ndecls; i++) {
        const embi_css_decl *d = &r->decls[i];
        embi_str nm = embi_atom_str(d->name);
        /* The first non-whitespace component value of the descriptor. */
        const embi_css_cv *first = nullptr;
        for (uint32_t j = 0; j < d->value.n && first == nullptr; j++) {
            if (d->value.p[j].tok.type != EMBI_CSS_WS) {
                first = &d->value.p[j];
            }
        }
        if (first == nullptr) {
            continue;
        }
        if (embi_str_ieq(nm, EMBI_STR("syntax")) && first->tok.type == EMBI_CSS_STRING) {
            reg.syntax = first->tok.value;
        } else if (embi_str_ieq(nm, EMBI_STR("inherits")) && first->tok.type == EMBI_CSS_IDENT) {
            reg.inherits = embi_str_ieq(first->tok.value, EMBI_STR("true"));
        } else if (embi_str_ieq(nm, EMBI_STR("initial-value"))) {
            reg.has_initial = true;
            reg.initial = d->value;
        }
    }
    if (reg.syntax.len == 0) {
        return; /* a registration needs a syntax descriptor */
    }
    for (uint32_t i = 0; i < set->nregs; i++) {
        if (set->regs[i].name == reg.name) {
            set->regs[i] = reg;
            return;
        }
    }
    if (set->nregs == set->capregs) {
        uint32_t cap = set->capregs == 0 ? 8u : set->capregs * 2u;
        set->regs = embi_realloc(set->regs, sizeof(embi_sreg) * set->capregs,
                                 sizeof(embi_sreg) * cap);
        set->capregs = cap;
    }
    set->regs[set->nregs++] = reg;
}

static void flatten_rules(embi_flat *f, const embi_css_rule *rules, uint32_t n, uint32_t path,
                          uint16_t layer, const embi_sel_list *nest, uint32_t depth)
{
    if (depth > 64) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_rule *r = &rules[i];
        switch (r->kind) {
        case EMBI_CSS_RULE_STYLE:
            flatten_style(f, r, path, layer, nest, depth);
            break;
        case EMBI_CSS_RULE_MEDIA:
            if (embi_style_media_match(r->prelude, f->env)) {
                if (nest != nullptr && r->ndecls > 0) {
                    /* Declarations directly inside a conditional nested rule apply to the parent
                     * selectors. */
                    const embi_rcache_rec *rec = rcache_find(f, r, nest);
                    if (rec == nullptr) {
                        uint32_t nd0 = 0;
                        const embi_pdecl *pds0 = preparse(f, r->decls, r->ndecls, &nd0);
                        rec = rcache_put(f, r, nest, nest, pds0, nd0);
                    }
                    uint64_t order = (f->seq << 32) | r->order;
                    if (rec->nd > 0) {
                        for (uint32_t k = 0; k < nest->n; k++) {
                            add_entry(f, &nest->items[k], rec->pds, rec->nd, order, layer);
                        }
                    }
                }
                flatten_rules(f, r->kids, r->nkids, path, layer, nest, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_SUPPORTS:
            if (r->cond) {
                flatten_rules(f, r->kids, r->nkids, path, layer, nest, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_LAYER:
            if (r->block) {
                uint32_t node = layer_path(f, path, r->name);
                flatten_rules(f, r->kids, r->nkids, node, (uint16_t)node, nest, depth + 1);
            } else {
                for (uint32_t k = 0; k < r->nlayers; k++) {
                    (void)layer_path(f, path, r->layers[k]);
                }
            }
            break;
        case EMBI_CSS_RULE_IMPORT:
            if (r->sheet != nullptr && r->cond &&
                (r->prelude.n == 0 || embi_style_media_match(r->prelude, f->env))) {
                uint32_t p2 = path;
                uint16_t l2 = layer;
                if (r->has_layer) {
                    p2 = layer_path(f, path, r->name);
                    l2 = (uint16_t)p2;
                }
                /* An imported sheet has its own namespace scope. */
                embi_sel_nsmap *saved_ns = f->ns;
                uint32_t saved_nns = f->nns, saved_cap = f->capns;
                f->ns = nullptr;
                f->nns = 0;
                f->capns = 0;
                f->seq++;
                flatten_rules(f, r->sheet->rules, r->sheet->nrules, p2, l2, nullptr, depth + 1);
                f->seq++;
                embi_free(f->ns, sizeof(embi_sel_nsmap) * f->capns);
                f->ns = saved_ns;
                f->nns = saved_nns;
                f->capns = saved_cap;
            }
            break;
        case EMBI_CSS_RULE_NAMESPACE:
            if (f->nns == f->capns) {
                uint32_t cap = f->capns == 0 ? 8u : f->capns * 2u;
                f->ns = embi_realloc(f->ns, sizeof(embi_sel_nsmap) * f->capns,
                                     sizeof(embi_sel_nsmap) * cap);
                f->capns = cap;
            }
            f->ns[f->nns].prefix = r->name;
            f->ns[f->nns].uri = embi_atom_intern(r->url.ptr, r->url.len);
            f->nns++;
            break;
        case EMBI_CSS_RULE_PROPERTY:
            register_property(f, r);
            break;
        case EMBI_CSS_RULE_COUNTER_STYLE: {
            embi_sset *set = f->set;
            if (set->ncstyles == set->capcstyles) {
                uint32_t cap = set->capcstyles == 0 ? 4u : set->capcstyles * 2u;
                set->cstyles = embi_realloc(set->cstyles, sizeof(*set->cstyles) * set->capcstyles,
                                            sizeof(*set->cstyles) * cap);
                set->capcstyles = cap;
            }
            set->cstyles[set->ncstyles++] = r;
            break;
        }
        default:
            /* @container and @scope are stored by the sheet model but not applied yet (W3, W6);
             * @font-face is registered by embi_style_register_fonts; @keyframes is W6.
             * Registration is per sheet, at set_author_sheets time, and is NOT re-evaluated on
             * set_env (a @font-face inside a changed @media is not added or removed). */
            break;
        }
    }
}

/* ------------------------------------------------------------------ sheets */

static void flatten_sheet(embi_flat *f, const embi_css_sheet *sheet, uint32_t path, uint16_t layer)
{
    embi_sel_nsmap *saved_ns = f->ns;
    uint32_t saved_nns = f->nns, saved_cap = f->capns;
    f->ns = nullptr;
    f->nns = 0;
    f->capns = 0;
    f->seq++;
    flatten_rules(f, sheet->rules, sheet->nrules, path, layer, nullptr, 0);
    embi_free(f->ns, sizeof(embi_sel_nsmap) * f->capns);
    f->ns = saved_ns;
    f->nns = saved_nns;
    f->capns = saved_cap;
}

void embi_style_sset_free(embi_sset *set)
{
    embi_sel_index_free(&set->index);
    embi_free(set->e, sizeof(embi_sentry) * set->cap);
    embi_free(set->regs, sizeof(embi_sreg) * set->capregs);
    embi_free(set->cstyles, sizeof(*set->cstyles) * set->capcstyles);
    memset(set, 0, sizeof *set);
}

void embi_style_flatten_sheets(embi_style_engine *e, embi_sset *set, embi_css_sheet *const *sheets,
                               uint32_t n, uint8_t origin, const embi_style_env *env)
{
    embi_style_sset_free(set);
    embi_flat f;
    memset(&f, 0, sizeof f);
    f.eng = e;
    f.set = set;
    f.origin = origin;
    f.env = env;
    f.nnodes = 1; /* node 0: the root */
    f.capnodes = 16;
    f.nodes = embi_calloc(f.capnodes, sizeof(embi_lnode));
    for (uint32_t i = 0; i < n; i++) {
        flatten_sheet(&f, sheets[i], 0, EMBI_LAYER_UNLAYERED);
    }
    /* Entries hold layer node indices. Every rank is known now, so each entry gets its rank. */
    assign_ranks(&f, 0, 0);
    for (uint32_t i = 0; i < set->n; i++) {
        if (set->e[i].layer != EMBI_LAYER_UNLAYERED) {
            set->e[i].layer = (uint16_t)f.nodes[set->e[i].layer].rank;
        }
    }
    embi_hashmap_free(&f.layers);
    embi_free(f.nodes, sizeof(embi_lnode) * f.capnodes);
}

/* ------------------------------------------------------------------ unsafe selectors */

static bool list_unsafe(const embi_sel_list *l);
static bool list_state(const embi_sel_list *l);

static bool simple_unsafe(const embi_sel_simple *ss)
{
    switch (ss->kind) {
    /* :lang() and :root depend on the element's ancestors (part of the sharing key) and on the
     * document root, so they are position-independent for sharing. :nth-*, :has, :dir() (which
     * reads text for dir=auto) and the sibling-dependent pseudo-classes are not. */
    case EMBI_SS_NTH:
    case EMBI_SS_HAS:
    case EMBI_SS_DIR:
        return true;
    case EMBI_SS_LANG:
        return false;
    case EMBI_SS_PC:
        switch (ss->sub) {
        case EMBI_PC_ROOT:
            return false;
        case EMBI_PC_EMPTY:
        case EMBI_PC_FIRST_CHILD:
        case EMBI_PC_LAST_CHILD:
        case EMBI_PC_ONLY_CHILD:
        case EMBI_PC_FIRST_OF_TYPE:
        case EMBI_PC_LAST_OF_TYPE:
        case EMBI_PC_ONLY_OF_TYPE:
        /* :enabled / :disabled depend on a legend sibling (fieldset > legend), which the
         * ancestor-chain sharing key does not capture. */
        case EMBI_PC_ENABLED:
        case EMBI_PC_DISABLED:
        case EMBI_PC_SCOPE:
        case EMBI_PC_TARGET:
        case EMBI_PC_MODAL:
        case EMBI_PC_POPOVER_OPEN:
        case EMBI_PC_OPEN:
        case EMBI_PC_DEFAULT:
        case EMBI_PC_VALID:
        case EMBI_PC_INVALID:
        case EMBI_PC_IN_RANGE:
        case EMBI_PC_OUT_OF_RANGE:
        case EMBI_PC_PLACEHOLDER_SHOWN:
            return true;
        default:
            /* :hover, :active, :focus, :focus-visible and :focus-within are answered per element
             * by the state hook; the element itself is kept out of style sharing in resolve.c
             * (state_relevant), so they are not unsafe here. */
            return false;
        }
    case EMBI_SS_IS:
    case EMBI_SS_WHERE:
    case EMBI_SS_NOT:
        return list_unsafe(ss->list);
    default:
        return false;
    }
}

/* Whether a selector reads a user-action state (:hover, :active, :focus, :focus-visible, :focus-within). Only those
 * need a restyle when the pointer or the focus moves, so only they make the document's style depend on the input. */
static bool simple_state(const embi_sel_simple *ss)
{
    switch (ss->kind) {
    case EMBI_SS_PC:
        return ss->sub == EMBI_PC_HOVER || ss->sub == EMBI_PC_ACTIVE || ss->sub == EMBI_PC_FOCUS ||
               ss->sub == EMBI_PC_FOCUS_VISIBLE || ss->sub == EMBI_PC_FOCUS_WITHIN;
    case EMBI_SS_IS:
    case EMBI_SS_WHERE:
    case EMBI_SS_NOT:
    case EMBI_SS_HAS:
        return list_state(ss->list);
    default:
        return false;
    }
}

static bool list_state(const embi_sel_list *l)
{
    if (l == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < l->n; i++) {
        if (embi_style_sel_state(&l->items[i])) {
            return true;
        }
    }
    return false;
}

bool embi_style_sel_state(const embi_sel_complex *c)
{
    for (uint32_t i = 0; i < c->nc; i++) {
        for (uint32_t j = 0; j < c->comp[i].n; j++) {
            if (simple_state(&c->comp[i].ss[j])) {
                return true;
            }
        }
    }
    return false;
}

static bool list_unsafe(const embi_sel_list *l)
{
    if (l == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < l->n; i++) {
        if (embi_style_sel_unsafe(&l->items[i])) {
            return true;
        }
    }
    return false;
}

bool embi_style_sel_unsafe(const embi_sel_complex *c)
{
    for (uint32_t i = 0; i < c->nc; i++) {
        if (i > 0 && (c->comb[i] == EMBI_SC_NEXT || c->comb[i] == EMBI_SC_SUBSEQ)) {
            return true;
        }
        for (uint32_t j = 0; j < c->comp[i].n; j++) {
            if (simple_unsafe(&c->comp[i].ss[j])) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ font faces */

static void fonts_rules(embi_style_engine *e, const embi_css_rule *rules, uint32_t n,
                        embi_str base, const embi_style_env *env, uint32_t depth)
{
    if (depth > 64) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_rule *r = &rules[i];
        switch (r->kind) {
        case EMBI_CSS_RULE_FONT_FACE:
            embi_style_font_face_rule(e, base, r);
            break;
        case EMBI_CSS_RULE_MEDIA:
            if (embi_style_media_match(r->prelude, env)) {
                fonts_rules(e, r->kids, r->nkids, base, env, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_SUPPORTS:
            if (r->cond) {
                fonts_rules(e, r->kids, r->nkids, base, env, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_LAYER:
            if (r->block) {
                fonts_rules(e, r->kids, r->nkids, base, env, depth + 1);
            }
            break;
        case EMBI_CSS_RULE_IMPORT:
            if (r->sheet != nullptr && r->cond &&
                (r->prelude.n == 0 || embi_style_media_match(r->prelude, env))) {
                fonts_rules(e, r->sheet->rules, r->sheet->nrules, base, env,
                            depth + 1);
            }
            break;
        default:
            break;
        }
    }
}

/* Registers the faces of each sheet once: a sheet is retained by the engine, so its address
 * identifies it for the engine's lifetime. */
void embi_style_register_fonts(embi_style_engine *e, embi_css_sheet *const *sheets, uint32_t n,
                               const embi_style_env *env)
{
    for (uint32_t i = 0; i < n; i++) {
        bool seen = false;
        for (uint32_t k = 0; k < e->nfont_sheets && !seen; k++) {
            seen = e->font_sheets[k] == sheets[i];
        }
        if (seen) {
            continue;
        }
        if (e->nfont_sheets == e->capfont_sheets) {
            uint32_t cap = e->capfont_sheets == 0 ? 8u : e->capfont_sheets * 2u;
            e->font_sheets = embi_realloc(e->font_sheets, sizeof(embi_css_sheet *) * e->capfont_sheets,
                                          sizeof(embi_css_sheet *) * cap);
            e->capfont_sheets = cap;
        }
        e->font_sheets[e->nfont_sheets++] = sheets[i];
        e->font_pending = false;
        fonts_rules(e, sheets[i]->rules, sheets[i]->nrules, e->font_base, env, 0);
        if (e->font_pending) {
            e->nfont_sheets--; /* a source has not arrived: the sheet registers again at the next call */
            e->font_pending = false;
        }
    }
}

/* Preparses a declaration list outside a sheet (style attributes, presentational hints). */
const embi_pdecl *embi_style_preparse(embi_style_engine *e, const embi_css_decl *decls, uint32_t n,
                                      uint32_t *count)
{
    embi_flat f;
    memset(&f, 0, sizeof f);
    f.eng = e;
    return preparse(&f, decls, n, count);
}
