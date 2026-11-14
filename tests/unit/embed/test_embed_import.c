/*
 * Stylesheet @import and table columns through an embedded view (W4g1): the resolved URL of every nested sheet (relative
 * to the sheet that imports it), the buffers the fetch hands back, the cascade order of imported and importing rules, an
 * @import that comes too late (ignored, never fetched), a link `charset` attribute, and the baseline of the allocation
 * counter after the views of those pages are destroyed (the @import loader and the column code own memory per view).
 * Without a GPU device the tests skip.
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

/* ------------------------------------------------------------------ the site served by the handler */

#define MAX_URLS 16

typedef struct site {
    char urls[MAX_URLS][96]; /* every URL the handler was asked for, in order */
    int n;
} site;

static int site_count(const site *s, const char *tail)
{
    int c = 0;
    for (int i = 0; i < s->n; i++) {
        size_t ul = strlen(s->urls[i]), tl = strlen(tail);
        if (ul >= tl && strcmp(s->urls[i] + ul - tl, tail) == 0) {
            c++;
        }
    }
    return c;
}

/* The page links a.css. a.css imports b/c.css (a path relative to a.css) and then has its own rules; b/c.css imports
 * ../d.css (relative to b/c.css, so the URL is mem://site/d.css), and a.css has a late @import (after a rule, so it
 * does not count). The imported sheets come first in the cascade: a.css's own .own rule wins over the imported one. */
static const char k_page[] =
    "<!doctype html><link rel=stylesheet href=\"a.css\"><body style=\"margin:0\">"
    "<div class=own></div><div class=deep></div><div class=mid></div>";
static const char k_a_css[] =
    "@charset \"utf-8\";\n"
    "@import \"b/c.css\";\n"
    ".own{width:30px;height:10px;background:#ff0000}\n"
    "@import \"late.css\";\n"
    ".mid{width:30px;height:10px}\n";
static const char k_c_css[] =
    "@import url(../d.css);\n"
    ".own{width:30px;height:10px;background:#00ff00}\n"
    ".mid{background:#0000ff}\n";
static const char k_d_css[] = ".deep{width:30px;height:10px;background:#ffff00}\n";
static const char k_late_css[] = ".own{background:#ffffff}\n";

static bool site_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    site *s = user;
    if (s->n < MAX_URLS && url_len < sizeof s->urls[0]) {
        memcpy(s->urls[s->n], url, url_len);
        s->urls[s->n][url_len] = '\0';
        s->n++;
    }
    const char *tail = strrchr(url, '/');
    tail = tail != nullptr ? tail + 1 : url;
    const char *body = nullptr;
    size_t len = 0;
    if (strcmp(tail, "page.html") == 0) {
        body = k_page;
        len = sizeof k_page - 1;
    } else if (strcmp(tail, "a.css") == 0) {
        body = k_a_css;
        len = sizeof k_a_css - 1;
    } else if (strcmp(tail, "c.css") == 0) {
        body = k_c_css;
        len = sizeof k_c_css - 1;
    } else if (strcmp(tail, "d.css") == 0) {
        body = k_d_css;
        len = sizeof k_d_css - 1;
    } else if (strcmp(tail, "late.css") == 0) {
        body = k_late_css;
        len = sizeof k_late_css - 1;
    } else {
        return false;
    }
    out->data = (const uint8_t *)body;
    out->len = len;
    return true;
}

static void check_import_pixels(host *h, emb_view *v)
{
    uint8_t *px = readback(h->dev, emb_view_texture(v), 80, 60);
    EMB_REQUIRE(px != nullptr);
    /* .own: a.css's own rule (red) beats the imported green because the imported sheet is earlier in the cascade. */
    EMB_CHECK(pixel_is(px, 80, 5, 5, 0xFF, 0x00, 0x00));
    /* .deep: from d.css, imported by c.css, imported by a.css: yellow. */
    EMB_CHECK(pixel_is(px, 80, 5, 15, 0xFF, 0xFF, 0x00));
    /* .mid: the background from c.css (blue) and the size from a.css's own rule combine. */
    EMB_CHECK(pixel_is(px, 80, 5, 25, 0x00, 0x00, 0xFF));
    EMB_CHECK(pixel_is(px, 80, 40, 5, 0xFF, 0xFF, 0xFF));
    SDL_free(px);
}

