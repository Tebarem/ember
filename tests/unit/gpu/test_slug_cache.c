#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <stdint.h>

#include <SDL3/SDL_stdinc.h>

#include "gpu/gpu.h"
#include "gpu/render.h"
#include "gpu/slug.h"

/* The slug blob cache, driven directly (embi_slug) and checked against the bytes the GPU holds.
 * Blob id is a closed polygon whose geometry is a pure function of the id, so the test can
 * rebuild the expected curves and compare them with the slug buffer read back from the GPU:
 *   - growth: blobs past the initial 64 KB, the buffer grows with its old contents copied over;
 *   - upload failure at each step leaves the old contents and the queue alone, and the retry
 *     succeeds;
 *   - churn: many frames of new blobs stay under the soft cap, the buffer stays bounded, and the
 *     blobs of the last three frames (the quarantine) are never overwritten;
 *   - a full buffer refuses blobs without growing past its limit, and recovers after quarantine;
 *   - drop removes one blob, and a later get rebuilds it;
 *   - the renderer, past one growth, draws the same pixels as before.
 * Keys are small integers, so bit 63 stays clear (font keys). */

#define GROW_BLOBS 1500u
#define UPLOAD_OLD_BLOBS 10u
#define UPLOAD_NEW_BLOBS 600u
#define CHURN_FRAMES 600u
#define CHURN_PER_FRAME 40u
#define CHURN_SIDES 64u
#define FULL_SIDES 512u
#define FULL_TRIES 12000u
#define FULL_FLUSH 256u
#define RENDER_QUADS 1500u
#define MIN_BUFFER (64u * 1024u)

typedef struct cache_rig {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    embi_slug *s;
} cache_rig;

static bool cache_open(cache_rig *cr)
{
    SDL_memset(cr, 0, sizeof *cr);
    cr->g = embi_gpu_create(nullptr, false);
    if (cr->g == nullptr) {
        return false;
    }
    cr->dev = embi_gpu_device(cr->g);
    cr->s = embi_slug_create(cr->dev);
    return cr->s != nullptr;
}

static void cache_close(cache_rig *cr)
{
    embi_slug_destroy(cr->s);
    if (cr->g != nullptr) {
        embi_gpu_destroy(cr->g);
    }
    SDL_memset(cr, 0, sizeof *cr);
}

static uint64_t key_of(uint32_t id)
{
    return (uint64_t)id + 1u;
}

/* Sides of blob id for the small-blob tests: 4 to 16. */
static uint32_t small_sides(uint32_t id)
{
    return 4u + id % 13u;
}

/* Blob id: a closed polygon of `sides` straight quadratic segments. */
static void make_blob(uint32_t id, uint32_t sides, embi_qcurve *c)
{
    const float cx = (float)(id % 97u) * 4.0f;
    const float cy = (float)(id / 97u) * 4.0f;
    const float r = 1.0f + (float)(id % 5u) * 0.5f;
    for (uint32_t k = 0; k < sides; k++) {
        const float a0 = 6.2831853f * (float)k / (float)sides;
        const float a1 = 6.2831853f * (float)(k + 1u) / (float)sides;
        const float x0 = cx + r * SDL_cosf(a0);
        const float y0 = cy + r * SDL_sinf(a0);
        const float x1 = cx + r * SDL_cosf(a1);
        const float y1 = cy + r * SDL_sinf(a1);
        const embi_qcurve q = {x0, y0, 0.5f * (x0 + x1), 0.5f * (y0 + y1), x1, y1};
        c[k] = q;
    }
}

/* Uploads the staged blobs in a command buffer of their own. */
static bool cache_flush(cache_rig *cr)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(cr->dev);
    if (cmd == nullptr) {
        return false;
    }
    const bool ok = embi_slug_upload(cr->s, cmd);
    SDL_SubmitGPUCommandBuffer(cmd);
    return ok;
}

