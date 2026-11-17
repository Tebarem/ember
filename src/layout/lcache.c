/*
 * The layout cache: within one layout run, a box laid out again with the constraints of one of its earlier layouts in the
 * run gives back that layout's fragment, as that layout left it, instead of being laid out again.
 *
 * Why. Flex and grid containers lay their items out several times (a measure of the content height, the hypothetical
 * cross size, the final size; the track sizing of a grid), and a container that is itself an item is laid out several times
 * by its own parent, so the work at depth d grows like k^d. A page shell of nested flex and grid containers (an editor:
 * panels, toolbars, lists) laid out its 800 fragments with 60,000 fragment allocations per layout. Most of these layouts
 * repeat one that was done before in the same run: the inner boxes see the same few constraints each time.
 *
 * What the layout of a box b reads (its key). It is a function of b's box subtree, which a run does not change, and of:
 *   - the constraint space cs: the available width, the percentage bases, the fit mode, the margin overrides, and the
 *     exclusion space of the formatting context it lies in (the float rectangles and b's position in that context);
 *   - the forced sizes, when b is the forced box (c->force_box == b: the sizes of a flex or grid item); no layout reads them
 *     for another box;
 *   - the scrollbar gutters the scroll containers of its subtree start from (EMBI_BF2_VGUT, HGUT: block.c starts a box with
 *     the gutters its last layout needed, and a box that fits without a scrollbar but not with one keeps the one it started
 *     with). These are not in the key: a hit requires the current gutters of the subtree to be the ones the cached layout
 *     started from.
 * Everything else a layout reads is fixed for the run (the styles, the viewport, the fonts, the image sizes) or reset by
 * embi_lay_box (the percentage basis of intrinsic sizing). Boxes whose layout reads more are not cached: the boxes of a
 * table (a table also moves the content of its cells after their layout), the boxes laid out under a container query
 * container, for a subgrid or in a collapsed-border cell, under a line clamp (the clamp's count of lines left is read and
 * written by the layout of each line, linecl.c), and the boxes on the chain of the partial relayout (incr.c records them,
 * and the boxes below them). The cache is off in the final pass (position.c applies relative offsets and places the
 * out-of-flow boxes in place) and in partial relayouts.
 *
 * What the layout of b writes, and what a hit restores:
 *   - b's fragment tree. A fragment made by a layout is changed afterwards only by the parent's layout, which places and links
 *     it (position, parent, next sibling, margins, flags): the fragments a layout makes for its own content (lines, text runs,
 *     the boxes of children that are not cached) are never touched again in the run, while the fragment of a cached child
 *     can be given back to another layout of the parent and placed there. So an entry keeps b's fragment record as b's layout
 *     left it, and, for each fragment in its tree that another entry made (a reference, found in lcf), that entry, its
 *     serial and the fragment's record as b's layout placed it. A hit checks that every entry it reaches through references
 *     still holds the same layout (the serial: a slot that is written again gets a new one), then writes the records back,
 *     depth first. The tree is then exactly as b's layout left it.
 *   - the fragment pointers of the boxes of b's subtree (each box points to its first fragment, a measure fragment
 *     included) and their gutter flags: kept for every box of the subtree and written back.
 *   - the out-of-flow records it made (position.c), made again in order, so the last record of each box is the same.
 *   - the percentage-basis counter of intrinsic sizing (ih_need): its increase is added again.
 *   - floats added to the exclusion space: a layout that adds any is not cached.
 * Fragments are not recycled while the cache is on (block.c): a fragment the cache can give back stays where it is until
 * the run ends and the arena is rewound. The code that lays boxes out never uses a box's earlier fragment once it has laid
 * the box out again (without the cache that fragment may already be on the free list), so writing back records of fragments
 * that are also in such an earlier tree changes nothing that is still in use.
 *
 * Each box has up to LC_SLOTS entries (the measure, the hypothetical and the final layout of an item); the least recently
 * used one is written over.
 *
 * EMBER_VERIFY_INCREMENTAL=1 lays every document out a second time without the cache and compares the fragment trees, for
 * full layouts too (layout.c). EMBER_NO_LAYOUT_CACHE=1 turns the cache off (for bisecting a difference).
 */
#include "layout_int.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* The force bits of a key. */
enum {
    LC_FORCED = 1u,
    LC_WSET = 2u,
    LC_HSET = 4u,
    LC_HNONE = 8u,
    LC_HSOFT = 16u,
    LC_NOMINMAXW = 32u,
    LC_HRAW = 64u,
};

