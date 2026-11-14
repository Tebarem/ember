/* An offscreen view in a host-owned window.
 *
 *   offscreen [--frames N] [PATH_OR_URL]
 *
 * The host creates the window and the SDL_GPUDevice and claims the window for it. Each frame it calls emb_view_render on
 * its own command buffer, which records the page into the view's texture only when the picture changed. Then the host
 * draws a spinning quad on the right of the window with its own render pass, and blits the view's texture on the left
 * with SDL_BlitGPUTexture. The keys and the mouse wheel reach the view through the host, as a host forwards input.
 * Without a PATH_OR_URL the built-in demo page is shown. --frames N ends the run after N frames (for smoke runs).
 *
 * The spinning quad is the only drawing that is not Ember's: its shader is shaders/host_spin.slang, compiled at build
 * time into ember_shaders/host_spin.h (see examples/CMakeLists.txt). Desktop only: it uses a plain main().
 */

#define SDL_MAIN_HANDLED
#include <ember/ember.h>

#include "ember_shaders/host_spin.h"
#include "gpu/gpu.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define WIN_W 1100u
#define WIN_H 620u
#define VIEW_X 16u /* the view's top-left corner in the window, device px */
#define VIEW_Y 16u
#define VIEW_W 640u /* the view's size in CSS px, which is its device size at scale 1 */
#define VIEW_H 588u
#define WHEEL_PX 53.0f
#define KEY_PX 48.0f

static const char k_demo_page[] =
    "<!doctype html><title>Ember offscreen demo</title><style>"
    "body{margin:0;font:16px sans-serif;background:#fbfbfd;color:#222}"
    "h1{margin:16px 20px 4px;font-size:26px;color:#1a237e}"
    "p{margin:4px 20px 14px;color:#555}"
    ".row{display:flex;gap:12px;margin:0 20px 12px}"
    ".card{width:140px;height:96px;border-radius:10px;padding:10px;box-sizing:border-box;color:#fff;background:#3f51b5}"
    ".card.b{background:#00897b}.card.c{background:#e64a19}.card.d{background:#6a1b9a;opacity:.6}"
    ".wide{margin:0 20px 12px;height:40px;background:linear-gradient(90deg,#ffb300,#e53935);border-radius:6px}"
    ".tall{margin:0 20px;height:900px;background:repeating-linear-gradient(0deg,#eceff1 0 23px,#cfd8dc 23px 24px)}"
    "</style><h1>Rendered by Ember into a texture</h1>"
    "<p>The host blits this view and draws the spinning quad itself. Scroll with the wheel or the arrow keys.</p>"
    "<div class=row><div class=card>One</div><div class='card b'>Two</div><div class='card c'>Three</div>"
    "<div class='card d'>Four</div></div><div class=wide></div><div class=tall></div>";

static void print_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    static const char *const names[] = {"debug", "info", "warn", "error"};
    int idx = (int)level;
    if (idx < 0 || idx > (int)EMB_LOG_ERROR) {
        idx = (int)EMB_LOG_ERROR;
    }
    fprintf(stderr, "[ember %s] %s\n", names[idx], message);
}

/* The file path of a file: URL or a bare path (relative paths are resolved against the current directory), with
 * forward slashes. False for other URLs, which need no directory. */
static bool file_path_of(const char *target, char *out, size_t cap)
{
    const char *p = target;
    if (SDL_strncasecmp(p, "file:", 5) == 0) {
        p += 5;
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') {
                p++;
            }
        }
        size_t n = 0;
        for (; *p != '\0' && n + 1 < cap; p++) {
            if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hex[3] = {p[1], p[2], '\0'};
                out[n++] = (char)strtol(hex, nullptr, 16);
                p += 2;
            } else {
                out[n++] = *p == '\\' ? '/' : *p;
            }
        }
        out[n] = '\0';
        return n > 0;
    }
    if (strstr(p, "://") != nullptr) {
        return false;
    }
    bool absolute = p[0] == '/' || p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':');
    char *cwd = absolute ? nullptr : SDL_GetCurrentDirectory();
    size_t cl = cwd != nullptr ? SDL_strlen(cwd) : 0;
    while (cl > 0 && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) {
        cl--;
    }
    int n = cwd != nullptr ? SDL_snprintf(out, cap, "%.*s/%s", (int)cl, cwd, p) : SDL_snprintf(out, cap, "%s", p);
    SDL_free(cwd);
    for (char *c = out; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    return n > 0 && (size_t)n < cap;
}

