/*
 * Canvas 2D in a view (R4b): a script draws on a canvas element and the frame shows it as a replaced element backed by the
 * canvas texture. The canvas is never read back by script, so this exercises the frame-time flush, the display item of the
 * element ("ember-canvas:") and the version in aux2 that damages the element's rect when a later script draws again.
 * Needs a GPU device; without one the tests skip.
 */
#include "emb_test.h"

#include "embed/embed_int.h"

#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

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

/* The RGB of pixel (x, y) of a 128-wide frame, as 0xRRGGBB. */
static uint32_t pixel(const uint8_t *px, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * 128u + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void copy_result(void *user, bool ok, const char *text, size_t len)
{
    (void)ok;
    char *dst = user;
    size_t n = len < 63 ? len : 63;
    memcpy(dst, text, n);
    dst[n] = 0;
}

static void ignore_result(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    (void)ok;
    (void)text;
    (void)len;
}

/* A 40 x 30 canvas at the top left, drawn red by the page's script; a second canvas (never drawn on) at (0, 40). */
static const char k_page[] =
    "<!doctype html><body style='margin:0;background:#fff'>"
    "<canvas id=c width=40 height=30 style='display:block'></canvas>"
    "<canvas id=idle width=20 height=10 style='display:block;margin-top:10px'></canvas>"
    "<script>var x = document.getElementById('c').getContext('2d');"
    "x.fillStyle = '#f00'; x.fillRect(0, 0, 40, 30);</script>";

EMB_TEST(canvas_in_a_view_shows_the_script_drawing_and_redraws)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view_desc desc = {
        .size = sizeof desc,
        .mode = EMB_VIEW_OFFSCREEN,
        .width = 128,
        .height = 96,
        .scale = 1.0f,
        .device = dev,
        .scheme = EMB_SCHEME_LIGHT,
    };
    emb_view *v = emb_view_create(rt, &desc);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page, strlen(k_page), "http://t.test/canvas.html"));
    EMB_CHECK(render_now(dev, v));

    uint8_t *px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0xFF0000);
    EMB_CHECK(pixel(px, 39, 29) == 0xFF0000);
    EMB_CHECK(pixel(px, 60, 10) == 0xFFFFFF);  /* beside the canvas: the page */
    EMB_CHECK(pixel(px, 10, 35) == 0xFFFFFF);  /* the margin between the canvases */
    EMB_CHECK(pixel(px, 5, 45) == 0xFFFFFF);   /* the idle canvas is transparent: the page shows through */
    SDL_free(px);

    /* Nothing changed: no frame. */
    EMB_CHECK(!render_now(dev, v));

    /* A second script draws green on the left half: the canvas version changes the item, the rect is damaged and redrawn. */
    const char *draw = "x.fillStyle = '#0f0'; x.fillRect(0, 0, 20, 30); 1";
    char res[64] = "";
    EMB_CHECK(emb_view_eval(v, draw, strlen(draw), copy_result, res));
    EMB_CHECK(render_now(dev, v));
    px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0x00FF00);
    EMB_CHECK(pixel(px, 30, 10) == 0xFF0000);
    EMB_CHECK(pixel(px, 60, 10) == 0xFFFFFF);
    SDL_free(px);

    /* A resize of the canvas bitmap clears it and the new size shows: transparent over the white page. */
    const char *resize = "document.getElementById('c').width = 16; 1";
    EMB_CHECK(emb_view_eval(v, resize, strlen(resize), ignore_result, nullptr));
    EMB_CHECK(render_now(dev, v));
    px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0xFFFFFF);
    SDL_free(px);

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

/* The GPU device needs the video subsystem, initialized once around the run (as the other GPU tests do). */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
