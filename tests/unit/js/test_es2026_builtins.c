#include "emb_test.h"

#include "js/js.h"
#include "js/object.h"

#include <string.h>

/* The small ES2026 builtins of J4f: Error.isError, Math.sumPrecise,
 * Iterator.concat, Map and WeakMap getOrInsert and getOrInsertComputed, and the Date tag that
 * Object.prototype.toString reads. */

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

/* Runs a script that evaluates to true. Logs and clears any exception. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "es2026.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(error_is_error)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "typeof Error.isError === 'function' && Error.isError.length === 1"));
    EMB_CHECK(js_true(e, "Error.isError(new Error('x')) === true && "
                         "Error.isError(new TypeError()) === true && "
                         "Error.isError(new AggregateError([])) === true"));
    EMB_CHECK(js_true(e, "Error.isError({}) === false && Error.isError(Error.prototype) === false"));
    EMB_CHECK(js_true(e, "Error.isError({__proto__: Error.prototype}) === false"));
    EMB_CHECK(js_true(e, "Error.isError() === false && Error.isError('Error') === false"));
    EMB_CHECK(js_true(e, "class MyErr extends Error {}; Error.isError(new MyErr()) === true"));
    env_free(e);
}

EMB_TEST(math_sum_precise_basic)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Math.sumPrecise.length === 1 && Math.sumPrecise.name === 'sumPrecise'"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1, 2, 3]) === 6"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([]) === 0 && 1 / Math.sumPrecise([]) === -Infinity"));
    EMB_CHECK(js_true(e, "1 / Math.sumPrecise([-0]) === -Infinity && "
                         "1 / Math.sumPrecise([-0, -0]) === -Infinity"));
    EMB_CHECK(js_true(e, "1 / Math.sumPrecise([-0, 0]) === Infinity"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1e308, -1e308]) === 0"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1e308]) === 1e308"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1e308, 1e308, 0.1, 0.1, 1e30, 0.1, -1e30, -1e308, -1e308])"
                         " === 0.30000000000000004"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1e30, 0.1, -1e30]) === 0.1"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([1.3588124894186193e+308, 1.4803986201152006e+223, "
                         "6.741349255733684e+307]) === Infinity"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([-1.9807040628566093e+28, 1.7976931348623157e+308, "
                         "9.9792015476736e+291]) === 1.7976931348623157e+308"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([Infinity, -Infinity]) !== Math.sumPrecise([Infinity, -Infinity]) "
                         "&& Math.sumPrecise([Infinity]) === Infinity && "
                         "Math.sumPrecise([-Infinity, -Infinity]) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.sumPrecise([NaN, 1]) !== Math.sumPrecise([NaN, 1])"));
    EMB_CHECK(js_true(e, "Math.sumPrecise(new Set([1, 2])) === 3"));
    env_free(e);
}

/* Items must be numbers: a bigint, a string or an object throws a TypeError, and the iterator is
 * closed. */
EMB_TEST(math_sum_precise_rejects_non_numbers_and_closes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "(function () { try { Math.sumPrecise([0n]); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { Math.sumPrecise(['1']); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { var closed = 0; var it = { i: 0, "
                         "[Symbol.iterator]() { return this; }, next() { return { done: false, "
                         "value: 'no' }; }, return() { closed++; return {}; } }; "
                         "try { Math.sumPrecise(it); } catch (x) { } return closed === 1; })()"));
    EMB_CHECK(js_true(e, "(function () { try { Math.sumPrecise(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
}

EMB_TEST(iterator_concat_sequences_iterables)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Iterator.concat.length === 0 && Iterator.concat.name === 'concat'"));
    EMB_CHECK(js_true(e, "[...Iterator.concat([1, 2], [], new Set([3]))].join() === '1,2,3'"));
    EMB_CHECK(js_true(e, "[...Iterator.concat()].length === 0"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(Iterator.concat([])) === "
                         "Object.getPrototypeOf(Iterator.prototype.map.call([], x => x))"));
    EMB_CHECK(js_true(e, "(function () { var g = Iterator.concat((function* () { yield 1; "
                         "yield 2; })()); return g.next().value === 1 && g.next().value === 2 && "
                         "g.next().done === true; })()"));
    EMB_CHECK(js_true(e, "(function () { try { Iterator.concat(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { Iterator.concat('ab'); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { try { Iterator.concat({}); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { var c = 0; var it = { [Symbol.iterator]() { c++; return "
                         "[1][Symbol.iterator](); } }; var g = Iterator.concat(it); "
                         "return c === 0 && g.next().value === 1 && c === 1; })()"));
    /* return before the start never calls the underlying return. */
    EMB_CHECK(js_true(e, "(function () { var rc = 0; var it = { [Symbol.iterator]() { return { "
                         "next() { return { done: false, value: 1 }; }, return() { rc++; "
                         "return {}; } }; } }; var g = Iterator.concat(it); g.return(); "
                         "return rc === 0 && g.next().done === true; })()"));
    /* return while the inner iterator is open closes it. */
    EMB_CHECK(js_true(e, "(function () { var rc = 0; var it = { [Symbol.iterator]() { return { "
                         "next() { return { done: false, value: 1 }; }, return() { rc++; "
                         "return {}; } }; } }; var g = Iterator.concat(it); g.next(); g.return(); "
                         "return rc === 1; })()"));
    env_free(e);
}

