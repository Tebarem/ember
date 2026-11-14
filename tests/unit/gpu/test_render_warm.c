#include "emb_render_test.h"

#include "render_check.h"

#include "base/base.h"

/* The first-frame buffer warm-up (embi_renderer_warm): after it, a small frame creates no GPU buffer inside
 * embi_frame_end, and the frames still draw the right pixels (a full frame and a region frame, which needs the
 * replacing pipeline the warm-up makes). */

typedef struct rig {
    embi_gpu *g;
    embi_renderer *r;
    SDL_GPUDevice *dev;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *tb;
    uint32_t w;
    uint32_t h;
} rig;

static bool rig_open(rig *rg, uint32_t w, uint32_t h)
{
    SDL_memset(rg, 0, sizeof *rg);
    rg->w = w;
    rg->h = h;
    rg->g = embi_gpu_create(nullptr, false);
    if (rg->g == nullptr) {
        return false;
    }
    rg->dev = embi_gpu_device(rg->g);
    rg->r = embi_renderer_create(rg->g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    rg->tex = SDL_CreateGPUTexture(
        rg->dev, &(SDL_GPUTextureCreateInfo){
                     .type = SDL_GPU_TEXTURETYPE_2D,
                     .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                     .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                     .width = w,
                     .height = h,
                     .layer_count_or_depth = 1,
                     .num_levels = 1,
                     .sample_count = SDL_GPU_SAMPLECOUNT_1,
                 });
    rg->tb = SDL_CreateGPUTransferBuffer(
        rg->dev, &(SDL_GPUTransferBufferCreateInfo){
                     .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                     .size = w * h * 4u,
                 });
    return rg->r != nullptr && rg->tex != nullptr && rg->tb != nullptr;
}

static void rig_close(rig *rg)
{
    /* The renderer first: it joins the warm-up thread and waits for the GPU itself. */
    if (rg->r != nullptr) {
        embi_renderer_destroy(rg->r);
    }
    if (rg->dev != nullptr) {
        if (rg->tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(rg->dev, rg->tb);
        }
        if (rg->tex != nullptr) {
            SDL_ReleaseGPUTexture(rg->dev, rg->tex);
        }
    }
    if (rg->g != nullptr) {
        embi_gpu_destroy(rg->g);
    }
    SDL_memset(rg, 0, sizeof *rg);
}

static void frame_fill(rig *rg, bool region, uint32_t color, bool clear)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    embi_frame *f = region ? embi_frame_begin_region(rg->r, rg->w, rg->h, 1.0f, (embi_dev_rect){0, 0, 8, 8})
                           : embi_frame_begin(rg->r, rg->w, rg->h, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float rect[4] = {0, 0, 16, 16};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_fill(f, rect, radii, color, 0, 0);
    embi_frame_end(f, cmd, rg->tex, clear, 0);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
}

static uint8_t *rig_read(rig *rg)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(
        cp, &(SDL_GPUTextureRegion){.texture = rg->tex, .w = rg->w, .h = rg->h, .d = 1},
        &(SDL_GPUTextureTransferInfo){.transfer_buffer = rg->tb, .pixels_per_row = rg->w, .rows_per_layer = rg->h});
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
    uint8_t *out = nullptr;
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(rg->dev, rg->tb, false);
    if (mapped != nullptr) {
        out = SDL_malloc((size_t)rg->w * rg->h * 4u);
        if (out != nullptr) {
            SDL_memcpy(out, mapped, (size_t)rg->w * rg->h * 4u);
        }
        SDL_UnmapGPUTransferBuffer(rg->dev, rg->tb);
    }
    return out;
}

EMB_TEST(warm_first_frame_creates_no_buffers)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) == 0);
    embi_renderer_warm(rg.r);
    embi_renderer_warm_wait(rg.r);
    const uint32_t warmed = embi_renderer_buffer_creates(rg.r);
    EMB_CHECK(warmed >= 12); /* the storage buffers, the channel buffers, the upload buffer, the slug buffers */
    embi_renderer_warm(rg.r); /* a second call does nothing */
    embi_renderer_warm_wait(rg.r);
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) == warmed);
    frame_fill(&rg, false, RGBA(255, 0, 0, 255), true);
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) == warmed);
    frame_fill(&rg, false, RGBA(0, 255, 0, 255), true);
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) == warmed);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 4, 4, 0, 255, 0, 255);
    SDL_free(px);
    rig_close(&rg);
}

EMB_TEST(warm_unwarmed_first_frame_creates_the_buffers)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    frame_fill(&rg, false, RGBA(0, 0, 255, 255), true);
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) >= 10); /* what the warm-up moves out of the first frame */
    rig_close(&rg);
}

EMB_TEST(warm_region_frame_clears_with_the_replacing_pipeline)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_renderer_warm(rg.r); /* no wait: embi_frame_end joins the thread itself */
    frame_fill(&rg, false, RGBA(255, 0, 0, 255), true);
    /* A region frame over (0,0,8,8) that clears, then paints a blue 16x16 fill: the damage is blue, the rest red. */
    frame_fill(&rg, true, RGBA(0, 0, 255, 255), true);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 4, 4, 0, 0, 255, 255);
    EMB_CHECK_PIXEL(px, 16, 12, 12, 255, 0, 0, 255);
    SDL_free(px);
    rig_close(&rg);
}

EMB_TEST(warm_destroy_without_wait_is_safe)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_renderer_warm(rg.r);
    rig_close(&rg); /* destroys the renderer while the thread may still run */
}

/* A frame over the warm sizes (5000 prims) grows the prim buffer and still renders. */
EMB_TEST(warm_larger_frame_grows_and_renders)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_renderer_warm(rg.r);
    embi_renderer_warm_wait(rg.r);
    const uint32_t warmed = embi_renderer_buffer_creates(rg.r);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    embi_frame *f = embi_frame_begin(rg.r, 16, 16, 1.0f);
    EMB_REQUIRE(f != nullptr);
    const float radii[4] = {0, 0, 0, 0};
    for (int i = 0; i < 5000; i++) {
        const float rect[4] = {0, 0, 16, 16};
        embi_frame_fill(f, rect, radii, i == 4999 ? RGBA(0, 255, 0, 255) : RGBA(255, 0, 0, 255), 0, 0);
    }
    embi_frame_end(f, cmd, rg.tex, true, 0);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg.dev);
    EMB_CHECK(embi_renderer_buffer_creates(rg.r) > warmed);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 8, 8, 0, 255, 0, 255);
    SDL_free(px);
    rig_close(&rg);
}

/* Without the region clear pipeline the renderer says so (the caller then draws a full frame), the failure is
 * remembered, and a region frame that is recorded anyway is dropped without touching the target. */
EMB_TEST(warm_missing_clear_pipeline_is_reported_and_remembered)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    embi_renderer_warm(rg.r);
    EMB_CHECK(embi_renderer_region_ok(rg.r));
    embi_renderer_fault_clear_pipeline(rg.r);
    EMB_CHECK(!embi_renderer_region_ok(rg.r));
    EMB_CHECK(!embi_renderer_region_ok(rg.r));
    frame_fill(&rg, false, RGBA(255, 0, 0, 255), true); /* a full frame still works */
    frame_fill(&rg, true, RGBA(0, 0, 255, 255), true);  /* dropped */
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 4, 4, 255, 0, 0, 255);
    SDL_free(px);
    /* The caller's fallback: the same picture as a full frame. */
    frame_fill(&rg, false, RGBA(0, 0, 255, 255), true);
    px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 4, 4, 0, 0, 255, 255);
    SDL_free(px);
    rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
