#pragma once
/*
 * Shared rig of the crisp text tests (test_text_crisp*.c): renders a page of tests/pages through an
 * offscreen view and reads the pixels back. Not a test itself.
 */
#include "emb_test.h"
#include "emb_gold.h"
#include "emb_render_test.h"

#include "embed/embed_int.h"

#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

typedef struct crisp_page {
    uint8_t *px; /* RGBA8, w * h, SDL_free */
    int w, h;
} crisp_page;

static inline uint8_t *crisp_readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
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

/* Renders html (len bytes) at scale 1 and size w x h with the given text antialiasing. False when
 * there is no GPU device (the test should skip). */
static inline bool crisp_render_html(const char *html, size_t len, int w, int h, emb_text_aa aa,
                                     crisp_page *out)
{
    out->px = nullptr;
    out->w = w;
    out->h = h;
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        return false;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = (uint32_t)w;
    d.height = (uint32_t)h;
    d.scale = 1.0f;
    d.device = dev;
    d.text_aa = aa;
    emb_view *v = rt != nullptr ? emb_view_create(rt, &d) : nullptr;
    if (v != nullptr && emb_view_load_html(v, html, len, nullptr)) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd != nullptr) {
            (void)emb_view_render(v, cmd);
            SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            if (fence != nullptr) {
                SDL_WaitForGPUFences(dev, true, &fence, 1);
                SDL_ReleaseGPUFence(dev, fence);
            }
            out->px = crisp_readback(dev, emb_view_texture(v), (uint32_t)w, (uint32_t)h);
        }
    }
    if (v != nullptr) {
        emb_view_destroy(v);
    }
    if (rt != nullptr) {
        emb_runtime_destroy(rt);
    }
    embi_gpu_destroy(g);
    return out->px != nullptr;
}

static inline bool crisp_render_page(const char *rel, int w, int h, emb_text_aa aa, crisp_page *out)
{
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/%s", EMBER_SOURCE_DIR, rel);
    size_t n = 0;
    char *html = SDL_LoadFile(path, &n);
    if (html == nullptr) {
        SDL_Log("cannot read %s", path);
        return false;
    }
    bool ok = crisp_render_html(html, n, w, h, aa, out);
    SDL_free(html);
    return ok;
}

static inline void crisp_free(crisp_page *p)
{
    SDL_free(p->px);
    p->px = nullptr;
}

/* EMBER_CRISP_OUT=<dir>: writes <dir>/crisp_<name>.bmp. */
static inline void crisp_dump(const crisp_page *p, const char *name)
{
    const char *dir = SDL_getenv("EMBER_CRISP_OUT");
    if (dir == nullptr || dir[0] == '\0' || p->px == nullptr) {
        return;
    }
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/crisp_%s.bmp", dir, name);
    (void)emb_gold_save(path, p->px, p->w, p->h);
}
