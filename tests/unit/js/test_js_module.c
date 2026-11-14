/*
 * ES modules (J3c, src/js/module.c): linking, evaluation and the host loader, through an in-memory
 * loader. The loader answers synchronously, or defers its answers until the test flushes them (an
 * asynchronous host). Outcomes are read back from the realm: each module body records what it
 * saw in globalThis.out, and each promise sets a state global (S<n>). Every test checks that the
 * runtime's memory returns to its baseline.
 */
#include "emb_test.h"
#include "base/base.h"
#include "js/js.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* ------------------------------------------------------------------ in-memory loader */

typedef struct modsrc {
    const char *spec; /* the module's key (its specifier) */
    const char *type; /* nullptr for a source module, "json" or another type attribute */
    const char *src;
} modsrc;

typedef struct qitem {
    uint32_t req;
    char spec[64];
    char type[8]; /* empty for none */
} qitem;

#define QMAX 32
#define LOG_MAX 1024

static struct {
    const modsrc *mods;
    int nmods;
    bool defer;          /* requests are queued until flush() */
    qitem q[QMAX];
    int nq;
    char log[LOG_MAX];   /* one "referrer|specifier|type;" entry per request */
    size_t loglen;
    int meta_hits;       /* import.meta objects created */
    char meta_url[128];  /* the url the import.meta hook gave the last module */
} T;

static embi_js_runtime *g_rt;
static embi_js_realm *g_r;

static void log_add(const char *s, size_t n)
{
    if (T.loglen + n + 1 < LOG_MAX) {
        memcpy(T.log + T.loglen, s, n);
        T.loglen += n;
        T.log[T.loglen] = '\0';
    }
}

static const modsrc *find_src(const char *spec, size_t slen, const char *type)
{
    for (int i = 0; i < T.nmods; i++) {
        const modsrc *m = &T.mods[i];
        bool same_type = type == nullptr ? m->type == nullptr
                                         : m->type != nullptr && strcmp(m->type, type) == 0;
        if (strlen(m->spec) == slen && memcmp(m->spec, spec, slen) == 0 && same_type) {
            return m;
        }
    }
    return nullptr;
}

/* Completes one request from the table: the key is the specifier. */
static void answer(embi_js_runtime *rt, uint32_t req, const char *spec, size_t slen,
                   const char *type)
{
    const modsrc *m = find_src(spec, slen, type);
    if (m == nullptr) {
        embi_js_module_load_failed(rt, req, "no such module");
        return;
    }
    embi_js_module_loaded(rt, req, spec, slen, m->src, strlen(m->src));
}

static void loader(embi_js_runtime *rt, void *user, uint32_t req, const char *ref, size_t rlen,
                   const char *spec, size_t slen, const char *type)
{
    (void)user;
    log_add(ref != nullptr ? ref : "-", ref != nullptr ? rlen : 1);
    log_add("|", 1);
    log_add(spec, slen);
    log_add("|", 1);
    if (type != nullptr) {
        log_add(type, strlen(type));
    } else {
        log_add("-", 1);
    }
    log_add(";", 1);
    if (!T.defer) {
        answer(rt, req, spec, slen, type);
        return;
    }
    if (T.nq == QMAX || slen >= sizeof T.q[0].spec || (type != nullptr && strlen(type) >= 8)) {
        embi_js_module_load_failed(rt, req, "test queue full");
        return;
    }
    qitem *it = &T.q[T.nq++];
    it->req = req;
    memcpy(it->spec, spec, slen);
    it->spec[slen] = '\0';
    it->type[0] = '\0';
    if (type != nullptr) {
        memcpy(it->type, type, strlen(type) + 1);
    }
}

/* Completes the queued requests in order. Answers may queue more requests (still deferred). */
static void flush(void)
{
    while (T.nq > 0) {
        qitem it = T.q[0];
        memmove(&T.q[0], &T.q[1], (size_t)(T.nq - 1) * sizeof T.q[0]);
        T.nq--;
        answer(g_rt, it.req, it.spec, strlen(it.spec), it.type[0] != '\0' ? it.type : nullptr);
    }
}

static void meta_hook(embi_js_runtime *rt, embi_js_value meta, const char *key, size_t klen,
                      void *user)
{
    (void)user;
    T.meta_hits++;
    if (klen < sizeof T.meta_url) {
        memcpy(T.meta_url, key, klen);
        T.meta_url[klen] = '\0';
    }
    embi_js_string *s = embi_js_str_new_utf8(rt, key, klen);
    if (s != nullptr) {
        embi_js_define_raw(rt, embi_js_as_object(meta), embi_js_key_c(rt, "url"),
                           embi_js_from_string(s),
                           EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE | EMBI_JS_ATTR_CONFIGURABLE);
    }
}

/* ------------------------------------------------------------------ helpers */

static void setup(const modsrc *mods, int n)
{
    memset(&T, 0, sizeof T);
    T.mods = mods;
    T.nmods = n;
    g_rt = embi_js_runtime_new();
    EMB_REQUIRE(g_rt != nullptr);
    g_r = embi_js_realm_new(g_rt);
    EMB_REQUIRE(g_r != nullptr);
    embi_js_set_module_loader(g_rt, loader, nullptr);
    embi_js_set_import_meta_hook(g_rt, meta_hook, nullptr);
    static const char init[] = "globalThis.out = ''; globalThis.S = '';";
    (void)embi_js_eval(g_r, init, sizeof init - 1, "setup.js", 0);
}

