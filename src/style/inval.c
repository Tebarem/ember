/*
 * Style invalidation (W6b): from the selectors of the UA, user and author rules, which elements a change to a class, id,
 * attribute, child list or text can affect, and the marking of those elements for the partial restyle (inval_walk.c).
 *
 * The analysis is conservative: it may mark more than a browser would, never less. A name that occurs only in the subject
 * compound of selectors with no combinator reaches only the changed element itself. Anywhere else (an ancestor, a
 * sibling, inside :not(), :is(), :nth-child(of S), :has()) it reaches the elements the combinators relate to it.
 */
#include "inval.h"

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

/* ------------------------------------------------------------------ verification mode */

static int g_verify = -1;
static uint32_t g_verify_fail;

int embi_incr_verify_mode(void)
{
    if (g_verify < 0) {
        const char *v = SDL_getenv("EMBER_VERIFY_INCREMENTAL");
        g_verify = v != nullptr && v[0] == '1' ? 1 : 0;
    }
    return g_verify;
}

void embi_incr_set_verify(int mode)
{
    g_verify = mode;
}

void embi_incr_verify_fail(const char *what)
{
    g_verify_fail++;
    SDL_Log("EMBER_VERIFY_INCREMENTAL: mismatch: %s", what);
    if (g_verify == 1) {
        abort();
    }
}

uint32_t embi_incr_verify_failures(void)
{
    return g_verify_fail;
}

/* ------------------------------------------------------------------ analysis of selectors */

enum { IVK_ID = 1, IVK_CLASS = 2, IVK_ATTR = 3 };

/* The relation between a compound and the subject of its selector, tracked along the combinators that lead to the
 * subject: the element itself, its next sibling (a + b), its following siblings, its descendants, the descendants of the
 * next sibling, or the descendants of its following siblings. */
enum { ST_SELF = 0, ST_NEXT, ST_SIBS, ST_DESCS, ST_NEXTDEEP, ST_SIBDEEP };

static int st_step(int st, uint8_t comb)
{
    bool sib = comb == EMBI_SC_NEXT || comb == EMBI_SC_SUBSEQ;
    switch (st) {
    case ST_SELF:
        if (comb == EMBI_SC_NEXT) {
            return ST_NEXT;
        }
        return sib ? ST_SIBS : ST_DESCS;
    case ST_NEXT:
        /* a + b + c reaches the sibling after the next one: all following siblings, conservatively */
        return sib ? ST_SIBS : ST_NEXTDEEP;
    case ST_SIBS:
        return sib ? ST_SIBS : ST_SIBDEEP;
    default:
        return st;
    }
}

static uint8_t st_mask(int st)
{
    switch (st) {
    case ST_SELF:
        return EMBI_IVM_SELF;
    case ST_NEXT:
        return EMBI_IVM_NEXT;
    case ST_NEXTDEEP:
        return EMBI_IVM_NEXTDEEP;
    case ST_SIBS:
        return EMBI_IVM_SIB;
    case ST_DESCS:
        return EMBI_IVM_DESC;
    case ST_SIBDEEP:
        return EMBI_IVM_SIBDEEP;
    default:
        return 0;
    }
}

static uint64_t ivkey(int kind, embi_atom a)
{
    return ((uint64_t)kind << 32) | (uint64_t)a;
}

/* The key of a name read in a compound that has a type selector: only an element of that type is reached. A collision can
 * only add a spurious entry, which makes a lookup find more, never less. */
static uint64_t ivkey_typed(int kind, embi_atom a, embi_atom tag)
{
    uint64_t h = ((uint64_t)a * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)tag * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)kind << 7);
    return (h >> 4) | (4ull << 60);
}

/* CF1: the feature an element must have to be the subject of a selector whose subject compound is cp (embi_ivfs): its
 * id, else a class, else its type; 0 when the compound names none of them. */
static uint64_t subj_feature(const embi_sel_compound *cp)
{
    uint64_t cls = 0, tag = 0;
    for (uint32_t i = 0; i < cp->n; i++) {
        const embi_sel_simple *s = &cp->ss[i];
        if (s->kind == EMBI_SS_ID && s->name != 0) {
            return 3ull << 32 | embi_sel_lower_atom(s->name);
        }
        if (s->kind == EMBI_SS_CLASS && s->name != 0 && cls == 0) {
            cls = 2ull << 32 | embi_sel_lower_atom(s->name);
        }
        if (s->kind == EMBI_SS_TYPE && tag == 0 && (s->lname != 0 || s->name != 0)) {
            tag = 1ull << 32 | embi_sel_lower_atom(s->lname != 0 ? s->lname : s->name);
        }
    }
    return cls != 0 ? cls : tag;
}

/* Adds the feature f (0: every element) to the feature set *id of iv (made when *id is 0). */
static void fs_add(embi_ivset *iv, uint32_t *id, uint64_t f)
{
    if (*id == 0) {
        embi_ivfs z;
        memset(&z, 0, sizeof z);
        embi_vec_push(&iv->fs, z);
        *id = iv->fs.len;
    }
    embi_ivfs *s = &iv->fs.data[*id - 1];
    if (s->all) {
        return;
    }
    if (f == 0) {
        s->all = true;
        return;
    }
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->f[i] == f) {
            return;
        }
    }
    if (s->n == EMBI_IVFS_MAX) {
        s->all = true;
        return;
    }
    s->f[s->n++] = f;
}

