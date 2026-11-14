/*
 * A page background that tiles a 1x1 image over the whole viewport (BUGBURN2, display-contents-root-background): more
 * tiles than the renderer draws one by one must still paint the image. Without a GPU device the tests skip.
 */
#include "emb_test.h"

#include "embed/embed_int.h"

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

typedef struct host {
    embi_gpu *g;
    SDL_GPUDevice *dev;
} host;

static bool host_open(host *h)
{
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

static uint8_t *readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    uint8_t *out = nullptr;
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

static emb_view *open_view(emb_runtime *rt, SDL_GPUDevice *dev, uint32_t w, uint32_t h)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = w;
    d.height = h;
    d.scale = 1.0f;
    d.device = dev;
    return emb_view_create(rt, &d);
}

static bool pixel_is(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t *p = px + ((size_t)y * w + x) * 4u;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 0xFF;
}

/* A 1x1 green PNG, served for g.png. */
static const char k_png_b64[] = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABAQMAAAAl21bKAAAABGdBTUEAAK/INwWK6QAAAANQTFRFAIAAnPmlkQAAAApJREFUeNpjYAAAAAIAAeUn3vwAAAAZdEVYdFNvZnR3YXJlAEFkb2JlIEltYWdlUmVhZHlxyWU8AAAAAElFTkSuQmCC";
static const char k_page[] = "<!doctype html><body style=\"margin:0;background:url(g.png)\"><p style=\"margin:0\">x</p>";
static uint8_t g_png[256];
static size_t g_png_len;

static bool b64_decode(void)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0;
    int bits = 0;
    g_png_len = 0;
    for (const char *c = k_png_b64; *c != 0 && *c != '='; c++) {
        const char *p = strchr(tab, *c);
        if (p == nullptr) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)(p - tab);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            g_png[g_png_len++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    return true;
}

static bool handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    (void)user;
    (void)url_len;
    const char *tail = strrchr(url, '/');
    tail = tail != nullptr ? tail + 1 : url;
    if (strcmp(tail, "page.html") == 0) {
        out->data = (const uint8_t *)k_page;
        out->len = sizeof k_page - 1;
        return true;
    }
    if (strcmp(tail, "g.png") == 0) {
        out->data = g_png;
        out->len = g_png_len;
        return true;
    }
    return false;
}

/* 400x300 px of 1x1 tiles are 120000 tiles: far past what is drawn one by one, so the layer must still fill the viewport. */
EMB_TEST(one_pixel_image_tiled_over_the_viewport_paints_all_of_it)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    EMB_REQUIRE(b64_decode());
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", handler, nullptr));
    emb_view *v = open_view(rt, h.dev, 400, 300);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    bool green = false;
    for (int i = 0; i < 100 && !green; i++) {
        (void)render_now(h.dev, v);
        uint8_t *px = readback(h.dev, emb_view_texture(v), 400, 300);
        EMB_REQUIRE(px != nullptr);
        green = pixel_is(px, 400, 300, 200, 0x00, 0x80, 0x00);
        if (green) {
            EMB_CHECK(pixel_is(px, 400, 399, 299, 0x00, 0x80, 0x00));
            EMB_CHECK(pixel_is(px, 400, 5, 150, 0x00, 0x80, 0x00));
        }
        SDL_free(px);
        if (!green) {
            SDL_Delay(10);
        }
    }
    EMB_CHECK(green);
    emb_view_destroy(v);
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "mem", nullptr, nullptr));
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* The GPU tests use the video subsystem, initialized once around the run. They draw offscreen, so unless the environment
 * names a video driver the offscreen one is used and the tests run on a machine without a display. */
int main(void)
{
    (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_VIDEO_DRIVER", "offscreen", false);
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