static void teardown(void)
{
    embi_js_realm_free(g_r);
    embi_js_runtime_free(g_rt);
    g_r = nullptr;
    g_rt = nullptr;
}

#define MOD_BEGIN(mods, n)                                                                         \
    size_t mem0_ = embi_mem_live();                                                                \
    setup((mods), (int)(n))
#define MOD_END()                                                                                  \
    teardown();                                                                                    \
    EMB_CHECK(embi_mem_live() == mem0_)

/* Evaluates src as a script. False (and the exception cleared) when it throws. */
static bool run(const char *src)
{
    embi_js_value v = embi_js_eval(g_r, src, strlen(src), "test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(g_rt);
        return false;
    }
    return true;
}

/* The value of expr as ASCII text (an exception is reported as "<exception>"). */
static void text_of(const char *expr, char *buf, size_t cap)
{
    embi_js_value v = embi_js_eval(g_r, expr, strlen(expr), "expr.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(g_rt);
        SDL_snprintf(buf, cap, "<exception>");
        return;
    }
    embi_js_string *s = embi_js_to_string(g_rt, v);
    if (s == nullptr) {
        embi_js_exception_clear(g_rt);
        SDL_snprintf(buf, cap, "<tostring failed>");
        return;
    }
    size_t k = 0;
    uint32_t n = embi_js_str_len(s);
    for (uint32_t i = 0; i < n && k + 1 < cap; i++) {
        uint16_t c = embi_js_str_at(s, i);
        buf[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    buf[k] = '\0';
}

static void expect_text(const char *expr, const char *want)
{
    char got[512];
    text_of(expr, got, sizeof got);
    if (!emb_test_str_equal(got, want)) {
        SDL_Log("    %s = \"%s\", want \"%s\"", expr, got, want);
    }
    EMB_CHECK(emb_test_str_equal(got, want));
}

static void expect_log(const char *want)
{
    if (!emb_test_str_equal(T.log, want)) {
        SDL_Log("    loader log \"%s\", want \"%s\"", T.log, want);
    }
    EMB_CHECK(emb_test_str_equal(T.log, want));
}

/* Runs the queued jobs and the flushed requests until nothing is left. */
static void drain(void)
{
    do {
        flush();
        embi_js_run_jobs(g_rt);
    } while (T.nq > 0);
}

/* Evaluates the module named spec, and records its promise as S<n> ("ok", or "err:<name>"). */
static void watch_module(int n, const char *spec)
{
    embi_js_value p = embi_js_module_eval(g_r, spec, strlen(spec));
    char key[16];
    SDL_snprintf(key, sizeof key, "P%d", n);
    embi_js_define_raw(g_rt, g_r->global, embi_js_key_c(g_rt, key), p,
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    char js[320];
    SDL_snprintf(js, sizeof js,
                 "globalThis.S%d = 'pending';"
                 "globalThis.P%d.then(function () { globalThis.S%d = 'ok'; },"
                 "function (e) { globalThis.S%d = 'err:' + (e && e.name); });",
                 n, n, n, n);
    EMB_CHECK(run(js));
}

/* Evaluates a script that starts imports (their promises settle in drain). */
static void run_src(const char *src)
{
    EMB_CHECK(run(src));
}

/* ------------------------------------------------------------------ modules and linking */

static const modsrc basic_mods[] = {
    {"a", nullptr, "import { x } from 'b'; globalThis.got = x + 1;"},
    {"b", nullptr, "export let x = 41;"},
};

EMB_TEST(module_import_and_export)
{
    MOD_BEGIN(basic_mods, 2);
    watch_module(1, "a");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.got", "42");
    MOD_END();
}

EMB_TEST(module_missing_dependency_rejects)
{
    MOD_BEGIN(basic_mods, 2);
    watch_module(1, "missing");
    drain();
    expect_text("S1", "err:Error");
    MOD_END();
}

static const modsrc missing_dep_mods[] = {
    {"a", nullptr, "import { x } from 'nope'; globalThis.out = 'ran';"},
};

EMB_TEST(module_missing_dependency_message)
{
    MOD_BEGIN(missing_dep_mods, 1);
    watch_module(1, "a");
    drain();
    expect_text("S1", "err:Error");
    expect_text("globalThis.out", "");
    MOD_END();
}

static const modsrc cycle_mods[] = {
    {"a", nullptr, "import { fb } from 'b'; export function fa() { return 'a'; } globalThis.out += 'A';"},
    {"b", nullptr, "import { fa } from 'a'; export function fb() { return fa(); } globalThis.out += 'B';"},
};

EMB_TEST(module_cycle_evaluates_each_once)
{
    MOD_BEGIN(cycle_mods, 2);
    watch_module(1, "a");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "BA");
    MOD_END();
}

static const modsrc live_mods[] = {
    {"a", nullptr,
     "import { x, bump } from 'b'; globalThis.before = x; bump(); globalThis.after = x;"},
    {"b", nullptr, "export let x = 1; export function bump() { x = 2; }"},
};

EMB_TEST(module_live_bindings)
{
    MOD_BEGIN(live_mods, 2);
    watch_module(1, "a");
    drain();
    expect_text("S1", "ok");
    expect_text("before + ',' + after", "1,2");
    MOD_END();
}

static const modsrc star_mods[] = {
    {"m1", nullptr, "export var v = 'm1';"},
    {"m2", nullptr, "export var v = 'm2';"},
    {"m3", nullptr, "export var w = 'm3';"},
    {"mstar", nullptr, "export * from 'm1'; export * from 'm2';"},
    {"mok", nullptr, "export * from 'm1'; export * from 'm3';"},
    {"amb", nullptr, "import { v } from 'mstar'; globalThis.out = v;"},
    {"good", nullptr, "import { v, w } from 'mok'; globalThis.out = v + w;"},
    {"ns_star", nullptr, "import * as ns from 'mstar'; globalThis.keys = Object.keys(ns).join() + '#';"},
};

EMB_TEST(module_export_star_and_ambiguity)
{
    MOD_BEGIN(star_mods, 8);
    watch_module(1, "amb");
    watch_module(2, "good");
    watch_module(3, "ns_star");
    drain();
    expect_text("S1", "err:SyntaxError");
    expect_text("S2", "ok");
    expect_text("S3", "ok");
    expect_text("globalThis.keys", "#");
    expect_text("globalThis.out", "m1m3");
    MOD_END();
}

static const modsrc star_good_mods[] = {
    {"good", nullptr, "import { v, w } from 'mok'; globalThis.out = v + w;"},
    {"m1", nullptr, "export var v = 'm1';"},
    {"m3", nullptr, "export var w = 'm3';"},
    {"mok", nullptr, "export * from 'm1'; export * from 'm3';"},
};

EMB_TEST(module_export_star_resolves)
{
    MOD_BEGIN(star_good_mods, 4);
    watch_module(1, "good");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "m1m3");
    MOD_END();
}

static const modsrc reexport_mods[] = {
    {"b", nullptr, "export let x = 5; export default 'dflt'; export const y = 'yy';"},
    {"c", nullptr,
     "export { x as z } from 'b'; export * as ns from 'b'; import { y } from 'b'; export { y };"},
    {"main", nullptr,
     "import { z, ns, y } from 'c'; globalThis.got = z + ':' + ns.x + ':' + y;"},
};

EMB_TEST(module_reexports)
{
    MOD_BEGIN(reexport_mods, 3);
    watch_module(1, "main");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.got", "5:5:yy");
    MOD_END();
}

static const modsrc ns_mods[] = {
    {"b", nullptr, "export let x = 1; export function f() {} export default 2; export var a2 = 'z';"},
    {"main", nullptr,
     "import * as ns from 'b';"
     "globalThis.got = Object.keys(ns).join() + '|' + ns[Symbol.toStringTag] + '|' +"
     "  (Object.getPrototypeOf(ns) === null) + '|' + Object.isExtensible(ns) + '|' +"
     "  Reflect.set(ns, 'x', 2) + '|' + Reflect.deleteProperty(ns, 'x') + '|' + ns.x;"
     "try { delete ns.x; globalThis.del = 'no'; } catch (e) { globalThis.del = e.name; }"},
    {"tdz_a", nullptr, "import 'tdz_b'; export let v = 1;"},
    {"tdz_b", nullptr,
     "import * as ns from 'tdz_a';"
     "globalThis.r = (function () { try { return ns.v; } catch (e) { return e.name; } })();"
     "globalThis.in_ = 'v' in ns;"},
};

EMB_TEST(module_namespace_object)
{
    MOD_BEGIN(ns_mods, 4);
    watch_module(1, "main");
    watch_module(2, "tdz_a");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.got", "a2,default,f,x|Module|true|false|false|false|1");
    expect_text("globalThis.del", "TypeError");
    expect_text("S2", "ok");
    expect_text("globalThis.r", "ReferenceError");
    expect_text("String(globalThis.in_)", "true");
    MOD_END();
}

static const modsrc order_mods[] = {
    {"t1", nullptr, "await null; globalThis.out += 'T';"},
    {"tm", nullptr, "import 't1'; globalThis.out += 'M';"},
    {"tcyc_b", nullptr, "import 'tcyc_a'; await 0; globalThis.out += 'B';"},
    {"tcyc_a", nullptr, "import 'tcyc_b'; globalThis.out += 'A';"},
    {"tthrow", nullptr, "throw new RangeError('early'); await 0;"},
    {"tparent", nullptr, "import 'tthrow'; globalThis.out = 'ran';"},
};

EMB_TEST(module_tla_ordering)
{
    MOD_BEGIN(order_mods, 6);
    watch_module(1, "tm");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "TM");
    MOD_END();
}

