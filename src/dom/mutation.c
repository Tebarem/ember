/*
 * MutationObserver core (W6d, DOM Standard "queue a mutation record" and transient registered observers). See
 * dom/mutation.h for the model: the DOM hooks queue records, the binding delivers them. Nothing here runs script.
 */
#include "dom/internal.h"
#include "dom/mutation.h"

/* A transient registration: the observer of a removed subtree, kept until the observer is notified. */
typedef struct mtrans {
    embi_mo *mo;
    embi_node *src; /* the node of the registration this transient registration was made from (its source) */
    embi_mreg reg;
} mtrans;

/* Scratch flag of embi_mut_release (node flags: CONNECTED 0x01, INLINE_DATA 0x02, HELD 0x04). */
#define NF_MARK 0x08u

struct embi_mut {
    embi_document *doc;
    embi_node **held; /* nodes with EMBI_NF_HELD, each listed once */
    uint32_t nheld, capheld;
    bool touched;     /* a reference was dropped since the last embi_mut_release */
    embi_mo **mos; /* observers with at least one observation, in the order they started observing */
    uint32_t nmos, capmos;
    mtrans *trans;
    uint32_t ntrans, captrans;
    embi_mo **active; /* observers with queued records (take_active sorts them by creation order) */
    uint32_t nactive, capactive;
    bool signaled;
    void (*signal)(void *user);
    void *signal_user;
    void *ext;
    void (*ext_free)(void *p);
};

enum { K_CHILD = 1, K_ATTR, K_CHAR };

/* ------------------------------------------------------------------ small helpers */

/* Grows a pointer array of *cap entries (esz bytes each) to hold need entries. Returns the new block. */
static void *grow(void *p, uint32_t *cap, uint32_t need, size_t esz)
{
    if (need <= *cap) {
        return p;
    }
    uint32_t nc = *cap ? *cap : 4u;
    while (nc < need) {
        nc *= 2u;
    }
    void *q = embi_realloc(p, (size_t)*cap * esz, (size_t)nc * esz);
    *cap = nc;
    return q;
}

static void free_arr(void *p, uint32_t cap, size_t esz)
{
    if (p) {
        embi_free(p, (size_t)cap * esz);
    }
}

void embi_mut_hold(embi_node *n)
{
    if (!n || (n->flags & EMBI_NF_HELD)) {
        return;
    }
    embi_mut *m = embi_mut_get(n->doc);
    n->flags |= EMBI_NF_HELD;
    m->held = grow(m->held, &m->capheld, m->nheld + 1u, sizeof *m->held);
    m->held[m->nheld++] = n;
}

/* Number of nodes in the sibling run first..last (linked through next). */
static uint32_t run_len(embi_node *first, embi_node *last)
{
    uint32_t n = 0;
    if (!first) {
        return 0;
    }
    for (embi_node *x = first;; x = x->next) {
        n++;
        if (x == last || !x->next) {
            break;
        }
    }
    return n;
}

static embi_dstr copy_dstr(embi_dstr d)
{
    embi_dstr out = embi_dstr_empty();
    if (d.len == 0) {
        return out;
    }
    size_t bytes = (size_t)d.len * (d.wide ? 2u : 1u);
    void *p = embi_alloc(bytes);
    SDL_memcpy(p, d.ptr, bytes);
    out.ptr = p;
    out.len = d.len;
    out.wide = d.wide;
    return out;
}

static void free_dstr(embi_dstr d)
{
    if (d.len) {
        embi_free((void *)d.ptr, (size_t)d.len * (d.wide ? 2u : 1u));
    }
}

static void copy_opts(embi_mo_opts *dst, const embi_mo_opts *src)
{
    *dst = *src;
    dst->filter = nullptr;
    if (src->filter && src->nfilter) {
        dst->filter = embi_alloc(src->nfilter * sizeof *src->filter);
        SDL_memcpy(dst->filter, src->filter, src->nfilter * sizeof *src->filter);
    } else {
        dst->nfilter = 0;
    }
}

static void free_opts(embi_mo_opts *o)
{
    if (o->filter) {
        embi_free(o->filter, o->nfilter * sizeof *o->filter);
    }
    o->filter = nullptr;
    o->nfilter = 0;
}

/* Does an observation with these options want a record of this kind for an attribute (ns, local) or for children and
 * character data? */