#define LC_SLOTS 8u
#define LC_RECTS_MAX 64u
#define LC_GUT (EMBI_BF2_VGUT | EMBI_BF2_HGUT)
/* The box states and references one run may keep (about 16 and 200 bytes each); past these the run stops caching. */
#define LC_STATES_MAX (1u << 20)
#define LC_REFS_MAX (1u << 16)

typedef struct embi_lc_bs {
    embi_frag *frag; /* the box's fragment pointer after the layout */
    uint16_t g0, g1; /* its gutter flags before and after */
} embi_lc_bs;

typedef struct embi_lc_ref {
    embi_frag *frag; /* a fragment another entry made, in this entry's tree */
    uint32_t ent;    /* that entry: index + 1 in lcv */
    uint32_t serial; /* ... and its serial then */
    embi_frag snap;  /* the fragment's record as this entry's layout placed it */
    embi_fext xsnap;
    bool xhas;
} embi_lc_ref;

typedef struct embi_lc_ent {
    const embi_box *box;
    uint32_t serial; /* 0: the slot holds nothing */
    uint32_t next;   /* the box's next entry: index + 1 in lcv, 0 for none */
    uint32_t used;   /* hits and stores stamp it (the slot written over is the least recently used) */
    /* the key */
    embi_cs cs;      /* excl is cleared: has_excl, the rectangles (lcr) and bfc_x, bfc_y stand for it */
    bool has_excl;
    uint32_t nrects, roff;
    uint8_t force;
    float fw, fh;
    /* the result */
    embi_lres r;
    embi_frag snap;  /* the fragment record as the layout left it */
    embi_fext xsnap;
    bool xhas;
    uint32_t pos0, pos1;
    uint32_t ih_need;
    uint32_t soff, sn;   /* the boxes of the subtree, in lcs */
    uint32_t refoff, nref; /* the references, in lcref */
    uint32_t run;    /* the run that made it (ld->lc_run) */
    bool xrun;       /* a later run may reuse it (moved_by_final_pass) */
} embi_lc_ent;

static int g_lc_off = -1; /* -1: not read yet, 0: on, 1: off */

void embi_lay_set_cache_enabled(bool on)
{
    g_lc_off = on ? 0 : 1;
}

bool embi_lay_cache_enabled(void)
{
    if (g_lc_off < 0) {
        g_lc_off = SDL_getenv("EMBER_NO_LAYOUT_CACHE") != nullptr ? 1 : 0;
    }
    return g_lc_off == 0;
}

void embi_lay_lc_drop(embi_lay_doc *ld)
{
    embi_hashmap_clear(&ld->lcmap);
    embi_hashmap_clear(&ld->lcf);
    embi_vec_clear(&ld->lcv);
    embi_vec_clear(&ld->lcr);
    embi_vec_clear(&ld->lcs);
    embi_vec_clear(&ld->lcref);
    embi_vec_clear(&ld->lct);
    ld->lc_keep = false;
}

void embi_lay_lc_begin(embi_lay_doc *ld, bool on)
{
    ld->lc_on = on && !ld->scratch && embi_lay_cache_enabled();
    if (!ld->lc_keep || !ld->lc_on) {
        embi_lay_lc_drop(ld);
    }
    embi_vec_clear(&ld->lct);
    ld->lc_hits = 0;
    ld->lc_misses = 0;
    ld->lc_xhits = 0;
    ld->lc_run++;
    ld->lc_keep = ld->lc_on; /* what this run caches may serve the next one (embi_lay_lc_end) */
}

void embi_lay_lc_end(embi_lay_doc *ld)
{
    ld->lc_on = false; /* the entries stay (ld->lc_keep) */
    embi_vec_clear(&ld->lct);
}

void embi_lay_lc_free(embi_lay_doc *ld)
{
    embi_hashmap_free(&ld->lcmap);
    embi_hashmap_free(&ld->lcf);
    embi_vec_free(&ld->lcv);
    embi_vec_free(&ld->lcr);
    embi_vec_free(&ld->lcs);
    embi_vec_free(&ld->lcref);
    embi_vec_free(&ld->lct);
    ld->lc_on = false;
    ld->lc_keep = false;
}

