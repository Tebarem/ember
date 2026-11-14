/* for-of throw paths (JFOROF): a throw out of the body, the head's destructuring or a nested loop
 * closes the iterator and rethrows the original exception, whatever kind of iterator it is (the
 * Array, String, Map and Set fast paths, generators, user iterators) and whatever return() does. */
#include "es6_util.h"

/* The source evaluates to a string; it must equal want. */
#define CASE_TEXT(src, want) E6_TEXT(e, src, want)

EMB_TEST(forof_throw_body)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Destructured pairs over an array of arrays, a Map, a Set of arrays: the body throws. */
    CASE_TEXT("var r = ''; try { for (const [k, v] of [[1, 2], [3, 4]]) { r += k + v; throw new Error('b'); } }"
              "catch (x) { r += x.message; } r", "3b");
    CASE_TEXT("var r = ''; try { for (const [k, v] of new Map([['a', 1], ['b', 2]])) { r += k + v; throw new Error('m'); } }"
              "catch (x) { r += x.message; } r", "a1m");
    CASE_TEXT("var r = ''; try { for (const [k, v] of new Set([[5, 6]])) { r += k + v; throw new Error('s'); } }"
              "catch (x) { r += x.message; } r", "11s");
    CASE_TEXT("var r = ''; try { for (const c of 'ab') { r += c; throw new Error('t'); } }"
              "catch (x) { r += x.message; } r", "at");
    /* Non-object throw values. */
    CASE_TEXT("var r = ''; try { for (const v of [1]) { throw 7; } } catch (x) { r += typeof x + x; } r", "number7");
    CASE_TEXT("var r = ''; try { for (const v of [1]) { throw undefined; } } catch (x) { r += typeof x; } r", "undefined");
    CASE_TEXT("var r = ''; try { for (const v of [1]) { throw null; } } catch (x) { r += String(x); } r", "null");
    CASE_TEXT("var r = ''; try { for (const v of new Map([[1, 2]])) { throw 'str'; } } catch (x) { r += x; } r", "str");
    CASE_TEXT("var r = ''; var o = {}; try { for (const v of new Map([[1, 2]])) { throw o; } } catch (x) { r += (x === o); } r", "true");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(forof_throw_head)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* A throw from the head's destructuring (a non-iterable element, a null source, a default). */
    CASE_TEXT("var r = ''; try { for (const [a] of [1]) { r += 'body'; } } catch (x) { r += x.constructor.name; } r", "TypeError");
    CASE_TEXT("var r = ''; try { for (const {a} of [null]) { r += 'body'; } } catch (x) { r += x.constructor.name; } r", "TypeError");
    CASE_TEXT("var r = ''; try { for (const [a = (() => { throw new RangeError('d'); })()] of [[]]) { r += 'body'; } }"
              "catch (x) { r += x.constructor.name; } r", "RangeError");
    CASE_TEXT("var r = ''; var it = { [Symbol.iterator]() { return { next() { return { value: [], done: false }; },"
              "return() { r += 'R'; return {}; } }; } };"
              "try { for (const [a = (() => { throw new RangeError('d'); })()] of it) { r += 'body'; } }"
              "catch (x) { r += x.constructor.name; } r", "RRangeError");
    CASE_TEXT("var r = ''; var it = { [Symbol.iterator]() { return { next() { return { value: [1], done: false }; },"
              "return() { r += 'R'; return {}; } }; } };"
              "try { for (const [{ z }] of [[null]]) { } } catch (x) { r += x.constructor.name; }"
              "try { for (var [q] of it) { throw 1; } } catch (x) { r += x; } r", "TypeErrorR1");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(forof_close_completions)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *mk = "var log = ''; function mk(retFn) { return { [Symbol.iterator]() { return { n: 0,"
                     "next() { return { value: this.n++, done: false }; }, return: retFn }; } }; } 0";
    EMB_REQUIRE(es6_run(e, mk) != EMBI_JS_EXCEPTION);
    /* A throw completion keeps its exception when return() throws or returns a non-object. */
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; throw new RangeError('r'); })) { throw new SyntaxError('b'); } }"
              "catch (x) { log += x.constructor.name; } log", "RSyntaxError");
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; return 5; })) { throw new SyntaxError('b'); } }"
              "catch (x) { log += x.constructor.name; } log", "RSyntaxError");
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; return 5; })) { throw 3; } }"
              "catch (x) { log += x; } log", "R3");
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; return undefined; })) { throw 'p'; } }"
              "catch (x) { log += x; } log", "Rp");
    /* A normal completion (break, return, continue to an outer label) reports return()'s problems. */
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; return 5; })) { break; } }"
              "catch (x) { log += x.constructor.name; } log", "RTypeError");
    CASE_TEXT("log = ''; try { for (var v of mk(function () { log += 'R'; throw new RangeError('r'); })) { break; } }"
              "catch (x) { log += x.constructor.name; } log", "RRangeError");
    CASE_TEXT("log = ''; function f() { for (var v of mk(function () { log += 'R'; return 5; })) { return 1; } }"
              "try { f(); } catch (x) { log += x.constructor.name; } log", "RTypeError");
    CASE_TEXT("log = ''; try { o: for (var a of [1]) { for (var v of mk(function () { log += 'R'; return 5; })) { continue o; } } }"
              "catch (x) { log += x.constructor.name; } log", "RTypeError");
    /* A non-callable return is a TypeError (GetMethod); on a throw completion it is dropped. */
    CASE_TEXT("log = ''; try { for (var v of mk(1)) { break; } } catch (x) { log += x.constructor.name; } log", "TypeError");
    CASE_TEXT("log = ''; try { for (var v of mk(1)) { throw new SyntaxError('b'); } } catch (x) { log += x.constructor.name; } log", "SyntaxError");
    CASE_TEXT("log = ''; try { for (var v of mk(null)) { break; } log += 'ok'; } catch (x) { log += x.constructor.name; } log", "ok");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(forof_close_nested)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *mk = "var log = ''; function mk(name, retFn) { return { [Symbol.iterator]() { return { n: 0,"
                     "next() { return { value: this.n++, done: false }; }, return() { log += name; return retFn ? retFn() : {}; } }; } }; } 0";
    EMB_REQUIRE(es6_run(e, mk) != EMBI_JS_EXCEPTION);
    /* Nested loops: the throw closes inner then outer, each once, and keeps the original. */
    CASE_TEXT("log = ''; try { for (var a of mk('A')) { for (var b of mk('B')) { throw new Error('e'); } } }"
              "catch (x) { log += x.message; } log", "BAe");
    CASE_TEXT("log = ''; try { for (var a of mk('A')) { for (var b of mk('B', function () { throw new RangeError('rr'); })) { throw new Error('e'); } } }"
              "catch (x) { log += x.message; } log", "BAe");
    /* try/finally between the loops. */
    CASE_TEXT("log = ''; try { for (var a of mk('A')) { try { for (var b of mk('B')) { throw new Error('e'); } } finally { log += 'F'; } } }"
              "catch (x) { log += x.message; } log", "BFAe");
    CASE_TEXT("log = ''; for (var a of mk('A')) { try { for (var b of mk('B')) { break; } } finally { log += 'F'; } break; } log", "BFA");
    CASE_TEXT("log = ''; function g() { for (var a of mk('A')) { try { for (var b of mk('B')) { return 1; } } finally { log += 'F'; } } }"
              "g(); log", "BFA");
    /* A finally that throws replaces the throw; the iterator is still closed. */
    CASE_TEXT("log = ''; try { for (var a of mk('A')) { try { throw new Error('first'); } finally { throw new Error('second'); } } }"
              "catch (x) { log += x.message; } log", "Asecond");
    /* A catch inside the body: the loop continues, no close until it ends. */
    CASE_TEXT("log = ''; for (var a of mk('A')) { try { throw 1; } catch (z) { log += 'c'; } if (a === 1) break; } log", "ccA");
    CASE_TEXT("log = ''; try { for (var a of mk('A')) { break; } } finally { log += 'F'; } log", "AF");
    CASE_TEXT("log = ''; o: for (const [k, v] of [[1, 2]]) { for (const [p, q] of new Map([[3, 4]])) { log += k + v + p + q; break o; } } log", "10");
    /* Generators being iterated. */
    CASE_TEXT("log = ''; function* gen() { try { yield 1; yield 2; } finally { log += 'G'; } }"
              "try { for (var v of gen()) { throw new Error('e'); } } catch (x) { log += x.message; } log", "Ge");
    CASE_TEXT("log = ''; function* gen2() { try { yield 1; } finally { throw new RangeError('fin'); } }"
              "try { for (var v of gen2()) { throw new Error('e'); } } catch (x) { log += x.message; } log", "e");
    CASE_TEXT("log = ''; function* gen3() { try { yield 1; } finally { throw new RangeError('fin'); } }"
              "try { for (var v of gen3()) { break; } } catch (x) { log += x.constructor.name; } log", "RangeError");
    CASE_TEXT("log = ''; function* gen4() { try { yield 1; } finally { return 9; } }"
              "for (var v of gen4()) { break; } log += 'ok'; log", "ok");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(forof_throw_in_functions)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    CASE_TEXT("function* g() { for (const [k, v] of new Map([[1, 2], [3, 4]])) { yield k; throw new Error('gx'); } }"
              "var it = g(); var r = it.next().value; try { it.next(); } catch (x) { r += x.message; } '' + r", "1gx");
    CASE_TEXT("var r = 0; for (var i = 0; i < 2000; i++) { try { for (const [k, v] of new Map([[1, {a: i}]])) { throw {i: i, pad: [1, 2, 3]}; } }"
              "catch (x) { r += x.i === i ? 1 : 0; } } '' + r", "2000");
    CASE_TEXT("var r = ''; var pairs = new Map([['a', 1]]); function f() { for (const [k, v] of pairs) { if (v) throw new TypeError('tt'); } }"
              "try { f(); } catch (x) { r = x.constructor.name + x.message; } r", "TypeErrortt");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A return statement consumes its value: the compile-time stack depth after it is the depth before
 * it. It used to stay one too high (and grew with each finally route), so a later for-of in the same
 * function got a handler whose depth was one slot too high: the handler closed a stale slot as the
 * iterator record (a crash, a missing close, a ToObject of a sentinel). */