static bool opt_match(const embi_mo_opts *o, int kind, uint8_t ns, embi_atom local)
{
    switch (kind) {
    case K_CHILD:
        return o->child_list;
    case K_CHAR:
        return o->character_data;
    default:
        if (!o->attributes) {
            return false;
        }
        if (!o->has_filter) {
            return true;
        }
        if (ns != EMBI_NS_NULL) {
            return false;
        }
        for (uint32_t i = 0; i < o->nfilter; i++) {
            if (o->filter[i] == local) {
                return true;
            }
        }
        return false;
    }
}

static bool opt_old(const embi_mo_opts *o, int kind)
{
    if (kind == K_ATTR) {
        return o->attr_old;
    }
    return kind == K_CHAR && o->char_old;
}

static void ptr_list_remove(embi_mo **arr, uint32_t *n, embi_mo *mo)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (arr[i] == mo) {
            for (uint32_t j = i + 1; j < *n; j++) {
                arr[j - 1] = arr[j];
            }
            (*n)--;
            return;
        }
    }
}

/* Drops the transient registrations of mo (only those made from the registration of src when src is not nullptr). */
static void remove_trans_match(embi_mut *m, embi_mo *mo, const embi_node *src)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < m->ntrans; i++) {
        if (m->trans[i].mo == mo && (!src || m->trans[i].src == src)) {
            free_opts(&m->trans[i].reg.opts);
            m->touched = true;
            continue;
        }
        if (w != i) {
            m->trans[w] = m->trans[i];
        }
        w++;
    }
    m->ntrans = w;
}

static void remove_trans_of(embi_mut *m, embi_mo *mo)
{
    remove_trans_match(m, mo, nullptr);
}

/* ------------------------------------------------------------------ records */

static void rec_free_contents(embi_mrec *r)
{
    if (r->added) {
        embi_free(r->added, r->nadded * sizeof *r->added);
    }
    if (r->removed) {
        embi_free(r->removed, r->nremoved * sizeof *r->removed);
    }
    free_dstr(r->old_value);
    r->added = nullptr;
    r->removed = nullptr;
    r->old_value = embi_dstr_empty();
}

void embi_mrec_free_all(embi_mrec *recs, uint32_t n)
{
    if (!recs) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        rec_free_contents(&recs[i]);
    }
    free_arr(recs, n, sizeof *recs);
}

static void mo_drop_recs(embi_mo *mo)
{
    for (uint32_t i = 0; i < mo->nrecs; i++) {
        rec_free_contents(&mo->recs[i]);
    }
    free_arr(mo->recs, mo->caprecs, sizeof *mo->recs);
    mo->recs = nullptr;
    mo->nrecs = 0;
    mo->caprecs = 0;
}

static void mo_drop_regs(embi_mo *mo)
{
    for (uint32_t i = 0; i < mo->nregs; i++) {
        free_opts(&mo->regs[i].opts);
    }
    free_arr(mo->regs, mo->capregs, sizeof *mo->regs);
    mo->regs = nullptr;
    mo->nregs = 0;
    mo->capregs = 0;
}

static embi_mrec *push_rec(embi_mo *mo)
{
    mo->recs = grow(mo->recs, &mo->caprecs, mo->nrecs + 1u, sizeof *mo->recs);
    embi_mrec *r = &mo->recs[mo->nrecs++];
    SDL_memset(r, 0, sizeof *r);
    return r;
}

static void activate(embi_mut *m, embi_mo *mo)
{
    if (mo->active) {
        return;
    }
    mo->active = true;
    m->active = grow(m->active, &m->capactive, m->nactive + 1u, sizeof *m->active);
    m->active[m->nactive++] = mo;
}

/* Asks for the delivery once per batch: the signal runs after the batch is complete. */
static void flush(embi_mut *m, bool queued)
{
    if (queued && m->nactive && !m->signaled && m->signal) {
        m->signaled = true;
        m->signal(m->signal_user);
    }
}

/* One observer that matches a change, with whether it wants the old value. */
typedef struct hit {
    embi_mo *mo;
    bool old;
} hit;

static void hit_add(hit *h, uint32_t *n, embi_mo *mo, bool old)
{
    for (uint32_t i = 0; i < *n; i++) {
        if (h[i].mo == mo) {
            h[i].old = h[i].old || old;
            return;
        }
    }
    h[*n].mo = mo;
    h[*n].old = old;
    (*n)++;
}

