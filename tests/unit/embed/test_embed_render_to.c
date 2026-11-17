/*
 * Offscreen views drawn into a rectangle of a host texture (emb_view_render_to): the pixels outside the rectangle are never
 * written, the rectangle equals a whole-view render, two views share one texture, a partial redraw equals a fresh full draw,
 * a raster scale matches a scale without a new layout, BGRA targets, a mipmapped own texture, a view with no texture of its
 * own, and bad targets are refused. Without a GPU device the tests skip.
 */
#include "emb_test.h"

#include "embed/embed_int.h"

#include <stddef.h>
#include <string.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#define VIEW_W 100
#define VIEW_H 80
#define HOST_W 300
#define HOST_H 200

/* The clear color of the host texture: a sentinel no page paints. Opaque, so the readback is exact. */
#define SENTINEL_R 0x80u
#define SENTINEL_G 0x40u
#define SENTINEL_B 0xC0u
#define SENTINEL_A 0xFFu

/* A page with a solid background, a bordered box and a line of text. The box's color is the only difference between the
 * pages that differ in it. */
#define PAGE_HEAD "<!doctype html><title>R</title><style>body{margin:0;background:#fff}"
#define PAGE_BOX(COLOR)                                                                                                \
    ".box{position:absolute;left:10px;top:8px;width:56px;height:30px;background:" COLOR                                 \
    ";border:3px solid #000;border-radius:6px}"
#define PAGE_TEXT ".t{position:absolute;left:6px;top:52px;font:14px sans-serif;color:#222}</style>"                        \
    "<div class=box id=box></div><div class=t>Render me</div>"
static const char k_page_red[] = PAGE_HEAD PAGE_BOX("#e53935") PAGE_TEXT;
static const char k_page_blue[] = PAGE_HEAD PAGE_BOX("#1e88e5") PAGE_TEXT;
static const char k_eval_blue[] = "document.getElementById('box').style.background = '#1e88e5';";

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

/* Submits cmd and waits for the GPU. */
static void submit_wait(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
}

/* Renders the view's own frame. Returns the render's answer (changed). */
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

/* Renders the view into the rectangle (x, y) of tex (tw x th). Returns the answer of emb_view_render_to. */
static bool render_to_now(SDL_GPUDevice *dev, emb_view *v, SDL_GPUTexture *tex, uint32_t tw, uint32_t th, int32_t x,
                          int32_t y, uint32_t flags)
{
    emb_render_target t;
    memset(&t, 0, sizeof t);
    t.size = sizeof t;
    t.texture = tex;
    t.texture_w = tw;
    t.texture_h = th;
    t.x = x;
    t.y = y;
    t.flags = flags;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool ok = emb_view_render_to(v, cmd, &t);
    submit_wait(dev, cmd);
    return ok;
}

static SDL_GPUTexture *make_tex(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h, uint32_t levels)
{
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = fmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = levels;
    return SDL_CreateGPUTexture(dev, &ci);
}

/* Fills tex with the sentinel color (a render pass that clears it). */
static void clear_sentinel(SDL_GPUDevice *dev, SDL_GPUTexture *tex)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return;
    }
    SDL_GPUColorTargetInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.texture = tex;
    ci.clear_color.r = (float)SENTINEL_R / 255.0f;
    ci.clear_color.g = (float)SENTINEL_G / 255.0f;
    ci.clear_color.b = (float)SENTINEL_B / 255.0f;
    ci.clear_color.a = (float)SENTINEL_A / 255.0f;
    ci.load_op = SDL_GPU_LOADOP_CLEAR;
    ci.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
    if (pass != nullptr) {
        SDL_EndGPURenderPass(pass);
    }
    submit_wait(dev, cmd);
}

/* Reads mip level mip of tex (w x h at that level) back as tightly packed 4-byte pixels (SDL_free). */
static uint8_t *readback_mip(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h, uint32_t mip)
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
    region.mip_level = mip;
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

static uint8_t *readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    return readback_mip(dev, tex, w, h, 0);
}