EMB_TEST(module_tla_cycle_root_order)
{
    MOD_BEGIN(order_mods, 6);
    watch_module(1, "tcyc_a");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "BA");
    MOD_END();
}

static const modsrc rejorder_mods[] = {
    {"setup", nullptr,
     "export const p1 = Promise.withResolvers(); export const pA_start = Promise.withResolvers();"
     "export const pB_start = Promise.withResolvers();"},
    {"ro_a", nullptr, "import 'ro_a_sent'; import 'ro_b';"},
    {"ro_a_sent", nullptr, "import { pA_start } from 'setup'; pA_start.resolve();"},
    {"ro_b", nullptr, "import 'ro_b_sent'; import { p1 } from 'setup'; await p1.promise;"},
    {"ro_b_sent", nullptr, "import { pB_start } from 'setup'; pB_start.resolve();"},
    {"ro_main", nullptr,
     "import { p1, pA_start, pB_start } from 'setup';"
     "globalThis.logs = [];"
     "const importsP = Promise.all(["
     "  pB_start.promise.then(() => import('ro_a').finally(() => logs.push('A'))).catch(() => {}),"
     "  import('ro_b').finally(() => logs.push('B')).catch(() => {}),"
     "]);"
     "Promise.all([pA_start.promise, pB_start.promise]).then(p1.reject);"
     "importsP.then(() => { globalThis.done = logs.join(); });"},
};

