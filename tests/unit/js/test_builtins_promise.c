/*
 * Promise (builtins_promise.c, J3a): settlement, reaction ordering (the tick counts of ECMA-262
 * 27.2 that test262 checks), thenables, the combinators, finally, try, withResolvers, species and
 * subclassing. Each case runs a script, drains the job queue, and compares the global `log`.
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
    return embi_js_eval(e.r, src, strlen(src), "promise.js", 0);
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

/* ------------------------------------------------------------------ ordering */

EMB_TEST(promise_reactions_run_in_registration_order) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.resolve().then(function () { log += 'a'; });"
                       "Promise.resolve().then(function () { log += 'b'; });",
               "ab");
    env_free(e);
}

/* Resolving with a native promise takes two extra ticks (the thenable job, then the reaction). */
EMB_TEST(promise_thenable_adoption_takes_two_extra_ticks) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var p = Promise.resolve();"
                       "var q = new Promise(function (r) { r(p); });"
                       "q.then(function () { log += 'q'; });"
                       "p.then(function () { log += '1'; })"
                       "  .then(function () { log += '2'; })"
                       "  .then(function () { log += '3'; });",
               "12q3");
    env_free(e);
}

EMB_TEST(promise_handlers_pass_values_through_when_missing) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "Promise.resolve(5).then().then(function (v) { log += v; });", "5");
    expect_log(e, PRELUDE "Promise.reject('e').then().catch(function (x) { log += x; });", "e");
    env_free(e);
}

EMB_TEST(promise_thenable_objects_are_adopted) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.resolve({ then: function (r) { r(9); } })"
                       "  .then(function (v) { log += v; });",
               "9");
    expect_log(e,
               PRELUDE "Promise.resolve({ get then() { throw 'g'; } })"
                       "  .catch(function (x) { log += x; });",
               "g");
    env_free(e);
}

EMB_TEST(promise_resolve_with_self_is_a_type_error) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var res; var p = new Promise(function (r) { res = r; });"
                       "res(p); p.catch(function (x) { log += x.name; });",
               "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ constructor */

EMB_TEST(promise_constructor_contract) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "try { Promise(function () {}); } catch (x) { log += x.name; }",
               "TypeError");
    expect_log(e, PRELUDE "try { new Promise(1); } catch (x) { log += x.name; }", "TypeError");
    expect_log(e,
               PRELUDE "new Promise(function () { throw 'x'; }).catch(function (x) { log += x; });",
               "x");
    expect_log(e,
               PRELUDE "log += [Promise.length, Promise.name, Promise.prototype.then.length,"
                       " Promise.prototype.catch.length, Promise.all.length].join(',');",
               "1,Promise,2,1,1");
    expect_log(e, PRELUDE "log += Object.prototype.toString.call(Promise.resolve());",
               "[object Promise]");
    expect_log(e, PRELUDE "log += (Promise[Symbol.species] === Promise);", "true");
    env_free(e);
}

/* ------------------------------------------------------------------ then, catch, finally */

EMB_TEST(promise_finally_passes_the_outcome_through) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.resolve(1).finally(function () { log += 'f'; })"
                       "  .then(function (v) { log += v; });",
               "f1");
    expect_log(e,
               PRELUDE "Promise.reject(2).finally(function () { log += 'f'; })"
                       "  .catch(function (x) { log += x; });",
               "f2");
    expect_log(e,
               PRELUDE "Promise.resolve(1).finally(function () { return Promise.reject('r'); })"
                       "  .catch(function (x) { log += x; });",
               "r");
    env_free(e);
}

/* ------------------------------------------------------------------ combinators */

EMB_TEST(promise_all_collects_values_or_the_first_rejection) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.all([1, Promise.resolve(2), 3])"
                       "  .then(function (a) { log += a.join(','); });",
               "1,2,3");
    expect_log(
        e, PRELUDE "Promise.all([Promise.reject('x'), 1]).catch(function (x) { log += x; });", "x");
    expect_log(e, PRELUDE "Promise.all([]).then(function (a) { log += a.length; });", "0");
    env_free(e);
}

EMB_TEST(promise_all_settled_reports_each_outcome) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.allSettled([1, Promise.reject(2)]).then(function (r) {"
                       "  log += r[0].status + r[0].value + r[1].status + r[1].reason; });",
               "fulfilled1rejected2");
    env_free(e);
}

EMB_TEST(promise_any_and_race) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "Promise.any([Promise.reject(1), Promise.reject(2)]).catch(function (e) {"
                       "  log += (e instanceof AggregateError) + ':' + e.errors.join(','); });",
               "true:1,2");
    expect_log(e,
               PRELUDE "Promise.any([Promise.reject(1), Promise.resolve(7)])"
                       "  .then(function (v) { log += v; });",
               "7");
    expect_log(e, PRELUDE "Promise.any([]).catch(function (e) { log += e.errors.length; });", "0");
    expect_log(e,
               PRELUDE "Promise.race([new Promise(function () {}), Promise.resolve('r')])"
                       "  .then(function (v) { log += v; });",
               "r");
    env_free(e);
}

/* ------------------------------------------------------------------ resolve, reject, try,
 * withResolvers */

