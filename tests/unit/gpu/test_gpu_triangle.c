#include "emb_test.h"

#include <SDL3/SDL.h>

#include "ember_shaders/formats.h"
#include "ember_shaders/triangle.h"
#include "gpu/gpu.h"

#define TARGET_W 64u
#define TARGET_H 64u
#define TARGET_BYTES (TARGET_W * TARGET_H * 4u)

/* Counts SDL GPU errors (validation and resource problems) while a case runs. Every message
 * is still forwarded to the previous log function. */
static SDL_AtomicInt gpu_error_count; /* atomic: SDL backends log from their own threads */
static SDL_LogOutputFunction prev_log_fn;
static void *prev_log_userdata;

static void SDLCALL count_gpu_errors(void *userdata, int category, SDL_LogPriority priority,
                                     const char *message)
{
    (void)userdata;
    if (category == SDL_LOG_CATEGORY_GPU && priority >= SDL_LOG_PRIORITY_ERROR) {
        SDL_AddAtomicInt(&gpu_error_count, 1);
    }
    prev_log_fn(prev_log_userdata, category, priority, message);
}

static void gpu_log_capture_begin(void)
{
    SDL_GetLogOutputFunction(&prev_log_fn, &prev_log_userdata);
    SDL_SetAtomicInt(&gpu_error_count, 0);
    SDL_SetLogOutputFunction(count_gpu_errors, nullptr);
}

static void gpu_log_capture_end(void)
{
    SDL_SetLogOutputFunction(prev_log_fn, prev_log_userdata);
}

static uint8_t pixel_channel(const uint8_t *img, unsigned x, unsigned y, unsigned c)
{
    return img[(y * TARGET_W + x) * 4u + c];
}

/* Renders the triangle over a blue clear into a 64x64 target, reads it back into img and
 * releases every resource. Sets *skipped when no device can be created with the driver. */
static void render_triangle_case(const char *driver, uint8_t *img, bool *skipped)
{
    *skipped = false;
    gpu_log_capture_begin();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        *skipped = true;
        gpu_log_capture_end();
        return;
    }

    if (driver) {
        SDL_SetHint(SDL_HINT_GPU_DRIVER, driver);
    } else {
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
    }

    embi_gpu *g = embi_gpu_create(nullptr, true);
    SDL_ResetHint(SDL_HINT_GPU_DRIVER);
    if (!g) {
        *skipped = true;
        SDL_Quit();
        gpu_log_capture_end();
        return;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    SDL_Log("triangle: driver %s, shader format 0x%x", SDL_GetGPUDeviceDriver(dev),
            (unsigned)embi_gpu_format(g));

    SDL_GPUGraphicsPipeline *pipeline =
        embi_gpu_make_graphics(g, &embi_shader_triangle, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    EMB_CHECK(pipeline != nullptr);

    SDL_GPUTexture *target = SDL_CreateGPUTexture(
        dev, &(SDL_GPUTextureCreateInfo){
                 .type = SDL_GPU_TEXTURETYPE_2D,
                 .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                 .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                 .width = TARGET_W,
                 .height = TARGET_H,
                 .layer_count_or_depth = 1,
                 .num_levels = 1,
                 .sample_count = SDL_GPU_SAMPLECOUNT_1,
             });
    EMB_CHECK(target != nullptr);

    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(
        dev, &(SDL_GPUTransferBufferCreateInfo){
                 .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                 .size = TARGET_BYTES,
             });
    EMB_CHECK(download != nullptr);

    if (pipeline && target && download) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        EMB_CHECK(cmd != nullptr);
        if (cmd) {
            const SDL_GPUColorTargetInfo color = {
                .texture = target,
                .clear_color = {0.0f, 0.0f, 1.0f, 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);

            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_DownloadFromGPUTexture(
                copy,
                &(SDL_GPUTextureRegion){.texture = target, .w = TARGET_W, .h = TARGET_H, .d = 1},
                &(SDL_GPUTextureTransferInfo){
                    .transfer_buffer = download,
                    .pixels_per_row = TARGET_W,
                    .rows_per_layer = TARGET_H,
                });
            SDL_EndGPUCopyPass(copy);

            EMB_CHECK(SDL_SubmitGPUCommandBuffer(cmd));
            EMB_CHECK(SDL_WaitForGPUIdle(dev));

            const uint8_t *mapped = SDL_MapGPUTransferBuffer(dev, download, false);
            EMB_CHECK(mapped != nullptr);
            if (mapped) {
                SDL_memcpy(img, mapped, TARGET_BYTES);
                SDL_UnmapGPUTransferBuffer(dev, download);

                /* (4,4) lies in the triangle, (60,60) does not. */
                EMB_CHECK_EQ_INT(pixel_channel(img, 4, 4, 0), 255);
                EMB_CHECK_EQ_INT(pixel_channel(img, 4, 4, 1), 0);
                EMB_CHECK_EQ_INT(pixel_channel(img, 4, 4, 2), 0);
                EMB_CHECK_EQ_INT(pixel_channel(img, 4, 4, 3), 255);
                EMB_CHECK_EQ_INT(pixel_channel(img, 60, 60, 0), 0);
                EMB_CHECK_EQ_INT(pixel_channel(img, 60, 60, 1), 0);
                EMB_CHECK_EQ_INT(pixel_channel(img, 60, 60, 2), 255);
                EMB_CHECK_EQ_INT(pixel_channel(img, 60, 60, 3), 255);
            }
        }
    }

    SDL_ReleaseGPUTransferBuffer(dev, download);
    SDL_ReleaseGPUTexture(dev, target);
    SDL_ReleaseGPUGraphicsPipeline(dev, pipeline);
    embi_gpu_destroy(g);
    SDL_Quit();

    EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&gpu_error_count), 0);
    gpu_log_capture_end();
}

/* The default driver is whatever SDL picks for the formats A1 built (D3D12 for DXIL on Windows,
 * Vulkan when only SPIR-V is built), so the test does not force one. */
EMB_TEST(triangle_renders_on_default_driver)
{
    uint8_t img[TARGET_BYTES];
    bool skipped = false;
    render_triangle_case(nullptr, img, &skipped);
    if (skipped) {
        EMB_SKIP("no GPU device for the default driver");
    }
}

EMB_TEST(triangle_renders_on_vulkan)
{
    uint8_t img[TARGET_BYTES];
    bool skipped = false;
    render_triangle_case("vulkan", img, &skipped);
    if (skipped) {
        EMB_SKIP("no Vulkan driver");
    }
}

EMB_TEST(external_device_is_not_destroyed)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    /* The host asks for the same formats embi_gpu_create would (the set A1 built). */
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(EMBI_BUILT_SHADER_FORMATS, false, nullptr);
    if (!dev) {
        SDL_Quit();
        EMB_SKIP("no GPU device");
    }

    embi_gpu *g = embi_gpu_create(dev, false);
    EMB_CHECK(g != nullptr);
    if (g) {
        EMB_CHECK(embi_gpu_device(g) == dev);
        EMB_CHECK(embi_gpu_format(g) != SDL_GPU_SHADERFORMAT_INVALID);
        embi_gpu_destroy(g);
    }

    /* The wrapper must not have destroyed the host's device. */
    EMB_CHECK(SDL_GetGPUDeviceDriver(dev) != nullptr);
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
}

EMB_TEST_MAIN()
