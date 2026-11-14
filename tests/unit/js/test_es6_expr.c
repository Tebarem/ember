/* ES2015+ expressions (J2a §2): arrow functions, spread, object literal extensions, template
 * literals, optional chaining, ??, logical assignment, new.target, name inference, and toString. */
#include "es6_util.h"

/* Arrow functions: lexical this, arguments and new.target; expression and block bodies. */
EMB_TEST(es6_arrow_functions)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var f1 = (a, b) => a + b; f1(2, 3)", 5);
    E6_NUM(e, "var f2 = x => x * 2; f2(4)", 8);
    E6_NUM(e, "var f3 = () => { return 7 }; f3()", 7);
    E6_NUM(e, "var f4 = (a = 2, ...r) => a + r.length; f4(undefined, 1, 1)", 4);
    E6_NUM(e, "var f6 = () => ({v: 9}); f6().v", 9);
    E6_NUM(e, "var f7 = (a,) => a; f7(3)", 3);
    E6_NUM(e, "var c1 = 0; var f8 = () => ++c1; f8(); f8(); c1", 2);
    E6_BOOL(e, "var outerArgs = (function () { var f9 = () => arguments; return f9(7) })(1); outerArgs[0] === 1", true);
    E6_TEXT(e, "function outerThis() { return [1].map(() => this.v).join() } outerThis.call({v: 'q'})", "q");
    E6_NUM(e, "function outerNT() { return (() => new.target)() === undefined ? 1 : 0 } outerNT()", 1);
    E6_NUM(e, "function NT() { this.k = (() => new.target === NT)() ? 1 : 0 } new NT().k", 1);
    E6_NUM(e, "var fx = (q) => { var q2 = q; return q2 }; fx(6)", 6);
    E6_NUM(e, "var g = { v: 5, m() { return (() => this.v)() } }; g.m()", 5);
    E6_THROWS(e, "var ac = () => 1; new ac();", "TypeError");
    E6_BOOL(e, "var ap = () => 1; ap.hasOwnProperty('prototype')", false);
    E6_SYNTAX(e, "var bad = (a, a) => 1;");
    E6_SYNTAX(e, "var bad2 = (a) \n=> 1;");
    E6_SYNTAX(e, "var bad3 = a + (b) => 1;");
    E6_SYNTAX(e, "'use strict'; var bad4 = (eval) => 1;");
    es6_env_free(e);
}

/* Arrow functions are named from the binding that holds them. */
EMB_TEST(es6_name_inference)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var na = () => 1; na.name", "na");
    E6_TEXT(e, "var nf = function () {}; nf.name", "nf");
    E6_TEXT(e, "var nn = function inner() {}; nn.name", "inner");
    E6_TEXT(e, "let nl = (a) => a; nl.name", "nl");
    E6_TEXT(e, "var nd = (function () { return function () {} })(); nd.name", "");
    E6_TEXT(e, "var na2; na2 = () => 2; na2.name", "na2");
    E6_TEXT(e, "var no = {}; no.p = function () {}; no.p.name", "");
    E6_TEXT(e, "var lo = {q: function () {}, r: () => 1}; lo.q.name + lo.r.name", "qr");
    E6_TEXT(e, "var k = 'dyn'; var co = {[k]: function () {}}; co.dyn.name", "dyn");
    E6_TEXT(e, "var k2 = 'acc'; var ca = {get [k2]() { return 1 }}; Object.getOwnPropertyDescriptor(ca, 'acc').get.name", "get acc");
    E6_TEXT(e, "var mo = {m() {}}; mo.m.name", "m");
    E6_TEXT(e, "var dk = 'z'; var cm = {[dk]() {}}; cm.z.name", "z");
    E6_TEXT(e, "var pa = 1; var lg; lg ||= function () {}; lg.name", "lg");
    E6_TEXT(e, "function dp(a = function () {}) { return a.name } dp()", "a");
    es6_env_free(e);
}

