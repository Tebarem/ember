#include "gpu/slug.h"

#include "base/base.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

/* Blob layout, in 32-bit words from the blob start (shaders/lib/outline.slang reads the same layout):
 *
 *   word 0..3    header: nh, nv, curve count, reserved
 *   word 4..    band table: 2 words per band (byte offset of the list from the blob start, count),
 *               the nh horizontal bands first, then the nv vertical ones
 *   then         curves: 6 floats each (x0 y0 cx cy x1 y1), 24 bytes
 *   then         the band lists: curve numbers, 32-bit each
 *
 * A horizontal band covers a slice of the em y range and lists the curves whose y range reaches
 * it, sorted by descending maximum x (the ray runs toward +x, so the first curves are the ones
 * that can still be hit). A vertical band does the same along x, sorted by descending maximum y.
 * Bands overlap by a sliver so a curve that touches a band edge is listed on both sides.
 *
 * Blobs are padded to a multiple of 16 bytes. */

#define EMBI_SLUG_NONE UINT32_MAX
#define EMBI_SLUG_MIN_BYTES (64u * 1024u)
#define EMBI_SLUG_MAX_CURVES (1u << 20)
#define EMBI_SLUG_ALIGN 16u
#define EMBI_SLUG_BAND_EPS 1e-5f /* band overlap, as a fraction of the box size */
#define EMBI_SLUG_FULL_LOG_FRAMES 600u /* the buffer-full warning is logged at most this often */

typedef struct embi_slug_ent {
    uint64_t key;
    uint64_t last_use; /* frame stamp of the last embi_slug_get that returned it */
    float box[4];
    uint32_t offset;
    uint32_t bytes;
    uint32_t nh;
    uint32_t nv;
    uint32_t prev; /* LRU neighbors: prev is more recent, next is less recent */
    uint32_t next;
} embi_slug_ent;

/* A free range below hwm. release is the frame that freed it (the range is reusable
 * EMBI_SLUG_QUARANTINE frames later). */
typedef struct embi_slug_range {
    uint32_t offset;
    uint32_t bytes;
    uint64_t release;
} embi_slug_range;

/* One upload: stage bytes [src, src + bytes) go to slug buffer bytes [dst, dst + bytes). */
typedef struct embi_slug_copy {
    uint32_t dst;
    uint32_t src;
    uint32_t bytes;
} embi_slug_copy;

typedef struct embi_slug_sort {
    float key;
    uint32_t idx;
} embi_slug_sort;

typedef struct embi_slug_cbox {
    float x0, x1, y0, y1;
} embi_slug_cbox;

struct embi_slug {
    SDL_GPUDevice *dev;
    uint64_t frame;
    embi_hashmap index; /* key -> entry number */
    EMBI_VEC(embi_slug_ent) ents;
    EMBI_VEC(uint32_t) free_ents;
    uint32_t lru_head; /* most recently used */
    uint32_t lru_tail; /* least recently used */

    /* Space. Bytes below hwm are either in a live blob or in ranges; bytes from hwm on are unused
     * and immediately reusable, so a fresh blob never waits for the quarantine. */
    EMBI_VEC(embi_slug_range) ranges; /* free space below hwm, sorted by offset, never adjacent */
    uint32_t hwm;

    uint64_t live_bytes;
    uint32_t live_blobs;
    uint64_t evictions;
    uint64_t full_misses;
    uint64_t last_full_log;
    uint32_t uploaded;
    int fault_step;

    SDL_GPUBuffer *buf;
    uint32_t cap; /* physical size of buf in bytes */
    SDL_GPUTransferBuffer *tb;
    uint32_t tb_cap;

    EMBI_VEC(uint32_t) stage;     /* words of blobs built since the last upload */
    EMBI_VEC(embi_slug_copy) copies;
    EMBI_VEC(uint32_t) words;     /* scratch: the blob being built */
    EMBI_VEC(embi_slug_cbox) cbox;
    EMBI_VEC(embi_slug_sort) sort_x;
    EMBI_VEC(embi_slug_sort) sort_y;
};

/* LRU list ---------------------------------------------------------------- */

