/*
 * Async generators and for-await-of (ES2018, J3b): the request queue, yield and return awaiting
 * their operands, resumption by throw and return, AsyncFromSyncIterator, and the closing of the
 * iterator on break and throw. Each case runs a script, drains the job queue, and compares the
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
    return embi_js_eval(e.r, src, strlen(src), "asyncgen.js", 0);
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

/* ------------------------------------------------------------------ objects and protocol */

EMB_TEST(async_generator_objects_have_the_async_iterator_protocol) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() {} var it = g();"
                       "log += (it[Symbol.asyncIterator]() === it);"
                       "log += Object.prototype.toString.call(it);",
               "true[object AsyncGenerator]");
    expect_log(e,
               PRELUDE "async function* g() {} log += Object.prototype.toString.call(g);",
               "[object AsyncGeneratorFunction]");
    env_free(e);
}

/* ------------------------------------------------------------------ request queue */

EMB_TEST(next_requests_are_answered_in_order) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { yield 'a'; yield 'b'; }"
                       "var it = g();"
                       "it.next().then(function (r) { log += r.value; });"
                       "it.next().then(function (r) { log += r.value; });",
               "ab");
    env_free(e);
}

EMB_TEST(yield_operands_are_awaited) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { yield Promise.resolve('p'); }"
                       "g().next().then(function (r) { log += r.value; });",
               "p");
    expect_log(e,
               PRELUDE "async function* g() { yield Promise.reject('no'); }"
                       "g().next().catch(function (x) { log += x; });",
               "no");
    env_free(e);
}

EMB_TEST(return_values_are_awaited) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { return Promise.resolve('r'); }"
                       "g().next().then(function (r) { log += '' + r.value + r.done; });",
               "rtrue");
    env_free(e);
}

EMB_TEST(await_inside_the_body) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { var v = await Promise.resolve(5); yield v + 1; }"
                       "g().next().then(function (r) { log += r.value; });",
               "6");
    env_free(e);
}

/* ------------------------------------------------------------------ throw and return */

EMB_TEST(throw_resumes_at_the_yield) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { try { yield 1; } catch (x) { log += 'c' + x; } }"
                       "var it = g(); it.next();"
                       "it.throw('x').then(function (r) { log += r.done; });",
               "cxtrue");
    env_free(e);
}

EMB_TEST(return_runs_finally_blocks) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { try { yield 1; } finally { log += 'f'; } }"
                       "var it = g(); it.next();"
                       "it.return('v').then(function (r) { log += '' + r.value + r.done; });",
               "fvtrue");
    env_free(e);
}

/* A return on a generator that has not started completes it without running the body. */
EMB_TEST(return_before_start_skips_the_body) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { log += 'never'; }"
                       "g().return(7).then(function (r) { log += '' + r.value + r.done; });",
               "7true");
    env_free(e);
}

EMB_TEST(next_after_completion_gives_done) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() {} var it = g(); it.next();"
                       "it.next().then(function (r) { log += r.done; });",
               "true");
    env_free(e);
}

/* ------------------------------------------------------------------ for-await-of */

EMB_TEST(for_await_over_an_async_generator) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { yield 1; yield 2; }"
                       "(async function () { for await (var x of g()) log += x; })();",
               "12");
    env_free(e);
}

/* for await over a sync iterable wraps it in an AsyncFromSyncIterator that awaits each value. */
EMB_TEST(for_await_over_sync_iterables_awaits_values) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "(async function () { for await (var x of [Promise.resolve(1), 2]) log += x; })();",
               "12");
    expect_log(e,
               PRELUDE "(async function () { try { for await (var x of [Promise.reject('q')]) ; }"
                       "  catch (y) { log += y; } })();",
               "q");
    env_free(e);
}

EMB_TEST(break_in_for_await_closes_the_iterator) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { try { yield 1; yield 2; } finally { log += 'F'; } }"
                       "(async function () { for await (var x of g()) { log += x; break; }"
                       "  log += 'e'; })();",
               "1Fe");
    env_free(e);
}

EMB_TEST(throw_in_for_await_body_closes_the_iterator) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { try { yield 1; } finally { log += 'F'; } }"
                       "(async function () { try { for await (var x of g()) throw 'T'; }"
                       "  catch (y) { log += y; } })();",
               "FT");
    env_free(e);
}

EMB_TEST(for_await_requires_an_async_function) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "try { eval('function f() { for await (var x of []) ; }'); }"
                       "catch (y) { log += y.name; }",
               "SyntaxError");
    env_free(e);
}

/* ------------------------------------------------------------------ yield* */