/* The observers that match a change of kind at target, each once. The caller frees *out with hits_free(*cap). */
static uint32_t gather(embi_mut *m, embi_node *target, int kind, uint8_t ns, embi_atom local, hit **out,
                       uint32_t *cap)
{
    *cap = m->nmos + m->ntrans;
    hit *h = embi_alloc((size_t)(*cap ? *cap : 1u) * sizeof *h);
    uint32_t n = 0;
    for (embi_node *x = target; x; x = x->parent) {
        bool self = x == target;
        for (uint32_t i = 0; i < m->nmos; i++) {
            embi_mo *mo = m->mos[i];
            for (uint32_t j = 0; j < mo->nregs; j++) {
                const embi_mreg *r = &mo->regs[j];
                if (r->node != x || (!self && !r->opts.subtree) || !opt_match(&r->opts, kind, ns, local)) {
                    continue;
                }
                hit_add(h, &n, mo, opt_old(&r->opts, kind));
            }
        }
        for (uint32_t i = 0; i < m->ntrans; i++) {
            const mtrans *t = &m->trans[i];
            if (t->reg.node != x || (!self && !t->reg.opts.subtree) ||
                !opt_match(&t->reg.opts, kind, ns, local)) {
                continue;
            }
            hit_add(h, &n, t->mo, opt_old(&t->reg.opts, kind));
        }
    }
    *out = h;
    return n;
}

static void hits_free(hit *h, uint32_t cap)
{
    embi_free(h, (size_t)(cap ? cap : 1u) * sizeof *h);
}

/* ------------------------------------------------------------------ document state */

embi_mut *embi_mut_get(embi_document *doc)
{
    if (!doc->mut) {
        embi_mut *m = embi_calloc(1, sizeof *m);
        m->doc = doc;
        doc->mut = m;
    }
    return doc->mut;
}

embi_mut *embi_mut_peek(const embi_document *doc)
{
    return doc->mut;
}

bool embi_mut_watched(const embi_node *node)
{
    const embi_document *doc = node->doc;
    embi_mut *m = doc ? doc->mut : nullptr;
    return m && m->nmos > 0;
}

void embi_mut_set_signal(embi_document *doc, void (*fn)(void *user), void *user)
{
    embi_mut *m = embi_mut_get(doc);
    m->signal = fn;
    m->signal_user = user;
}

void *embi_mut_ext(const embi_document *doc)
{
    return doc->mut ? ((embi_mut *)doc->mut)->ext : nullptr;
}

void embi_mut_set_ext(embi_document *doc, void *p, void (*free_fn)(void *p))
{
    embi_mut *m = embi_mut_get(doc);
    m->ext = p;
    m->ext_free = free_fn;
}

void embi_mut_free(embi_document *doc)
{
    embi_mut *m = doc->mut;
    if (!m) {
        return;
    }
    for (uint32_t i = 0; i < m->nheld; i++) {
        m->held[i]->flags &= (uint8_t)~EMBI_NF_HELD;
    }
    free_arr(m->held, m->capheld, sizeof *m->held);
    for (uint32_t i = 0; i < m->nmos; i++) {
        embi_mo *mo = m->mos[i];
        mo_drop_regs(mo);
        mo_drop_recs(mo);
        mo->mut = nullptr;
        mo->active = false;
    }
    for (uint32_t i = 0; i < m->ntrans; i++) {
        free_opts(&m->trans[i].reg.opts);
    }
    if (m->ext && m->ext_free) {
        m->ext_free(m->ext);
    }
    free_arr(m->mos, m->capmos, sizeof *m->mos);
    free_arr(m->trans, m->captrans, sizeof *m->trans);
    free_arr(m->active, m->capactive, sizeof *m->active);
    embi_free(m, sizeof *m);
    doc->mut = nullptr;
}

/* ------------------------------------------------------------------ observers */