static emb_view *open_view(emb_runtime *rt, SDL_GPUDevice *dev, uint32_t w, uint32_t h, float scale, uint32_t flags,
                           uint32_t format)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = w;
    d.height = h;
    d.scale = scale;
    d.device = dev;
    d.flags = flags;
    d.format = format;
    return emb_view_create(rt, &d);
}

/* The number of pixels that differ between the rectangle (x, y, w, h) of the tw-wide picture a and the w x h picture b. */
static size_t rect_mismatch(const uint8_t *a, uint32_t tw, int32_t x, int32_t y, const uint8_t *b, uint32_t w, uint32_t h)
{
    size_t bad = 0;
    for (uint32_t r = 0; r < h; r++) {
        const uint8_t *ra = a + (((size_t)(y + (int32_t)r) * tw) + (size_t)x) * 4u;
        const uint8_t *rb = b + (size_t)r * w * 4u;
        for (uint32_t c = 0; c < w; c++) {
            bad += memcmp(ra + c * 4u, rb + c * 4u, 4) != 0 ? 1u : 0u;
        }
    }
    return bad;
}

/* The number of pixels of the whole w x h picture that differ from the sentinel in the sentinel's channels. */
static size_t count_not_sentinel(const uint8_t *px, uint32_t w, uint32_t h)
{
    size_t n = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *p = px + i * 4u;
        if (p[0] != SENTINEL_R || p[1] != SENTINEL_G || p[2] != SENTINEL_B || p[3] != SENTINEL_A) {
            n++;
        }
    }
    return n;
}

/* Two pictures of the same size, at most 2 levels apart per channel, on all but 0.5 % of the pixels. */
static bool close_pictures(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h)
{
    size_t far = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int m = 0;
        for (int c = 0; c < 4; c++) {
            int d = (int)a[i * 4u + (size_t)c] - (int)b[i * 4u + (size_t)c];
            if (d < 0) {
                d = -d;
            }
            if (d > m) {
                m = d;
            }
        }
        far += m > 2 ? 1u : 0u;
    }
    return far * 200u <= (size_t)w * h;
}

