/*
 * The public scripting API on an offscreen view (W5c): emb_view_eval, emb_view_set_console_callback, emb_view_post_task, and a
 * DOM change made by a timer showing up in the next rendered frame. Needs a GPU device; without one the test skips.
 */
#include "emb_test.h"

#include "base/clock.h"
#include "embed/embed_int.h"

#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

typedef struct view_env {
    char console[256];
    size_t console_len;
    int task_runs;
} view_env;

static void console_cb(void *user, emb_log_level level, const char *text, size_t len)
{
    (void)level;
    view_env *e = user;
    if (e->console_len + len + 1 < sizeof e->console) {
        memcpy(e->console + e->console_len, text, len);
        e->console_len += len;
        e->console[e->console_len++] = ';';
        e->console[e->console_len] = '\0';
    }
}

static void count_task(void *user)
{
    ((view_env *)user)->task_runs++;
}

/* Copies an eval result into the caller's buffer (64 bytes). */
static void copy_result(void *user, bool ok, const char *text, size_t len)
{
    (void)ok;
    char *dst = user;
    size_t n = len < 63 ? len : 63;
    memcpy(dst, text, n);
    dst[n] = '\0';
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

/* The RGB of pixel (x, y) of a 128-wide frame, as 0xRRGGBB. */
static uint32_t pixel(const uint8_t *px, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * 128u + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* A red box at the top left of the page; a timer at 16 ms turns it green. A second timer at 40 ms computes only. */
static const char k_page[] =
    "<!doctype html><body style='margin:0'><div id=box style='width:20px;height:20px;background:#f00'></div>"
    "<script>setTimeout(function () {"
    "  document.getElementById('box').style.background = '#0f0';"
    "  console.log('fired');"
    "}, 16);</script>";

/* A page whose only timer is an interval that touches no DOM. */
static const char k_idle_page[] =
    "<!doctype html><body style='margin:0'><div style='width:20px;height:20px;background:#00f'></div>"
    "<script>var ticks = 0; setInterval(function () { ticks++; }, 16);</script>";

static emb_view *make_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc desc = {
        .size = sizeof desc,
        .mode = EMB_VIEW_OFFSCREEN,
        .width = 128,
        .height = 96,
        .scale = 1.0f,
        .device = dev,
        .scheme = EMB_SCHEME_LIGHT,
    };
    return emb_view_create(rt, &desc);
}

EMB_TEST(view_script_api_and_timer_change_the_next_frame)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    embi_clock_set_fake(true, 1000000000ULL);
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);

    view_env e = {0};
    emb_view_set_console_callback(v, console_cb, &e);
    EMB_REQUIRE(emb_view_load_html(v, k_page, strlen(k_page), "http://t.test/index.html"));
    (void)render_now(dev, v);

    /* The first frame shows the red box. */
    uint8_t *px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0xFF0000);
    SDL_free(px);

    /* Eval runs in the page's realm and sees the page's DOM: the box is still red. */
    char result[64] = "";
    const char *expr = "document.getElementById('box').style.background";
    bool ok = emb_view_eval(v, expr, strlen(expr), copy_result, result);
    EMB_CHECK(ok);
    EMB_CHECK(SDL_strcasecmp(result, "#f00") == 0 || SDL_strcasecmp(result, "rgb(255, 0, 0)") == 0 ||
              SDL_strcasecmp(result, "red") == 0);

    /* A posted task runs on the next update of the view, and changes no pixel (no DOM work, no new frame). */
    EMB_CHECK(emb_view_post_task(v, count_task, &e));
    EMB_CHECK_EQ_INT(e.task_runs, 0);
    EMB_CHECK(!render_now(dev, v));
    EMB_CHECK_EQ_INT(e.task_runs, 1);

    /* Before the timer is due, the frame does not change. */
    EMB_CHECK(!render_now(dev, v));
    EMB_CHECK_EQ_STR(e.console, "");

    /* The timer fires at 16 ms: its DOM change is in the frame of that update, and the box is green. */
    embi_clock_advance_fake(16000000ULL);
    EMB_CHECK(render_now(dev, v));
    EMB_CHECK_EQ_STR(e.console, "fired;");
    px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0x00FF00);
    EMB_CHECK(pixel(px, 60, 60) == 0xFFFFFF); /* the rest of the page is untouched */
    SDL_free(px);

    /* Nothing else is pending: no change, no new frame. */
    EMB_CHECK(!render_now(dev, v));

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
    embi_clock_set_fake(false, 0);
}

