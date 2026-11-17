/*
 * -webkit-text-stroke in the display list and in the pixels: a stroked run is one stroke path item over its fill, a zero width
 * emits nothing, equal text shares its path's content hash, a translucent stroke is never darker than its alpha (no overlapping
 * copies), and a 6 px stroke widens the ink of an I by 6 px. The pixel tests need a GPU device and skip without one.
 */
#include "paint_harness.h"

#include "embed/embed_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

/* The stroke item of the display list: the first EMBI_DI_PATH item with the stroke flag, or -1. */
static int stroke_item(const embi_dl *d)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_PATH && (d->items.data[i].flags & EMBI_DF_STROKE) != 0) {
            return (int)i;
        }
    }
    return -1;
}

EMB_TEST(stroked_word_emits_one_stroke_path_item_over_one_fill)
{
    ph h = ph_make("<div style=\"-webkit-text-stroke:4px #f00\">Hello</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_PATH), 1);
    int si = stroke_item(&h.dl);
    EMB_REQUIRE(si >= 0);
    const embi_ditem *it = &h.dl.items.data[si];
    EMB_CHECK(fabsf(it->f[0] - 4.0f) < 1e-4f);      /* the width */
    EMB_CHECK(it->f[1] == 4.0f);                     /* the miter limit */
    EMB_CHECK(it->styles[1] == 0 && it->styles[2] == 0); /* butt caps, miter joins */
    EMB_CHECK(it->colors[0] == PH_RED);
    /* The fill comes first, the stroke over it. */
    int ti = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(ti >= 0);
    EMB_CHECK(ti < si);
    ph_free(&h);
}

EMB_TEST(zero_stroke_width_emits_nothing)
{
    ph h = ph_make("<div style=\"-webkit-text-stroke:0\">Hello</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_PATH), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    ph_free(&h);
}

/* The content hash of the stroke path of an html page (0 when it has none). */
static uint64_t stroke_hash(const char *html)
{
    ph h = ph_make(html, "");
    int si = stroke_item(&h.dl);
    uint64_t hash = si >= 0 ? h.dl.paths.data[h.dl.items.data[si].aux].hash : 0;
    ph_free(&h);
    return hash;
}

EMB_TEST(repainted_stroked_text_has_the_same_path_hash)
{
    /* The same text at the same place gives the same path content, so an incremental paint sees no change. A different
     * stroke width gives a different path hash. */
    const char *page = "<div style=\"-webkit-text-stroke-width:2px;-webkit-text-stroke-color:#000\">Same</div>";
    const uint64_t a = stroke_hash(page);
    EMB_CHECK(a != 0);
    EMB_CHECK(a == stroke_hash(page));
    EMB_CHECK(a != stroke_hash("<div style=\"-webkit-text-stroke-width:2px;-webkit-text-stroke-color:#000\">Other</div>"));
}

/* ---- pixels */

#define VIEW_W 120
#define VIEW_H 90

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

static void submit_wait(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
}

static bool render_now(SDL_GPUDevice *dev, emb_view *v)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    submit_wait(dev, cmd);
    return changed;
}

/* Reads the w x h texture back as tightly packed RGBA8 (SDL_free). */
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

static emb_view *open_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = VIEW_W;
    d.height = VIEW_H;
    d.scale = 1.0f;
    d.device = dev;
    return emb_view_create(rt, &d);
}

/* Renders html in an offscreen view and returns its pixels (SDL_free), or nullptr. */
static uint8_t *render_page(emb_runtime *rt, SDL_GPUDevice *dev, const char *html)
{
    emb_view *v = open_view(rt, dev);
    if (v == nullptr) {
        return nullptr;
    }
    uint8_t *px = nullptr;
    if (emb_view_load_html(v, html, strlen(html), nullptr) && render_now(dev, v)) {
        px = readback(dev, emb_view_texture(v), VIEW_W, VIEW_H);
    }
    emb_view_destroy(v);
    return px;
}

/* The width in px of the dark ink (red below 128) of a picture, from its leftmost to its rightmost column; -1 without ink. */
static int ink_width(const uint8_t *px)
{
    int minx = VIEW_W, maxx = -1;
    for (int y = 0; y < VIEW_H; y++) {
        for (int x = 0; x < VIEW_W; x++) {
            if (px[((size_t)y * VIEW_W + (size_t)x) * 4u] < 128) {
                minx = x < minx ? x : minx;
                maxx = x > maxx ? x : maxx;
            }
        }
    }
    return maxx < 0 ? -1 : maxx - minx + 1;
}

#define I_PAGE "<!doctype html><style>body{margin:0;background:#fff}</style>" \
               "<div style=\"position:absolute;left:30px;top:8px;font:48px sans-serif;line-height:60px;color:#000;%s\">I</div>"

EMB_TEST(six_px_stroke_widens_the_ink_of_an_i_by_six)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char plain[512], stroked[512];
    SDL_snprintf(plain, sizeof plain, I_PAGE, "");
    SDL_snprintf(stroked, sizeof stroked, I_PAGE, "-webkit-text-stroke-width:6px;-webkit-text-stroke-color:#000");
    uint8_t *a = render_page(rt, h.dev, plain);
    uint8_t *b = render_page(rt, h.dev, stroked);
    EMB_REQUIRE(a != nullptr && b != nullptr);
    const int wa = ink_width(a), wb = ink_width(b);
    EMB_REQUIRE(wa > 0 && wb > 0);
    EMB_CHECK(abs((wb - wa) - 6) <= 1);
    SDL_free(a);
    SDL_free(b);
    emb_runtime_destroy(rt);
}

EMB_TEST(translucent_stroke_is_never_darker_than_its_alpha)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char page[512];
    SDL_snprintf(page, sizeof page, I_PAGE, "color:transparent;-webkit-text-stroke-width:4px;-webkit-text-stroke-color:rgba(0,0,0,0.5)");
    uint8_t *px = render_page(rt, h.dev, page);
    EMB_REQUIRE(px != nullptr);
    /* On white, a stroke of alpha a leaves 255 * (1 - a) in each channel. One stroke copy over the outline gives no pixel
     * more than 0.5 alpha, plus a level of rounding: a red channel below 126 would mean overlapping copies. */
    int darkest = 255;
    for (size_t i = 0; i < (size_t)VIEW_W * VIEW_H; i++) {
        darkest = px[i * 4u] < darkest ? px[i * 4u] : darkest;
    }
    EMB_CHECK(darkest >= 126);
    EMB_CHECK(darkest < 255); /* the stroke is there */
    SDL_free(px);
    emb_runtime_destroy(rt);
}

int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
