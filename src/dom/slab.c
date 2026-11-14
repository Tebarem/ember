#include "dom/slab.h"

#define EMBI_SLAB_HDR 16u          /* owner + total size, immediately before the payload */
#define EMBI_SLAB_SMALL_MAX 512u   /* largest block served from chunks */
#define EMBI_SLAB_MIN_BLOCK 32u    /* room for the free-list link */
#define EMBI_SLAB_CHUNK 65536u
#define EMBI_SLAB_CHUNK_MAX (1024u * 1024u)
#define EMBI_SLAB_CLASSES (EMBI_SLAB_SMALL_MAX / 16u + 1u)

typedef struct embi_slab_hdr {
    embi_slab *owner;
    size_t total; /* whole block size, including the header (and big-block links) */
} embi_slab_hdr;

typedef struct embi_slab_chunk {
    struct embi_slab_chunk *next;
    size_t size;
} embi_slab_chunk;

/* Big blocks start with these links; the header follows at offset 16 and the payload at 32. */
typedef struct embi_slab_big {
    struct embi_slab_big *prev;
    struct embi_slab_big *next;
} embi_slab_big;

struct embi_slab {
    uint32_t refs;
    size_t live_bytes;
    size_t live_blocks;
    void *free_list[EMBI_SLAB_CLASSES]; /* block bases, linked through the payload */
    embi_slab_chunk *chunks;
    size_t chunk_bytes; /* total bytes of chunks so far: later chunks grow with it */
    unsigned char *bump;
    unsigned char *bump_end;
    embi_slab_big *bigs;
    EMBI_VEC(embi_slab *) kept;
};

static size_t round16(size_t n)
{
    return (n + 15u) & ~(size_t)15u;
}

embi_slab *embi_slab_new(void)
{
    embi_slab *s = embi_calloc(1, sizeof *s);
    s->refs = 1;
    return s;
}

void embi_slab_retain(embi_slab *s)
{
    EMBI_ASSERT(s->refs > 0);
    s->refs++;
}

void embi_slab_release(embi_slab *s)
{
    EMBI_ASSERT(s->refs > 0);
    if (--s->refs != 0) {
        return;
    }
    embi_slab_chunk *c = s->chunks;
    while (c) {
        embi_slab_chunk *next = c->next;
        embi_free(c, c->size);
        c = next;
    }
    embi_slab_big *b = s->bigs;
    while (b) {
        embi_slab_big *next = b->next;
        embi_slab_hdr *h = (embi_slab_hdr *)((unsigned char *)b + EMBI_SLAB_HDR);
        embi_free(b, h->total);
        b = next;
    }
    for (uint32_t i = 0; i < s->kept.len; i++) {
        embi_slab_release(s->kept.data[i]);
    }
    embi_vec_free(&s->kept);
    embi_free(s, sizeof *s);
}

void embi_slab_keep(embi_slab *dst, embi_slab *src)
{
    if (dst == src || src == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < dst->kept.len; i++) {
        if (dst->kept.data[i] == src) {
            return;
        }
    }
    embi_slab_retain(src);
    embi_vec_push(&dst->kept, src);
}

void embi_slab_drop_kept(embi_slab *s)
{
    /* Releasing a kept slab can free it, and that can release slabs it keeps, but never s itself:
     * s is still held by its document reference while this runs. */
    for (uint32_t i = 0; i < s->kept.len; i++) {
        embi_slab_release(s->kept.data[i]);
    }
    s->kept.len = 0;
}

/* Turns the unused tail of the current chunk into free-list blocks, so starting a new chunk does
 * not strand it. A tail under EMBI_SLAB_MIN_BLOCK (16 bytes) cannot hold a block and is lost. */
static void release_tail(embi_slab *s)
{
    size_t rem = (size_t)(s->bump_end - s->bump);
    while (rem >= EMBI_SLAB_MIN_BLOCK) {
        size_t blk = rem < EMBI_SLAB_SMALL_MAX ? rem : EMBI_SLAB_SMALL_MAX;
        if (rem - blk != 0 && rem - blk < EMBI_SLAB_MIN_BLOCK) {
            blk -= 16u; /* leave a remainder that is itself a valid block */
        }
        size_t cls = blk / 16u;
        unsigned char *base = s->bump;
        *(void **)(base + EMBI_SLAB_HDR) = s->free_list[cls];
        s->free_list[cls] = base;
        s->bump += blk;
        rem -= blk;
    }
}

