/* J6b: J6a follow-ups. Each case failed before its fix and states the behavior the spec requires. */
#include "es6_util.h"

#include "js/heap.h"

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <stdlib.h>

/* The --gc-stress hang. Reflect.construct and Reflect.apply pass the elements of their argument
 * list cell on as argv: an interior pointer, which the conservative scan does not count. An
 * optimized build tail-called the target, so nothing held the list cell, and a collection in the
 * target (an Error constructor that defines its message) freed the arguments; reading the reused
 * cell as the message looped forever. The scripts run under a collection before every allocation,
 * on a thread with a time limit, so a regression fails instead of hanging. (A Debug build keeps the
 * list in its frame and passes either way; the Release ctest run on the box is the one that
 * catches it.) */
typedef struct stress_job {
    SDL_Semaphore *finished;
    uint32_t every;
    bool ok;
} stress_job;

static const char k_stress_src[] =
    "var ok = true;"
    "function N() {}"
    "var ctors = [Error, TypeError, RangeError, SyntaxError, ReferenceError, EvalError, URIError];"
    "for (var i = 0; i < ctors.length; i++) {"
    "  var e = Reflect.construct(ctors[i], ['m' + i], N);"
    "  ok = ok && e.message === 'm' + i && Object.getPrototypeOf(e) === N.prototype;"
    "  var f = Reflect.construct(ctors[i], ['x' + i, { cause: i }], function M() {});"
    "  ok = ok && f.message === 'x' + i && f.cause === i;"
    "}"
    "var t = Reflect.apply(String.prototype.concat, 'a', ['b' + 1, 'c' + 2, 'd' + 3]);"
    "ok = ok && t === 'ab1c2d3';"
    "var s = Reflect.apply(Array.prototype.join, ['p' + 1, 'q' + 2], ['-' + '']);"
    "ok = ok && s === 'p1-q2';"
    "var r = 'a1b2'.replace(/(\\d)/g, function (m, d, at, str) { return '<' + d + at + str.length + '>' });"
    "ok = ok && r === 'a<114>b<234>';"
    "var n = 'x1y'.replace(/(?<d>\\d)/, function (m, d, at, str, g) { return '[' + g.d + ']' });"
    "ok && n === 'x[1]y'";

static int stress_main(void *data)
{
    stress_job *j = data;
    es6_env e = es6_env_new();
    if (e.r != nullptr) {
        embi_js_gc_set_stress(e.rt, j->every);
        embi_js_value v = es6_run(e, k_stress_src);
        j->ok = v == EMBI_JS_TRUE;
        if (v == EMBI_JS_EXCEPTION) {
            es6_note_exc(e.rt);
            embi_js_exception_clear(e.rt);
        }
        embi_js_gc_set_stress(e.rt, 0);
    }
    es6_env_free(e);
    SDL_SignalSemaphore(j->finished);
    return 0;
}

EMB_TEST(j6b_gc_stress_interior_argv)
{
    const int64_t limit_ms = 20000 * (EMB_TEST_ASAN ? 4 : 1);
    for (uint32_t every = 1; every <= 3; every++) {
        stress_job j = {SDL_CreateSemaphore(0), every, false};
        EMB_REQUIRE(j.finished != nullptr);
        SDL_Thread *th = SDL_CreateThread(stress_main, "j6b_stress", &j);
        EMB_REQUIRE(th != nullptr);
        if (!SDL_WaitSemaphoreTimeout(j.finished, limit_ms)) {
            /* The thread is stuck in the engine and cannot be stopped: fail the whole test now. */
            SDL_Log("FAIL j6b_gc_stress_interior_argv: no result after %lld ms (gc stress %u)",
                    (long long)limit_ms, every);
            exit(1);
        }
        SDL_WaitThread(th, nullptr);
        SDL_DestroySemaphore(j.finished);
        if (!j.ok) {
            SDL_Log("    gc stress %u", every);
        }
        EMB_CHECK(j.ok);
    }
}

/* ECMA-262 13.15.2 and PutValue step 5.a: a strict assignment to a name that is unresolvable when
 * the reference is made throws a ReferenceError, even when code that the right-hand side runs
 * (a getter, valueOf, toString, Symbol.toPrimitive, a global accessor) creates the global. */
