/*
 * Views of the embedding layer (W4e) in offscreen mode: a host device, one frame per update, the golden image of a small
 * page, the no-change frame that records nothing, resize and scale, the layer of an opacity group (and none for a group at
 * opacity 0), the scroll clamp, the
 * nested scroll containers, the title, a page loaded through the file loader, and the create and destroy leak check.
 * Also: the canvas color of the root's used scheme, a page with a stylesheet link (plain and media-wrapped) and images
 * served by a handler scheme and by the file loader, partial redraws compared pixel for pixel with a fresh full redraw
 * (with an opacity layer, at scale one and two), and a stats struct from an older caller.
 * Without a GPU device the tests skip.
 */
#include "emb_test.h"
#include "emb_gold.h"

#include "embed/embed_int.h"
#include "style/style.h"

#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

/* The page of the golden image: solid boxes, an opacity group with a border (two overlapping items, so it needs a layer),
 * and a margin box. No text, so the picture does not depend on glyph rasterization. */
#define GOLD_W 128
#define GOLD_H 96
static const char k_gold_page[] =
    "<!doctype html><title>Golden</title><style>"
    "body{margin:0;background:#fff}"
    ".a{position:absolute;left:8px;top:8px;width:48px;height:32px;background:#e53935}"
    ".b{position:absolute;left:40px;top:24px;width:48px;height:32px;background:#1e88e5;opacity:.5;"
    "border:2px solid #000;border-radius:6px}"
    ".c{margin:8px;width:64px;height:16px;border:4px solid #43a047}"
    "</style><div class=a></div><div class=b></div><div class=c></div>";

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

EMB_TEST(embed_offscreen_page_matches_golden)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    uint8_t *px = readback(h.dev, emb_view_texture(v), GOLD_W, GOLD_H);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(emb_gold_check("embed_offscreen_page", px, GOLD_W, GOLD_H));
    SDL_free(px);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* FP2b: the view target is made by a thread while the document builds, and the UA sheet is shared. Destroying the view
 * right after creating it (before any load or frame) joins every thread and returns the memory; two views of one runtime
 * share one UA sheet; the target has the right size and format. */
EMB_TEST(embed_view_created_and_destroyed_before_first_paint_is_clean)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *warm_rt = emb_runtime_create(nullptr);
    emb_view *warm = open_view(warm_rt, h.dev, 64, 48, 1.0f);
    EMB_REQUIRE(warm != nullptr);
    emb_view_destroy(warm);
    emb_runtime_destroy(warm_rt);
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        emb_runtime *rt = emb_runtime_create(nullptr);
        EMB_REQUIRE(rt != nullptr);
        emb_view *v = open_view(rt, h.dev, 96, 64, 1.0f);
        EMB_REQUIRE(v != nullptr);
        EMB_CHECK(emb_view_texture(v) != nullptr);
        emb_view_destroy(v);
        emb_runtime_destroy(rt);
    }
    EMB_CHECK(embi_mem_live() == base);
    EMB_CHECK(embi_style_ua_users() == 0);
    host_close(&h);
}