static void *bump_alloc(embi_slab *s, size_t total)
{
    if ((size_t)(s->bump_end - s->bump) < total) {
        release_tail(s);
        /* Chunks grow with the slab (a quarter of what it holds, up to 1 MiB, in 64 KiB steps).
         * A big document then costs a few direct system allocations instead of hundreds of
         * 64 KiB heap blocks, which are several times slower to commit and release. Small
         * documents keep the 64 KiB chunk. */
        size_t size = (s->chunk_bytes / 4u + EMBI_SLAB_CHUNK - 1u) & ~((size_t)EMBI_SLAB_CHUNK - 1u);
        if (size < EMBI_SLAB_CHUNK) {
            size = EMBI_SLAB_CHUNK;
        } else if (size > EMBI_SLAB_CHUNK_MAX) {
            size = EMBI_SLAB_CHUNK_MAX;
        }
        if (size < total + EMBI_SLAB_HDR) {
            size = round16(total + EMBI_SLAB_HDR);
        }
        embi_slab_chunk *c = embi_alloc(size);
        s->chunk_bytes += size;
        c->next = s->chunks;
        c->size = size;
        s->chunks = c;
        s->bump = (unsigned char *)c + EMBI_SLAB_HDR;
        s->bump_end = (unsigned char *)c + size;
    }
    void *base = s->bump;
    s->bump += total;
    return base;
}

void *embi_slab_alloc(embi_slab *s, size_t size)
{
    EMBI_ASSERT(s->refs > 0);
    size_t small = round16(size + EMBI_SLAB_HDR);
    if (small < EMBI_SLAB_MIN_BLOCK) {
        small = EMBI_SLAB_MIN_BLOCK;
    }
    if (small <= EMBI_SLAB_SMALL_MAX) {
        size_t cls = small / 16u;
        unsigned char *base = s->free_list[cls];
        if (base) {
            s->free_list[cls] = *(void **)(base + EMBI_SLAB_HDR);
        } else {
            base = bump_alloc(s, small);
        }
        embi_slab_hdr *h = (embi_slab_hdr *)base;
        h->owner = s;
        h->total = small;
        s->live_bytes += small;
        s->live_blocks++;
        return base + EMBI_SLAB_HDR;
    }
    size_t total = round16(size + EMBI_SLAB_HDR + sizeof(embi_slab_big));
    unsigned char *base = embi_alloc(total);
    embi_slab_big *b = (embi_slab_big *)base;
    b->prev = nullptr;
    b->next = s->bigs;
    if (s->bigs) {
        s->bigs->prev = b;
    }
    s->bigs = b;
    embi_slab_hdr *h = (embi_slab_hdr *)(base + EMBI_SLAB_HDR);
    h->owner = s;
    h->total = total;
    s->live_bytes += total;
    s->live_blocks++;
    return base + EMBI_SLAB_HDR + sizeof(embi_slab_big);
}

void embi_slab_free(void *p)
{
    if (p == nullptr) {
        return;
    }
    unsigned char *pc = p;
    embi_slab_hdr *h = (embi_slab_hdr *)(pc - EMBI_SLAB_HDR);
    embi_slab *s = h->owner;
    size_t total = h->total;
    s->live_bytes -= total;
    s->live_blocks--;
    if (total <= EMBI_SLAB_SMALL_MAX) {
        unsigned char *base = pc - EMBI_SLAB_HDR;
        size_t cls = total / 16u;
        *(void **)(base + EMBI_SLAB_HDR) = s->free_list[cls];
        s->free_list[cls] = base;
        return;
    }
    unsigned char *base = pc - EMBI_SLAB_HDR - sizeof(embi_slab_big);
    embi_slab_big *b = (embi_slab_big *)base;
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        s->bigs = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    embi_free(base, total);
}

size_t embi_slab_capacity(const void *p)
{
    const unsigned char *pc = p;
    const embi_slab_hdr *h = (const embi_slab_hdr *)(pc - EMBI_SLAB_HDR);
    if (h->total <= EMBI_SLAB_SMALL_MAX) {
        return h->total - EMBI_SLAB_HDR;
    }
    return h->total - EMBI_SLAB_HDR - sizeof(embi_slab_big);
}

embi_slab *embi_slab_owner(const void *p)
{
    const unsigned char *pc = p;
    return ((const embi_slab_hdr *)(pc - EMBI_SLAB_HDR))->owner;
}

size_t embi_slab_live_bytes(const embi_slab *s)
{
    return s->live_bytes;
}

size_t embi_slab_live_blocks(const embi_slab *s)
{
    return s->live_blocks;
}

size_t embi_slab_kept_count(const embi_slab *s)
{
    return s->kept.len;
}
