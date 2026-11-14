/*
 * The module host with the JS engine (W7d): a module graph with a cycle, a JSON module (type json), import.meta.url
 * and import.meta.resolve, a module whose media type is HTML (refused), a module that throws (its import rejects with
 * the error), dynamic import from a classic script (the script's base URL), an import map alias, an inline module
 * (its URL is the document's), and the freezing of the import maps once a module has loaded. The loads come from a table
 * in memory; the results are read back as globals.
 */
#include "loader_test.h"

#include "base/base.h"
#include "js/js.h"
#include "loader/modules.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct mod_src {
    const char *url;
    const char *mime;
    const char *body;
} mod_src;

static const mod_src k_src[] = {
    {"app://t/main.js", "text/javascript",
     "import {a} from './a.js'; import d from './d.json' with {type: 'json'};"
     "globalThis.r = a + ':' + d.v + ':' + import.meta.url;"},
    {"app://t/a.js", "text/javascript", "import {b} from './b.js'; export const a = 'A' + b;"},
    {"app://t/b.js", "text/javascript", "import {a} from './a.js'; export const b = 'B'; export function getA() { return a; }"},
    {"app://t/d.json", "application/json", "{\"v\": 7}"},
    {"app://t/page.html", "text/html", "export default 1;"},
    {"app://t/throws.js", "text/javascript", "throw new Error('boom');"},
    {"app://t/meta.js", "text/javascript", "globalThis.res = import.meta.resolve('./x.js');"},
    {"app://t/c.js", "text/javascript", "export const c = 'C';"},
    {"app://t/noext", "text/javascript", "export const q = 'noext';"},
};

typedef struct host_state {
    int loads;
} host_state;

/* The embedder's load: the table entry for the URL, with its media type. */
static bool load_src(void *user, embi_str url, uint8_t **data, size_t *len, embi_sb *mime)
{
    host_state *hs = user;
    hs->loads++;
    for (size_t i = 0; i < sizeof k_src / sizeof k_src[0]; i++) {
        if (url.len == strlen(k_src[i].url) && memcmp(url.ptr, k_src[i].url, url.len) == 0) {
            size_t n = strlen(k_src[i].body);
            uint8_t *copy = SDL_malloc(n > 0 ? n : 1);
            SDL_memcpy(copy, k_src[i].body, n);
            *data = copy;
            *len = n;
            embi_sb_append(mime, (embi_str){k_src[i].mime, (uint32_t)strlen(k_src[i].mime)});
            return true;
        }
    }
    return false;
}

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
    embi_modules *m;
    host_state hs;
} env;

static void setup(env *e)
{
    memset(e, 0, sizeof *e);
    e->rt = embi_js_runtime_new();
    e->r = embi_js_realm_new(e->rt);
    embi_mod_host h = {&e->hs, load_src, nullptr, nullptr, nullptr};
    e->m = embi_modules_new(e->rt, e->r, &h);
    embi_modules_attach(e->m);
    embi_modules_set_doc_url(e->m, EMBI_STR("app://t/index.html"));
}

static void teardown(env *e)
{
    embi_js_realm_free(e->r);
    embi_js_runtime_free(e->rt);
    embi_modules_free(e->m);
}

/* Runs a classic script and drains the jobs. */
static void run(env *e, const char *src)
{
    (void)embi_js_eval(e->r, src, strlen(src), "classic", 0);
    embi_js_exception_clear(e->rt);
    embi_js_run_jobs(e->rt);
}