EMB_TEST(embed_two_views_share_one_ua_sheet)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *a = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    emb_view *b = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(a != nullptr && b != nullptr);
    EMB_CHECK(emb_view_load_html(a, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(emb_view_load_html(b, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, a));
    EMB_CHECK(render_now(h.dev, b));
    /* One user for each view's engine, and one for the runtime's prefetch thread (which may not have run yet). */
    EMB_CHECK(embi_style_ua_users() >= 2 && embi_style_ua_users() <= 3);
    uint8_t *pa = readback(h.dev, emb_view_texture(a), GOLD_W, GOLD_H);
    uint8_t *pb = readback(h.dev, emb_view_texture(b), GOLD_W, GOLD_H);
    EMB_REQUIRE(pa != nullptr && pb != nullptr);
    EMB_CHECK(memcmp(pa, pb, (size_t)GOLD_W * GOLD_H * 4u) == 0);
    SDL_free(pa);
    SDL_free(pb);
    emb_view_destroy(a); /* the first view goes while the second still uses the shared sheet */
    (void)render_now(h.dev, b);
    emb_view_destroy(b);
    emb_runtime_destroy(rt);
    EMB_CHECK(embi_style_ua_users() == 0);
    host_close(&h);
}

EMB_TEST(embed_no_change_frame_records_nothing)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    uint8_t *before = readback(h.dev, emb_view_texture(v), GOLD_W, GOLD_H);
    EMB_REQUIRE(before != nullptr);
    EMB_CHECK(!render_now(h.dev, v));
    /* The texture is untouched by the second frame. */
    uint8_t *after = readback(h.dev, emb_view_texture(v), GOLD_W, GOLD_H);
    EMB_REQUIRE(after != nullptr);
    EMB_CHECK(memcmp(before, after, (size_t)GOLD_W * GOLD_H * 4u) == 0);
    SDL_free(before);
    SDL_free(after);
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.draw_calls == 0);
    EMB_CHECK(st.layers == 0);
    EMB_CHECK(st.damage_rects == 0);
    EMB_CHECK(!st.full_redraw);
    EMB_CHECK(st.items > 0);
    EMB_CHECK(st.size == sizeof st);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_opacity_group_needs_a_layer)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    /* The .b box is an opacity group whose background and border overlap: one intermediate layer. */
    EMB_CHECK(st.layers >= 1);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* Checked checkboxes (their check mark cannot fold into the group's alpha) and an overlapping background and border, in
 * groups of opacity OPACITY: TodoMVC's hidden toggles. */
#define CLK_PAGE(OPACITY)                                                                                              \
    "<!doctype html><style>body{margin:0;background:#fff}input{margin:2px;opacity:" OPACITY "}"                        \
    ".b{width:40px;height:20px;background:#1e88e5;border:2px solid #000;opacity:" OPACITY "}</style>"                  \
    "<input type=checkbox checked><input type=checkbox checked><input type=checkbox checked>"                          \
    "<input type=checkbox checked><input type=checkbox checked><input type=checkbox checked>"                          \
    "<input type=checkbox checked><input type=checkbox checked><div class=b></div>"

static emb_view_stats clk_render_page(host *h, emb_runtime *rt, const char *html, uint8_t **px)
{
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    *px = nullptr;
    emb_view *v = open_view(rt, h->dev, GOLD_W, GOLD_H, 1.0f);
    if (v == nullptr || !emb_view_load_html(v, html, strlen(html), nullptr) || !render_now(h->dev, v)) {
        emb_view_destroy(v);
        return st;
    }
    emb_view_stats_get(v, &st);
    *px = readback(h->dev, emb_view_texture(v), GOLD_W, GOLD_H);
    emb_view_destroy(v);
    return st;
}

/* A group at opacity 0 shows nothing and gets no layer (the click frame of TodoMVC's toggle-all made 101 layers, which took
 * about 800 ms on Direct3D 12). The same page at opacity .5 does need layers, so the check is not vacuous. */
EMB_TEST(embed_opacity_zero_group_gets_no_layer)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    uint8_t *px = nullptr;
    const emb_view_stats half = clk_render_page(&h, rt, CLK_PAGE(".5"), &px);
    EMB_CHECK(half.layers >= 2);
    SDL_free(px);
    const emb_view_stats zero = clk_render_page(&h, rt, CLK_PAGE("0"), &px);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(zero.items > 0);
    EMB_CHECK(zero.layers == 0);
    EMB_CHECK(zero.draw_calls == 1);
    bool white = true;
    for (size_t i = 0; i < (size_t)GOLD_W * GOLD_H * 4u && white; i++) {
        white = px[i] == 0xFF;
    }
    EMB_CHECK(white);
    SDL_free(px);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_resize_and_scale_repaint_the_whole_view)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));

    emb_view_resize(v, 100, 80);
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.full_redraw);

    emb_view_set_scale(v, 2.0f);
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.full_redraw);
    EMB_CHECK(v->dev_w == 200 && v->dev_h == 160);
    /* The .a box (#e53935) starts at css (8, 8): device (16, 16) at scale 2. Pixel (20, 20) is inside it. */
    uint8_t *px = readback(h.dev, emb_view_texture(v), 200, 160);
    EMB_REQUIRE(px != nullptr);
    const size_t at = (size_t)(20 * 200 + 20) * 4u;
    EMB_CHECK(px[at + 0] == 0xE5 && px[at + 1] == 0x39 && px[at + 2] == 0x35 && px[at + 3] == 0xFF);
    SDL_free(px);

    /* A scheme change repaints too (the page sets its own white body, so the frame is the new picture of the same page). */
    emb_view_set_color_scheme(v, EMB_SCHEME_DARK);
    EMB_CHECK(render_now(h.dev, v));
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_scroll_clamps_to_the_overflow)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 800, 600, 1.0f);
    EMB_REQUIRE(v != nullptr);
    const char *page =
        "<!doctype html><style>body{margin:0}.tall{height:2000px;background:#ccc}</style><div class=tall></div>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(render_now(h.dev, v));

    emb_view_scroll_by(v, 0.0f, 500.0f);
    EMB_CHECK(render_now(h.dev, v));
    float x = 0.0f, y = 0.0f;
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(y > 499.0f && y < 501.0f);
    /* Far past the end: the offset stops at the overflow (2000 - 600). */
    emb_view_scroll_by(v, 0.0f, 1.0e6f);
    EMB_CHECK(render_now(h.dev, v));
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(y > 1399.0f && y < 1401.0f);
    /* Back to the top. */
    emb_view_scroll_by(v, 0.0f, -1.0e6f);
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(y == 0.0f);
    /* The scroll made a bar visible, which fades out over time: it is animating now. */
    EMB_CHECK(embi_view_animating(v));
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_wheel_goes_to_the_innermost_container)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 800, 600, 1.0f);
    EMB_REQUIRE(v != nullptr);
    /* A 100 px box with its own scroll at the top left, and a tall page. */
    const char *page =
        "<!doctype html><style>body{margin:0}"
        "#in{width:200px;height:100px;overflow:auto}#in div{height:500px}"
        ".tall{height:2000px}</style>"
        "<div id=in><div></div></div><div class=tall></div>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(render_now(h.dev, v));

    /* Over the container: the container moves (its key is the one the view tracked) and the viewport stays. */
    emb_view_wheel(v, 10.0f, 10.0f, 0.0f, 50.0f);
    EMB_CHECK(render_now(h.dev, v));
    EMB_REQUIRE(v->ntracks >= 1);
    float x = 0.0f, y = 0.0f;
    EMB_CHECK(embi_doc_scroller_offset(v->doc, v->tracks[0].key, &x, &y));
    EMB_CHECK(y > 49.0f && y < 51.0f);
    EMB_CHECK(x == 0.0f);
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(y == 0.0f);

    /* Outside the container: the viewport moves. */
    emb_view_wheel(v, 700.0f, 500.0f, 0.0f, 50.0f);
    EMB_CHECK(render_now(h.dev, v));
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(y > 49.0f && y < 51.0f);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_wheel_horizontal_positive_dx_scrolls_right)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 400, 300, 1.0f);
    EMB_REQUIRE(v != nullptr);
    const char *page = "<!doctype html><style>body{margin:0}.wide{width:2000px;height:100px;background:#369}</style>"
                       "<div class=wide></div>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    float x = 0.0f, y = 0.0f;
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(x == 0.0f);
    /* Positive dx is the page moving left, which is scrollLeft growing. */
    emb_view_wheel(v, 100.0f, 50.0f, 40.0f, 0.0f);
    EMB_CHECK(render_now(h.dev, v));
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(x > 39.0f && x < 41.0f);
    emb_view_wheel(v, 100.0f, 50.0f, -15.0f, 0.0f);
    EMB_CHECK(render_now(h.dev, v));
    embi_doc_root_offset(v->doc, &x, &y);
    EMB_CHECK(x > 24.0f && x < 26.0f);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_title_and_stylesheet_of_a_document)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 200, 100, 1.0f);
    EMB_REQUIRE(v != nullptr);
    const char *page =
        "<!doctype html><title>Hello there</title>"
        "<style>div{width:50px;height:10px;background:#f00}</style><div></div>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(strcmp(emb_view_title(v), "Hello there") == 0);
    EMB_CHECK(render_now(h.dev, v));
    /* The <style> block applied: the red box is there. The body margin (8px) puts it at (8, 8), 50 x 10. */
    uint8_t *px = readback(h.dev, emb_view_texture(v), 200, 100);
    EMB_REQUIRE(px != nullptr);
    const size_t at = (size_t)(10 * 200 + 10) * 4u;
    EMB_CHECK(px[at + 0] == 0xFF && px[at + 1] == 0x00 && px[at + 2] == 0x00 && px[at + 3] == 0xFF);
    SDL_free(px);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_title_is_collapsed_and_media_and_disabled_sheets_are_honored)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 200, 100, 1.0f);
    EMB_REQUIRE(v != nullptr);
    /* The title's runs of white space collapse to one space and its ends are trimmed. */
    const char *page =
        "<!doctype html><title>  Hello \n\t  there  </title>"
        "<style media='(min-width: 100px)'>div{width:50px;height:10px;background:#f00}</style>"
        "<style media=print>div{background:#00f}</style>"
        "<style media=speech>div{background:#0f0}</style>"
        "<style type=text/x-other>div{background:#0f0}</style>"
        "<style>@media (min-width:9000px){div{background:#0f0}}</style>"
        "<div></div>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(strcmp(emb_view_title(v), "Hello there") == 0);
    EMB_CHECK(render_now(h.dev, v));
    /* Only the matching screen sheet applies: the box is red (not the print blue, the speech or other-type green). */
    uint8_t *px = readback(h.dev, emb_view_texture(v), 200, 100);
    EMB_REQUIRE(px != nullptr);
    const size_t at = (size_t)(10 * 200 + 10) * 4u;
    EMB_CHECK(px[at + 0] == 0xFF && px[at + 1] == 0x00 && px[at + 2] == 0x00);
    SDL_free(px);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* A decoration line thinner than a device px (and dashed or dotted) used to loop forever: this page must finish. */