static const modsrc waiting_mods[] = {
    {"wf", nullptr, "globalThis.started(); export let x = 1; await globalThis.gate;"},
    {"wf2", nullptr, "export let y = 2;"},
};

/* A second import() of a module that is still waiting on top-level await settles after the first
 * (test262 top-level-await/dynamic-import-of-waiting-module). */
EMB_TEST(module_dynamic_import_of_waiting_module)
{
    MOD_BEGIN(waiting_mods, 2);
    run_src("globalThis.gate = new Promise(function (r) { globalThis.cont = r; });"
            "globalThis.st = new Promise(function (r) { globalThis.started = r; });"
            "globalThis.res = [];"
            "globalThis.p1 = import('wf');"
            "(async function () {"
            "  await st;"
            "  globalThis.p2 = import('wf');"
            "  await import('wf2');"
            "  cont();"
            "  await Promise.all([p1.then(function () { res.push(1); }),"
            "                     p2.then(function () { res.push(2); })]);"
            "  globalThis.done = res.join();"
            "})();");
    drain();
    expect_text("globalThis.done", "1,2");
    MOD_END();
}

/* The rejection order of two imports that share an async module (test262 top-level-await). */
EMB_TEST(module_tla_rejection_order)
{
    MOD_BEGIN(rejorder_mods, 6);
    watch_module(1, "ro_main");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.done", "B,A");
    MOD_END();
}

EMB_TEST(module_tla_sync_throw_rejects)
{
    MOD_BEGIN(order_mods, 6);
    watch_module(1, "tthrow");
    watch_module(2, "tparent");
    drain();
    expect_text("S1", "err:RangeError");
    expect_text("S2", "err:RangeError");
    expect_text("globalThis.out", "");
    MOD_END();
}

static const modsrc eval_mods[] = {
    {"ev_b", nullptr, "export let q = 'imp';"},
    {"ev_m", nullptr,
     "import { q } from 'ev_b';"
     "globalThis.out = eval('q') + ':' + (function () { return eval('q'); })();"
     "globalThis.p1 = eval('q'); try { eval('q = 1'); globalThis.w = 'no' + eval('q'); } catch (e) { globalThis.w = e.name; }" "globalThis.gq = typeof globalThis.q;"},
    {"this_m", nullptr, "globalThis.tt = String(this === undefined);"},
};

EMB_TEST(module_direct_eval_sees_imports)
{
    MOD_BEGIN(eval_mods, 3);
    watch_module(1, "ev_m");
    watch_module(2, "this_m");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "imp:imp");
    expect_text("globalThis.w", "TypeError");
    expect_text("globalThis.tt", "true");
    expect_text("globalThis.p1", "imp");
    expect_text("globalThis.gq", "undefined");
    expect_text("S2", "ok");
    MOD_END();
}

static const modsrc throw_mods[] = {
    {"errdep", nullptr, "throw new RangeError('boom');"},
    {"errmain", nullptr, "import 'errdep'; globalThis.out = 'ran';"},
    {"linkbad", nullptr, "import { nope } from 'ev_b';"},
    {"ev_b", nullptr, "export let x = 1;"},
};

EMB_TEST(module_evaluation_and_link_errors)
{
    MOD_BEGIN(throw_mods, 4);
    watch_module(1, "errmain");
    watch_module(2, "linkbad");
    drain();
    expect_text("S1", "err:RangeError");
    expect_text("S2", "err:SyntaxError");
    expect_text("globalThis.out", "");
    MOD_END();
}

static const modsrc early_mods[] = {
    {"e_dupexp", nullptr, "export { a }; export { a }; var a;"},
    {"e_unres", nullptr, "export { nope };"},
    {"e_await", nullptr, "var await;"},
    {"e_dupdefault", nullptr, "export default 1; export default 2;"},
    {"e_dupname", nullptr, "let z; var z;"},
    {"e_comma_from", nullptr, "import a, from 'b';"},
    {"e_comma_str", nullptr, "import a, 'b';"},
    {"e_comma_from_attr", nullptr, "import a, from 'b' with { type: 'json' };"},
    {"e_star_noas", nullptr, "import * from 'b';"},
    {"e_empty_clause", nullptr, "import from 'b';"},
    {"b", nullptr, "export default 1;"},
};

EMB_TEST(module_early_errors)
{
    MOD_BEGIN(early_mods, 11);
    watch_module(6, "e_comma_from");
    watch_module(7, "e_comma_str");
    watch_module(8, "e_comma_from_attr");
    watch_module(9, "e_star_noas");
    watch_module(10, "e_empty_clause");
    watch_module(1, "e_dupexp");
    watch_module(2, "e_unres");
    watch_module(3, "e_await");
    watch_module(4, "e_dupdefault");
    watch_module(5, "e_dupname");
    drain();
    for (int i = 1; i <= 10; i++) {
        char expr[8];
        SDL_snprintf(expr, sizeof expr, "S%d", i);
        expect_text(expr, "err:SyntaxError");
    }
    MOD_END();
}