static void embi_slug_lru_unlink(embi_slug *s, uint32_t i)
{
    embi_slug_ent *e = &embi_vec_at(&s->ents, i);
    if (e->prev != EMBI_SLUG_NONE) {
        embi_vec_at(&s->ents, e->prev).next = e->next;
    } else {
        s->lru_head = e->next;
    }
    if (e->next != EMBI_SLUG_NONE) {
        embi_vec_at(&s->ents, e->next).prev = e->prev;
    } else {
        s->lru_tail = e->prev;
    }
    e->prev = EMBI_SLUG_NONE;
    e->next = EMBI_SLUG_NONE;
}

static void embi_slug_lru_push(embi_slug *s, uint32_t i)
{
    embi_slug_ent *e = &embi_vec_at(&s->ents, i);
    e->prev = EMBI_SLUG_NONE;
    e->next = s->lru_head;
    if (s->lru_head != EMBI_SLUG_NONE) {
        embi_vec_at(&s->ents, s->lru_head).prev = i;
    } else {
        s->lru_tail = i;
    }
    s->lru_head = i;
}

/* Free ranges -------------------------------------------------------------- */

static uint64_t embi_slug_maxu64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static void embi_slug_range_remove(embi_slug *s, uint32_t i)
{
    SDL_memmove(&s->ranges.data[i], &s->ranges.data[i + 1],
                (size_t)(s->ranges.len - i - 1u) * sizeof(embi_slug_range));
    s->ranges.len--;
}

/* Returns [offset, offset + bytes) to the free list, merging it with adjacent ranges. */
static void embi_slug_free_range(embi_slug *s, uint32_t offset, uint32_t bytes, uint64_t release)
{
    uint32_t i = 0;
    while (i < s->ranges.len && embi_vec_at(&s->ranges, i).offset < offset) {
        i++;
    }
    const embi_slug_range r = {.offset = offset, .bytes = bytes, .release = release};
    embi_vec_push(&s->ranges, r);
    SDL_memmove(&s->ranges.data[i + 1], &s->ranges.data[i],
                (size_t)(s->ranges.len - 1u - i) * sizeof(embi_slug_range));
    s->ranges.data[i] = r;

    if (i > 0) {
        embi_slug_range *p = &embi_vec_at(&s->ranges, i - 1);
        if (p->offset + p->bytes == offset) {
            p->bytes += bytes;
            p->release = embi_slug_maxu64(p->release, release);
            embi_slug_range_remove(s, i);
            i--;
        }
    }
    if (i + 1 < s->ranges.len) {
        embi_slug_range *p = &embi_vec_at(&s->ranges, i);
        const embi_slug_range nx = embi_vec_at(&s->ranges, i + 1);
        if (p->offset + p->bytes == nx.offset) {
            p->bytes += nx.bytes;
            p->release = embi_slug_maxu64(p->release, nx.release);
            embi_slug_range_remove(s, i + 1);
        }
    }
}

/* First fit over the reusable free ranges, then fresh space above hwm. */
static bool embi_slug_alloc(embi_slug *s, uint32_t bytes, uint32_t *offset)
{
    for (uint32_t i = 0; i < s->ranges.len; i++) {
        embi_slug_range *r = &embi_vec_at(&s->ranges, i);
        if (r->release + EMBI_SLUG_QUARANTINE > s->frame || r->bytes < bytes) {
            continue;
        }
        *offset = r->offset;
        if (r->bytes == bytes) {
            embi_slug_range_remove(s, i);
        } else {
            r->offset += bytes;
            r->bytes -= bytes;
        }
        return true;
    }
    if ((uint64_t)s->hwm + bytes > EMBI_SLUG_MAX_BYTES) {
        return false;
    }
    *offset = s->hwm;
    s->hwm += bytes;
    return true;
}

/* Eviction ----------------------------------------------------------------- */

/* Removes entry i from the index and the LRU list, and frees its range. The range is stamped with
 * the last frame that read the blob, not the current one: the quarantine then counts from the
 * last read, so an evicted blob's range is reusable as soon as the frames that could read it have
 * completed (see EMBI_SLUG_QUARANTINE). */
static void embi_slug_release_ent(embi_slug *s, uint32_t i)
{
    const embi_slug_ent *e = &embi_vec_at(&s->ents, i);
    const uint64_t key = e->key;
    const uint64_t last_read = e->last_use;
    const uint32_t offset = e->offset;
    const uint32_t bytes = e->bytes;
    embi_hashmap_remove(&s->index, key);
    embi_slug_free_range(s, offset, bytes, last_read);
    embi_slug_lru_unlink(s, i);
    embi_vec_push(&s->free_ents, i);
    s->live_bytes -= bytes;
    s->live_blobs--;
    s->evictions++;
}

