/* ES2015 scoping (J2a §1): let, const, TDZ, per-iteration bindings, redeclaration early errors,
 * labels and finally across lexical scopes, and the sloppy/strict block function rules. */
#include "es6_util.h"

/* let and const bindings are block-scoped and shadow outer names. */
EMB_TEST(es6_block_scope_basics)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var a = 1; { let a = 2; } a", 1);
    E6_NUM(e, "{ let b = 3; { let b = 4; } b }", 3);
    E6_NUM(e, "{ const c = 5; c }", 5);
    E6_NUM(e, "let top = 6; top", 6);
    E6_NUM(e, "function sh() { let x = 1; { let x = 2; } return x } sh()", 1);
    E6_NUM(e, "var n = 0; for (let i = 0; i < 3; i++) { let j = i; n += j } n", 3);
    E6_NUM(e, "switch (2) { case 1: let s = 1; break; case 2: let s2 = 9; s2 }", 9);
    E6_NUM(e, "try { throw 4 } catch (ex) { let y = ex; y }", 4);
    E6_TEXT(e, "let u = 'x'; var uy; { let u = 'y'; uy = u } uy + u", "yx");
    es6_env_free(e);
}

/* Reads and writes before initialization throw ReferenceError, including from closures and
 * switch case bodies. */
EMB_TEST(es6_tdz)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "x; let x = 1;", "ReferenceError");
    E6_THROWS(e, "{ y = 2; let y = 1; }", "ReferenceError");
    E6_THROWS(e, "{ z; let z; }", "ReferenceError");
    E6_THROWS(e, "function f1() { return w } f1(); let w = 1;", "ReferenceError");
    E6_THROWS(e, "var g = function () { return q }; g(); let q = 2;", "ReferenceError");
    E6_NUM(e, "var g2 = function () { return q2 }; let q2 = 2; g2()", 2);
    E6_THROWS(e, "switch (1) { case 0: let k = 1; case 1: k; }", "ReferenceError");
    E6_THROWS(e, "{ let t = t + 1; }", "ReferenceError");
    E6_THROWS(e, "typeof tdzv; let tdzv = 1;", "ReferenceError");
    E6_NUM(e, "{ let ok = 1; ok }", 1);
    es6_env_free(e);
}

/* const requires an initializer and cannot be assigned. */
EMB_TEST(es6_const_rules)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "const k = 1; k = 2;", "TypeError");
    E6_THROWS(e, "const k2 = 1; k2 += 2;", "TypeError");
    E6_THROWS(e, "const k3 = 1; k3++;", "TypeError");
    E6_SYNTAX(e, "const nope;");
    E6_SYNTAX(e, "for (const q; ;) {}");
    E6_NUM(e, "const obj = {a: 1}; obj.a = 2; obj.a", 2);
    es6_env_free(e);
}

/* Redeclaration rules (early errors). */
EMB_TEST(es6_redeclaration_errors)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "let r1 = 1; let r1 = 2;");
    E6_SYNTAX(e, "let r2; var r2;");
    E6_SYNTAX(e, "var r3; let r3;");
    E6_SYNTAX(e, "const r4 = 1; const r4 = 2;");
    E6_SYNTAX(e, "{ let r5; var r5; }");
    E6_SYNTAX(e, "{ let r6; { var r6; } }");
    E6_SYNTAX(e, "let r7; function r7() {}");
    E6_SYNTAX(e, "function r8(a) { let a; }");
    E6_SYNTAX(e, "switch (0) { case 1: let r9; case 2: let r9; }");
    E6_SYNTAX(e, "{ let r10; function r10() {} }");
    E6_SYNTAX(e, "try {} catch (r11) { let r11; }");
    /* allowed: separate scopes, var twice, sloppy duplicate functions */
    E6_TEXT(e, "{ let r12 = 1; } { let r12 = 2; } typeof r12", "undefined");
    E6_NUM(e, "var r13 = 1; var r13 = 2; r13", 2);
    E6_NUM(e, "function r14() {} function r14() { return 5 } r14()", 5);
    E6_NUM(e, "try {} catch (r15) { var r15 = 4; } 1", 1);
    es6_env_free(e);
}

