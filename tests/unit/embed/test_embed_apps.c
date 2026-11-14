/*
 * Canary for the framework apps of tests/apps (W8a).
 *
 * For each built app (tests/apps/<name>/dist/, made by tools/apps/build.sh) this loads
 * app://local/index.html through an offscreen view, keeps rendering the view for a bounded time (PUMP_MS, so
 * timers, fetches and lazy imports can finish), and reports:
 *   - whether the document loaded, whether it painted, and how many display list items the last frame had.
 *     "Painted" means more items than a blank page with the apps' skeleton (a root div). Without scripts an app
 *     shows only what its CSS paints (one item, its body background), so "painted" alone is not proof that the UI
 *     rendered; the scenario steps of W8b are that proof;
 *   - the error lines the library logged while the app ran, counted and printed in full (multi-line messages
 *     with their stacks included) by kind:
 *       "uncaught exception: ..."   an uncaught exception from a script,
 *       "unhandled rejection: ..."  a promise rejected with no handler,
 *       "console.error: ..."        a console.error call,
 *       anything else               other errors of the library (for example a failed view creation);
 *   - the number of warning lines;
 *   - a sorted summary of the missing APIs named in the errors ("X is not a function", "X is not defined", "Cannot read
 *     properties of undefined (reading 'Y')"), most frequent first, so it shows what blocks which framework.
 *
 * The prefixes above are the contract of the script layer (W5c) for the script errors it logs through the runtime's log
 * callback.
 *
 * The scenario steps run in the second test of this executable, embed_apps_scenarios (test_embed_apps_player.h).
 *
 * Since W8b the test is strict by default (EMBER_APPS_STRICT=0 makes it a report that always passes): every app must be
 * built, load, paint and log no error, and every step of its scenario must pass (embed_apps_scenarios). The test also runs a second pass on a fresh runtime
 * with one frame per app and requires the embi_alloc live bytes to be the same as after the first pass (a per-run
 * leak check; process-wide tables such as atoms made by the first pass are not counted again). Without a video
 * subsystem or GPU device the test skips. The whole run stays under 5 s (about 1.5 s in debug, 3 s under ASan).
 */
#include "emb_test.h"

#include "base/base.h"
#include "ember/ember.h"
#include "gpu/gpu.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define APP_URL_PREFIX "app://local/"
#define APP_URL_PREFIX_LEN ((int)(sizeof APP_URL_PREFIX - 1))

/* Wall time the view keeps rendering after each load (scaled under AddressSanitizer). */
#if EMB_TEST_ASAN
#define PUMP_MS 200
#else
#define PUMP_MS 50
#endif

/* Strict mode (W8b): the default. Every app must be built, load, paint and log no error, and every scenario step must pass.
 * EMBER_APPS_STRICT=0 turns it off for a report run (the test then always passes and prints how far each app got). */
static bool apps_strict(void)
{
    const char *env = SDL_getenv("EMBER_APPS_STRICT");
    return env == nullptr || SDL_strcmp(env, "0") != 0;
}

/* Room for one error message, stack included; longer messages are cut and marked. */
#define MSG_MAX 4096

enum {
    CAT_EXCEPTION = 0,
    CAT_REJECTION,
    CAT_CONSOLE,
    CAT_OTHER,
    CAT_COUNT,
};

static const char *const k_cat_title[CAT_COUNT] = {
    "uncaught exception",
    "unhandled rejection",
    "console.error",
    "other error",
};

static const char *const k_apps[] = {
    "preact-todomvc", "react-todomvc", "vue-todomvc", "svelte-todomvc",
    "solid-todomvc",  "react-router",  "vite-vanilla",
};
#define APP_COUNT ((int)(sizeof k_apps / sizeof k_apps[0]))

/* Serves the files of one app's dist/ folder under app://local/. */
typedef struct app_files {
    char dir[1024];
    void *last; /* the file handed to the library last; freed when the next one replaces it */
} app_files;