EMB_TEST(sub_rect_writes_only_its_rect)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);
    uint8_t *before = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(before != nullptr);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 37, 21, 0));
    uint8_t *after = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(after != nullptr);

    /* Every pixel outside the rectangle is unchanged (the sentinel). */
    size_t outside = 0;
    for (uint32_t y = 0; y < HOST_H; y++) {
        for (uint32_t x = 0; x < HOST_W; x++) {
            const bool inside = x >= 37 && x < 37 + VIEW_W && y >= 21 && y < 21 + VIEW_H;
            const size_t i = ((size_t)y * HOST_W + x) * 4u;
            if (!inside && memcmp(&after[i], &before[i], 4) != 0) {
                outside++;
            }
        }
    }
    EMB_CHECK(outside == 0);
    EMB_CHECK(after[0] == SENTINEL_R && after[1] == SENTINEL_G && after[2] == SENTINEL_B && after[3] == SENTINEL_A);

    /* The rectangle equals the same page drawn whole. */
    emb_view *ref = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(ref != nullptr);
    EMB_REQUIRE(emb_view_load_html(ref, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, ref));
    uint8_t *rp = readback(h.dev, emb_view_texture(ref), VIEW_W, VIEW_H);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rect_mismatch(after, HOST_W, 37, 21, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(after);
    SDL_free(before);
    emb_view_destroy(ref);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(two_views_one_texture)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *atlas = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(atlas != nullptr);
    clear_sentinel(h.dev, atlas);

    emb_view *a = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    emb_view *b = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(a != nullptr && b != nullptr);
    EMB_REQUIRE(emb_view_load_html(a, k_page_red, strlen(k_page_red), nullptr));
    EMB_REQUIRE(emb_view_load_html(b, k_page_blue, strlen(k_page_blue), nullptr));

    /* Both views go into one command buffer. */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(h.dev);
    EMB_REQUIRE(cmd != nullptr);
    emb_render_target ta;
    memset(&ta, 0, sizeof ta);
    ta.size = sizeof ta;
    ta.texture = atlas;
    ta.texture_w = HOST_W;
    ta.texture_h = HOST_H;
    ta.x = 0;
    ta.y = 0;
    emb_render_target tb = ta;
    tb.x = 120;
    EMB_CHECK(emb_view_render_to(a, cmd, &ta));
    EMB_CHECK(emb_view_render_to(b, cmd, &tb));
    submit_wait(h.dev, cmd);

    uint8_t *px = readback(h.dev, atlas, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);

    emb_view *ra = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    emb_view *rb = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(ra != nullptr && rb != nullptr);
    EMB_REQUIRE(emb_view_load_html(ra, k_page_red, strlen(k_page_red), nullptr));
    EMB_REQUIRE(emb_view_load_html(rb, k_page_blue, strlen(k_page_blue), nullptr));
    EMB_CHECK(render_now(h.dev, ra));
    EMB_CHECK(render_now(h.dev, rb));
    uint8_t *pa = readback(h.dev, emb_view_texture(ra), VIEW_W, VIEW_H);
    uint8_t *pb = readback(h.dev, emb_view_texture(rb), VIEW_W, VIEW_H);
    EMB_REQUIRE(pa != nullptr && pb != nullptr);
    EMB_CHECK(rect_mismatch(px, HOST_W, 0, 0, pa, VIEW_W, VIEW_H) == 0);
    EMB_CHECK(rect_mismatch(px, HOST_W, 120, 0, pb, VIEW_W, VIEW_H) == 0);

    /* The rest of the texture is still the sentinel: the 20 px between the views and everything below them. */
    size_t stray = 0;
    for (uint32_t y = 0; y < HOST_H; y++) {
        for (uint32_t x = 0; x < HOST_W; x++) {
            const bool in_a = x < VIEW_W && y < VIEW_H;
            const bool in_b = x >= 120 && x < 120 + VIEW_W && y < VIEW_H;
            if (!in_a && !in_b && count_not_sentinel(px + (((size_t)y * HOST_W + x) * 4u), 1, 1) != 0) {
                stray++;
            }
        }
    }
    EMB_CHECK(stray == 0);

    SDL_free(pa);
    SDL_free(pb);
    SDL_free(px);
    emb_view_destroy(ra);
    emb_view_destroy(rb);
    emb_view_destroy(a);
    emb_view_destroy(b);
    SDL_ReleaseGPUTexture(h.dev, atlas);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(preserved_damage_equals_full)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 20, 30, 0));

    /* One box changes color; the second draw redraws only what changed. */
    EMB_CHECK(emb_view_eval(v, k_eval_blue, strlen(k_eval_blue), nullptr, nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 20, 30, EMB_RENDER_PRESERVED));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(!st.full_redraw);
    EMB_CHECK(st.damage_rects > 0);

    /* Nothing changed since: the preserved rectangle records nothing. */
    EMB_CHECK(!render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 20, 30, EMB_RENDER_PRESERVED));

    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);

    /* A fresh full draw of the changed page. */
    emb_view *ref = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(ref != nullptr);
    EMB_REQUIRE(emb_view_load_html(ref, k_page_blue, strlen(k_page_blue), nullptr));
    EMB_CHECK(render_now(h.dev, ref));
    uint8_t *rp = readback(h.dev, emb_view_texture(ref), VIEW_W, VIEW_H);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rect_mismatch(px, HOST_W, 20, 30, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(ref);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(raster_scale_matches_scale)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);

    /* View A: scale 1, then raster 2 set after the first draw. */
    emb_view *a = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(emb_view_load_html(a, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, a));
    const uint64_t serial = embi_doc_layout_serial(a->doc);
    emb_view_set_raster_scale(a, 2.0f);
    EMB_CHECK(render_now(h.dev, a));
    /* The raster changes the drawing only: the next update lays nothing out again. */
    EMB_CHECK(embi_doc_layout_serial(a->doc) == serial);
    uint32_t aw = 0, ah = 0;
    emb_view_device_size(a, &aw, &ah);
    EMB_CHECK(aw == 2 * VIEW_W && ah == 2 * VIEW_H);

    /* View B: scale 2. */
    emb_view *b = open_view(rt, h.dev, VIEW_W, VIEW_H, 2.0f, 0, 0);
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(emb_view_load_html(b, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, b));
    uint32_t bw = 0, bh = 0;
    emb_view_device_size(b, &bw, &bh);
    EMB_CHECK(bw == aw && bh == ah);

    uint8_t *pa = readback(h.dev, emb_view_texture(a), aw, ah);
    uint8_t *pb = readback(h.dev, emb_view_texture(b), bw, bh);
    EMB_REQUIRE(pa != nullptr && pb != nullptr);
    EMB_CHECK(close_pictures(pa, pb, aw, ah));

    SDL_free(pa);
    SDL_free(pb);
    emb_view_destroy(a);
    emb_view_destroy(b);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(bgra_format)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);

    emb_view *bg = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
    emb_view *rg = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(bg != nullptr && rg != nullptr);
    EMB_REQUIRE(emb_view_load_html(bg, k_page_red, strlen(k_page_red), nullptr));
    EMB_REQUIRE(emb_view_load_html(rg, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, bg));
    EMB_CHECK(render_now(h.dev, rg));
    uint8_t *pb = readback(h.dev, emb_view_texture(bg), VIEW_W, VIEW_H);
    uint8_t *pr = readback(h.dev, emb_view_texture(rg), VIEW_W, VIEW_H);
    EMB_REQUIRE(pb != nullptr && pr != nullptr);

    /* Same picture with the red and blue channels swapped. */
    size_t bad = 0;
    for (size_t i = 0; i < (size_t)VIEW_W * VIEW_H; i++) {
        const uint8_t *q = pb + i * 4u;
        const uint8_t *p = pr + i * 4u;
        if (q[0] != p[2] || q[1] != p[1] || q[2] != p[0] || q[3] != p[3]) {
            bad++;
        }
    }
    EMB_CHECK(bad == 0);

    /* The same view drawn into a BGRA host texture, one rectangle. */
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);
    EMB_CHECK(render_to_now(h.dev, bg, host_tex, HOST_W, HOST_H, 7, 9, 0));
    uint8_t *hp = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(hp != nullptr);
    EMB_CHECK(rect_mismatch(hp, HOST_W, 7, 9, pb, VIEW_W, VIEW_H) == 0);

    SDL_free(hp);
    SDL_free(pb);
    SDL_free(pr);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_view_destroy(bg);
    emb_view_destroy(rg);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(mipmaps_owned_texture)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, EMB_VIEW_MIPMAPS, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, v));

    /* Level 0 is the picture; level 1 is its half size, a box filter of it. */
    uint8_t *l0 = readback_mip(h.dev, emb_view_texture(v), VIEW_W, VIEW_H, 0);
    uint8_t *l1 = readback_mip(h.dev, emb_view_texture(v), VIEW_W / 2, VIEW_H / 2, 1);
    EMB_REQUIRE(l0 != nullptr && l1 != nullptr);
    EMB_CHECK(count_not_sentinel(l1, VIEW_W / 2, VIEW_H / 2) > 0);

    double sum0[4] = {0, 0, 0, 0}, sum1[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < (size_t)VIEW_W * VIEW_H; i++) {
        for (int c = 0; c < 4; c++) {
            sum0[c] += l0[i * 4u + (size_t)c];
        }
    }
    for (size_t i = 0; i < (size_t)(VIEW_W / 2) * (VIEW_H / 2); i++) {
        for (int c = 0; c < 4; c++) {
            sum1[c] += l1[i * 4u + (size_t)c];
        }
    }
    bool close = true;
    for (int c = 0; c < 4; c++) {
        const double a0 = sum0[c] / (VIEW_W * VIEW_H);
        const double a1 = sum1[c] / ((VIEW_W / 2) * (VIEW_H / 2));
        close = close && (a0 - a1 < 4.0 && a1 - a0 < 4.0);
    }
    EMB_CHECK(close);

    SDL_free(l0);
    SDL_free(l1);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(no_texture_view)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, EMB_VIEW_NO_TEXTURE, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(emb_view_texture(v) == nullptr);
    EMB_CHECK(!render_now(h.dev, v));

    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 50, 60, 0));
    uint8_t *hp = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(hp != nullptr);

    /* The rectangle equals the same page in an ordinary view. */
    emb_view *ref = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(ref != nullptr);
    EMB_REQUIRE(emb_view_load_html(ref, k_page_red, strlen(k_page_red), nullptr));
    EMB_CHECK(render_now(h.dev, ref));
    uint8_t *rp = readback(h.dev, emb_view_texture(ref), VIEW_W, VIEW_H);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rect_mismatch(hp, HOST_W, 50, 60, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(hp);
    emb_view_destroy(ref);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(bad_targets_rejected)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);
    uint8_t *before = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(before != nullptr);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));

    /* Past the right edge, past the bottom, a negative origin, a null texture, and a target too small to hold the flags. */
    EMB_CHECK(!render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, HOST_W - VIEW_W + 1, 0, 0));
    EMB_CHECK(!render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 0, HOST_H - VIEW_H + 1, 0));
    EMB_CHECK(!render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, -1, 0, 0));
    EMB_CHECK(!render_to_now(h.dev, v, nullptr, HOST_W, HOST_H, 0, 0, 0));
    {
        emb_render_target small;
        memset(&small, 0, sizeof small);
        small.size = 4;
        small.texture = host_tex;
        small.texture_w = HOST_W;
        small.texture_h = HOST_H;
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(h.dev);
        EMB_REQUIRE(cmd != nullptr);
        EMB_CHECK(!emb_view_render_to(v, cmd, &small));
        submit_wait(h.dev, cmd);
    }

    uint8_t *after = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(after != nullptr);
    EMB_CHECK(memcmp(before, after, (size_t)HOST_W * HOST_H * 4u) == 0);

    SDL_free(after);
    SDL_free(before);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* The number of pixels outside the rectangle (x, y, w, h) of the tw x th picture px that are not the sentinel. */