EMB_TEST(embed_tiny_decoration_thickness_terminates)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, 400, 100, 1.0f);
    EMB_REQUIRE(v != nullptr);
    const char *page =
        "<!doctype html><body style=\"margin:0\">"
        "<p style=\"text-decoration:underline dotted;text-decoration-thickness:0.00001px\">dotted text</p>"
        "<p style=\"text-decoration:underline dashed;text-decoration-thickness:0.0001px\">dashed text</p>"
        "<p style=\"text-decoration:line-through dotted;text-decoration-thickness:1e-30px\">more</p>"
        "</body>";
    EMB_REQUIRE(emb_view_load_html(v, page, strlen(page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.items > 0);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

EMB_TEST(embed_loads_a_page_through_the_file_loader)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/tests/pages", EMBER_SOURCE_DIR);
    EMB_REQUIRE(emb_runtime_allow_path(rt, dir));
    emb_view *v = open_view(rt, h.dev, 800, 600, 1.0f);
    EMB_REQUIRE(v != nullptr);
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/tests/pages/static-todomvc.html", EMBER_SOURCE_DIR);
    EMB_CHECK(emb_view_load_url(v, path));
    EMB_CHECK(strcmp(emb_view_title(v), "Ember \xE2\x80\xA2 TodoMVC (static)") == 0);
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.items > 0);
    EMB_CHECK(st.display_list_ms >= 0.0);

    /* A file outside the allowed directory does not load, and the view keeps its page. */
    char outside[1024];
    SDL_snprintf(outside, sizeof outside, "%s/assets/fonts/OFL.txt", EMBER_SOURCE_DIR);
    EMB_CHECK(!emb_view_load_url(v, outside));
    EMB_CHECK(strcmp(emb_view_title(v), "Ember \xE2\x80\xA2 TodoMVC (static)") == 0);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* The site page, its stylesheets and images are defined further down; the baseline test uses them through this. */
static void load_site_twice(emb_runtime *rt, host *h);

EMB_TEST(embed_create_destroy_returns_to_baseline)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    /* One warm-up view: the pipelines and the shared tables are created once. */
    emb_view *warm = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(warm != nullptr);
    EMB_REQUIRE(emb_view_load_html(warm, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, warm));
    emb_view_destroy(warm);
    load_site_twice(rt, &h);
    size_t base = embi_mem_live();
    for (int i = 0; i < 2; i++) {
        emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
        EMB_REQUIRE(v != nullptr);
        EMB_REQUIRE(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
        EMB_CHECK(render_now(h.dev, v));
        emb_view_destroy(v);
    }
    EMB_CHECK(embi_mem_live() == base);
    /* A page with stylesheets, a media sheet and images from the handler, loaded twice (the image cache is reset on the
     * second load) and destroyed. */
    for (int i = 0; i < 2; i++) {
        load_site_twice(rt, &h);
    }
    EMB_CHECK(embi_mem_live() == base);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* ------------------------------------------------------------------ canvas color of the root's used scheme */

static bool pixel_is(const uint8_t *px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t *p = px + ((size_t)y * w + x) * 4u;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 0xFF;
}

/* Renders html in a w x hh view of the scheme and returns the readback (SDL_free), or nullptr. */
static uint8_t *render_page(host *h, emb_runtime *rt, const char *html, uint32_t w, uint32_t hh, emb_color_scheme scheme)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = w;
    d.height = hh;
    d.scale = 1.0f;
    d.device = h->dev;
    d.scheme = scheme;
    emb_view *v = emb_view_create(rt, &d);
    if (v == nullptr || !emb_view_load_html(v, html, strlen(html), nullptr)) {
        emb_view_destroy(v);
        return nullptr;
    }
    render_now(h->dev, v);
    uint8_t *px = readback(h->dev, emb_view_texture(v), w, hh);
    emb_view_destroy(v);
    return px;
}

EMB_TEST(embed_dark_scheme_canvas_follows_the_root_color_scheme)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    /* A page that does not opt in to dark keeps the white canvas under a dark preference (black text on it stays readable). */
    uint8_t *plain = render_page(&h, rt, "<p>x</p>", 64, 48, EMB_SCHEME_DARK);
    EMB_REQUIRE(plain != nullptr);
    EMB_CHECK(pixel_is(plain, 64, 63, 47, 0xFF, 0xFF, 0xFF));
    SDL_free(plain);
    /* A root that lists dark gets the dark Canvas color. */
    uint8_t *dark = render_page(&h, rt, "<html style=\"color-scheme:dark\"><p>x</p>", 64, 48, EMB_SCHEME_DARK);
    EMB_REQUIRE(dark != nullptr);
    const uint8_t *c = dark + ((size_t)47 * 64 + 63) * 4u;
    EMB_CHECK(c[0] < 0x40 && c[1] < 0x40 && c[2] < 0x40 && c[3] == 0xFF);
    SDL_free(dark);
    /* A page that supports only dark is dark whatever the preference; one that lists both follows the preference. */
    uint8_t *only = render_page(&h, rt, "<html style=\"color-scheme:dark\"><p>x</p>", 64, 48, EMB_SCHEME_LIGHT);
    EMB_REQUIRE(only != nullptr);
    EMB_CHECK(only[(size_t)(47 * 64 + 63) * 4u] < 0x40);
    SDL_free(only);
    uint8_t *light = render_page(&h, rt, "<html style=\"color-scheme:light dark\"><p>x</p>", 64, 48, EMB_SCHEME_LIGHT);
    EMB_REQUIRE(light != nullptr);
    EMB_CHECK(pixel_is(light, 64, 63, 47, 0xFF, 0xFF, 0xFF));
    SDL_free(light);
    /* `light dark` follows the preference. */
    uint8_t *both = render_page(&h, rt, "<html style=\"color-scheme:light dark\"><p>x</p>", 64, 48, EMB_SCHEME_DARK);
    EMB_REQUIRE(both != nullptr);
    c = both + ((size_t)47 * 64 + 63) * 4u;
    EMB_CHECK(c[0] < 0x40 && c[1] < 0x40 && c[2] < 0x40);
    SDL_free(both);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* ------------------------------------------------------------------ subresources: stylesheets and images */

/* A 4 x 4 opaque PNG of #00C853 (0, 200, 83). */
static const uint8_t k_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00, 0x00, 0x26, 0x93, 0x09, 0x29, 0x00, 0x00, 0x00, 0x0F, 0x49,
    0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x38, 0x11, 0x8C, 0x40, 0xC4, 0x71, 0x00, 0xC6, 0xF3, 0x11, 0xB1, 0xA3,
    0x6A, 0x73, 0x24, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
#define K_PNG_DATA_URL \
    "data:image/png;base64," \
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAD0lEQVR4nGNgOBGMQMRxAMbzEbGjanMkAAAAAElFTkSuQmCC"

/* The page, its two stylesheets (the second only for media=screen, so it goes through the media wrapper) and the image,
 * served by the handler. The sheets stack the images: without the media sheet they would sit side by side. */
static const char k_site_page[] =
    "<!doctype html><link rel=stylesheet href=\"s.css\"><link rel=stylesheet href=\"m.css\" media=\"screen\">"
    "<body style=\"margin:0\"><div class=box></div>"
    "<img width=40 height=30 src=\"" K_PNG_DATA_URL "\"><img width=40 height=30 src=\"pic.png\">";
static const char k_site_css[] = ".box{width:40px;height:20px;background:#ff0000}";
static const char k_site_media_css[] = "img{display:block}";

typedef struct site {
    int page, css, media, pic, other;
} site;

static bool site_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    site *s = user;
    (void)url_len;
    const char *tail = strrchr(url, '/');
    tail = tail != nullptr ? tail + 1 : url;
    if (strcmp(tail, "page.html") == 0) {
        s->page++;
        out->data = (const uint8_t *)k_site_page;
        out->len = sizeof k_site_page - 1;
    } else if (strcmp(tail, "s.css") == 0) {
        s->css++;
        out->data = (const uint8_t *)k_site_css;
        out->len = sizeof k_site_css - 1;
    } else if (strcmp(tail, "m.css") == 0) {
        s->media++;
        out->data = (const uint8_t *)k_site_media_css;
        out->len = sizeof k_site_media_css - 1;
    } else if (strcmp(tail, "pic.png") == 0) {
        s->pic++;
        out->data = k_png;
        out->len = sizeof k_png;
    } else {
        s->other++;
        return false;
    }
    return true;
}

