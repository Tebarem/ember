#include "emb_test.h"

#include "js/js.h"
#include "js/ops.h"

#include <stdint.h>
#include <string.h>

/* The ES2025 Set methods (union, intersection, difference, symmetricDifference, isSubsetOf,
 * isSupersetOf, isDisjointFrom) with Set and set-like arguments, their GetSetRecord checks, and
 * Map.groupBy and Object.groupBy. ES5 syntax only; iterables are built by hand. */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "setops.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "setops.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected %s: %s", want, src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "name"));
        if (embi_js_is_string(n)) {
            const embi_js_string *ns = embi_js_as_string(n);
            ok = embi_js_str_len(ns) == strlen(want);
            for (size_t i = 0; ok && i < strlen(want); i++) {
                ok = embi_js_str_at(ns, (uint32_t)i) == (uint16_t)want[i];
            }
        }
    }
    if (!ok) {
        SDL_Log("    wrong exception (wanted %s): %s", want, src);
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

/* Shared script prelude: S(array) builds a Set, like(size, vals) builds a set-like object whose
 * has and keys read vals, and mk(vals) builds an iterable that counts return() calls in `closed`. */
#define PRELUDE                                                                                \
    "function S(a) { var s = new Set(); a.forEach(function (x) { s.add(x); }); return s; } "    \
    "function like(size, vals) { return { size: size, has: function (x) { return vals.indexOf(x) >= 0; }," \
    " keys: function () { var i = 0; return { next: function () { if (i < vals.length) {"      \
    " return { value: vals[i++], done: false }; } return { value: undefined, done: true }; } }; } }; } " \
    "var closed = 0; function mk(vals) { var o = {}; o[Symbol.iterator] = function () {"          \
    " var i = 0; return { next: function () { if (i < vals.length) { return { value: vals[i++], done: false }; }" \
    " return { value: undefined, done: true }; }, 'return': function () { closed++; return {}; } }; };" \
    " return o; } "

/* Returns the sorted members of a set as a string, for compact comparisons. */
#define MEMBERS "function members(s) { var out = []; s.forEach(function (v) { out.push(v); }); out.sort(); return out.join(','); } "

EMB_TEST(set_union_intersection_difference)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2]).union(S([2, 3]))) === '1,2,3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2, 3]).intersection(S([2, 3, 4]))) === '2,3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2, 3]).difference(S([2]))) === '1,3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2]).symmetricDifference(S([2, 3]))) === '1,3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "var a = S([1]); var u = a.union(S([2])); u !== a && a.size === 1 && u.size === 2 && u instanceof Set"));
    env_free(e);
}

EMB_TEST(set_predicates)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE "S([1]).isSubsetOf(S([1, 2])) === true && S([1, 3]).isSubsetOf(S([1, 2])) === false"));
    EMB_CHECK(js_true(e, PRELUDE "S([1, 2]).isSupersetOf(S([1])) === true && S([1]).isSupersetOf(S([1, 2])) === false"));
    EMB_CHECK(js_true(e, PRELUDE "S([1]).isDisjointFrom(S([2])) === true && S([1, 2]).isDisjointFrom(S([2])) === false"));
    EMB_CHECK(js_true(e, PRELUDE "S([]).isSubsetOf(S([])) && S([]).isDisjointFrom(S([]))"));
    env_free(e);
}

/* Set-like arguments: only size, has and keys are read. */
EMB_TEST(set_methods_accept_set_like_arguments)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2, 3]).union(like(2, [5, 1]))) === '1,2,3,5'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1]).intersection(like(10, [1, 9]))) === '1'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2, 3]).intersection(like(2, [3, 4]))) === '3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2, 3]).difference(like(10, [2, 7]))) === '1,3'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2]).difference(like(1, [2]))) === '1'"));
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([1, 2]).symmetricDifference(like(2, [2, 4]))) === '1,4'"));
    EMB_CHECK(js_true(e, PRELUDE "S([1, 2, 3]).isSubsetOf(like(2, [1, 2])) === false && S([1]).isSubsetOf(like(5, [1])) === true"));
    EMB_CHECK(js_true(e, PRELUDE "S([1, 2]).isSupersetOf(like(1, [1])) === true && S([1]).isSupersetOf(like(2, [1, 5])) === false"));
    EMB_CHECK(js_true(e, PRELUDE "S([1]).isDisjointFrom(like(1, [2])) === true && S([1]).isDisjointFrom(like(5, [1])) === false"));
    env_free(e);
}

/* The keys iterator is closed when isSupersetOf stops early, and not when it finishes. */
EMB_TEST(set_predicates_close_keys_iterator)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE
                         "var other = { size: 1, has: function () { return true; }, keys: function () { var n = 0;"
                         " return { next: function () { n++; return { value: n === 1 ? 1 : 99, done: false }; },"
                         " 'return': function () { closed++; return {}; } }; } };"
                         "S([1]).isSupersetOf(other) === false && closed === 1"));
    EMB_CHECK(js_throws(e, PRELUDE "S([1, 2]).isSupersetOf(mk([1, 2]))", "TypeError"));
    env_free(e);
}

