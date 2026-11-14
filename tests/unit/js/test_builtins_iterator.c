/* The iterator builtins (J2c): the Iterator constructor, Iterator.from with its wrapper, the lazy
 * helpers (map, filter, take, drop, flatMap) and the eager methods (reduce, toArray, forEach,
 * some, every, find), their close semantics (return on a helper, take exhausted, callback throws,
 * argument validation), the re-entrancy check, the Array and String iterators, and the
 * GeneratorFunction constructor. Each test checks that the runtime's live memory returns to its
 * baseline. */
#include "es6_util.h"

/* A source iterator of n values (0 .. n-1) that counts its next and return calls. */
#define MK_SRC                                                                                     \
    "var closes = 0; var calls = 0;"                                                               \
    "function mk(n) { var i = 0; var it = Object.create(Iterator.prototype);"                       \
    "  it.next = function () { calls++; return { value: i, done: i++ >= n }; };"                    \
    "  it.return = function () { closes++; return {}; }; return it; }"

EMB_TEST(iterator_ctor_and_from)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, MK_SRC) != EMBI_JS_EXCEPTION);
    E6_THROWS(e, "new Iterator()", "TypeError");
    E6_THROWS(e, "Iterator()", "TypeError");
    E6_BOOL(e, "Iterator.prototype.constructor === Iterator", true);
    E6_TEXT(e, "Object.prototype.toString.call(Iterator.prototype)", "[object Iterator]");
    E6_BOOL(e, "typeof Iterator.from === 'function'", true);
    /* An iterator that already inherits from %Iterator.prototype% comes back as itself. */
    E6_NUM(e, "var f0 = mk(2); Iterator.from(f0) === f0 ? 1 : 0", 1);
    /* A plain iterator object is wrapped; the wrapper forwards next to it. */
    E6_NUM(e, "var plain = { i: 0, next() { return { value: this.i, done: this.i++ >= 2 }; } };"
              "Iterator.from(plain).toArray().length", 2);
    /* Iterables and strings are iterated through Symbol.iterator. */
    E6_TEXT(e, "Iterator.from([5, 6]).toArray().join(',')", "5,6");
    E6_TEXT(e, "Iterator.from('ab').toArray().join(',')", "a,b");
    E6_THROWS(e, "Iterator.from(5)", "TypeError");
    /* Return on a wrapper forwards to the underlying return, or reports done without one. */
    E6_NUM(e, "var rw = 0; var pw = { next() { return { value: 1, done: false }; },"
              "  return() { rw++; return {}; } }; Iterator.from(pw).return(); rw", 1);
    E6_NUM(e, "Iterator.from({ next() { return { done: true }; } }).return().done ? 1 : 0", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(iterator_helpers_lazy)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, MK_SRC) != EMBI_JS_EXCEPTION);
    E6_TEXT(e, "mk(3).map(function (x) { return x + 1; }).toArray().join(',')", "1,2,3");
    E6_TEXT(e, "mk(5).filter(function (x) { return x % 2; }).toArray().join(',')", "1,3");
    E6_TEXT(e, "mk(5).take(2).toArray().join(',')", "0,1");
    E6_TEXT(e, "mk(5).take('2').toArray().join(',')", "0,1");
    E6_TEXT(e, "mk(4).drop(2).toArray().join(',')", "2,3");
    E6_TEXT(e, "mk(2).flatMap(function (x) { return [x, x * 10]; }).toArray().join(',')", "0,0,1,10");
    E6_TEXT(e, "Object.prototype.toString.call(mk(1).map(function (x) { return x; }))",
            "[object Iterator Helper]");
    /* take exhausted closes the source, once the helper is asked for the value after the limit. */
    E6_NUM(e, "closes = 0; mk(5).take(2).toArray(); closes", 1);
    /* take(0) closes the source at the first step, without calling its next. */
    E6_NUM(e, "closes = 0; calls = 0; mk(5).take(0).toArray().length * 10 + closes + calls * 100", 1);
    E6_THROWS(e, "mk(3).take(NaN)", "RangeError");
    E6_THROWS(e, "mk(3).drop(-1)", "RangeError");
    E6_THROWS(e, "mk(3).take(-1)", "RangeError");
    E6_THROWS(e, "mk(3).take(9007199254740992)", "RangeError");
    E6_THROWS(e, "mk(3).drop(9007199254740992)", "RangeError");
    E6_NUM(e, "mk(2).take(Infinity).toArray().length", 2);
    E6_NUM(e, "mk(2).take(9007199254740991).toArray().length", 2);
    /* A bad argument closes the underlying iterator first. */
    E6_NUM(e, "closes = 0; try { mk(3).map(5); } catch (err) { } closes", 1);
    E6_NUM(e, "closes = 0; try { mk(3).take(-1); } catch (err) { } closes", 1);
    /* flatMap: a mapper result that is not an object closes the source and throws. */
    E6_NUM(e, "closes = 0; try { mk(1).flatMap(function () { return 5; }).toArray(); } catch (err) { } closes", 1);
    E6_NUM(e, "closes = 0; var inner = mk(3); var h = mk(2).flatMap(function () { return inner; });"
              "h.next(); h.return(); closes", 2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(iterator_helper_return_states)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, MK_SRC) != EMBI_JS_EXCEPTION);
    /* Return before any next: the source is closed and the helper is done. */
    E6_NUM(e, "closes = 0; var h1 = mk(3).map(function (x) { return x; }); h1.return(); closes", 1);
    E6_NUM(e, "var h2 = mk(3).map(function (x) { return x; }); h2.return(); h2.next().done ? 1 : 0", 1);
    /* Return after a value: the source closes once. */
    E6_NUM(e, "closes = 0; var h3 = mk(3).map(function (x) { return x; }); h3.next(); h3.return(); closes", 1);
    /* Return on a helper whose source has no return method: nothing to call. */
    E6_NUM(e, "var h4 = [1, 2].values().map(function (x) { return x; }); h4.return().done ? 1 : 0", 1);
    /* Return while an inner iterator of flatMap is open closes the inner, then the source. */
    E6_NUM(e, "closes = 0; var inner2 = mk(3); var h5 = mk(2).flatMap(function () { return inner2; });"
              "h5.next(); h5.return(); closes", 2);
    /* A callback that throws closes the source (the helper is then done). */
    E6_NUM(e, "closes = 0; var h6 = mk(3).map(function () { throw new RangeError('cb'); });"
              "try { h6.next(); } catch (err) { } closes", 1);
    E6_NUM(e, "var h7 = mk(3).map(function () { throw new RangeError('cb'); });"
              "try { h7.next(); } catch (err) { } h7.next().done ? 1 : 0", 1);
    /* Re-entrancy: a helper that is already running throws when it is stepped again. */
    E6_THROWS(e, "var rh; var rhh = mk(3).map(function () { return rh.next(); }); rh = rhh; rh.next();",
              "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(iterator_eager_methods)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    EMB_CHECK(es6_run(e, MK_SRC) != EMBI_JS_EXCEPTION);
    E6_NUM(e, "mk(3).toArray().length", 3);
    E6_NUM(e, "mk(4).reduce(function (a, b) { return a + b; })", 6);
    E6_NUM(e, "mk(3).reduce(function (a, b) { return a + b; }, 10)", 13);
    E6_THROWS(e, "mk(0).reduce(function (a, b) { return a; })", "TypeError");
    E6_NUM(e, "var fe = 0; mk(4).forEach(function (v) { fe += v; }); fe", 6);
    E6_BOOL(e, "mk(5).some(function (v) { return v === 2; })", true);
    E6_NUM(e, "closes = 0; mk(5).some(function (v) { return v === 1; }); closes", 1);
    E6_BOOL(e, "mk(5).every(function (v) { return v < 2; })", false);
    E6_NUM(e, "closes = 0; mk(5).every(function (v) { return v < 2; }); closes", 1);
    E6_NUM(e, "mk(5).find(function (v) { return v > 2; })", 3);
    E6_NUM(e, "closes = 0; mk(5).find(function (v) { return v > 1; }); closes", 1);
    E6_BOOL(e, "mk(0).every(function () { return false; })", true);
    E6_BOOL(e, "mk(0).some(function () { return true; })", false);
    /* A throwing callback propagates and closes the source (forEach, reduce and some). */
    E6_NUM(e, "closes = 0; try { mk(5).forEach(function () { throw new RangeError('x'); }); } catch (err) { } closes", 1);
    E6_NUM(e, "closes = 0; try { mk(5).reduce(function () { throw new RangeError('x'); }, 0); } catch (err) { } closes", 1);
    E6_NUM(e, "closes = 0; try { mk(5).some(function () { throw new RangeError('x'); }); } catch (err) { } closes", 1);
    /* A non-callable argument closes the source and throws. */
    E6_NUM(e, "closes = 0; try { mk(5).forEach(7); } catch (err) { } closes", 1);
    E6_NUM(e, "closes = 0; try { mk(5).find({}); } catch (err) { } closes", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(iterator_array_string_iterators)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var ai = [1].values(); ai.next(); ai.next(); ai.next().done ? 1 : 0", 1);
    E6_NUM(e, "[...[7, 8].keys()].join(',') === '0,1' ? 1 : 0", 1);
    E6_NUM(e, "[...[7, 8].entries()][1][1]", 8);
    E6_NUM(e, "[...'abc'].length", 3);
    E6_NUM(e, "[...'a\\uD83D\\uDE00b'].length", 3);
    E6_NUM(e, "[...'\\uD83Dx'].length", 2);
    E6_TEXT(e, "[...'abc'].join('')", "abc");
    E6_BOOL(e, "'x'[Symbol.iterator]().next().value === 'x'", true);
    E6_TEXT(e, "Object.prototype.toString.call(''[Symbol.iterator]())", "[object String Iterator]");
    E6_TEXT(e, "Object.prototype.toString.call([].keys())", "[object Array Iterator]");
    /* A keys or entries iterator has its own next in the chain, and the array's iterator proto
     * inherits from %Iterator.prototype%. */
    E6_BOOL(e, "Object.getPrototypeOf(Object.getPrototypeOf([].keys())) === Iterator.prototype", true);
    E6_BOOL(e, "[].values().map !== undefined", true);
    E6_NUM(e, "[1, 2, 3].values().map(function (x) { return x * 2; }).reduce(function (a, b) { return a + b; })", 12);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* GeneratorFunction (ES2025 27.3.1.1): reachable through a generator function's prototype, not a
 * global, and it builds generators from source text with the same checks Function applies. */
EMB_TEST(generator_function_ctor)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "Object.getPrototypeOf(function* () {}).constructor.name", "GeneratorFunction");
    E6_BOOL(e, "typeof GeneratorFunction === 'undefined'", true);
    E6_NUM(e, "var GF2 = Object.getPrototypeOf(function* () {}).constructor;"
              "var g2 = GF2('a', 'yield a; yield a + 1')(4); g2.next().value * 10 + g2.next().value", 45);
    E6_BOOL(e, "Object.getPrototypeOf(Object.getPrototypeOf(function* () {}).constructor) === Function", true);
    E6_BOOL(e, "Object.getPrototypeOf(function* () {}).constructor.prototype === Object.getPrototypeOf(function* () {})", true);
    E6_BOOL(e, "var GFP = Object.getPrototypeOf(function* () {}); GFP.prototype === Object.getPrototypeOf(Object.getPrototypeOf((function* () {})()))", true);
    E6_NUM(e, "Object.getPrototypeOf(function* () {}).constructor('yield 1; yield 2')().next().value", 1);
    E6_NUM(e, "var GFc = Object.getPrototypeOf(function* () {}).constructor; var g3 = new GFc('yield 5'); g3().next().value", 5);
    E6_THROWS(e, "Object.getPrototypeOf(function* () {}).constructor('}); (function () {')", "SyntaxError");
    E6_THROWS(e, "Object.getPrototypeOf(function* () {}).constructor('a b', 'return 1')", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