bool embi_mo_observe(embi_mo *mo, embi_document *doc, embi_node *target, const embi_mo_opts *opts)
{
    embi_mut *m = embi_mut_get(doc);
    bool first = mo->mut == nullptr;
    /* One document per MutationObserver: the binding observes only nodes of its context's document. */
    SDL_assert(first || mo->mut == m);
    if (first) {
        mo->mut = m;
        m->mos = grow(m->mos, &m->capmos, m->nmos + 1u, sizeof *m->mos);
        m->mos[m->nmos++] = mo;
    }
    embi_mut_hold(target);
    for (uint32_t j = 0; j < mo->nregs; j++) {
        if (mo->regs[j].node == target) {
            /* Replacing the options removes the transient registrations made from this registration (DOM Standard,
             * observe(), step 7.1). */
            remove_trans_match(m, mo, target);
            free_opts(&mo->regs[j].opts);
            copy_opts(&mo->regs[j].opts, opts);
            return first;
        }
    }
    mo->regs = grow(mo->regs, &mo->capregs, mo->nregs + 1u, sizeof *mo->regs);
    embi_mreg *r = &mo->regs[mo->nregs++];
    r->node = target;
    copy_opts(&r->opts, opts);
    return first;
}

bool embi_mo_disconnect(embi_mo *mo)
{
    embi_mut *m = mo->mut;
    mo_drop_regs(mo);
    mo_drop_recs(mo);
    if (!m) {
        return false;
    }
    m->touched = true;
    remove_trans_of(m, mo);
    ptr_list_remove(m->mos, &m->nmos, mo);
    if (mo->active) {
        ptr_list_remove(m->active, &m->nactive, mo);
        mo->active = false;
    }
    mo->mut = nullptr;
    return true;
}

void embi_mut_detach_if(embi_document *doc, bool (*pred)(embi_mo *mo, void *ctx), void *ctx)
{
    embi_mut *m = doc->mut;
    if (!m) {
        return;
    }
    /* Walks backwards: a disconnect removes the entry at i and shifts only the entries already visited. */
    for (uint32_t i = m->nmos; i-- > 0;) {
        if (i < m->nmos && pred(m->mos[i], ctx)) {
            (void)embi_mo_disconnect(m->mos[i]);
        }
    }
}

bool embi_mo_observing(const embi_mo *mo)
{
    return mo->mut != nullptr;
}

uint32_t embi_mo_take_records(embi_mo *mo, embi_mrec **out)
{
    uint32_t n = mo->nrecs;
    *out = nullptr;
    if (n && mo->mut) {
        mo->mut->touched = true;
    }
    if (n) {
        embi_mrec *arr = embi_alloc(n * sizeof *arr);
        SDL_memcpy(arr, mo->recs, n * sizeof *arr);
        *out = arr;
    }
    free_arr(mo->recs, mo->caprecs, sizeof *mo->recs);
    mo->recs = nullptr;
    mo->nrecs = 0;
    mo->caprecs = 0;
    return n;
}

void embi_mut_touch(embi_document *doc)
{
    embi_mut *m = embi_mut_get(doc);
    m->touched = true;
}

/* Marks a held node that a live reference reaches (embi_mut_release). */
static void mark_node(void *mctx, embi_node *n)
{
    (void)mctx;
    if (n && (n->flags & EMBI_NF_HELD)) {
        n->flags |= NF_MARK;
    }
}

