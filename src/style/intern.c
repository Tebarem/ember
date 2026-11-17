/*
 * Interning of computed values, style groups and styles (W2d). Values are canonicalized by their
 * serialization, so equal computed values share one pointer, and groups and styles are interned by
 * the pointers they hold. Groups and styles are reference counted; the arena that holds the values
 * lives as long as the engine. Animated values live in value blocks (vblock.c), which the groups pointing into them pin.
 */
#include "style_int.h"

#include <string.h>

/* ------------------------------------------------------------------ layout */

embi_layout embi_g_layout;
bool embi_g_layout_built;

static uint32_t group_key_index(const char *name, bool inherited, const char **names,
                                bool *inh, uint32_t count)
{
    for (uint32_t g = 0; g < count; g++) {
        if (inh[g] == inherited && strcmp(names[g], name) == 0) {
            return g;
        }
    }
    return UINT32_MAX;
}

void embi_style_engine_init_layout(void)
{
    if (embi_g_layout_built) {
        return;
    }
    static const char *names[EMBI_STYLE_MAX_GROUPS];
    static bool inh[EMBI_STYLE_MAX_GROUPS];
    uint32_t count = 0;
    uint16_t counts[EMBI_STYLE_MAX_GROUPS] = {0};

    for (uint32_t p = 0; p < EMBI_PROP_COUNT; p++) {
        embi_g_layout.gid[p] = EMBI_STYLE_NO_GROUP;
        embi_g_layout.slot[p] = 0;
    }
    for (uint32_t p = 1; p < EMBI_PROP_COUNT; p++) {
        const embi_prop_info *pi = embi_prop_info_of((embi_prop_id)p);
        if (pi == nullptr || (pi->flags & EMBI_PF_SHORTHAND) != 0 || pi->group == nullptr ||
            embi_prop_is_logical((embi_prop_id)p)) {
            continue;
        }
        bool i = (pi->flags & EMBI_PF_INHERITED) != 0;
        uint32_t g = group_key_index(pi->group, i, names, inh, count);
        if (g == UINT32_MAX) {
            EMBI_ASSERT(count < EMBI_STYLE_MAX_GROUPS);
            names[count] = pi->group;
            inh[count] = i;
            g = count++;
        }
        embi_g_layout.gid[p] = (uint16_t)g;
        embi_g_layout.slot[p] = counts[g];
        counts[g]++;
    }
    embi_g_layout.ngroups = count;
    for (uint32_t g = 0; g < count; g++) {
        embi_g_layout.inherited[g] = inh[g];
        embi_g_layout.n[g] = counts[g];
        embi_g_layout.props[g] = embi_calloc(counts[g], sizeof(uint16_t));
    }
    for (uint32_t p = 1; p < EMBI_PROP_COUNT; p++) {
        if (embi_g_layout.gid[p] != EMBI_STYLE_NO_GROUP) {
            embi_g_layout.props[embi_g_layout.gid[p]][embi_g_layout.slot[p]] = (uint16_t)p;
        }
    }
    embi_g_layout_built = true;
}


/* ------------------------------------------------------------------ values */

