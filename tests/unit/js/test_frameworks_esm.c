/* Preact 11 module-graph smoke test (exit criteria of the module loader). The ESM build of Preact 11
 * (tests/js/frameworks/preact-11, vendored unmodified, MIT) loads as a module graph through the
 * embedder's module loader: the entry module preact11_smoke.mjs imports preact.mjs, and
 * hooks/hooks.mjs imports the bare specifier "preact", which this loader maps to the same file.
 * The module renders into the DOM-less stub document (dom_stub.js, evaluated as a script first).
 *
 * Preact 11's bundle uses regular expression literals (parsed since J4). The smoke module also
 * renders a component with state and an effect; the stub's timer queue (dom_stub.js flushTimers)
 * runs the effects. The job queue is the engine's real one: the module promise settles in
 * embi_js_run_jobs.
 */
#include "es6_util.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <string.h>

#define FW_DIR EMBER_SOURCE_DIR "/tests/js/frameworks"
#define ENTRY_KEY FW_DIR "/preact11_smoke.mjs"
#define PREACT_KEY FW_DIR "/preact-11/preact.mjs"

/* The loader's specifier table. A relative specifier resolves against the directory of its
 * referrer (without "../" segments); a bare specifier names a file in the frameworks directory. */
typedef struct bare_map {
    const char *spec;
    const char *key;
} bare_map;

static const bare_map bare_specs[] = {
    {"preact", PREACT_KEY},
};

/* Builds the key of a relative specifier: the referrer's directory joined with the specifier
 * minus its leading "./". False for "../" or a path that does not fit. */
static bool relative_key(const char *ref, size_t rlen, const char *spec, size_t slen, char *out,
                         size_t cap)
{
    if (ref == nullptr || slen < 2 || spec[0] != '.' || spec[1] != '/') {
        return false;
    }
    size_t d = rlen;
    while (d > 0 && ref[d - 1] != '/') {
        d--;
    }
    size_t rest = slen - 2;
    if (d + rest + 1 > cap) {
        return false;
    }
    memcpy(out, ref, d);
    memcpy(out + d, spec + 2, rest);
    out[d + rest] = '\0';
    return true;
}

/* The embedder's loader: reads the file a key names and completes the request with it. Only
 * source modules are served; import attributes fail (the test imports none). */
static void loader(embi_js_runtime *rt, void *user, uint32_t req, const char *ref, size_t rlen,
                   const char *spec, size_t slen, const char *type)
{
    (void)user;
    char key[1024];
    if (type != nullptr) {
        embi_js_module_load_failed(rt, req, "import attributes are not used by this test");
        return;
    }
    if (slen >= sizeof key) {
        embi_js_module_load_failed(rt, req, "specifier too long");
        return;
    }
    char name[1024];
    memcpy(name, spec, slen);
    name[slen] = '\0';
    bool mapped = false;
    for (size_t i = 0; i < sizeof bare_specs / sizeof bare_specs[0]; i++) {
        if (strcmp(bare_specs[i].spec, name) == 0) {
            SDL_snprintf(key, sizeof key, "%s", bare_specs[i].key);
            mapped = true;
            break;
        }
    }
    if (ref == nullptr) {
        /* The entry module: its specifier is already its key (an absolute path). */
        SDL_snprintf(key, sizeof key, "%s", name);
        mapped = true;
    }
    if (!mapped && !relative_key(ref, rlen, spec, slen, key, sizeof key)) {
        embi_js_module_load_failed(rt, req, "unsupported module specifier");
        return;
    }
    size_t n = 0;
    char *src = SDL_LoadFile(key, &n);
    if (src == nullptr) {
        embi_js_module_load_failed(rt, req, "cannot read module");
        return;
    }
    embi_js_module_loaded(rt, req, key, strlen(key), src, n);
    SDL_free(src);
}

/* Copies the string value of a global property into buf (empty when missing or not a string). */
static void global_text(embi_js_runtime *rt, embi_js_realm *r, const char *name, char *buf,
                        size_t cap)
{
    buf[0] = '\0';
    embi_js_value v = embi_js_get(rt, r->global, embi_js_key_c(rt, name));
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(rt);
        return;
    }
    embi_js_string *s = embi_js_to_string(rt, v);
    if (s == nullptr) {
        embi_js_exception_clear(rt);
        return;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    SDL_strlcpy(buf, embi_sb_cstr(&sb), cap);
    embi_sb_free(&sb);
}

/* Evaluates a classic script; false (with the exception cleared) when it throws. */
static bool run_script(embi_js_realm *r, const char *src, const char *name)
{
    embi_js_value v = embi_js_eval(r, src, strlen(src), name, 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(r->rt);
        return false;
    }
    return true;
}

/* The outcome of one run: the entry module's promise state and the smoke module's result. */
typedef struct esm_result {
    char state[384]; /* "ok", "err:<name>:<message>" or "pending" */
    char result[64]; /* the smoke module's globalThis.preact11Result */
    bool dom_failed;
} esm_result;

static esm_result run_esm(void)
{
    esm_result res;
    memset(&res, 0, sizeof res);
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_realm *r = rt != nullptr ? embi_js_realm_new(rt) : nullptr;
    if (r == nullptr) {
        if (rt != nullptr) {
            embi_js_runtime_free(rt);
        }
        SDL_snprintf(res.state, sizeof res.state, "err:no realm");
        return res;
    }
    embi_js_set_module_loader(rt, loader, nullptr);

    size_t dlen = 0;
    char *dsrc = SDL_LoadFile(FW_DIR "/dom_stub.js", &dlen);
    if (dsrc == nullptr || !run_script(r, dsrc, "dom_stub.js")) {
        res.dom_failed = true;
    }
    SDL_free(dsrc);

    if (!res.dom_failed) {
        embi_js_value p = embi_js_module_eval(r, ENTRY_KEY, strlen(ENTRY_KEY));
        embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "preact11Promise"), p,
                           EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
        static const char watch[] =
            "globalThis.preact11State = 'pending';"
            "globalThis.preact11Promise.then(function () {"
            "  globalThis.preact11State = 'ok';"
            "}, function (e) {"
            "  globalThis.preact11State = 'err:' + (e && e.name) + ':' + (e && e.message);"
            "});";
        if (!run_script(r, watch, "watch.js")) {
            SDL_snprintf(res.state, sizeof res.state, "err:watch failed");
        }
        embi_js_run_jobs(rt);
        global_text(rt, r, "preact11State", res.state, sizeof res.state);
        global_text(rt, r, "preact11Result", res.result, sizeof res.result);
    } else {
        SDL_snprintf(res.state, sizeof res.state, "err:dom stub failed");
    }
    embi_js_realm_free(r);
    embi_js_runtime_free(rt);
    return res;
}

/* Preact 11 (ESM): the module graph loads, renders the same checks as the Preact 10 smoke test
 * (elements, attributes, text, reuse on a second render, a function component, unmount), a hook
 * from hooks.mjs, and a stateful component whose effect runs after its state update. */
EMB_TEST(frameworks_preact11_esm_smoke)
{
    size_t mem0 = embi_mem_live();
    esm_result r = run_esm();
    SDL_Log("    preact11: state \"%s\", result \"%s\"", r.state, r.result);
    EMB_CHECK(embi_mem_live() == mem0);
    EMB_CHECK(!r.dom_failed);
    EMB_CHECK(strcmp(r.state, "ok") == 0);
    EMB_CHECK(strcmp(r.result, "ok") == 0);
}

EMB_TEST_MAIN()
