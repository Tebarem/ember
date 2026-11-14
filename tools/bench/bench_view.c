/*
 * bench_view (W4e): the embedding layer's first paint and scroll numbers against the budgets of the plan, printed as
 * PASS or FAIL lines. It is a report: the exit status is 0 whatever the numbers are, except for the baseline gate of --app
 * (W8b, below).
 *
 *   bench_view [--page PATH | --app DIST ...] [--lib-bytes N] [--frames N] [--runs N] [--system-fonts]
 *
 * --page PATH     the page to measure (default tests/pages/static-todomvc.html, relative to the working directory); when it
 *                 cannot be loaded the synthetic page of 300 rows is measured instead, and the output says so
 * --lib-bytes N   the size of the stripped libember in bytes (CI passes it), checked against the 3.2 MB budget (MiB; the size configuration)
 * --frames N      scroll frames measured (default 120)
 * --runs N        first-paint runs; the best is reported (default 5)
 * --system-fonts  the runtimes index and use the machine's fonts (default off, so the numbers do not depend on them)
 * --app DIST      measure a built app (tests/apps/<name>/dist) served from app://local/ instead of a page: first paint and
 *                 first content by stage, heap and idle RSS per view, and with --todomvc KEY the scroll of --items N todos
 *                 (default 1000) and the toggle-all animation on --toggle-items N todos (default 100; bench_view_app.h
 *                 has the details); --ready SEL is the
 *                 selector of the app's UI (default .todoapp), --extra-views N the views of the RSS average (default 4)
 * --metrics FILE  writes the app metrics as "KEY name value" lines
 * --baseline FILE compares the app metrics with the lines of --key KEY in FILE (tools/bench/budget_baseline.txt): a
 *                 deterministic metric (library size, heap per view) more than 5% above its baseline makes the exit status
 *                 1; the wall clock metrics are printed with their change but not enforced (shared CI runners vary by more)
 *
 * The first paint is measured as the plan defines it: the wall clock from just before emb_runtime_create to just after
 * the first frame's command buffer is submitted (the document file read, the stylesheets and the CSS parse are inside
 * it). The verdict is the first run of the process, a cold start (no warm font, shader or allocator state); the best of
 * the runs after it is shown as the warm figure. The stage breakdown beside it comes from the library's own timers;
 * "other" is the wall clock the timers do not cover. Fonts (the first text shaping) are a part of layout and are shown
 * under it, image uploads are a part of the GPU record. GPU work completes asynchronously after the submit, and the wait
 * for it is not measured.
 *
 * The scroll frame is the average over scroll steps of the whole update, recording and submit. The heap per view is the
 * live heap after a view is loaded (embi_mem_live), not the resident set: the process RSS is not measured here.
 *
 * On a software Vulkan driver (llvmpipe, as on the Linux CI runner) the GPU stages are slower than on real hardware, so
 * those numbers are a floor. Run it on a Release build; the budgets are checked, not enforced.
 */
#include <ember/ember.h>

#include "base/base.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define BUDGET_FIRST_PAINT_MS 20.0
#define BUDGET_FRAME_MS (1000.0 / 60.0)
#define BUDGET_LIB_BYTES (3.2 * 1024.0 * 1024.0)
#define BUDGET_HEAP_BYTES (15.0 * 1024.0 * 1024.0)
#define BENCH_DEFAULT_PAGE "tests/pages/static-todomvc.html"

/* A page of 300 rows of boxes and text, taller than the viewport, so the scroll does work. Used when the page file is
 * missing. */
