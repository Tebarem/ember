#include "emb_test.h"

#include "js/js.h"
#include "js/ops.h"

#include <stdint.h>
#include <string.h>

/* Map, Set, WeakMap and WeakSet: constructors, the prototype methods, the iterators, the
 * iteration-under-mutation rules, and the receiver checks. Scripts use ES5 syntax only (for-of and
 * the ES2015 syntax arrive with J2a and J2c), so iterables are built by hand. */

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

/* Evaluates src. True when it yields the boolean true; logs the script otherwise. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "collections.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_value ex = embi_js_exception_get(e.rt);
        char buf[256];
        buf[0] = 0;
        if (embi_js_is_object(ex)) {
            embi_js_value m = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "message"));
            if (embi_js_is_string(m)) {
                const embi_js_string *ms = embi_js_as_string(m);
                uint32_t n = embi_js_str_len(ms) < sizeof buf - 1 ? embi_js_str_len(ms) : (uint32_t)(sizeof buf - 1);
                for (uint32_t i = 0; i < n; i++) {
                    uint16_t c = embi_js_str_at(ms, i);
                    buf[i] = c < 128 ? (char)c : '?';
                }
                buf[n] = 0;
            }
        }
        SDL_Log("    js threw (%s): %s", buf, src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

/* Evaluates src, which must throw an exception whose name is want. */
static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "collections.js", 0);
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

/* ------------------------------------------------------------------ Map */

EMB_TEST(map_basic_operations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); m.size === 0"));
    EMB_CHECK(js_true(e, "m.set('a', 1) === m && m.get('a') === 1 && m.has('a')"));
    EMB_CHECK(js_true(e, "m.get('missing') === undefined && !m.has('b')"));
    EMB_CHECK(js_true(e, "m.size === 1 && m.delete('a') === true && m.delete('a') === false"));
    EMB_CHECK(js_true(e, "m.size === 0"));
    EMB_CHECK(js_true(e, "m.set('k', 1); m.set('k', 2); m.size === 1 && m.get('k') === 2"));
    EMB_CHECK(js_true(e, "m.clear(); m.size === 0 && !m.has('k')"));
    env_free(e);
}

EMB_TEST(map_key_equality_is_same_value_zero)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); m.set(NaN, 'nan'); m.get(NaN) === 'nan' && m.has(0 / 0)"));
    EMB_CHECK(js_true(e, "m.set(0, 'zero'); m.get(-0) === 'zero' && m.size === 2"));
    EMB_CHECK(js_true(e, "m.set(-0, 'neg'); m.size === 2 && m.get(0) === 'neg'"));
    EMB_CHECK(js_true(e, "var z = new Map(); z.set(-0, 1); 1 / z.keys().next().value === Infinity"));
    EMB_CHECK(js_true(e, "var s = new Map(); s.set('x' + 'y', 5); s.get('xy') === 5"));
    EMB_CHECK(js_true(e, "var o = {}; var b = new Map(); b.set(o, 1); b.get({}) === undefined && b.get(o) === 1"));
    EMB_CHECK(js_true(e, "var n = new Map(); n.set(1, 'num'); n.get('1') === undefined && n.get(1) === 'num'"));
    EMB_CHECK(js_true(e, "var f = new Map(); f.set(2.5, 'f'); f.get(5 / 2) === 'f' && f.get(2.5000000001) === undefined"));
    EMB_CHECK(js_true(e, "var big = new Map(); big.set(4294967296, 'big'); big.get(4294967296) === 'big'"));
    EMB_CHECK(js_true(e, "var bo = new Map(); bo.set(true, 'T'); bo.set(null, 'N'); bo.set(undefined, 'U'); bo.get(true) === 'T' && bo.get(null) === 'N' && bo.get(undefined) === 'U' && bo.get(false) === undefined"));
    env_free(e);
}

