/* Early errors of lexical scoping and parameter lists (J2a sections 1 and 2): redeclarations
 * between let, const, var, functions, parameters and catch parameters, block-level functions in
 * sloppy and strict code, duplicate parameters, and labelled function declarations. */
#include "es6_util.h"

/* A block function and a var of the same name in one block (or a nested one) are an error. */
EMB_TEST(es6_block_function_vs_var)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "{ function a() {} var a; }");
    E6_SYNTAX(e, "{ var a; function a() {} }");
    E6_SYNTAX(e, "{ function a() {} { var a; } }");
    E6_SYNTAX(e, "{ { var a; } function a() {} }");
    E6_SYNTAX(e, "{ let a; var a; }");
    E6_SYNTAX(e, "{ var a; let a; }");
    E6_SYNTAX(e, "{ { var a; } let a; }");
    E6_SYNTAX(e, "{ function a() {} let a; }");
    E6_SYNTAX(e, "{ let a; function a() {} }");
    E6_SYNTAX(e, "{ const a = 1; function a() {} }");
    E6_SYNTAX(e, "switch (0) { case 1: function a() {} default: var a; }");
    E6_SYNTAX(e, "switch (0) { case 1: let a; default: let a; }");
    E6_SYNTAX(e, "switch (0) { case 1: let a; default: var a; }");
    E6_SYNTAX(e, "for (let a;;) { var a; }");
    /* Allowed shapes. */
    E6_NUM(e, "{ function a1() {} { var b1 = 1; } } b1", 1);
    E6_NUM(e, "var c1 = 1; { let c1 = 2; } c1", 1);
    E6_NUM(e, "{ let d1 = 1; { let d1 = 2; } } 3", 3);
    E6_NUM(e, "{ function e1() { return 1 } } e1()", 1);
    E6_NUM(e, "{ { function f1() { return 2 } } function f1() { return 3 } } f1()", 3);
    es6_env_free(e);
}

/* Two functions of one name in a block: a sloppy-mode allowance only. */
EMB_TEST(es6_block_function_duplicates)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "'use strict'; { function a() {} function a() {} }");
    E6_SYNTAX(e, "function f() { 'use strict'; { function a() {} function a() {} } }");
    E6_SYNTAX(e, "'use strict'; switch (0) { case 1: function a() {} default: function a() {} }");
    E6_NUM(e, "{ function a() { return 1 } function a() { return 2 } } a()", 2);
    E6_NUM(e, "'use strict'; { function a2() {} } typeof a2 === 'undefined' ? 1 : 0", 1);
    E6_NUM(e, "'use strict'; { function a3() { return 4 } a3() }", 4);
    /* A top-level function and var of one name are fine. */
    E6_NUM(e, "function t() { return 1 } var t; t()", 1);
    E6_SYNTAX(e, "function u() {} let u;");
    E6_SYNTAX(e, "let u2; function u2() {}");
    es6_env_free(e);
}

/* The catch parameter and the catch block: a lexical name of the block may not repeat it. */
EMB_TEST(es6_catch_redeclaration)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "try {} catch (e) { function e() {} }");
    E6_SYNTAX(e, "try {} catch (e) { let e; }");
    E6_SYNTAX(e, "try {} catch (e) { const e = 1; }");
    E6_NUM(e, "try { throw 1 } catch (e) { var e = 2; } 5", 5);
    E6_NUM(e, "try { throw 1 } catch (e) { { let e = 4; } } 6", 6);
    E6_NUM(e, "try { throw 1 } catch (e) { { function e2() {} } } 7", 7);
    E6_NUM(e, "try { throw 3 } catch { 8 }", 8);
    E6_NUM(e, "var cc = 0; try { throw 1 } catch { cc = 2 } finally { cc++ } cc", 3);
    es6_env_free(e);
}

/* Duplicate parameter names: allowed only in the plain sloppy function, never with defaults or
 * rest, in methods, accessors, arrows, or strict code. */