/* Makes the directory of a page readable, so its images and stylesheets load. */
static void allow_dir_of(emb_runtime *rt, const char *target)
{
    char path[2048];
    if (!file_path_of(target, path, sizeof path)) {
        return;
    }
    char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        return;
    }
    if (slash == path) {
        emb_runtime_allow_path(rt, "/");
        return;
    }
    *slash = '\0';
    emb_runtime_allow_path(rt, path);
}

/* A shader of the program's stage in the first format the device takes. nullptr when there is none. */
static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const embi_shader_stage *st, SDL_GPUShaderStage stage)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev);
    const embi_shader_blob *blob = nullptr;
    SDL_GPUShaderFormat fmt = 0;
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0 && st->dxil.code != nullptr) {
        blob = &st->dxil;
        fmt = SDL_GPU_SHADERFORMAT_DXIL;
    } else if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0 && st->spirv.code != nullptr) {
        blob = &st->spirv;
        fmt = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0 && st->msl.code != nullptr) {
        blob = &st->msl;
        fmt = SDL_GPU_SHADERFORMAT_MSL;
    }
    if (blob == nullptr) {
        return nullptr;
    }
    embi_gpu_blob_view bv = embi_gpu_blob_open(blob); /* the code, expanded when the build packs its blobs */
    if (bv.code == nullptr) {
        return nullptr;
    }
    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.code = bv.code;
    ci.code_size = bv.size;
    ci.entrypoint = blob->entry;
    ci.format = fmt;
    ci.stage = stage;
    ci.num_samplers = st->num_samplers;
    ci.num_storage_textures = st->num_storage_textures;
    ci.num_storage_buffers = st->num_storage_buffers;
    ci.num_uniform_buffers = st->num_uniform_buffers;
    SDL_GPUShader *sh = SDL_CreateGPUShader(dev, &ci);
    embi_gpu_blob_close(&bv);
    return sh;
}