/* Evicts the least recently used blob if no frame has touched it within the quarantine. Returns
 * its byte count, or 0 when the tail is recent (every other blob is then more recent) or the
 * cache is empty. The freed range becomes reusable EMBI_SLUG_QUARANTINE frames later. */
static uint64_t embi_slug_evict_one(embi_slug *s)
{
    const uint32_t i = s->lru_tail;
    if (i == EMBI_SLUG_NONE) {
        return 0;
    }
    const embi_slug_ent *e = &embi_vec_at(&s->ents, i);
    if (e->last_use + EMBI_SLUG_QUARANTINE > s->frame) {
        return 0;
    }
    const uint64_t bytes = e->bytes;
    embi_slug_release_ent(s, i);
    return bytes;
}

/* Logs a refused build at most every EMBI_SLUG_FULL_LOG_FRAMES frames. */
static void embi_slug_note_full(embi_slug *s, uint32_t bytes)
{
    s->full_misses++;
    if (s->full_misses == 1u || s->frame - s->last_full_log >= EMBI_SLUG_FULL_LOG_FRAMES) {
        s->last_full_log = s->frame;
        embi_log_warn("slug: buffer full (%llu of %u bytes live, %u needed), glyph skipped "
                      "(%llu such misses so far)",
                      (unsigned long long)s->live_bytes, (unsigned)EMBI_SLUG_MAX_BYTES,
                      (unsigned)bytes, (unsigned long long)s->full_misses);
    }
}

/* Build -------------------------------------------------------------------- */

static bool embi_slug_finite(float x)
{
    return x - x == 0.0f;
}

/* Descending by key, ties by ascending index, so the order is deterministic. */
static int embi_slug_cmp_desc(const void *pa, const void *pb)
{
    const embi_slug_sort *a = pa;
    const embi_slug_sort *b = pb;
    if (a->key > b->key) {
        return -1;
    }
    if (a->key < b->key) {
        return 1;
    }
    return (a->idx < b->idx) ? -1 : (a->idx > b->idx ? 1 : 0);
}

static void embi_slug_push_f(embi_slug *s, float x)
{
    uint32_t w = 0;
    SDL_memcpy(&w, &x, sizeof w);
    embi_vec_push(&s->words, w);
}