static size_t outside_not_sentinel(const uint8_t *px, uint32_t tw, uint32_t th, int32_t x, int32_t y, uint32_t w,
                                   uint32_t h)
{
    size_t n = 0;
    for (uint32_t r = 0; r < th; r++) {
        for (uint32_t c = 0; c < tw; c++) {
            const bool inside = (int32_t)c >= x && (int32_t)c < x + (int32_t)w && (int32_t)r >= y && (int32_t)r < y + (int32_t)h;
            if (!inside) {
                n += count_not_sentinel(px + (((size_t)r * tw) + c) * 4u, 1, 1);
            }
        }
    }
    return n;
}

/* A tightly packed copy of the rectangle (x, y, w, h) of the tw-wide picture px (SDL_free). */
static uint8_t *crop_rect(const uint8_t *px, uint32_t tw, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    uint8_t *out = SDL_malloc((size_t)w * h * 4u);
    if (out == nullptr) {
        return nullptr;
    }
    for (uint32_t r = 0; r < h; r++) {
        SDL_memcpy(out + (size_t)r * w * 4u, px + (((size_t)(y + (int32_t)r) * tw) + (size_t)x) * 4u, (size_t)w * 4u);
    }
    return out;
}

/* Draws html in a fresh ordinary view (no rectangle, no preserved state) with emb_view_render and reads its picture back
 * (SDL_free). The device size goes to ow, oh. nullptr on failure. */