static void load_site_twice(emb_runtime *rt, host *h)
{
    site s = {0, 0, 0, 0, 0};
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", site_handler, &s));
    emb_view *v = open_view(rt, h->dev, 120, 100, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h->dev, v));
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h->dev, v));
    EMB_CHECK(s.pic == 2);
    emb_view_destroy(v);
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "mem", nullptr, nullptr));
}

/* The pixel checks of the site page at 1x: red box, the two stacked images, white elsewhere. */
static void check_site_pixels(const uint8_t *px, uint32_t w)
{
    EMB_CHECK(pixel_is(px, w, 10, 10, 0xFF, 0x00, 0x00));
    EMB_CHECK(pixel_is(px, w, 20, 35, 0x00, 0xC8, 0x53)); /* the data: PNG, y 20..50 */
    EMB_CHECK(pixel_is(px, w, 20, 65, 0x00, 0xC8, 0x53)); /* the second PNG, y 50..80: below the first, not beside it */
    EMB_CHECK(pixel_is(px, w, 60, 35, 0xFF, 0xFF, 0xFF));
    EMB_CHECK(pixel_is(px, w, 60, 65, 0xFF, 0xFF, 0xFF));
}

EMB_TEST(embed_handler_scheme_serves_page_stylesheets_and_images)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    site s = {0, 0, 0, 0, 0};
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", site_handler, &s));
    emb_view *v = open_view(rt, h.dev, 120, 100, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h.dev, v));
    EMB_CHECK(s.page == 1 && s.css == 1 && s.media == 1 && s.pic == 1 && s.other == 0);
    uint8_t *px = readback(h.dev, emb_view_texture(v), 120, 100);
    EMB_REQUIRE(px != nullptr);
    check_site_pixels(px, 120);
    SDL_free(px);
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    EMB_CHECK(st.stylesheets_ms > 0.0 && st.fetch_ms >= 0.0 && st.upload_ms >= 0.0);

    /* Reloading the same URL fetches the images again (the cache is per document), and shows the same picture. */
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h.dev, v));
    EMB_CHECK(s.pic == 2);
    px = readback(h.dev, emb_view_texture(v), 120, 100);
    EMB_REQUIRE(px != nullptr);
    check_site_pixels(px, 120);
    SDL_free(px);

    /* A handler that refuses a URL fails the load of a document it serves, and the view keeps its page. */
    EMB_CHECK(!emb_view_load_url(v, "mem://site/missing.html"));
    EMB_CHECK(s.other == 1);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* Writes the site's files into dir (created) and returns the path of the page in out. */