bool embi_lay_lc_keepable(const embi_lay_doc *ld, size_t fa_used)
{
    /* Each run adds the fragments of what it laid out to the arena (the earlier ones may still be given back); past three
     * times what a run left in an empty arena, the next run starts it empty again, without the kept entries. */
    return fa_used <= 3u * ld->lc_fa_base + (2u << 20) && ld->lcs.len <= LC_STATES_MAX / 2u &&
           ld->lcref.len <= LC_REFS_MAX / 2u && ld->lcr.len <= (1u << 20);
}

/* The next box after x in a pre-order walk of the subtree of top (text boxes have no children). */
static embi_box *walk_next(const embi_box *top, embi_box *x)
{
    if (!embi_box_is_text(x) && x->first != nullptr) {
        return x->first;
    }
    while (x != top && x->next == nullptr) {
        x = x->parent;
    }
    return x == top ? nullptr : x->next;
}

/* The entries of b no longer hold a layout of it. Entries whose trees reference them fail refs_hold from now on. */
static void ent_kill(embi_lay_doc *ld, const embi_box *b)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(&ld->lcmap, (uint64_t)(uintptr_t)b, &v)) {
        return;
    }
    for (uint32_t ei = (uint32_t)v; ei != 0; ei = ld->lcv.data[ei - 1].next) {
        embi_lc_ent *e = &ld->lcv.data[ei - 1];
        if (e->serial != 0) {
            uint64_t fv = 0;
            if (embi_hashmap_get(&ld->lcf, (uint64_t)(uintptr_t)e->r.frag, &fv) && fv == ei) {
                (void)embi_hashmap_remove(&ld->lcf, (uint64_t)(uintptr_t)e->r.frag);
            }
            e->serial = 0;
        }
    }
}

void embi_lay_lc_dirty(embi_lay_doc *ld, const embi_box *b)
{
    if (embi_hashmap_len(&ld->lcmap) == 0) {
        return;
    }
    for (const embi_box *p = b; p != nullptr; p = p->parent) {
        ent_kill(ld, p);
    }
}

void embi_lay_lc_dirty_subtree(embi_lay_doc *ld, const embi_box *b)
{
    if (embi_hashmap_len(&ld->lcmap) == 0 || b == nullptr) {
        return;
    }
    embi_box *top = (embi_box *)(uintptr_t)b;
    for (embi_box *x = top; x != nullptr; x = walk_next(top, x)) {
        if (!embi_box_is_text(x)) {
            ent_kill(ld, x);
        }
    }
    embi_lay_lc_dirty(ld, b->parent);
}

/* A box below the top of an entry whose fragments the final pass moves or adds to (position.c: the relative and sticky
 * offsets, the out-of-flow boxes placed in their containing blocks). An entry made in an earlier run holds its fragment tree
 * as that run's final pass left it, so a later run reuses it only when that pass changed nothing below its top fragment
 * (whose record the entry keeps as the layout left it). */
static bool moved_by_final_pass(const embi_lctx *c, const embi_box *x)
{
    if ((x->flags & EMBI_BF_ABSPOS) != 0) {
        return true;
    }
    if ((x->flags & EMBI_BF_ANON) != 0 || x->style == nullptr) {
        return false; /* (an anonymous box holds its parent's style for the inherited properties) */
    }
    uint16_t pos = embi_lay_ssum_of(c, x->style)->pos;
    return pos == EMBI_KW_relative || pos == EMBI_KW_sticky || pos == EMBI_KW_absolute || pos == EMBI_KW_fixed;
}