/* Spread in calls, new and array literals; holes; trailing commas. */
EMB_TEST(es6_spread)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function sum() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s } sum(...[1, 2], 3, ...[4])", 10);
    E6_NUM(e, "var xs = [1, 2]; Math.max(...xs, 0)", 2);
    E6_NUM(e, "function two(a, b) { return a * 10 + b } two(...[4, 2])", 42);
    E6_NUM(e, "function Ctor(a, b) { this.s = a + b } new Ctor(...[2, 5]).s", 7);
    E6_NUM(e, "var arr = [...[1, 2], 3, ...'ab']; arr.length", 5);
    E6_TEXT(e, "[...'hi', 0].join('|')", "h|i|0");
    E6_NUM(e, "var h = [1, , 3]; h.length", 3);
    E6_BOOL(e, "var h2 = [1, , 3]; 1 in h2", false);
    E6_NUM(e, "[...[1], , ].length", 2);
    E6_NUM(e, "[1, 2, ].length", 2);
    E6_NUM(e, "[,].length", 1);
    E6_NUM(e, "function tc(a, b,) { return a + b } tc(1, 2,)", 3);
    E6_NUM(e, "var o = {a: 1, b: 2,}; Object.keys(o).length", 2);
    E6_NUM(e, "var sp = {...{p: 1, q: 2}, q: 3}; sp.p * 10 + sp.q", 13);
    E6_BOOL(e, "var src = {x: 1}; var cp = {...src}; cp !== src && cp.x === 1", true);
    E6_NUM(e, "var nul = {...null, ...undefined}; Object.keys(nul).length", 0);
    E6_NUM(e, "var getterCount = 0; var g = {get v() { getterCount++; return 1 }}; var cpy = {...g}; getterCount", 1);
    es6_env_free(e);
}

/* Object literal extensions: shorthand, methods, computed keys, __proto__, accessors, duplicates. */
EMB_TEST(es6_object_literals)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var sa = 1, sb = 2; var so = {sa, sb}; so.sa + so.sb * 10", 21);
    E6_NUM(e, "var mo = {m(x) { return x + 1 }}; mo.m(4)", 5);
    E6_NUM(e, "var kk = 'key'; var ck = {[kk]: 3, [kk + '2']: 4}; ck.key * 10 + ck.key2", 34);
    E6_NUM(e, "var ck2 = {['a' + 1]() { return 8 }}; ck2.a1()", 8);
    E6_NUM(e, "var pr = {x: 1}; var po = {__proto__: pr}; po.x", 1);
    E6_BOOL(e, "var pr2 = {}; var po2 = {__proto__: pr2}; Object.getPrototypeOf(po2) === pr2", true);
    E6_BOOL(e, "var po3 = {['__proto__']: 1}; Object.getPrototypeOf(po3) === Object.prototype && po3.__proto__ === 1", true);
    E6_BOOL(e, "var po4 = {__proto__}; var __proto__ = 1; Object.getPrototypeOf(po4) === Object.prototype", true);
    E6_SYNTAX(e, "var dpr = {__proto__: 1, __proto__: 2};");
    E6_NUM(e, "var ac = {get [('g' + 1)]() { return 6 }, set s(v) { this.sv = v }}; ac.g1 + (ac.s = 2, ac.sv)", 8);
    E6_NUM(e, "var ov = {a: 1, a: 2}; ov.a", 2);
    E6_NUM(e, "var ov2 = {get b() { return 1 }, b: 5}; ov2.b", 5);
    E6_NUM(e, "var tv = {f: 1, f: 2, 'f': 3}; tv.f", 3);
    E6_NUM(e, "function Cn() {} var cm = {m() {}}; cm.m.prototype === undefined ? 1 : 0", 1);
    E6_THROWS(e, "var mm = {m() {}}; new mm.m();", "TypeError");
    es6_env_free(e);
}