static bool write_site(const char *dir, char *out, size_t cap)
{
    char path[1100];
    if (!SDL_CreateDirectory(dir)) {
        return false;
    }
    const struct {
        const char *name;
        const void *data;
        size_t len;
    } files[] = {
        {"page.html", k_site_page, sizeof k_site_page - 1},
        {"s.css", k_site_css, sizeof k_site_css - 1},
        {"m.css", k_site_media_css, sizeof k_site_media_css - 1},
        {"pic.png", k_png, sizeof k_png},
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        SDL_snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
        if (!SDL_SaveFile(path, files[i].data, files[i].len)) {
            return false;
        }
    }
    SDL_snprintf(out, cap, "%s/page.html", dir);
    return true;
}

static void remove_site(const char *dir)
{
    const char *names[] = {"page.html", "s.css", "m.css", "pic.png"};
    char path[1100];
    for (size_t i = 0; i < 4; i++) {
        SDL_snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        SDL_RemovePath(path);
    }
    SDL_RemovePath(dir);
}

/* The body of the file-loader test, apart from the test so that a failed requirement returns to the cleanup. */
static void file_loader_body(host *h, const char *dir, const char *page)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = open_view(rt, h->dev, 120, 100, 1.0f);
    if (v == nullptr) {
        emb_runtime_destroy(rt);
        EMB_REQUIRE(false);
    }
    /* Without the directory the page, and so its subresources, are refused. */
    EMB_CHECK(!emb_view_load_url(v, page));
    EMB_CHECK(emb_runtime_allow_path(rt, dir));
    EMB_CHECK(emb_view_load_url(v, page));
    EMB_CHECK(render_now(h->dev, v));
    uint8_t *px = readback(h->dev, emb_view_texture(v), 120, 100);
    EMB_CHECK(px != nullptr);
    if (px != nullptr) {
        check_site_pixels(px, 120);
    }
    SDL_free(px);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
}

