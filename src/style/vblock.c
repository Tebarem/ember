/*
 * Value blocks (WPTASAN): the lifetime rule of the values the animation layer puts in styles (see style.h). A block is an
 * arena the engine owns. Groups pin the blocks their slots point into from the moment they are created to the moment they
 * are freed; a block goes when its holder (the animation layer, while it fills it) has dropped it and no group pins it.
 *
 * Finding the block a value pointer lies in (VBLOCKPERF). Done once per slot of every new group while any block is sealed,
 * and a block is made and dropped per animated element and frame, so insert and remove must not cost O(blocks) (the sorted
 * address array this replaced moved up to 24 bytes per live block per insert and per remove: 137 ms a frame at 10,000
 * animated elements). The arena blocks have no alignment we could mask a pointer with (embi_alloc), and adding a header
 * that a pointer could be traced back to would need that alignment, so the memory is indexed by address instead: a hash map
 * from the page number (address >> VPAGE_SHIFT, 2 KB) to a doubly linked chain of the pieces on that page. A piece is the
 * part of one arena block on one page (a block of the default 512 bytes is on one or two pages), and carries the whole range
 * of its arena block. Lookup: one hash probe and a walk of the chain, which holds the few blocks that share a 2 KB page;
 * insert and remove: one hash probe per page the block covers and O(1) list surgery. The pieces of a block are an array in
 * the block, kept when the block goes to the free list, so a reused block allocates nothing. With no block sealed the map is
 * empty and a group costs nothing (nvpieces == 0 is tested before any lookup).
 *
 * A group that pins blocks has an entry in vpins: the block itself, or (tagged with bit 0) a list of them.
 */
#include "style_int.h"

#include <string.h>

#define VPAGE_SHIFT 11u
/* The filter in front of the page map: a table of VFILTER_SIZE counters indexed by a hash of the page number, each counting
 * the pieces on the pages that hash to it (saturated at 255, from where it is never lowered). Most slots of a group point
 * at memory that is no value block's, and the table is small enough to stay in the cache while the map is not. */
#define VFILTER_BITS 16u
#define VFILTER_SIZE (1u << VFILTER_BITS)

static uint32_t vfilter_idx(uintptr_t page)
{
    return (uint32_t)(((uint64_t)page * 0x9E3779B97F4A7C15ull) >> (64u - VFILTER_BITS));
}

/* The part of one arena block of a sealed value block on one page. */
typedef struct embi_vpiece {
    uintptr_t lo, hi; /* the whole arena block, [lo, hi) */
    uintptr_t page;
    embi_style_vblock *b;
    struct embi_vpiece *prev, *next; /* the chain of the page */
} embi_vpiece;

struct embi_style_vblock {
    embi_arena ar;
    uint32_t pins; /* groups that point into the block */
    bool held;     /* the animation layer has not dropped it yet */
    bool sealed;   /* its pieces are in the page map */
    embi_style_vblock *next; /* on the free list */
    embi_vpiece *pieces;     /* its pieces while sealed; the array is kept across reuse */
    uint32_t npieces, cappieces;
    const embi_arena_block *sealed_last; /* the arena at the seal, to check that it did not grow after it */
    size_t sealed_used;
};

/* Freed blocks kept for reuse, so that an overlay per animated element and frame does not cost two allocations. */
#define VFREE_MAX 64u

typedef struct embi_vpin_list {
    uint32_t n;
    embi_style_vblock *b[];
} embi_vpin_list;

static size_t vpin_list_bytes(uint32_t n)
{
    return sizeof(embi_vpin_list) + sizeof(embi_style_vblock *) * n;
}

static size_t block_bytes(const embi_style_vblock *b)
{
    size_t n = sizeof *b;
    for (const embi_arena_block *k = b->ar.first; k != nullptr; k = k->next) {
        n += sizeof *k + k->cap;
    }
    return n;
}

/* A sealed block's memory must not move or grow: its pieces describe it as it was sealed (a value allocated after the seal
 * would not be found, so no group would pin the block for it). */