/* export * skips "default"; an export * cycle terminates; a dynamic import of a graph with a
 * missing dependency rejects. */
static const modsrc star_default_mods[] = {
    {"sd_src", nullptr, "export default 'd'; export var k = 'k';"},
    {"sd_star", nullptr, "export * from 'sd_src';"},
    {"sd_use", nullptr,
     "import * as ns from 'sd_star'; globalThis.out = Object.keys(ns).join() + '|' + ('default' in ns);"},
    {"cy_a", nullptr, "export * from 'cy_b'; export var fromA = 1;"},
    {"cy_b", nullptr, "export * from 'cy_a'; export var fromB = 2;"},
    {"cy_use", nullptr,
     "import * as ns from 'cy_a'; globalThis.cyout = Object.keys(ns).join();"},
    {"dm_a", nullptr, "import 'dm_missing'; globalThis.dmran = 1;"},
};

EMB_TEST(module_star_default_cycle_and_failed_dynamic_graph)
{
    MOD_BEGIN(star_default_mods, 7);
    watch_module(1, "sd_use");
    watch_module(2, "cy_use");
    run_src("globalThis.dm = 'pending';"
            "import('dm_a').then(function () { globalThis.dm = 'ok'; },"
            "function (e) { globalThis.dm = 'err:' + e.name; });");
    drain();
    expect_text("S1", "ok");
    expect_text("S2", "ok");
    expect_text("globalThis.out", "k|false");
    expect_text("globalThis.cyout", "fromA,fromB");
    expect_text("globalThis.dm", "err:Error");
    expect_text("globalThis.dmran", "undefined");
    MOD_END();
}

/* ------------------------------------------------------------------ the host loader */

static const modsrc referrer_mods[] = {
    {"main_m", nullptr, "import { x } from 'b'; import d from 'data.json' with { type: 'json' };"
                        "globalThis.out = x + ':' + d.k;"},
    {"b", nullptr, "export let x = 41;"},
    {"data.json", "json", "{\"k\": 7}"},
};

EMB_TEST(module_loader_sees_referrer_specifier_type)
{
    MOD_BEGIN(referrer_mods, 3);
    watch_module(1, "main_m");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "41:7");
    expect_log("-|main_m|-;main_m|b|-;main_m|data.json|json;");
    MOD_END();
}

static const modsrc nojson_mods[] = {
    {"data.json", "json", "{\"k\": 7}"},
    {"main_n", nullptr, "import d from 'data.json'; globalThis.out = d;"},
};

EMB_TEST(module_json_import_needs_type)
{
    MOD_BEGIN(nojson_mods, 2);
    watch_module(1, "main_n");
    drain();
    expect_text("S1", "err:Error");
    expect_text("globalThis.out", "");
    MOD_END();
}

static const modsrc json_mods[] = {
    {"b", nullptr, "export let x = 41;"},
    {"data.json", "json", "{\"k\": 7}"},
    {"css.json", "css", "{\"k\": 9}"},
    {"bad.json", "json", "{"},
};

EMB_TEST(module_dynamic_import_success_and_failure)
{
    MOD_BEGIN(json_mods, 4);
    run_src("globalThis.r0 = ''; globalThis.r1 = ''; globalThis.r2 = ''; globalThis.r3 = '';"
            "globalThis.r4 = ''; globalThis.r5 = ''; globalThis.r6 = '';"
            "import('b').then(function (ns) { globalThis.r0 = 'x' + ns.x; });"
            "import('missing').catch(function (e) { globalThis.r1 = e.name + ':' + e.message; });"
            "import('b', 1).catch(function (e) { globalThis.r2 = e.name; });"
            "import('b', { with: { type: 1 } }).catch(function (e) { globalThis.r3 = e.name; });"
            "import('data.json', { with: { type: 'json' } })"
            "  .then(function (ns) { globalThis.r4 = 'j' + ns.default.k; });"
            "import('css.json', { with: { type: 'css' } })"
            "  .catch(function (e) { globalThis.r5 = e.name; });"
            "import('bad.json', { with: { type: 'json' } })"
            "  .catch(function (e) { globalThis.r6 = e.name; });");
    drain();
    expect_text("[globalThis.r0, globalThis.r1, globalThis.r2, globalThis.r3, globalThis.r4, "
                "globalThis.r5, globalThis.r6].join('|')",
                "x41|Error:no such module|TypeError|TypeError|j7|TypeError|SyntaxError");
    MOD_END();
}

static const modsrc dynamic_order_mods[] = {
    {"dfs_main", nullptr, "import 'dfs_a'; import 'dfs_b';"},
    {"dfs_a", nullptr, "import('dfs_b'); globalThis.out += 'A';"},
    {"dfs_b", nullptr, "globalThis.out += 'B';"},
};

EMB_TEST(module_dynamic_import_does_not_preempt)
{
    MOD_BEGIN(dynamic_order_mods, 3);
    watch_module(1, "dfs_main");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "AB");
    MOD_END();
}

static const modsrc meta_mods[] = {
    {"meta_m", nullptr,
     "globalThis.out = import.meta.url + '|' + (Object.getPrototypeOf(import.meta) === null) + '|'"
     "  + (import.meta === import.meta);"},
};

EMB_TEST(module_import_meta_and_hook)
{
    MOD_BEGIN(meta_mods, 1);
    watch_module(1, "meta_m");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "meta_m|true|true");
    EMB_CHECK(T.meta_hits == 1);
    EMB_CHECK(strcmp(T.meta_url, "meta_m") == 0);
    MOD_END();
}