static void iv_add(embi_ivset *iv, int kind, embi_atom a, uint8_t mask, embi_atom tag, uint64_t subj)
{
    if (mask == 0 || a == 0) {
        return;
    }
    embi_atom l = embi_sel_lower_atom(a);
    uint64_t k = tag != 0 ? ivkey_typed(kind, l, tag) : ivkey(kind, l);
    uint64_t old = 0;
    embi_hashmap_get(&iv->names, k, &old);
    embi_hashmap_put(&iv->names, k, old | mask);
    if ((mask & EMBI_IVM_DESC) != 0) {
        uint64_t id = 0;
        embi_hashmap_get(&iv->dfeat, k, &id);
        uint32_t id32 = (uint32_t)id;
        fs_add(iv, &id32, subj);
        embi_hashmap_put(&iv->dfeat, k, id32);
    }
}

#define IV_CHAIN_MAX 32

typedef struct iv_chain {
    uint8_t c[IV_CHAIN_MAX];
    uint32_t n;
} iv_chain;

static void an_list(embi_ivset *iv, const embi_sel_list *l, const iv_chain *chain, bool has, uint64_t subj);

/* The user-action state that the pseudo-class pc is (EMBI_USTATE_*), or -1. */
static int ustate_of(uint8_t pc)
{
    switch (pc) {
    case EMBI_PC_HOVER:
        return EMBI_USTATE_HOVER;
    case EMBI_PC_ACTIVE:
        return EMBI_USTATE_ACTIVE;
    case EMBI_PC_FOCUS:
        return EMBI_USTATE_FOCUS;
    case EMBI_PC_FOCUS_VISIBLE:
        return EMBI_USTATE_FOCUS_VISIBLE;
    case EMBI_PC_FOCUS_WITHIN:
        return EMBI_USTATE_FOCUS_WITHIN;
    default:
        return -1;
    }
}

/* nth-child(an+b of S), nth-last-child(an+b of S): S is read on the siblings, so a change of what S reads on one sibling
 * (a class, an id, an attribute, a state) changes the index of the others. Every simple selector of S counts, at any
 * nesting depth (:is, :not, :where, :has and nested nth lists inside S). */
static void nth_of_siblings(embi_ivset *iv, const embi_sel_list *l, int depth)
{
    if (l == nullptr || depth > 16) {
        iv->anyattr |= EMBI_IVM_ALLSIB;
        for (int k = 0; k < EMBI_USTATE_COUNT; k++) {
            iv->ustate[k] |= EMBI_IVM_ALLSIB;
        }
        return;
    }
    for (uint32_t i = 0; i < l->n; i++) {
        const embi_sel_complex *c = &l->items[i];
        for (uint32_t q = 0; q < c->nc; q++) {
            for (uint32_t r = 0; r < c->comp[q].n; r++) {
                const embi_sel_simple *t = &c->comp[q].ss[r];
                if (t->kind == EMBI_SS_CLASS) {
                    iv_add(iv, IVK_CLASS, t->name, EMBI_IVM_ALLSIB, 0, 0);
                } else if (t->kind == EMBI_SS_ID) {
                    iv_add(iv, IVK_ID, t->name, EMBI_IVM_ALLSIB, 0, 0);
                } else if (t->kind == EMBI_SS_ATTR) {
                    iv_add(iv, IVK_ATTR, t->lname != 0 ? t->lname : t->name, EMBI_IVM_ALLSIB, 0, 0);
                } else if (t->kind == EMBI_SS_PC) {
                    int k = ustate_of(t->sub);
                    if (k >= 0) {
                        iv->ustate[k] |= EMBI_IVM_ALLSIB; /* CF1: a user-action state flip on a sibling */
                    } else {
                        iv->anyattr |= EMBI_IVM_ALLSIB;
                    }
                } else if (t->list != nullptr && (t->kind == EMBI_SS_IS || t->kind == EMBI_SS_WHERE ||
                                                  t->kind == EMBI_SS_NOT || t->kind == EMBI_SS_HAS ||
                                                  t->kind == EMBI_SS_NTH)) {
                    nth_of_siblings(iv, t->list, depth + 1);
                }
            }
        }
    }
}

/* The simple selectors of one compound; chain is the combinators from this compound to the outermost subject, subj the
 * feature of that subject (subj_feature). */