static void check_sealed(const embi_style_vblock *b)
{
    EMBI_ASSERT(b->ar.last == b->sealed_last && (b->ar.last == nullptr || b->ar.last->used == b->sealed_used));
    (void)b;
}

/* ------------------------------------------------------------------ pages */

static embi_style_vblock *vr_find(const embi_style_engine *e, const void *v)
{
    uintptr_t p = (uintptr_t)v;
    uint64_t head = 0;
    if (e->vfilter == nullptr || e->vfilter[vfilter_idx(p >> VPAGE_SHIFT)] == 0 ||
        !embi_hashmap_get(&e->vpages, (uint64_t)(p >> VPAGE_SHIFT), &head)) {
        return nullptr;
    }
    for (const embi_vpiece *c = (const embi_vpiece *)(uintptr_t)head; c != nullptr; c = c->next) {
        if (p >= c->lo && p < c->hi) {
            return c->b;
        }
    }
    return nullptr;
}

/* Puts the pieces of b's arena blocks in the page map. */
static void vr_insert_block(embi_style_engine *e, embi_style_vblock *b)
{
    uint32_t total = 0;
    for (const embi_arena_block *k = b->ar.first; k != nullptr; k = k->next) {
        if (k->cap > 0) {
            uintptr_t lo = (uintptr_t)k->data;
            total += (uint32_t)(((lo + k->cap - 1u) >> VPAGE_SHIFT) - (lo >> VPAGE_SHIFT) + 1u);
        }
    }
    if (total > b->cappieces) {
        b->pieces = embi_realloc(b->pieces, sizeof(embi_vpiece) * b->cappieces, sizeof(embi_vpiece) * total);
        b->cappieces = total;
    }
    uint32_t n = 0;
    if (e->vfilter == nullptr) {
        e->vfilter = embi_calloc(VFILTER_SIZE, 1);
    }
    for (const embi_arena_block *k = b->ar.first; k != nullptr; k = k->next) {
        if (k->cap == 0) {
            continue;
        }
        uintptr_t lo = (uintptr_t)k->data, hi = lo + k->cap;
        for (uintptr_t pg = lo >> VPAGE_SHIFT; pg <= (hi - 1u) >> VPAGE_SHIFT; pg++) {
            embi_vpiece *c = &b->pieces[n++];
            uint64_t head = 0;
            c->lo = lo;
            c->hi = hi;
            c->page = pg;
            c->b = b;
            c->prev = nullptr;
            bool added = false;
            uint64_t *hp = embi_hashmap_ref(&e->vpages, (uint64_t)pg, (uint64_t)(uintptr_t)c, &added);
            head = *hp;
            *hp = (uint64_t)(uintptr_t)c;
            c->next = added ? nullptr : (embi_vpiece *)(uintptr_t)head;
            if (c->next != nullptr) {
                c->next->prev = c;
            }
            uint8_t *fc = &e->vfilter[vfilter_idx(pg)];
            *fc = *fc < 255u ? (uint8_t)(*fc + 1u) : *fc;
        }
    }
    b->npieces = n;
    e->nvpieces += n;
}

static void vr_remove_block(embi_style_engine *e, embi_style_vblock *b)
{
    for (uint32_t i = 0; i < b->npieces; i++) {
        embi_vpiece *c = &b->pieces[i];
        uint8_t *fc = &e->vfilter[vfilter_idx(c->page)];
        *fc = (*fc > 0 && *fc < 255u) ? (uint8_t)(*fc - 1u) : *fc;
        if (c->prev != nullptr) {
            c->prev->next = c->next;
        } else if (c->next != nullptr) {
            embi_hashmap_put(&e->vpages, (uint64_t)c->page, (uint64_t)(uintptr_t)c->next);
        } else {
            embi_hashmap_remove(&e->vpages, (uint64_t)c->page);
        }
        if (c->next != nullptr) {
            c->next->prev = c->prev;
        }
    }
    e->nvpieces -= b->npieces;
    b->npieces = 0;
}