typedef struct app_report {
    const char *name;
    bool loaded;
    bool painted;
    int changed_frames;
    uint32_t items;
    int warnings;
    int counts[CAT_COUNT];
    bool cut[CAT_COUNT];
    char first[CAT_COUNT][MSG_MAX]; /* the first message of each kind, in full */
} app_report;

/* One missing-API entry of the summary. */
typedef struct missing_api {
    char key[160];
    int count;
    uint32_t app_mask;
} missing_api;
#define MISSING_MAX 128
static missing_api g_missing[MISSING_MAX];
static int g_missing_len;
static int g_missing_dropped;

/* The report of the app that is running: the library's log callback writes into it. */
static app_report *g_cur_report;
static uint32_t g_cur_app;

static void note_missing(const char *key, size_t len, uint32_t app_index)
{
    if (len == 0 || len >= sizeof g_missing[0].key) {
        return;
    }
    for (int i = 0; i < g_missing_len; i++) {
        if (SDL_strncmp(g_missing[i].key, key, len) == 0 && g_missing[i].key[len] == '\0') {
            g_missing[i].count++;
            g_missing[i].app_mask |= 1u << app_index;
            return;
        }
    }
    if (g_missing_len == MISSING_MAX) {
        g_missing_dropped++;
        return;
    }
    missing_api *m = &g_missing[g_missing_len++];
    SDL_memcpy(m->key, key, len);
    m->key[len] = '\0';
    m->count = 1;
    m->app_mask = 1u << app_index;
}

/* "document.createElement is not a function" -> "document.createElement", "X is not defined" -> "X (not defined)". Works
 * on multi-line messages too. */
static void parse_missing(const char *msg, uint32_t app_index)
{
    static const char k_reading[] = "Cannot read properties of undefined (reading '";
    const char *not_fn = SDL_strstr(msg, " is not a function");
    if (not_fn != nullptr) {
        const char *start = not_fn;
        while (start > msg && (SDL_isalnum((unsigned char)start[-1]) || start[-1] == '.' || start[-1] == '_' ||
                               start[-1] == '$')) {
            start--;
        }
        note_missing(start, (size_t)(not_fn - start), app_index);
    }
    /* "ReferenceError: SVGElement is not defined" -> "SVGElement (not defined)": a missing global. */
    const char *not_def = SDL_strstr(msg, " is not defined");
    if (not_def != nullptr) {
        const char *start = not_def;
        while (start > msg && (SDL_isalnum((unsigned char)start[-1]) || start[-1] == '_' || start[-1] == '$')) {
            start--;
        }
        char key[160];
        int n = SDL_snprintf(key, sizeof key, "%.*s (not defined)", (int)(not_def - start), start);
        if (not_def > start && n > 0 && (size_t)n < sizeof key) {
            note_missing(key, (size_t)n, app_index);
        }
    }
    /* "Cannot read properties of undefined (reading 'Y')" -> "read 'Y' of undefined". */
    const char *reading = SDL_strstr(msg, k_reading);
    if (reading != nullptr) {
        const char *name = reading + (sizeof k_reading - 1);
        const char *end = SDL_strstr(name, "')");
        if (end != nullptr) {
            char key[160];
            int n = SDL_snprintf(key, sizeof key, "read '%.*s' of undefined", (int)(end - name), name);
            if (n > 0 && (size_t)n < sizeof key) {
                note_missing(key, (size_t)n, app_index);
            }
        }
    }
}

/* The kind of an error line, by the prefix the script layer gives it (see the header). */
static int classify(const char *msg)
{
    if (SDL_strncmp(msg, "uncaught exception", SDL_strlen("uncaught exception")) == 0) {
        return CAT_EXCEPTION;
    }
    if (SDL_strncmp(msg, "unhandled rejection", SDL_strlen("unhandled rejection")) == 0) {
        return CAT_REJECTION;
    }
    if (SDL_strncmp(msg, "console.error", SDL_strlen("console.error")) == 0) {
        return CAT_CONSOLE;
    }
    return CAT_OTHER;
}