static void an_compound(embi_ivset *iv, const embi_sel_compound *cp, const iv_chain *chain, bool has, uint64_t subj)
{
    int st = ST_SELF;
    for (uint32_t i = 0; i < chain->n; i++) {
        st = st_step(st, chain->c[i]);
    }
    uint8_t m = has ? (uint8_t)EMBI_IVM_HAS : st_mask(st);
    embi_atom tag = 0; /* the type selector of the compound: only elements of that type can match it */
    for (uint32_t i = 0; i < cp->n; i++) {
        if (cp->ss[i].kind == EMBI_SS_TYPE) {
            tag = embi_sel_lower_atom(cp->ss[i].lname != 0 ? cp->ss[i].lname : cp->ss[i].name);
        }
    }
    for (uint32_t i = 0; i < cp->n; i++) {
        const embi_sel_simple *s = &cp->ss[i];
        switch (s->kind) {
        case EMBI_SS_ID:
            iv_add(iv, IVK_ID, s->name, m, tag, subj);
            break;
        case EMBI_SS_CLASS:
            iv_add(iv, IVK_CLASS, s->name, m, tag, subj);
            break;
        case EMBI_SS_ATTR:
            iv_add(iv, IVK_ATTR, s->lname != 0 ? s->lname : s->name, m, tag, subj);
            break;
        case EMBI_SS_PC:
            switch (s->sub) {
            case EMBI_PC_EMPTY:
                iv->flags |= EMBI_IVF_EMPTY | EMBI_IVF_STRUCT;
                if (st != ST_SELF || has) {
                    iv->flags |= EMBI_IVF_STRUCT_ANC;
                }
                break;
            case EMBI_PC_FIRST_CHILD:
            case EMBI_PC_LAST_CHILD:
            case EMBI_PC_ONLY_CHILD:
            case EMBI_PC_FIRST_OF_TYPE:
            case EMBI_PC_LAST_OF_TYPE:
            case EMBI_PC_ONLY_OF_TYPE:
                iv->flags |= EMBI_IVF_STRUCT;
                if (st != ST_SELF || has) {
                    iv->flags |= EMBI_IVF_STRUCT_ANC;
                }
                break;
            case EMBI_PC_HOVER:
            case EMBI_PC_ACTIVE:
            case EMBI_PC_FOCUS:
            case EMBI_PC_FOCUS_VISIBLE:
            case EMBI_PC_FOCUS_WITHIN: {
                /* CF1: a flip of the state marks what it reaches */
                int k = s->sub == EMBI_PC_HOVER    ? EMBI_USTATE_HOVER
                        : s->sub == EMBI_PC_ACTIVE ? EMBI_USTATE_ACTIVE
                        : s->sub == EMBI_PC_FOCUS  ? EMBI_USTATE_FOCUS
                        : s->sub == EMBI_PC_FOCUS_VISIBLE ? EMBI_USTATE_FOCUS_VISIBLE
                                                          : EMBI_USTATE_FOCUS_WITHIN;
                iv->ustate[k] |= m;
                if ((m & EMBI_IVM_DESC) != 0) {
                    fs_add(iv, &iv->ufs[k], subj);
                }
                break;
            }
            case EMBI_PC_ROOT:
            case EMBI_PC_SCOPE:
            case EMBI_PC_VISITED:
            case EMBI_PC_TARGET:
                break; /* constant within a document, or a change marks the document dirty (embi_style_mark_dirty) */
            default:
                /* an attribute-derived state (:checked, :disabled, :link ...): any attribute change may flip it */
                iv->anyattr |= m;
                break;
            }
            break;
        case EMBI_SS_NTH: {
            iv->flags |= EMBI_IVF_STRUCT;
            if (st != ST_SELF || has) {
                iv->flags |= EMBI_IVF_STRUCT_ANC;
            }
            if (s->list != nullptr) {
                /* nth-child(an+b of S): S is read on the siblings, so a sibling's class changes the index of the others */
                an_list(iv, s->list, chain, has, subj);
                nth_of_siblings(iv, s->list, 0);
            }
            break;
        }
        case EMBI_SS_IS:
        case EMBI_SS_WHERE:
        case EMBI_SS_NOT:
            if (s->list != nullptr) {
                an_list(iv, s->list, chain, has, subj);
            }
            break;
        case EMBI_SS_HAS:
            iv->flags |= EMBI_IVF_HAS;
            if (s->list != nullptr) {
                an_list(iv, s->list, chain, true, subj);
            }
            break;
        default:
            break;
        }
    }
}

/* One complex selector whose subject is the element the enclosing compound is about; outer is the chain of combinators
 * from that compound to the outermost subject. */
static void an_complex(embi_ivset *iv, const embi_sel_complex *c, const iv_chain *outer, bool has, uint64_t subj)
{
    for (uint32_t i = 0; i < c->nc; i++) {
        iv_chain ch;
        ch.n = 0;
        for (uint32_t j = i + 1; j < c->nc && ch.n < IV_CHAIN_MAX; j++) {
            ch.c[ch.n++] = c->comb[j];
            if (c->comb[j] == EMBI_SC_NEXT || c->comb[j] == EMBI_SC_SUBSEQ) {
                iv->flags |= EMBI_IVF_SIBCOMB;
            }
        }
        for (uint32_t j = 0; j < outer->n && ch.n < IV_CHAIN_MAX; j++) {
            ch.c[ch.n++] = outer->c[j];
        }
        if (has && (c->comb[0] == EMBI_SC_NEXT || c->comb[0] == EMBI_SC_SUBSEQ)) {
            iv->flags |= EMBI_IVF_HASSIB | EMBI_IVF_SIBCOMB;
        }
        an_compound(iv, &c->comp[i], &ch, has, subj);
    }
}