/* Builds the blob for n curves into s->words. Writes the em box and the band count. */
static bool embi_slug_build(embi_slug *s, const embi_qcurve *c, uint32_t n, float box[4],
                            uint32_t *nb)
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    s->cbox.len = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &c[i];
        const float xs[3] = {q->x0, q->cx, q->x1};
        const float ys[3] = {q->y0, q->cy, q->y1};
        for (int k = 0; k < 3; k++) {
            if (!embi_slug_finite(xs[k]) || !embi_slug_finite(ys[k])) {
                embi_log_error("slug: non-finite outline coordinate, blob not built");
                return false;
            }
        }
        const embi_slug_cbox b = {
            .x0 = SDL_min(SDL_min(xs[0], xs[1]), xs[2]),
            .x1 = SDL_max(SDL_max(xs[0], xs[1]), xs[2]),
            .y0 = SDL_min(SDL_min(ys[0], ys[1]), ys[2]),
            .y1 = SDL_max(SDL_max(ys[0], ys[1]), ys[2]),
        };
        if (i == 0) {
            x0 = b.x0;
            x1 = b.x1;
            y0 = b.y0;
            y1 = b.y1;
        } else {
            x0 = SDL_min(x0, b.x0);
            x1 = SDL_max(x1, b.x1);
            y0 = SDL_min(y0, b.y0);
            y1 = SDL_max(y1, b.y1);
        }
        embi_vec_push(&s->cbox, b);
    }
    /* An empty box is widened to a sliver, so the band arithmetic never divides by zero. */
    if (!(x1 - x0 >= 1e-6f)) {
        x1 = x0 + 1e-6f;
    }
    if (!(y1 - y0 >= 1e-6f)) {
        y1 = y0 + 1e-6f;
    }

    uint32_t nbands = (uint32_t)(SDL_sqrtf((float)n) * 0.5f + 0.5f);
    if (nbands < 1u) {
        nbands = 1u;
    }
    if (nbands > EMBI_SLUG_MAX_BANDS) {
        nbands = EMBI_SLUG_MAX_BANDS;
    }

    /* The lists are sorted once per direction and then filtered per band, so they come out sorted. */
    s->sort_x.len = 0;
    s->sort_y.len = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_slug_sort kx = {.key = s->cbox.data[i].x1, .idx = i};
        const embi_slug_sort ky = {.key = s->cbox.data[i].y1, .idx = i};
        embi_vec_push(&s->sort_x, kx);
        embi_vec_push(&s->sort_y, ky);
    }
    SDL_qsort(s->sort_x.data, n, sizeof(embi_slug_sort), embi_slug_cmp_desc);
    SDL_qsort(s->sort_y.data, n, sizeof(embi_slug_sort), embi_slug_cmp_desc);

    s->words.len = 0;
    embi_vec_push(&s->words, nbands);
    embi_vec_push(&s->words, nbands);
    embi_vec_push(&s->words, n);
    embi_vec_push(&s->words, 0u);
    for (uint32_t i = 0; i < 4u * nbands; i++) {
        embi_vec_push(&s->words, 0u); /* band table, filled in below */
    }
    for (uint32_t i = 0; i < n; i++) {
        const embi_qcurve *q = &c[i];
        embi_slug_push_f(s, q->x0);
        embi_slug_push_f(s, q->y0);
        embi_slug_push_f(s, q->cx);
        embi_slug_push_f(s, q->cy);
        embi_slug_push_f(s, q->x1);
        embi_slug_push_f(s, q->y1);
    }

    const float hy = y1 - y0;
    const float wx = x1 - x0;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        const bool horiz = (pass == 0u);
        const uint32_t nb_pass = nbands;
        for (uint32_t j = 0; j < nb_pass; j++) {
            const float lo_src = horiz ? y0 : x0;
            const float span = horiz ? hy : wx;
            const float eps = span * EMBI_SLUG_BAND_EPS;
            const float lo = lo_src + span * ((float)j / (float)nbands) - eps;
            const float hi = lo_src + span * ((float)(j + 1u) / (float)nbands) + eps;
            const uint32_t entry = 4u + 2u * (horiz ? j : nbands + j);
            const uint32_t start = s->words.len * 4u;
            uint32_t count = 0;
            const embi_slug_sort *order = horiz ? s->sort_x.data : s->sort_y.data;
            for (uint32_t k = 0; k < n; k++) {
                const uint32_t i = order[k].idx;
                const embi_slug_cbox *b = &s->cbox.data[i];
                const float cmin = horiz ? b->y0 : b->x0;
                const float cmax = horiz ? b->y1 : b->x1;
                if (cmin <= hi && cmax >= lo) {
                    embi_vec_push(&s->words, i);
                    count++;
                }
            }
            s->words.data[entry] = start;
            s->words.data[entry + 1u] = count;
        }
    }

    box[0] = x0;
    box[1] = y0;
    box[2] = x1;
    box[3] = y1;
    *nb = nbands;
    return true;
}

/* Test hook --------------------------------------------------------------- */

static bool embi_slug_take_fault(embi_slug *s, int step)
{
    if (s->fault_step != step) {
        return false;
    }
    s->fault_step = 0;
    return true;
}

void embi_slug_fault_next_upload(embi_slug *s, int step)
{
    if (s != nullptr) {
        s->fault_step = step;
    }
}

/* Public API --------------------------------------------------------------- */

embi_slug *embi_slug_create(SDL_GPUDevice *dev)
{
    if (dev == nullptr) {
        return nullptr;
    }
    embi_slug *s = embi_calloc(1, sizeof *s);
    s->dev = dev;
    /* Starting at the quarantine length means a range released at frame 0 is reusable from the
     * first frame, even when the caller never calls embi_slug_begin_frame. */
    s->frame = EMBI_SLUG_QUARANTINE;
    s->lru_head = EMBI_SLUG_NONE;
    s->lru_tail = EMBI_SLUG_NONE;
    embi_hashmap_init(&s->index);
    return s;
}