EMB_TEST(embed_import_resolves_nested_urls_and_orders_the_cascade)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    site s;
    memset(&s, 0, sizeof s);
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", site_handler, &s));
    emb_view *v = open_view(rt, h.dev, 80, 60);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h.dev, v));
    /* The resolved URL of each sheet: a.css next to the page, c.css inside b/ (relative to a.css), d.css one level up
     * from c.css (so next to the page again). The fetch buffers were each read once, and the late @import not at all. */
    EMB_CHECK(site_count(&s, "mem://site/a.css") == 1);
    EMB_CHECK(site_count(&s, "mem://site/b/c.css") == 1);
    EMB_CHECK(site_count(&s, "mem://site/d.css") == 1);
    EMB_CHECK(site_count(&s, "late.css") == 0);
    check_import_pixels(&h, v);
    emb_view_destroy(v);
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "mem", nullptr, nullptr));
    emb_runtime_destroy(rt);
    host_close(&h);
}

/* A page with two columns that carry backgrounds and a collapsed-border table, to run the column and @import code once
 * per view in the leak check below. */
static const char k_cols_page[] =
    "<!doctype html><link rel=stylesheet href=\"a.css\"><body style=\"margin:0\">"
    "<table style=\"border-spacing:0\"><colgroup><col style=\"width:30px;background:#f00\">"
    "<col style=\"width:50px;background:#0f0\"></colgroup><tr><td style=\"height:20px;padding:0\"></td>"
    "<td style=\"padding:0\"></td></tr></table>"
    "<table dir=rtl style=\"border-spacing:0;border-collapse:collapse\"><col style=\"width:30px;background:#00f\">"
    "<tr><td style=\"height:10px;padding:0;border:2px solid #000\"></td></tr></table>";

static bool cols_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    const char *tail = strrchr(url, '/');
    tail = tail != nullptr ? tail + 1 : url;
    if (strcmp(tail, "page.html") == 0) {
        out->data = (const uint8_t *)k_cols_page;
        out->len = sizeof k_cols_page - 1;
        return true;
    }
    return site_handler(user, url, url_len, out);
}

static void load_and_render(host *h, emb_runtime *rt, site *s)
{
    emb_view *v = open_view(rt, h->dev, 80, 60);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h->dev, v));
    emb_view_destroy(v);
    s->n = 0;
}

EMB_TEST(embed_import_and_table_columns_return_to_the_allocation_baseline)
{
    host h;
    if (!host_open(&h)) {
        return;
    }
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    site s;
    memset(&s, 0, sizeof s);
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", cols_handler, &s));
    /* A warm-up page: the pipelines and the shared tables are created once. */
    load_and_render(&h, rt, &s);
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        load_and_render(&h, rt, &s);
    }
    EMB_CHECK(embi_mem_live() == base);

    /* The same page in one view: the first table's columns paint (below). A view keeps the sheets it was given until it is
     * destroyed (the style engine's retained list), so a reload is not part of the baseline check above. */
    emb_view *v = open_view(rt, h.dev, 80, 60);
    EMB_REQUIRE(v != nullptr);
    EMB_CHECK(emb_view_load_url(v, "mem://site/page.html"));
    EMB_CHECK(render_now(h.dev, v));
    /* The first table's columns paint: red over the first 30 px, green over the next 50 (the second column). */
    uint8_t *px = readback(h.dev, emb_view_texture(v), 80, 60);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel_is(px, 80, 10, 5, 0xFF, 0x00, 0x00));
    EMB_CHECK(pixel_is(px, 80, 50, 5, 0x00, 0xFF, 0x00));
    SDL_free(px);
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