static void an_list(embi_ivset *iv, const embi_sel_list *l, const iv_chain *chain, bool has, uint64_t subj)
{
    for (uint32_t i = 0; i < l->n; i++) {
        an_complex(iv, &l->items[i], chain, has, subj);
    }
}

static void iv_build(embi_ivset *iv, const embi_sset *set)
{
    iv_chain none;
    none.n = 0;
    for (uint32_t i = 0; i < set->n; i++) {
        const embi_sel_complex *c = set->e[i].sel;
        if (c != nullptr && c->nc > 0) {
            an_complex(iv, c, &none, false, subj_feature(&c->comp[c->nc - 1]));
        }
    }
}

static void iv_reset(embi_ivset *iv)
{
    embi_hashmap_clear(&iv->names);
    embi_hashmap_clear(&iv->dfeat);
    embi_vec_clear(&iv->fs);
    memset(iv->ufs, 0, sizeof iv->ufs);
    iv->flags = 0;
    iv->anyattr = 0;
    memset(iv->ustate, 0, sizeof iv->ustate);
    iv->valid = false;
}

/* The info of the UA and user sets, then the document's. */
static void ensure(embi_docstate *ds)
{
    embi_style_engine *e = ds->eng;
    if (!e->iv.valid) {
        iv_reset(&e->iv);
        embi_style_ua_ensure(e);
        iv_build(&e->iv, &e->ua);
        iv_build(&e->iv, &e->user);
        e->iv.valid = true;
    }
    if (!ds->iv.valid) {
        iv_reset(&ds->iv);
        iv_build(&ds->iv, &ds->author);
        ds->iv.valid = true;
    }
}

/* CF1: the descendants a change has to restyle: those with one of the features f (embi_ivfs), or all of them. */
typedef struct ivfilt {
    bool all;
    uint32_t n;
    uint64_t f[4 * EMBI_IVFS_MAX];
} ivfilt;

/* Adds the feature set id of iv (0: none recorded, which is every descendant) to flt. */
static void filt_add(ivfilt *flt, const embi_ivset *iv, uint32_t id)
{
    if (flt->all) {
        return;
    }
    const embi_ivfs *s = id != 0 && id <= iv->fs.len ? &iv->fs.data[id - 1] : nullptr;
    if (s == nullptr || s->all) {
        flt->all = true;
        return;
    }
    for (uint32_t i = 0; i < s->n; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < flt->n && !dup; j++) {
            dup = flt->f[j] == s->f[i];
        }
        if (!dup) {
            if (flt->n == 4 * EMBI_IVFS_MAX) {
                flt->all = true;
                return;
            }
            flt->f[flt->n++] = s->f[i];
        }
    }
}

static void filt_key(ivfilt *flt, const embi_ivset *iv, uint64_t key, uint64_t mask)
{
    if ((mask & EMBI_IVM_DESC) != 0) {
        uint64_t id = 0;
        embi_hashmap_get(&iv->dfeat, key, &id);
        filt_add(flt, iv, (uint32_t)id);
    }
}

/* What the name a (a class, id or attribute of kind) reaches when it changes on el: the rules that read it in any compound,
 * and those that read it in a compound with the type selector of el. CF1: flt receives the descendants they reach. */
static uint8_t iv_lookup_f(const embi_docstate *ds, int kind, embi_atom a, const embi_element *el, ivfilt *flt)
{
    flt->all = false;
    flt->n = 0;
    if (a == 0) {
        return 0;
    }
    embi_atom l = embi_sel_lower_atom(a);
    uint64_t k = ivkey(kind, l);
    uint64_t kt = ivkey_typed(kind, l, embi_sel_lower_atom(el->base.name));
    uint64_t v = 0, w = 0, x = 0, y = 0;
    embi_hashmap_get(&ds->eng->iv.names, k, &v);
    embi_hashmap_get(&ds->iv.names, k, &w);
    embi_hashmap_get(&ds->eng->iv.names, kt, &x);
    embi_hashmap_get(&ds->iv.names, kt, &y);
    filt_key(flt, &ds->eng->iv, k, v);
    filt_key(flt, &ds->iv, k, w);
    filt_key(flt, &ds->eng->iv, kt, x);
    filt_key(flt, &ds->iv, kt, y);
    return (uint8_t)(v | w | x | y);
}

/* ------------------------------------------------------------------ the dirty map */

uint16_t embi_inval_get(const embi_docstate *ds, const embi_element *el)
{
    uint64_t v = 0;
    if (embi_hashmap_len(&ds->dmap) > 0 && embi_hashmap_get(&ds->dmap, (uint64_t)(uintptr_t)el, &v)) {
        return (uint16_t)v;
    }
    return 0;
}