static const modsrc concurrent_mods[] = {
    {"A", nullptr, "import 'mid'; globalThis.out += 'A';"},
    {"mid", nullptr, "import 'leaf'; globalThis.out += 'M';"},
    {"leaf", nullptr, "globalThis.out += 'L';"},
};

/* Two jobs share a graph that the other job is still loading: each finishes only when its whole
 * graph is loaded, and every module evaluates once. */
EMB_TEST(module_concurrent_jobs_share_loading_graph)
{
    MOD_BEGIN(concurrent_mods, 3);
    T.defer = true;
    watch_module(1, "A");
    watch_module(2, "mid");
    EMB_CHECK(T.nq == 2);
    drain();
    expect_text("S1", "ok");
    expect_text("S2", "ok");
    expect_text("globalThis.out", "LMA");
    MOD_END();
}

EMB_TEST(module_async_loader_completes_later)
{
    MOD_BEGIN(basic_mods, 2);
    T.defer = true;
    watch_module(1, "a");
    expect_text("S1", "pending");
    embi_js_run_jobs(g_rt);
    expect_text("S1", "pending");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.got", "42");
    MOD_END();
}

EMB_TEST(module_embedder_namespace_api)
{
    MOD_BEGIN(basic_mods, 2);
    watch_module(1, "b");
    drain();
    EMB_CHECK(embi_js_is_object(embi_js_module_namespace(g_rt, "b", 1)));
    embi_js_value ns = embi_js_module_namespace(g_rt, "nope", 4);
    EMB_CHECK(ns == EMBI_JS_EXCEPTION);
    embi_js_exception_clear(g_rt);
    MOD_END();
}

/* Live bytes that n runs of src add (each run is drained, then a collection is run). */
static size_t growth_of(const char *src, int n)
{
    run_src(src);
    drain();
    embi_js_gc(g_rt);
    size_t before = embi_mem_live();
    for (int i = 0; i < n; i++) {
        run_src(src);
        drain();
    }
    embi_js_gc(g_rt);
    size_t after = embi_mem_live();
    return after > before ? after - before : 0;
}

/* Repeated import() must not keep its jobs, requests or promises. The engine's own per-run
 * retention (a plain script that creates promises) is the control; the import loop may add at most
 * a fixed allowance over it. */
EMB_TEST(module_repeated_imports_do_not_accumulate)
{
    MOD_BEGIN(basic_mods, 2);
    /* Warm-up: the first loop of imports can add one chunk that the heap keeps (J5m3: chunks grow
     * geometrically, so the import path first needs a 256 KB chunk). That is a one-time cost; the
     * loops after it add nothing, which is what this test is about. */
    (void)growth_of("import('b'); import('a'); import('b');", 200);
    size_t control = growth_of("Promise.resolve(1).then(function () {}); Promise.resolve(2);", 200);
    size_t imports = growth_of("import('b'); import('a'); import('b');", 200);
    EMB_CHECK(imports <= control + 262144u);
    MOD_END();
}

EMB_TEST(module_repeated_failures_do_not_accumulate)
{
    MOD_BEGIN(basic_mods, 2);
    /* Warm-up, as in the test above: the first loop may make the heap keep one more chunk (a one-time
     * cost that depends on how the size classes of the compiled code fall on pages). */
    (void)growth_of("import('missing').catch(function () {});", 200);
    size_t control = growth_of("Promise.resolve(1).then(function () {}); Promise.resolve(2);", 200);
    size_t failing = growth_of("import('missing').catch(function () {});", 200);
    size_t failing4 = growth_of("import('missing').catch(function () {});", 800);
    if (failing > control + 262144u || failing4 > control + 262144u) {
        SDL_Log("failing growth %llu (800 runs: %llu), control %llu", (unsigned long long)failing,
                (unsigned long long)failing4, (unsigned long long)control);
    }
    EMB_CHECK(failing <= control + 262144u);
    EMB_CHECK(failing4 <= control + 262144u);
    MOD_END();
}

/* ------------------------------------------------------------------ review fixes (J3c round 3) */

/* Namespace objects hold fresh boxes (export * as ns): a collection between the boxes must not free
 * one. The collector runs on nearly every allocation. */
static const modsrc nsgc_mods[] = {
    {"l1", nullptr, "export let v = 1;"},
    {"l2", nullptr, "export let v = 2;"},
    {"l3", nullptr, "export let v = 3;"},
    {"l4", nullptr, "export let v = 4;"},
    {"l5", nullptr, "export let v = 5;"},
    {"l6", nullptr, "export let v = 6;"},
    {"ns_m", nullptr,
     "export * as a from 'l1'; export * as b from 'l2'; export * as c from 'l3';"
     "export * as d from 'l4'; export * as e from 'l5'; export * as f from 'l6';"
     "export * as g from 'l1'; export * as h from 'l2';"},
    {"ns_user", nullptr,
     "import * as ns from 'ns_m'; import { a as ra } from 'ns_m';"
     "globalThis.out = Object.keys(ns).join() + ':' + ns.a.v + ns.b.v + ns.c.v + ns.d.v + ns.e.v +"
     "ns.f.v + ns.g.v + ns.h.v + ':' + (ns.a === ns.g) + (ra === ns.a);"},
};