EMB_TEST(promise_static_resolve_reject_try_with_resolvers) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e, PRELUDE "var p = Promise.resolve(); log += (Promise.resolve(p) === p);", "true");
    expect_log(e, PRELUDE "try { Promise.resolve.call(1, 2); } catch (x) { log += x.name; }",
               "TypeError");
    expect_log(e, PRELUDE "Promise.reject('no').catch(function (x) { log += x; });", "no");
    expect_log(e,
               PRELUDE "Promise.try(function (a, b) { return a + b; }, 1, 2)"
                       "  .then(function (v) { log += v; });",
               "3");
    expect_log(e,
               PRELUDE "Promise.try(function () { throw 'z'; }).catch(function (x) { log += x; });",
               "z");
    /* A promise made by the same constructor is returned as is; others are wrapped. */
    expect_log(e,
               PRELUDE "var sent = Promise.resolve();"
                       "log += (Promise.try(function () { return sent; }) === sent);",
               "true");
    expect_log(e,
               PRELUDE "(function () { class S extends Promise {}"
                       "  var p = Promise.resolve(1);"
                       "  var r = S.try(function () { return p; });"
                       "  log += (r === p); log += (r instanceof S); })();",
               "falsetrue");
    expect_log(e,
               PRELUDE "Promise.try(function () { return {then: function (f) { f('t'); }}; })"
                       "  .then(function (v) { log += v; });"
                       "Promise.try(function () { return 5; }).then(function (v) { log += v; });",
               "5t");
    expect_log(e,
               PRELUDE "var w = Promise.withResolvers();"
                       "w.promise.then(function (v) { log += v; }); w.resolve('w');",
               "w");
    expect_log(e, PRELUDE "log += Object.keys(Promise.withResolvers()).join(',');",
               "promise,resolve,reject");
    env_free(e);
}

/* ------------------------------------------------------------------ subclassing and species */

EMB_TEST(promise_subclass_instances_and_derived_promises) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "(function () { class S extends Promise {}"
                       "  var s = S.resolve(1);"
                       "  log += (s instanceof S);"
                       "  log += (S.resolve(s) === s);"
                       "  log += (s.then(function () {}) instanceof S);"
                       "  log += (S.reject(1).catch(function () {}) instanceof S); })();",
               "truetruetruetrue");
    expect_log(e,
               PRELUDE
               "(function () { class S extends Promise {}"
               "  S.try(function () { throw 1; }).catch(function () { log += 'c'; }); })();",
               "c");
    env_free(e);
}

EMB_TEST(promise_capability_constructor_must_be_a_constructor) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var C = function () { throw new RangeError('boom'); };"
                       "try { Promise.resolve.call(C, 1); } catch (x) { log += x.name; }",
               "RangeError");
    env_free(e);
}

/* ------------------------------------------------------------------ species thenables, elements */

EMB_TEST(promise_subclass_adopted_through_thenable_job) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "(function () { class S extends Promise {}"
                       "  var s = S.resolve('s');"
                       "  Promise.resolve(s).then(function (v) { log += v; }); })();",
               "s");
    env_free(e);
}

EMB_TEST(promise_all_element_function_runs_once) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    expect_log(e,
               PRELUDE "var fns = [];"
                       "function C(ex) { return new Promise(ex); }"
                       "C.resolve = function () { return {then: function (f) { fns.push(f); }}; };"
                       "Promise.all.call(C, [1, 2]).then(function (v) { log += v.join(''); });"
                       "fns[0]('a'); fns[0]('b'); log += '|';"
                       "Promise.resolve().then(function () { fns[1]('c'); });",
               "|ac");
    env_free(e);
}

/* ------------------------------------------------------------------ the C API (for J3b) */

static int c_reports;

static void count_reports(embi_js_runtime *rt, embi_js_value error, void *user) {
    (void)rt;
    (void)error;
    (void)user;
    c_reports++;
}

EMB_TEST(promise_c_api_then_without_capability) {
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    c_reports = 0;
    embi_js_set_host_report_error(e.rt, count_reports, nullptr);
    embi_js_value thrower = run(e, "(function () { throw new Error('x'); })");
    EMB_REQUIRE(embi_js_is_object(thrower));
    /* An await-style then: no capability, so a throwing handler reaches the host report. */
    embi_js_object *p = embi_js_promise_new(e.rt);
    EMB_REQUIRE(p != nullptr);
    EMB_CHECK(embi_js_promise_then(e.rt, p, thrower, EMBI_JS_UNDEFINED, nullptr) ==
              EMBI_JS_UNDEFINED);
    embi_js_promise_resolve_with(e.rt, p, embi_js_from_int32(1));
    embi_js_run_jobs(e.rt);
    EMB_CHECK_EQ_INT(c_reports, 1);

    /* reject_with and a capability from the intrinsic constructor. */
    embi_js_value ctor = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "Promise"));
    embi_js_pcap_t cap;
    EMB_REQUIRE(embi_js_promise_capability_new(e.rt, ctor, &cap));
    EMB_CHECK(embi_js_is_promise(cap.promise));
    embi_js_object *q = embi_js_promise_new(e.rt);
    embi_js_value derived =
        embi_js_promise_then(e.rt, q, EMBI_JS_UNDEFINED, EMBI_JS_UNDEFINED, &cap);
    EMB_CHECK(derived == cap.promise);
    embi_js_promise_reject_with(e.rt, q, embi_js_from_int32(7));
    embi_js_run_jobs(e.rt);
    EMB_CHECK(embi_js_promise_is_handled(embi_js_from_object(q)));
    embi_js_set_host_report_error(e.rt, nullptr, nullptr);
    embi_js_exception_clear(e.rt);
    env_free(e);
}

EMB_TEST_MAIN()
