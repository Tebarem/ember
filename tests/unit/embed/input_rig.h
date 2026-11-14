#pragma once
/*
 * A small rig for the input tests of a view (W5d): an offscreen 300x200 view with a page loaded and laid out, a script
 * evaluator that returns the result text, a key helper, and the logger of the DOM events a page's listeners see. Views need
 * an offscreen device; OPEN_HTML_OR_SKIP skips the test (77) without one.
 */
#include "emb_test.h"

#include "base/base.h"
#include "embed/embed_int.h"
#include "embed/doc.h"
#include "embed/input_int.h"
#include "embed/script_int.h"

#include <ember/ember_input.h>
#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

static emb_view *rig_open(emb_runtime *rt, embi_gpu **gout, const char *html)
{
    *gout = embi_gpu_create(nullptr, false);
    if (*gout == nullptr) {
        return nullptr;
    }
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = 300;
    d.height = 200;
    d.scale = 1.0f;
    d.device = embi_gpu_device(*gout);
    emb_view *v = emb_view_create(rt, &d);
    if (v == nullptr) {
        return nullptr;
    }
    (void)emb_view_load_html(v, html, strlen(html), "http://127.0.0.1/page.html");
    (void)embi_view_update(v);
    return v;
}

static void rig_close(emb_view *v, embi_gpu *g, emb_runtime *rt)
{
    emb_view_destroy(v);
    embi_gpu_destroy(g);
    emb_runtime_destroy(rt);
}

static char g_res[4096];
static bool g_ok;

static void rig_on_result(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    g_ok = ok;
    size_t n = len < sizeof g_res - 1 ? len : sizeof g_res - 1;
    memcpy(g_res, text, n);
    g_res[n] = '\0';
}

/* Runs js in the view and returns its result text (or "!throw"). */
static const char *run(emb_view *v, const char *js)
{
    g_res[0] = '\0';
    g_ok = false;
    (void)emb_view_eval(v, js, strlen(js), rig_on_result, nullptr);
    return g_ok ? g_res : "!throw";
}

[[maybe_unused]] static float root_y(emb_view *v)
{
    float x = 0, y = 0;
    embi_doc_root_offset(v->doc, &x, &y);
    return y;
}

static emb_key_event rig_key(const char *name, const char *code, bool down, uint32_t mods)
{
    emb_key_event k;
    memset(&k, 0, sizeof k);
    k.size = sizeof k;
    k.down = down;
    k.mods = mods;
    k.key = name;
    k.code = code;
    return k;
}

/* A key press and release (keydown then keyup). */
[[maybe_unused]] static void rig_tap_key(emb_view *v, const char *name, const char *code, uint32_t mods)
{
    emb_key_event d = rig_key(name, code, true, mods);
    emb_view_key(v, &d);
    emb_key_event u = rig_key(name, code, false, mods);
    emb_view_key(v, &u);
}

/* Installs window.log and logs "type:target" (the target's id, else its tag name) for each listed type at the document in
 * the capture phase (so each dispatch logs once). types is the JS array literal body, e.g. "'click','focus'". */
[[maybe_unused]] static void rig_log(emb_view *v, const char *types)
{
    char js[1024];
    SDL_snprintf(js, sizeof js,
                 "window.log = []; [%s].forEach(function (t) { document.addEventListener(t, function (e) { "
                 "log.push(t + ':' + (e.target.id || e.target.tagName)); }, true); }); true",
                 types);
    (void)run(v, js);
}

#define OPEN_HTML_OR_SKIP(v, rt, g, html)                                                                  \
    do {                                                                                                   \
        (v) = rig_open((rt), &(g), (html));                                                                \
        if ((v) == nullptr) {                                                                              \
            emb_test_cur_skip = "no GPU device";                                                           \
            if ((g) != nullptr) {                                                                          \
                embi_gpu_destroy(g);                                                                       \
            }                                                                                              \
            emb_runtime_destroy(rt);                                                                       \
            return;                                                                                        \
        }                                                                                                  \
    } while (0)

static int g_nav_calls;
static char g_nav_url[512];

[[maybe_unused]] static void rig_nav(void *user, const char *url, size_t len)
{
    (void)user;
    g_nav_calls++;
    size_t n = len < sizeof g_nav_url - 1 ? len : sizeof g_nav_url - 1;
    memcpy(g_nav_url, url, n);
    g_nav_url[n] = '\0';
}

/* The GPU tests use the video subsystem, which is initialized once around the run (as the other GPU tests do). */
#define RIG_MAIN()                                                                                         \
    int main(void)                                                                                         \
    {                                                                                                      \
        const bool video = SDL_Init(SDL_INIT_VIDEO);                                                       \
        const int rc = emb_test_run_all();                                                                 \
        if (video) {                                                                                       \
            SDL_Quit();                                                                                    \
        }                                                                                                  \
        return rc;                                                                                         \
    }