/* Template literals and tagged templates. */
EMB_TEST(es6_templates)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "`plain`", "plain");
    E6_TEXT(e, "var t1 = 'x'; `a${t1}b${1 + 1}c`", "axb2c");
    E6_TEXT(e, "`multi\nline`", "multi\nline");
    E6_TEXT(e, "`${null} ${undefined} ${[1, 2]} ${{}}`", "null undefined 1,2 [object Object]");
    E6_TEXT(e, "`\\u0041\\x42`", "AB");
    E6_NUM(e, "`${1}${2}`.length", 2);
    E6_SYNTAX(e, "`\\u{zz}`");
    E6_TEXT(e, "function tag(s) { return s[0] + '|' + s[1] + '|' + s.raw[1] } tag`A${1}\\n`", "A|\n|\\n");
    E6_TEXT(e, "function tag2(s, a, b) { return s.length + ':' + a + b } tag2`x${1}y${2}z`", "3:12");
    E6_BOOL(e, "function same(s) { return s } var got = []; for (var i = 0; i < 2; i++) got.push(same`q`); got[0] === got[1]", true);
    E6_BOOL(e, "function fr(s) { return Object.isFrozen(s) && Object.isFrozen(s.raw) } fr`z`", true);
    E6_TEXT(e, "function bad(s) { return s[0] === undefined ? 'undef' : 'def' } bad`\\unicode`", "undef");
    E6_SYNTAX(e, "var u = `\\unicode`;");
    E6_TEXT(e, "function ro(s) { return s.raw[0] } ro`\\u0041`", "\\u0041");
    E6_TEXT(e, "String.raw`a\\nb${1}`", "a\\nb1");
    E6_TEXT(e, "var o2 = {m(s) { return s[0] }}; o2.m`meth`", "meth");
    es6_env_free(e);
}

/* Optional chaining: short-circuiting the whole chain, calls, indexes, and delete. */
EMB_TEST(es6_optional_chaining)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var oa = {b: {c: 3}}; oa?.b.c", 3);
    E6_BOOL(e, "var na = null; na?.b === undefined", true);
    E6_BOOL(e, "var na2 = undefined; na2?.b.c.d.e === undefined", true);
    E6_NUM(e, "var na3 = null; var hits = 0; na3?.[hits++]; hits", 0);
    E6_NUM(e, "var ar = [5]; ar?.[0]", 5);
    E6_NUM(e, "var fo = {f() { return this.v }, v: 4}; fo.f?.()", 4);
    E6_BOOL(e, "var nf = {}; nf.g?.() === undefined", true);
    E6_BOOL(e, "var nf2 = null; nf2?.() === undefined", true);
    E6_NUM(e, "var cnt = 0; var nul = null; nul?.[cnt++].x.y; cnt", 0);
    E6_NUM(e, "var fun = function (a) { return a + 1 }; fun?.(1)", 2);
    E6_NUM(e, "var pz = {}; pz.q?.r ?? 9", 9);
    E6_BOOL(e, "var dn = {p: 1}; delete dn?.p; !('p' in dn)", true);
    E6_BOOL(e, "var dn2 = null; delete dn2?.p", true);
    E6_SYNTAX(e, "var sy = null; sy?.a = 1;");
    E6_SYNTAX(e, "var sy2 = null; new sy2?.b();");
    E6_SYNTAX(e, "var sy3 = null; sy3?.b`t`;");
    E6_NUM(e, "var deep = {a: {b: function () { return 2 }}}; deep?.a?.b?.()", 2);
    es6_env_free(e);
}

/* ?? and logical assignment. */
EMB_TEST(es6_nullish_and_logical_assignment)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "null ?? 4", 4);
    E6_NUM(e, "0 ?? 4", 0);
    E6_TEXT(e, "'' ?? 'x'", "");
    E6_NUM(e, "undefined ?? null ?? 6", 6);
    E6_NUM(e, "var calls = 0; function side() { calls++; return 1 } 2 ?? side(); calls", 0);
    E6_NUM(e, "(null || undefined) ?? 8", 8);
    E6_SYNTAX(e, "a || b ?? c;");
    E6_SYNTAX(e, "a ?? b || c;");
    E6_SYNTAX(e, "a && b ?? c;");
    E6_NUM(e, "var ww = null; ww ?\?= 5; ww", 5);
    E6_NUM(e, "var wx = 1; wx ?\?= 5; wx", 1);
    E6_NUM(e, "var wa = 0; wa ||= 7; wa", 7);
    E6_NUM(e, "var wb = 3; wb ||= 7; wb", 3);
    E6_NUM(e, "var wc = 3; wc &&= 9; wc", 9);
    E6_NUM(e, "var wd = 0; wd &&= 9; wd", 0);
    E6_NUM(e, "var setc = 0; var so = {get p() { return 0 }, set p(v) { setc++ }}; so.p &&= 1; setc", 0);
    E6_NUM(e, "var sob = {p: 0}; sob.p ||= 4; sob.p", 4);
    E6_NUM(e, "var sob2 = {p: null}; sob2.p ?\?= 2; sob2.p", 2);
    E6_NUM(e, "var ia = [0]; var ii = 0; ia[ii++] ||= 5; ia[0] + ii", 6);
    E6_NUM(e, "var ka = {q: 1}; var r1 = (ka.q &&= 0); r1 + ka.q", 0);
    E6_NUM(e, "const cl = 0; try { cl ||= 2 } catch (err) { err instanceof TypeError ? 1 : 0 }", 1);
    E6_THROWS(e, "const cl2 = 0; cl2 ||= 2;", "TypeError");
    es6_env_free(e);
}

