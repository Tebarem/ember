/*
 * Async functions (ES2017, J3b): declarations, expressions, arrows, object and class methods,
 * await ordering and tick counts, rejection, parameter errors, the early errors of await, and
 * Function.prototype.toString. Each case runs a script, drains the job queue, and compares the
 * global `log`.
 */
#include "emb_test.h"

#include "js/jobs.h"
#include "js/js.h"
#include "js/promise.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
    size_t mem0; /* live bytes before the runtime existed: must return here */
} env;

static env env_new(void) {
    env e;
    e.mem0 = embi_mem_live();
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e) {
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
    EMB_CHECK(embi_mem_live() == e.mem0);
}

static embi_js_value run(env e, const char *src) {
    return embi_js_eval(e.r, src, strlen(src), "async.js", 0);
}

/* True when the value is the string want. */
static bool value_text_is(embi_js_runtime *rt, embi_js_value v, const char *want) {
    embi_js_string *s = embi_js_to_string(rt, v);
    if (s == nullptr) {
        embi_js_exception_clear(rt);
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

/* Runs src, drains the jobs, and checks that the global log then reads want. */
static void expect_log(env e, const char *src, const char *want) {
    embi_js_value v = run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    script threw: %s", src);
        EMB_CHECK(false);
        embi_js_exception_clear(e.rt);
        return;
    }
    embi_js_run_jobs(e.rt);
    embi_js_value log = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "log"));
    bool ok = value_text_is(e.rt, log, want);
    if (!ok) {
        SDL_Log("    script: %s", src);
        SDL_Log("    wanted log \"%s\"", want);
    }
    EMB_CHECK(ok);
}

#define PRELUDE "var log = '';"

/* ------------------------------------------------------------------ results */

EMB_TEST(async_function_returns_a_promise_of_its_value) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "async function f() { return 1; } f().then(function (v) { log += v; });",
               "1");
    expect_log(e,
               PRELUDE "(async function () {})() instanceof Promise && (log += 'p');", "p");
    env_free(e);
}

EMB_TEST(async_function_body_runs_synchronously_until_first_await) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f() { log += 'a'; await 0; log += 'c'; }"
                       "f(); log += 'b';",
               "abc");
    env_free(e);
}

EMB_TEST(await_of_a_native_promise_takes_one_tick) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* await subscribes to the promise itself, so its continuation runs before the later then. */
    expect_log(e,
               PRELUDE "async function f() { await Promise.resolve(); log += '1'; }"
                       "f(); Promise.resolve().then(function () { log += '2'; });",
               "12");
    env_free(e);
}

EMB_TEST(await_unwraps_thenables) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f() { return await { then: function (r) { r(5); } }; }"
                       "f().then(function (v) { log += v; });",
               "5");
    env_free(e);
}

EMB_TEST(returned_promise_is_adopted) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f() { return Promise.reject('z'); }"
                       "f().catch(function (x) { log += x; });",
               "z");
    env_free(e);
}

/* ------------------------------------------------------------------ rejection */

EMB_TEST(throw_rejects_the_result_promise) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "async function f() { throw 'x'; } f().catch(function (v) { log += v; });",
               "x");
    env_free(e);
}

EMB_TEST(awaited_rejection_is_catchable_in_the_body) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f() { try { await Promise.reject('r'); } catch (x) { log += x; } }"
                       "f();",
               "r");
    env_free(e);
}

/* Errors in the parameter list reject the promise instead of throwing to the caller. */
EMB_TEST(parameter_errors_reject_instead_of_throwing) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f(a = (function () { throw 'p'; })()) {}"
                       "var out = 'none'; try { f().catch(function (x) { log += x; }); } catch (y) { log += 'threw'; }",
               "p");
    env_free(e);
}

/* ------------------------------------------------------------------ forms */

EMB_TEST(async_arrow_functions) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "var g = async (x) => x * 2; g(3).then(function (v) { log += v; });", "6");
    expect_log(e, PRELUDE "var h = async x => await x; h(7).then(function (v) { log += v; });", "7");
    env_free(e);
}

EMB_TEST(async_methods_in_objects_and_classes) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var o = { async m() { return await 4; } };"
                       "o.m().then(function (v) { log += v; });",
               "4");
    expect_log(e,
               PRELUDE "class C { async m() { return 'k'; } static async s() { return 's'; } }"
                       "new C().m().then(function (v) { log += v; });"
                       "C.s().then(function (v) { log += v; });",
               "ks");
    env_free(e);
}

EMB_TEST(async_function_is_not_a_constructor) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function f() {} try { new f(); } catch (x) { log += x.name; }",
               "TypeError");
    env_free(e);
}

EMB_TEST(async_function_tag_and_source_text) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "log += Object.prototype.toString.call(async function () {});",
               "[object AsyncFunction]");
    expect_log(e, PRELUDE "log += (async function f() { }).toString();",
               "async function f() { }");
    env_free(e);
}

/* ------------------------------------------------------------------ early errors */

EMB_TEST(await_is_an_identifier_in_scripts) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "var await = 3; log += await;", "3");
    env_free(e);
}