EMB_TEST(yield_star_delegates_to_async_iterables) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* inner() { yield 'a'; return 'r'; }"
                       "async function* g() { var v = yield* inner(); yield v; }"
                       "(async function () { for await (var x of g()) log += x; })();",
               "ar");
    expect_log(e,
               PRELUDE "async function* g() { yield* [1, 2]; }"
                       "(async function () { for await (var x of g()) log += x; })();",
               "12");
    env_free(e);
}

EMB_TEST(yield_star_forwards_throw) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* inner() { try { yield 1; } catch (x) { log += 'c' + x; yield 2; } }"
                       "async function* g() { yield* inner(); }"
                       "var it = g(); it.next(); it.throw('x').then(function (r) { log += r.value; });",
               "cx2");
    env_free(e);
}

EMB_TEST(yield_star_forwards_return_to_the_inner_iterator) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* inner() { try { yield 1; } finally { log += 'F'; } }"
                       "async function* g() { yield* inner(); }"
                       "var it = g(); it.next();"
                       "it.return('v').then(function (r) { log += '' + r.value + r.done; });",
               "Fvtrue");
    expect_log(e,
               PRELUDE "async function* g() { yield* [1]; }"
                       "var it = g(); it.next();"
                       "it.return('q').then(function (r) { log += '' + r.value + r.done; });",
               "qtrue");
    env_free(e);
}

/* A missing throw method closes the iterator and throws a TypeError. */
EMB_TEST(yield_star_without_throw_closes_then_throws) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var inner = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { log += 'R'; return { done: true }; } }; } };"
                       "async function* g() { yield* inner; }"
                       "var it = g(); it.next();"
                       "it.throw('x').catch(function (x) { log += x.name; });",
               "RTypeError");
    env_free(e);
}

EMB_TEST(yield_star_propagates_inner_rejection) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* inner() { yield 1; throw 'boom'; }"
                       "async function* g() { try { yield* inner(); } catch (e) { log += e; } }"
                       "(async function () { for await (var x of g()) log += x; })();",
               "1boom");
    env_free(e);
}

/* ------------------------------------------------------------------ closing the iterator once
 * An async iterable whose return() counts its calls. The close of a break, return or throw runs
 * return exactly once, whatever it does: throws, rejects, or returns a non-object (a TypeError). */

EMB_TEST(break_closes_once_when_return_throws) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; throw 'R'; } }; } };"
                       "(async function () { try { for await (var x of it) { break; } }"
                       "  catch (e) { log += e + calls; } })();",
               "R1");
    env_free(e);
}

EMB_TEST(break_closes_once_when_return_rejects) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; return Promise.reject('J'); } }; } };"
                       "(async function () { try { for await (var x of it) { break; } }"
                       "  catch (e) { log += e + calls; } })();",
               "J1");
    env_free(e);
}

EMB_TEST(break_closes_once_when_return_gives_a_non_object) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; return 5; } }; } };"
                       "(async function () { try { for await (var x of it) { break; } }"
                       "  catch (e) { log += e.name + calls; } })();",
               "TypeError1");
    env_free(e);
}

EMB_TEST(return_in_for_await_closes_once_when_return_throws) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; throw 'R'; } }; } };"
                       "(async function () { try { for await (var x of it) { return 1; } }"
                       "  catch (e) { log += e + calls; } })();",
               "R1");
    env_free(e);
}

EMB_TEST(body_throw_closes_once_when_return_throws) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; throw 'R'; } }; } };"
                       "(async function () { try { for await (var x of it) { throw 'T'; } }"
                       "  catch (e) { log += e + calls; } })();",
               "T1");
    env_free(e);
}

EMB_TEST(body_throw_closes_once_when_return_rejects) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return { value: 1, done: false }; },"
                       "  return() { calls++; return Promise.reject('R'); } }; } };"
                       "(async function () { try { for await (var x of it) { throw 'T'; } }"
                       "  catch (e) { log += e + calls; } })();",
               "T1");
    env_free(e);
}

/* A rejected next() is the loop's own error: the iterator is not closed. */
EMB_TEST(rejected_next_does_not_close_the_iterator) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var calls = 0; var it = { [Symbol.asyncIterator]() { return {"
                       "  next() { return Promise.reject('N'); },"
                       "  return() { calls++; return {}; } }; } };"
                       "(async function () { try { for await (var x of it) ; }"
                       "  catch (e) { log += e + calls; } })();",
               "N0");
    env_free(e);
}

/* AsyncGenerator.prototype.return awaits its value at the yield, so a rejected value throws there
 * and the body can catch it. */
EMB_TEST(async_generator_return_rejection_is_thrown_at_the_yield) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "async function* g() { try { yield 1; } catch (e) { log += 'C' + e; }"
                       "  yield 2; }"
                       "var it = g();"
                       "it.next().then(function () { return it.return(Promise.reject('X')); })"
                       "  .then(function (r) { log += '|' + r.value + r.done; });",
               "CX|2false");
    env_free(e);
}

EMB_TEST_MAIN()
