#include "emb_render_test.h"

#include "render_check.h"
#include "slug_rig.h"

#include <SDL3/SDL_stdinc.h>

/* Frames that share a command buffer, and command buffers in flight. A page drawn with layers records a frame per layer into
 * one command buffer; the renderer writes every frame's arrays after the last frame's in its upload buffer (a ring that
 * cycles only when it is full) and uploads into its storage buffers without cycling them (SDL orders the copy after the
 * draws that read them). Each target must show what its own last frame drew, whatever came before or is still running. */

#define W 16
#define H 16
#define TARGETS 4
#define FRAMES 48     /* per command buffer, round-robin over the targets */
#define CMDS 3        /* submitted one after the other without waiting */
#define PRIMS 700     /* per frame: 45 KB of prims, so the ring fills and cycles inside a command buffer */

static SDL_GPUTexture *make_target(SDL_GPUDevice *dev)
{
    return SDL_CreateGPUTexture(dev, &(SDL_GPUTextureCreateInfo){
                                         .type = SDL_GPU_TEXTURETYPE_2D,
                                         .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                         .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                         .width = W,
                                         .height = H,
                                         .layer_count_or_depth = 1,
                                         .num_levels = 1,
                                         .sample_count = SDL_GPU_SAMPLECOUNT_1,
                                     });
}

/* The color of frame k: distinct for every frame of the test. */
static uint32_t frame_color(uint32_t k)
{
    return RGBA((k * 37u) & 255u, (k * 91u + 40u) & 255u, (k / 3u * 53u + 7u) & 255u, 255);
}

/* Frame k: PRIMS fills; the last one, over the whole target, has the frame's color and the rest are a decoy color, so a
 * frame that read another frame's prims (or a stale part of the ring) shows the wrong color. A second rect in the corner
 * comes from the frame's own transform table, which also goes through the ring. */
static void record_frame(embi_renderer *r, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t k)
{
    embi_frame *f = embi_frame_begin(r, W, H, 1.0f);
    if (f == nullptr) {
        return;
    }
    const float radii[4] = {0, 0, 0, 0};
    const float decoy[4] = {0, 0, W, H};
    for (uint32_t i = 0; i + 1 < PRIMS; i++) {
        embi_frame_fill(f, decoy, radii, RGBA(255, 0, 255, 255), 0, 0);
    }
    const float full[4] = {0, 0, W, H};
    embi_frame_fill(f, full, radii, frame_color(k), 0, 0);
    const float m[6] = {1, 0, 0, 1, (float)(k % 5u), 0};
    const uint32_t xf = embi_frame_xform(f, m);
    const float corner[4] = {0, 0, 2, 2};
    embi_frame_fill(f, corner, radii, frame_color(k + 1000u), xf, 0);
    embi_frame_end(f, cmd, tex, true, 0);
}

static uint8_t *read_target(SDL_GPUDevice *dev, SDL_GPUTexture *tex, SDL_GPUTransferBuffer *tb)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(cp, &(SDL_GPUTextureRegion){.texture = tex, .w = W, .h = H, .d = 1},
                               &(SDL_GPUTextureTransferInfo){.transfer_buffer = tb, .pixels_per_row = W, .rows_per_layer = H});
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(dev);
    uint8_t *out = SDL_malloc((size_t)W * H * 4u);
    const uint8_t *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (out != nullptr && mapped != nullptr) {
        SDL_memcpy(out, mapped, (size_t)W * H * 4u);
    }
    if (mapped != nullptr) {
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    return out;
}

EMB_TEST(render_frames_sharing_command_buffers_in_flight_draw_their_own_data)
{
    slug_rig rg;
    if (!slug_rig_open(&rg, W, H)) {
        slug_rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    SDL_GPUTexture *tex[TARGETS] = {nullptr};
    bool ok = true;
    for (int t = 0; t < TARGETS; t++) {
        tex[t] = make_target(rg.dev);
        ok = ok && tex[t] != nullptr;
    }
    EMB_REQUIRE(ok);
    uint32_t last[TARGETS] = {0};
    uint32_t k = 0;
    for (int c = 0; c < CMDS; c++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
        EMB_REQUIRE(cmd != nullptr);
        for (int i = 0; i < FRAMES; i++, k++) {
            const uint32_t t = k % TARGETS;
            record_frame(rg.r, cmd, tex[t], k);
            last[t] = k;
        }
        SDL_SubmitGPUCommandBuffer(cmd); /* not waited for: the next one records while this one may run */
    }
    SDL_WaitForGPUIdle(rg.dev);
    for (int t = 0; t < TARGETS; t++) {
        uint8_t *px = read_target(rg.dev, tex[t], rg.tb);
        EMB_REQUIRE(px != nullptr);
        const uint32_t want = frame_color(last[t]);
        const uint32_t corner = frame_color(last[t] + 1000u);
        const uint32_t cx = last[t] % 5u;
        EMB_CHECK_PIXEL(px, W, 10, 10, want & 255u, (want >> 8) & 255u, (want >> 16) & 255u, 255);
        EMB_CHECK_PIXEL(px, W, cx, 1, corner & 255u, (corner >> 8) & 255u, (corner >> 16) & 255u, 255);
        SDL_free(px);
    }
    for (int t = 0; t < TARGETS; t++) {
        SDL_ReleaseGPUTexture(rg.dev, tex[t]);
    }
    slug_rig_close(&rg);
}

EMB_RENDER_TEST_MAIN()
