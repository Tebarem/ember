#pragma once
/*
 * Offscreen render helper for scene tests. Include after emb_gold.h.
 *
 *     static void scene_box(embi_frame *f, void *user) { ... embi_frame_fill(...); }
 *     ...
 *     uint8_t *px = emb_test_render(64, 64, scene_box, nullptr);
 *     EMB_RENDER_OR_SKIP(px);
 *     EMB_CHECK(emb_gold_check("box_fill", px, 64, 64));
 *     SDL_free(px);
 *
 * emb_test_render creates a GPU device and a renderer, runs the scene into a cleared RGBA8
 * target, reads the target back and returns the pixels (SDL_free them). Without a usable GPU
 * device it marks the test as skipped (exit code 77 through ctest) instead of failing.
 *
 * Only embi_gpu (src/gpu/gpu.h) is needed for this header to compile. The renderer part is
 * guarded by __has_include("gpu/render.h"), so it appears once R1a adds that header.
 */

#include "emb_test.h"

#include <stdint.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include "gpu/gpu.h"

/* Passes control to the test function: when px is nullptr and the test was marked as skipped,
 * returns; otherwise a missing result is a failure. Use only inside an EMB_TEST body. */
#define EMB_RENDER_OR_SKIP(px)                                                                 \
    do {                                                                                       \
        if ((px) == nullptr && emb_test_cur_skip != nullptr) {                                 \
            return;                                                                            \
        }                                                                                      \
        EMB_REQUIRE((px) != nullptr);                                                          \
    } while (0)

#if __has_include("gpu/render.h")
#include "gpu/render.h"

/* Records prims into the frame. The frame is valid only for the duration of the call. */
typedef void (*emb_test_scene_fn)(embi_frame *f, void *user);

/* Renders scene at device scale 1 into a width x height target cleared to transparent black.
 * Returns tightly packed RGBA8 pixels (SDL_free them), or nullptr. nullptr with
 * emb_test_cur_skip set means there is no usable GPU device. */
static inline uint8_t *emb_test_render(uint32_t width, uint32_t height, emb_test_scene_fn scene,
                                       void *user)
{
    if (width == 0 || height == 0 || scene == nullptr) {
        return nullptr;
    }

    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return nullptr;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    embi_renderer *r = embi_renderer_create(g, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if (r == nullptr) {
        embi_gpu_destroy(g);
        return nullptr;
    }

    const size_t bytes = (size_t)width * (size_t)height * 4u;
    uint8_t *out = nullptr;

    SDL_GPUTextureCreateInfo tci = {0};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tci.width = width;
    tci.height = height;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &tci);

    SDL_GPUTransferBufferCreateInfo tbci = {0};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = tex ? SDL_CreateGPUTransferBuffer(dev, &tbci) : nullptr;

    SDL_GPUCommandBuffer *cmd = (tb != nullptr) ? SDL_AcquireGPUCommandBuffer(dev) : nullptr;
    if (cmd != nullptr) {
        embi_frame *f = embi_frame_begin(r, width, height, 1.0f);
        scene(f, user);
        embi_frame_end(f, cmd, tex, true, 0);

        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion region = {0};
        region.texture = tex;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_GPUTextureTransferInfo info = {0};
        info.transfer_buffer = tb;
        info.pixels_per_row = width;
        info.rows_per_layer = height;
        SDL_DownloadFromGPUTexture(cp, &region, &info);
        SDL_EndGPUCopyPass(cp);

        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence != nullptr) {
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
            if (mapped != nullptr) {
                out = SDL_malloc(bytes);
                if (out != nullptr) {
                    SDL_memcpy(out, mapped, bytes);
                }
                SDL_UnmapGPUTransferBuffer(dev, tb);
            }
        }
    }

    if (tb != nullptr) {
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    if (tex != nullptr) {
        SDL_ReleaseGPUTexture(dev, tex);
    }
    embi_renderer_destroy(r);
    embi_gpu_destroy(g);
    return out;
}
#endif