static SDL_GPUGraphicsPipeline *make_pipeline(SDL_GPUDevice *dev, SDL_Window *win)
{
    SDL_GPUShader *vs = make_shader(dev, &embi_shader_host_spin.vs, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fs = make_shader(dev, &embi_shader_host_spin.fs, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (vs == nullptr || fs == nullptr) {
        fprintf(stderr, "offscreen: no shader for this device (%s)\n", SDL_GetError());
        SDL_ReleaseGPUShader(dev, vs);
        SDL_ReleaseGPUShader(dev, fs);
        return nullptr;
    }
    SDL_GPUColorTargetDescription ctd;
    memset(&ctd, 0, sizeof ctd);
    ctd.format = SDL_GetGPUSwapchainTextureFormat(dev, win);
    SDL_GPUGraphicsPipelineCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.num_color_targets = 1;
    pi.target_info.color_target_descriptions = &ctd;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (p == nullptr) {
        fprintf(stderr, "offscreen: the pipeline could not be created (%s)\n", SDL_GetError());
    }
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    return p;
}

/* Draws the spinning quad into the swapchain, which it clears first. Its centre sits to the right of the view. */
static void draw_spinner(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *sw, uint32_t sw_w, uint32_t sw_h,
                         SDL_GPUGraphicsPipeline *pipe, double seconds)
{
    const float angle = (float)(seconds * 1.4);
    const float cx = (float)sw_w * 0.78f;
    const float cy = (float)sw_h * 0.5f;
    const float half = 0.10f * (float)sw_h;
    const float params[8] = {cosf(angle), sinf(angle), 2.0f / (float)sw_w, 2.0f / (float)sw_h, cx, cy, half, 0.0f};
    SDL_GPUColorTargetInfo ct;
    memset(&ct, 0, sizeof ct);
    ct.texture = sw;
    ct.clear_color = (SDL_FColor){0.09f, 0.09f, 0.11f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    SDL_PushGPUVertexUniformData(cmd, 0, params, sizeof params);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

int main(int argc, char **argv)
{
    long frames_limit = -1;
    const char *target = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames_limit = strtol(argv[++i], nullptr, 10);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "usage: %s [--frames N] [PATH_OR_URL]\n", argv[0]);
            return 2;
        } else {
            target = argv[i];
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int status = 1;
    SDL_Window *win = SDL_CreateWindow("Ember offscreen", (int)WIN_W, (int)WIN_H, SDL_WINDOW_RESIZABLE);
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                             false, nullptr);
    if (win == nullptr || dev == nullptr || !SDL_ClaimWindowForGPUDevice(dev, win)) {
        fprintf(stderr, "window or device: %s\n", SDL_GetError());
        if (dev != nullptr) {
            SDL_DestroyGPUDevice(dev);
        }
        if (win != nullptr) {
            SDL_DestroyWindow(win);
        }
        SDL_Quit();
        return 1;
    }
    SDL_GPUGraphicsPipeline *pipe = make_pipeline(dev, win);
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = print_log;
    emb_runtime *rt = pipe != nullptr ? emb_runtime_create(&rd) : nullptr;
    emb_view *view = nullptr;
    if (rt != nullptr) {
        if (target != nullptr) {
            allow_dir_of(rt, target);
        }
        emb_view_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.size = sizeof vd;
        vd.mode = EMB_VIEW_OFFSCREEN;
        vd.width = VIEW_W;
        vd.height = VIEW_H;
        vd.scale = 1.0f;
        vd.device = dev;
        view = emb_view_create(rt, &vd);
        bool loaded = view != nullptr && (target == nullptr ? emb_view_load_html(view, k_demo_page, sizeof k_demo_page - 1, nullptr)
                                                            : emb_view_load_url(view, target));
        if (!loaded) {
            fprintf(stderr, "offscreen: the page could not be shown\n");
        } else {
            status = 0;
        }
    }

    const uint64_t t0 = SDL_GetTicks();
    long frame = 0;
    bool quit = status != 0;
    while (!quit && (frames_limit < 0 || frame < frames_limit)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) {
                    quit = true;
                } else if (ev.key.key == SDLK_DOWN) {
                    emb_view_scroll_by(view, 0.0f, KEY_PX);
                } else if (ev.key.key == SDLK_UP) {
                    emb_view_scroll_by(view, 0.0f, -KEY_PX);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: {
                /* The wheel over the view goes to the view, with the point in its CSS px. Positive y is away from the
                 * user, so the content moves down. */
                float x = ev.wheel.mouse_x - (float)VIEW_X, y = ev.wheel.mouse_y - (float)VIEW_Y;
                if (view != nullptr && x >= 0.0f && y >= 0.0f && x < (float)VIEW_W && y < (float)VIEW_H) {
                    emb_view_wheel(view, x, y, ev.wheel.x * WHEEL_PX, -ev.wheel.y * WHEEL_PX);
                }
                break;
            }
            default:
                break;
            }
        }
        if (quit || view == nullptr) {
            break;
        }

        /* The view records its frame on this command buffer when its picture changed. The host presents every frame:
         * the spinner moves each frame, and the view's texture is blitted into the left part of the window. */
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd == nullptr) {
            break;
        }
        emb_view_render(view, cmd);
        SDL_GPUTexture *sw = nullptr;
        uint32_t sw_w = 0, sw_h = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &sw, &sw_w, &sw_h) && sw != nullptr) {
            draw_spinner(cmd, sw, sw_w, sw_h, pipe, (double)(SDL_GetTicks() - t0) / 1000.0);
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof bi);
            bi.source.texture = emb_view_texture(view);
            bi.source.w = VIEW_W;
            bi.source.h = VIEW_H;
            bi.destination.texture = sw;
            bi.destination.x = VIEW_X;
            bi.destination.y = VIEW_Y;
            bi.destination.w = VIEW_W;
            bi.destination.h = VIEW_H;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        frame++;
    }

    emb_view_destroy(view);
    emb_runtime_destroy(rt);
    if (pipe != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
    }
    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return status;
}