EMB_TEST(embed_file_loader_serves_page_stylesheets_and_images)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    /* A directory of its own under the temp directory (the current directory when none is set), removed whatever the
     * outcome. */
    const char *tmp = SDL_getenv("TEMP");
    if (tmp == nullptr) {
        tmp = SDL_getenv("TMPDIR");
    }
    char *cwd = tmp == nullptr ? SDL_GetCurrentDirectory() : nullptr;
    char dir[1024];
    SDL_snprintf(dir, sizeof dir, "%s/embed_site_test_%llu", tmp != nullptr ? tmp : cwd,
                 (unsigned long long)SDL_GetPerformanceCounter());
    SDL_free(cwd);
    for (char *c = dir; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    char page[1100];
    bool wrote = write_site(dir, page, sizeof page);
    if (wrote) {
        file_loader_body(&h, dir, page);
    }
    remove_site(dir);
    EMB_REQUIRE(wrote);
    host_close(&h);
}

/* ------------------------------------------------------------------ partial redraw equals full redraw */

/* A scroll container (without a bar, so the picture does not depend on the clock) over rows of colors, a half-opaque box
 * that overlaps it (an opacity layer) and a PNG that overlaps its corner. Scrolling the container damages only its box. */
static const char k_partial_page[] =
    "<!doctype html><style>body{margin:0;background:#fff}"
    "#in{position:absolute;left:20px;top:20px;width:100px;height:80px;overflow:auto;scrollbar-width:none;background:#eee}"
    "#in div{height:20px}"
    ".o{position:absolute;left:60px;top:60px;width:80px;height:60px;background:#1e88e5;opacity:.5;border:3px solid #000}"
    "img{position:absolute;left:96px;top:70px}"
    "</style><div id=in>"
    "<div style=\"background:#e53935\"></div><div style=\"background:#fb8c00\"></div><div style=\"background:#fdd835\"></div>"
    "<div style=\"background:#43a047\"></div><div style=\"background:#1e88e5\"></div><div style=\"background:#8e24aa\"></div>"
    "<div style=\"background:#6d4c41\"></div><div style=\"background:#546e7a\"></div></div>"
    "<div class=o></div><img width=40 height=30 src=\"" K_PNG_DATA_URL "\">";

#define PART_W 200u
#define PART_H 150u

