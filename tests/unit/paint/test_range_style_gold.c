/*
 * Golden images of the Chromium-style slider and scrollbar CSS of a game editor (the acceptance CSS: ::-webkit-slider-runnable-
 * track and ::-webkit-slider-thumb on a range with appearance: none, and a 10px ::-webkit-scrollbar with a rounded thumb), through
 * the whole embedding path. Each image is what Chromium draws from that CSS (see the notes at each test). Without a GPU device
 * the tests skip.
 */
#include "emb_test.h"
#include "emb_gold.h"

#include "embed/embed_int.h"

#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#define EDITOR_CSS                                                                                                          \
    ":root{--accent:#3a79bb}"                                                                                                \
    "body{margin:0;background:#2b2b2b}"                                                                                      \
    "::-webkit-scrollbar { width: 10px; height: 10px; }"                                                                     \
    "::-webkit-scrollbar-thumb { background: #5a5a5a; border: 2px solid transparent; background-clip: padding-box; "         \
    "border-radius: 6px; }"                                                                                                  \
    "::-webkit-scrollbar-thumb:hover { background-color: #6d6d6d; }"                                                         \
    "::-webkit-scrollbar-track, ::-webkit-scrollbar-corner { background: transparent; }"                                     \
    "input[type=range] { -webkit-appearance: none; appearance: none; width: 100%; height: 18px; margin: 0; "                 \
    "background: transparent; cursor: pointer; --pct: 50%; }"                                                                \
    "input[type=range]:focus { outline: none; }"                                                                             \
    "input[type=range]::-webkit-slider-runnable-track { height: 4px; border-radius: 2px; "                                   \
    "background: linear-gradient(to right, var(--accent) 0 var(--pct), #4a4a4a var(--pct) 100%); }"                          \
    "input[type=range]:hover::-webkit-slider-runnable-track { "                                                              \
    "background: linear-gradient(to right, #4b8ad0 0 var(--pct), #565656 var(--pct) 100%); }"                                \
    "input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 12px; height: 12px; "      \
    "margin-top: -4px; border: 0; border-radius: 50%; background: #e6e6e6; "                                                 \
    "box-shadow: 0 0 0 1px rgba(0,0,0,.45), 0 1px 3px rgba(0,0,0,.5); transition: transform .08s ease-out, background .08s; }" \
    "input[type=range]:hover::-webkit-slider-thumb { transform: scale(1.2); background: #fff; }"                             \
    "input[type=range]:active::-webkit-slider-thumb { transform: scale(1.35); background: #fff; "                            \
    "box-shadow: 0 0 0 1px rgba(0,0,0,.45), 0 0 0 4px rgba(58,121,187,.35); }"                                               \
    ".paint-panel { position: absolute; left: 8px; width: 200px; display: flex; align-items: center; }"                      \
    ".paint-panel input[type=\"range\"] { flex: 1; min-width: 0; }"

static const char k_sliders[] =
    "<!doctype html><title>Sliders</title><style>" EDITOR_CSS "</style>"
    "<div class=paint-panel style='top:8px'><input type=range min=0 max=100 value=0 style='--pct:0%'></div>"
    "<div class=paint-panel style='top:36px'><input type=range min=0 max=100 value=37 style='--pct:37%'></div>"
    "<div class=paint-panel style='top:64px'><input type=range min=0 max=100 value=100 style='--pct:100%'></div>";

static const char k_scroll[] =
    "<!doctype html><title>Scroll</title><style>" EDITOR_CSS
    ".box{position:absolute;left:8px;top:8px;width:120px;height:80px;overflow:auto;background:#3a3a3a}"
    ".tall{height:240px;background:linear-gradient(#444,#666)}"
    ".both{position:absolute;left:144px;top:8px;width:64px;height:80px;overflow:scroll;background:#3a3a3a}"
    ".wide{width:200px;height:120px;background:linear-gradient(to right,#444,#666)}"
    "</style><div class=box><div class=tall></div></div><div class=both><div class=wide></div></div>";

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

#define W 224
#define H 96

typedef struct rig {
    host h;
    emb_runtime *rt;
    emb_view *v;
} rig;

static bool rig_open(rig *r, const char *html)
{
    memset(r, 0, sizeof *r); /* rig_close runs after a failed open too */
    if (!host_open(&r->h)) {
        return false;
    }
    r->rt = emb_runtime_create(nullptr);
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = W;
    d.height = H;
    d.scale = 1.0f;
    d.device = r->h.dev;
    r->v = r->rt != nullptr ? emb_view_create(r->rt, &d) : nullptr;
    if (r->v == nullptr) {
        return false;
    }
    (void)emb_view_load_html(r->v, html, strlen(html), nullptr);
    return render_now(r->h.dev, r->v);
}

static void rig_close(rig *r)
{
    emb_view_destroy(r->v);
    emb_runtime_destroy(r->rt);
    host_close(&r->h);
}

static bool rig_gold(rig *r, const char *name)
{
    (void)render_now(r->h.dev, r->v);
    uint8_t *px = readback(r->h.dev, emb_view_texture(r->v), W, H);
    if (px == nullptr) {
        return false;
    }
    bool ok = emb_gold_check(name, px, W, H);
    SDL_free(px);
    return ok;
}

/* Three sliders at 0%, 37% and 100% with the editor's CSS. Chromium draws a 4px track that is the accent up to the fraction
 * and grey after, rounded at both ends, centred in the 18px input, and a 12px light disc with a hairline dark ring and a soft
 * shadow whose left edge is at fraction * (track width - 12). */
EMB_TEST(styled_sliders_match_golden)
{
    rig r;
    if (!rig_open(&r, k_sliders)) {
        rig_close(&r);
        return;
    }
    EMB_CHECK(rig_gold(&r, "range_styled_values"));
    rig_close(&r);
}

/* The pointer over the middle slider: its track is the lighter blue and grey, and its thumb is white and 1.2 times as large. */
EMB_TEST(styled_slider_hover_matches_golden)
{
    rig r;
    if (!rig_open(&r, k_sliders)) {
        rig_close(&r);
        return;
    }
    emb_view_mouse_move(r.v, 150.0f, 45.0f, 0);
    EMB_CHECK(rig_gold(&r, "range_styled_hover"));
    rig_close(&r);
}

/* A 120x80 box and a 64x80 one with the 10px scrollbars: the first has a vertical bar only (a #5a5a5a thumb inset 2px with
 * rounded ends in a transparent track), the second both, with an empty corner. The content is 10px narrower than the box. */
EMB_TEST(styled_scrollbars_match_golden)
{
    rig r;
    if (!rig_open(&r, k_scroll)) {
        rig_close(&r);
        return;
    }
    EMB_CHECK(rig_gold(&r, "scrollbar_styled"));
    rig_close(&r);
}

/* The pointer on the thumb of the first box: the thumb is #6d6d6d. */
EMB_TEST(styled_scrollbar_thumb_hover_matches_golden)
{
    rig r;
    if (!rig_open(&r, k_scroll)) {
        rig_close(&r);
        return;
    }
    emb_view_mouse_move(r.v, 123.0f, 20.0f, 0);
    EMB_CHECK(rig_gold(&r, "scrollbar_styled_hover"));
    rig_close(&r);
}

EMB_TEST_MAIN()