/* ------------------------------------------------------------------ blocks */

static void block_free(embi_style_engine *e, embi_style_vblock *b)
{
    if (b->sealed) {
        check_sealed(b);
        vr_remove_block(e, b);
    }
    e->vblock_bytes -= block_bytes(b);
    e->nvblocks--;
    if (e->nvfree < VFREE_MAX) {
        embi_arena_reset(&b->ar); /* keeps its first block; in the poison mode of the tests, poisoned and kept aside */
        b->next = e->vfree;
        e->vfree = b;
        e->nvfree++;
        return;
    }
    embi_arena_free(&b->ar); /* poisoned and kept aside in the poison mode of the tests */
    embi_free(b->pieces, sizeof(embi_vpiece) * b->cappieces);
    embi_free(b, sizeof *b);
}

embi_style_vblock *embi_style_vblock_open(embi_style_engine *e, size_t block_size)
{
    embi_style_vblock *b = e->vfree;
    if (b != nullptr && b->ar.block_size == block_size) {
        e->vfree = b->next;
        e->nvfree--;
        b->next = nullptr;
        b->pins = 0;
        b->sealed = false;
    } else {
        b = embi_calloc(1, sizeof *b);
        embi_arena_init(&b->ar, block_size);
    }
    b->held = true;
    e->nvblocks++;
    e->vblock_bytes += sizeof *b;
    return b;
}

embi_arena *embi_style_vblock_arena(embi_style_vblock *b)
{
    return &b->ar;
}

void embi_style_vblock_seal(embi_style_engine *e, embi_style_vblock *b)
{
    EMBI_ASSERT(!b->sealed);
    b->sealed = true;
    b->sealed_last = b->ar.last;
    b->sealed_used = b->ar.last != nullptr ? b->ar.last->used : 0;
    vr_insert_block(e, b);
    e->vblock_bytes += block_bytes(b) - sizeof *b;
}

void embi_style_vblock_drop(embi_style_engine *e, embi_style_vblock *b)
{
    b->held = false;
    if (b->pins == 0) {
        if (!b->sealed) {
            e->vblock_bytes += block_bytes(b) - sizeof *b; /* block_free takes the whole size off */
        }
        block_free(e, b);
    }
}

const embi_style_vblock *embi_style_vblock_debug_find(const embi_style_engine *e, const void *p)
{
    return vr_find(e, p);
}

uint32_t embi_style_vblock_count(const embi_style_engine *e)
{
    return e->nvblocks;
}

size_t embi_style_vblock_bytes(const embi_style_engine *e)
{
    return e->vblock_bytes;
}

/* ------------------------------------------------------------------ pins */

void embi_style_vpin_group(embi_style_engine *e, const embi_style_group *g)
{
    if (e->nvpieces == 0) {
        return;
    }
    embi_style_vblock *found[16];
    uint32_t nf = 0;
    EMBI_VEC(embi_style_vblock *) more = {0}; /* past 16 different blocks (a group made of that many animations) */
    for (uint32_t i = 0; i < g->n; i++) {
        const embi_value *v = g->vals[i];
        embi_style_vblock *b = v != nullptr ? vr_find(e, v) : nullptr;
        if (b == nullptr) {
            continue;
        }
        check_sealed(b);
        bool seen = false;
        for (uint32_t j = 0; j < nf && !seen; j++) {
            seen = found[j] == b;
        }
        for (uint32_t j = 0; j < more.len && !seen; j++) {
            seen = more.data[j] == b;
        }
        if (seen) {
            continue;
        }
        if (nf < 16u) {
            found[nf++] = b;
        } else {
            embi_vec_push(&more, b);
        }
    }
    uint32_t total = nf + more.len;
    if (total == 0) {
        return;
    }
    uint64_t rec;
    if (total == 1) {
        rec = (uint64_t)(uintptr_t)found[0];
    } else {
        embi_vpin_list *l = embi_alloc(vpin_list_bytes(total));
        l->n = total;
        memcpy(l->b, found, sizeof(found[0]) * nf);
        if (more.len > 0) {
            memcpy(&l->b[nf], more.data, sizeof(found[0]) * more.len);
        }
        rec = (uint64_t)(uintptr_t)l | 1u;
    }
    for (uint32_t j = 0; j < nf; j++) {
        found[j]->pins++;
    }
    for (uint32_t j = 0; j < more.len; j++) {
        more.data[j]->pins++;
    }
    embi_vec_free(&more);
    embi_hashmap_put(&e->vpins, (uint64_t)(uintptr_t)g, rec);
}

