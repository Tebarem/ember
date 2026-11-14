#pragma once
/*
 * bench_view --app (W8b): the project budgets (idle memory, first paint, frame time) measured on a built framework app (tests/apps/<name>/dist
 * reference app is react-todomvc), served from app://local/ through emb_runtime_mount_dir as an embedder would, with a
 * host-provided GPU device. Included by bench_view.c after its helpers (ms_since, render_and_wait, make_view, quiet_log).
 *
 *   first paint     the wall clock from before emb_runtime_create to the submit of the view's first frame after
 *                   emb_view_load_url returned, broken down by the library's stage timers; "scripts and the rest" is what the
 *                   timers do not cover (the app's module scripts run inside the load). Run 1 is the cold start (the
 *                   verdict), the best of the other runs the warm one.
 *   first content   the same clock to the submit of the first frame after the app's UI exists (--ready SELECTOR matches,
 *                   default .todoapp): frameworks that render from a scheduled task (React's scheduler) show their UI a few
 *                   frames after the load.
 *   heap per view   the embi_alloc live bytes a loaded view of the app adds (deterministic: gated in CI).
 *   RSS per view    the resident set (VmRSS of /proc/self/status, Linux only) of the first view with the app, which includes
 *                   the GPU driver's first-use cost, and of each additional view (the average of --extra-views views): the
 *                   the project's "idle RSS per view" is the latter. Measured after the view has idled for 200 ms.
 *   TodoMVC frames  with --todomvc KEY: the app is reloaded with --items N todos in localStorage KEY (default 1000), then
 *                   the list is scrolled (--frames frames); then the app is reloaded with --toggle-items N todos (default
 *                   100) and toggle-all is clicked through the input API (the label of #toggle-all, hit tested as a user's
 *                   click); the frames of the label color transition that follows are paced at 60 Hz for 600 ms and the
 *                   ones that changed are timed. Each frame is the update, the recording, the submit and the wait for the GPU, so on a
 *                   software rasterizer the numbers are a floor. Reported: average, 95th percentile and worst frame against
 *                   16.7 ms (60 fps). The click frame (the framework's re-render of every row and the restyle) is reported
 *                   apart from the animation frames.
 */
#include <ember/ember_input.h>
#include <ember/ember_resource.h>
#include <ember/ember_script.h>

#define APP_MAX_FRAMES 4096

typedef struct app_opts {
    const char *dir;          /* the app's dist/ folder */
    const char *ready;        /* the selector of the app's UI */
    const char *todomvc_key;  /* localStorage key of the TodoMVC list; nullptr: no TodoMVC frames */
    int items;                /* todos of the scroll measurement */
    int toggle_items;         /* todos of the toggle-all measurement */
    int toggle_repeat;        /* further toggle-all clicks after the animation, each with one frame (medians; for profiling) */
    int extra_views;          /* additional views for the RSS per view */
} app_opts;

/* ---- metrics: printed as "METRIC name value unit" lines and written to --metrics FILE ---- */

typedef struct metric {
    char name[48];
    double value;
    char unit[8];
    bool gated; /* deterministic: compared with the baseline and failing CI on a regression of more than 5% */
} metric;

static metric g_metrics[64];
static int g_nmetrics;

static void metric_put(const char *name, double value, const char *unit, bool gated)
{
    if (g_nmetrics == (int)(sizeof g_metrics / sizeof g_metrics[0])) {
        return;
    }
    metric *m = &g_metrics[g_nmetrics++];
    SDL_strlcpy(m->name, name, sizeof m->name);
    m->value = value;
    SDL_strlcpy(m->unit, unit, sizeof m->unit);
    m->gated = gated;
}

/* ---- the eval bridge ---- */

static char g_app_ev[2048];
static bool g_app_ev_ok;

static void app_ev_cb(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    g_app_ev_ok = ok;
    size_t n = len < sizeof g_app_ev - 1 ? len : sizeof g_app_ev - 1;
    memcpy(g_app_ev, text, n);
    g_app_ev[n] = '\0';
}