static void dor(embi_docstate *ds, const embi_element *el, uint16_t bits)
{
    bool added = false;
    uint64_t *v = embi_hashmap_ref(&ds->dmap, (uint64_t)(uintptr_t)el, bits, &added);
    *v |= bits;
}

/* Every element ancestor of node carries SUB. The marks of an ancestor imply those of its own ancestors. */
static void mark_ancestors(embi_docstate *ds, const embi_node *node)
{
    for (embi_node *p = node->parent; p != nullptr && p->type == EMBI_NODE_ELEMENT; p = p->parent) {
        embi_element *pe = (embi_element *)p;
        bool added = false;
        uint64_t *v = embi_hashmap_ref(&ds->dmap, (uint64_t)(uintptr_t)pe, EMBI_IVD_SUB, &added);
        if (!added) {
            if ((*v & EMBI_IVD_SUB) != 0) {
                break;
            }
            *v |= EMBI_IVD_SUB;
        }
    }
}

static void mark(embi_docstate *ds, embi_element *el, uint16_t bits)
{
    dor(ds, el, bits);
    mark_ancestors(ds, &el->base);
}

static embi_element *parent_el(const embi_node *n)
{
    return n->parent != nullptr && n->parent->type == EMBI_NODE_ELEMENT ? (embi_element *)n->parent : nullptr;
}

/* :has() subjects: the element's ancestors, and the siblings before each of them (a :has(+ x) subject). */
static void mark_has(embi_docstate *ds, embi_element *el, bool sib)
{
    for (embi_element *a = el; a != nullptr; a = parent_el(&a->base)) {
        mark(ds, a, EMBI_IVD_SELF);
        if (sib) {
            embi_element *p = parent_el(&a->base);
            if (p != nullptr) {
                mark(ds, p, EMBI_IVD_KIDS);
            }
        }
    }
}

/* CF1: whether el has one of the features of flt (a superset of the elements the rules select; the names compare
 * lowercase). */
static bool feat_match(embi_element *el, const ivfilt *flt)
{
    const embi_atom *cls = nullptr;
    uint32_t ncls = 0;
    bool got = false;
    for (uint32_t i = 0; i < flt->n; i++) {
        uint32_t kind = (uint32_t)(flt->f[i] >> 32);
        embi_atom a = (embi_atom)(flt->f[i] & 0xFFFFFFFFu);
        if (kind == 1) {
            if (embi_sel_lower_atom(el->base.name) == a) {
                return true;
            }
        } else if (kind == 2) {
            if (!got) {
                cls = embi_el_classes(el, &ncls);
                got = true;
            }
            for (uint32_t j = 0; j < ncls; j++) {
                if (embi_sel_lower_atom(cls[j]) == a) {
                    return true;
                }
            }
        } else {
            embi_atom id = embi_el_id(el);
            if (id != 0 && embi_sel_lower_atom(id) == a) {
                return true;
            }
        }
    }
    return false;
}

/* CF1: the descendants of el that have a feature of flt are restyled (instead of every descendant). */
static void mark_desc_filtered(embi_docstate *ds, embi_element *el, const ivfilt *flt)
{
    embi_node *top = &el->base;
    embi_node *k = top->first_child;
    while (k != nullptr) {
        if (k->type == EMBI_NODE_ELEMENT) {
            if (feat_match((embi_element *)k, flt)) {
                mark(ds, (embi_element *)k, EMBI_IVD_SELF);
            }
            if (k->first_child != nullptr) {
                k = k->first_child;
                continue;
            }
        }
        while (k != top && k->next == nullptr) {
            k = k->parent;
        }
        if (k == top) {
            break;
        }
        k = k->next;
    }
}

/* flt (nullptr: every descendant) says which descendants an EMBI_IVM_DESC bit of m reaches. */
static void apply_mask_f(embi_docstate *ds, embi_element *el, uint8_t m, const ivfilt *flt)
{
    if ((m & EMBI_IVM_DESC) != 0) {
        if (flt == nullptr || flt->all) {
            mark(ds, el, EMBI_IVD_DESC);
        } else {
            mark_desc_filtered(ds, el, flt);
        }
    }
    if ((m & EMBI_IVM_SIB) != 0) {
        mark(ds, el, EMBI_IVD_SIB);
    }
    if ((m & EMBI_IVM_SIBDEEP) != 0) {
        mark(ds, el, EMBI_IVD_SIBDEEP);
    }
    if ((m & EMBI_IVM_NEXT) != 0) {
        mark(ds, el, EMBI_IVD_NEXT);
    }
    if ((m & EMBI_IVM_NEXTDEEP) != 0) {
        mark(ds, el, EMBI_IVD_NEXTDEEP);
    }
    if ((m & EMBI_IVM_SELF) != 0) {
        mark(ds, el, EMBI_IVD_SELF);
    }
    if ((m & EMBI_IVM_HAS) != 0) {
        mark_has(ds, el, ((ds->iv.flags | ds->eng->iv.flags) & EMBI_IVF_HASSIB) != 0);
    }
    if ((m & EMBI_IVM_ALLSIB) != 0) {
        embi_element *p = parent_el(&el->base);
        if (p != nullptr) {
            /* ALLSIB comes from :nth-child(an+b of S); when such a compound is outside the subject (li:nth-child(1 of
             * .x) span), the siblings' descendants read it too (STRUCT_ANC is set for it) */
            const bool deep = ((ds->iv.flags | ds->eng->iv.flags) & EMBI_IVF_STRUCT_ANC) != 0;
            mark(ds, p, deep ? EMBI_IVD_KIDSDEEP : EMBI_IVD_KIDS);
        }
    }
}