static uint8_t *draw_ref(emb_runtime *rt, SDL_GPUDevice *dev, const char *html, uint32_t w, uint32_t h, float scale,
                         float raster, uint32_t *ow, uint32_t *oh)
{
    emb_view *v = open_view(rt, dev, w, h, scale, 0, 0);
    if (v == nullptr) {
        return nullptr;
    }
    emb_view_set_raster_scale(v, raster);
    uint8_t *px = nullptr;
    if (emb_view_load_html(v, html, strlen(html), nullptr) && render_now(dev, v)) {
        emb_view_device_size(v, ow, oh);
        px = readback(dev, emb_view_texture(v), *ow, *oh);
    }
    emb_view_destroy(v);
    return px;
}

/* A page whose clipped box overflows its rounded overflow:hidden parent, with text inside the clip. */
static const char k_page_clip[] =
    "<!doctype html><title>C</title><style>body{margin:0;background:#fff}"
    ".clip{position:absolute;left:12px;top:10px;width:70px;height:50px;overflow:hidden;border-radius:16px;background:#ffe082}"
    ".big{position:absolute;left:-20px;top:-15px;width:120px;height:90px;background:#43a047}"
    ".t{position:absolute;left:4px;top:20px;font:14px sans-serif;color:#102}"
    "</style><div class=clip><div class=big></div><div class=t>Clipped text</div></div>";