static const char *app_eval(emb_view *v, const char *js)
{
    g_app_ev[0] = '\0';
    g_app_ev_ok = false;
    (void)emb_view_eval(v, js, strlen(js), app_ev_cb, nullptr);
    return g_app_ev_ok ? g_app_ev : "";
}

/* The resident set in bytes from /proc/self/status (VmRSS), or -1 where there is none (Windows, macOS). */
static double rss_bytes(void)
{
    /* procfs files report a size of 0, so they are read in a loop rather than with SDL_LoadFile */
    SDL_IOStream *io = SDL_IOFromFile("/proc/self/status", "r");
    if (io == nullptr) {
        return -1.0;
    }
    static char buf[8192];
    size_t len = 0;
    for (;;) {
        size_t n = SDL_ReadIO(io, buf + len, sizeof buf - 1 - len);
        if (n == 0) {
            break;
        }
        len += n;
    }
    SDL_CloseIO(io);
    buf[len] = '\0';
    const char *p = SDL_strstr(buf, "VmRSS:");
    return p != nullptr ? SDL_strtod(p + 6, nullptr) * 1024.0 : -1.0;
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct frame_stats {
    int n;
    double avg, p95, max;
} frame_stats;

static frame_stats frame_summary(double *ms, int n)
{
    frame_stats s = {n, 0, 0, 0};
    if (n == 0) {
        return s;
    }
    double sum = 0;
    for (int i = 0; i < n; i++) {
        sum += ms[i];
    }
    SDL_qsort(ms, (size_t)n, sizeof ms[0], cmp_double);
    s.avg = sum / n;
    s.p95 = ms[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
    s.max = ms[n - 1];
    return s;
}

/* The stage times of the frames of one measurement, summed (the view's stats of each frame). */
typedef struct frame_stages {
    double style, layout, dl, record, upload, wall, damage;
    double items, recorded, culled, draws, batches, layers; /* counters, summed over the frames that recorded */
    int reused;                                      /* frames that reused the display list (a pure scroll) */
    int n;
} frame_stages;
static frame_stages g_stages;

static emb_view_stats view_stats(emb_view *v)
{
    emb_view_stats st;
    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    return st;
}

/* One frame: update, record, submit and wait for the GPU. Returns its wall time in ms and adds its stages to g_stages.
 * The stats hold the times of the last layout, paint and recording, so a stage counts for this frame only when its time
 * changed during the frame (a frame that only scrolls does not lay out, and must not repeat the last layout's time). */
static double timed_frame(SDL_GPUDevice *dev, emb_view *v)
{
    const emb_view_stats b = view_stats(v);
    uint64_t t0 = SDL_GetPerformanceCounter();
    render_and_wait(dev, v, nullptr);
    const double ms = ms_since(t0);
    const emb_view_stats st = view_stats(v);
    g_stages.style += st.style_ms != b.style_ms ? st.style_ms : 0.0;
    g_stages.layout += st.layout_ms != b.layout_ms ? st.layout_ms : 0.0;
    g_stages.dl += st.display_list_ms != b.display_list_ms ? st.display_list_ms : 0.0;
    g_stages.record += st.gpu_ms != b.gpu_ms ? st.gpu_ms : 0.0;
    g_stages.upload += st.gpu_ms != b.gpu_ms ? st.upload_ms : 0.0;
    g_stages.damage += st.display_list_ms != b.display_list_ms || st.damage_ms != b.damage_ms ? st.damage_ms : 0.0;
    if (st.gpu_ms != b.gpu_ms) {
        g_stages.items += st.items;
        g_stages.recorded += st.items_recorded;
        g_stages.culled += st.items_culled;
        g_stages.draws += st.draw_calls;
        g_stages.batches += st.batches;
        g_stages.layers += st.layers;
    }
    g_stages.reused += st.scroll_reused && st.gpu_ms != b.gpu_ms ? 1 : 0;
    g_stages.wall += ms;
    g_stages.n++;
    return ms;
}

static void print_stages(void)
{
    const frame_stages *g = &g_stages;
    if (g->n == 0) {
        return;
    }
    const double n = g->n;
    printf("     per frame (ms): style %.2f, layout %.2f, display list %.2f, GPU record %.2f (image uploads %.2f), the rest "
           "%.2f (scripts, animation sampling, the submit and the wait for the GPU)\n",
           g->style / n, g->layout / n, g->dl / n, g->record / n, g->upload / n,
           (g->wall - g->style - g->layout - g->dl - g->record) / n);
    printf("     per frame: display list items %.0f, recorded %.0f, culled %.0f, draw calls %.1f, batches %.1f, layers %.1f; damage "
           "compute %.2f ms; %d of %d frames reused the display list\n",
           g->items / n, g->recorded / n, g->culled / n, g->draws / n, g->batches / n, g->layers / n, g->damage / n, g->reused, g->n);
}

/* Renders frames until the selector matches (or ms elapse). Returns true when it matched; *frames counts the frames. */
static bool pump_until(SDL_GPUDevice *dev, emb_view *v, const char *js_true, int ms, int *frames)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    for (;;) {
        bool ok = SDL_strcmp(app_eval(v, js_true), "true") == 0;
        if (ok) {
            return true;
        }
        if (ms_since(t0) > ms) {
            return false;
        }
        render_and_wait(dev, v, nullptr);
        if (frames != nullptr) {
            (*frames)++;
        }
    }
}

static void pump_ms(SDL_GPUDevice *dev, emb_view *v, int ms)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    while (ms_since(t0) < ms) {
        render_and_wait(dev, v, nullptr);
        SDL_Delay(1);
    }
}