static void apply_mask(embi_docstate *ds, embi_element *el, uint8_t m)
{
    apply_mask_f(ds, el, m, nullptr);
}

/* apply_mask of what the name a of kind reaches from el (iv_lookup_f). */
static void apply_name(embi_docstate *ds, embi_element *el, int kind, embi_atom a)
{
    ivfilt flt;
    uint8_t m = iv_lookup_f(ds, kind, a, el, &flt);
    apply_mask_f(ds, el, m, &flt);
}

static void orig_or(embi_docstate *ds, embi_element *el, uint8_t bits)
{
    if (!ds->track) {
        return;
    }
    uint64_t v = 0;
    embi_hashmap_get(&ds->omap, (uint64_t)(uintptr_t)el, &v);
    embi_hashmap_put(&ds->omap, (uint64_t)(uintptr_t)el, v | bits);
}

static bool in_doc(const embi_docstate *ds, const embi_node *n)
{
    return (n->flags & EMBI_NF_CONNECTED) != 0 && n->doc == ds->doc;
}

/* ------------------------------------------------------------------ DOM change hooks */

static bool range_relevant(const embi_node *first, const embi_node *last)
{
    if (first == nullptr) {
        return false;
    }
    const embi_node *n = first;
    for (;;) {
        if (n->type == EMBI_NODE_ELEMENT || n->type == EMBI_NODE_TEXT) {
            return true;
        }
        if (n == last) {
            return false;
        }
        n = n->next;
        if (n == nullptr) {
            return true; /* the chain does not reach last: do not guess */
        }
    }
}

void embi_inval_children(embi_docstate *ds, embi_node *parent, embi_node *fa, embi_node *la, embi_node *fr,
                         embi_node *lr)
{
    ds->dirty = true;
    if (!range_relevant(fa, la) && !range_relevant(fr, lr)) {
        return; /* comments and processing instructions: no style, no box */
    }
    if (parent->type != EMBI_NODE_ELEMENT) {
        if (parent->type == EMBI_NODE_DOCUMENT) {
            ds->full = true; /* the top level changed */
        }
        return;
    }
    embi_element *p = (embi_element *)parent;
    if (!in_doc(ds, parent)) {
        return;
    }
    orig_or(ds, p, EMBI_ORIG_STRUCT);
    if (ds->full || !ds->resolved) {
        return;
    }
    ensure(ds);
    uint32_t fl = ds->iv.flags | ds->eng->iv.flags;
    if ((fl & (EMBI_IVF_STRUCT | EMBI_IVF_HAS | EMBI_IVF_EMPTY)) != 0) {
        mark(ds, p, EMBI_IVD_SELF);
    } else {
        mark_ancestors(ds, &p->base);
        dor(ds, p, EMBI_IVD_SUB);
    }
    if ((fl & EMBI_IVF_STRUCT_ANC) != 0 || (fl & EMBI_IVF_SIBCOMB) != 0) {
        mark(ds, p, EMBI_IVD_KIDSDEEP);
    } else if ((fl & EMBI_IVF_STRUCT) != 0) {
        mark(ds, p, EMBI_IVD_KIDS);
    }
    if ((fl & EMBI_IVF_HAS) != 0) {
        mark_has(ds, p, (fl & EMBI_IVF_HASSIB) != 0);
    }
}