/* Layer composites: multiply over a gradient, backdrop blur over text, an opacity group and a drop-shadow. */
static const char k_page_blend[] =
    "<!doctype html><title>B</title><style>body{margin:0;background:#fff}"
    ".grad{position:absolute;left:0;top:0;width:100px;height:80px;background:linear-gradient(90deg,#ff6f00,#3949ab)}"
    ".mul{position:absolute;left:8px;top:8px;width:40px;height:30px;background:#e91e63;mix-blend-mode:multiply}"
    ".txt{position:absolute;left:4px;top:50px;font:16px sans-serif;color:#000}"
    ".bd{position:absolute;left:30px;top:44px;width:50px;height:20px;backdrop-filter:blur(4px);background:rgba(255,255,255,.2)}"
    ".grp{position:absolute;left:60px;top:10px;opacity:.5}"
    ".grp div{width:30px;height:30px;background:#00acc1}"
    ".ds{position:absolute;left:10px;top:60px;width:20px;height:12px;background:#7b1fa2;filter:drop-shadow(3px 3px 2px #000)}"
    "</style><div class=grad></div><div class=mul></div><div class=txt>Backdrop text</div>"
    "<div class=bd></div><div class=grp><div></div></div><div class=ds></div>";

/* An opaque page canvas (the default view) under a difference blend group. */
static const char k_page_canvas[] =
    "<!doctype html><title>U</title><style>body{margin:0;background:#f5f5f5}"
    ".a{position:absolute;left:6px;top:6px;width:50px;height:40px;background:#ff9800}"
    ".d{position:absolute;left:20px;top:20px;width:60px;height:40px;background:#2196f3;mix-blend-mode:difference}"
    ".t{position:absolute;left:8px;top:60px;font:14px sans-serif;color:#000}"
    "</style><div class=a></div><div class=d></div><div class=t>Canvas under</div>";

/* A page with a 10x10 corner element at the view's bottom-right; the color is the only difference between the two. */
#define CORNER_PAGE(COLOR)                                                                                             \
    "<!doctype html><title>K</title><style>body{margin:0;background:#fff}"                                             \
    ".box{position:absolute;left:10px;top:8px;width:56px;height:30px;background:#e53935}"                              \
    "#c{position:absolute;right:0;bottom:0;width:10px;height:10px;background:" COLOR "}"                               \
    ".t{position:absolute;left:6px;top:52px;font:14px sans-serif;color:#222}"                                          \
    "</style><div class=box></div><div id=c></div><div class=t>Render me</div>"
static const char k_corner_green[] = CORNER_PAGE("#00c853");
static const char k_corner_purple[] = CORNER_PAGE("#d500f9");
static const char k_eval_purple[] = "document.getElementById('c').style.background = '#d500f9';";

