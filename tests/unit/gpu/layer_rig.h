#pragma once
/*
 * Shared rig for the layer and partial-redraw tests (test_layer_*.c). Not a test itself: the test
 * glob picks up test_*.c only. A rig owns a device, a renderer and a layer pool, and records
 * several frames into one command buffer, which is how a real frame sequence runs.
 *
 *     layer_rig k;
 *     if (!layer_rig_open(&k)) { EMB_SKIP("no GPU device"); return; }
 *     SDL_GPUTexture *t = layer_rig_target(&k, 64, 64);
 *     SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(k.dev);
 *     ... embi_frame_begin / embi_frame_end (cmd, t, ...) ...
 *     layer_dl d = layer_rig_download(&k, cmd, t, 64, 64);
 *     layer_rig_submit(&k, cmd);
 *     uint8_t *px = layer_rig_map(&k, d);      // SDL_free it
 *     layer_rig_close(&k);
 */

#include "emb_render_test.h"

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_stdinc.h>

#include "gpu/layer.h"

#include "render_check.h"

typedef struct layer_rig {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    embi_renderer *r;
    embi_layer_pool *pool;
} layer_rig;

typedef struct layer_dl {
    SDL_GPUTransferBuffer *tb;
    uint32_t bytes;
} layer_dl;

static inline void layer_rig_close(layer_rig *k)
{
    embi_layer_pool_destroy(k->pool);
    embi_renderer_destroy(k->r);
    embi_gpu_destroy(k->g);
    *k = (layer_rig){0};
}

/* Returns false when there is no usable GPU (the caller skips). */
static inline bool layer_rig_open(layer_rig *k)
{
    *k = (layer_rig){0};
    k->g = embi_gpu_create(nullptr, false);
    if (k->g == nullptr) {
        return false;
    }
    k->dev = embi_gpu_device(k->g);
    k->r = embi_renderer_create(k->g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    k->pool = embi_layer_pool_create(k->g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 64u << 20);
    if (k->r == nullptr || k->pool == nullptr) {
        layer_rig_close(k);
        return false;
    }
    return true;
}

/* A color target that can be rendered to and read back (as embi_frame_end wants). */
static inline SDL_GPUTexture *layer_rig_target(layer_rig *k, uint32_t w, uint32_t h)
{
    return SDL_CreateGPUTexture(
        k->dev, &(SDL_GPUTextureCreateInfo){
                    .type = SDL_GPU_TEXTURETYPE_2D,
                    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                    .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                    .width = w,
                    .height = h,
                    .layer_count_or_depth = 1,
                    .num_levels = 1,
                    .sample_count = SDL_GPU_SAMPLECOUNT_1,
                });
}

/* Records a readback of the top-left w x h of tex into cmd. The data is valid after the command
 * buffer has been submitted and waited for (layer_rig_submit, layer_rig_map). */
static inline layer_dl layer_rig_download(layer_rig *k, SDL_GPUCommandBuffer *cmd,
                                          SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    layer_dl d = {0};
    d.bytes = w * h * 4u;
    d.tb = SDL_CreateGPUTransferBuffer(
        k->dev, &(SDL_GPUTransferBufferCreateInfo){
                    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                    .size = d.bytes,
                });
    if (d.tb == nullptr) {
        return d;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTextureRegion region = {.texture = tex, .w = w, .h = h, .d = 1};
    const SDL_GPUTextureTransferInfo info = {
        .transfer_buffer = d.tb,
        .pixels_per_row = w,
        .rows_per_layer = h,
    };
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    return d;
}

/* Submits cmd and waits until the GPU is done with it. */
static inline bool layer_rig_submit(layer_rig *k, SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        return false;
    }
    SDL_WaitForGPUFences(k->dev, true, &fence, 1);
    SDL_ReleaseGPUFence(k->dev, fence);
    return true;
}

/* Copies a finished download out (SDL_free the result) and releases its buffer. */
static inline uint8_t *layer_rig_map(layer_rig *k, layer_dl d)
{
    uint8_t *out = nullptr;
    if (d.tb == nullptr) {
        return nullptr;
    }
    void *m = SDL_MapGPUTransferBuffer(k->dev, d.tb, false);
    if (m != nullptr) {
        out = SDL_malloc(d.bytes);
        if (out != nullptr) {
            SDL_memcpy(out, m, d.bytes);
        }
        SDL_UnmapGPUTransferBuffer(k->dev, d.tb);
    }
    SDL_ReleaseGPUTransferBuffer(k->dev, d.tb);
    return out;
}

/* Fills the whole target with one color (non-premultiplied), in its own frame. */
static inline void layer_rig_clear(layer_rig *k, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex,
                                   uint32_t w, uint32_t h, uint32_t rgba)
{
    embi_frame *f = embi_frame_begin(k->r, w, h, 1.0f);
    embi_frame_end(f, cmd, tex, true, rgba);
}