static emb_runtime *app_runtime(const app_opts *o, bool system_fonts)
{
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = quiet_log;
    rd.flags = system_fonts ? 0u : EMB_RUNTIME_NO_SYSTEM_FONTS;
    emb_runtime *rt = emb_runtime_create(&rd);
    if (rt != nullptr && !emb_runtime_mount_dir(rt, "app", "local", o->dir)) {
        printf("bench_view: cannot mount %s as app://local/\n", o->dir);
        emb_runtime_destroy(rt);
        return nullptr;
    }
    return rt;
}

#define APP_URL "app://local/index.html"

static void app_first_paint(SDL_GPUDevice *dev, const app_opts *o, int runs, bool system_fonts)
{
    char ready_js[512];
    SDL_snprintf(ready_js, sizeof ready_js, "String(document.querySelector('%s') !== null)", o->ready);
    typedef struct fp {
        double wall, create, submit, content;
        int content_frames;
        emb_view_stats st;
    } fp;
    fp cold, warm;
    memset(&cold, 0, sizeof cold);
    memset(&warm, 0, sizeof warm);
    warm.wall = 1.0e30;
    double warm_content = 1.0e30;
    bool have_cold = false;
    for (int r = 0; r < runs; r++) {
        uint64_t t_start = SDL_GetPerformanceCounter();
        emb_runtime *rt = app_runtime(o, system_fonts);
        if (rt == nullptr) {
            return;
        }
        uint64_t t0 = SDL_GetPerformanceCounter();
        emb_view *v = make_view(rt, dev);
        double create_ms = ms_since(t0);
        if (v == nullptr) {
            printf("bench_view: view create failed\n");
            emb_runtime_destroy(rt);
            return;
        }
        if (!emb_view_load_url(v, APP_URL)) {
            printf("bench_view: %s did not load from %s\n", APP_URL, o->dir);
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
        /* First content: frames until the UI exists, then one more frame that shows it. */
        int frames = 0;
        if (pump_until(dev, v, ready_js, 3000, &frames)) {
            uint64_t t_c = 0;
            render_and_wait(dev, v, &t_c);
            cur.content = (double)(t_c - t_start) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            cur.content_frames = frames + 1;
        } else {
            cur.content = -1.0;
        }
        if (r == 0) {
            cold = cur;
            have_cold = true;
        } else {
            if (cur.wall < warm.wall) {
                warm = cur;
            }
            if (cur.content >= 0 && cur.content < warm_content) {
                warm_content = cur.content;
            }
        }
        emb_view_destroy(v);
        emb_runtime_destroy(rt);
    }
    if (!have_cold) {
        return;
    }
    const emb_view_stats *s = &cold.st;
    double sum = s->runtime_init_ms + cold.create + s->fetch_ms + s->parse_ms + s->stylesheets_ms + s->style_ms +
                 s->layout_ms + s->display_list_ms + s->gpu_ms + cold.submit;
    printf("app first paint breakdown, cold start (run 1 of %d, ms):\n", runs);
    printf("  runtime init          %8.2f\n", s->runtime_init_ms);
    printf("  view create           %8.2f  (GPU pipelines %.2f: renderer, atlas, layer pool)\n", cold.create,
           s->gpu_pipelines_ms);
    printf("  document fetch        %8.2f  (app:// handler)\n", s->fetch_ms);
    printf("  parse                 %8.2f\n", s->parse_ms);
    printf("    of which: HTML parse and the rest %.2f, realm %.2f, script compile %.2f, script run %.2f\n",
           s->parse_ms - s->script_realm_ms - s->script_compile_ms - s->script_run_ms, s->script_realm_ms,
           s->script_compile_ms, s->script_run_ms);
    printf("  stylesheets           %8.2f\n", s->stylesheets_ms);
    printf("  style                 %8.2f\n", s->style_ms);
    printf("  layout                %8.2f  (fonts %.2f)\n", s->layout_ms, s->fonts_ms);
    printf("  display list          %8.2f\n", s->display_list_ms);
    printf("  GPU record            %8.2f  (image uploads %.2f)\n", s->gpu_ms, s->upload_ms);
    printf("  submit                %8.2f\n", cold.submit);
    printf("  scripts and the rest  %8.2f  (module scripts, script fetches, realm setup: not timed by stage)\n",
           cold.wall - sum);
    printf("  items %u, layers %u, draw calls %u\n", s->items, s->layers, s->draw_calls);
    if (warm.wall < 1.0e29) {
        printf("  warm first paint (best of the other %d runs): %.2f ms\n", runs - 1, warm.wall);
    }
    printf("%s app first paint %.2f ms wall clock, cold (budget %.0f ms; emb_runtime_create to frame submitted)\n",
           verdict(cold.wall < BUDGET_FIRST_PAINT_MS), cold.wall, BUDGET_FIRST_PAINT_MS);
    if (cold.content >= 0) {
        printf("%s app first content %.2f ms cold, %.2f ms warm (%d frames until %s exists)\n",
               verdict(cold.content < BUDGET_FIRST_PAINT_MS), cold.content, warm_content < 1.0e29 ? warm_content : -1.0,
               cold.content_frames, o->ready);
    } else {
        printf("FAIL app first content: %s never appeared within 3 s\n", o->ready);
    }
    metric_put("first_paint_cold_ms", cold.wall, "ms", false);
    metric_put("first_paint_warm_ms", warm.wall < 1.0e29 ? warm.wall : cold.wall, "ms", false);
    metric_put("first_content_cold_ms", cold.content, "ms", false);
    metric_put("stage_view_create_ms", cold.create, "ms", false);
    metric_put("stage_parse_ms", s->parse_ms, "ms", false);
    metric_put("stage_style_layout_ms", s->style_ms + s->layout_ms, "ms", false);
    metric_put("stage_scripts_rest_ms", cold.wall - sum, "ms", false);
}

static void app_memory(SDL_GPUDevice *dev, const app_opts *o, bool system_fonts)
{
    char ready_js[512];
    SDL_snprintf(ready_js, sizeof ready_js, "String(document.querySelector('%s') !== null)", o->ready);
    emb_runtime *rt = app_runtime(o, system_fonts);
    if (rt == nullptr) {
        return;
    }
    const double rss0 = rss_bytes();
    const size_t heap0 = embi_mem_live();
    emb_view *v = make_view(rt, dev);
    if (v == nullptr || !emb_view_load_url(v, APP_URL)) {
        printf("bench_view: the app did not load for the memory measure\n");
        emb_view_destroy(v);
        emb_runtime_destroy(rt);
        return;
    }
    (void)pump_until(dev, v, ready_js, 3000, nullptr);
    pump_ms(dev, v, 200);
    const size_t heap1 = embi_mem_live();
    const double rss1 = rss_bytes();
    double heap = heap1 > heap0 ? (double)(heap1 - heap0) : 0.0;
    printf("%s app heap per view %.2f MB (budget %.0f MB; embi_alloc live bytes of a loaded, idle view)\n",
           verdict(heap < BUDGET_HEAP_BYTES), heap / 1048576.0, BUDGET_HEAP_BYTES / 1048576.0);
    metric_put("heap_per_view_bytes", heap, "B", true);

    emb_view *extra[16];
    int n_extra = o->extra_views < 16 ? o->extra_views : 16;
    int made = 0;
    for (int i = 0; i < n_extra; i++) {
        extra[made] = make_view(rt, dev);
        if (extra[made] == nullptr) {
            break;
        }
        (void)emb_view_load_url(extra[made], APP_URL);
        (void)pump_until(dev, extra[made], ready_js, 3000, nullptr);
        made++;
    }
    for (int i = 0; i < made; i++) {
        pump_ms(dev, extra[i], 200 / (made > 0 ? made : 1));
    }
    const double rss2 = rss_bytes();
    if (rss0 >= 0 && rss1 >= 0 && rss2 >= 0) {
        const double first = rss1 - rss0;
        const double each = made > 0 ? (rss2 - rss1) / made : 0.0;
        printf("     app RSS of the first view %.2f MB (includes the GPU driver's first-use allocations)\n",
               first / 1048576.0);
        printf("%s app idle RSS per view %.2f MB (budget %.0f MB; the average of %d additional views, each loaded and idle)\n",
               verdict(each < BUDGET_HEAP_BYTES), each / 1048576.0, BUDGET_HEAP_BYTES / 1048576.0, made);
        printf("     process RSS %.2f MB before the first view, %.2f MB with %d views\n", rss0 / 1048576.0,
               rss2 / 1048576.0, made + 1);
        metric_put("rss_first_view_bytes", first, "B", false);
        metric_put("rss_per_view_bytes", each, "B", false);
    } else {
        printf("NOT MEASURED app idle RSS per view: no /proc/self/status on this platform\n");
    }
    for (int i = 0; i < made; i++) {
        emb_view_destroy(extra[i]);
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
}

/* The centre of the first element matching sel, in view coordinates. */
static bool app_center(emb_view *v, const char *sel, float *x, float *y)
{
    char js[512];
    SDL_snprintf(js, sizeof js,
                 "(function(){var e=document.querySelector('%s');if(!e)return '';var r=e.getBoundingClientRect();"
                 "return (r.left+r.width/2)+','+(r.top+r.height/2)})()",
                 sel);
    const char *r = app_eval(v, js);
    return SDL_sscanf(r, "%f,%f", x, y) == 2;
}

static void print_frames(const char *what, frame_stats s, int changed, const char *metric_prefix)
{
    print_stages();
    const bool ok = s.avg < BUDGET_FRAME_MS && s.p95 < BUDGET_FRAME_MS;
    printf("%s %s: average %.2f ms, p95 %.2f ms, worst %.2f ms over %d frames (%d changed; budget %.2f ms, 60 fps)\n",
           s.n > 0 ? verdict(ok) : "NOT MEASURED", what, s.avg, s.p95, s.max, s.n, changed, BUDGET_FRAME_MS);
    char name[48];
    SDL_snprintf(name, sizeof name, "%s_avg_ms", metric_prefix);
    metric_put(name, s.avg, "ms", false);
    SDL_snprintf(name, sizeof name, "%s_p95_ms", metric_prefix);
    metric_put(name, s.p95, "ms", false);
}

/* A view of the app showing n todos: the list is written to localStorage (the TodoMVC shape) from a first view, then a
 * second view loads it (the runtime keeps localStorage). nullptr when the app did not show them. */
static emb_view *todomvc_view(SDL_GPUDevice *dev, emb_runtime *rt, const app_opts *o, int n)
{
    char ready_js[512];
    SDL_snprintf(ready_js, sizeof ready_js, "String(document.querySelector('%s') !== null)", o->ready);
    emb_view *v = make_view(rt, dev);
    if (v == nullptr || !emb_view_load_url(v, APP_URL) || !pump_until(dev, v, ready_js, 3000, nullptr)) {
        printf("NOT MEASURED TodoMVC frames: the app did not start\n");
        emb_view_destroy(v);
        return nullptr;
    }
    char js[512];
    SDL_snprintf(js, sizeof js,
                 "localStorage.setItem('%s', JSON.stringify(Array.from({length: %d}, function (_, i) {"
                 " return {id: 'b' + i, title: 'Bench item ' + i, completed: false}; }))); 'ok'",
                 o->todomvc_key, n);
    (void)app_eval(v, js);
    emb_view_destroy(v);
    v = make_view(rt, dev);
    char count_js[256];
    SDL_snprintf(count_js, sizeof count_js, "String(document.querySelectorAll('.todo-list li').length === %d)", n);
    uint64_t t_load = SDL_GetPerformanceCounter();
    if (v == nullptr || !emb_view_load_url(v, APP_URL) || !pump_until(dev, v, count_js, 10000, nullptr)) {
        printf("NOT MEASURED TodoMVC frames: the app did not show the %d items of %s\n", n, o->todomvc_key);
        emb_view_destroy(v);
        return nullptr;
    }
    printf("     TodoMVC with %d items: loaded and shown in %.1f ms\n", n, ms_since(t_load));
    pump_ms(dev, v, 100);
    return v;
}

static void app_todomvc_frames(SDL_GPUDevice *dev, const app_opts *o, int frames, bool system_fonts)
{
    static double ms[APP_MAX_FRAMES];
    emb_runtime *rt = app_runtime(o, system_fonts);
    if (rt == nullptr) {
        return;
    }
    /* Scrolling a list of o->items: 16 frames down, 16 up, so every frame moves. */
    emb_view *v = todomvc_view(dev, rt, o, o->items);
    if (v != nullptr) {
        int changed = 0, n = frames < APP_MAX_FRAMES ? frames : APP_MAX_FRAMES;
        memset(&g_stages, 0, sizeof g_stages);
        for (int f = 0; f < n; f++) {
            emb_view_scroll_by(v, 0.0f, ((f / 16) & 1) ? -40.0f : 40.0f);
            ms[f] = timed_frame(dev, v);
            emb_view_stats fs;
            memset(&fs, 0, sizeof fs);
            fs.size = sizeof fs;
            emb_view_stats_get(v, &fs);
            changed += fs.damage_area > 0;
            if (SDL_getenv("BENCH_TRACE") != nullptr) {
                printf("       frame %d: %.2f ms, display list %.2f, damage %.2f, record %.2f, reused %d, rects %u, full %d\n", f, ms[f],
                       fs.display_list_ms, fs.damage_ms, fs.gpu_ms, fs.scroll_reused, fs.damage_rects, fs.full_redraw);
            }
        }
        char what[96];
        SDL_snprintf(what, sizeof what, "TodoMVC scroll of %d items", o->items);
        print_frames(what, frame_summary(ms, n), changed, "scroll");
        emb_view_destroy(v);
    }

    /* Toggle-all on a list of o->toggle_items: a user's click on the label of #toggle-all, then the color transition of
     * every label (todo.css: transition: color 0.4s). */
    v = todomvc_view(dev, rt, o, o->toggle_items);
    float x = 0, y = 0;
    if (v != nullptr && !app_center(v, "label[for=toggle-all]", &x, &y)) {
        printf("NOT MEASURED TodoMVC toggle-all: no label[for=toggle-all]\n");
    } else if (v != nullptr) {
        emb_view_mouse_move(v, x, y, 0);
        (void)timed_frame(dev, v);
        memset(&g_stages, 0, sizeof g_stages);
        uint64_t t0 = SDL_GetPerformanceCounter();
        emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
        const double dispatch_ms = ms_since(t0);
        (void)timed_frame(dev, v);
        const double click_ms = ms_since(t0);
        printf("%s TodoMVC toggle-all click frame %.2f ms with %d items (the input, the framework's re-render of every "
               "row, the restyle and the frame; budget %.2f ms)\n",
               verdict(click_ms < BUDGET_FRAME_MS), click_ms, o->toggle_items, BUDGET_FRAME_MS);
        printf("     of which: input dispatch (the events and the framework's re-render) %.2f ms, the frame %.2f ms\n",
               dispatch_ms, click_ms - dispatch_ms);
        print_stages();
        metric_put("toggle_all_click_ms", click_ms, "ms", false);
        metric_put("toggle_all_dispatch_ms", dispatch_ms, "ms", false);
        /* Frames paced at 60 Hz, as a display would ask for them; the frames that changed (the transition's steps) are
         * the ones measured. */
        memset(&g_stages, 0, sizeof g_stages);
        int an = 0, paced = 0;
        uint64_t ta = SDL_GetPerformanceCounter();
        while (ms_since(ta) < 600.0 && an < APP_MAX_FRAMES) {
            const double slot = (paced + 1) * BUDGET_FRAME_MS;
            paced++;
            const frame_stages before = g_stages;
            const double fms = timed_frame(dev, v);
            emb_view_stats fs;
            memset(&fs, 0, sizeof fs);
            fs.size = sizeof fs;
            emb_view_stats_get(v, &fs);
            if (fs.damage_area > 0) {
                ms[an++] = fms;
            } else {
                g_stages = before; /* an unchanged frame is not part of the animation's numbers */
            }
            const double left = slot - ms_since(ta);
            if (left > 0) {
                SDL_DelayNS((Uint64)(left * 1e6));
            }
        }
        const int changed = an;
        const char *done = app_eval(v, "String(document.querySelectorAll('.todo-list li.completed').length)");
        char what[128];
        SDL_snprintf(what, sizeof what, "TodoMVC toggle-all animation frames (%d items, %s completed)", o->toggle_items,
                     done);
        print_frames(what, frame_summary(ms, an), changed, "toggle_all_anim");
        /* --toggle-repeat N: N more clicks, each timed as the first one (the input, then one frame), after the transitions
         * of the one before have finished (600 ms of frames, not timed). For a steadier median and for profiling. */
        if (o->toggle_repeat > 0) {
            static double rd[APP_MAX_FRAMES], rf[APP_MAX_FRAMES];
            const int rn = o->toggle_repeat < APP_MAX_FRAMES ? o->toggle_repeat : APP_MAX_FRAMES;
            frame_stages rs;
            memset(&rs, 0, sizeof rs);
            for (int r = 0; r < rn; r++) {
                pump_ms(dev, v, 600);
                memset(&g_stages, 0, sizeof g_stages);
                uint64_t tr = SDL_GetPerformanceCounter();
                emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
                emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
                rd[r] = ms_since(tr);
                rf[r] = timed_frame(dev, v);
                rs.style += g_stages.style;
                rs.layout += g_stages.layout;
                rs.dl += g_stages.dl;
                rs.record += g_stages.record;
                rs.upload += g_stages.upload;
                rs.damage += g_stages.damage;
                rs.wall += g_stages.wall;
                rs.items += g_stages.items;
                rs.recorded += g_stages.recorded;
                rs.culled += g_stages.culled;
                rs.draws += g_stages.draws;
                rs.batches += g_stages.batches;
                rs.layers += g_stages.layers;
                rs.reused += g_stages.reused;
                rs.n += g_stages.n;
            }
            static double rt_[APP_MAX_FRAMES];
            for (int r = 0; r < rn; r++) {
                rt_[r] = rd[r] + rf[r];
            }
            SDL_qsort(rd, (size_t)rn, sizeof rd[0], cmp_double);
            SDL_qsort(rf, (size_t)rn, sizeof rf[0], cmp_double);
            SDL_qsort(rt_, (size_t)rn, sizeof rt_[0], cmp_double);
            printf("     %d more toggle-all clicks: click frame median %.2f ms (min %.2f, max %.2f); input dispatch median %.2f ms "
                   "(min %.2f, max %.2f), frame median %.2f ms (min %.2f, max %.2f)\n",
                   rn, rt_[rn / 2], rt_[0], rt_[rn - 1], rd[rn / 2], rd[0], rd[rn - 1], rf[rn / 2], rf[0], rf[rn - 1]);
            g_stages = rs;
            print_stages();
        }
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
}

/* ---- the baseline gate ---- */

/* Writes the metrics as "key name value" lines. */
static void metrics_write(const char *path, const char *key)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "w");
    if (io == nullptr) {
        printf("bench_view: cannot write %s\n", path);
        return;
    }
    for (int i = 0; i < g_nmetrics; i++) {
        SDL_IOprintf(io, "%s %s %.6g\n", key, g_metrics[i].name, g_metrics[i].value);
    }
    SDL_CloseIO(io);
}

/* The baseline value of metric name under key in text ("key name value" lines, # comments), or -1. */
static double baseline_find(const char *text, const char *key, const char *name)
{
    const size_t klen = SDL_strlen(key), nlen = SDL_strlen(name);
    double base = -1.0;
    for (const char *line = text; line != nullptr && *line != '\0';) {
        const char *eol = SDL_strchr(line, '\n');
        if (line[0] != '#' && SDL_strncmp(line, key, klen) == 0 && line[klen] == ' ' &&
            SDL_strncmp(line + klen + 1, name, nlen) == 0 && line[klen + 1 + nlen] == ' ') {
            base = SDL_strtod(line + klen + 1 + nlen + 1, nullptr);
        }
        line = eol != nullptr ? eol + 1 : nullptr;
    }
    return base;
}

/* Compares the metrics with the baseline lines of key, or of the key "any" for a metric key has no line for (the heap per
 * view does not depend on the compiler or the GPU, so one line serves every runner). A gated metric more than 5% above
 * its baseline fails; the others are printed with their change. Returns the number of failures. Metrics with no baseline
 * pass, and the lines to add are printed. */
static int baseline_gate(const char *path, const char *key)
{
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    if (text == nullptr) {
        printf("GATE no baseline file %s: nothing compared\n", path);
        return 0;
    }
    int failures = 0, found = 0;
    for (int i = 0; i < g_nmetrics; i++) {
        const metric *m = &g_metrics[i];
        double base = baseline_find(text, key, m->name);
        if (base < 0) {
            base = baseline_find(text, "any", m->name);
        }
        if (base < 0) {
            continue;
        }
        found++;
        const double change = base > 0 ? (m->value - base) / base * 100.0 : 0.0;
        const bool regressed = m->value > base * 1.05 && m->value - base > 1e-9;
        if (m->gated) {
            printf("GATE %s %s: %.6g, baseline %.6g (%+.1f%%)\n", regressed ? "FAIL" : "PASS", m->name, m->value, base,
                   change);
            failures += regressed;
        } else {
            printf("GATE info %s: %.6g, baseline %.6g (%+.1f%%)%s\n", m->name, m->value, base, change,
                   regressed ? " slower by more than 5% (wall clock: reported, not enforced)" : "");
        }
    }
    if (found < g_nmetrics) {
        printf("GATE %d of %d metrics have no baseline for key %s in %s; the lines to add:\n", g_nmetrics - found,
               g_nmetrics, key, path);
        for (int i = 0; i < g_nmetrics; i++) {
            if (baseline_find(text, key, g_metrics[i].name) < 0 && baseline_find(text, "any", g_metrics[i].name) < 0) {
                printf("%s %s %.6g\n", key, g_metrics[i].name, g_metrics[i].value);
            }
        }
    }
    SDL_free(text);
    return failures;
}