EMB_TEST(j6b_strict_unresolvable_rhs_code)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A getter reached through a member access. */
    E6_THROWS(e, "var o1 = { get p() { globalThis.u1 = 1; return 2 } };"
                 "(function () { 'use strict'; u1 = o1.p; })()",
              "ReferenceError");
    /* valueOf run by a converting operator on a local. */
    E6_THROWS(e, "(function () { 'use strict'; var v = { valueOf() { globalThis.u2 = 1; return 3 } };"
                 " u2 = v + 1; })()",
              "ReferenceError");
    /* An array literal converted by an operator runs Array.prototype.join. */
    E6_THROWS(e, "var j0 = Array.prototype.join; Array.prototype.join = function () {"
                 " globalThis.u3 = 1; return 'x' };"
                 "try { (function () { 'use strict'; u3 = [] + 1; })() }"
                 " finally { Array.prototype.join = j0 }",
              "ReferenceError");
    /* A computed key of an object literal converts its key. */
    E6_THROWS(e, "(function () { 'use strict'; var k = { toString() { globalThis.u4 = 1; return 'k' } };"
                 " u4 = { [k]: 1 }; })()",
              "ReferenceError");
    /* A global accessor read by name. */
    E6_THROWS(e, "Object.defineProperty(globalThis, 'acc5', { get() { globalThis.u5 = 1; return 5 },"
                 " configurable: true });"
                 "(function () { 'use strict'; u5 = acc5; })()",
              "ReferenceError");
    /* typeof of a global accessor reads it too. */
    E6_THROWS(e, "Object.defineProperty(globalThis, 'acc6', { get() { globalThis.u6 = 1; return 6 },"
                 " configurable: true });"
                 "(function () { 'use strict'; u6 = typeof acc6; })()",
              "ReferenceError");
    /* An indexed member access. */
    E6_THROWS(e, "var a7 = { get 0() { globalThis.u7 = 1; return 7 } };"
                 "(function () { 'use strict'; u7 = a7[0]; })()",
              "ReferenceError");
    /* Loose equality converts an object operand. */
    E6_THROWS(e, "(function () { 'use strict'; var w = { valueOf() { globalThis.u8 = 1; return 1 } };"
                 " u8 = w == 1; })()",
              "ReferenceError");
    E6_BOOL(e, "u1 === 1 && u2 === 1 && u3 === 1 && u4 === 1 && u5 === 1 && u6 === 1 &&"
               " u7 === 1 && u8 === 1",
            true);
    es6_env_free(e);
}

/* Right-hand sides that cannot run code keep storing to a global that exists at the store (the
 * plain path), and to one that exists when the reference is made (the early path). */
EMB_TEST(j6b_strict_assignment_stores)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "globalThis.g1 = 0; (function () { 'use strict'; g1 = 1 + 2 * 3; })(); g1", 7);
    E6_NUM(e, "globalThis.g2 = 0; (function () { 'use strict'; var l = 4; g2 = l === 4 ? 9 : 0; })(); g2",
           9);
    E6_TEXT(e, "globalThis.g3 = 0; (function () { 'use strict'; var l = 'b'; g3 = [l, 'c', { x: 1 }]; })();"
               " g3[0] + g3[1] + g3[2].x",
            "bc1");
    E6_NUM(e, "globalThis.g4 = 0; (function () { 'use strict'; var o = { p: 6 }; g4 = o.p; })(); g4", 6);
    E6_NUM(e, "globalThis.g5 = 0; (function () { 'use strict'; var v = { valueOf() { return 2 } };"
              " g5 = v + 3; })(); g5",
           5);
    E6_NUM(e, "var g6 = 1; (function () { 'use strict'; g6 = g6 + 1; })(); g6", 2);
    E6_THROWS(e, "(function () { 'use strict'; undecl_q = 1; })()", "ReferenceError");
    E6_THROWS(e, "(function () { 'use strict'; var l = 2; undecl_r = l + 1; })()", "ReferenceError");
    es6_env_free(e);
}

