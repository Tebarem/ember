/*
 * bench_anim (W6e): the frame cost of a page that animates, against the 60 fps budget. It prints PASS or
 * FAIL lines; the exit status is 0 whatever the numbers are (CI prints them) unless --strict is given.
 *
 *   bench_anim [--frames N] [--pages DIR] [--system-fonts] [--strict]
 *
 * --frames N      frames measured per scenario (default 360, six seconds of the fake frame clock)
 * --pages DIR     the directory with anim-60fps.html and static-todomvc.html (default tests/pages)
 * --system-fonts  the runtime indexes and uses the machine's fonts (default off, so the numbers do not depend on them)
 * --strict        exit 1 when a line is FAIL
 *
 * Scenarios (each a fresh runtime and view, the document's scripts run for real):
 *   anim-60fps     tests/pages/anim-60fps.html: 200 items with transform and opacity transitions that a script flips every
 *                  400 ms, plus one bar whose width animation changes layout every frame.
 *   anim-compose   the same page with the width animation removed: only transform and opacity change, so no layout may run
 *                  while the transitions play (the fragments are kept, the frame is a recomposite or a paint-only update).
 *   todomvc-toggle tests/pages/static-todomvc.html with 100 todos, opacity, transform and background transitions, and a
 *                  script that plays toggle-all (completes every todo, then clears them) every 500 ms.
 *
 * Frames. The frame clock is the fake clock (embi_clock_set_fake) stepped by 1000/60 ms per frame, so the animations,
 * transitions and the requestAnimationFrame timestamps advance exactly one frame per step whatever the speed of the host,
 * while the cost is measured with the real performance counter. The frame time is the host time of the whole frame: from just
 * before emb_view_render (the rAF callbacks, animation sampling, restyle, layout, observers, display list, damage, GPU record)
 * to just after the command buffer is submitted. The wait for the GPU to finish is not measured. The 95th percentile
 * must be under 16.6 ms. On a software Vulkan driver (llvmpipe) the GPU share is a worst case.
 *
 * Steady state. After a warm-up of 30 frames, the style engine's full passes, the layout engine's full tree builds and the
 * layout runs that were full (EMBI_LU_FULL) are counted over the measured frames; all must be zero: a transition or an
 * animation is a partial restyle and a surgery or a paint-only update, never a rebuild. For anim-compose and todomvc-toggle
 * (no layout-affecting animation) the frames that ran a layout surgery are counted too: transform and opacity must not relayout,
 * so the only surgeries allowed are those of the frames where the script changed a class (the flip frames).
 */
#include <ember/ember.h>

#include "base/base.h"
#include "base/clock.h"
#include "embed/doc.h"
#include "embed/embed_int.h"
#include "embed/script_int.h"
#include "layout/layout.h"
#include "style/style.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define BUDGET_FRAME_MS 16.6
#define FRAME_NS (1000000000ULL / 60ULL)
#define WARMUP_FRAMES 30
#define MAX_FRAMES 4096

#define TODO_STYLE_TEXT \
    "<style>" \
    ".todo-list li{transition:opacity 300ms linear,transform 300ms ease-out,background-color 300ms linear}" \
    ".todo-list li.completed{opacity:.5;transform:translateX(12px);background-color:#eef}" \
    ".todo-list{max-height:none;overflow:visible}" \
    "</style>"
#define TODO_SCRIPT_TEXT \
    "<script>" \
    "var list=document.querySelector('.todo-list'),first=list.firstElementChild;" \
    "for(var i=0;i<100;i++){var c=first.cloneNode(true);c.className='';list.appendChild(c)}" \
    "window.__flips=0;var last=0,all=false;" \
    "function tick(t){if(t-last>=500){last=t;all=!all;window.__flips++;" \
    "var lis=list.children;for(var i=0;i<lis.length;i++)lis[i].classList.toggle('completed',all)}" \
    "requestAnimationFrame(tick)}requestAnimationFrame(tick);" \
    "</script>"

static void quiet_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    if (level >= EMB_LOG_WARN) {
        fprintf(stderr, "bench_anim: %s\n", message);
    }
}

