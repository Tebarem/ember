/* ES2015 generators (J2c): function*, yield, next/throw/return, the generator object state machine,
 * parameters bound at call time, yield inside expressions, finally blocks on return, and generator
 * methods in object literals. yield* is in test_es6_gen_delegate.c. */
#include "es6_util.h"
#include <stdio.h>

/* A generator yields its values in order, then reports done with its return value. */
EMB_TEST(gen_basic_protocol)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* g() { yield 1; yield 2; return 3; }"
              "var it = g(); var a = it.next(); var b = it.next(); var c = it.next(); var d = it.next();"
              "a.value * 1000 + b.value * 100 + c.value * 10 + (d.value === undefined ? 5 : 0)", 1235);
    E6_BOOL(e, "var it2 = g(); it2.next(); it2.next(); it2.next().done", true);
    E6_BOOL(e, "var it3 = g(); it3.next(); it3.next(); it3.next(); it3.next().done", true);
    E6_NUM(e, "var it4 = g(); it4.next(); it4.next(); it4.next().value", 3);
    E6_NUM(e, "var it5 = g(); it5.next(); it5.next(); it5.next(); it5.next().value === undefined ? 9 : 0", 9);
    E6_NUM(e, "var n = 0; for (var q = g(), r; !(r = q.next()).done;) n++; n", 2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The body does not run until the first next(), and parameters are bound by the call. */
EMB_TEST(gen_lazy_and_params)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var ran = 0; function* g(a, b = a + 1) { ran++; yield a + b; }"
              "var it = g(2); ran", 0);
    E6_NUM(e, "var ran2 = 0; function* h(a) { ran2 = 1; yield a; } var it2 = h(5); ran2 * 10 + it2.next().value", 5);
    E6_NUM(e, "function* d(x = 1) { yield x; } d().next().value", 1);
    E6_NUM(e, "function* d2(x = 1) { yield x; } d2(7).next().value", 7);
    E6_THROWS(e, "function* bad(x = (function () { throw new TypeError('p'); })()) { yield x; } bad();", "TypeError");
    E6_NUM(e, "function* args() { yield arguments.length; } args(1, 2, 3).next().value", 3);
    E6_NUM(e, "function* rest(...r) { yield r.length; } rest(4, 5).next().value", 2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* next(v) sends v into the yield expression; the first next() value is discarded. */
EMB_TEST(gen_send_values)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* echo() { var x = yield 'first'; while (true) x = yield x * 2; }"
              "var it = echo(); it.next(99); it.next(4).value", 8);
    E6_NUM(e, "function* sum() { var t = 0; while (true) { var v = yield t; if (v === undefined) return t; t += v; } }"
              "var s = sum(); s.next(); s.next(1); s.next(2); s.next(3).value", 6);
    E6_NUM(e, "function* sumdone() { var t = 0; while (true) { var v = yield t; if (v === undefined) return t; t += v; } }"
              "var sd = sumdone(); sd.next(); sd.next(5); sd.next(); sd.next().value === undefined ? 1 : 0", 1);
    E6_NUM(e, "function* ex() { var a = 10 + (yield 1) * 2; yield a; }"
              "var x = ex(); x.next(); x.next(3).value", 16);
    E6_NUM(e, "function* ex2() { return [1, yield 'a', yield 'b'].join(','); }"
              "var y = ex2(); y.next(); y.next('B'); y.next('C').value === '1,B,C' ? 1 : 0", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A yield in the middle of an expression keeps the operands evaluated before it. */
EMB_TEST(gen_yield_in_expression)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* f() { return 100 + (yield 1) + (yield 2); }"
              "var it = f(); it.next(); it.next(10); it.next(20).value", 130);
    E6_TEXT(e, "function* g() { var s = 'a' + (yield 0) + 'c'; return s; }"
               "var it = g(); it.next(); it.next('b').value", "abc");
    E6_NUM(e, "function* h() { var o = {k: yield 'k', n: 2}; return o.k * o.n; }"
              "var it = h(); it.next(); it.next(7).value", 14);
    E6_NUM(e, "function* c() { var arr = [1, 2]; arr.push(yield 'p'); return arr.length; }"
              "var it = c(); it.next(); it.next(9).value", 3);
    E6_NUM(e, "function* t() { try { yield 1; yield 2; } finally { } return 5; }"
              "var it = t(); it.next(); it.next().value + it.next().value", 7);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* throw(e) raises e at the suspended yield, where a try block can catch it. */
EMB_TEST(gen_throw_method)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* g() { try { yield 1; } catch (x) { yield x + 1; } return 9; }"
              "var it = g(); it.next(); it.throw(41).value", 42);
    E6_NUM(e, "function* g2() { try { yield 1; } catch (x) { } return 9; }"
              "var it2 = g2(); it2.next(); var r2 = it2.throw(0); r2.value * 10 + (r2.done ? 1 : 0)", 91);
    E6_THROWS(e, "function* g3() { yield 1; } var it3 = g3(); it3.next(); it3.throw(new TypeError('boom'));",
              "TypeError");
    E6_NUM(e, "function* g4() { yield 1; } var it4 = g4(); try { it4.throw(5); } catch (x) { x; } it4.next().done ? 8 : 0", 8);
    E6_NUM(e, "function* g5() { yield 1; } var it5 = g5(); try { it5.throw(5); } catch (x) { } it5.next().value === undefined ? 6 : 0", 6);
    E6_NUM(e, "function* g6() { yield 1; } var it6 = g6(); var c6; it6.next(); try { it6.throw(7); } catch (x) { c6 = x; } c6", 7);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* return(v) completes the generator, running the finally blocks around the yield first. */