/* let at the start of a statement: ASI, and let as a sloppy identifier. */
EMB_TEST(es6_let_asi_and_identifier)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "let\nasi = 7; asi", 7);
    E6_NUM(e, "var let = 3; let", 3);
    E6_NUM(e, "var let2 = 1; let2 + 1", 2);
    E6_SYNTAX(e, "let let = 1;");
    E6_SYNTAX(e, "if (true) let x = 1;");
    E6_SYNTAX(e, "while (false) const y = 1;");
    es6_env_free(e);
}

/* Labeled break and continue leave lexical scopes correctly. */
EMB_TEST(es6_labels_across_lexical_scopes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var r = 0; outer: for (var i = 0; i < 3; i++) { let a = i; for (;;) { let b = a; r += b; continue outer; } } r", 3);
    E6_NUM(e, "var s = 0; blk: { let x = 5; s = x; break blk; } s", 5);
    E6_NUM(e, "var c = 0; lp: for (let k = 0; k < 4; k++) { { let m = k; if (m == 2) break lp; c = m; } } c", 1);
    E6_NUM(e, "function lb() { lb2: { let z = 3; return z } } lb()", 3);
    es6_env_free(e);
}

/* finally runs for return, break and continue that leave lexical scopes. */
EMB_TEST(es6_finally_across_lexical_scopes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var log = 0; function fr() { { let a = 1; try { return a } finally { log = a + 1 } } } fr(); log", 2);
    E6_NUM(e, "var n = 0; for (var i = 0; i < 2; i++) { let v = i; try { continue } finally { n += v + 1 } } n", 3);
    E6_NUM(e, "var t = 0; x: { let w = 4; try { break x } finally { t = w } } t", 4);
    E6_NUM(e, "function fo() { let q = 1; try { throw 2 } catch (e) { let r = e; return r + q } finally { let f2 = 9; } } fo()", 3);
    es6_env_free(e);
}

/* for (let ...;;) gives each iteration its own binding (CreatePerIterationEnvironment). */
EMB_TEST(es6_for_let_per_iteration)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* closures see the value of their own iteration */
    E6_NUM(e, "var fs = []; for (let i = 0; i < 3; i++) { fs.push(function () { return i }) } fs[0]() + fs[1]() * 10 + fs[2]() * 100", 210);
    /* mutation in the body is carried into the next iteration and seen by that closure */
    E6_NUM(e, "var g = []; for (let j = 0; j < 4; j++) { g.push(() => j); j++ } g.length * 10 + g[0]()", 21);
    /* mutation in the update expression lands in the copy of the next iteration */
    E6_NUM(e, "var h = []; for (let k = 0; k < 6; k++, k++) { h.push(() => k) } h[0]() * 10 + h[1]()", 2);
    /* a closure made in the test sees the binding of the iteration that made it */
    E6_NUM(e, "var t = []; for (let m = 0; t.push(() => m), m < 2; m++) {} t[0]() * 10 + t[2]()", 2);
    /* the loop variable is fresh when the loop starts again */
    E6_NUM(e, "var outer = []; for (var o = 0; o < 2; o++) { for (let p = 0; p < 1; p++) outer.push(() => p) } outer[0]() + outer[1]()", 0);
    es6_env_free(e);
}

/* for (let x in/of ...) makes a fresh binding per iteration. */
EMB_TEST(es6_for_in_per_iteration)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var out = []; for (let key in {a: 1, b: 2}) { out.push(() => key) } out[0]() + out[1]()", "ab");
    E6_NUM(e, "var cnt = 0; for (const kk in {x: 1, y: 2, z: 3}) { cnt++ } cnt", 3);
    E6_NUM(e, "var fl = []; for (let q in {p: 1, r: 2}) { fl.push(function () { return q.length }) } fl.length", 2);
    E6_SYNTAX(e, "for (let a, b in {}) {}");
    es6_env_free(e);
}