/* RegExp.prototype[Symbol.split] runs the splitter's exec, and each successful exec updates the
 * legacy static properties (RegExp.$1 ..., lastMatch, input, leftContext, rightContext), on the
 * fast path as on the generic one. A failed match leaves them alone. */
EMB_TEST(j6b_regexp_split_legacy_statics)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "/(z)/.exec('xzy'); 'a1b2c'.split(/(\\d)/);"
               "[RegExp.$1, RegExp.lastMatch, RegExp.input, RegExp['$`'], RegExp[\"$'\"],"
               " RegExp.lastParen].join('|')",
            "2|2|a1b2c|a1b|c|2");
    /* The generic path (the regexp is not pristine: an own exec) agrees. */
    E6_TEXT(e, "/(z)/.exec('xzy'); var r = /(\\d)/; r.exec = RegExp.prototype.exec;"
               " 'a1b2c'.split(r);"
               "[RegExp.$1, RegExp.lastMatch, RegExp.input, RegExp['$`'], RegExp[\"$'\"]].join('|')",
            "2|2|a1b2c|a1b|c");
    /* A limit stops the matching early: the statics follow the last match that ran. */
    E6_TEXT(e, "'a1b2c3d'.split(/(\\d)/, 2); [RegExp.$1, RegExp.input].join('|')", "1|a1b2c3d");
    /* No match: the statics stay. */
    E6_TEXT(e, "/(q)/.exec('aqb'); 'abc'.split(/(\\d)/); [RegExp.$1, RegExp.input].join('|')", "q|aqb");
    /* The empty string: a match updates them. */
    E6_TEXT(e, "/(q)/.exec('aqb'); ''.split(/(?:)/); RegExp.input + '|' + RegExp.lastMatch + '|' + RegExp.$1",
            "||");
    /* A match that does not advance (an empty match at the start) is an exec too. */
    E6_TEXT(e, "'ab'.split(/(x)?/); [RegExp.input, RegExp.lastMatch, RegExp.leftContext].join('|')",
            "ab||a");
    es6_env_free(e);
}

/* TypedArray [[PreventExtensions]] (ECMA-262 10.4.5.x): a TypedArray that is not fixed-length
 * (length-tracking, or on a resizable buffer) refuses, so Object.preventExtensions, Object.seal and
 * Object.freeze throw a TypeError and Reflect.preventExtensions returns false. A fixed-length one
 * (also on a growable shared buffer) becomes non-extensible. */
EMB_TEST(j6b_typedarray_integrity)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "Object.preventExtensions(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 })))",
              "TypeError");
    E6_THROWS(e, "Object.seal(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 })))", "TypeError");
    E6_THROWS(e, "Object.freeze(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 })))", "TypeError");
    /* Fixed length on a resizable buffer is still not fixed-length (IsTypedArrayFixedLength). */
    E6_THROWS(e, "Object.seal(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }), 0, 2))", "TypeError");
    E6_THROWS(e, "Object.preventExtensions(new Float64Array(new ArrayBuffer(16, { maxByteLength: 32 }), 8))",
              "TypeError");
    E6_BOOL(e, "var ta = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }));"
               " Reflect.preventExtensions(ta) === false && Object.isExtensible(ta)",
            true);
    /* Fixed-length views on fixed buffers are unchanged. */
    E6_BOOL(e, "var f = new Uint8Array(4); Object.preventExtensions(f) === f && !Object.isExtensible(f)", true);
    E6_BOOL(e, "var s = Object.seal(new Uint8Array(0)); Object.isSealed(s) && !Object.isExtensible(s)", true);
    /* Elements of a TypedArray stay configurable, so sealing a non-empty one throws (after it
     * became non-extensible). */
    E6_BOOL(e, "var s2 = new Uint8Array(2); try { Object.seal(s2) } catch (x) { }"
               " !Object.isExtensible(s2)",
            true);
    E6_BOOL(e, "var z = Object.freeze(new Uint8Array(0)); Object.isFrozen(z)", true);
    E6_THROWS(e, "Object.freeze(new Uint8Array(2))", "TypeError");
    E6_BOOL(e, "Reflect.preventExtensions(new Int16Array(new ArrayBuffer(4), 2))", true);
    es6_env_free(e);
}

EMB_TEST_MAIN()