static double ms_between(uint64_t t0, uint64_t t1)
{
    return (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Replaces the first occurrence of from in text with to. Returns a new SDL_malloc'd NUL-terminated string. */
static char *replace_first(const char *text, size_t len, const char *from, const char *to, size_t *out_len)
{
    const char *at = SDL_strstr(text, from);
    size_t fl = SDL_strlen(from), tl = SDL_strlen(to);
    size_t before = at != nullptr ? (size_t)(at - text) : len;
    size_t n = at != nullptr ? len - fl + tl : len;
    char *out = SDL_malloc(n + 1);
    memcpy(out, text, before);
    if (at != nullptr) {
        memcpy(out + before, to, tl);
        memcpy(out + before + tl, at + fl, len - before - fl);
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

static char *load_page(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (data == nullptr) {
        printf("bench_anim: cannot read %s: %s\n", path, SDL_GetError());
        return nullptr;
    }
    char *s = SDL_malloc(n + 1);
    memcpy(s, data, n);
    s[n] = '\0';
    SDL_free(data);
    *len = n;
    return s;
}

typedef struct totals {
    uint32_t frames;
    double ft[MAX_FRAMES];       /* host time of each frame, ms */
    double style_sum, layout_sum, dl_sum, gpu_sum;
    double style_max, layout_max, dl_max, gpu_max;
    uint64_t restyled_sum;
    uint32_t restyled_max;
    uint32_t restyle_frames;     /* frames that restyled something */
    uint32_t surgery_frames;     /* frames whose layout ran a surgery */
    uint32_t paint_only_frames;  /* frames whose update was a paint-only one */
    uint32_t idle_frames;        /* frames that changed nothing */
    uint32_t laid_out_max;
    uint64_t full_passes;        /* style engine: full restyles in the window */
    uint64_t full_layouts;       /* layout engine: full box tree builds (EMBI_LU_FULL runs) in the window */
    uint32_t tree_builds;        /* layout engine: tree builds in the window */
    uint32_t changed_frames;     /* frames whose picture changed */
    uint32_t full_redraws;
    uint64_t damage_sum;
    double flips0, flips1;       /* window.__flips before and after */
    uint32_t surgery_unexplained; /* surgery frames that are not the frame of a class change (see scenario flags) */
} totals;

static void eval_cb(void *user, bool ok, const char *text, size_t len)
{
    char *dst = user;
    size_t n = ok && len < 63 ? len : 0;
    memcpy(dst, text, n);
    dst[n] = '\0';
}

static double eval_num(emb_view *v, const char *expr)
{
    char src[160];
    SDL_snprintf(src, sizeof src, "String(%s)", expr);
    char res[64] = "";
    if (v->doc == nullptr || !embi_script_eval(embi_doc_script(v->doc), src, SDL_strlen(src), eval_cb, res)) {
        return -1.0;
    }
    return SDL_atof(res);
}

/* One frame: the clock moves a frame, the view renders and the command buffer is submitted. Returns the host time in ms. */
static double run_frame(SDL_GPUDevice *dev, emb_view *v)
{
    embi_clock_advance_fake(FRAME_NS);
    uint64_t t0 = SDL_GetPerformanceCounter();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return 0.0;
    }
    emb_view_render(v, cmd);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    uint64_t t1 = SDL_GetPerformanceCounter();
    if (f != nullptr) {
        SDL_WaitForGPUFences(dev, true, &f, 1);
        SDL_ReleaseGPUFence(dev, f);
    }
    return ms_between(t0, t1);
}

static double percentile(const double *sorted, uint32_t n, double p)
{
    if (n == 0) {
        return 0.0;
    }
    uint32_t i = (uint32_t)(p * (double)n + 0.999999);
    if (i < 1) {
        i = 1;
    }
    if (i > n) {
        i = n;
    }
    return sorted[i - 1];
}

/* Runs one scenario and prints its lines. Returns the number of FAIL lines. */
static int scenario(SDL_GPUDevice *dev, const char *name, const char *html, size_t html_len, int frames, bool system_fonts,
                    bool layout_free)
{
    int fails = 0;
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = quiet_log;
    rd.flags = system_fonts ? 0u : EMB_RUNTIME_NO_SYSTEM_FONTS;
    embi_clock_set_fake(true, 5000000000ULL);
    emb_runtime *rt = emb_runtime_create(&rd);
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 800;
    vd.height = 600;
    vd.scale = 1.0f;
    vd.device = dev;
    emb_view *v = rt != nullptr ? emb_view_create(rt, &vd) : nullptr;
    if (v == nullptr || !emb_view_load_html(v, html, html_len, "http://bench.test/page.html")) {
        printf("FAIL %s: the page did not load\n", name);
        if (v != nullptr) {
            emb_view_destroy(v);
        }
        if (rt != nullptr) {
            emb_runtime_destroy(rt);
        }
        embi_clock_set_fake(false, 0);
        return 1;
    }
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        run_frame(dev, v);
    }

    static totals t; /* large: kept off the stack */
    memset(&t, 0, sizeof t);
    embi_style_engine *se = embi_doc_style_engine(v->doc);
    embi_layout_engine *le = embi_doc_layout_engine(v->doc);
    embi_document *dom = embi_doc_dom(v->doc);
    embi_style_restyle_stats rs0, rs;
    embi_layout_incr_stats li0, li;
    embi_style_restyle_stats_get(se, dom, &rs0);
    embi_layout_incr_stats_get(le, dom, &li0);
    t.flips0 = eval_num(v, "window.__flips");
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }
    uint64_t prev_total = rs0.total_restyled;
    uint64_t prev_surgery = li0.n_surgery, prev_full = li0.n_full, prev_paint = li0.n_paint;
    for (int i = 0; i < frames; i++) {
        double ms = run_frame(dev, v);
        t.ft[t.frames++] = ms;
        emb_view_stats st;
        memset(&st, 0, sizeof st);
        st.size = sizeof st;
        emb_view_stats_get(v, &st);
        embi_style_restyle_stats_get(se, dom, &rs);
        embi_layout_incr_stats_get(le, dom, &li);
        uint32_t restyled = (uint32_t)(rs.total_restyled - prev_total);
        prev_total = rs.total_restyled;
        bool surgery = li.n_surgery > prev_surgery || li.n_full > prev_full;
        bool paint_only = li.n_paint > prev_paint;
        prev_surgery = li.n_surgery;
        prev_full = li.n_full;
        prev_paint = li.n_paint;
        t.restyled_sum += restyled;
        if (restyled > t.restyled_max) {
            t.restyled_max = restyled;
        }
        t.restyle_frames += restyled > 0;
        t.surgery_frames += surgery;
        t.paint_only_frames += paint_only;
        if (surgery && layout_free && restyled == 0) {
            t.surgery_unexplained++;
        }
        if (li.boxes_laid_out > t.laid_out_max && surgery) {
            t.laid_out_max = li.boxes_laid_out;
        }
        /* The stage times are those of the latest update that ran the stage; a frame that ran none keeps the old value, so
         * they are counted only on the frames that did the work. */
        if (restyled > 0 || surgery) {
            t.style_sum += st.style_ms;
            t.layout_sum += st.layout_ms;
            t.style_max = st.style_ms > t.style_max ? st.style_ms : t.style_max;
            t.layout_max = st.layout_ms > t.layout_max ? st.layout_ms : t.layout_max;
        }
        if (st.damage_area > 0 || st.full_redraw) {
            t.changed_frames++;
            t.dl_sum += st.display_list_ms;
            t.gpu_sum += st.gpu_ms;
            t.dl_max = st.display_list_ms > t.dl_max ? st.display_list_ms : t.dl_max;
            t.gpu_max = st.gpu_ms > t.gpu_max ? st.gpu_ms : t.gpu_max;
            t.damage_sum += st.damage_area;
        } else {
            t.idle_frames++;
        }
        t.full_redraws += st.full_redraw;
    }
    t.flips1 = eval_num(v, "window.__flips");
    t.full_passes = rs.full_passes - rs0.full_passes;
    t.full_layouts = li.n_full - li0.n_full;
    t.tree_builds = li.tree_builds - li0.tree_builds;

    double sorted[MAX_FRAMES];
    memcpy(sorted, t.ft, t.frames * sizeof sorted[0]);
    qsort(sorted, t.frames, sizeof sorted[0], cmp_double);
    double sum = 0.0;
    for (uint32_t i = 0; i < t.frames; i++) {
        sum += t.ft[i];
    }
    double p50 = percentile(sorted, t.frames, 0.50), p95 = percentile(sorted, t.frames, 0.95);
    double p99 = percentile(sorted, t.frames, 0.99), mx = sorted[t.frames - 1];
    uint32_t cf = t.changed_frames > 0 ? t.changed_frames : 1;
    uint32_t sf = t.restyle_frames + t.surgery_frames > 0 ? t.restyle_frames + t.surgery_frames : 1;
    printf("%s: %u frames, %u flips of the script\n", name, t.frames, (unsigned)(t.flips1 - t.flips0));
    printf("  frame time ms     avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", sum / (double)t.frames, p50, p95, p99, mx);
    printf("  style ms          avg %.3f  max %.3f  (over the %u frames that restyled or laid out)\n", t.style_sum / (double)sf,
           t.style_max, sf);
    printf("  layout ms         avg %.3f  max %.3f  (same frames)\n", t.layout_sum / (double)sf, t.layout_max);
    printf("  display list ms   avg %.3f  max %.3f  (over the %u frames that changed the picture)\n", t.dl_sum / (double)cf,
           t.dl_max, t.changed_frames);
    printf("  GPU record ms     avg %.3f  max %.3f  (same frames)\n", t.gpu_sum / (double)cf, t.gpu_max);
    printf("  restyled elements per frame: avg %.1f  max %u  (%u frames restyled)\n",
           (double)t.restyled_sum / (double)t.frames, t.restyled_max, t.restyle_frames);
    printf("  layout: %u surgery frames (most boxes laid out in one: %u), %u paint-only, %u frames with no layout work\n",
           t.surgery_frames, t.laid_out_max, t.paint_only_frames, t.frames - t.surgery_frames - t.paint_only_frames);
    printf("  paint: %u of %u frames changed the picture, %u full redraws, damage avg %.0f px\n", t.changed_frames, t.frames,
           t.full_redraws, (double)t.damage_sum / (double)cf);

    bool ok95 = p95 < BUDGET_FRAME_MS;
    printf("%s %s: frame p95 %.2f ms (budget %.1f ms, 60 fps)\n", ok95 ? "PASS" : "FAIL", name, p95, BUDGET_FRAME_MS);
    fails += !ok95;
    bool steady = t.full_passes == 0 && t.full_layouts == 0 && t.tree_builds == 0;
    printf("%s %s: steady state, %" PRIu64 " full restyles, %" PRIu64 " full layouts, %u box tree builds\n",
           steady ? "PASS" : "FAIL", name, t.full_passes, t.full_layouts, t.tree_builds);
    fails += !steady;
    if (layout_free) {
        bool nolayout = t.surgery_unexplained == 0;
        printf("%s %s: no relayout for transform and opacity (%u layout surgeries on frames without a restyle)\n",
               nolayout ? "PASS" : "FAIL", name, t.surgery_unexplained);
        fails += !nolayout;
    }
    bool animated = t.flips1 - t.flips0 >= 1.0 && t.changed_frames > t.frames / 2;
    printf("%s %s: the page animates (%u of %u frames changed the picture)\n", animated ? "PASS" : "FAIL", name, t.changed_frames,
           t.frames);
    fails += !animated;
    printf("\n");

    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    embi_clock_set_fake(false, 0);
    return fails;
}

int main(int argc, char **argv)
{
    int frames = 360;
    const char *dir = "tests/pages";
    bool system_fonts = false, strict = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--system-fonts") == 0) {
            system_fonts = true;
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--pages DIR] [--system-fonts] [--strict]\n", argv[0]);
            return 0;
        }
    }
    if (frames < 60) {
        frames = 60;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("bench_anim: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (dev == nullptr) {
        printf("bench_anim: no GPU device (%s); nothing measured\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    const char *driver = SDL_GetGPUDeviceDriver(dev);
    printf("bench_anim: GPU driver %s, %d frames per scenario at 60 fps of the fake frame clock\n", driver, frames);
    if (SDL_strcmp(driver, "vulkan") == 0) {
        printf("bench_anim: note: software Vulkan (llvmpipe) on a host without a GPU, so the GPU share is a worst case\n");
    }
    printf("\n");

    int fails = 0;
    size_t len = 0;
    char *page = load_page(dir, "anim-60fps.html", &len);
    if (page != nullptr) {
        fails += scenario(dev, "anim-60fps", page, len, frames, system_fonts, false);
        size_t len2 = 0;
        char *compose = replace_first(page, len, "animation: grow 1.6s ease-in-out infinite alternate;", "animation: none;", &len2);
        fails += scenario(dev, "anim-compose", compose, len2, frames, system_fonts, true);
        SDL_free(compose);
        SDL_free(page);
    } else {
        fails++;
    }
    page = load_page(dir, "static-todomvc.html", &len);
    if (page != nullptr) {
        size_t l1 = 0, l2 = 0;
        char *a = replace_first(page, len, "</head>", TODO_STYLE_TEXT "</head>", &l1);
        char *b = replace_first(a, l1, "</body>", TODO_SCRIPT_TEXT "</body>", &l2);
        fails += scenario(dev, "todomvc-toggle", b, l2, frames, system_fonts, true);
        SDL_free(a);
        SDL_free(b);
        SDL_free(page);
    } else {
        fails++;
    }
    printf("bench_anim: %d line(s) FAIL\n", fails);
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    return strict && fails > 0 ? 1 : 0;
}