static char *build_page(size_t *out_len)
{
    size_t cap = 64 * 1024;
    char *buf = SDL_malloc(cap);
    size_t n = (size_t)SDL_snprintf(buf, cap,
        "<!doctype html><title>Bench</title><style>"
        "body{margin:0;font:14px sans-serif;background:#fafafa}"
        ".row{display:flex;height:24px;margin:2px 8px;align-items:center}"
        ".chip{width:40px;height:16px;border-radius:4px;background:#3f51b5;margin-right:8px}"
        ".row span{color:#222}"
        "</style>");
    for (int i = 0; i < 300; i++) {
        if (n + 256 >= cap) {
            cap *= 2;
            buf = SDL_realloc(buf, cap);
        }
        n += (size_t)SDL_snprintf(buf + n, cap - n,
            "<div class=row><div class=chip style=\"background:#%06x\"></div><span>Row %d of the bench page</span></div>",
            (unsigned)((i * 2654435761u) & 0xFFFFFFu), i);
    }
    *out_len = n;
    return buf;
}

/* The runtime's messages of warning and error level go to stderr; the rest are dropped, so the timings stay quiet. */
static void quiet_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    if (level >= EMB_LOG_WARN) {
        fprintf(stderr, "bench_view: %s\n", message);
    }
}

static double ms_since(uint64_t t0)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static emb_view *make_view(emb_runtime *rt, SDL_GPUDevice *dev)
{
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = 800;
    d.height = 600;
    d.scale = 1.0f;
    d.device = dev;
    return emb_view_create(rt, &d);
}

/* Allows the directory of a page path (relative paths resolve against the working directory). */
static void allow_page_dir(emb_runtime *rt, const char *page)
{
    char dir[2048];
    bool absolute = page[0] == '/' || page[0] == '\\' || (page[0] != '\0' && page[1] == ':');
    char *cwd = absolute ? nullptr : SDL_GetCurrentDirectory();
    size_t cl = cwd != nullptr ? SDL_strlen(cwd) : 0;
    while (cl > 0 && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) {
        cl--;
    }
    SDL_snprintf(dir, sizeof dir, "%.*s%s%s", (int)cl, cwd != nullptr ? cwd : "", cwd != nullptr ? "/" : "", page);
    SDL_free(cwd);
    char *slash = SDL_strrchr(dir, '/');
    char *bs = SDL_strrchr(dir, '\\');
    if (bs != nullptr && (slash == nullptr || bs > slash)) {
        slash = bs;
    }
    if (slash == nullptr) {
        return;
    }
    *slash = '\0';
    for (char *c = dir; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    emb_runtime_allow_path(rt, dir);
}

/* Records the frame, submits it and returns the host's submit time in ms; the wait for the GPU is not part of it.
 * *end receives the performance counter right after the submit returned (nullptr: not wanted). */
static double render_and_wait(SDL_GPUDevice *dev, emb_view *v, uint64_t *end)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        printf("bench_view: SDL_AcquireGPUCommandBuffer failed: %s\n", SDL_GetError());
        if (end != nullptr) {
            *end = SDL_GetPerformanceCounter();
        }
        return 0.0;
    }
    emb_view_render(v, cmd);
    uint64_t t0 = SDL_GetPerformanceCounter();
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    uint64_t t1 = SDL_GetPerformanceCounter();
    if (end != nullptr) {
        *end = t1;
    }
    double submit = (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (f != nullptr) {
        SDL_WaitForGPUFences(dev, true, &f, 1);
        SDL_ReleaseGPUFence(dev, f);
    }
    return submit;
}

static const char *verdict(bool ok)
{
    return ok ? "PASS" : "FAIL";
}

#include "bench_view_app.h"