/* new.target: undefined for calls, the constructor for new. */
EMB_TEST(es6_new_target)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function nt1() { return new.target } nt1() === undefined", true);
    E6_BOOL(e, "function nt2() { this.t = new.target } new nt2().t === nt2", true);
    /* a constructor that returns an object (here the function itself) yields that object */
    E6_BOOL(e, "function nt3() { return new.target } new nt3() === nt3", true);
    E6_SYNTAX(e, "new.target;");
    es6_env_free(e);
}

/* Function.prototype.toString returns the exact source slice. */
EMB_TEST(es6_function_source_text)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "(function  f ( a ) { return a })+''", "function  f ( a ) { return a }");
    E6_TEXT(e, "(x => x+1)+''", "x => x+1");
    E6_TEXT(e, "((a, b) => { return a })+''", "(a, b) => { return a }");
    E6_TEXT(e, "var so = {  m ( ) { } }; so.m + ''", "m ( ) { }");
    E6_TEXT(e, "var sg = {get  q() { return 1 }}; Object.getOwnPropertyDescriptor(sg, 'q').get + ''", "get  q() { return 1 }");
    E6_TEXT(e, "function  decl (x) {\n  return x\n} decl + ''", "function  decl (x) {\n  return x\n}");
    E6_TEXT(e, "var sd = (function () { return ()=>1 })(); sd + ''", "()=>1");
    E6_TEXT(e, "(function(a = 1, ...b) {}) + ''", "function(a = 1, ...b) {}");
    es6_env_free(e);
}


/* A parenthesized assignment target does not name an anonymous function. */
EMB_TEST(es6_paren_target_no_name)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var pa; (pa) = function(){}; pa.name", "");
    E6_TEXT(e, "var pb; pb = function(){}; pb.name", "pb");
    E6_TEXT(e, "var pc = null; (pc) ?" "?= function(){}; pc.name", "");
    E6_TEXT(e, "var pd = null; pd ?" "?= function(){}; pd.name", "pd");
    es6_env_free(e);
}

/* Member compound and logical assignment check the base before converting the key. */
EMB_TEST(es6_member_assign_base_first)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var n=null, c=0, k={toString(){c++;return 'k'}}; try { n[k] += 1 } catch(x){} c", 0);
    E6_NUM(e, "var n1=undefined, c1=0, k1={toString(){c1++;return 'k'}}; try { n1[k1] ||= 1 } catch(x){} c1", 0);
    E6_NUM(e, "var n2=null, c2=0, k2={toString(){c2++;return 'k'}}; try { n2[k2] &&= 1 } catch(x){} c2", 0);
    E6_NUM(e, "var n3=null, c3=0, k3={toString(){c3++;return 'k'}}; try { n3[k3]++ } catch(x){} c3", 0);
    E6_THROWS(e, "var n4=null; n4[{toString(){return 'k'}}] += 1", "TypeError");
    E6_NUM(e, "var ob={}, c5=0, k5={toString(){c5++;return 'k'}}; ob[k5] = 1; ob[k5] += 2; ob.k + c5*100", 203);
    es6_env_free(e);
}

EMB_TEST_MAIN()