void embi_slug_destroy(embi_slug *s)
{
    if (s == nullptr) {
        return;
    }
    SDL_GPUDevice *dev = s->dev;
    SDL_WaitForGPUIdle(dev);
    if (s->buf != nullptr) {
        SDL_ReleaseGPUBuffer(dev, s->buf);
    }
    if (s->tb != nullptr) {
        SDL_ReleaseGPUTransferBuffer(dev, s->tb);
    }
    embi_hashmap_free(&s->index);
    embi_vec_free(&s->ents);
    embi_vec_free(&s->free_ents);
    embi_vec_free(&s->ranges);
    embi_vec_free(&s->stage);
    embi_vec_free(&s->copies);
    embi_vec_free(&s->words);
    embi_vec_free(&s->cbox);
    embi_vec_free(&s->sort_x);
    embi_vec_free(&s->sort_y);
    embi_free(s, sizeof *s);
}

void embi_slug_begin_frame(embi_slug *s)
{
    if (s == nullptr) {
        return;
    }
    s->frame++;
    /* Old blobs go before the cache grows past the soft cap, so an animated or immediate-mode
     * path cannot fill the buffer with stale versions. */
    while (s->live_bytes > EMBI_SLUG_SOFT_BYTES) {
        if (embi_slug_evict_one(s) == 0) {
            break;
        }
    }
}

void embi_slug_drop(embi_slug *s, uint64_t key)
{
    if (s == nullptr) {
        return;
    }
    uint64_t found = 0;
    if (embi_hashmap_get(&s->index, key, &found)) {
        embi_slug_release_ent(s, (uint32_t)found);
    }
}

static void embi_slug_fill_out(const embi_slug_ent *e, embi_slug_entry *out)
{
    for (int i = 0; i < 4; i++) {
        out->box[i] = e->box[i];
    }
    out->offset = e->offset;
    out->nh = e->nh;
    out->nv = e->nv;
}

bool embi_slug_get(embi_slug *s, uint64_t key, const embi_qcurve *curves, uint32_t n,
                   embi_slug_entry *out)
{
    if (s == nullptr || out == nullptr) {
        return false;
    }
    uint64_t found = 0;
    if (embi_hashmap_get(&s->index, key, &found)) {
        const uint32_t i = (uint32_t)found;
        embi_slug_ent *e = &embi_vec_at(&s->ents, i);
        if (e->last_use != s->frame) {
            e->last_use = s->frame;
            embi_slug_lru_unlink(s, i);
            embi_slug_lru_push(s, i);
        }
        embi_slug_fill_out(e, out);
        return true;
    }

    if (curves == nullptr || n == 0 || n > EMBI_SLUG_MAX_CURVES) {
        return false;
    }
    float box[4];
    uint32_t nb = 0;
    if (!embi_slug_build(s, curves, n, box, &nb)) {
        return false;
    }
    /* Pad to 16 bytes. */
    while ((s->words.len * 4u) % EMBI_SLUG_ALIGN != 0u) {
        embi_vec_push(&s->words, 0u);
    }
    const uint64_t bytes64 = (uint64_t)s->words.len * 4u;
    if (bytes64 > EMBI_SLUG_MAX_BYTES) {
        embi_log_error("slug: blob of %llu bytes exceeds the slug buffer",
                       (unsigned long long)bytes64);
        return false;
    }
    const uint32_t bytes = (uint32_t)bytes64;

    /* Keep the cache under the soft cap: evict the oldest blobs first. */
    while (s->live_bytes + bytes > EMBI_SLUG_SOFT_BYTES) {
        if (embi_slug_evict_one(s) == 0) {
            break;
        }
    }
    uint32_t offset = 0;
    if (!embi_slug_alloc(s, bytes, &offset)) {
        /* Full. Evict old blobs until their bytes would cover this one. Their ranges are reusable
         * only EMBI_SLUG_QUARANTINE frames later, so this glyph is skipped and a later frame builds
         * it. */
        uint64_t freed = 0;
        while (freed < bytes) {
            const uint64_t got = embi_slug_evict_one(s);
            if (got == 0) {
                break;
            }
            freed += got;
        }
        embi_slug_note_full(s, bytes);
        return false;
    }

    uint32_t idx = 0;
    if (s->free_ents.len > 0) {
        idx = embi_vec_pop(&s->free_ents);
    } else {
        const embi_slug_ent blank = {.prev = EMBI_SLUG_NONE, .next = EMBI_SLUG_NONE};
        embi_vec_push(&s->ents, blank);
        idx = s->ents.len - 1u;
    }
    if (!embi_hashmap_put(&s->index, key, idx)) {
        embi_vec_push(&s->free_ents, idx);
        embi_slug_free_range(s, offset, bytes, s->frame);
        return false;
    }
    embi_slug_ent *e = &embi_vec_at(&s->ents, idx);
    e->key = key;
    e->last_use = s->frame;
    for (int i = 0; i < 4; i++) {
        e->box[i] = box[i];
    }
    e->offset = offset;
    e->bytes = bytes;
    e->nh = nb;
    e->nv = nb;
    embi_slug_lru_push(s, idx);
    s->live_bytes += bytes;
    s->live_blobs++;

    /* Stage the words. A blob that lands right after the previous one, with its words right
     * after them in the stage, extends that copy instead of making a new one. An evicted range
     * that a staged blob still points at is harmless: its bytes are only written into space that
     * is quarantined, and any later blob in that space is staged after them, so it wins. */
    const uint32_t src = s->stage.len * 4u;
    embi_vec_reserve(&s->stage, s->stage.len + s->words.len);
    SDL_memcpy(&s->stage.data[s->stage.len], s->words.data,
               (size_t)s->words.len * sizeof(uint32_t));
    s->stage.len += s->words.len;
    if (s->copies.len > 0) {
        embi_slug_copy *last = &embi_vec_at(&s->copies, s->copies.len - 1);
        if (last->dst + last->bytes == offset && last->src + last->bytes == src) {
            last->bytes += bytes;
            embi_slug_fill_out(e, out);
            return true;
        }
    }
    const embi_slug_copy c = {.dst = offset, .src = src, .bytes = bytes};
    embi_vec_push(&s->copies, c);
    embi_slug_fill_out(e, out);
    return true;
}

