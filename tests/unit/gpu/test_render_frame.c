#include "emb_render_test.h"

#include "render_check.h"

#include "base/base.h"

/* Frame-level behavior that needs a renderer kept across frames: device scale, argument checks,
 * clear and load, the draw-call budget at 50k prims, and steady-state CPU allocations. Those
 * need a renderer that outlives one frame, so this file has its own small rig. */

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
static uint32_t rig_frame(rig *rg, float scale, emb_test_scene_fn scene, void *user, bool clear,
                          uint32_t clear_rgba)
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
static uint8_t *rig_read(rig *rg)
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

/* ------------------------------------------------------------------ scenes */

static void scene_scaled_rect(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {10, 10, 20, 20};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(frame_device_scale_scales_geometry)
{
    rig rg;
    if (!rig_open(&rg, 128, 128)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    /* A 2x device scale turns the CSS rect (10, 10, 20, 20) into device (20..60). */
    rig_frame(&rg, 2.0f, scene_scaled_rect, nullptr, true, 0);
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 128, 30, 30, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 128, 59, 59, 255, 0, 0, 255);
    EMB_CHECK_PIXEL(px, 128, 15, 15, 0, 0, 0, 0);
    EMB_CHECK_PIXEL(px, 128, 62, 62, 0, 0, 0, 0);
    SDL_free(px);
    rig_close(&rg);
}

static void scene_nothing(embi_frame *f, void *user)
{
    (void)user;
    (void)f;
}

static void scene_fill_red_full(embi_frame *f, void *user)
{
    (void)user;
    const float rect[4] = {0, 0, 16, 16};
    const float radii[4] = {0, 0, 0, 0};
    embi_frame_fill(f, rect, radii, RGBA(255, 0, 0, 255), 0, 0);
}

EMB_TEST(frame_clear_premultiplies_and_load_keeps_contents)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }

    /* A half-transparent blue clear is stored premultiplied: (0, 0, 128, 128). */
    rig_frame(&rg, 1.0f, scene_nothing, nullptr, true, RGBA(0, 0, 255, 128));
    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    CHECK_PX_NEAR(px, 16, 3, 3, 0, 0, 128, 128, 1);
    SDL_free(px);

    /* Opaque red over it, then a frame that loads and draws nothing: red must survive. */
    rig_frame(&rg, 1.0f, scene_fill_red_full, nullptr, false, 0);
    rig_frame(&rg, 1.0f, scene_nothing, nullptr, false, 0);
    px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_PIXEL(px, 16, 3, 3, 255, 0, 0, 255);
    SDL_free(px);
    rig_close(&rg);
}

static void scene_grid(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < 50000u; i++) {
        const float rect[4] = {(float)((i % 256u) * 2u), (float)((i / 256u) * 2u), 2, 2};
        const uint32_t color = RGBA(i & 255u, 128, 0, 255);
        embi_frame_fill(f, rect, zero, color, 0, 0);
    }
}

EMB_TEST(frame_50k_prims_take_at_most_four_draws)
{
    rig rg;
    if (!rig_open(&rg, 512, 512)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    const uint32_t draws = rig_frame(&rg, 1.0f, scene_grid, nullptr, true, 0);
    EMB_CHECK(draws >= 1);
    EMB_CHECK(draws <= 4);

    uint8_t *px = rig_read(&rg);
    EMB_REQUIRE(px != nullptr);
    /* Prim 1000 is at column 232, row 3: rect (464, 6, 2, 2), color (232, 128, 0). */
    EMB_CHECK_PIXEL(px, 512, 464, 6, 232, 128, 0, 255);
    EMB_CHECK_PIXEL(px, 512, 465, 7, 232, 128, 0, 255);
    /* Last prim 49999: column 79, row 195, rect (158, 390, 2, 2), color (79, 128, 0). */
    EMB_CHECK_PIXEL(px, 512, 158, 390, 79, 128, 0, 255);
    /* Past the last prim (index 50175 would be next) nothing is drawn. */
    EMB_CHECK_PIXEL(px, 512, 511, 391, 0, 0, 0, 0);
    SDL_free(px);
    rig_close(&rg);
}

static void scene_steady(embi_frame *f, void *user)
{
    (void)user;
    const float zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < 1000u; i++) {
        const float rect[4] = {(float)((i % 32u) * 8u), (float)((i / 32u) * 8u), 6, 6};
        embi_frame_fill(f, rect, zero, RGBA(i & 255u, 64, 128, 255), 0, 0);
    }
}

EMB_TEST(frame_steady_state_makes_no_cpu_allocations)
{
    const size_t before_open = embi_mem_live();
    {
        rig rg;
        if (!rig_open(&rg, 256, 256)) {
            rig_close(&rg);
            EMB_SKIP("no GPU device");
        }
        /* Warm-up frames grow the CPU arrays and the GPU buffers to their final sizes. */
        rig_frame(&rg, 1.0f, scene_steady, nullptr, true, 0);
        rig_frame(&rg, 1.0f, scene_steady, nullptr, true, 0);
        const size_t steady = embi_mem_live();
        for (int i = 0; i < 100; i++) {
            rig_frame(&rg, 1.0f, scene_steady, nullptr, true, 0);
        }
        EMB_CHECK_EQ_INT(embi_mem_live(), (long long)steady);
        rig_close(&rg);
    }
    /* Everything the renderer allocated has been freed again. */
    EMB_CHECK_EQ_INT(embi_mem_live(), (long long)before_open);
}

static void scene_empty(embi_frame *f, void *user)
{
    (void)user;
    (void)f;
}

EMB_TEST(frame_begin_rejects_empty_size_and_bad_scale)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    EMB_CHECK(embi_frame_begin(rg.r, 0, 16, 1.0f) == nullptr);
    EMB_CHECK(embi_frame_begin(rg.r, 16, 0, 1.0f) == nullptr);
    EMB_CHECK(embi_frame_begin(rg.r, 16, 16, 0.0f) == nullptr);
    EMB_CHECK(embi_frame_begin(rg.r, 16, 16, __builtin_nanf("")) == nullptr);
    /* A valid begin still works after the rejections. */
    rig_frame(&rg, 1.0f, scene_empty, nullptr, true, 0);
    EMB_CHECK_EQ_INT(embi_renderer_last_draw_calls(rg.r), 0);
    rig_close(&rg);
}

/* Transform dedup: consecutive equal matrices share one index, and the identity is index 0.
 * This runs a frame with no prims, and all checks happen outside any scene callback. */
EMB_TEST(frame_xform_dedup_shares_consecutive_indices)
{
    rig rg;
    if (!rig_open(&rg, 16, 16)) {
        rig_close(&rg);
        EMB_SKIP("no GPU device");
    }
    const float m[6] = {2, 0, 0, 2, 3, 4};
    const float id[6] = {1, 0, 0, 1, 0, 0};
    const float other[6] = {1, 0, 0, 1, 5, 0};

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(rg.dev);
    embi_frame *f = embi_frame_begin(rg.r, 16, 16, 1.0f);
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t identity = 1;
    uint32_t third = 0;
    if (f != nullptr) {
        first = embi_frame_xform(f, m);
        second = embi_frame_xform(f, m);
        identity = embi_frame_xform(f, id);
        third = embi_frame_xform(f, other);
        embi_frame_end(f, cmd, rg.tex, true, 0);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(rg.dev);
    rig_close(&rg);

    EMB_CHECK(f != nullptr);
    EMB_CHECK(first != 0);
    EMB_CHECK(first == second);
    EMB_CHECK(identity == 0);
    EMB_CHECK(third != first);
}

EMB_RENDER_TEST_MAIN()