static bool simple_value(const embi_value *v)
{
    switch (v->kind) {
    case EMBI_V_KEYWORD:
    case EMBI_V_NUMBER:
    case EMBI_V_INTEGER:
    case EMBI_V_PERCENT:
    case EMBI_V_LENGTH:
    case EMBI_V_ANGLE:
    case EMBI_V_TIME:
    case EMBI_V_RESOLUTION:
    case EMBI_V_COLOR:
    case EMBI_V_STRING:
    case EMBI_V_URL:
    case EMBI_V_IDENT:
    case EMBI_V_INITIAL:
    case EMBI_V_INHERIT:
    case EMBI_V_UNSET:
    case EMBI_V_REVERT:
    case EMBI_V_REVERT_LAYER:
        return true;
    case EMBI_V_LIST:
        for (uint32_t i = 0; i < v->list.n; i++) {
            const embi_value *it = &v->list.items[i];
            if (it->kind == EMBI_V_LIST || !simple_value(it)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

typedef struct embi_canon_rec {
    const char *text;
    uint32_t len;
    const embi_value *v;
} embi_canon_rec;

const embi_value *embi_style_canon(embi_style_engine *e, const embi_value *v)
{
    if (!simple_value(v)) {
        embi_value *nv = embi_arena_alloc(&e->arena, sizeof *nv, 8);
        *nv = *v;
        return nv;
    }
    embi_sb_clear(&e->sb);
    embi_value_serialize(&e->sb, v);
    embi_str text = {e->sb.data, e->sb.len};
    uint64_t h = embi_hash_bytes(text.ptr, text.len, 0x5157u);
    uint64_t found = 0;
    if (embi_hashmap_get(&e->vtab, h, &found)) {
        const embi_canon_rec *rec = (const embi_canon_rec *)(uintptr_t)found;
        if (rec->len == text.len && memcmp(rec->text, text.ptr, text.len) == 0) {
            return rec->v;
        }
        /* A hash collision: keep the value uncanonical. Practically unreachable. */
        embi_value *nv = embi_arena_alloc(&e->arena, sizeof *nv, 8);
        *nv = *v;
        return nv;
    }
    embi_value *nv = embi_arena_alloc(&e->arena, sizeof *nv, 8);
    *nv = *v;
    embi_canon_rec *rec = embi_arena_alloc(&e->arena, sizeof *rec, 8);
    rec->text = embi_style_copy_str(&e->arena, text).ptr;
    rec->len = text.len;
    rec->v = nv;
    embi_hashmap_put(&e->vtab, h, (uint64_t)(uintptr_t)rec);
    return nv;
}

embi_custom_map *embi_style_custom_alloc(embi_style_engine *e, uint32_t n)
{
    size_t sz = sizeof(embi_custom_map) + (size_t)n * sizeof(embi_cust_entry);
    embi_custom_map *m = embi_arena_alloc(&e->arena, sz, 8);
    m->n = n;
    e->ncustom++;
    e->custom_bytes += sz;
    return m;
}

/* CF1: interning of custom property maps. Two token lists are equal when every field of every token is (the strings by
 * content), and so are their nested lists. */
static uint64_t str_hash(embi_str s, uint64_t h)
{
    return s.len > 0 ? embi_hash_bytes(s.ptr, s.len, h) : embi_hash64(h ^ 0x5bd1e995u);
}

static uint64_t cvs_hash(const embi_css_cv *p, uint32_t n, uint64_t h)
{
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_token *t = &p[i].tok;
        uint64_t num = 0;
        memcpy(&num, &t->num, sizeof num);
        h = embi_hash64(h ^ ((uint64_t)t->type | (uint64_t)t->flag << 8 | (uint64_t)p[i].nkids << 16));
        h = embi_hash64(h ^ num);
        h = embi_hash64(h ^ ((uint64_t)t->start << 32 | t->end));
        h = embi_hash64(h ^ ((uint64_t)t->cp << 32 | t->cp_end) ^ ((uint64_t)p[i].end << 7));
        h = str_hash(t->value, h);
        h = str_hash(t->repr, h);
        if (p[i].nkids > 0) {
            h = cvs_hash(p[i].kids, p[i].nkids, h);
        }
    }
    return h;
}

static bool str_same(embi_str a, embi_str b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static bool cvs_same(const embi_css_cv *a, const embi_css_cv *b, uint32_t n)
{
    if (a == b) {
        return true;
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_css_token *x = &a[i].tok, *y = &b[i].tok;
        if (x->type != y->type || x->flag != y->flag || x->start != y->start || x->end != y->end ||
            memcmp(&x->num, &y->num, sizeof x->num) != 0 || x->cp != y->cp || x->cp_end != y->cp_end ||
            a[i].nkids != b[i].nkids || a[i].end != b[i].end || !str_same(x->value, y->value) ||
            !str_same(x->repr, y->repr)) {
            return false;
        }
        if (a[i].nkids > 0 && !cvs_same(a[i].kids, b[i].kids, a[i].nkids)) {
            return false;
        }
    }
    return true;
}

/* A copy of the token list p in the engine's arena: the token strings and the nested lists too. The values of a custom
 * property map point into the memory of the declaration they came from (a style sheet's arena, the engine arena for a
 * style attribute). An interned map lives as long as the engine and later maps are compared with it, so it owns its
 * tokens rather than rely on the engine keeping every sheet it was given (retain_sheet) for as long. */
static embi_str str_own(embi_style_engine *e, embi_str s)
{
    if (s.len == 0) {
        return (embi_str){"", 0};
    }
    char *d = embi_arena_alloc(&e->arena, s.len, 1);
    memcpy(d, s.ptr, s.len);
    e->custom_bytes += s.len;
    return (embi_str){d, s.len};
}

static const embi_css_cv *cvs_own(embi_style_engine *e, const embi_css_cv *p, uint32_t n)
{
    if (n == 0) {
        return nullptr;
    }
    embi_css_cv *d = embi_arena_alloc(&e->arena, sizeof(embi_css_cv) * n, 8);
    e->custom_bytes += sizeof(embi_css_cv) * n;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = p[i];
        d[i].tok.value = str_own(e, p[i].tok.value);
        d[i].tok.repr = str_own(e, p[i].tok.repr);
        d[i].kids = cvs_own(e, p[i].kids, p[i].nkids);
    }
    return d;
}

/* The canonical copy of the token list v: one copy per content in the engine's arena (a hash collision leaves a second
 * copy, which only costs the pointer compares below a miss). An entry a map passes on from its parent is canonical
 * already and found by its address, without hashing its tokens; so is the same value in the next map built from the same
 * declarations. Equal pointers then mean equal values, which lets maps (here, CF1) and restyles (embi_style_diff, the
 * custom-only walk) compare entries without serializing them. */
static embi_css_slice cust_canon(embi_style_engine *e, embi_css_slice v)
{
    if (v.n == 0) {
        return (embi_css_slice){nullptr, 0};
    }
    uint64_t found = 0;
    if (embi_hashmap_get(&e->cvown, (uint64_t)(uintptr_t)v.p, &found) && found == v.n) {
        return v;
    }
    uint64_t h = cvs_hash(v.p, v.n, embi_hash64(0x6376u ^ v.n));
    if (embi_hashmap_get(&e->cvtab, h, &found)) {
        const embi_css_slice *c = (const embi_css_slice *)(uintptr_t)found;
        if (c->n == v.n && cvs_same(c->p, v.p, v.n)) {
            return *c;
        }
    }
    embi_css_slice *c = embi_arena_alloc(&e->arena, sizeof *c, 8);
    c->n = v.n;
    c->p = cvs_own(e, v.p, v.n);
    if (found == 0) {
        embi_hashmap_put(&e->cvtab, h, (uint64_t)(uintptr_t)c);
    }
    embi_hashmap_put(&e->cvown, (uint64_t)(uintptr_t)c->p, v.n);
    return *c;
}

const embi_custom_map *embi_style_custom_intern(embi_style_engine *e, const embi_cust_entry *ents, uint32_t n)
{
    embi_cust_entry stack[64];
    embi_cust_entry *c = n <= 64 ? stack : embi_alloc(sizeof(embi_cust_entry) * n);
    uint64_t h = embi_hash64(0x63757374u ^ n);
    for (uint32_t i = 0; i < n; i++) {
        c[i].name = ents[i].name;
        c[i].val = cust_canon(e, ents[i].val);
        h = embi_hash64(h ^ c[i].name ^ ((uint64_t)c[i].val.n << 32) ^ embi_hash64((uint64_t)(uintptr_t)c[i].val.p));
    }
    uint64_t found = 0;
    const embi_custom_map *res = nullptr;
    if (embi_hashmap_get(&e->ctab, h, &found)) {
        const embi_custom_map *m = (const embi_custom_map *)(uintptr_t)found;
        bool same = m->n == n;
        for (uint32_t i = 0; same && i < n; i++) {
            same = m->e[i].name == c[i].name && m->e[i].val.n == c[i].val.n && m->e[i].val.p == c[i].val.p;
        }
        if (same) {
            res = m;
        }
    }
    if (res == nullptr) {
        embi_custom_map *m = embi_style_custom_alloc(e, n);
        memcpy(m->e, c, sizeof(embi_cust_entry) * n);
        if (found == 0) {
            embi_hashmap_put(&e->ctab, h, (uint64_t)(uintptr_t)m);
        }
        res = m;
    }
    if (c != stack) {
        embi_free(c, sizeof(embi_cust_entry) * n);
    }
    return res;
}

/* ------------------------------------------------------------------ groups */

uint64_t embi_style_slot_mix(uint32_t slot, const embi_value *v)
{
    return embi_hash64(((uint64_t)slot << 48) ^ (uint64_t)(uintptr_t)v ^ 0x9e3779b97f4a7c15ull);
}

static uint64_t group_key(uint32_t gid, uint64_t xh)
{
    return embi_hash64(xh ^ (0x6772ull + gid));
}

const embi_style_group *embi_style_group_intern_xh(embi_style_engine *e, uint32_t gid,
                                                   const embi_value *const *vals, uint64_t xh)
{
    const embi_layout *L = embi_layout_get();
    uint32_t n = L->n[gid];
    uint64_t h = group_key(gid, xh);
    uint64_t found = 0;
    bool taken = embi_hashmap_get(&e->gtab, h, &found); /* a different group with the same key: the new one stays out of the table */
    if (taken) {
        embi_style_group *g = (embi_style_group *)(uintptr_t)found;
        if (g->gid == gid && g->n == n && g->xh == xh &&
            memcmp(g->vals, vals, sizeof(void *) * n) == 0) {
            g->refs++;
            return g;
        }
    }
    size_t sz = sizeof(embi_style_group) + (size_t)n * sizeof(const embi_value *);
    embi_style_group *g = embi_alloc(sz);
    g->refs = 1;
    g->hash = (uint32_t)(xh ^ (xh >> 32));
    g->xh = xh;
    g->gid = gid;
    g->n = n;
    memcpy((void *)g->vals, vals, sizeof(void *) * n);
    if (!taken) {
        embi_hashmap_insert_new(&e->gtab, h, (uint64_t)(uintptr_t)g);
    }
    e->ngroups++;
    e->group_bytes += sz;
    if (e->nvpieces > 0) {
        embi_style_vpin_group(e, g); /* WPTASAN: the group keeps the animated values it points at alive */
    }
    return g;
}

const embi_style_group *embi_style_group_intern(embi_style_engine *e, uint32_t gid,
                                                const embi_value *const *vals)
{
    uint32_t n = embi_layout_get()->n[gid];
    uint64_t xh = 0;
    for (uint32_t i = 0; i < n; i++) {
        xh ^= embi_style_slot_mix(i, vals[i]);
    }
    return embi_style_group_intern_xh(e, gid, vals, xh);
}

static void group_free(embi_style_engine *e, embi_style_group *g)
{
    uint64_t h = group_key(g->gid, g->xh);
    (void)embi_hashmap_remove_if(&e->gtab, h, (uint64_t)(uintptr_t)g);
    if (embi_hashmap_len(&e->vpins) > 0) {
        embi_style_vunpin_group(e, g);
    }
    size_t sz = sizeof(embi_style_group) + (size_t)g->n * sizeof(const embi_value *);
    e->ngroups--;
    e->group_bytes -= sz;
    embi_free(g, sz);
}

/* ------------------------------------------------------------------ styles */

static uint64_t style_hash(const embi_style_group *const *groups, uint32_t ng,
                           const embi_custom_map *custom, uint32_t flags, uint64_t vrefs)
{
    uint64_t h = embi_hash64(0x7374u ^ (uint64_t)(uintptr_t)custom ^ ((uint64_t)flags << 56) ^ embi_hash64(vrefs));
    for (uint32_t i = 0; i < ng; i++) {
        h = embi_hash64(h ^ (uint64_t)(uintptr_t)groups[i]);
    }
    return h;
}

embi_style *embi_style_intern(embi_style_engine *e, const embi_style_group *const *groups,
                              const embi_custom_map *custom)
{
    return embi_style_intern_flags(e, groups, custom, 0, 0);
}

embi_style *embi_style_intern_flags(embi_style_engine *e, const embi_style_group *const *groups,
                                    const embi_custom_map *custom, uint32_t flags, uint64_t vrefs)
{
    uint32_t ng = embi_layout_get()->ngroups;
    uint64_t h = style_hash(groups, ng, custom, flags, vrefs);
    uint64_t found = 0;
    bool taken = embi_hashmap_get(&e->stab, h, &found);
    if (taken) {
        embi_style *s = (embi_style *)(uintptr_t)found;
        bool same = s->custom == custom && s->flags == flags && s->vrefs == vrefs;
        for (uint32_t i = 0; same && i < ng; i++) {
            same = s->groups[i] == groups[i];
        }
        if (same) {
            s->refs++;
            for (uint32_t i = 0; i < ng; i++) {
                embi_style_group *g = (embi_style_group *)groups[i];
                /* The caller's reference to each group is not needed: the style has them. */
                g->refs--;
                if (g->refs == 0) {
                    group_free(e, g);
                }
            }
            return s;
        }
    }
    size_t sz = sizeof(embi_style) + (size_t)ng * sizeof(void *);
    embi_style *s = embi_alloc(sz);
    s->refs = 1;
    s->hash = (uint32_t)(h ^ (h >> 32));
    s->custom = custom;
    s->flags = flags;
    s->vrefs = vrefs;
    for (uint32_t i = 0; i < ng; i++) {
        s->groups[i] = groups[i];
    }
    if (!taken) {
        embi_hashmap_insert_new(&e->stab, h, (uint64_t)(uintptr_t)s);
    }
    e->nstyles++;
    e->style_bytes += sz;
    return s;
}

void embi_style_ref(const embi_style *s)
{
    if (s != nullptr) {
        ((embi_style *)s)->refs++;
    }
}

void embi_style_unref(embi_style_engine *e, const embi_style *s)
{
    if (s == nullptr) {
        return;
    }
    embi_style *m = (embi_style *)s;
    EMBI_ASSERT(m->refs > 0);
    if (--m->refs != 0) {
        return;
    }
    uint32_t ng = embi_layout_get()->ngroups;
    uint64_t h = style_hash(s->groups, ng, s->custom, s->flags, s->vrefs);
    (void)embi_hashmap_remove_if(&e->stab, h, (uint64_t)(uintptr_t)s);
    for (uint32_t i = 0; i < ng; i++) {
        embi_style_group *g = (embi_style_group *)s->groups[i];
        if (--g->refs == 0) {
            group_free(e, g);
        }
    }
    size_t sz = sizeof(embi_style) + (size_t)ng * sizeof(void *);
    e->nstyles--;
    e->style_bytes -= sz;
    embi_free(m, sz);
}