uint32_t embi_slug_prewarm(embi_slug *s)
{
    if (s == nullptr || s->buf != nullptr) {
        return 0;
    }
    SDL_GPUBuffer *nb = SDL_CreateGPUBuffer(
        s->dev, &(SDL_GPUBufferCreateInfo){
                    .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                    .size = EMBI_SLUG_MIN_BYTES,
                });
    if (nb == nullptr) {
        return 0;
    }
    uint32_t made = 1;
    if (s->tb == nullptr) {
        s->tb = SDL_CreateGPUTransferBuffer(s->dev, &(SDL_GPUTransferBufferCreateInfo){
                                                        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                        .size = EMBI_SLUG_MIN_BYTES,
                                                    });
        s->tb_cap = s->tb != nullptr ? EMBI_SLUG_MIN_BYTES : 0;
        made += s->tb != nullptr ? 1u : 0u;
    }
    s->buf = nb;
    s->cap = EMBI_SLUG_MIN_BYTES;
    return made;
}

/* Uploads the staged words. The slug buffer is never cycled: its contents must survive from one
 * frame to the next. A grow builds the new buffer under a local name, copies the old contents in
 * the same copy pass, and swaps it in only after every step has succeeded. A failure at any step
 * releases the new buffer and leaves the old one, the queue and the copies untouched. */