static void on_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    app_report *r = g_cur_report;
    if (r == nullptr) {
        return;
    }
    if (level == EMB_LOG_ERROR) {
        int cat = classify(message);
        if (r->counts[cat] == 0) {
            SDL_snprintf(r->first[cat], MSG_MAX, "%s", message);
            r->cut[cat] = SDL_strlen(message) >= MSG_MAX;
        }
        r->counts[cat]++;
        parse_missing(message, g_cur_app);
    } else if (level == EMB_LOG_WARN) {
        r->warnings++;
    }
}

/* A path below the app's folder: no drive or absolute paths, no backslashes, no percent escapes (not decoded, so
 * refused), and no ".." segment. A name such as "a..b" is an ordinary file name. */
static bool safe_rel_path(const char *rel, size_t len)
{
    if (len == 0 || rel[0] == '/') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (rel[i] == '%' || rel[i] == '\\' || rel[i] == ':') {
            return false;
        }
    }
    size_t seg = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || rel[i] == '/') {
            if (i - seg == 2 && rel[seg] == '.' && rel[seg + 1] == '.') {
                return false;
            }
            seg = i + 1;
        }
    }
    return true;
}

/* The resource handler: app://local/<path> is read from the app's dist/ folder. */
static bool serve_app(void *user, const char *url, size_t url_len, emb_resource *out)
{
    app_files *files = user;
    if (url_len < (size_t)APP_URL_PREFIX_LEN || SDL_strncmp(url, APP_URL_PREFIX, APP_URL_PREFIX_LEN) != 0) {
        return false;
    }
    const char *rel = url + APP_URL_PREFIX_LEN;
    size_t rel_len = url_len - (size_t)APP_URL_PREFIX_LEN;
    for (size_t i = 0; i < rel_len; i++) {
        if (rel[i] == '?' || rel[i] == '#') {
            rel_len = i;
            break;
        }
    }
    if (rel_len == 0) {
        rel = "index.html";
        rel_len = SDL_strlen(rel);
    }
    if (!safe_rel_path(rel, rel_len)) {
        return false;
    }
    char path[1200];
    int n = SDL_snprintf(path, sizeof path, "%s/%.*s", files->dir, (int)rel_len, rel);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return false;
    }
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    if (data == nullptr) {
        return false;
    }
    SDL_free(files->last);
    files->last = data;
    out->data = data;
    out->len = size;
    return true;
}