/* Sloppy block functions are visible after the block (Annex B), strict ones are block-scoped. */
EMB_TEST(es6_block_functions)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "{ function bf() { return 3 } } bf()", 3);
    E6_TEXT(e, "typeof bq; { function bq() {} } typeof bq", "function");
    E6_TEXT(e, "'use strict'; { function sf() {} } typeof sf", "undefined");
    E6_NUM(e, "function ff() { { function inner() { return 8 } } return inner() } ff()", 8);
    E6_NUM(e, "var ab = 1; { function ab() {} } typeof ab === 'function' ? 2 : 3", 2);
    es6_env_free(e);
}

/* Every function of a block exists from the block's entry, so a block function can call one declared after it in the
 * same block, before the declaration is reached (the Falcon editor's script runs inside one try block). */
EMB_TEST(es6_block_functions_forward)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var r1; { function g1() { return typeof h1 } r1 = g1(); function h1() {} } r1", "function");
    E6_NUM(e, "var r2; try { function g2() { return h2() } r2 = g2(); function h2() { return 7 } } catch (x) {} r2", 7);
    E6_NUM(e, "var r3; { let a = 1; function g3() { return h3() + a } r3 = g3(); let k = 2; function h3() { return 5 } } r3", 6);
    E6_NUM(e, "function outer() { { function g4() { return h4() } var r = g4(); function h4() { return 9 } } return r } outer()",
           9);
    E6_TEXT(e, "var r5; { r5 = typeof h5; function h5() {} } r5", "function");
    E6_TEXT(e, "'use strict'; var r6; { function g6() { return typeof h6 } r6 = g6(); function h6() {} } r6", "function");
    /* Each entry of a loop body makes fresh bindings: the escaped closures see their own iteration's functions. */
    E6_NUM(e, "var fs = []; for (var i = 0; i < 2; i++) { let k = i; function a7() { return b7() } function b7() { return k } "
              "fs.push(a7) } fs[0]() * 10 + fs[1]()",
           1);
    E6_NUM(e, "var r8; switch (1) { case 1: function g8() { return h8() } r8 = g8(); function h8() { return 3 } } r8", 3);
    E6_NUM(e, "var r9; { function* g9() { yield h9() } r9 = g9().next().value; function h9() { return 4 } } r9", 4);
    E6_NUM(e, "var r10; { { function g10() { return h10() } r10 = g10(); } function h10() { return 6 } } r10", 6);
    E6_NUM(e, "var esc; { function ev(n) { return n ? od(n - 1) : 1 } function od(n) { return n ? ev(n - 1) : 0 } esc = ev } "
              "esc(4) * 10 + od(3)",
           11);
    E6_TEXT(e, "{ function g12() { return h12 } function h12() {} } typeof g12() + typeof h12", "functionfunction");
    es6_env_free(e);
}

/* A closure captures the binding of its block: each block entry makes a fresh one. */
EMB_TEST(es6_block_closures)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var fa = []; for (var i = 0; i < 2; i++) { let v = i * 10; fa.push(function () { return v }) } fa[0]() + fa[1]()", 10);
    E6_NUM(e, "function mk() { let c = 0; return function () { return ++c } } var m1 = mk(), m2 = mk(); m1(); m1(); m2() + m1()", 4);
    es6_env_free(e);
}