int main(int argc, char **argv)
{
    double lib_bytes = -1.0;
    int frames = 120;
    int runs = 5;
    const char *page_path = BENCH_DEFAULT_PAGE;
    bool system_fonts = false; /* --system-fonts: the runtimes index and use the machine's fonts (W4i) */
    app_opts app = {.dir = nullptr, .ready = ".todoapp", .todomvc_key = nullptr, .items = 1000, .toggle_items = 100,
                   .extra_views = 4};
    const char *metrics_path = nullptr;
    const char *baseline_path = nullptr;
    const char *key = "local";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            app.dir = argv[++i];
        } else if (strcmp(argv[i], "--ready") == 0 && i + 1 < argc) {
            app.ready = argv[++i];
        } else if (strcmp(argv[i], "--todomvc") == 0 && i + 1 < argc) {
            app.todomvc_key = argv[++i];
        } else if (strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
            app.items = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--toggle-items") == 0 && i + 1 < argc) {
            app.toggle_items = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--toggle-repeat") == 0 && i + 1 < argc) {
            app.toggle_repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--extra-views") == 0 && i + 1 < argc) {
            app.extra_views = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--metrics") == 0 && i + 1 < argc) {
            metrics_path = argv[++i];
        } else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) {
            baseline_path = argv[++i];
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key = argv[++i];
        } else if (strcmp(argv[i], "--lib-bytes") == 0 && i + 1 < argc) {
            lib_bytes = atof(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            page_path = argv[++i];
        } else if (strcmp(argv[i], "--system-fonts") == 0) {
            system_fonts = true;
        } else {
            fprintf(stderr,
                    "usage: %s [--page PATH | --app DIST [--ready SEL] [--todomvc KEY] [--items N] [--toggle-items N]\n"
                    "          [--toggle-repeat N] [--extra-views N]]\n"
                    "          [--lib-bytes N] [--frames N] [--runs N] [--system-fonts] [--metrics FILE]\n"
                    "          [--baseline FILE --key KEY]\n",
                    argv[0]);
            return 0;
        }
    }
    if (frames < 1) {
        frames = 1;
    }
    if (runs < 1) {
        runs = 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("bench_view: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (dev == nullptr) {
        printf("bench_view: no GPU device (%s); nothing measured\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    const char *driver = SDL_GetGPUDeviceDriver(dev);
    printf("bench_view: GPU driver %s\n", driver);
    if (SDL_strcmp(driver, "vulkan") == 0) {
        printf("bench_view: note: software Vulkan (llvmpipe) on this host, so the GPU stages are a floor and slower than on\n"
               "bench_view:       real hardware; the CPU stages are not affected\n");
    }

    if (app.dir != nullptr) {
        /* --app: the budgets on a built framework app (bench_view_app.h); the page measurements are skipped. */
        printf("bench_view: app %s from app://local/\n", app.dir);
        app_first_paint(dev, &app, runs, system_fonts);
        app_memory(dev, &app, system_fonts);
        if (app.todomvc_key != nullptr) {
            app_todomvc_frames(dev, &app, frames, system_fonts);
        }
        if (lib_bytes >= 0.0) {
            printf("%s libember %.0f bytes stripped (budget %.0f bytes)\n", verdict(lib_bytes < BUDGET_LIB_BYTES),
                   lib_bytes, BUDGET_LIB_BYTES);
            metric_put("lib_bytes", lib_bytes, "B", true);
        } else {
            printf("libember size not given (pass --lib-bytes)\n");
        }
        SDL_DestroyGPUDevice(dev);
        SDL_Quit();
        if (metrics_path != nullptr) {
            metrics_write(metrics_path, key);
        }
        /* The gate: a deterministic metric (library size, heap per view) more than 5% over the baseline fails. */
        return baseline_path != nullptr && baseline_gate(baseline_path, key) > 0 ? 1 : 0;
    }

    /* The page: the snapshot, or the synthetic page when the file is missing. */
    size_t page_len = 0;
    char *page = nullptr;
    size_t file_len = 0;
    void *file = SDL_LoadFile(page_path, &file_len);
    const char *page_name = page_path;
    if (file != nullptr) {
        page = (char *)file;
        page_len = file_len;
    } else {
        page = build_page(&page_len);
        page_name = "synthetic 300-row page";
        printf("bench_view: %s not found; measuring the %s\n", BENCH_DEFAULT_PAGE, page_name);
    }
    printf("bench_view: page %s (%zu bytes)\n", page_name, page_len);

    /* First paint: wall clock from before emb_runtime_create to after the submit of the first frame. Run 0 is the cold
     * start and gives the verdict; the later runs (a fresh runtime and view each time) give the warm figure. */
    typedef struct fp {
        double wall, create, submit, fetch_other;
        emb_view_stats st;
    } fp;
    fp cold, warm;
    memset(&cold, 0, sizeof cold);
    memset(&warm, 0, sizeof warm);
    warm.wall = 1.0e30;
    bool have_cold = false;
    for (int r = 0; r < runs; r++) {
        emb_runtime_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.size = sizeof rd;
        rd.log = quiet_log;
        rd.flags = system_fonts ? 0u : EMB_RUNTIME_NO_SYSTEM_FONTS;
        uint64_t t_start = SDL_GetPerformanceCounter();
        emb_runtime *rt = emb_runtime_create(&rd);
        if (rt == nullptr) {
            printf("bench_view: runtime create failed\n");
            break;
        }
        if (file != nullptr) {
            allow_page_dir(rt, page_path);
        }
        uint64_t t0 = SDL_GetPerformanceCounter();
        emb_view *v = make_view(rt, dev);
        double create_ms = ms_since(t0);
        if (v == nullptr) {
            printf("bench_view: view create failed\n");
            emb_runtime_destroy(rt);
            break;
        }
        bool loaded = file != nullptr ? emb_view_load_url(v, page_path) : emb_view_load_html(v, page, page_len, nullptr);
        if (!loaded) {
            printf("bench_view: the page did not load\n");
        }
        uint64_t t_end = 0;
        double submit = render_and_wait(dev, v, &t_end);
        fp cur;
        memset(&cur, 0, sizeof cur);
        cur.wall = (double)(t_end - t_start) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        cur.create = create_ms;
        cur.submit = submit;
        cur.st.size = sizeof cur.st;
        emb_view_stats_get(v, &cur.st);
        if (r == 0) {
            cold = cur;
            have_cold = true;
        } else if (cur.wall < warm.wall) {
            warm = cur;
        }
        emb_view_destroy(v);
        emb_runtime_destroy(rt);
    }

    if (have_cold) {
        const emb_view_stats *s = &cold.st;
        double sum = s->runtime_init_ms + cold.create + s->fetch_ms + s->parse_ms + s->stylesheets_ms + s->style_ms +
                     s->layout_ms + s->display_list_ms + s->gpu_ms + cold.submit;
        printf("first paint breakdown, cold start (run 1 of %d, ms):\n", runs);
        printf("  runtime init          %8.2f\n", s->runtime_init_ms);
        printf("  GPU pipelines         %8.2f  (view create %.2f, includes the renderer, atlas and layer pool)\n",
               s->gpu_pipelines_ms, cold.create);
        printf("  document fetch        %8.2f  (the file read)\n", s->fetch_ms);
        printf("  parse                 %8.2f\n", s->parse_ms);
        printf("    of which: HTML parse and the rest %.2f, realm %.2f, script compile %.2f, script run %.2f\n",
               s->parse_ms - s->script_realm_ms - s->script_compile_ms - s->script_run_ms, s->script_realm_ms,
               s->script_compile_ms, s->script_run_ms);
        printf("  stylesheets           %8.2f  (<style> and <link> fetch and CSS parse)\n", s->stylesheets_ms);
        printf("  style                 %8.2f\n", s->style_ms);
        printf("  layout                %8.2f  (fonts %.2f, the first shaping)\n", s->layout_ms, s->fonts_ms);
        printf("  display list          %8.2f\n", s->display_list_ms);
        printf("  GPU record            %8.2f  (image uploads %.2f)\n", s->gpu_ms, s->upload_ms);
        printf("  submit                %8.2f\n", cold.submit);
        printf("  other (not timed)     %8.2f\n", cold.wall - sum);
        printf("  items %u, layers %u, draw calls %u, damage area %" PRIu64 " px\n", s->items, s->layers, s->draw_calls,
               s->damage_area);
        if (warm.wall < 1.0e29) {
            printf("  warm first paint (best of the other %d runs): %.2f ms\n", runs - 1, warm.wall);
        }
        printf("%s first paint %.2f ms wall clock, cold (budget %.0f ms; emb_runtime_create to frame submitted)\n",
               verdict(cold.wall < BUDGET_FIRST_PAINT_MS), cold.wall, BUDGET_FIRST_PAINT_MS);
        if (SDL_strcmp(driver, "vulkan") == 0) {
            printf("bench_view: (software renderer: the GPU record and submit lines are slower than on real hardware)\n");
        }
    }

    /* Scroll: average frame time of a steady scroll over the page. */
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = quiet_log;
    rd.flags = system_fonts ? 0u : EMB_RUNTIME_NO_SYSTEM_FONTS;
    emb_runtime *rt = emb_runtime_create(&rd);
    emb_view *v = rt != nullptr ? make_view(rt, dev) : nullptr;
    if (v != nullptr) {
        bool loaded;
        if (file != nullptr) {
            allow_page_dir(rt, page_path);
            loaded = emb_view_load_url(v, page_path);
        } else {
            loaded = emb_view_load_html(v, page, page_len, nullptr);
        }
        if (loaded) {
            render_and_wait(dev, v, nullptr);
            uint32_t changed = 0;
            uint64_t t0 = SDL_GetPerformanceCounter();
            for (int f = 0; f < frames; f++) {
                /* Down for 8 frames, then up for 8: the page is short, so it turns at both ends and every frame moves. */
                emb_view_scroll_by(v, 0.0f, ((f / 8) & 1) ? -9.0f : 9.0f);
                render_and_wait(dev, v, nullptr);
                emb_view_stats fs;
                memset(&fs, 0, sizeof fs);
                fs.size = sizeof fs;
                emb_view_stats_get(v, &fs);
                if (fs.damage_area > 0) {
                    changed++;
                }
            }
            double frame_avg = ms_since(t0) / (double)frames;
            if (changed == 0) {
                printf("NOT MEASURED scroll frame: the page does not scroll in this viewport (no frame changed)\n");
            } else {
                printf("%s scroll frame %.2f ms average over %d frames, %u of them changed (budget %.2f ms, 60 fps)\n",
                       verdict(frame_avg < BUDGET_FRAME_MS), frame_avg, frames, changed, BUDGET_FRAME_MS);
            }
        }
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);

    /* Heap per view: the live heap after a view is loaded, minus the heap before it. */
    emb_runtime *hrt = emb_runtime_create(&rd);
    size_t before = embi_mem_live();
    emb_view *hv = hrt != nullptr ? make_view(hrt, dev) : nullptr;
    if (hv != nullptr) {
        if (file != nullptr) {
            allow_page_dir(hrt, page_path);
            emb_view_load_url(hv, page_path);
        } else {
            emb_view_load_html(hv, page, page_len, nullptr);
        }
        render_and_wait(dev, hv, nullptr);
    }
    size_t after = embi_mem_live();
    double heap = hv != nullptr && after > before ? (double)(after - before) : 0.0;
    printf("%s heap per view %.2f MB (budget %.0f MB; live heap, not RSS)\n", verdict(heap < BUDGET_HEAP_BYTES),
           heap / (1024.0 * 1024.0), BUDGET_HEAP_BYTES / (1024.0 * 1024.0));
    emb_view_destroy(hv);
    emb_runtime_destroy(hrt);

    if (lib_bytes >= 0.0) {
        printf("%s libember %.0f bytes (budget %.0f bytes)\n", verdict(lib_bytes < BUDGET_LIB_BYTES), lib_bytes,
               BUDGET_LIB_BYTES);
    } else {
        printf("libember size not given (pass --lib-bytes)\n");
    }

    SDL_free(page); /* the file's bytes, or the synthetic page */
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    return 0;
}