EMB_TEST(await_in_async_parameters_is_a_syntax_error) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('async function f(a = await 1) {}'); } catch (x) { log += x.name; }",
               "SyntaxError");
    env_free(e);
}

EMB_TEST(yield_is_reserved_inside_generator_bodies) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('function* g() { var yield; }'); } catch (x) { log += x.name; }",
               "SyntaxError");
    env_free(e);
}

EMB_TEST(for_async_of_is_rejected) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('for (async of [1]) ;'); } catch (x) { log += x.name; }",
               "SyntaxError");
    env_free(e);
}

/* The async prefix of an object or class member is a method's: a shorthand or a field is an error,
 * and an async constructor is an error (ECMA-262 13.2.5.1, 15.7.1). */
EMB_TEST(async_prefix_needs_a_method) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('({async async})'); } catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('class C { async x }'); } catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('({async async(){}, async a(){}})'); log += 'ok'; }"
                       "catch (x) { log += x.name; }",
               "ok");
    env_free(e);
}

EMB_TEST(async_constructor_is_an_early_error) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('class C { async constructor(){} }'); } catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('class C { async *constructor(){} }'); } catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('class C { static async constructor(){} }'); log += 'ok'; }"
                       "catch (x) { log += x.name; }",
               "ok");
    env_free(e);
}

/* In a sloppy generator body yield is an identifier for a non-generator function expression's
 * name, but not for a generator's name or as a label (ECMA-262 14.1, 15.5.1). */
EMB_TEST(yield_names_in_generator_bodies) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('function* g() { (function yield() {}); }'); log += 'ok'; }"
                       "catch (x) { log += x.name; }",
               "ok");
    expect_log(e,
               PRELUDE "try { eval('function* g() { (function* yield() {}); }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('function* g() { yield: ; }'); } catch (x) { log += x.name; }",
               "SyntaxError");
    env_free(e);
}

/* A function declared in a block may repeat only a plain function declaration, and only in sloppy
 * code (Annex B.3.2.4, B.3.3.4). */
EMB_TEST(block_function_redeclarations) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('{ function f() {} function f() {} }'); log += 'ok'; }"
                       "catch (x) { log += x.name; }",
               "ok");
    expect_log(e,
               PRELUDE "try { eval('{ function f() {} async function f() {} }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('{ async function f() {} function f() {} }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('{ async function f() {} async function f() {} }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('{ function* f() {} function* f() {} }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    expect_log(e,
               PRELUDE "try { eval('{ function f() {} function* f() {} }'); }"
                       "catch (x) { log += x.name; }",
               "SyntaxError");
    env_free(e);
}

/* ------------------------------------------------------------------ Array.fromAsync */

EMB_TEST(array_from_async_shape) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "log += Array.fromAsync.length + Array.fromAsync.name;", "1fromAsync");
    expect_log(e,
               PRELUDE "try { new Array.fromAsync([]); } catch (x) { log += x.name; }",
               "TypeError");
    env_free(e);
}

EMB_TEST(array_from_async_collects_sync_and_array_like_items) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "Array.fromAsync([1, 2]).then(function (a) { log += a.join(); });", "1,2");
    expect_log(e,
               PRELUDE "Array.fromAsync([Promise.resolve('p'), 'q'])"
                       "  .then(function (a) { log += a.join(); });",
               "p,q");
    expect_log(e,
               PRELUDE "Array.fromAsync({ length: 2, 0: Promise.resolve('a'), 1: 'b' })"
                       "  .then(function (a) { log += a.join(); });",
               "a,b");
    env_free(e);
}

EMB_TEST(array_from_async_collects_async_iterables) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var o = { [Symbol.asyncIterator]() { var i = 0; return {"
                       "  next() { i++; return Promise.resolve(i <= 2 ? { value: i * 10, done: false }"
                       "                                             : { done: true }); } }; } };"
                       "Array.fromAsync(o).then(function (a) { log += a.join(); });",
               "10,20");
    env_free(e);
}

EMB_TEST(array_from_async_maps_values) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Array.fromAsync([1, 2], async function (x, k) { return x + k; })"
                       "  .then(function (a) { log += a.join(); });",
               "1,3");
    expect_log(e,
               PRELUDE "Array.fromAsync([1, 2], function (x) { return Promise.resolve(x * 3); })"
                       "  .then(function (a) { log += a.join(); });",
               "3,6");
    env_free(e);
}

EMB_TEST(array_from_async_rejects_on_errors) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "Array.fromAsync([Promise.reject('r')]).catch(function (x) { log += x; });", "r");
    expect_log(e, PRELUDE "Array.fromAsync([], 5).catch(function (x) { log += x.name; });", "TypeError");
    env_free(e);
}

/* A throwing mapping function closes the async iterator (its return result is awaited) and then
 * rejects with the mapping error. */
EMB_TEST(array_from_async_closes_the_iterator_on_a_mapping_error) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var o = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { log += 'R'; return { done: true }; } }; } };"
                       "Array.fromAsync(o, function () { throw 'm'; }).catch(function (x) { log += x; });",
               "Rm");
    env_free(e);
}

EMB_TEST_MAIN()
