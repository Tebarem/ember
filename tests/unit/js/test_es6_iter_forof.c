/* for-of (J2c): the iterator protocol, IteratorClose on break, return and throw, the heads (var, let,
 * const, patterns, member targets), spread on iterables, and the fast paths for unmodified arrays,
 * strings, Maps and Sets, which must stay invisible to the script. */
#include "es6_util.h"
#include <stdio.h>

EMB_TEST(iter_forof_basic)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var s = 0; for (var v of [1, 2, 3]) s += v; s", 6);
    E6_NUM(e, "var s2 = 0; for (let v of [4, 5]) s2 += v; s2", 9);
    E6_NUM(e, "var fs = []; for (const v of [1, 2]) fs.push(function () { return v; }); fs[0]() * 10 + fs[1]()", 12);
    E6_TEXT(e, "var t = ''; for (var ch of 'a\\uD83D\\uDE00b') t += ch + '|'; t", "a|\xF0\x9F\x98\x80|b|");
    E6_NUM(e, "var n = 0; for (var q of []) n++; n", 0);
    E6_NUM(e, "var pr = []; for ([a1, b1] of [[1, 2], [3, 4]]) pr.push(a1 + b1); pr[0] * 10 + pr[1]", 37);
    E6_NUM(e, "var o = {}; for (o.x of [8]) ; o.x", 8);
    E6_NUM(e, "var arr = [1, 2]; var cnt = 0; for (var k in arr) cnt++; var s3 = 0; for (var k2 of arr) s3 += k2; s3 * 10 + cnt", 32);
    E6_NUM(e, "var outer = 0; outer: for (var i of [1, 2, 3]) { for (var j of [1, 2]) { if (j === 2) continue outer; outer += 1; } } outer", 3);
    E6_NUM(e, "var z = 0; for (var zz of [1, 2, 3, 4]) { if (zz === 3) break; z = zz; } z", 2);
    E6_NUM(e, "function f() { for (var v of [1, 2, 3]) { if (v === 2) return v * 7; } return 0; } f()", 14);
    E6_NUM(e, "var lab = 0; a: for (var p of [1]) { b: for (var q2 of [1]) { lab = 5; break a; } } lab", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A custom iterator: break, return and a throw in the body each call return(), exactly once. */
EMB_TEST(iter_forof_close)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *mk = "var closes = 0; function mkiter(n) { var i = 0; return { [Symbol.iterator]() { return this; },"
                     "  next() { return { value: i++, done: i > n }; }, return(v) { closes++; return { done: true, value: v }; } }; } closes";
    E6_NUM(e, mk, 0);
    E6_NUM(e, "for (var v of mkiter(5)) { if (v === 1) break; } closes", 1);
    E6_NUM(e, "for (var v2 of mkiter(5)) { } closes", 1);
    E6_NUM(e, "function g() { for (var v3 of mkiter(5)) { return 3; } } g(); closes", 2);
    E6_NUM(e, "try { for (var v4 of mkiter(5)) { throw new Error('x'); } } catch (x) { } closes", 3);
    E6_NUM(e, "outer: for (var a of [1]) { for (var v5 of mkiter(5)) { continue outer; } } closes", 4);
    E6_NUM(e, "outer2: for (var a2 of [1]) { for (var v6 of mkiter(5)) { break outer2; } } closes", 5);
    E6_NUM(e, "var cc = 0; for (var v7 of mkiter(2)) cc++; cc * 10 + closes", 25);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* IteratorClose errors: the return of a break propagates; the return of a throw is dropped. */
EMB_TEST(iter_forof_close_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "var bad = { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; },"
                 "  return() { throw new RangeError('closing'); } }; } };"
                 "for (var x of bad) { break; }", "RangeError");
    E6_THROWS(e, "for (var y of bad) { throw new TypeError('body'); }", "TypeError");
    E6_NUM(e, "var bad2 = { [Symbol.iterator]() { return { next() { return { value: 1, done: false }; },"
              "  return() { return 5; } }; } };"
              "var threw = 0; try { for (var z of bad2) break; } catch (err) { threw = 1; } threw", 1);
    E6_THROWS(e, "for (var w of 5) {}", "TypeError");
    E6_THROWS(e, "var nx = { [Symbol.iterator]() { return { next() { return 1; } }; } }; for (var u of nx) {}", "TypeError");
    E6_THROWS(e, "var nn = { [Symbol.iterator]() { return null; } }; for (var u2 of nn) {}", "TypeError");
    E6_THROWS(e, "var ithrow = { [Symbol.iterator]() { return { next() { throw new SyntaxError('n'); } }; } };"
                 "for (var u3 of ithrow) {}", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Array fast path: the loop sees elements appended and changed during iteration, as the spec's
 * ArrayIterator does, and a patched %ArrayIteratorPrototype%.next is observed. */
EMB_TEST(iter_forof_array_fastpath)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var g = [1, 2]; var seen = 0; for (var v of g) { seen++; if (g.length < 4) g.push(v * 10); } seen", 4);
    E6_NUM(e, "var h = [1, 2, 3]; var sum = 0; for (var v2 of h) { sum += v2; h[2] = 10; } sum", 13);
    E6_NUM(e, "var hole = [1, , 3]; var c2 = 0; for (var v3 of hole) c2++; c2", 3);
    E6_NUM(e, "var pr = Object.getPrototypeOf([][Symbol.iterator]()); var oldn = pr.next; var calls = 0;"
              "pr.next = function () { calls++; return oldn.call(this); };"
              "var s4 = 0; for (var v4 of [1, 2]) s4 += v4; pr.next = oldn; calls * 10 + s4", 33);
    E6_NUM(e, "var AP = Array.prototype; var savedIt = AP[Symbol.iterator];"
              "AP[Symbol.iterator] = function () { var i = 0; return { next() { return i < 2 ? { value: 'x' + (i++), done: false } : { done: true }; } }; };"
              "var out = ''; for (var v5 of [9, 9, 9]) out += v5; AP[Symbol.iterator] = savedIt; out === 'x0x1' ? 1 : 0", 1);
    E6_NUM(e, "var arrDel = [1, 2, 3]; var cnt = 0; for (var v6 of arrDel) { cnt++; arrDel.length = 1; } cnt", 1);
    E6_NUM(e, "var ta = [5, 6]; var ks = 0; for (var v7 of ta.entries()) ks += v7[0] * 10 + v7[1]; ks", 21);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Map and Set fast paths: the entry order, and changes made during iteration. */
EMB_TEST(iter_forof_map_set)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var m = new Map([[1, 'a'], [2, 'bb']]); var t = 0; for (var kv of m) t += kv[0] * 10 + kv[1].length; t", 33);
    E6_NUM(e, "var m2 = new Map([[1, 1]]); var n = 0; for (var kv2 of m2) { n++; if (n < 3) m2.set(n + 1, n); } n", 3);
    E6_NUM(e, "var s = new Set([1, 2, 2, 3]); var q = 0; for (var v of s) q += v; q", 6);
    E6_NUM(e, "var s2 = new Set([1, 2, 3]); var seen = []; for (var v2 of s2) { seen.push(v2); if (v2 === 1) s2.delete(2); } seen.join(',') === '1,3' ? 1 : 0", 1);
    E6_NUM(e, "var s3 = new Set(); var c = 0; for (var v3 of s3) c++; c", 0);
    E6_NUM(e, "var mk = new Map([[7, 1]]); var keys = 0; for (var kk of mk.keys()) keys += kk; keys", 7);
    E6_NUM(e, "var ms = new Map([[1, 2]]); var mit = ms[Symbol.iterator](); var r = mit.next(); r.value[1]", 2);
    E6_NUM(e, "var cnt = 0; for (var pair of new Map([[1,2],[3,4]])) cnt += pair.length; cnt", 4);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Spread, the iterator-based spreads and destructuring of iterables. */
EMB_TEST(iter_spread_and_pattern)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var a = [...'hé!']; a.length", 3);
    E6_NUM(e, "var b = [...new Set([3, 3, 4])]; b.length * 10 + b[1]", 24);
    E6_NUM(e, "function f(x, y, z) { return x + y * 10 + z * 100; } f(...[1, 2], ...'3')", 321);
    E6_NUM(e, "var [p, q, ...r] = new Set([1, 2, 3, 4]); p * 1000 + q * 100 + r.length", 1202);
    E6_NUM(e, "var [c1, c2] = 'xy'; (c1 === 'x' && c2 === 'y') ? 1 : 0", 1);
    E6_NUM(e, "var m = new Map([[1, 'one']]); var [[mk, mv]] = m; mk + mv.length", 4);
    E6_NUM(e, "var o = [...new Map([[1, 2]]).values()]; o[0]", 2);
    E6_THROWS(e, "[...5];", "TypeError");
    E6_NUM(e, "var arrs = [0, ...[1, 2], ...'ab']; arrs.length", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The fast paths are invisible: a return method on %IteratorPrototype% is looked up and called on
 * break, for an array, a string, a Map, a Set and a destructuring pattern. A return defined in the
 * loop body is found at the close too, and the method gets the real iterator as its receiver. */
EMB_TEST(iter_forof_return_lookup)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *ip = "var IP = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())); var rc = 0;"
                     "IP.return = function () { rc++; return {}; };";
    EMB_CHECK(es6_run(e, ip) != EMBI_JS_EXCEPTION);
    E6_NUM(e, "for (var x of [1, 2]) break; rc", 1);
    E6_NUM(e, "for (var y of 'ab') break; rc", 2);
    E6_NUM(e, "for (var m of new Map([[1, 2]])) break; rc", 3);
    E6_NUM(e, "for (var s of new Set([1])) break; rc", 4);
    E6_NUM(e, "var [a1] = [1, 2, 3]; rc", 5);
    E6_NUM(e, "var [b1] = 'xy'; rc", 6);
    E6_NUM(e, "for (var z of [1]) { } rc", 6);
    E6_NUM(e, "var mid = 0; var ts = ''; for (var q of [1, 2]) { IP.return = function () { mid++; ts = Object.prototype.toString.call(this); return {}; }; break; } mid * 10 + (ts === '[object Array Iterator]' ? 1 : 0)", 11);
    E6_NUM(e, "delete IP.return; var before = mid; for (var w of [1]) break; mid === before ? 1 : 0", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A throw while the head's member target is assigned closes the iterator (once). The setter is
 * the throw here; the replayed target code runs for every key. */
EMB_TEST(iter_forof_close_member_target)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    const char *mk = "var n = ''; function mk(k) { var i = 0; return { [Symbol.iterator]() { return this; },"
                     "  next() { n += 'n'; return { value: i, done: i++ >= k }; },"
                     "  return() { n += 'r'; return {}; } }; }";
    EMB_CHECK(es6_run(e, mk) != EMBI_JS_EXCEPTION);
    E6_THROWS(e, "var thrower = { set x(v) { throw new RangeError('s'); } }; for (thrower.x of mk(3)) {}",
              "RangeError");
    E6_TEXT(e, "n", "nr");
    E6_TEXT(e, "n = ''; var o2 = { set y(v) { if (v === 1) throw new RangeError('y'); } };"
               "try { for (o2.y of mk(3)) {} } catch (err) {} n", "nnr");
    E6_TEXT(e, "n = ''; var ob = {}; for (ob.z of mk(2)) {} n", "nnn");
    /* The target's base is evaluated for every key, and its throw closes the iterator too. */
    E6_TEXT(e, "n = ''; var base = { get b() { throw new RangeError('b'); } };"
               "try { for (base.b.x of mk(3)) {} } catch (err) {} n", "nr");
    E6_TEXT(e, "n = ''; var pat = { set w(v) { throw new RangeError('w'); } };"
               "var mk2 = { [Symbol.iterator]() { var i = 0; return { next() { n += 'n';"
               "  return { value: [1], done: i++ >= 2 }; }, return() { n += 'r'; return {}; } }; } };"
               "try { for ([pat.w] of mk2) {} } catch (err) {} n", "nr");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* String and array iterator methods: keys, values, entries, and the string code point steps. */
EMB_TEST(iter_builtin_methods)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var it = ['a', 'b'].keys(); var r1 = it.next(); r1.value + it.next().value", 1);
    E6_TEXT(e, "[...['a', 'b'].values()].join('')", "ab");
    E6_TEXT(e, "var en = [...['x', 'y'].entries()]; en[1].join(':')", "1:y");
    E6_NUM(e, "var sit = 'a\\uD83D\\uDE00'[Symbol.iterator](); sit.next(); sit.next().value.length", 2);
    E6_TEXT(e, "Object.prototype.toString.call([].values())", "[object Array Iterator]");
    E6_TEXT(e, "Object.prototype.toString.call(''[Symbol.iterator]())", "[object String Iterator]");
    E6_BOOL(e, "var ai = [][Symbol.iterator](); ai[Symbol.iterator]() === ai", true);
    E6_BOOL(e, "Array.prototype[Symbol.iterator] === Array.prototype.values", true);
    E6_NUM(e, "function args() { var s = 0; for (var v of arguments) s += v; return s; } args(1, 2, 3)", 6);
    E6_NUM(e, "var ai2 = [7][Symbol.iterator](); var d1 = ai2.next(); var d2 = ai2.next(); d1.value * 10 + (d2.done ? 1 : 0)", 71);
    E6_NUM(e, "var mit = new Map().keys(); Object.getPrototypeOf(Object.getPrototypeOf(mit)) === Object.getPrototypeOf(Object.getPrototypeOf([].values())) ? 1 : 0", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