/* Copies the first `bytes` of the slug buffer to the host. SDL_free the result. */
static uint8_t *cache_read(cache_rig *cr, uint32_t bytes)
{
    SDL_GPUBuffer *buf = embi_slug_buffer(cr->s);
    if (buf == nullptr || bytes == 0) {
        return nullptr;
    }
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(
        cr->dev, &(SDL_GPUTransferBufferCreateInfo){
                     .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                     .size = bytes,
                 });
    if (tb == nullptr) {
        return nullptr;
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(cr->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUBuffer(cp,
                              &(SDL_GPUBufferRegion){.buffer = buf, .offset = 0, .size = bytes},
                              &(SDL_GPUTransferBufferLocation){.transfer_buffer = tb, .offset = 0});
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(cr->dev);

    uint8_t *out = nullptr;
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(cr->dev, tb, false);
    if (mapped != nullptr) {
        out = SDL_malloc(bytes);
        if (out != nullptr) {
            SDL_memcpy(out, mapped, bytes);
        }
        SDL_UnmapGPUTransferBuffer(cr->dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(cr->dev, tb);
    return out;
}

/* True when the blob at e->offset holds the header, band counts and curves of blob id. gpu holds
 * gpu_bytes bytes of the slug buffer. Layout: src/gpu/slug.c. */
static bool blob_on_gpu(const uint8_t *gpu, uint32_t gpu_bytes, const embi_slug_entry *e,
                        uint32_t id, uint32_t sides)
{
    embi_qcurve c[CHURN_SIDES];
    make_blob(id, sides, c);
    const uint32_t off = e->offset;
    if (off > gpu_bytes || gpu_bytes - off < 16u) {
        return false;
    }
    uint32_t hdr[3];
    SDL_memcpy(hdr, gpu + off, sizeof hdr);
    if (hdr[0] != e->nh || hdr[1] != e->nv || hdr[2] != sides) {
        return false;
    }
    const uint32_t cbase = off + 16u + 8u * (e->nh + e->nv);
    const uint32_t need = 24u * sides;
    if (cbase > gpu_bytes || gpu_bytes - cbase < need) {
        return false;
    }
    return SDL_memcmp(gpu + cbase, c, need) == 0;
}

/* ------------------------------------------------------------------ growth */

EMB_TEST(slug_cache_growth_keeps_every_blob_intact)
{
    cache_rig cr;
    if (!cache_open(&cr)) {
        cache_close(&cr);
        EMB_SKIP("no GPU device");
    }
    embi_slug_entry *ents = SDL_malloc(GROW_BLOBS * sizeof *ents);
    EMB_REQUIRE(ents != nullptr);

    embi_slug_begin_frame(cr.s);
    for (uint32_t i = 0; i < GROW_BLOBS; i++) {
        embi_qcurve c[16];
        make_blob(i, small_sides(i), c);
        EMB_CHECK(embi_slug_get(cr.s, key_of(i), c, small_sides(i), &ents[i]));
    }
    EMB_CHECK(cache_flush(&cr));

    const embi_slug_stats st = embi_slug_get_stats(cr.s);
    EMB_CHECK(st.blobs == GROW_BLOBS);
    EMB_CHECK(st.cap >= st.hwm);
    EMB_CHECK(st.cap > MIN_BUFFER);
    uint8_t *gpu = cache_read(&cr, st.hwm);
    EMB_REQUIRE(gpu != nullptr);
    for (uint32_t i = 0; i < GROW_BLOBS; i++) {
        EMB_CHECK(blob_on_gpu(gpu, st.hwm, &ents[i], i, small_sides(i)));
    }
    SDL_free(gpu);

    /* A repeated key is a hit: same entry, nothing staged, nothing copied by the next upload. */
    embi_slug_begin_frame(cr.s);
    embi_slug_entry again;
    EMB_CHECK(embi_slug_get(cr.s, key_of(7), nullptr, 0, &again));
    EMB_CHECK(again.offset == ents[7].offset && again.nh == ents[7].nh);
    EMB_CHECK(cache_flush(&cr));
    EMB_CHECK(embi_slug_get_stats(cr.s).uploaded == 0);

    /* Refused input leaves the cache alone. */
    embi_qcurve bad = {0};
    bad.x0 = SDL_sqrtf(-1.0f);
    EMB_CHECK(!embi_slug_get(cr.s, UINT64_C(1) << 40, &bad, 1, &again));
    EMB_CHECK(!embi_slug_get(cr.s, UINT64_C(1) << 41, nullptr, 0, &again));
    EMB_CHECK(embi_slug_get_stats(cr.s).blobs == GROW_BLOBS);

    SDL_free(ents);
    cache_close(&cr);
}

/* ------------------------------------------------------------------ upload failure */

EMB_TEST(slug_cache_upload_failure_keeps_old_contents_and_retries)
{
    /* Step 1: growing the slug buffer. Step 2: growing the transfer buffer. Step 3: the copy pass. */
    for (int step = 1; step <= 3; step++) {
        cache_rig cr;
        if (!cache_open(&cr)) {
            cache_close(&cr);
            EMB_SKIP("no GPU device");
        }
        const uint32_t total = UPLOAD_OLD_BLOBS + UPLOAD_NEW_BLOBS;
        embi_slug_entry *ents = SDL_malloc(total * sizeof *ents);
        EMB_REQUIRE(ents != nullptr);

        /* The first upload creates both buffers, at their minimum size. */
        embi_slug_begin_frame(cr.s);
        for (uint32_t i = 0; i < UPLOAD_OLD_BLOBS; i++) {
            embi_qcurve c[16];
            make_blob(i, small_sides(i), c);
            EMB_CHECK(embi_slug_get(cr.s, key_of(i), c, small_sides(i), &ents[i]));
        }
        EMB_CHECK(cache_flush(&cr));
        const uint32_t cap_before = embi_slug_get_stats(cr.s).cap;
        EMB_CHECK(cap_before == MIN_BUFFER);

        /* The second batch is larger than both buffers, so the next upload must grow them. */
        embi_slug_begin_frame(cr.s);
        for (uint32_t i = UPLOAD_OLD_BLOBS; i < total; i++) {
            embi_qcurve c[16];
            make_blob(i, small_sides(i), c);
            EMB_CHECK(embi_slug_get(cr.s, key_of(i), c, small_sides(i), &ents[i]));
        }
        EMB_CHECK(embi_slug_get_stats(cr.s).hwm > MIN_BUFFER);

        embi_slug_fault_next_upload(cr.s, step);
        EMB_CHECK(!cache_flush(&cr));
        const embi_slug_stats failed = embi_slug_get_stats(cr.s);
        EMB_CHECK(failed.cap == cap_before);
        EMB_CHECK(failed.blobs == total);

        /* The retry uploads the queue that failed, and the old blobs read back intact. */
        EMB_CHECK(cache_flush(&cr));
        const embi_slug_stats st = embi_slug_get_stats(cr.s);
        EMB_CHECK(st.cap >= st.hwm);
        uint8_t *gpu = cache_read(&cr, st.hwm);
        EMB_REQUIRE(gpu != nullptr);
        for (uint32_t i = 0; i < total; i++) {
            EMB_CHECK(blob_on_gpu(gpu, st.hwm, &ents[i], i, small_sides(i)));
        }
        SDL_free(gpu);
        SDL_free(ents);
        cache_close(&cr);
    }
}

/* ------------------------------------------------------------------ churn */

EMB_TEST(slug_cache_churn_stays_bounded_and_keeps_recent_blobs)
{
    cache_rig cr;
    if (!cache_open(&cr)) {
        cache_close(&cr);
        EMB_SKIP("no GPU device");
    }
    /* The entries of the last three frames, by slot (frame % 3). */
    embi_slug_entry ring[3][CHURN_PER_FRAME];
    uint32_t ring_frame[3] = {0, 0, 0};
    bool all_built = true;
    for (uint32_t f = 0; f < CHURN_FRAMES; f++) {
        embi_slug_begin_frame(cr.s);
        for (uint32_t j = 0; j < CHURN_PER_FRAME; j++) {
            const uint32_t id = f * CHURN_PER_FRAME + j;
            embi_qcurve c[CHURN_SIDES];
            make_blob(id, CHURN_SIDES, c);
            if (!embi_slug_get(cr.s, key_of(id), c, CHURN_SIDES, &ring[f % 3u][j])) {
                all_built = false;
            }
        }
        ring_frame[f % 3u] = f;
        EMB_CHECK(cache_flush(&cr));
    }
    EMB_CHECK(all_built);

    const embi_slug_stats st = embi_slug_get_stats(cr.s);
    /* Eviction ran, and the cache stays near the soft cap: the last frames' blobs are the only
     * ones that may not be evicted yet. */
    EMB_CHECK(st.evictions > 0);
    EMB_CHECK(st.live_bytes <= (uint64_t)EMBI_SLUG_SOFT_BYTES + (1u << 20));
    /* Without reuse the buffer would hold every blob ever built (about 58 MB). With reuse it stays
     * at the soft cap (measured: 16.78 MB of live blobs, no gap between them). */
    EMB_CHECK(st.hwm <= EMBI_SLUG_SOFT_BYTES + (1u << 20));
    EMB_CHECK(st.cap >= st.hwm);

    /* Every blob of the last three frames is intact on the GPU. */
    uint8_t *gpu = cache_read(&cr, st.hwm);
    EMB_REQUIRE(gpu != nullptr);
    for (uint32_t slot = 0; slot < 3u; slot++) {
        for (uint32_t j = 0; j < CHURN_PER_FRAME; j++) {
            const uint32_t id = ring_frame[slot] * CHURN_PER_FRAME + j;
            EMB_CHECK(blob_on_gpu(gpu, st.hwm, &ring[slot][j], id, CHURN_SIDES));
        }
    }
    SDL_free(gpu);

    /* A blob touched in the last frame is still a hit, at the same place. */
    const uint32_t last_id = ring_frame[(CHURN_FRAMES - 1u) % 3u] * CHURN_PER_FRAME;
    embi_slug_begin_frame(cr.s);
    embi_slug_entry hit;
    EMB_CHECK(embi_slug_get(cr.s, key_of(last_id), nullptr, 0, &hit));
    EMB_CHECK(hit.offset == ring[(CHURN_FRAMES - 1u) % 3u][0].offset);

    /* The first blob ever built was evicted long ago: asking for it again rebuilds it. */
    embi_qcurve c[CHURN_SIDES];
    make_blob(0, CHURN_SIDES, c);
    embi_slug_entry rebuilt;
    EMB_CHECK(embi_slug_get(cr.s, key_of(0), c, CHURN_SIDES, &rebuilt));
    EMB_CHECK(cache_flush(&cr));
    const embi_slug_stats st2 = embi_slug_get_stats(cr.s);
    gpu = cache_read(&cr, st2.hwm);
    EMB_REQUIRE(gpu != nullptr);
    EMB_CHECK(blob_on_gpu(gpu, st2.hwm, &rebuilt, 0, CHURN_SIDES));
    SDL_free(gpu);
    cache_close(&cr);
}

/* ------------------------------------------------------------------ full buffer */

EMB_TEST(slug_cache_buffer_full_is_bounded_and_recovers)
{
    cache_rig cr;
    if (!cache_open(&cr)) {
        cache_close(&cr);
        EMB_SKIP("no GPU device");
    }
    embi_qcurve *c = SDL_malloc(FULL_SIDES * sizeof *c);
    EMB_REQUIRE(c != nullptr);

    /* One frame touching every blob: none is evictable, so the buffer fills to its limit. */
    embi_slug_begin_frame(cr.s);
    uint32_t built = 0;
    uint32_t refused = 0;
    for (uint32_t id = 0; id < FULL_TRIES; id++) {
        make_blob(id, FULL_SIDES, c);
        embi_slug_entry e;
        if (embi_slug_get(cr.s, key_of(id), c, FULL_SIDES, &e)) {
            built++;
        } else {
            refused++;
        }
        if (id % FULL_FLUSH == FULL_FLUSH - 1u) {
            EMB_CHECK(cache_flush(&cr));
        }
    }
    EMB_CHECK(cache_flush(&cr));

    embi_slug_stats st = embi_slug_get_stats(cr.s);
    EMB_CHECK(refused > 0);
    EMB_CHECK(st.full_misses == refused);
    EMB_CHECK(built > 1000u);
    EMB_CHECK(st.hwm <= EMBI_SLUG_MAX_BYTES);
    EMB_CHECK(st.live_bytes <= EMBI_SLUG_MAX_BYTES);
    EMB_CHECK(st.hwm + MIN_BUFFER >= EMBI_SLUG_MAX_BYTES); /* it did fill up */

    /* Three frames later the first frame's blobs are evictable: a new blob builds. */
    for (uint32_t k = 0; k < EMBI_SLUG_QUARANTINE; k++) {
        embi_slug_begin_frame(cr.s);
    }
    make_blob(FULL_TRIES, FULL_SIDES, c);
    embi_slug_entry fresh;
    EMB_CHECK(embi_slug_get(cr.s, key_of(FULL_TRIES), c, FULL_SIDES, &fresh));
    EMB_CHECK(cache_flush(&cr));
    st = embi_slug_get_stats(cr.s);
    EMB_CHECK(st.live_bytes <= (uint64_t)EMBI_SLUG_SOFT_BYTES + (1u << 20));

    SDL_free(c);
    cache_close(&cr);
}

/* ------------------------------------------------------------------ drop */

EMB_TEST(slug_cache_drop_frees_the_blob_and_rebuilds_on_demand)
{
    cache_rig cr;
    if (!cache_open(&cr)) {
        cache_close(&cr);
        EMB_SKIP("no GPU device");
    }
    embi_slug_entry ents[3];
    embi_slug_begin_frame(cr.s);
    for (uint32_t i = 0; i < 3u; i++) {
        embi_qcurve c[16];
        make_blob(i, small_sides(i), c);
        EMB_CHECK(embi_slug_get(cr.s, key_of(i), c, small_sides(i), &ents[i]));
    }
    EMB_CHECK(cache_flush(&cr));
    const embi_slug_stats before = embi_slug_get_stats(cr.s);
    EMB_CHECK(before.blobs == 3u);

    embi_slug_drop(cr.s, key_of(1));
    const embi_slug_stats dropped = embi_slug_get_stats(cr.s);
    EMB_CHECK(dropped.blobs == 2u);
    EMB_CHECK(dropped.evictions == 1u);
    EMB_CHECK(dropped.live_bytes < before.live_bytes);
    embi_slug_drop(cr.s, key_of(99)); /* not cached: no effect */
    EMB_CHECK(embi_slug_get_stats(cr.s).evictions == 1u);

    embi_slug_begin_frame(cr.s);
    embi_qcurve c[16];
    make_blob(1, small_sides(1), c);
    EMB_CHECK(embi_slug_get(cr.s, key_of(1), c, small_sides(1), &ents[1]));
    EMB_CHECK(embi_slug_get_stats(cr.s).blobs == 3u);
    EMB_CHECK(cache_flush(&cr));

    const embi_slug_stats st = embi_slug_get_stats(cr.s);
    uint8_t *gpu = cache_read(&cr, st.hwm);
    EMB_REQUIRE(gpu != nullptr);
    for (uint32_t i = 0; i < 3u; i++) {
        EMB_CHECK(blob_on_gpu(gpu, st.hwm, &ents[i], i, small_sides(i)));
    }
    SDL_free(gpu);
    cache_close(&cr);
}

/* ------------------------------------------------------------------ renderer */

static void rect_curves(embi_qcurve *c, float x0, float y0, float x1, float y1)
{
    const float px[5] = {x0, x1, x1, x0, x0};
    const float py[5] = {y0, y0, y1, y1, y0};
    for (uint32_t k = 0; k < 4u; k++) {
        const embi_qcurve q = {px[k], py[k], 0.5f * (px[k] + px[k + 1u]),
                               0.5f * (py[k] + py[k + 1u]), px[k + 1u], py[k + 1u]};
        c[k] = q;
    }
}

static void scene_many_quads(embi_frame *f, void *user)
{
    (void)user;
    const embi_paint paint = embi_paint_solid(RGBA(255, 0, 0, 255));
    for (uint32_t i = 0; i < RENDER_QUADS; i++) {
        embi_qcurve c[4];
        rect_curves(c, 0.0f, 0.0f, 2.0f, 2.0f);
        const float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, (float)((i % 64u) * 8u) + 1.0f,
                            (float)((i / 64u) * 8u) + 1.0f};
        embi_frame_fill_quads(f, (uint64_t)i + 1u, c, 4, m, &paint, EMBI_FILL_NONZERO, 0, 0);
    }
}

EMB_TEST(slug_cache_render_after_growth_is_exact)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, 512, 512)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* The first frame builds 1500 blobs and grows the buffer past 64 KB in its upload. The second
     * frame is all hits. */
    slug_rig_frame(&rg, 1.0f, scene_many_quads, nullptr, true, RGBA(0, 0, 0, 0));
    slug_rig_frame(&rg, 1.0f, scene_many_quads, nullptr, true, RGBA(0, 0, 0, 0));
    uint8_t *px = slug_rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* 1500 squares of 2 x 2 px, all on pixel edges: 6000 units of full coverage. */
    EMB_CHECK(SDL_fabs(slug_alpha_sum(px, 512, 512) - 4.0 * RENDER_QUADS) < 0.5);
    EMB_CHECK_PIXEL(px, 512, 0, 0, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 512, 1, 1, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 512, 2, 2, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 512, 3, 1, 0, 0, 0, 0);
    const uint32_t last = RENDER_QUADS - 1u;
    const uint32_t lx = (last % 64u) * 8u + 1u;
    const uint32_t ly = (last / 64u) * 8u + 1u;
    EMB_CHECK_PIXEL(px, 512, lx + 1u, ly + 1u, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 512, lx + 2u, ly + 1u, 0, 0, 0, 0);
    SDL_free(px);
    slug_rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