EMB_TEST(es6_duplicate_parameters)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "function f(a = 1, a) {}");
    E6_SYNTAX(e, "function f(a, a = 1) {}");
    E6_SYNTAX(e, "function f(a, ...a) {}");
    E6_SYNTAX(e, "(function (a, a = 1) {})");
    E6_SYNTAX(e, "({ m(a, a) {} })");
    E6_SYNTAX(e, "({ m(a = 1, a) {} })");
    E6_SYNTAX(e, "({ set x(a, a) {} })");
    E6_SYNTAX(e, "((a, a) => 1)");
    E6_SYNTAX(e, "(a, a) => 1");
    E6_SYNTAX(e, "'use strict'; function f(a, a) {}");
    E6_SYNTAX(e, "function f(a, a) { 'use strict'; }");
    E6_NUM(e, "function dp(a, a) { return a } dp(1, 2)", 2);
    E6_NUM(e, "(function (a, a) { return a })(1, 2)", 2);
    E6_NUM(e, "function dq(a, b, a) { return b } dq(1, 5, 3)", 5);
    es6_env_free(e);
}

/* Setters take exactly one plain parameter. */
EMB_TEST(es6_setter_parameters)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "({ set x(...a) {} })");
    E6_SYNTAX(e, "({ set x() {} })");
    E6_SYNTAX(e, "({ set x(a, b) {} })");
    E6_SYNTAX(e, "({ get x(a) {} })");
    E6_NUM(e, "var so = { set x(a = 5) { this.v = a } }; so.x = undefined; so.v", 5);
    E6_NUM(e, "var sp = { set x(a) { this.v = a } }; sp.x = 3; sp.v", 3);
    es6_env_free(e);
}

/* A labelled function declaration is not a statement of a loop, if or with body. */
EMB_TEST(es6_labelled_function_positions)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "while (0) l: function f() {}");
    E6_SYNTAX(e, "do l: function f() {} while (0)");
    E6_SYNTAX(e, "for (;0;) l: function f() {}");
    E6_SYNTAX(e, "for (var x in {}) l: function f() {}");
    E6_SYNTAX(e, "if (1) l: function f() {}");
    E6_SYNTAX(e, "if (0) ; else l: function f() {}");
    E6_SYNTAX(e, "with ({}) l: function f() {}");
    E6_SYNTAX(e, "while (0) a: b: function f() {}");
    E6_NUM(e, "l1: function lf() { return 1 } lf()", 1);
    E6_NUM(e, "{ l2: function lg() { return 2 } } lg()", 2);
    E6_NUM(e, "var n = 0; l3: while (n < 2) { n++; continue l3; } n", 2);
    es6_env_free(e);
}

/* let as an identifier in sloppy code, and const without an initializer. */
EMB_TEST(es6_lexical_declaration_syntax)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "const x;");
    E6_SYNTAX(e, "for (const i;;) {}");
    E6_SYNTAX(e, "let let = 1;");
    E6_SYNTAX(e, "let a, a;");
    E6_SYNTAX(e, "if (1) let x = 1;");
    E6_SYNTAX(e, "while (0) const y = 1;");
    E6_NUM(e, "var let = 5; let", 5);
    E6_NUM(e, "var lt; lt = let\n+ 1; 0", 0);
    es6_env_free(e);
}

/* review round 4: for (let in ...), eval stores to a self-name, bounded spread. */
EMB_TEST(es6_round4_fixes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var let; var n = 0; for (let in {a: 1, b: 2}) n++; n", 2);
    E6_SYNTAX(e, "for (let of []) ;");
    E6_TEXT(e, "(function f() { eval('f = 2'); return typeof f })()", "function");
    E6_TEXT(e, "(function f() { 'use strict'; try { eval('f = 2') } catch (x) { return x.name } })()",
            "TypeError");
    E6_THROWS(e, "Math.max(...{length: 4294967296})", "TypeError");
    E6_THROWS(e, "[...{length: 4294967296}]", "TypeError");
    E6_THROWS(e, "Math.max(...{length: 2e9})", "TypeError");
    es6_env_free(e);
}

EMB_TEST_MAIN()