EMB_TEST(gen_return_method)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var log = ''; function* g() { try { yield 1; yield 2; } finally { log += 'f'; } }"
              "var it = g(); it.next(); var r = it.return(7); r.value * 10 + (r.done ? 1 : 0) + log.length", 72);
    E6_TEXT(e, "var log2 = ''; function* h() { try { yield 1; } finally { log2 += 'x'; yield 'in-finally'; } }"
               "var hit = h(); hit.next(); var rr = hit.return(3); rr.value", "in-finally");
    E6_NUM(e, "var log3 = ''; function* k() { try { yield 1; } finally { log3 = 'done'; } }"
              "var kit = k(); kit.return(1); log3 === '' ? 1 : 0", 1);
    E6_NUM(e, "function* ret() { try { yield 1; } finally { return 'override'; } }"
              "var rit = ret(); rit.next(); rit.return(5).value === 'override' ? 4 : 0", 4);
    E6_NUM(e, "function* nested() { for (var i = 0; i < 3; i++) { try { yield i; } finally { } } }"
              "var nit = nested(); nit.next(); var q = nit.return(42); q.value * 2 + (q.done ? 1 : 0)", 85);
    E6_NUM(e, "function* sb() { yield 1; } var sbit = sb(); var bq = sbit.return(11); bq.value * 2 + (bq.done ? 1 : 0)", 23);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The state machine: completed generators stay completed, and a running generator rejects reentry. */
EMB_TEST(gen_state_machine)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function* g() { yield 1; } var it = g(); it.next(); it.next(); it.next().done ? 1 : 0", 1);
    E6_NUM(e, "function* g2() { yield 1; } var it2 = g2(); var rs = it2.return(4); rs.value === 4 && rs.done ? 2 : 0", 2);
    E6_THROWS(e, "function* g3() { yield 1; } var it3 = g3(); it3.return(4); it3.throw(new RangeError('x'));",
              "RangeError");
    E6_NUM(e, "function* g4() { yield 1; } var it4 = g4(); it4.return(); it4.next().done ? 3 : 0", 3);
    E6_THROWS(e, "function* g5() { yield g5i.next(); } var g5i = g5(); g5i.next();", "TypeError");
    E6_NUM(e, "var it6; function* g6() { try { it6.next(); } catch (x) { yield x.name === 'TypeError' ? 5 : 0; } }"
              "it6 = g6(); it6.next().value", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Generator objects, functions and prototypes. */
EMB_TEST(gen_objects_and_prototypes)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "function* g() {} typeof g", "function");
    E6_BOOL(e, "function* g() {} g.prototype instanceof Object", true);
    E6_BOOL(e, "function* g2() {} var p = g2.prototype; Object.getPrototypeOf(p) === Object.getPrototypeOf(g2.prototype)", true);
    E6_BOOL(e, "function* g3() {} var o3 = g3(); Object.getPrototypeOf(o3) === g3.prototype", true);
    E6_BOOL(e, "function* g4() {} g4() instanceof g4", true);
    E6_TEXT(e, "function* g5() {} Object.prototype.toString.call(g5())", "[object Generator]");
    E6_TEXT(e, "function* g6() {} Object.prototype.toString.call(g6)", "[object GeneratorFunction]");
    E6_BOOL(e, "function* g7() {} var GF = Object.getPrototypeOf(g7); GF.prototype === Object.getPrototypeOf(g7.prototype)", true);
    E6_BOOL(e, "function* g8() {} var it8 = g8(); it8[Symbol.iterator]() === it8", true);
    E6_THROWS(e, "function* g9() {} new g9();", "TypeError");
    E6_BOOL(e, "function* g10() {} 'prototype' in g10 && !g10.hasOwnProperty('caller')", true);
    E6_TEXT(e, "function* g11() {} g11.name", "g11");
    E6_NUM(e, "var gx = function* named() { yield named.name.length; }; gx().next().value", 5);
    E6_NUM(e, "var gy = { *m(a) { yield a * 2; } }; gy.m(4).next().value", 8);
    E6_NUM(e, "var gz = { *[ 'co' + 'mp' ]() { yield 3; } }; gz.comp().next().value", 3);
    E6_TEXT(e, "var gw = { *m2() {} }; gw.m2.name", "m2");
    E6_BOOL(e, "var gv = { *m3() {} }; !gv.m3.hasOwnProperty('caller') && gv.m3.prototype !== undefined", true);
    E6_THROWS(e, "var gu = { *m4() {} }; new gu.m4();", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The iterator prototype is in the chain, and a generator iterates itself. */
EMB_TEST(gen_iterator_chain)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function* g() {} var it = g(); var IP = Object.getPrototypeOf(Object.getPrototypeOf(it));"
               "IP[Symbol.iterator].call(5) === 5 || typeof IP[Symbol.iterator] === 'function'", true);
    E6_NUM(e, "function* g2() { yield 1; yield 2; } var s = 0; for (var v of g2()) s += v; s", 3);
    E6_NUM(e, "function* g3() { yield 1; yield 2; } var arr = [...g3()]; arr.length * 10 + arr[1]", 22);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
