/* Framework smoke tests (exit criteria of the JS engine). The IIFE/UMD builds of Preact 10 and of
 * Vue's reactivity package (tests/js/frameworks, vendored unmodified with their MIT licenses) are
 * evaluated in a fresh realm each, then a small script built on them runs (tests/js/frameworks/
 * *_smoke.js). Preact renders into a stub document (tests/js/frameworks/dom_stub.js).
 *
 * The engine's real job queue (J3a) runs here: after each smoke script, the queue is drained
 * (embi_js_run_jobs), and the script's microtask must have run (globalThis.smokeJob). Preact
 * 10's renderer does not depend on jobs (its debounce uses Promise only when a component
 * schedules an update), so the render checks are unaffected by the queue.
 *
 * Preact 11 (ESM) is tested in test_js_frameworks_esm.c, through the module loader.
 *
 * The Vue smoke test (load plus script) must run under 50 ms in a Debug build, which is the
 * reactivity budget of J2 section 8. Under AddressSanitizer the budget is scaled.
 */
#include "es6_util.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define FRAME_TEXT_MAX 256
#define BUDGET_MS 50.0
#define BUDGET_ASAN_SCALE 20.0

typedef struct frame_result {
    bool loaded;              /* the framework bundle evaluated */
    bool blocked;             /* it failed on a feature owned by a later task (regex literals) */
    bool job_ran;             /* the smoke script's microtask ran on the job queue */
    char text[FRAME_TEXT_MAX]; /* the smoke script's string result, or the error that stopped it */
    double load_ms;           /* framework bundle evaluation */
    double run_ms;            /* the smoke script */
} frame_result;

static char *load_frame(const char *name, size_t *len)
{
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/tests/js/frameworks/%s", EMBER_SOURCE_DIR, name);
    return SDL_LoadFile(path, len);
}

static double elapsed_ms(uint64_t since)
{
    uint64_t now = SDL_GetPerformanceCounter();
    return (double)(now - since) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* Copies the text of a value (its string conversion) into buf. Returns false when the conversion
 * throws, with the exception still pending. */
static bool value_text(embi_js_runtime *rt, embi_js_value v, char *buf, size_t cap)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    SDL_strlcpy(buf, embi_sb_cstr(&sb), cap);
    embi_sb_free(&sb);
    return true;
}

/* Copies the message of the pending exception into buf and clears the exception. */
static void take_exception(embi_js_runtime *rt, char *buf, size_t cap)
{
    embi_js_value ex = embi_js_exception_get(rt);
    buf[0] = 0;
    if (embi_js_is_object(ex)) {
        embi_js_value m = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "message"));
        if (embi_js_is_string(m)) {
            (void)value_text(rt, m, buf, cap);
        }
    }
    embi_js_exception_clear(rt);
}

/* Whether the smoke script's microtask recorded that it ran (globalThis.smokeJob === 'done'). */
static bool job_flag_is_done(es6_env e)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "smokeJob"));
    return v != EMBI_JS_EXCEPTION && es6_text_is(e.rt, v, "done");
}

/* Evaluates the framework bundle (after the DOM stub when needed) and then the smoke script. */
static frame_result run_frame(const char *frame_file, const char *smoke_file, bool needs_dom)
{
    frame_result res;
    memset(&res, 0, sizeof res);
    es6_env e = es6_env_new();
    if (!e.r) {
        SDL_strlcpy(res.text, "no realm", sizeof res.text);
        return res;
    }
    size_t flen = 0;
    size_t slen = 0;
    size_t dlen = 0;
    char *fsrc = load_frame(frame_file, &flen);
    char *ssrc = load_frame(smoke_file, &slen);
    char *dsrc = needs_dom ? load_frame("dom_stub.js", &dlen) : nullptr;
    if (!fsrc || !ssrc || (needs_dom && !dsrc)) {
        SDL_strlcpy(res.text, "fixture missing", sizeof res.text);
    } else {
        uint64_t t0 = SDL_GetPerformanceCounter();
        bool ok = true;
        if (dsrc) {
            embi_js_value dv = embi_js_eval(e.r, dsrc, dlen, "dom_stub.js", 0);
            if (dv == EMBI_JS_EXCEPTION) {
                take_exception(e.rt, res.text, sizeof res.text);
                ok = false;
            }
        }
        if (ok) {
            embi_js_value v = embi_js_eval(e.r, fsrc, flen, frame_file, 0);
            res.load_ms = elapsed_ms(t0);
            if (v == EMBI_JS_EXCEPTION) {
                take_exception(e.rt, res.text, sizeof res.text);
                res.blocked = strstr(res.text, "regular expression literals") != nullptr;
                ok = false;
            } else {
                res.loaded = true;
            }
        }
        if (ok) {
            uint64_t t1 = SDL_GetPerformanceCounter();
            embi_js_value v = embi_js_eval(e.r, ssrc, slen, smoke_file, 0);
            res.run_ms = elapsed_ms(t1);
            if (v == EMBI_JS_EXCEPTION) {
                take_exception(e.rt, res.text, sizeof res.text);
            } else if (!value_text(e.rt, v, res.text, sizeof res.text)) {
                take_exception(e.rt, res.text, sizeof res.text);
            } else {
                embi_js_run_jobs(e.rt);
                res.job_ran = job_flag_is_done(e);
            }
        }
    }
    SDL_free(fsrc);
    SDL_free(ssrc);
    SDL_free(dsrc);
    es6_env_free(e);
    return res;
}

static double budget_ms(void)
{
    return EMB_TEST_ASAN ? BUDGET_MS * BUDGET_ASAN_SCALE : BUDGET_MS;
}

/* Vue's reactivity package: reactive objects, effects, computed values, collections, arrays,
 * refs and readonly proxies, all driven by the smoke script. */
EMB_TEST(frameworks_vue_reactivity_smoke)
{
    frame_result r = run_frame("vue-reactivity-3.5.45.global.js", "vue_smoke.js", false);
    SDL_Log("    vue: load %.1f ms, smoke %.1f ms, result \"%s\"", r.load_ms, r.run_ms, r.text);
    EMB_CHECK(r.loaded);
    EMB_CHECK(strcmp(r.text, "ok") == 0);
    EMB_CHECK(r.job_ran);
    EMB_CHECK(r.load_ms + r.run_ms < budget_ms());
}

/* Preact 10: h() and render() into the stub document, with a reuse of the tree on a second render,
 * a function component, and unmounting. Preact's bundle uses regular expression literals, which
 * the engine does not parse until J4, so until then this test is skipped with that reason. */
EMB_TEST(frameworks_preact_render_smoke)
{
    frame_result r = run_frame("preact-10.29.8.umd.js", "preact_smoke.js", true);
    if (r.blocked) {
        EMB_SKIP("Preact bundle needs regular expression literals (J4)");
    }
    SDL_Log("    preact: load %.1f ms, smoke %.1f ms, result \"%s\"", r.load_ms, r.run_ms, r.text);
    EMB_CHECK(r.loaded);
    EMB_CHECK(strcmp(r.text, "ok") == 0);
    EMB_CHECK(r.job_ran);
}

EMB_TEST_MAIN()