EMB_TEST(embed_partial_redraw_matches_a_fresh_full_redraw)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *a = open_view(rt, h.dev, PART_W, PART_H, 1.0f);
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(emb_view_load_html(a, k_partial_page, strlen(k_partial_page), nullptr));
    EMB_CHECK(render_now(h.dev, a));

    /* Scroll the container: only its box (and what it overlaps) changes. */
    for (int step = 0; step < 3; step++) {
        emb_view_wheel(a, 30.0f, 30.0f, 0.0f, 23.0f);
        EMB_CHECK(render_now(h.dev, a));
        emb_view_stats st;
        memset(&st, 0, sizeof st);
        st.size = sizeof st;
        emb_view_stats_get(a, &st);
        EMB_CHECK(!st.full_redraw);
        EMB_CHECK(st.damage_rects >= 1);
        EMB_CHECK(st.damage_area > 0 && st.damage_area < (uint64_t)PART_W * PART_H);
        EMB_CHECK(st.draw_calls > 0);
    }
    uint8_t *partial = readback(h.dev, emb_view_texture(a), PART_W, PART_H);
    EMB_REQUIRE(partial != nullptr);

    /* A fresh view at the same scroll state draws everything in one full frame. */
    emb_view *b = open_view(rt, h.dev, PART_W, PART_H, 1.0f);
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(emb_view_load_html(b, k_partial_page, strlen(k_partial_page), nullptr));
    emb_view_wheel(b, 30.0f, 30.0f, 0.0f, 69.0f);
    EMB_CHECK(render_now(h.dev, b));
    emb_view_stats stb;
    memset(&stb, 0, sizeof stb);
    stb.size = sizeof stb;
    emb_view_stats_get(b, &stb);
    EMB_CHECK(stb.full_redraw);
    uint8_t *full = readback(h.dev, emb_view_texture(b), PART_W, PART_H);
    EMB_REQUIRE(full != nullptr);
    EMB_CHECK(memcmp(partial, full, (size_t)PART_W * PART_H * 4u) == 0);

    /* The picture is the scrolled one and shows every kind of item: the first row has left the corner, and the PNG is on
     * top of the box's corner. */
    EMB_CHECK(!pixel_is(full, PART_W, 30, 30, 0xE5, 0x39, 0x35));
    EMB_CHECK(pixel_is(full, PART_W, 110, 80, 0x00, 0xC8, 0x53));
    SDL_free(partial);
    SDL_free(full);
    emb_view_destroy(a);
    emb_view_destroy(b);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* The same at scale two, where the damage rects are in device px and the culling margin scales. */
EMB_TEST(embed_partial_redraw_with_a_layer_at_scale_two)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *a = open_view(rt, h.dev, PART_W, PART_H, 2.0f);
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(emb_view_load_html(a, k_partial_page, strlen(k_partial_page), nullptr));
    EMB_CHECK(render_now(h.dev, a));
    emb_view_wheel(a, 30.0f, 30.0f, 0.0f, 37.0f);
    EMB_CHECK(render_now(h.dev, a));
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(a, &st);
    EMB_CHECK(!st.full_redraw);
    EMB_CHECK(st.damage_area > 0 && st.damage_area < (uint64_t)PART_W * PART_H * 4u);
    uint8_t *partial = readback(h.dev, emb_view_texture(a), PART_W * 2u, PART_H * 2u);
    EMB_REQUIRE(partial != nullptr);

    emb_view *b = open_view(rt, h.dev, PART_W, PART_H, 2.0f);
    EMB_REQUIRE(b != nullptr);
    EMB_REQUIRE(emb_view_load_html(b, k_partial_page, strlen(k_partial_page), nullptr));
    emb_view_wheel(b, 30.0f, 30.0f, 0.0f, 37.0f);
    EMB_CHECK(render_now(h.dev, b));
    uint8_t *full = readback(h.dev, emb_view_texture(b), PART_W * 2u, PART_H * 2u);
    EMB_REQUIRE(full != nullptr);
    EMB_CHECK(memcmp(partial, full, (size_t)PART_W * PART_H * 16u) == 0);
    SDL_free(partial);
    SDL_free(full);
    emb_view_destroy(a);
    emb_view_destroy(b);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* ------------------------------------------------------------------ stats and versioning */

EMB_TEST(embed_stats_get_honors_a_smaller_caller_struct)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    emb_view *v = open_view(rt, h.dev, GOLD_W, GOLD_H, 1.0f);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_gold_page, strlen(k_gold_page), nullptr));
    EMB_CHECK(render_now(h.dev, v));
    emb_view_stats full;
    memset(&full, 0, sizeof full);
    full.size = sizeof full;
    emb_view_stats_get(v, &full);
    EMB_CHECK(full.items > 0);

    /* An older caller knows only the fields before display_list_ms: those are filled, the rest of its buffer is not
     * touched. */
    struct {
        emb_view_stats s;
        unsigned char guard[16];
    } buf;
    memset(&buf, 0xAB, sizeof buf);
    const uint32_t small = (uint32_t)offsetof(emb_view_stats, display_list_ms);
    buf.s.size = small;
    emb_view_stats_get(v, &buf.s);
    EMB_CHECK(buf.s.size == small);
    EMB_CHECK(buf.s.runtime_init_ms == full.runtime_init_ms);
    EMB_CHECK(buf.s.parse_ms == full.parse_ms);
    EMB_CHECK(buf.s.layout_ms == full.layout_ms);
    EMB_CHECK(buf.s.items == 0xABABABABu);
    const unsigned char *after = (const unsigned char *)&buf.s + small;
    for (size_t i = 0; i < sizeof buf - small; i++) {
        EMB_CHECK(after[i] == 0xAB);
    }
    /* A size below the size field itself writes nothing. */
    memset(&buf, 0xAB, sizeof buf);
    buf.s.size = 2;
    emb_view_stats_get(v, &buf.s);
    EMB_CHECK(buf.s.runtime_init_ms != full.runtime_init_ms);
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* ------------------------------------------------------------------ scrolling a long page (scroll60) */