EMB_TEST(map_foreach_order_and_arguments)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); m.set('a', 1); m.set('b', 2); m.set('c', 3); m.delete('b'); m.set('b', 4); var seen = ''; m.forEach(function (v, k) { seen += k + v; }); seen === 'a1c3b4'"));
    EMB_CHECK(js_true(e, "var same = false; m.forEach(function (v, k, mm) { same = mm === m; }); same"));
    EMB_CHECK(js_true(e, "var t = {}; var okThis = false; m.forEach(function () { okThis = this === t; }, t); okThis"));
    EMB_CHECK(js_true(e, "var grow = new Map(); grow.set(1, 1); var n = 0; grow.forEach(function (v, k) { n++; if (k < 4) { grow.set(k + 1, k + 1); } }); n === 4"));
    EMB_CHECK(js_true(e, "var del = new Map(); del.set(1, 1); del.set(2, 2); del.set(3, 3); var vis = ''; del.forEach(function (v, k) { vis += k; if (k === 1) { del.delete(2); } }); vis === '13'"));
    EMB_CHECK(js_true(e, "var cl = new Map(); cl.set(1, 1); cl.set(2, 2); var v4 = ''; cl.forEach(function (v, k) { v4 += k; if (k === 1) { cl.clear(); cl.set(9, 9); } }); v4 === '19'"));
    EMB_CHECK(js_throws(e, "new Map().forEach(5)", "TypeError"));
    env_free(e);
}

EMB_TEST(map_iterators_follow_the_protocol)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); m.set('x', 10); m.set('y', 20); true"));
    EMB_CHECK(js_true(e, "var it = m.keys(); var r1 = it.next(); r1.value === 'x' && r1.done === false"));
    EMB_CHECK(js_true(e, "var r2 = it.next(); r2.value === 'y' && r2.done === false"));
    EMB_CHECK(js_true(e, "var r3 = it.next(); r3.done === true && r3.value === undefined"));
    EMB_CHECK(js_true(e, "m.set('z', 30); it.next().done === true"));
    EMB_CHECK(js_true(e, "var vi = m.values(); vi.next().value === 10"));
    EMB_CHECK(js_true(e, "var ei = m.entries(); var pair = ei.next().value; pair[0] === 'x' && pair[1] === 10"));
    EMB_CHECK(js_true(e, "Map.prototype[Symbol.iterator] === Map.prototype.entries"));
    EMB_CHECK(js_true(e, "var NextFn = Object.getPrototypeOf(m.keys()).next; var threw = false; try { NextFn.call({}); } catch (x) { threw = x instanceof TypeError; } threw"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(m.keys()) === '[object Map Iterator]'"));
    env_free(e);
}

/* A cursor keeps its place across compactions: the entries it has not reached stay in order. */
EMB_TEST(map_iterator_survives_compaction)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); for (var i = 0; i < 100; i++) { m.set(i, i); }"
                         "var it = m.keys(); for (var j = 0; j < 10; j++) { it.next(); }"
                         "for (var k = 0; k < 90; k++) { m.delete(k); }"
                         "for (var q = 100; q < 400; q++) { m.set(q, q); }"
                         "var rest = 0; var first = -1; var r = it.next();"
                         "while (!r.done) { if (first < 0) { first = r.value; } rest++; r = it.next(); }"
                         "rest === 310 && first === 90"));
    env_free(e);
}

EMB_TEST(map_iterator_after_clear_sees_new_entries)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var m = new Map(); m.set(1, 1); m.set(2, 2); var it = m.keys(); it.next(); m.clear(); m.set(7, 7);"
                         "var r = it.next(); r.value === 7 && it.next().done === true"));
    env_free(e);
}

EMB_TEST(map_removing_and_readding_during_iteration)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Deleting the current key and adding it again appends it, so the iteration visits it once more. */
    EMB_CHECK(js_true(e, "var m = new Map(); m.set('a', 1); m.set('b', 2); var seen = []; m.forEach(function (v, k) { seen.push(k); if (k === 'a' && seen.length < 4) { m.delete('a'); m.set('a', 9); } });"
                         "seen.length === 4 && seen[0] === 'a' && seen[1] === 'b' && seen[2] === 'a' && seen[3] === 'a'"));
    env_free(e);
}

/* ------------------------------------------------------------------ constructors */