EMB_TEST(view_idle_timer_that_touches_no_dom_causes_no_frame)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    embi_clock_set_fake(true, 1000000000ULL);
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);
    EMB_REQUIRE(emb_view_load_html(v, k_idle_page, strlen(k_idle_page), "http://t.test/idle.html"));
    (void)render_now(dev, v);
    (void)render_now(dev, v); /* the lifecycle tasks run here; they change nothing */
    EMB_CHECK(!render_now(dev, v));

    /* The interval fires many times; every update reports no change, and the page did run. */
    for (int i = 0; i < 20; i++) {
        embi_clock_advance_fake(16000000ULL);
        EMB_CHECK(!render_now(dev, v));
    }
    char result[64] = "";
    const char *expr = "ticks";
    EMB_CHECK(emb_view_eval(v, expr, strlen(expr), copy_result, result));
    EMB_CHECK_EQ_STR(result, "20");

    /* The frame still shows the blue box. */
    uint8_t *px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0x0000FF);
    SDL_free(px);

    /* A change the page makes through a DOM call does produce a frame. */
    const char *change = "document.body.style.background = '#ff0'";
    EMB_CHECK(emb_view_eval(v, change, strlen(change), copy_result, result));
    EMB_CHECK(render_now(dev, v));
    px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 60, 60) == 0xFFFF00);
    SDL_free(px);

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
    embi_clock_set_fake(false, 0);
}

/* emb_view_desc.no_script: the script does not run (the body stays white, not red) and the <noscript> content is
 * built (the green box). */
static const char k_noscript_page[] =
    "<!doctype html><body style='margin:0'>"
    "<script>document.body.style.background = '#f00';</script>"
    "<noscript><div style='width:20px;height:20px;background:#0f0'></div></noscript>";

EMB_TEST(view_no_script_skips_scripts_and_builds_noscript)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    for (int off = 0; off < 2; off++) {
        emb_view_desc desc = {
            .size = sizeof desc,
            .mode = EMB_VIEW_OFFSCREEN,
            .width = 128,
            .height = 96,
            .scale = 1.0f,
            .device = dev,
            .scheme = EMB_SCHEME_LIGHT,
            .no_script = off != 0,
        };
        emb_view *v = emb_view_create(rt, &desc);
        EMB_REQUIRE(v != nullptr);
        EMB_REQUIRE(emb_view_load_html(v, k_noscript_page, strlen(k_noscript_page), "http://t.test/ns.html"));
        (void)render_now(dev, v);
        uint8_t *px = readback(dev, emb_view_texture(v), 128, 96);
        EMB_REQUIRE(px != nullptr);
        if (off) {
            EMB_CHECK(pixel(px, 10, 10) == 0x00FF00);
            EMB_CHECK(pixel(px, 60, 60) == 0xFFFFFF);
        } else {
            EMB_CHECK(pixel(px, 10, 10) == 0xFF0000);
            EMB_CHECK(pixel(px, 60, 60) == 0xFF0000);
        }
        SDL_free(px);
        emb_view_destroy(v);
    }
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
}

/* A ResizeObserver is notified by the view's frame (after layout, before paint): its callback runs during the first
 * render, and the style change it makes is drawn. */
static const char k_resize_page[] =
    "<!doctype html><body style='margin:0'><div id=box style='width:20px;height:20px;background:#f00'></div>"
    "<script>var box = document.getElementById('box');"
    "new ResizeObserver(function (entries) {"
    "  console.log('ro ' + entries.length + ' ' + entries[0].contentRect.width);"
    "  box.style.background = '#0f0';"
    "}).observe(box);</script>";

EMB_TEST(view_frame_delivers_resize_observations)
{
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return;
    }
    embi_clock_set_fake(true, 1000000000ULL);
    SDL_GPUDevice *dev = embi_gpu_device(g);
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    emb_view *v = make_view(rt, dev);
    EMB_REQUIRE(v != nullptr);
    view_env e = {0};
    emb_view_set_console_callback(v, console_cb, &e);
    EMB_REQUIRE(emb_view_load_html(v, k_resize_page, strlen(k_resize_page), "http://t.test/ro.html"));
    EMB_CHECK_EQ_STR(e.console, "");
    (void)render_now(dev, v);
    EMB_CHECK_EQ_STR(e.console, "ro 1 20;");
    uint8_t *px = readback(dev, emb_view_texture(v), 128, 96);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK(pixel(px, 10, 10) == 0x00FF00);
    SDL_free(px);

    /* The size did not change: no second notification. */
    (void)render_now(dev, v);
    EMB_CHECK_EQ_STR(e.console, "ro 1 20;");

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_gpu_destroy(g);
    embi_clock_set_fake(false, 0);
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