EMB_TEST(module_namespace_boxes_survive_collections)
{
    MOD_BEGIN(nsgc_mods, 8);
    embi_js_set_gc_params(g_rt, 1, 0.0);
    watch_module(1, "ns_user");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "a,b,c,d,e,f,g,h:12345612:truetrue");
    embi_js_set_gc_params(g_rt, 1u << 20, 2.0);
    MOD_END();
}

/* export default of an anonymous function, class or arrow is named "default". */
static const modsrc dn_mods[] = {
    {"dn_cls", nullptr, "export default (class {});"},
    {"dn_fn", nullptr, "export default (function () {});"},
    {"dn_gen", nullptr, "export default (function* () {});"},
    {"dn_arrow", nullptr, "export default () => {};"},
    {"dn_dcls", nullptr, "export default class {}"},
    {"dn_dfn", nullptr, "export default function () {}"},
    {"dn_named", nullptr, "export default (function g() {});"},
    {"dn_main", nullptr,
     "import a from 'dn_cls'; import b from 'dn_fn'; import c from 'dn_gen'; import d from 'dn_arrow';"
     "import e from 'dn_dcls'; import f from 'dn_dfn'; import g from 'dn_named';"
     "globalThis.out = [a, b, c, d, e, f, g].map(function (x) { return x.name; }).join();"},
};

EMB_TEST(module_export_default_names_anonymous_definitions)
{
    MOD_BEGIN(dn_mods, 8);
    watch_module(1, "dn_main");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "default,default,default,default,default,default,g");
    MOD_END();
}

/* Top-level functions are lexical in module code: repeats are early errors. */
static const modsrc dup_mods[] = {
    {"e1", nullptr, "function f() {} function f() {}"},
    {"e2", nullptr, "function* f() {} function f() {}"},
    {"e3", nullptr, "var f; function f() {}"},
    {"e4", nullptr, "function f() {} var f;"},
    {"e5", nullptr, "export default function f() {} function f() {}"},
    {"e6", nullptr, "async function f() {} let f;"},
    {"e7", nullptr, "let f; function f() {}"},
    {"e8", nullptr, "async function* f() {} async function f() {}"},
    {"ok1", nullptr, "function f() {} { function f() {} } var g; var g;"},
};

EMB_TEST(module_top_level_function_redeclarations_are_early_errors)
{
    MOD_BEGIN(dup_mods, 9);
    const char *names[] = {"e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8"};
    for (int i = 0; i < 8; i++) {
        watch_module(i + 1, names[i]);
    }
    watch_module(9, "ok1");
    drain();
    for (int i = 1; i <= 8; i++) {
        char expr[8];
        SDL_snprintf(expr, sizeof expr, "S%d", i);
        expect_text(expr, "err:SyntaxError");
    }
    expect_text("S9", "ok");
    MOD_END();
}

/* A member of an errored cycle rejects with the cycle's error; a member of a pending async cycle
 * settles together with the cycle root. */
static const modsrc cyc_mods[] = {
    {"cy_a", nullptr, "import 'cy_b'; throw new Error('boom');"},
    {"cy_b", nullptr, "import 'cy_a'; globalThis.out += 'b';"},
    {"cy_ta", nullptr,
     "import 'cy_tb'; await new Promise(function (r) { globalThis.release = r; });"
     "globalThis.out += 'a';"},
    {"cy_tb", nullptr, "import 'cy_ta'; globalThis.out += 'b';"},
};

EMB_TEST(module_import_of_errored_cycle_member_rejects)
{
    MOD_BEGIN(cyc_mods, 4);
    watch_module(1, "cy_a");
    drain();
    expect_text("S1", "err:Error");
    run_src("globalThis.S2 = 'pending';"
            "import('cy_b').then(function () { S2 = 'ok'; }, function (e) { S2 = 'err:' + e.message; });");
    drain();
    expect_text("S2", "err:boom");
    MOD_END();
}

EMB_TEST(module_import_of_async_cycle_member_waits_for_the_root)
{
    MOD_BEGIN(cyc_mods, 4);
    watch_module(1, "cy_ta");
    drain();
    expect_text("S1", "pending");
    expect_text("globalThis.out", "b");
    run_src("import('cy_tb').then(function () { out += '|imp'; });");
    drain();
    expect_text("globalThis.out", "b");
    run_src("release();");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "ba|imp");
    MOD_END();
}

/* The namespace object's @@toStringTag and exports follow OrdinaryDefineOwnProperty. */
static const modsrc nsdef_mods[] = {
    {"ns_leaf", nullptr, "export let v = 1;"},
    {"ns_def", nullptr, "import * as ns from 'ns_leaf'; globalThis.NS = ns;"},
};

EMB_TEST(module_namespace_define_own_property)
{
    MOD_BEGIN(nsdef_mods, 2);
    watch_module(1, "ns_def");
    drain();
    expect_text("S1", "ok");
    expect_text("[Reflect.defineProperty(NS, Symbol.toStringTag, {value: 'Module', writable: false,"
                " enumerable: false, configurable: false}),"
                "Reflect.defineProperty(NS, Symbol.toStringTag, {writable: true}),"
                "Reflect.defineProperty(NS, Symbol.toStringTag, {enumerable: true}),"
                "Reflect.defineProperty(NS, Symbol.toStringTag, {configurable: true}),"
                "Reflect.defineProperty(NS, Symbol.toStringTag, {value: 'X'}),"
                "Reflect.defineProperty(NS, Symbol.toStringTag, {})].join()",
                "true,false,false,false,false,true");
    expect_text("[Reflect.defineProperty(NS, 'v', {value: 1, writable: true, enumerable: true,"
                " configurable: false}),"
                "Reflect.defineProperty(NS, 'v', {writable: false}),"
                "Reflect.defineProperty(NS, 'v', {value: 2}),"
                "Reflect.defineProperty(NS, 'nope', {value: 1})].join()",
                "true,false,false,false");
    MOD_END();
}