/* The string value of a global expression, copied into buf (empty when it throws). */
static void global_str(env *e, const char *expr, char *buf, size_t cap)
{
    buf[0] = '\0';
    embi_js_value v = embi_js_eval(e->r, expr, strlen(expr), "read", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(e->rt);
        return;
    }
    embi_js_string *s = embi_js_to_string(e->rt, v);
    if (s == nullptr) {
        embi_js_exception_clear(e->rt);
        return;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    size_t n = sb.len < cap - 1 ? sb.len : cap - 1;
    if (n > 0) {
        memcpy(buf, sb.data, n);
    }
    buf[n] = '\0';
    embi_sb_free(&sb);
}

EMB_TEST(modules_graph_cycle_json_and_meta_url)
{
    env e;
    setup(&e);
    (void)embi_modules_eval_url(e.m, EMBI_STR("app://t/main.js"));
    embi_js_run_jobs(e.rt);
    char got[160];
    global_str(&e, "globalThis.r", got, sizeof got);
    EMB_CHECK(strcmp(got, "AB:7:app://t/main.js") == 0);
    /* Each module is fetched once: main, a, b, d.json. */
    EMB_CHECK(e.hs.loads == 4);
    teardown(&e);
}

EMB_TEST(modules_wrong_media_type_and_throwing_modules_reject)
{
    env e;
    setup(&e);
    run(&e, "import('app://t/page.html').catch(() => { globalThis.e1 = 'refused'; });"
            "import('app://t/throws.js').catch(e => { globalThis.e2 = e.message; });");
    char got[64];
    global_str(&e, "globalThis.e1", got, sizeof got);
    EMB_CHECK(strcmp(got, "refused") == 0);
    global_str(&e, "globalThis.e2", got, sizeof got);
    EMB_CHECK(strcmp(got, "boom") == 0);
    teardown(&e);
}

EMB_TEST(modules_import_meta_resolve_and_extensionless_by_mime)
{
    env e;
    setup(&e);
    (void)embi_modules_eval_url(e.m, EMBI_STR("app://t/meta.js"));
    embi_js_run_jobs(e.rt);
    char got[96];
    global_str(&e, "globalThis.res", got, sizeof got);
    EMB_CHECK(strcmp(got, "app://t/x.js") == 0);
    /* No extension: the media type of the response decides, and this one is JavaScript. */
    run(&e, "import('app://t/noext').then(m => { globalThis.nx = m.q; });");
    global_str(&e, "globalThis.nx", got, sizeof got);
    EMB_CHECK(strcmp(got, "noext") == 0);
    teardown(&e);
}

EMB_TEST(modules_dynamic_import_uses_the_classic_script_base_and_import_maps)
{
    env e;
    setup(&e);
    embi_modules_set_script_url(e.m, EMBI_STR("app://t/dir/script.js"));
    /* The import map maps the alias before any module loads. */
    embi_sb err = {nullptr, 0, 0};
    EMB_CHECK(embi_modules_add_importmap(e.m, EMBI_STR("{\"imports\": {\"alias\": \"./c.js\"}}"), &err));
    embi_sb_free(&err);
    run(&e, "import('../c.js').then(m => { globalThis.dyn = m.c; });"
            "import('alias').then(m => { globalThis.alias = m.c; });");
    embi_modules_set_script_url(e.m, (embi_str){nullptr, 0});
    char got[32];
    global_str(&e, "globalThis.dyn", got, sizeof got);
    EMB_CHECK(strcmp(got, "C") == 0);
    global_str(&e, "globalThis.alias", got, sizeof got);
    EMB_CHECK(strcmp(got, "C") == 0);
    teardown(&e);
}

EMB_TEST(modules_inline_module_has_the_document_url_and_maps_freeze)
{
    env e;
    setup(&e);
    (void)embi_modules_eval_source(e.m, EMBI_STR("globalThis.inl = import.meta.url + '|' + typeof import.meta.resolve;"));
    embi_js_run_jobs(e.rt);
    char got[96];
    global_str(&e, "globalThis.inl", got, sizeof got);
    EMB_CHECK(strcmp(got, "app://t/index.html|function") == 0);
    /* A module has loaded: a later import map is refused. */
    embi_sb err = {nullptr, 0, 0};
    EMB_CHECK(!embi_modules_add_importmap(e.m, EMBI_STR("{\"imports\": {\"late\": \"./l.js\"}}"), &err));
    EMB_CHECK(err.len > 0);
    embi_sb_free(&err);
    teardown(&e);
}

LOADER_TEST_MAIN()