static void unpin(embi_style_engine *e, embi_style_vblock *b)
{
    EMBI_ASSERT(b->pins > 0);
    if (--b->pins == 0 && !b->held) {
        block_free(e, b);
    }
}

void embi_style_vunpin_group(embi_style_engine *e, const embi_style_group *g)
{
    uint64_t rec = 0;
    if (embi_hashmap_len(&e->vpins) == 0 || !embi_hashmap_take(&e->vpins, (uint64_t)(uintptr_t)g, &rec)) {
        return;
    }
    if ((rec & 1u) == 0) {
        unpin(e, (embi_style_vblock *)(uintptr_t)rec);
        return;
    }
    embi_vpin_list *l = (embi_vpin_list *)(uintptr_t)(rec & ~(uint64_t)1u);
    for (uint32_t j = 0; j < l->n; j++) {
        unpin(e, l->b[j]);
    }
    embi_free(l, vpin_list_bytes(l->n));
}

void embi_style_vblocks_free(embi_style_engine *e)
{
    /* the groups that are left (styles nobody released) let go of their blocks */
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->vpins);
    uint64_t k = 0, rec = 0;
    while (embi_hashmap_iter_next(&it, &k, &rec)) {
        if ((rec & 1u) != 0) {
            embi_vpin_list *l = (embi_vpin_list *)(uintptr_t)(rec & ~(uint64_t)1u);
            embi_free(l, vpin_list_bytes(l->n));
        }
    }
    embi_hashmap_free(&e->vpins);
    EMBI_VEC(embi_style_vblock *) sealed = {0};
    it = embi_hashmap_iter_begin(&e->vpages);
    while (embi_hashmap_iter_next(&it, &k, &rec)) {
        for (const embi_vpiece *c = (const embi_vpiece *)(uintptr_t)rec; c != nullptr; c = c->next) {
            if (c == c->b->pieces) { /* once per block */
                embi_vec_push(&sealed, c->b);
            }
        }
    }
    for (uint32_t i = 0; i < sealed.len; i++) {
        block_free(e, sealed.data[i]);
    }
    embi_vec_free(&sealed);
    embi_hashmap_free(&e->vpages);
    if (e->vfilter != nullptr) {
        embi_free(e->vfilter, VFILTER_SIZE);
        e->vfilter = nullptr;
    }
    while (e->vfree != nullptr) {
        embi_style_vblock *b = e->vfree;
        e->vfree = b->next;
        embi_arena_free(&b->ar);
        embi_free(b->pieces, sizeof(embi_vpiece) * b->cappieces);
        embi_free(b, sizeof *b);
    }
    e->nvfree = 0;
}

/* ------------------------------------------------------------------ the poison check of the tests */

uint32_t embi_style_debug_poisoned(const embi_style *s)
{
    if (s == nullptr) {
        return 0;
    }
    const embi_layout *L = embi_layout_get();
    uint32_t n = 0;
    for (uint32_t g = 0; g < L->ngroups; g++) {
        const embi_style_group *gr = s->groups[g];
        for (uint32_t i = 0; i < gr->n; i++) {
            if (gr->vals[i] != nullptr) {
                n += embi_value_debug_poisoned(gr->vals[i]); /* the value and its parts */
            }
        }
    }
    return n;
}

uint32_t embi_style_debug_poison_reads(const embi_style_engine *e)
{
    return e->debug_poison_reads;
}