/* import() reads every attribute of options.with; non-string values and unsupported keys are a
 * TypeError, abrupt reads reject with the thrown value. A static unsupported key fails the load. */
static const modsrc attr_mods[] = {
    {"b", nullptr, "export let x = 41;"},
    {"unsup", nullptr, "import { x } from 'b' with { foo: 'bar' }; globalThis.out = 'ran';"},
};

EMB_TEST(module_import_attributes_validation)
{
    MOD_BEGIN(attr_mods, 2);
    run_src("function t(n, p) { globalThis['S' + n] = 'pending';"
            "  p.then(function () { globalThis['S' + n] = 'ok'; },"
            "         function (e) { globalThis['S' + n] = 'err:' + e.name; }); }"
            "t(1, import('b', { with: { foo: 'bar' } }));"
            "t(2, import('b', { with: { foo: 1 } }));"
            "t(3, import('b', { with: { get type() { throw new RangeError('x'); } } }));"
            "t(4, import('b', { with: 5 }));"
            "t(5, import('b', { with: {} }));"
            "t(6, import('b', 7));"
            "t(7, import('b', { with: { type: 'text' } }));"
            "var log = [];"
            "t(8, import('b', { with: new Proxy({}, { ownKeys: function () { log.push('k'); return ['z']; },"
            "  getOwnPropertyDescriptor: function () { log.push('d'); return {value: 's', enumerable: true,"
            "  configurable: true}; }, get: function () { log.push('g'); return 's'; } }) }));");
    watch_module(9, "unsup");
    drain();
    expect_text("S1", "err:TypeError");
    expect_text("S2", "err:TypeError");
    expect_text("S3", "err:RangeError");
    expect_text("S4", "err:TypeError");
    expect_text("S5", "ok");
    expect_text("S6", "err:TypeError");
    expect_text("S7", "err:Error"); /* the host has no such module */
    expect_text("S8", "err:TypeError");
    expect_text("log.join()", "k,d,g");
    expect_text("S9", "err:SyntaxError");
    expect_text("globalThis.out", "");
    MOD_END();
}

/* A JSON module is parsed by the intrinsic parser: user code cannot replace it. */
static const modsrc jsonhj_mods[] = {
    {"data.json", "json", "{\"k\": 7}"},
    {"jm", nullptr,
     "import d from 'data.json' with { type: 'json' }; globalThis.out = d.k;"},
};

EMB_TEST(module_json_ignores_replaced_json_parse)
{
    MOD_BEGIN(jsonhj_mods, 2);
    run_src("JSON.parse = function () { throw new Error('hijack'); };");
    watch_module(1, "jm");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "7");
    MOD_END();
}

/* An export named like an array index is found by [[Get]], [[HasProperty]] and the descriptor. */
static const modsrc ix_mods[] = {
    {"ix", nullptr, "export let x = 'v0'; export { x as \"0\" };"},
    {"ix_main", nullptr,
     "import * as ns from 'ix'; import { \"0\" as z } from 'ix';"
     "globalThis.out = [Object.keys(ns).join(), ns[0], ns['0'], 0 in ns,"
     "Object.getOwnPropertyDescriptor(ns, '0').value, z].join('|');"},
};

EMB_TEST(module_namespace_index_named_export)
{
    MOD_BEGIN(ix_mods, 2);
    watch_module(1, "ix_main");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out", "0,x|v0|v0|true|v0|v0");
    MOD_END();
}

/* import() inside direct eval in a module uses the module as its referrer. */
static const modsrc evref_mods[] = {
    {"dir/ev", nullptr, "eval(\"import('./sib')\");"},
    {"./sib", nullptr, "export let q = 1;"},
};

EMB_TEST(module_dynamic_import_in_eval_has_the_module_referrer)
{
    MOD_BEGIN(evref_mods, 2);
    watch_module(1, "dir/ev");
    drain();
    expect_text("S1", "ok");
    expect_log("-|dir/ev|-;dir/ev|./sib|-;");
    MOD_END();
}

/* A long chain loads in one job (the module map is a hash, graph walks are linear). */
EMB_TEST(module_long_chain_loads)
{
    enum { N = 300 };
    static modsrc mods[N];
    static char srcs[N][96];
    static char names[N][16];
    for (int i = 0; i < N; i++) {
        SDL_snprintf(names[i], sizeof names[i], "w%d", i);
        if (i + 1 < N) {
            SDL_snprintf(srcs[i], sizeof srcs[i], "import 'w%d'; globalThis.out += 'x';", i + 1);
        } else {
            SDL_snprintf(srcs[i], sizeof srcs[i], "globalThis.out += 'x';");
        }
        mods[i] = (modsrc){names[i], nullptr, srcs[i]};
    }
    MOD_BEGIN(mods, N);
    T.defer = true;
    watch_module(1, "w0");
    drain();
    expect_text("S1", "ok");
    expect_text("globalThis.out.length", "300");
    MOD_END();
}

EMB_TEST_MAIN()