void embi_mut_release(embi_document *doc, embi_mut_refs_fn extra, void *ctx)
{
    embi_mut *m = doc->mut;
    if (!m || !m->touched) {
        return;
    }
    m->touched = false;
    if (m->nheld == 0) {
        return;
    }
    for (uint32_t i = 0; i < m->nmos; i++) {
        embi_mo *mo = m->mos[i];
        for (uint32_t j = 0; j < mo->nregs; j++) {
            mark_node(nullptr, mo->regs[j].node);
        }
        for (uint32_t j = 0; j < mo->nrecs; j++) {
            const embi_mrec *r = &mo->recs[j];
            mark_node(nullptr, r->target);
            mark_node(nullptr, r->prev);
            mark_node(nullptr, r->next);
            for (uint32_t k = 0; k < r->nadded; k++) {
                mark_node(nullptr, r->added[k]);
            }
            for (uint32_t k = 0; k < r->nremoved; k++) {
                mark_node(nullptr, r->removed[k]);
            }
        }
    }
    for (uint32_t i = 0; i < m->ntrans; i++) {
        mark_node(nullptr, m->trans[i].reg.node);
    }
    if (extra) {
        extra(ctx, mark_node, nullptr);
    }
    /* Split the held list: the marked nodes stay, the others are released. */
    uint32_t total = m->nheld;
    embi_node **rel = embi_alloc((size_t)total * sizeof *rel);
    uint32_t nrel = 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < total; i++) {
        embi_node *n = m->held[i];
        if (n->flags & NF_MARK) {
            n->flags &= (uint8_t)~NF_MARK;
            m->held[w++] = n;
        } else {
            n->flags &= (uint8_t)~EMBI_NF_HELD;
            rel[nrel++] = n;
        }
    }
    m->nheld = w;
    /* The detached top of each released node, each once, found before anything is freed. A top that is attached, a
     * document or a fragment (its owner frees it) is left alone. */
    embi_node **roots = embi_alloc((size_t)total * sizeof *roots);
    uint32_t nroots = 0;
    for (uint32_t i = 0; i < nrel; i++) {
        embi_node *top = rel[i];
        while (top->parent) {
            top = top->parent;
        }
        if (top->type == EMBI_NODE_DOCUMENT || top->type == EMBI_NODE_FRAGMENT || (top->flags & EMBI_NF_CONNECTED) ||
            (top->flags & NF_MARK)) {
            continue;
        }
        top->flags |= NF_MARK;
        roots[nroots++] = top;
    }
    for (uint32_t i = 0; i < nroots; i++) {
        roots[i]->flags &= (uint8_t)~NF_MARK;
    }
    for (uint32_t i = 0; i < nroots; i++) {
        /* Distinct tops are disjoint trees, so freeing one leaves the others valid. */
        (void)embi_destroy_if_unowned(roots[i]);
    }
    embi_free(roots, (size_t)total * sizeof *roots);
    embi_free(rel, (size_t)total * sizeof *rel);
}

uint32_t embi_mut_take_active(embi_document *doc, embi_mo ***out)
{
    embi_mut *m = doc->mut;
    *out = nullptr;
    if (!m) {
        return 0;
    }
    uint32_t n = m->nactive;
    if (n) {
        embi_mo **arr = embi_alloc(n * sizeof *arr);
        SDL_memcpy(arr, m->active, n * sizeof *arr);
        /* Notify in creation order (the DOM Standard's list of all MutationObservers), stable insertion sort. */
        for (uint32_t i = 1; i < n; i++) {
            embi_mo *x = arr[i];
            uint32_t j = i;
            while (j > 0 && arr[j - 1u]->seq > x->seq) {
                arr[j] = arr[j - 1u];
                j--;
            }
            arr[j] = x;
        }
        for (uint32_t i = 0; i < n; i++) {
            arr[i]->active = false;
        }
        *out = arr;
    }
    /* The notify step removes the transient registrations of EVERY MutationObserver, also those without records. */
    for (uint32_t i = 0; i < m->ntrans; i++) {
        free_opts(&m->trans[i].reg.opts);
    }
    if (m->ntrans) {
        m->touched = true;
    }
    m->ntrans = 0;
    m->nactive = 0;
    m->signaled = false;
    return n;
}

/* ------------------------------------------------------------------ hooks for the DOM */

static void add_trans(embi_mut *m, embi_mo *mo, embi_node *node, embi_node *src, const embi_mo_opts *opts)
{
    for (uint32_t i = 0; i < m->ntrans; i++) {
        if (m->trans[i].mo == mo && m->trans[i].reg.node == node && m->trans[i].src == src) {
            return;
        }
    }
    m->trans = grow(m->trans, &m->captrans, m->ntrans + 1u, sizeof *m->trans);
    mtrans *t = &m->trans[m->ntrans++];
    t->mo = mo;
    t->src = src;
    t->reg.node = node;
    copy_opts(&t->reg.opts, opts);
    embi_mut_hold(node);
}