/* n rows of 20 px, each with a color and a line of text, in a document taller than the view. */
static char *tall_page(int n)
{
    size_t cap = (size_t)n * 110 + 256;
    char *s = SDL_malloc(cap);
    size_t len = (size_t)SDL_snprintf(s, cap, "<!doctype html><style>body{margin:0}div{height:20px;font:12px sans-serif}</style>");
    for (int i = 0; i < n; i++) {
        len += (size_t)SDL_snprintf(s + len, cap - len, "<div style=\"background:#%02x%02x%02x\">row number %d</div>",
                                    (i * 37) & 0xFF, 0x80 + ((i * 11) & 0x3F), 0xE0 - ((i * 5) & 0x3F), i);
    }
    return s;
}

static emb_view_stats stats_of(emb_view *v)
{
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    return st;
}

/* A scroll records the rows that are on screen, not the rows of the page: the list is reused (not built again), the items
 * outside the view are culled, the draw calls do not grow with the page, and the picture is the one a fresh view draws at the
 * same offset. */
EMB_TEST(embed_scroll_records_only_the_visible_rows_and_reuses_the_list)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    uint32_t calls[2] = {0, 0};
    uint8_t *shots[2] = {nullptr, nullptr};
    for (int k = 0; k < 2; k++) {
        const int rows = k == 0 ? 600 : 6000;
        char *html = tall_page(rows);
        emb_view *a = open_view(rt, h.dev, 200, 150, 1.0f);
        EMB_REQUIRE(a != nullptr);
        EMB_REQUIRE(emb_view_load_html(a, html, strlen(html), nullptr));
        EMB_CHECK(render_now(h.dev, a));
        emb_view_stats first = stats_of(a);
        EMB_CHECK(first.items >= (uint32_t)rows);
        EMB_CHECK(first.items_culled > (uint32_t)rows); /* the first frame culls the rows below the fold too */
        for (int step = 0; step < 4; step++) {
            emb_view_scroll_by(a, 0.0f, 333.0f);
            EMB_CHECK(render_now(h.dev, a));
            emb_view_stats st = stats_of(a);
            EMB_CHECK(st.scroll_reused);                      /* no display list was built */
            EMB_CHECK(st.items_recorded > 0 && st.items_recorded < 80);
            EMB_CHECK(st.items_culled >= (uint32_t)rows);      /* the rows off screen are not recorded */
            EMB_CHECK(st.draw_calls > 0 && st.draw_calls <= 8);
            EMB_CHECK(st.layers == 0);
            calls[k] = st.draw_calls;
        }
        shots[k] = readback(h.dev, emb_view_texture(a), 200, 150);
        EMB_REQUIRE(shots[k] != nullptr);

        /* A fresh view at the same offset builds its list there and draws it whole. */
        emb_view *b = open_view(rt, h.dev, 200, 150, 1.0f);
        EMB_REQUIRE(b != nullptr);
        EMB_REQUIRE(emb_view_load_html(b, html, strlen(html), nullptr));
        emb_view_scroll_by(b, 0.0f, 4.0f * 333.0f);
        EMB_CHECK(render_now(h.dev, b));
        EMB_CHECK(!stats_of(b).scroll_reused);
        uint8_t *fresh = readback(h.dev, emb_view_texture(b), 200, 150);
        EMB_REQUIRE(fresh != nullptr);
        EMB_CHECK(memcmp(shots[k], fresh, 200u * 150u * 4u) == 0);
        SDL_free(fresh);
        emb_view_destroy(a);
        emb_view_destroy(b);
        SDL_free(html);
    }
    /* Ten times the rows, the same work on screen. */
    EMB_CHECK(calls[1] <= calls[0] + 1);
    SDL_free(shots[0]);
    SDL_free(shots[1]);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* A scroll back and forth ends where it started: the picture is the first one again (the offsets are translations, so the
 * round trip is exact). */
EMB_TEST(embed_scroll_round_trip_restores_the_picture)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    char *html = tall_page(300);
    emb_view *a = open_view(rt, h.dev, 200, 150, 1.0f);
    EMB_REQUIRE(a != nullptr);
    EMB_REQUIRE(emb_view_load_html(a, html, strlen(html), nullptr));
    emb_view_scroll_by(a, 0.0f, 100.0f);
    EMB_CHECK(render_now(h.dev, a));
    uint8_t *p0 = readback(h.dev, emb_view_texture(a), 200, 150);
    emb_view_scroll_by(a, 0.0f, 777.0f);
    EMB_CHECK(render_now(h.dev, a));
    emb_view_scroll_by(a, 0.0f, -777.0f);
    EMB_CHECK(render_now(h.dev, a));
    EMB_CHECK(stats_of(a).scroll_reused);
    uint8_t *p1 = readback(h.dev, emb_view_texture(a), 200, 150);
    EMB_REQUIRE(p0 != nullptr && p1 != nullptr);
    EMB_CHECK(memcmp(p0, p1, 200u * 150u * 4u) == 0);
    SDL_free(p0);
    SDL_free(p1);
    SDL_free(html);
    emb_view_destroy(a);
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* The GPU tests use the video subsystem, which is initialized once around the run (as the other GPU tests do). */
int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