EMB_TEST(forof_throw_after_return)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *mk = "var log = ''; function mk(name, retMode) { return { [Symbol.iterator]() { return { n: 0,"
                     "next() { log += name + 'n'; return { value: this.n++, done: false }; },"
                     "return() { log += name + 'R'; if (retMode === 1) throw 'rt'; if (retMode === 2) return 5; return {}; } }; } }; }"
                     "function run(f) { log = ''; var r; try { r = f(); } catch (e) { r = 'EXC' + e; } return r + '|' + log; } 0";
    EMB_REQUIRE(es6_run(e, mk) != EMBI_JS_EXCEPTION);
    /* The close of the return throws a TypeError (caught), then a later loop throws. */
    CASE_TEXT("run(function () { try { for (var a of mk('A', 2)) { return 'r'; } } catch (e) { log += 'c'; }"
              "for (var x of mk('X', 0)) { throw 1; } })", "EXC1|AnARcXnXR");
    CASE_TEXT("run(function () { try { for (var a of mk('A', 2)) { return 'r'; } } catch (e) { log += 'c'; }"
              "for (var x of mk('X', 0)) { for (var y of mk('Y', 1)) { throw 1; } } })", "EXC1|AnARcXnYnYRXR");
    /* Returns routed through finally blocks, then a loop. */
    CASE_TEXT("run(function () { for (var a of mk('A', 0)) { try { if (a) return 'r1'; else return 'r2'; } finally { log += 'F'; } }"
              "for (var x of mk('X', 0)) { throw 3; } })", "r2|AnFAR");
    CASE_TEXT("run(function () { for (var i = 0; i < 2; i++) { try { for (var a of mk('A' + i, 0)) { return 'r'; } } finally { log += 'F'; } } })",
              "r|A0nA0RF");
    /* A plain return in a branch, a return inside a switch, a return in a nested loop. */
    CASE_TEXT("run(function () { var q = 0; if (q) return 1; for (var x of mk('X', 0)) { throw 4; } })", "EXC4|XnXR");
    CASE_TEXT("run(function () { switch (1) { case 1: try { for (var a of mk('A', 2)) { return 'r'; } } catch (e) { log += 'c'; } }"
              "for (var x of mk('X', 1)) { throw 5; } })", "EXC5|AnARcXnXR");
    CASE_TEXT("run(function () { for (var a of mk('A', 0)) { try { for (var b of mk('B', 2)) { return 'r'; } } catch (e) { log += 'c'; }"
              "for (var x of mk('X', 0)) { throw 6; } } })", "EXC6|AnBnBRcXnXRAR");
    /* Destructuring heads and array patterns after such returns. */
    CASE_TEXT("run(function () { try { for (var a of mk('A', 2)) { return 'r'; } } catch (e) { log += 'c'; }"
              "for (const [k, v] of new Map([[1, 2]])) { throw k + v; } })", "EXC3|AnARc");
    CASE_TEXT("run(function () { try { for (var a of mk('A', 2)) { return 'r'; } } catch (e) { log += 'c'; }"
              "var [p] = [1]; for (const [k] of [[1]]) { for (var z of mk('Z', 1)) { throw p + k; } } })", "EXC2|AnARcZnZR");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