EMB_TEST(collections_require_new)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_throws(e, "Map()", "TypeError"));
    EMB_CHECK(js_throws(e, "Set()", "TypeError"));
    EMB_CHECK(js_throws(e, "WeakMap()", "TypeError"));
    EMB_CHECK(js_throws(e, "WeakSet()", "TypeError"));
    env_free(e);
}

/* Builds an iterable over vals whose iterator counts return() calls in the global `closed`. */
#define ITERABLE_HELPER                                                                        \
    "var closed = 0; function mk(vals) { var o = {}; o[Symbol.iterator] = function () {"      \
    " var i = 0; return { next: function () { if (i < vals.length) { return { value: vals[i++], done: false }; }" \
    " return { value: undefined, done: true }; }, 'return': function () { closed++; return {}; } }; };" \
    " return o; } "

EMB_TEST(map_and_set_constructor_from_iterables)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, ITERABLE_HELPER "var m = new Map(mk([['k1', 1], ['k2', 2]])); m.get('k2') === 2 && m.size === 2"));
    EMB_CHECK(js_true(e, ITERABLE_HELPER "var s = new Set(mk([10, 20, 20, 30])); s.size === 3 && s.has(20)"));
    EMB_CHECK(js_true(e, ITERABLE_HELPER "var n = new Set(null); n.size === 0 && new Map(undefined).size === 0"));
    EMB_CHECK(js_true(e, ITERABLE_HELPER "var ws = new WeakSet(mk([{}, {}])); ws.size === undefined"));
    EMB_CHECK(js_true(e, ITERABLE_HELPER "var wm = new WeakMap(mk([[{}, 'v']])); wm instanceof WeakMap"));
    env_free(e);
}

/* A non-entry value closes the iterator and the TypeError propagates. */
EMB_TEST(map_constructor_closes_iterator_on_bad_entry)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, ITERABLE_HELPER
                         "var threw = false; try { new Map(mk([1, 2])); } catch (x) { threw = x instanceof TypeError; }"
                         "threw && closed === 1"));
    EMB_CHECK(js_true(e, ITERABLE_HELPER
                         "closed = 0; var t2 = false; try { new Set(mk([1])); new Map(mk([[1, 2]])); } catch (x) { t2 = true; }"
                         "!t2 && closed === 0"));
    env_free(e);
}

EMB_TEST(collections_species_and_tags)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Map[Symbol.species] === Map && Set[Symbol.species] === Set"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new Map()) === '[object Map]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new Set()) === '[object Set]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new WeakMap()) === '[object WeakMap]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new WeakSet()) === '[object WeakSet]'"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(new Map()) === Map.prototype && Map.prototype.constructor === Map"));
    EMB_CHECK(js_true(e, "Map.length === 0 && Map.prototype.set.length === 2 && Set.prototype.add.length === 1"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').get.name === 'get size'"));
    env_free(e);
}

EMB_TEST(collections_incompatible_receivers)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_throws(e, "Map.prototype.get.call(new Set(), 1)", "TypeError"));
    EMB_CHECK(js_throws(e, "Set.prototype.add.call({}, 1)", "TypeError"));
    EMB_CHECK(js_throws(e, "Object.getOwnPropertyDescriptor(Map.prototype, 'size').get.call({})", "TypeError"));
    EMB_CHECK(js_throws(e, "Object.getOwnPropertyDescriptor(Set.prototype, 'size').get.call(new Map())", "TypeError"));
    EMB_CHECK(js_throws(e, "WeakMap.prototype.set.call(new Map(), {}, 1)", "TypeError"));
    env_free(e);
}

/* ------------------------------------------------------------------ Set */