/* A const reached through a closure still cannot be assigned, in every assignment form. */
EMB_TEST(es6_const_assign_upvalue)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "function f(){ const y=1; return (()=>{ try { y=2 } catch(e){ return e.name } return 'none' })() } f()", "TypeError");
    E6_TEXT(e, "function f2(){ const y=1; return (()=>{ try { y+=2 } catch(e){ return e.name+y } return 'none' })() } f2()", "TypeError1");
    E6_TEXT(e, "function f3(){ const y=1; return (()=>{ try { y++ } catch(e){ return e.name+y } return 'none' })() } f3()", "TypeError1");
    E6_TEXT(e, "function f4(){ const y=1; return (()=>{ try { --y } catch(e){ return e.name+y } return 'none' })() } f4()", "TypeError1");
    E6_TEXT(e, "function f5(){ const y=1; return (()=>{ try { y&&=2 } catch(e){ return e.name+y } return 'none' })() } f5()", "TypeError1");
    E6_TEXT(e, "function f6(){ const y=0; return (()=>{ try { y||=2 } catch(e){ return e.name+y } return 'none' })() } f6()", "TypeError0");
    E6_TEXT(e, "function f7(){ const y=1; return (()=>{ y||=2; return 'ok' + y })() } f7()", "ok1");
    E6_TEXT(e, "var r=[]; for (let i=0;i<2;i++){ const c=i; r.push(()=>{ try { c=9 } catch(e){ return e.name+c } }) } r[0]()+r[1]()", "TypeError0TypeError1");
    E6_TEXT(e, "var o={a:1}, fs=[]; for (const k in o){ fs.push(()=>{ try { k='z' } catch(e){ return e.name+k } }) } fs[0]()", "TypeErrora");
    E6_TEXT(e, "'use strict'; function f8(){ const y=1; return (()=>{ try { y=2 } catch(e){ return e.name+y } return 'none' })() } f8()", "TypeError1");
    E6_TEXT(e, "function f9(){ var g=()=>{ try { y=2 } catch(e){ return e.name } return 'none' }; var r=g(); const y=1; return r } f9()", "ReferenceError");
    E6_TEXT(e, "function f10(){ var g=()=>{ try { y+=2 } catch(e){ return e.name } }; var r=g(); const y=1; return r } f10()", "ReferenceError");
    E6_TEXT(e, "function f11(){ const y=1; function g(){ try { y=2 } catch(e){ return e.name } return 'none' } return g() } f11()", "TypeError");
    es6_env_free(e);
}

/* let in a single-statement position is an identifier expression statement (ASI applies). */
EMB_TEST(es6_let_single_statement)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "if (1) let\nx", "ReferenceError");
    E6_NUM(e, "var n=4; while (0) let\n{}\n n", 4);
    E6_THROWS(e, "do let\nwhile(0)", "ReferenceError");
    E6_NUM(e, "var let=7; do let\nwhile(0)", 7);
    E6_NUM(e, "var let=3; l: let\n3", 3);
    E6_THROWS(e, "l: let\nx", "ReferenceError");
    E6_NUM(e, "var n2=4, let=1; for (;false;) let\n{}\n n2", 4);
    E6_NUM(e, "var n3=5, let=1; for (var k in {a:1}) let\n{}\n n3", 5);
    E6_BOOL(e, "var let=1, o={}; if (1) let in o", false);
    E6_BOOL(e, "var let=1; if (1) let instanceof Object", false);
    E6_SYNTAX(e, "if (1) let [a] = [1];");
    E6_SYNTAX(e, "if (1) let x = 1;");
    E6_SYNTAX(e, "while (0) let\n[a] = 1;");
    E6_NUM(e, "let\nz = 3; z", 3);
    es6_env_free(e);
}

/* with honors Symbol.unscopables. */
EMB_TEST(es6_with_unscopables)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var o={a:1,[Symbol.unscopables]:{a:true}}; var a=5; with(o){ a }", 5);
    E6_BOOL(e, "var o2={a:1,[Symbol.unscopables]:{a:true}}; var a2=5; with(o2){ a2 = 7 } a2===7 && o2.a===1", true);
    E6_NUM(e, "var o3={b:1,[Symbol.unscopables]:{c:true}}; var c=9; with(o3){ b + c }", 10);
    E6_NUM(e, "var o4={d:2,[Symbol.unscopables]:{d:false}}; with(o4){ d }", 2);
    E6_NUM(e, "var o5={e:3,[Symbol.unscopables]:{e:true}}; var e=8; with(o5){ (function(){ return e })() }", 8);
    es6_env_free(e);
}

EMB_TEST_MAIN()
