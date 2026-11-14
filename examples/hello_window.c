/* Opens a window, claims it for a GPU device and draws the Slang triangle every frame until
 * the window is closed. Built in CI but not run.
 *
 * Desktop only: examples/CMakeLists.txt does not build this on Android or iOS, where SDL needs
 * its own entry point (SDL_main). */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "ember_shaders/triangle.h"
#include "gpu/gpu.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Ember triangle", 640, 480, SDL_WINDOW_RESIZABLE);
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (!window || !g) {
        SDL_Log("setup failed: %s", SDL_GetError());
        embi_gpu_destroy(g);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GPUDevice *dev = embi_gpu_device(g);
    int status = 0;
    bool claimed = false;
    SDL_GPUGraphicsPipeline *pipeline = nullptr;

    if (!SDL_ClaimWindowForGPUDevice(dev, window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        status = 1;
    } else {
        claimed = true;
        pipeline = embi_gpu_make_graphics(g, &embi_shader_triangle,
                                          SDL_GetGPUSwapchainTextureFormat(dev, window));
        if (!pipeline) {
            status = 1;
        }
    }

    bool quit = status != 0;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                quit = true;
            }
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) {
            break;
        }

        SDL_GPUTexture *swapchain = nullptr;
        Uint32 width = 0;
        Uint32 height = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &width, &height) &&
            swapchain) {
            const SDL_GPUColorTargetInfo color = {
                .texture = swapchain,
                .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    SDL_WaitForGPUIdle(dev);
    if (pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(dev, pipeline);
    }
    if (claimed) {
        SDL_ReleaseWindowFromGPUDevice(dev, window);
    }
    embi_gpu_destroy(g);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