static bool same_bits(float a, float b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

/* The key of the layout of b with cs in the context c, in k (the rectangles are compared from cs). */
static void key_make(const embi_lctx *c, const embi_box *b, const embi_cs *cs, embi_lc_ent *k)
{
    memset(&k->cs, 0, sizeof k->cs);
    k->cs.avail_w = cs->avail_w;
    k->cs.cb_w = cs->cb_w;
    k->cs.cb_h = cs->cb_h;
    k->cs.fit = cs->fit;
    k->has_excl = cs->excl != nullptr;
    if (k->has_excl) {
        k->cs.bfc_x = cs->bfc_x; /* read only with an exclusion space */
        k->cs.bfc_y = cs->bfc_y;
    }
    k->cs.mg_ovr = cs->mg_ovr;
    if (cs->mg_ovr) {
        k->cs.ml_ovr = cs->ml_ovr;
        k->cs.mr_ovr = cs->mr_ovr;
    }
    k->nrects = cs->excl != nullptr ? cs->excl->rects.len : 0;
    k->force = 0;
    k->fw = 0.0f;
    k->fh = 0.0f;
    if (c->force_box == b) {
        k->force = (uint8_t)(LC_FORCED | (c->force_wset ? LC_WSET : 0u) | (c->force_hset ? LC_HSET : 0u) |
                             (c->force_hnone ? LC_HNONE : 0u) | (c->force_hsoft ? LC_HSOFT : 0u) |
                             (c->force_nominmaxw ? LC_NOMINMAXW : 0u) | (c->hraw_box == b ? LC_HRAW : 0u));
        k->fw = c->force_wset ? c->force_w : 0.0f;
        k->fh = c->force_hset ? c->force_h : 0.0f;
    }
}

static bool key_same(const embi_lay_doc *ld, const embi_lc_ent *e, const embi_lc_ent *k, const embi_cs *cs)
{
    const embi_cs *a = &e->cs, *b = &k->cs;
    if (e->has_excl != k->has_excl || e->nrects != k->nrects || e->force != k->force || !same_bits(e->fw, k->fw) ||
        !same_bits(e->fh, k->fh) || !same_bits(a->avail_w, b->avail_w) || !same_bits(a->cb_w, b->cb_w) ||
        !same_bits(a->cb_h, b->cb_h) || a->fit != b->fit || a->mg_ovr != b->mg_ovr || !same_bits(a->ml_ovr, b->ml_ovr) ||
        !same_bits(a->mr_ovr, b->mr_ovr) || !same_bits(a->bfc_x, b->bfc_x) || !same_bits(a->bfc_y, b->bfc_y)) {
        return false;
    }
    return k->nrects == 0 ||
           memcmp(&ld->lcr.data[e->roff], cs->excl->rects.data, (size_t)k->nrects * sizeof(embi_excl_rect)) == 0;
}

static bool cacheable(const embi_lctx *c, const embi_box *b, const embi_cs *cs)
{
    return b->tr == EMBI_TR_NONE && c->gsub == nullptr && c->edge_box == nullptr && c->ncq == 0 && !c->cl.on &&
           (cs->excl == nullptr || cs->excl->rects.len <= LC_RECTS_MAX);
}

/* True when the fragment records of entry e (index + 1) can be written back: it and every entry it reaches through its
 * references still hold the layouts they held when the references were taken. */
static bool refs_hold(const embi_lay_doc *ld, uint32_t ei, uint32_t serial, uint32_t depth)
{
    const embi_lc_ent *e = &ld->lcv.data[ei - 1];
    if (e->serial != serial || depth > 512u) {
        return false;
    }
    for (uint32_t i = 0; i < e->nref; i++) {
        const embi_lc_ref *r = &ld->lcref.data[e->refoff + i];
        if (!refs_hold(ld, r->ent, r->serial, depth + 1u)) {
            return false;
        }
    }
    return true;
}

static void put_record(embi_frag *f, const embi_frag *snap, const embi_fext *xsnap, bool xhas)
{
    *f = *snap;
    if (xhas) {
        *embi_frag_ext(f) = *xsnap;
    }
}

/* Writes back the fragment records of entry e (index + 1): its own fragment, then, depth first, those of its references. */
static void refs_put(embi_lay_doc *ld, uint32_t ei)
{
    const embi_lc_ent *e = &ld->lcv.data[ei - 1];
    put_record(e->r.frag, &e->snap, &e->xsnap, e->xhas);
    for (uint32_t i = 0; i < e->nref; i++) {
        const embi_lc_ref *r = &ld->lcref.data[e->refoff + i];
        refs_put(ld, r->ent);
        put_record(r->frag, &r->snap, &r->xsnap, r->xhas);
    }
}

bool embi_lay_lc_get(embi_lctx *c, embi_box *b, const embi_cs *cs, embi_lres *out, embi_lc_mark *m)
{
    embi_lay_doc *ld = c->ld;
    memset(m, 0, sizeof *m);
    if (!ld->lc_on || embi_box_is_text(b) || !cacheable(c, b, cs)) {
        return false;
    }
    uint64_t v = 0;
    if (embi_hashmap_get(&ld->lcmap, (uint64_t)(uintptr_t)b, &v)) {
        embi_lc_ent k;
        key_make(c, b, cs, &k);
        for (uint32_t ei = (uint32_t)v; ei != 0; ei = ld->lcv.data[ei - 1].next) {
            embi_lc_ent *e = &ld->lcv.data[ei - 1];
            if (e->serial == 0 || (e->run != ld->lc_run && !e->xrun) || !key_same(ld, e, &k, cs)) {
                continue;
            }
            /* the gutters the scroll containers below start from must be the ones the cached layout started from */
            const embi_lc_bs *s = &ld->lcs.data[e->soff];
            uint32_t i = 0;
            bool same = true;
            for (embi_box *x = b; x != nullptr && same; x = walk_next(b, x)) {
                if (!embi_box_is_text(x)) {
                    same = i < e->sn && (x->flags2 & LC_GUT) == s[i].g0;
                    i++;
                }
            }
            if (!same || i != e->sn || !refs_hold(ld, ei, e->serial, 0)) {
                break; /* one entry per key */
            }
            refs_put(ld, ei);
            i = 0;
            for (embi_box *x = b; x != nullptr; x = walk_next(b, x)) {
                if (!embi_box_is_text(x)) {
                    x->frag = s[i].frag;
                    x->flags2 = (uint16_t)((x->flags2 & ~LC_GUT) | s[i].g1);
                    i++;
                }
            }
            *out = e->r;
            if (e->pos1 > e->pos0) {
                embi_lay_pos_replay(c, e->pos0, e->pos1);
            }
            c->ih_need += e->ih_need;
            e->used = ++ld->lc_serial;
            ld->lc_hits++;
            if (e->run != ld->lc_run) {
                ld->lc_xhits++;
            }
            return true;
        }
    }
    /* a miss: the box is laid out, and an entry is written when that ends */
    m->slot = 1;
    m->pos0 = embi_lay_pos_count(c);
    m->nrects0 = cs->excl != nullptr ? cs->excl->rects.len : 0;
    m->ih_need0 = c->ih_need;
    m->t0 = ld->lct.len;
    for (embi_box *x = b; x != nullptr; x = walk_next(b, x)) {
        if (!embi_box_is_text(x)) {
            embi_vec_push(&ld->lct, (uint16_t)(x->flags2 & LC_GUT));
        }
    }
    ld->lc_misses++;
    return false;
}

/* The slot for the layout of b with key k: the entry with the same key, else a new one while the box has fewer than
 * LC_SLOTS, else the least recently used. Returns index + 1. */
static uint32_t slot_for(embi_lay_doc *ld, const embi_box *b, const embi_lc_ent *k, const embi_cs *cs)
{
    uint64_t v = 0;
    uint32_t n = 0, lru = 0, last = 0;
    if (embi_hashmap_get(&ld->lcmap, (uint64_t)(uintptr_t)b, &v)) {
        for (uint32_t ei = (uint32_t)v; ei != 0; ei = ld->lcv.data[ei - 1].next) {
            const embi_lc_ent *e = &ld->lcv.data[ei - 1];
            if (e->serial != 0 && key_same(ld, e, k, cs)) {
                return ei; /* the same key: written over */
            }
        }
        for (uint32_t ei = (uint32_t)v; ei != 0; ei = ld->lcv.data[ei - 1].next) {
            const embi_lc_ent *e = &ld->lcv.data[ei - 1];
            if (e->serial == 0) {
                return ei;
            }
            if (lru == 0 || e->used < ld->lcv.data[lru - 1].used) {
                lru = ei;
            }
            last = ei;
            n++;
        }
    }
    if (n >= LC_SLOTS) {
        return lru;
    }
    embi_lc_ent z;
    memset(&z, 0, sizeof z);
    z.box = b;
    embi_vec_push(&ld->lcv, z);
    uint32_t ni = ld->lcv.len;
    if (last != 0) {
        ld->lcv.data[last - 1].next = ni;
    } else {
        (void)embi_hashmap_put(&ld->lcmap, (uint64_t)(uintptr_t)b, ni);
    }
    return ni;
}

/* Adds to lcref the fragments below g that other entries made (as they are placed now), and walks the others. */
static bool collect_refs(embi_lay_doc *ld, const embi_frag *g, uint32_t *n, uint32_t depth)
{
    if (depth > 1024u) {
        return false;
    }
    for (embi_frag *x = embi_frag_first(g); x != nullptr; x = x->next) {
        uint64_t rv = 0;
        if (x->kind == EMBI_FK_BOX && embi_hashmap_get(&ld->lcf, (uint64_t)(uintptr_t)x, &rv)) {
            const embi_lc_ent *re = &ld->lcv.data[rv - 1];
            if (re->serial != 0 && re->r.frag == x) {
                embi_lc_ref r;
                memset(&r, 0, sizeof r);
                r.frag = x;
                r.ent = (uint32_t)rv;
                r.serial = re->serial;
                r.snap = *x;
                r.xhas = (x->flags & EMBI_FF_EXT) != 0;
                if (r.xhas) {
                    r.xsnap = *embi_frag_ext(x);
                }
                embi_vec_push(&ld->lcref, r);
                (*n)++;
                continue;
            }
        }
        if (x->kind != EMBI_FK_TEXT && !collect_refs(ld, x, n, depth + 1u)) {
            return false;
        }
    }
    return true;
}

void embi_lay_lc_put(embi_lctx *c, embi_box *b, const embi_cs *cs, const embi_lres *out, const embi_lc_mark *m)
{
    embi_lay_doc *ld = c->ld;
    if (m->slot == 0) {
        return;
    }
    uint32_t t0 = m->t0, tn = ld->lct.len - t0;
    embi_frag *f = out->frag;
    uint32_t n1 = cs->excl != nullptr ? cs->excl->rects.len : 0;
    if (!ld->lc_on || f == nullptr || f->box != b || f->kind != EMBI_FK_BOX || f->inl != 0 ||
        (f->flags & EMBI_FF_TABLE) != 0 || n1 != m->nrects0 || ld->lcs.len + tn > LC_STATES_MAX ||
        ld->lcref.len > LC_REFS_MAX) {
        ld->lct.len = t0;
        return; /* no fragment of its own to give back, it added floats to the formatting context, or the run kept enough */
    }
    embi_lc_ent k;
    key_make(c, b, cs, &k);
    uint32_t ei = slot_for(ld, b, &k, cs);
    embi_lc_ent *e = &ld->lcv.data[ei - 1];
    if (e->serial != 0) {
        (void)embi_hashmap_remove(&ld->lcf, (uint64_t)(uintptr_t)e->r.frag);
    }
    e->serial = 0;
    e->cs = k.cs;
    e->has_excl = k.has_excl;
    e->nrects = k.nrects;
    e->force = k.force;
    e->fw = k.fw;
    e->fh = k.fh;
    if (e->nrects != 0) {
        e->roff = ld->lcr.len;
        for (uint32_t i = 0; i < e->nrects; i++) {
            embi_vec_push(&ld->lcr, cs->excl->rects.data[i]);
        }
    }
    e->r = *out;
    e->snap = *f;
    e->xhas = (f->flags & EMBI_FF_EXT) != 0;
    if (e->xhas) {
        e->xsnap = *embi_frag_ext(f);
    }
    e->pos0 = m->pos0;
    e->pos1 = embi_lay_pos_count(c);
    e->ih_need = c->ih_need - m->ih_need0;
    /* the boxes of the subtree */
    e->soff = ld->lcs.len;
    e->run = ld->lc_run;
    e->xrun = e->pos1 == e->pos0;
    uint32_t i = 0;
    bool ok = true;
    for (embi_box *x = b; x != nullptr; x = walk_next(b, x)) {
        if (!embi_box_is_text(x)) {
            if (i >= tn) {
                ok = false; /* the subtree changed shape: cannot happen within a run */
                break;
            }
            if (x != b && e->xrun && moved_by_final_pass(c, x)) {
                e->xrun = false;
            }
            embi_lc_bs s = {x->frag, ld->lct.data[t0 + i], (uint16_t)(x->flags2 & LC_GUT)};
            embi_vec_push(&ld->lcs, s);
            i++;
        }
    }
    e->sn = i;
    ld->lct.len = t0;
    /* the fragments of other entries in the tree: walked from f, not entering them */
    e->refoff = ld->lcref.len;
    e->nref = 0;
    uint32_t nref = 0;
    ok = ok && collect_refs(ld, f, &nref, 0);
    ld->lcv.data[ei - 1].nref = nref;
    e = &ld->lcv.data[ei - 1];
    if (!ok || i != tn) {
        e->nref = 0;
        return;
    }
    e->serial = ++ld->lc_serial;
    e->used = e->serial;
    (void)embi_hashmap_put(&ld->lcf, (uint64_t)(uintptr_t)f, ei);
}