EMB_TEST(set_basic_operations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var s = new Set(); s.add(1) === s && s.has(1) && s.size === 1"));
    EMB_CHECK(js_true(e, "s.add(1); s.size === 1 && s.delete(1) === true && s.delete(1) === false && s.size === 0"));
    EMB_CHECK(js_true(e, "s.add(NaN); s.add(0 / 0); s.size === 1 && s.has(NaN)"));
    EMB_CHECK(js_true(e, "s.add(-0); s.add(0); s.size === 2 && s.has(-0) && s.has(0)"));
    EMB_CHECK(js_true(e, "var z = new Set(); z.add(-0); 1 / z.values().next().value === Infinity"));
    EMB_CHECK(js_true(e, "Set.prototype.keys === Set.prototype.values && Set.prototype[Symbol.iterator] === Set.prototype.values"));
    EMB_CHECK(js_true(e, "var es = new Set(); es.add(5); var ent = es.entries().next().value; ent[0] === 5 && ent[1] === 5"));
    EMB_CHECK(js_true(e, "var args = null; var one = new Set(); one.add(7); one.forEach(function (v, k, set) { args = [v, k, set]; }); args[0] === 7 && args[1] === 7 && args[2] === one"));
    EMB_CHECK(js_true(e, "Set.prototype.add.call(new Set(), 1) instanceof Set"));
    env_free(e);
}

/* ------------------------------------------------------------------ WeakMap and WeakSet */

EMB_TEST(weakmap_basic_operations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var wm = new WeakMap(); var k = {}; wm.set(k, 'v') === wm && wm.get(k) === 'v' && wm.has(k)"));
    EMB_CHECK(js_true(e, "wm.delete(k) === true && wm.has(k) === false && wm.get(k) === undefined && wm.delete(k) === false"));
    EMB_CHECK(js_true(e, "wm.get(1) === undefined && wm.has('x') === false && wm.delete(1) === false"));
    EMB_CHECK(js_true(e, "var sym = Symbol('ok'); wm.set(sym, 3); wm.get(sym) === 3"));
    EMB_CHECK(js_throws(e, "wm.set(1, 2)", "TypeError"));
    EMB_CHECK(js_throws(e, "wm.set(Symbol.for('registered'), 1)", "TypeError"));
    EMB_CHECK(js_true(e, "wm.get(Symbol.for('registered')) === undefined && wm.has(Symbol.for('r')) === false"));
    env_free(e);
}

EMB_TEST(weakset_basic_operations)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var ws = new WeakSet(); var k = {}; ws.add(k) === ws && ws.has(k) && ws.has({}) === false"));
    EMB_CHECK(js_true(e, "ws.delete(k) === true && ws.has(k) === false && ws.delete(k) === false"));
    EMB_CHECK(js_true(e, "ws.has(1) === false && ws.delete('x') === false"));
    EMB_CHECK(js_throws(e, "ws.add(1)", "TypeError"));
    EMB_CHECK(js_throws(e, "ws.add(Symbol.for('reg'))", "TypeError"));
    EMB_CHECK(js_true(e, "var s2 = Symbol('fine'); ws.add(s2); ws.has(s2)"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(ws) === '[object WeakSet]'"));
    env_free(e);
}

/* A weak collection's key may be reached only through its own value; the entry still works while
 * the key is held. Cycles through the table do not keep anything alive in the script itself. */
EMB_TEST(weakmap_values_reach_keys_and_chains)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var wm = new WeakMap(); var a = {}; var b = {}; wm.set(a, b); wm.set(b, 'end');"
                         "wm.get(wm.get(a)) === 'end' && wm.get(b) === 'end'"));
    EMB_CHECK(js_true(e, "var self = {}; wm.set(self, self); wm.get(self) === self"));
    env_free(e);
}

EMB_TEST(collections_big_table_grows_and_shrinks)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "var big = new Map(); for (var i = 0; i < 5000; i++) { big.set('k' + i, i); }"
                         "for (var j = 0; j < 5000; j += 2) { big.delete('k' + j); }"
                         "for (var n = 0; n < 5000; n++) { big.set('n' + n, n); }"
                         "big.size === 7500 && big.get('k4999') === 4999 && big.get('k0') === undefined && big.get('n42') === 42"));
    EMB_CHECK(js_true(e, "var bs = new Set(); for (var s = 0; s < 3000; s++) { bs.add(s); bs.delete(s - 1); } bs.size === 1 && bs.has(2999)"));
    env_free(e);
}

EMB_TEST_MAIN()