EMB_TEST(offset_clip_overflow_radius)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_clip, strlen(k_page_clip), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 37, 21, 0));
    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);

    /* Nothing outside the rectangle was written. */
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 37, 21, VIEW_W, VIEW_H) == 0);

    /* The rectangle equals the same page drawn whole. The clip subtracts the frame origin, so any offset error shows here. */
    uint32_t rw = 0, rh = 0;
    uint8_t *rp = draw_ref(rt, h.dev, k_page_clip, VIEW_W, VIEW_H, 1.0f, 1.0f, &rw, &rh);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rw == VIEW_W && rh == VIEW_H);
    EMB_CHECK(rect_mismatch(px, HOST_W, 37, 21, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(offset_blend_and_backdrop)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_blend, strlen(k_page_blend), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 23, 17, 0));
    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 23, 17, VIEW_W, VIEW_H) == 0);

    /* The copies from the target read the rectangle's own pixels, so the picture equals the whole-view draw. */
    uint32_t rw = 0, rh = 0;
    uint8_t *rp = draw_ref(rt, h.dev, k_page_blend, VIEW_W, VIEW_H, 1.0f, 1.0f, &rw, &rh);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rw == VIEW_W && rh == VIEW_H);
    EMB_CHECK(rect_mismatch(px, HOST_W, 23, 17, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(offset_canvas_under)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    /* The default view is opaque, so the page canvas is painted under the blend group. */
    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_canvas, strlen(k_page_canvas), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 41, 13, 0));
    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 41, 13, VIEW_W, VIEW_H) == 0);

    uint32_t rw = 0, rh = 0;
    uint8_t *rp = draw_ref(rt, h.dev, k_page_canvas, VIEW_W, VIEW_H, 1.0f, 1.0f, &rw, &rh);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rw == VIEW_W && rh == VIEW_H);
    EMB_CHECK(rect_mismatch(px, HOST_W, 41, 13, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(partial_damage_at_rect_edge)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_corner_green, strlen(k_corner_green), nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 50, 40, EMB_RENDER_PRESERVED));

    /* The bottom-right 10x10 element changes color; the rectangle's bottom-right corner is at (149, 119). */
    EMB_CHECK(emb_view_eval(v, k_eval_purple, strlen(k_eval_purple), nullptr, nullptr));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 50, 40, EMB_RENDER_PRESERVED));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(!st.full_redraw);
    EMB_CHECK(st.damage_rects > 0);

    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);

    /* The sentinel holds outside the rectangle after the partial frame, in particular the row just below (y = 120) and
     * the column just right (x = 150) of it. */
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 50, 40, VIEW_W, VIEW_H) == 0);
    size_t edge = 0;
    for (uint32_t c = 0; c < HOST_W; c++) {
        edge += count_not_sentinel(px + (((size_t)120 * HOST_W) + c) * 4u, 1, 1);
    }
    for (uint32_t r = 0; r < HOST_H; r++) {
        edge += count_not_sentinel(px + (((size_t)r * HOST_W) + 150) * 4u, 1, 1);
    }
    EMB_CHECK(edge == 0);

    /* The rectangle equals a fresh full draw of the changed page. */
    uint32_t rw = 0, rh = 0;
    uint8_t *rp = draw_ref(rt, h.dev, k_corner_purple, VIEW_W, VIEW_H, 1.0f, 1.0f, &rw, &rh);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rect_mismatch(px, HOST_W, 50, 40, rp, VIEW_W, VIEW_H) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(switch_render_and_render_to)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));

    /* The view's own frame, then a rectangle, then a DOM change and the own frame again. */
    EMB_CHECK(render_now(h.dev, v));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 20, 30, EMB_RENDER_PRESERVED));
    EMB_CHECK(emb_view_eval(v, k_eval_blue, strlen(k_eval_blue), nullptr, nullptr));
    EMB_CHECK(render_now(h.dev, v));

    uint32_t bw = 0, bh = 0;
    uint8_t *bp = draw_ref(rt, h.dev, k_page_blue, VIEW_W, VIEW_H, 1.0f, 1.0f, &bw, &bh);
    EMB_REQUIRE(bp != nullptr);
    uint8_t *own = readback(h.dev, emb_view_texture(v), VIEW_W, VIEW_H);
    EMB_REQUIRE(own != nullptr);
    EMB_CHECK(rect_mismatch(own, VIEW_W, 0, 0, bp, VIEW_W, VIEW_H) == 0);
    SDL_free(own);

    /* The rectangle again at the same place: it equals the reference, and the sentinel holds outside it. */
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 20, 30, EMB_RENDER_PRESERVED));
    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(rect_mismatch(px, HOST_W, 20, 30, bp, VIEW_W, VIEW_H) == 0);
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 20, 30, VIEW_W, VIEW_H) == 0);

    SDL_free(px);
    SDL_free(bp);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(preserved_after_move_or_raster)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);

    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, 0, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));

    /* A move to another rectangle is a different rectangle: it is drawn whole, and the first one still holds the picture. */
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 10, 10, EMB_RENDER_PRESERVED));
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 200, 10, EMB_RENDER_PRESERVED));
    uint32_t rw = 0, rh = 0;
    uint8_t *ref = draw_ref(rt, h.dev, k_page_red, VIEW_W, VIEW_H, 1.0f, 1.0f, &rw, &rh);
    EMB_REQUIRE(ref != nullptr);
    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(rect_mismatch(px, HOST_W, 200, 10, ref, VIEW_W, VIEW_H) == 0);
    EMB_CHECK(rect_mismatch(px, HOST_W, 10, 10, ref, VIEW_W, VIEW_H) == 0);
    SDL_free(px);
    SDL_free(ref);

    /* A raster change at the same place: the rectangle is drawn whole at the new size. The sentinel is restored first, so
     * any pixel the partial path left stale would show. */
    clear_sentinel(h.dev, host_tex);
    emb_view_set_raster_scale(v, 1.5f);
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 120, 60, EMB_RENDER_PRESERVED));
    uint32_t dw = 0, dh = 0;
    emb_view_device_size(v, &dw, &dh);
    EMB_CHECK(dw == 150 && dh == 120);
    px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    uint8_t *cut = crop_rect(px, HOST_W, 120, 60, dw, dh);
    EMB_REQUIRE(cut != nullptr);
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 120, 60, dw, dh) == 0);

    /* Compared with an ordinary view at scale 1.5, within the tolerance raster_scale_matches_scale uses. */
    uint32_t sw = 0, sh = 0;
    uint8_t *sp = draw_ref(rt, h.dev, k_page_red, VIEW_W, VIEW_H, 1.5f, 1.0f, &sw, &sh);
    EMB_REQUIRE(sp != nullptr);
    EMB_CHECK(sw == dw && sh == dh);
    EMB_CHECK(close_pictures(cut, sp, dw, dh));

    SDL_free(sp);
    SDL_free(cut);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
}