static bool file_exists(const char *path)
{
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    SDL_free(data);
    return data != nullptr;
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

/* Prints a message line by line, so a stack is shown in full, each line under the app's name. */
static void print_message(const char *app, const char *msg)
{
    const char *line = msg;
    while (*line != '\0') {
        const char *end = SDL_strchr(line, '\n');
        size_t len = end != nullptr ? (size_t)(end - line) : SDL_strlen(line);
        SDL_Log("APP %-16s |   %.*s", app, (int)len, line);
        if (end == nullptr) {
            break;
        }
        line = end + 1;
    }
}

static int compare_missing(const void *a, const void *b)
{
    const missing_api *x = a;
    const missing_api *y = b;
    if (x->count != y->count) {
        return y->count - x->count;
    }
    return SDL_strcmp(x->key, y->key);
}

static void print_missing_summary(void)
{
    if (g_missing_len == 0) {
        SDL_Log("APPS missing APIs: none reported");
        return;
    }
    SDL_qsort(g_missing, (size_t)g_missing_len, sizeof g_missing[0], compare_missing);
    SDL_Log("APPS missing APIs (count, apps, name), most frequent first:");
    for (int i = 0; i < g_missing_len; i++) {
        char apps[256] = "";
        size_t used = 0;
        for (int a = 0; a < APP_COUNT; a++) {
            if ((g_missing[i].app_mask & (1u << a)) != 0 && used < sizeof apps) {
                int n = SDL_snprintf(apps + used, sizeof apps - used, "%s%s", used ? "," : "", k_apps[a]);
                used += (n > 0) ? (size_t)n : 0;
            }
        }
        SDL_Log("APPS   %4d  [%s]  %s", g_missing[i].count, apps, g_missing[i].key);
    }
    if (g_missing_dropped > 0) {
        SDL_Log("APPS   (%d more reports not listed: the table holds %d names)", g_missing_dropped, MISSING_MAX);
    }
}

/* Creates an offscreen view of the runtime with the canary's size and scale. */
static emb_view *make_view(SDL_GPUDevice *dev, emb_runtime *rt)
{
    emb_view_desc vd;
    SDL_memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 800;
    vd.height = 600;
    vd.scale = 1.0f;
    vd.device = dev;
    return emb_view_create(rt, &vd);
}

/* The display list items of a blank page with the apps' skeleton (a root div, no script, no CSS): an app has
 * painted something beyond its skeleton only when its frame has more items than this. */
static uint32_t blank_items(SDL_GPUDevice *dev, emb_runtime *rt)
{
    static const char k_blank[] = "<!doctype html><title>blank</title><body><div id=\"root\"></div></body>";
    emb_view *v = make_view(dev, rt);
    if (v == nullptr) {
        return 0;
    }
    (void)emb_view_load_html(v, k_blank, sizeof k_blank - 1, nullptr);
    (void)render_now(dev, v);
    emb_view_stats st;
    SDL_memset(&st, 0, sizeof st);
    st.size = sizeof st;
    emb_view_stats_get(v, &st);
    emb_view_destroy(v);
    return st.items;
}

/* Loads one app, keeps its view rendering for PUMP_MS, and fills rep. Returns false when the app is not built. */
static bool run_app(SDL_GPUDevice *dev, emb_runtime *rt, int a, uint32_t blank, uint32_t pump_ms, app_report *rep)
{
    g_cur_app = (uint32_t)a;
    app_files files;
    SDL_memset(&files, 0, sizeof files);
    SDL_snprintf(files.dir, sizeof files.dir, "%s/tests/apps/%s/dist", EMBER_SOURCE_DIR, k_apps[a]);

    char index_path[1280];
    SDL_snprintf(index_path, sizeof index_path, "%s/index.html", files.dir);
    if (!file_exists(index_path)) {
        SDL_Log("APP %-16s not built: %s missing (run tools/apps/build.sh)", k_apps[a], index_path);
        return false;
    }

    EMB_CHECK(emb_runtime_set_resource_handler(rt, "app", serve_app, &files));
    emb_view *v = make_view(dev, rt);
    if (v != nullptr) {
        g_cur_report = rep;
        rep->loaded = emb_view_load_url(v, APP_URL_PREFIX "index.html");
        if (rep->loaded) {
            const uint64_t t0 = SDL_GetTicksNS();
            const uint64_t budget_ns = (uint64_t)pump_ms * 1000000u;
            for (;;) {
                if (render_now(dev, v)) {
                    rep->changed_frames++;
                }
                if (SDL_GetTicksNS() - t0 >= budget_ns) {
                    break;
                }
                SDL_Delay(1);
            }
            emb_view_stats st;
            SDL_memset(&st, 0, sizeof st);
            st.size = sizeof st;
            emb_view_stats_get(v, &st);
            rep->items = st.items;
            rep->painted = rep->changed_frames > 0 && st.items > blank;
        }
        g_cur_report = nullptr;
        emb_view_destroy(v);
    }
    EMB_CHECK(emb_runtime_set_resource_handler(rt, "app", nullptr, nullptr));
    SDL_free(files.last);
    return true;
}

EMB_TEST(embed_apps_canary)
{
    if (SDL_getenv("EMBER_APPS_ONLY") != nullptr) {
        EMB_SKIP("EMBER_APPS_ONLY runs the scenario player alone");
    }
    /* The GPU device needs the video subsystem, which the test initializes itself (as the GPU tests do). */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    const uint64_t t_start = SDL_GetTicksNS();
    const size_t mem_base = embi_mem_live();

    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        SDL_Quit();
        EMB_SKIP("no GPU device");
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);

    const bool strict = apps_strict();

    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = on_log;
    emb_runtime *rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);

    const uint32_t blank = blank_items(dev, rt);
    SDL_Log("APPS framework canary (tests/apps, dist/ built by tools/apps/build.sh); strict=%s", strict ? "yes" : "no");
    SDL_Log("APPS each view renders for %d ms after its load so timers, fetches and lazy imports can run.", PUMP_MS);
    SDL_Log("APPS painted = the frame has more display list items than a blank document (%u).", blank);

    /* First pass: the report. */
    for (int a = 0; a < APP_COUNT; a++) {
        app_report rep;
        SDL_memset(&rep, 0, sizeof rep);
        rep.name = k_apps[a];
        const bool built = run_app(dev, rt, a, blank, PUMP_MS, &rep);
        g_cur_report = nullptr;
        if (built) {
            int errors = 0;
            for (int c = 0; c < CAT_COUNT; c++) {
                errors += rep.counts[c];
            }
            SDL_Log("APP %-16s loaded=%s painted=%s items=%u errors=%d (exceptions=%d rejections=%d console=%d "
                    "other=%d) warnings=%d scenario=see the SCEN lines of embed_apps_scenarios",
                    k_apps[a], rep.loaded ? "yes" : "no", rep.painted ? "yes" : "no", rep.items, errors,
                    rep.counts[CAT_EXCEPTION], rep.counts[CAT_REJECTION], rep.counts[CAT_CONSOLE],
                    rep.counts[CAT_OTHER], rep.warnings);
            for (int c = 0; c < CAT_COUNT; c++) {
                if (rep.counts[c] > 0) {
                    SDL_Log("APP %-16s first %s%s:", k_apps[a], k_cat_title[c], rep.cut[c] ? " (cut)" : "");
                    print_message(k_apps[a], rep.first[c]);
                }
            }
        }
        if (strict) {
            EMB_CHECK(built);
            EMB_CHECK(rep.loaded);
            EMB_CHECK(rep.painted);
            int errors = 0;
            for (int c = 0; c < CAT_COUNT; c++) {
                errors += rep.counts[c];
            }
            EMB_CHECK(errors == 0);
        }
    }
    print_missing_summary();
    emb_runtime_destroy(rt);
    const size_t live_first = embi_mem_live();

    /* Second pass on a fresh runtime, one frame per app and no pump: whatever it leaves behind is a leak of its
     * own. Atoms and other process-wide tables made by the first pass are not counted again. */
    rt = emb_runtime_create(&rd);
    EMB_REQUIRE(rt != nullptr);
    app_report quiet;
    for (int a = 0; a < APP_COUNT; a++) {
        SDL_memset(&quiet, 0, sizeof quiet);
        quiet.name = k_apps[a];
        (void)run_app(dev, rt, a, blank, 0, &quiet);
        g_cur_report = nullptr;
    }
    emb_runtime_destroy(rt);
    const size_t live_second = embi_mem_live();
    SDL_Log("APPS live bytes (embi_alloc accounting): %llu at start, %llu after the first pass, %llu after the second",
            (unsigned long long)mem_base, (unsigned long long)live_first, (unsigned long long)live_second);
    EMB_CHECK(live_second == live_first);

    embi_gpu_destroy(g);
    const double run_ms = (double)(SDL_GetTicksNS() - t_start) / 1e6;
    SDL_Quit();
    SDL_Log("APPS run took %.0f ms for %d apps (two passes)", run_ms, APP_COUNT);
}

/* The scenario player (W8b0): a second test of this executable. */
#include "test_embed_apps_player.h"

EMB_TEST_MAIN()
