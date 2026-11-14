#pragma once
/*
 * Shared setup for the observer tests (W6d) that need real layout: a styled, laid-out document with a binding context
 * whose host gives the viewport and the scroll. frame() simulates the frame's order: restyle, lay out, then the
 * observers' after-layout step. Not a test itself (no test_ prefix).
 *
 * Memory. The tests of one process share caches that the first use of a path fills and keeps (the slab chunks that
 * documents share, the atoms of the names the tests use). ob_warm_once runs a scenario that uses every path once, so each
 * test's baseline (taken in ob_env_init) is the steady state, and ob_env_done returns what the test left allocated.
 */
#include "emb_test.h"

#include "base/base.h"
#include "bindings/bind.h"
#include "bindings/observers.h"
#include "css/css.h"
#include "css/props.h"
#include "html/parser.h"
#include "js/heap.h"
#include "js/js.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <string.h>

typedef struct ob_env {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *layout;
    embi_css_sheet *sheet;
    embi_js_runtime *rt;
    embi_document *doc;
    embi_bind_ctx *c;
    double vw, vh;
    double dpr; /* device pixel ratio the host reports (1 unless a test sets it) */
    size_t mem0; /* embi_mem_live() before the environment was built (ob_env_init) */
} ob_env;

/* Fake timers: the scheduled delivery tasks are kept and fired by the test. */
typedef struct ob_timers {
    void *payloads[16];
    uint32_t n;
} ob_timers;

static inline uint32_t ob_timer_add(void *user, double timeout_ms, bool repeat, void *payload)
{
    ob_timers *t = user;
    (void)timeout_ms;
    (void)repeat;
    if (t->n >= 16) {
        return 0;
    }
    t->payloads[t->n] = payload;
    return ++t->n;
}

static inline void *ob_timer_clear(void *user, uint32_t id)
{
    (void)user;
    (void)id;
    return nullptr;
}

static inline void ob_host_viewport(void *user, double *w, double *h, double *dpr)
{
    ob_env *e = user;
    *w = e->vw;
    *h = e->vh;
    *dpr = e->dpr > 0 ? e->dpr : 1;
}

static inline void ob_host_scroll(void *user, double x, double y)
{
    (void)user;
    (void)x;
    (void)y;
}

/* Builds the document (html with css), lays it out at vw x vh and binds a context to it. */
static inline void ob_build(ob_env *e, const char *html, const char *css)
{
    memset(e, 0, sizeof *e);
    e->vw = 1024;
    e->vh = 768;
    e->text = embi_tshim_create();
    e->style = embi_style_engine_create(embi_tshim_fonts(e->text), nullptr, nullptr);
    e->doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(e->doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    static const char reset[] = "body{margin:0}";
    size_t css_len = strlen(css);
    char *all = embi_alloc(sizeof reset + css_len);
    memcpy(all, reset, sizeof reset - 1);
    memcpy(all + sizeof reset - 1, css, css_len + 1);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    e->sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)(sizeof reset - 1 + css_len)}, &opts);
    embi_free(all, sizeof reset + css_len);
    embi_css_sheet *sheets[1] = {e->sheet};
    embi_document_set_author_sheets(e->style, e->doc, sheets, 1);
    embi_style_resolve_document(e->style, e->doc);
    e->layout = embi_layout_engine_create(e->text, e->style, nullptr);
    embi_layout_set_viewport(e->layout, (float)e->vw, (float)e->vh);
    embi_layout_document(e->layout, e->doc);
    e->rt = embi_js_runtime_new();
    e->c = embi_bind_new(e->rt, e->doc);
    embi_bind_host h = {.user = e,
                        .viewport = ob_host_viewport,
                        .scroll = ob_host_scroll,
                        .style = e->style,
                        .layout = e->layout};
    embi_bind_set_host(e->c, &h);
}

/* One frame: restyle and lay out what changed, then the observers' step. */
static inline void ob_frame(ob_env *e)
{
    embi_style_resolve_document(e->style, e->doc);
    embi_layout_document(e->layout, e->doc);
    embi_observers_after_layout(e->c);
}

/* Tears the environment down. Returns the bytes still allocated since the environment was built (0 when nothing leaked). */
static inline long long ob_env_done(ob_env *e)
{
    embi_bind_free(e->c);
    embi_js_runtime_free(e->rt);
    embi_layout_engine_destroy(e->layout);
    embi_style_forget_document(e->style, e->doc);
    embi_document_free(e->doc);
    embi_css_sheet_unref(e->sheet);
    embi_style_engine_destroy(e->style);
    embi_tshim_destroy(e->text);
    return (long long)embi_mem_live() - (long long)e->mem0;
}

static inline bool ob_js_true(ob_env *e, const char *src)
{
    embi_js_value v = embi_js_eval(embi_bind_realm(e->c), src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e->rt);
        return false;
    }
    if (v != embi_js_from_bool(true)) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

static const char ob_warm_html[] =
    "<!doctype html><html><head></head><body>"
    "<div id=a></div><div id=t></div><div id=far></div>"
    "</body></html>";

static const char ob_warm_css[] =
    "#a{width:100px;height:50px} #t{height:100px;width:200px} #far{width:200px;height:100px;margin-top:900px}";

/* Runs every observer path once: the three observers, the timer hooks, frames, a collection and the teardown. */
static inline void ob_warm(void)
{
    ob_env e;
    ob_build(&e, ob_warm_html, ob_warm_css);
    ob_timers tm = {{0}, 0};
    embi_bind_timers hooks = {&tm, ob_timer_add, ob_timer_clear};
    embi_bind_set_timers(e.c, &hooks);
    (void)ob_js_true(&e, "var ro = new ResizeObserver(function () {});"
                         "ro.observe(document.getElementById('a'));"
                         "var io = new IntersectionObserver(function () {}, {threshold: [0, 1]});"
                         "io.observe(document.getElementById('t'));"
                         "var mo = new MutationObserver(function () {});"
                         "mo.observe(document.body, {childList: true, attributes: true, characterData: true, subtree: true});"
                         "document.getElementById('a').setAttribute('x', '1');"
                         "document.getElementById('a').appendChild(document.createElement('i'));"
                         "document.getElementById('a').style.width = '60px'; true");
    ob_frame(&e);
    ob_frame(&e);
    embi_js_run_jobs(e.rt);
    uint32_t n = tm.n;
    for (uint32_t i = 0; i < n; i++) {
        (void)embi_bind_fire_timer(e.c, tm.payloads[i]);
        embi_bind_release_timer(e.c, tm.payloads[i]);
    }
    embi_js_run_jobs(e.rt);
    (void)ob_js_true(&e, "ro.disconnect(); io.disconnect(); mo.disconnect(); true");
    embi_js_gc(e.rt);
    (void)ob_env_done(&e);
}

/* Builds an environment. Its baseline (mem0) is the allocation count before the build; the warm-up runs first, once. */
static inline void ob_env_init(ob_env *e, const char *html, const char *css)
{
    static bool warmed;
    if (!warmed) {
        warmed = true;
        ob_warm();
    }
    size_t base = embi_mem_live();
    ob_build(e, html, css);
    e->mem0 = base;
}