#define NO_TEX_W 120
#define NO_TEX_H 70

EMB_TEST(no_texture_raster_resize)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h.dev, VIEW_W, VIEW_H, 1.0f, EMB_VIEW_NO_TEXTURE, 0);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_page_red, strlen(k_page_red), nullptr));
    emb_view_set_raster_scale(v, 2.0f);
    emb_view_resize(v, NO_TEX_W, NO_TEX_H);

    /* The device size follows the resize and the raster scale: 2 x 120 by 2 x 70. */
    uint32_t dw = 0, dh = 0;
    emb_view_device_size(v, &dw, &dh);
    EMB_CHECK(dw == 2 * NO_TEX_W && dh == 2 * NO_TEX_H);

    SDL_GPUTexture *host_tex = make_tex(h.dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, HOST_W, HOST_H, 1);
    EMB_REQUIRE(host_tex != nullptr);
    clear_sentinel(h.dev, host_tex);
    EMB_CHECK(render_to_now(h.dev, v, host_tex, HOST_W, HOST_H, 30, 20, 0));
    EMB_CHECK(emb_view_texture(v) == nullptr);
    EMB_CHECK(!render_now(h.dev, v));

    uint8_t *px = readback(h.dev, host_tex, HOST_W, HOST_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(outside_not_sentinel(px, HOST_W, HOST_H, 30, 20, dw, dh) == 0);

    /* The rectangle equals an ordinary view of the same size with the same raster scale. */
    uint32_t rw = 0, rh = 0;
    uint8_t *rp = draw_ref(rt, h.dev, k_page_red, NO_TEX_W, NO_TEX_H, 1.0f, 2.0f, &rw, &rh);
    EMB_REQUIRE(rp != nullptr);
    EMB_CHECK(rw == dw && rh == dh);
    EMB_CHECK(rect_mismatch(px, HOST_W, 30, 20, rp, rw, rh) == 0);

    SDL_free(rp);
    SDL_free(px);
    emb_view_destroy(v);
    SDL_ReleaseGPUTexture(h.dev, host_tex);
    emb_runtime_destroy(rt);
    host_close(&h);
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