bool embi_slug_upload(embi_slug *s, SDL_GPUCommandBuffer *cmd)
{
    if (s == nullptr || cmd == nullptr) {
        return false;
    }
    s->uploaded = 0;
    SDL_GPUDevice *dev = s->dev;
    uint32_t need = s->hwm;
    if (need < EMBI_SLUG_MIN_BYTES) {
        need = EMBI_SLUG_MIN_BYTES;
    }
    /* Nothing staged: nothing to write. The buffer is created by the first upload with data. */
    if (s->stage.len == 0 && (s->buf == nullptr || s->cap >= need)) {
        return true;
    }

    const bool grow = (s->buf == nullptr || s->cap < need);
    uint32_t want = s->cap;
    SDL_GPUBuffer *nb = nullptr; /* the grown buffer, until it is swapped in */
    SDL_GPUBuffer *old = nullptr;
    uint32_t copied = 0;
    bool ok = false;

    do {
        if (grow) {
            uint64_t w = (uint64_t)s->cap * 2u;
            if (w < need) {
                w = need;
            }
            if (w > EMBI_SLUG_MAX_BYTES) {
                w = EMBI_SLUG_MAX_BYTES;
            }
            want = (uint32_t)w;
            if (embi_slug_take_fault(s, 1)) {
                embi_log_error("slug: injected failure creating the slug buffer");
                break;
            }
            nb = SDL_CreateGPUBuffer(
                dev, &(SDL_GPUBufferCreateInfo){
                         .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                         .size = (Uint32)want,
                     });
            if (nb == nullptr) {
                embi_log_error("slug: SDL_CreateGPUBuffer(%u bytes) failed: %s", (unsigned)want,
                               SDL_GetError());
                break;
            }
        }

        const uint32_t stage_bytes = s->stage.len * 4u;
        if (stage_bytes > 0) {
            if (s->tb == nullptr || s->tb_cap < stage_bytes) {
                uint64_t w = (uint64_t)s->tb_cap * 2u;
                if (w < stage_bytes) {
                    w = stage_bytes;
                }
                if (w < EMBI_SLUG_MIN_BYTES) {
                    w = EMBI_SLUG_MIN_BYTES;
                }
                if (embi_slug_take_fault(s, 2)) {
                    embi_log_error("slug: injected failure creating the transfer buffer");
                    break;
                }
                SDL_GPUTransferBuffer *nt = SDL_CreateGPUTransferBuffer(
                    dev, &(SDL_GPUTransferBufferCreateInfo){
                             .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                             .size = (Uint32)w,
                         });
                if (nt == nullptr) {
                    embi_log_error("slug: SDL_CreateGPUTransferBuffer(%llu bytes) failed: %s",
                                   (unsigned long long)w, SDL_GetError());
                    break;
                }
                if (s->tb != nullptr) {
                    SDL_ReleaseGPUTransferBuffer(dev, s->tb);
                }
                s->tb = nt;
                s->tb_cap = (uint32_t)w;
            }
            void *mapped = SDL_MapGPUTransferBuffer(dev, s->tb, true);
            if (mapped == nullptr) {
                embi_log_error("slug: SDL_MapGPUTransferBuffer failed: %s", SDL_GetError());
                break;
            }
            SDL_memcpy(mapped, s->stage.data, stage_bytes);
            SDL_UnmapGPUTransferBuffer(dev, s->tb);
        }

        SDL_GPUCopyPass *cp = nullptr;
        if (embi_slug_take_fault(s, 3)) {
            embi_log_error("slug: injected failure starting the copy pass");
            break;
        }
        cp = SDL_BeginGPUCopyPass(cmd);
        if (cp == nullptr) {
            embi_log_error("slug: SDL_BeginGPUCopyPass failed: %s", SDL_GetError());
            break;
        }
        SDL_GPUBuffer *dst = grow ? nb : s->buf;
        if (grow && s->buf != nullptr && s->cap > 0) {
            SDL_CopyGPUBufferToBuffer(cp, &(SDL_GPUBufferLocation){.buffer = s->buf, .offset = 0},
                                      &(SDL_GPUBufferLocation){.buffer = nb, .offset = 0}, s->cap,
                                      false);
            copied += s->cap;
        }
        for (uint32_t i = 0; i < s->copies.len; i++) {
            const embi_slug_copy *c = &embi_vec_at(&s->copies, i);
            const SDL_GPUTransferBufferLocation src = {.transfer_buffer = s->tb, .offset = c->src};
            const SDL_GPUBufferRegion region = {.buffer = dst, .offset = c->dst, .size = c->bytes};
            SDL_UploadToGPUBuffer(cp, &src, &region, false);
            copied += c->bytes;
        }
        SDL_EndGPUCopyPass(cp);

        /* Commit. Nothing below can fail. */
        if (grow) {
            old = s->buf;
            s->buf = nb;
            s->cap = want;
            nb = nullptr;
        }
        s->uploaded = copied;
        s->stage.len = 0;
        s->copies.len = 0;
        ok = true;
    } while (0);

    if (!ok && nb != nullptr) {
        SDL_ReleaseGPUBuffer(dev, nb);
    }
    if (ok && old != nullptr) {
        SDL_ReleaseGPUBuffer(dev, old);
    }
    return ok;
}

SDL_GPUBuffer *embi_slug_buffer(const embi_slug *s)
{
    return s->buf;
}

embi_slug_stats embi_slug_get_stats(const embi_slug *s)
{
    embi_slug_stats st = {0};
    if (s == nullptr) {
        return st;
    }
    st.live_bytes = s->live_bytes;
    st.blobs = s->live_blobs;
    st.hwm = s->hwm;
    st.cap = s->cap;
    st.uploaded = s->uploaded;
    st.evictions = s->evictions;
    st.full_misses = s->full_misses;
    return st;
}