void embi_mut_removed(embi_node *parent, embi_node *const *nodes, uint32_t n)
{
    embi_mut *m = embi_mut_peek(parent->doc);
    if (!m || m->nmos == 0) {
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        embi_node *node = nodes[k];
        for (embi_node *x = parent; x; x = x->parent) {
            for (uint32_t i = 0; i < m->nmos; i++) {
                embi_mo *mo = m->mos[i];
                for (uint32_t j = 0; j < mo->nregs; j++) {
                    const embi_mreg *r = &mo->regs[j];
                    if (r->node == x && r->opts.subtree) {
                        add_trans(m, mo, node, r->node, &r->opts);
                    }
                }
            }
            /* The registered list of x also holds transient registrations (a node removed earlier in this task). */
            for (uint32_t i = 0; i < m->ntrans; i++) {
                if (m->trans[i].reg.node == x && m->trans[i].reg.opts.subtree) {
                    /* add_trans may realloc m->trans: take shallow copies first (the filter block stays valid). */
                    embi_mo *tmo = m->trans[i].mo;
                    embi_node *tsrc = m->trans[i].src;
                    embi_mo_opts o = m->trans[i].reg.opts;
                    add_trans(m, tmo, node, tsrc, &o);
                }
            }
        }
    }
}

void embi_mut_childlist(embi_node *parent, embi_node *first_added, embi_node *last_added,
                        embi_node *const *removed, uint32_t nremoved, embi_node *prev, embi_node *next)
{
    embi_mut *m = embi_mut_peek(parent->doc);
    if (!m || m->nmos == 0) {
        return;
    }
    uint32_t nadded = run_len(first_added, last_added);
    if (nadded == 0 && nremoved == 0) {
        return;
    }
    hit *h = nullptr;
    uint32_t hcap = 0;
    uint32_t nh = gather(m, parent, K_CHILD, EMBI_NS_NULL, 0, &h, &hcap);
    for (uint32_t i = 0; i < nh; i++) {
        embi_mrec *r = push_rec(h[i].mo);
        r->type = EMBI_MR_CHILD_LIST;
        r->target = parent;
        r->prev = prev;
        r->next = next;
        embi_mut_hold(parent);
        embi_mut_hold(prev);
        embi_mut_hold(next);
        if (nadded) {
            r->added = embi_alloc(nadded * sizeof *r->added);
            r->nadded = nadded;
            uint32_t k = 0;
            for (embi_node *x = first_added;; x = x->next) {
                r->added[k++] = x;
                embi_mut_hold(x);
                if (x == last_added || !x->next) {
                    break;
                }
            }
        }
        if (nremoved) {
            r->removed = embi_alloc(nremoved * sizeof *r->removed);
            r->nremoved = nremoved;
            for (uint32_t k = 0; k < nremoved; k++) {
                r->removed[k] = removed[k];
                embi_mut_hold(removed[k]);
            }
        }
        activate(m, h[i].mo);
    }
    flush(m, nh > 0);
    hits_free(h, hcap);
}

void embi_mut_attribute(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local, embi_dstr old,
                        bool had_old)
{
    embi_node *target = &el->base;
    embi_mut *m = embi_mut_peek(target->doc);
    if (!m || m->nmos == 0) {
        return;
    }
    hit *h = nullptr;
    uint32_t hcap = 0;
    uint32_t nh = gather(m, target, K_ATTR, ns, local, &h, &hcap);
    for (uint32_t i = 0; i < nh; i++) {
        embi_mrec *r = push_rec(h[i].mo);
        r->type = EMBI_MR_ATTRIBUTES;
        r->target = target;
        r->attr_ns = ns;
        r->attr_ns_uri = ns_uri;
        r->attr_local = local;
        embi_mut_hold(target);
        if (had_old && h[i].old) {
            r->has_old = true;
            r->old_value = copy_dstr(old);
        }
        activate(m, h[i].mo);
    }
    flush(m, nh > 0);
    hits_free(h, hcap);
}

void embi_mut_chardata(embi_node *node, embi_dstr old, bool had_old)
{
    embi_mut *m = embi_mut_peek(node->doc);
    if (!m || m->nmos == 0) {
        return;
    }
    hit *h = nullptr;
    uint32_t hcap = 0;
    uint32_t nh = gather(m, node, K_CHAR, EMBI_NS_NULL, 0, &h, &hcap);
    for (uint32_t i = 0; i < nh; i++) {
        embi_mrec *r = push_rec(h[i].mo);
        r->type = EMBI_MR_CHARACTER_DATA;
        r->target = node;
        embi_mut_hold(node);
        if (had_old && h[i].old) {
            r->has_old = true;
            r->old_value = copy_dstr(old);
        }
        activate(m, h[i].mo);
    }
    flush(m, nh > 0);
    hits_free(h, hcap);
}
