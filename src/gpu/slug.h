#pragma once
/*
 * Slug blob cache: the CPU side of the Slug renderer (shaders/lib/outline.slang, used by box.slang).
 *
 * A blob is the band structure of one outline: its quadratic curves in em space, split into N
 * horizontal and N vertical bands (N from the curve count, 1..16), each band listing the curves
 * that reach it, sorted for an early exit in the shader. Blobs live in one GPU storage buffer, the
 * slug buffer. Its space is sub-allocated: fresh space grows from the high-water mark (hwm), and
 * freed ranges below it are reused first. The buffer grows on demand up to EMBI_SLUG_MAX_BYTES.
 *
 * Caching. A blob is keyed by a caller key (uint64). A key with bit 63 set belongs to paths
 * (path.c); caller keys for fonts must leave it clear. Once built, a blob is reused until it is
 * evicted. Eviction takes the least recently used blob that no frame touched within the last
 * EMBI_SLUG_QUARANTINE frames. Two things trigger it: live bytes above EMBI_SLUG_SOFT_BYTES (checked
 * at each frame start and before each build), and a full buffer (a build evicts until the freed
 * bytes would cover it). Stale path versions age out this way, so a path that changes every frame
 * cannot fill the buffer. embi_slug_drop removes one blob at once.
 *
 * A freed range is reused only once EMBI_SLUG_QUARANTINE frames have passed since the last frame
 * that read its blob (for a failed build, since it was made). So a frame still in flight never
 * reads a range being rewritten. (This assumes frames complete within that many frames of being
 * recorded.)
 *
 * Per frame, in order: embi_slug_begin_frame, then embi_slug_get for each prim recorded, then
 * embi_slug_upload in the frame's command buffer before the render pass that draws the blobs.
 * Blobs staged but not yet uploaded stay queued until the next upload, so an abandoned frame does
 * not leave the cache pointing at data that never reached the GPU. A failed upload keeps the queue
 * and the old buffer contents, and the next upload retries it.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

#include "gpu/render_types.h"

#define EMBI_SLUG_MAX_BYTES (1u << 27)  /* the storage-buffer range Vulkan guarantees */
#define EMBI_SLUG_SOFT_BYTES (16u << 20) /* live blobs are evicted down to this when old enough */
#define EMBI_SLUG_MAX_BANDS 16u
#define EMBI_SLUG_QUARANTINE 3u         /* frames a freed range waits before it may be rewritten */

/* What a prim needs from a blob. box is the em box, nh and nv the band counts, offset the byte
 * offset of the blob in the slug buffer. */
typedef struct embi_slug_entry {
    float box[4];
    uint32_t offset;
    uint32_t nh;
    uint32_t nv;
} embi_slug_entry;

/* Counters for tests and tuning. */
typedef struct embi_slug_stats {
    uint64_t live_bytes;  /* bytes of cached blobs */
    uint32_t blobs;       /* cached blobs */
    uint32_t hwm;         /* bytes handed out so far: the buffer must hold this many */
    uint32_t cap;         /* physical size of the slug buffer, 0 before the first upload */
    uint32_t uploaded;    /* bytes copied by the last embi_slug_upload */
    uint64_t evictions;   /* blobs evicted or dropped */
    uint64_t full_misses; /* embi_slug_get calls refused for lack of buffer space */
} embi_slug_stats;

typedef struct embi_slug embi_slug;

[[nodiscard]] embi_slug *embi_slug_create(SDL_GPUDevice *dev);
void embi_slug_destroy(embi_slug *s);

/* Starts a frame: advances the frame stamp used by LRU eviction and the quarantine, and evicts
 * old blobs while live bytes exceed the soft cap. */
void embi_slug_begin_frame(embi_slug *s);

/* Finds or builds the blob for key from n closed quadratic curves (n >= 1), and marks it used in
 * this frame. The curves are only read on a cache miss. Returns false when the curves are empty,
 * have a non-finite coordinate, exceed the limits, or the slug buffer has no room for the blob
 * (the blob is then skipped; a later frame can build it). */
[[nodiscard]] bool embi_slug_get(embi_slug *s, uint64_t key, const embi_qcurve *curves, uint32_t n,
                                 embi_slug_entry *out);

/* Whether a blob is cached for key (embi_slug_get would then not need the curves). */
[[nodiscard]] bool embi_slug_has(const embi_slug *s, uint64_t key);

/* Removes the blob for key, if it is cached. Its range is reused after the quarantine, so a
 * prim already recorded in this frame still reads valid data. */
void embi_slug_drop(embi_slug *s, uint64_t key);

/* Writes the blobs staged since the last upload into the slug buffer, growing the buffer first
 * when the used range needs it (the old contents are copied over). Records its copies into cmd.
 * Call outside any render pass. Returns false on failure, with the buffer and the stage left as
 * they were: the next call retries. With nothing staged and a buffer in place, it records nothing.
 * The slug buffer is created by the first upload that has staged data. */
[[nodiscard]] bool embi_slug_upload(embi_slug *s, SDL_GPUCommandBuffer *cmd);

/* Creates the slug buffer and its transfer buffer at their minimum size, so the first upload that
 * has data creates nothing. It touches only the buffer fields, so it may run on another thread
 * while this thread builds blobs; the caller must join it before the next embi_slug_upload.
 * Returns the number of GPU buffers it created. The contents are uninitialised: nothing reads a range
 * before a blob is written there. A failure is not an error: the first upload then creates the buffers as before. */
uint32_t embi_slug_prewarm(embi_slug *s);

/* The slug buffer, valid after the first embi_slug_upload that staged data (or embi_slug_prewarm). */
[[nodiscard]] SDL_GPUBuffer *embi_slug_buffer(const embi_slug *s);

[[nodiscard]] embi_slug_stats embi_slug_get_stats(const embi_slug *s);

/* Test hook: makes the next embi_slug_upload fail at one step, then disarms. 1 fails the growth
 * of the slug buffer, 2 the growth of the transfer buffer, 3 the start of the copy pass. */
void embi_slug_fault_next_upload(embi_slug *s, int step);