void embi_inval_chardata(embi_docstate *ds, embi_node *node)
{
    ds->dirty = true;
    if (node->type != EMBI_NODE_TEXT || node->parent == nullptr || node->parent->type != EMBI_NODE_ELEMENT ||
        !in_doc(ds, node)) {
        return; /* a comment, or a node outside the tree */
    }
    embi_element *p = (embi_element *)node->parent;
    orig_or(ds, p, EMBI_ORIG_TEXT);
    if (ds->full || !ds->resolved) {
        return;
    }
    ensure(ds);
    uint32_t fl = ds->iv.flags | ds->eng->iv.flags;
    if ((fl & (EMBI_IVF_HAS | EMBI_IVF_EMPTY)) != 0) {
        mark(ds, p, EMBI_IVD_SELF);
        if ((fl & EMBI_IVF_HAS) != 0) {
            mark_has(ds, p, (fl & EMBI_IVF_HASSIB) != 0);
        }
    } else {
        mark_ancestors(ds, &p->base);
        dor(ds, p, EMBI_IVD_SUB);
    }
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* Marks what the class tokens that differ between the old and the new class attribute can reach. */
static void class_change(embi_docstate *ds, embi_element *el, embi_dstr old, bool had)
{
    uint32_t nn = 0;
    const embi_atom *now = embi_el_classes(el, &nn);
    embi_atom oldt[64];
    uint32_t no = 0;
    bool overflow = false;
    if (had) {
        size_t cap = 0;
        char *buf = nullptr;
        embi_str s = embi_style_attr_utf8(&old, &cap, &buf);
        size_t i = 0;
        while (i < s.len) {
            while (i < s.len && is_ws(s.ptr[i])) {
                i++;
            }
            size_t b = i;
            while (i < s.len && !is_ws(s.ptr[i])) {
                i++;
            }
            if (i > b) {
                if (no == 64) {
                    overflow = true;
                    break;
                }
                oldt[no++] = embi_atom_intern(s.ptr + b, (uint32_t)(i - b));
            }
        }
        embi_free(buf, cap);
    }
    if (overflow) {
        apply_mask(ds, el, EMBI_IVM_DESC | EMBI_IVM_SIBDEEP | EMBI_IVM_SELF);
        return;
    }
    for (uint32_t i = 0; i < no; i++) {
        bool found = false;
        for (uint32_t j = 0; j < nn && !found; j++) {
            found = now[j] == oldt[i];
        }
        if (!found) {
            apply_name(ds, el, IVK_CLASS, oldt[i]);
        }
    }
    for (uint32_t j = 0; j < nn; j++) {
        bool found = false;
        for (uint32_t i = 0; i < no && !found; i++) {
            found = now[j] == oldt[i];
        }
        if (!found) {
            apply_name(ds, el, IVK_CLASS, now[j]);
        }
    }
}

static bool name_is(embi_atom a, const char *lit)
{
    embi_str s = embi_atom_str(a);
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

static bool name_has_prefix(embi_atom a, const char *pre)
{
    embi_str s = embi_atom_str(a);
    size_t n = strlen(pre);
    if (s.len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != pre[i]) {
            return false;
        }
    }
    return true;
}

/* Attributes the box build and the paint never read, whatever the element: a change needs the restyle of the element
 * (its rules may select on them) but no rebuild of its boxes. */
static bool attr_layout_neutral(const embi_docstate *ds, embi_atom a)
{
    /* attr() in a user or author declaration, or in a style attribute, reads any attribute */
    if (ds->author.has_attrfn || ds->eng->user.has_attrfn || ds->eng->attrfn_inline) {
        return false;
    }
    /* CF1: name too (the form-control and radio-group name: no box, paint or state pseudo-class reads it) */
    return name_is(a, "class") || name_is(a, "style") || name_is(a, "id") || name_is(a, "title") || name_is(a, "role") ||
           name_is(a, "name") ||
           name_is(a, "tabindex") || name_is(a, "draggable") || name_is(a, "spellcheck") || name_has_prefix(a, "data-") ||
           name_has_prefix(a, "aria-");
}

void embi_inval_attr(embi_docstate *ds, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local, embi_dstr old,
                     bool had)
{
    if (had) {
        const embi_attr *now = embi_el_find_attr(el, ns, ns_uri, local);
        if (now != nullptr && embi_dstr_eq(now->value, old)) {
            return; /* CF1: set to the value it had: nothing a selector, a box or the paint reads changed */
        }
    }
    ds->dirty = true;
    if (!in_doc(ds, &el->base)) {
        return;
    }
    if (!attr_layout_neutral(ds, local)) {
        /* CF1: the checkedness the checked attribute sets is read by the paint of a checkbox or radio, never by the box
         * build (a control's box does not depend on it) */
        orig_or(ds, el, name_is(local, "checked") ? EMBI_ORIG_PAINT : EMBI_ORIG_ATTR);
    }
    if (ds->full || !ds->resolved) {
        return;
    }
    ensure(ds);
    /* The element is restyled when the attribute can matter to its own style: the box build and the hints read it (an
     * attribute that is not neutral), it is the inline style, or a selector reads it (the masks below carry SELF). */
    bool neutral = attr_layout_neutral(ds, local);
    if (!neutral || name_is(local, "style")) {
        mark(ds, el, EMBI_IVD_SELF);
    }
    if (!neutral) {
        /* an attribute-derived state (:checked, :disabled, :link ...) may flip with any attribute that is not neutral */
        uint8_t any = (uint8_t)(ds->iv.anyattr | ds->eng->iv.anyattr);
        apply_mask(ds, el, any);
    }
    apply_name(ds, el, IVK_ATTR, local); /* [class~=x], [id=y], [data-k] */
    if (name_is(local, "class")) {
        class_change(ds, el, old, had);
    } else if (name_is(local, "id")) {
        size_t cap = 0;
        char *buf = nullptr;
        if (had) {
            embi_str s = embi_style_attr_utf8(&old, &cap, &buf);
            if (s.len > 0) {
                apply_name(ds, el, IVK_ID, embi_atom_intern(s.ptr, (uint32_t)s.len));
            }
            embi_free(buf, cap);
        }
        apply_name(ds, el, IVK_ID, embi_el_id(el));
    } else {
        /* attributes whose effect reaches below the element without a selector: lang and dir (inherited, :lang and
         * :dir), and the table attributes the hints turn into rules for the cells */
        if (name_is(local, "lang") || name_is(local, "dir") || name_is(local, "cellpadding") ||
            name_is(local, "cellspacing") || name_is(local, "border") || name_is(local, "rules") ||
            name_is(local, "frame") || name_is(local, "bordercolor")) {
            mark(ds, el, EMBI_IVD_DESC);
        }
    }
}

/* W6c: the animation layer asks for a restyle of one element (its animated values moved). */
void embi_inval_mark_self(embi_docstate *ds, embi_element *el)
{
    ds->dirty = true;
    if (ds->full || !ds->resolved || !in_doc(ds, &el->base)) {
        return;
    }
    mark(ds, el, EMBI_IVD_SELF);
}

void embi_inval_mark_anim(embi_docstate *ds, embi_element *el)
{
    ds->dirty = true;
    if (ds->full || !ds->resolved || !in_doc(ds, &el->base)) {
        return;
    }
    mark(ds, el, EMBI_IVD_ANIM);
}

/* W8b: a state of el that the DOM does not carry changed (the checkedness of a checkbox or radio, the selectedness of an
 * option): el is restyled, and the rules that read an attribute-derived state pseudo-class (:checked ...) are matched again
 * where an attribute change of el would match them again (the siblings of `:checked + label`, the ancestors of :has()). */
void embi_inval_state(embi_docstate *ds, embi_element *el)
{
    ds->dirty = true;
    if (ds->full || !ds->resolved || !in_doc(ds, &el->base)) {
        return;
    }
    ensure(ds);
    mark(ds, el, EMBI_IVD_SELF);
    apply_mask(ds, el, (uint8_t)(ds->iv.anyattr | ds->eng->iv.anyattr));
}

/* CF1: the user-action state `which` (EMBI_USTATE_*) of el flipped (the pointer, a press or the focus moved): what the
 * rules that read it reach from el is restyled, as for an attribute change of el that those rules read. A rule that reads
 * it in its subject compound restyles el itself; one that reads it on an ancestor (`li:hover .destroy`) restyles el's
 * descendants, and so on. Nothing is marked when no rule reads the state. */
void embi_inval_user_state(embi_docstate *ds, embi_element *el, int which)
{
    ds->dirty = true;
    if (ds->full || !ds->resolved || !in_doc(ds, &el->base)) {
        return;
    }
    ensure(ds);
    uint8_t m = (uint8_t)(ds->iv.ustate[which] | ds->eng->iv.ustate[which]);
    ivfilt flt = {.all = false, .n = 0};
    if ((ds->iv.ustate[which] & EMBI_IVM_DESC) != 0) {
        filt_add(&flt, &ds->iv, ds->iv.ufs[which]);
    }
    if ((ds->eng->iv.ustate[which] & EMBI_IVM_DESC) != 0) {
        filt_add(&flt, &ds->eng->iv, ds->eng->iv.ufs[which]);
    }
    apply_mask_f(ds, el, m, &flt);
}

/* ------------------------------------------------------------------ bookkeeping */

static void grave_clear(embi_style_engine *e, embi_docstate *ds)
{
    for (uint32_t i = 0; i < ds->grave.len; i++) {
        embi_style_unref(e, ds->grave.data[i]);
    }
    embi_vec_clear(&ds->grave);
}

void embi_style_purge_element(embi_docstate *ds, embi_element *el)
{
    uint64_t key = (uint64_t)(uintptr_t)el;
    if (embi_hashmap_len(&ds->dmap) > 0) {
        embi_hashmap_remove(&ds->dmap, key);
    }
    if (embi_hashmap_len(&ds->omap) > 0) {
        embi_hashmap_remove(&ds->omap, key);
    }
    uint64_t idx = 0;
    if (embi_hashmap_len(&ds->cmap) > 0 && embi_hashmap_get(&ds->cmap, key, &idx)) {
        embi_style_chg *c = &ds->chg.data[idx - 1];
        embi_style_unref(ds->eng, c->old);
        c->old = nullptr;
        c->el = nullptr;
        embi_hashmap_remove(&ds->cmap, key);
    }
}

void embi_inval_ds_free(embi_style_engine *e, embi_docstate *ds)
{
    for (uint32_t i = 0; i < ds->chg.len; i++) {
        if (ds->chg.data[i].old != nullptr) {
            embi_style_unref(e, ds->chg.data[i].old);
        }
    }
    embi_vec_free(&ds->chg);
    grave_clear(e, ds);
    embi_vec_free(&ds->grave);
    embi_vec_free(&ds->outc);
    embi_vec_free(&ds->outo);
    embi_hashmap_free(&ds->dmap);
    embi_hashmap_free(&ds->cmap);
    embi_hashmap_free(&ds->omap);
    embi_hashmap_free(&ds->iv.names);
    embi_hashmap_free(&ds->iv.dfeat);
    embi_vec_free(&ds->iv.fs);
}
