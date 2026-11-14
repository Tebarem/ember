#pragma once
/*
 * A renderer kept across frames, for tests that need the same path or the draw count of a frame.
 * Include after render_check.h. Not a test itself (the test glob only picks up test_*.c).
 */

#include "emb_render_test.h"

#include <SDL3/SDL_stdinc.h>

typedef struct slug_rig {
    embi_gpu *g;
    embi_renderer *r;
    SDL_GPUDevice *dev;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *tb;
    uint32_t w;
    uint32_t h;
} slug_rig;

static inline bool slug_rig_open(slug_rig *rg, uint32_t w, uint32_t h)
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

static inline void slug_rig_close(slug_rig *rg)
{
    if (rg->dev != nullptr) {
        SDL_WaitForGPUIdle(rg->dev);
        if (rg->tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(rg->dev, rg->tb);
        }
        if (rg->tex != nullptr) {
            SDL_ReleaseGPUTexture(rg->dev, rg->tex);
        }
    }
    if (rg->r != nullptr) {
        embi_renderer_destroy(rg->r);
    }
    if (rg->g != nullptr) {
        embi_gpu_destroy(rg->g);
    }
    SDL_memset(rg, 0, sizeof *rg);
}

/* Runs one frame into the rig's target. Returns the draw count of the frame. */
static inline uint32_t slug_rig_frame(slug_rig *rg, float scale, emb_test_scene_fn scene, void *user,
                               bool clear, uint32_t clear_rgba)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    embi_frame *f = embi_frame_begin(rg->r, rg->w, rg->h, scale);
    if (f != nullptr && scene != nullptr) {
        scene(f, user);
    }
    if (f != nullptr) {
        embi_frame_end(f, cmd, rg->tex, clear, clear_rgba);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);
    return embi_renderer_last_draw_calls(rg->r);
}

/* Reads the target into a new RGBA8 buffer (SDL_free it). */
static inline uint8_t *slug_rig_read(slug_rig *rg)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(
        cp,
        &(SDL_GPUTextureRegion){.texture = rg->tex, .w = rg->w, .h = rg->h, .d = 1},
        &(SDL_GPUTextureTransferInfo){
            .transfer_buffer = rg->tb,
            .pixels_per_row = rg->w,
            .rows_per_layer = rg->h,
        });
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg->dev);

    const size_t bytes = (size_t)rg->w * (size_t)rg->h * 4u;
    uint8_t *out = nullptr;
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(rg->dev, rg->tb, false);
    if (mapped != nullptr) {
        out = SDL_malloc(bytes);
        if (out != nullptr) {
            SDL_memcpy(out, mapped, bytes);
        }
        SDL_UnmapGPUTransferBuffer(rg->dev, rg->tb);
    }
    return out;
}

/* Sum of the alpha channel over the image, in units of full coverage (1.0 per opaque pixel). */
static inline double slug_alpha_sum(const uint8_t *px, uint32_t w, uint32_t h)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < w * h; i++) {
        total += px[i * 4u + 3u];
    }
    return (double)total / 255.0;
}