/* The result of a method that stops early does not depend on a later has() call. */
EMB_TEST(set_union_intersection_follow_the_protocol)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* union takes the keys of other; the keys iterator is consumed fully. */
    EMB_CHECK(js_true(e, PRELUDE MEMBERS "members(S([]).union(like(2, [4, 5]))) === '4,5'"));
    /* isSubsetOf calls has on each member of this set and stops at the first miss. */
    EMB_CHECK(js_true(e, PRELUDE
                         "var calls = 0; var o = { size: 10, has: function () { calls++; return false; }, keys: function () { return null; } };"
                         "S([1, 2, 3]).isSubsetOf(o) === false && calls === 1"));
    /* A set method on a receiver that is not a Set throws. */
    EMB_CHECK(js_throws(e, PRELUDE "Set.prototype.union.call(new Map(), S([1]))", "TypeError"));
    env_free(e);
}

EMB_TEST(set_method_argument_validation)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union(5)", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: NaN, has: function () {}, keys: function () {} })", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: -1, has: function () {}, keys: function () {} })", "RangeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: 1, has: 5, keys: function () {} })", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: 1, has: function () {}, keys: 5 })", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: 1, has: function () {}, keys: function () { return 5; } })", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ size: 1, has: function () {}, keys: function () { return { next: 5 }; } })", "TypeError"));
    /* size undefined is NaN, so it is a TypeError; a fractional size truncates. */
    EMB_CHECK(js_throws(e, PRELUDE "S([]).union({ has: function () {}, keys: function () {} })", "TypeError"));
    EMB_CHECK(js_true(e, PRELUDE "S([1]).isSubsetOf({ size: 1.9, has: function () { return true; }, keys: function () {} })"));
    env_free(e);
}

/* ------------------------------------------------------------------ groupBy */

EMB_TEST(map_group_by)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE
                         "var g = Map.groupBy(mk([1, 2, 3, 4]), function (x) { return x % 2 ? 'odd' : 'even'; });"
                         "g instanceof Map && g.get('odd').length === 2 && g.get('odd')[1] === 3 && g.get('even')[1] === 4"));
    EMB_CHECK(js_true(e, PRELUDE
                         "var z = Map.groupBy(mk([1, 2]), function (x) { return x === 1 ? -0 : 'z'; });"
                         "z.get(0)[0] === 1 && z.has(0)"));
    EMB_CHECK(js_true(e, PRELUDE
                         "var idx = Map.groupBy(mk(['a', 'b']), function (v, i) { return i; }); idx.get(1)[0] === 'b'"));
    EMB_CHECK(js_true(e, PRELUDE
                         "var objKey = {}; var ob = Map.groupBy(mk([1, 2]), function (x) { return x === 1 ? objKey : 'o'; }); ob.get(objKey)[0] === 1"));
    EMB_CHECK(js_true(e, PRELUDE "Map.groupBy(mk([]), function () { return 1; }).size === 0"));
    EMB_CHECK(js_throws(e, PRELUDE "Map.groupBy(null, function () {})", "TypeError"));
    EMB_CHECK(js_throws(e, PRELUDE "Map.groupBy(mk([]), 5)", "TypeError"));
    EMB_CHECK(js_true(e, PRELUDE
                         "closed = 0; var threw = false; try { Map.groupBy(mk([1, 2]), function () { throw new RangeError('x'); }); }"
                         "catch (x) { threw = x instanceof RangeError; } threw && closed === 1"));
    env_free(e);
}

EMB_TEST(object_group_by)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, PRELUDE
                         "var o = Object.groupBy(mk([1, 2, 3]), function (x) { return x > 1 ? 'big' : 'small'; });"
                         "Object.getPrototypeOf(o) === null && o.big.length === 2 && o.small[0] === 1"));
    EMB_CHECK(js_true(e, PRELUDE
                         "var n = Object.groupBy(mk([1, 2, 3]), function (x) { return x % 2; }); n[1].length === 2 && n[0][0] === 2"));
    EMB_CHECK(js_true(e, PRELUDE
                         "var c = Object.groupBy(mk([1, 2]), function (x) { return { toString: function () { return 'key' + x; } }; });"
                         "c.key1[0] === 1 && c.key2[0] === 2"));
    EMB_CHECK(js_throws(e, PRELUDE "Object.groupBy(undefined, function () {})", "TypeError"));
    EMB_CHECK(js_true(e, "Object.groupBy.length === 2 && Map.groupBy.length === 2"));
    env_free(e);
}

EMB_TEST_MAIN()
