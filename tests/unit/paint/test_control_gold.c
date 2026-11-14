/*
 * A golden image of the native form controls (W5e, src/paint/control.c through the whole embedding path): checkboxes and
 * radios in their states, a range, progress and meter bars, a button face and an empty text field. The page has no text, so the
 * picture does not depend on glyph rasterization. Without a GPU device the test skips.
 */
#include "emb_test.h"
#include "emb_gold.h"

#include "embed/embed_int.h"

#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#define GOLD_W 240
#define GOLD_H 120

static const char k_page[] =
    "<!doctype html><title>Controls</title><style>"
    "body{margin:0;background:#fff} input,progress,meter,button{position:absolute;margin:0}"
    "</style>"
    "<input type=checkbox style='left:8px;top:8px'>"
    "<input type=checkbox checked style='left:32px;top:8px'>"
    "<input type=radio style='left:56px;top:8px'>"
    "<input type=radio checked style='left:80px;top:8px'>"
    "<input type=range min=0 max=10 value=3 style='left:8px;top:32px;width:120px'>"
    "<progress value=40 max=100 style='left:8px;top:56px;width:120px;height:12px'></progress>"
    "<meter value=70 min=0 max=100 low=30 high=80 optimum=50 style='left:8px;top:76px;width:120px;height:12px'></meter>"
    "<input type=text style='left:136px;top:8px;width:96px'>"
    "<button style='left:136px;top:40px;width:64px;height:24px'></button>"
    "<input type=checkbox checked style='left:136px;top:76px;appearance:none;width:13px;height:13px;background:#00f'>";

typedef struct host {
    embi_gpu *g;
    SDL_GPUDevice *dev;
} host;

static bool host_open(host *h)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        emb_test_cur_skip = "no video";
        return false;
    }
    h->g = embi_gpu_create(nullptr, false);
    if (h->g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return false;
    }
    h->dev = embi_gpu_device(h->g);
    return true;
}

static void host_close(host *h)
{
    embi_gpu_destroy(h->g);
    h->g = nullptr;
}

/* Renders the view's frame and waits for the GPU. Returns the render's answer (changed). */
static bool render_now(SDL_GPUDevice *dev, emb_view *v)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    return changed;
}

/* Reads the view's texture back as tightly packed RGBA8 (SDL_free). */
static uint8_t *readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    uint8_t *out = nullptr;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (tb == nullptr || cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = tex;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = w;
    info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            SDL_memcpy(out, mapped, bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

static emb_view *open_view(emb_runtime *rt, SDL_GPUDevice *dev, uint32_t w, uint32_t h, float scale)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = w;
    d.height = h;
    d.scale = scale;
    d.device = dev;
    return emb_view_create(rt, &d);
}

EMB_TEST(control_page_matches_golden)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_html(v, k_page, strlen(k_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    uint8_t *px = readback(h.dev, emb_view_texture(v), GOLD_W, GOLD_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(emb_gold_check("control_page", px, GOLD_W, GOLD_H));
    SDL_free(px);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST_MAIN()