EMB_TEST(map_get_or_insert)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Map.prototype.getOrInsert.length === 2 && "
                         "Map.prototype.getOrInsertComputed.length === 2"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); return m.getOrInsert('k', 1) === 1 && "
                         "m.getOrInsert('k', 2) === 1 && m.get('k') === 1; })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); m.getOrInsert(-0, 'z'); "
                         "return m.get(0) === 'z' && 1 / [...m.keys()][0] === Infinity; })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); var calls = 0; "
                         "var f = function (k) { calls++; return k; }; "
                         "m.getOrInsertComputed(-0, f); m.getOrInsertComputed(0, f); "
                         "return calls === 1 && m.get(0) === 0; })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); var seen; "
                         "m.getOrInsertComputed(-0, function (k) { seen = 1 / k; return 1; }); "
                         "return seen === Infinity; })()"));
    /* A callback that adds the key itself: its entry is overwritten by the callback's result. */
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); "
                         "m.getOrInsertComputed('a', function () { m.set('a', 'mut'); return 'r'; }); "
                         "return m.get('a') === 'r' && m.size === 1; })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); try { m.getOrInsertComputed('a', 1); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map([['a', 1]]); try { "
                         "m.getOrInsertComputed('a', 1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { var m = new Map(); try { m.getOrInsertComputed('x', "
                         "function () { throw new RangeError('no'); }); return false; } catch (x) { "
                         "return x instanceof RangeError && !m.has('x'); } })()"));
    EMB_CHECK(js_true(e, "(function () { try { Map.prototype.getOrInsert.call({}, 1, 2); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    env_free(e);
}

EMB_TEST(weakmap_get_or_insert)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); var k = {}; "
                         "return w.getOrInsert(k, 5) === 5 && w.getOrInsert(k, 6) === 5 && "
                         "w.get(k) === 5; })()"));
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); var k = Symbol('s'); "
                         "return w.getOrInsertComputed(k, function (x) { return x === k; }) === true; })()"));
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); var calls = 0; var k = {}; "
                         "w.getOrInsertComputed(k, function () { calls++; return 1; }); "
                         "w.getOrInsertComputed(k, function () { calls++; return 2; }); "
                         "return calls === 1 && w.get(k) === 1; })()"));
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); try { w.getOrInsert(1, 2); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); try { w.getOrInsert(Symbol.for('r'), 2); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    /* The callback is checked before the key: a non-callable with a bad key is still a TypeError,
     * and the callback never runs for an invalid key. */
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); var ran = 0; try { "
                         "w.getOrInsertComputed(1, function () { ran++; }); return false; } catch (x) { "
                         "return x instanceof TypeError && ran === 0; } })()"));
    EMB_CHECK(js_true(e, "(function () { var w = new WeakMap(); var k = {}; "
                         "w.getOrInsertComputed(k, function () { w.set(k, 'mutated'); return 'r'; }); "
                         "return w.get(k) === 'r'; })()"));
    EMB_CHECK(js_true(e, "(function () { try { WeakMap.prototype.getOrInsert.call(new Map(), {}, 1); "
                         "return false; } catch (x) { return x instanceof TypeError; } })()"));
    env_free(e);
}

/* Object.prototype.toString reads the Date class (J4d note): Date objects tag as Date. */
EMB_TEST(object_to_string_tags_date)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new Date(0)) === '[object Date]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(Date.prototype) === '[object Object]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(Date()) === '[object String]'"));
    env_free(e);
}

EMB_TEST_MAIN()
