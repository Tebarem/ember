/* ES2015 function parameters (J2a §2): default and rest parameters, length, parameter lists with
 * a trailing comma, and the simple parameter list rules for "use strict". */
#include "es6_util.h"

/* Default parameters: evaluated only for undefined, left to right, with earlier parameters in scope. */
EMB_TEST(es6_default_parameters)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function d(a, b = a + 1) { return b } d(1)", 2);
    E6_NUM(e, "function d2(a, b = a + 1) { return b } d2(1, 5)", 5);
    E6_BOOL(e, "function d3(a = 5) { return a } d3(null) === null", true);
    E6_NUM(e, "function d4(a = 5) { return a } d4(undefined)", 5);
    E6_NUM(e, "var calls = 0; function side() { calls++; return 4 } function d5(a = side()) { return a } d5(); d5(1); calls", 1);
    E6_NUM(e, "function d6(a = 1) { var a; return a } d6()", 1);
    E6_NUM(e, "function d7(a, b = arguments.length) { return b } d7(1)", 1);
    E6_NUM(e, "function d8(a, b = () => a) { a = 2; return b() } d8(1)", 2);
    E6_NUM(e, "var dd = (x = 3, y = x * 2) => x + y; dd()", 9);
    E6_THROWS(e, "function dt(a = (function () { throw new RangeError('x') })()) {} dt();", "RangeError");
    E6_NUM(e, "function dz(a = 0) { return arguments.length } dz()", 0);
    E6_NUM(e, "function dl(a, b = 2, c) { return c } dl(1, undefined, 3)", 3);
    es6_env_free(e);
}

/* Rest parameters: an array of the remaining arguments; it must be the last parameter. */
EMB_TEST(es6_rest_parameters)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function r(a, ...b) { return b.length } r(1, 2, 3)", 2);
    E6_NUM(e, "function r2(...b) { return b[0] } r2(7)", 7);
    E6_NUM(e, "(function (...b) { return b.length })()", 0);
    E6_BOOL(e, "function r3(...b) { return Array.isArray(b) } r3()", true);
    E6_NUM(e, "function r4(a, ...b) { return arguments.length } r4(1, 2, 3)", 3);
    E6_NUM(e, "function r5(a, ...b) { return a + b.length } r5(10)", 10);
    E6_NUM(e, "function r6(a = 1, ...b) { return a * 10 + b.length } r6(undefined, 9, 9)", 12);
    E6_NUM(e, "function r7(...b) { b.push(1); return b.length } r7(2)", 2);
    E6_NUM(e, "var rr = (...xs) => xs.length; rr(1, 2)", 2);
    E6_SYNTAX(e, "function bad(...a, b) {}");
    E6_SYNTAX(e, "function bad2(...a,) {}");
    E6_SYNTAX(e, "var bad3 = (...a, b) => 1;");
    es6_env_free(e);
}

/* The length property counts the parameters before the first default or rest parameter. */
EMB_TEST(es6_function_length)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "(function (a, b = 1, c) {}).length", 1);
    E6_NUM(e, "(function (...a) {}).length", 0);
    E6_NUM(e, "(function (a, ...b) {}).length", 1);
    E6_NUM(e, "(function (a, b,) {}).length", 2);
    E6_NUM(e, "((a, b) => 1).length", 2);
    E6_NUM(e, "((a = 1) => 1).length", 0);
    es6_env_free(e);
}

/* Trailing commas in parameter and argument lists. */
EMB_TEST(es6_trailing_commas)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function tc(a, b,) { return a + b } tc(1, 2,)", 3);
    E6_NUM(e, "var tca = (a, b,) => a * b; tca(3, 4)", 12);
    E6_NUM(e, "Math.max(1, 5,)", 5);
    es6_env_free(e);
}

/* A "use strict" directive is an early error in a function with a non-simple parameter list. */
EMB_TEST(es6_use_strict_non_simple_params)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "function sp(a = 1) { 'use strict'; }");
    E6_SYNTAX(e, "function sr(...a) { 'use strict'; }");
    E6_SYNTAX(e, "function sd(a, b = a) { 'use strict' }");
    E6_SYNTAX(e, "var sa = (a = 1) => { 'use strict'; };");
    E6_SYNTAX(e, "var sb = ({a}) => { 'use strict'; };");
    E6_NUM(e, "var sbp = ({a}) => a; sbp({a: 4})", 4);
    E6_NUM(e, "function ok(a, b) { 'use strict'; return a + b } ok(1, 2)", 3);
    E6_BOOL(e, "function okd(a = 1) { return this === undefined } okd()", false);
    E6_NUM(e, "function oks(a) { 'use strict'; return a } oks(3)", 3);
    es6_env_free(e);
}

/* Arrow parameter lists: no duplicates, and strict-only name checks apply to them. */
EMB_TEST(es6_arrow_parameter_rules)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "var ad = (a, a) => 1;");
    E6_SYNTAX(e, "'use strict'; var ae = (eval) => 1;");
    E6_SYNTAX(e, "'use strict'; var af = (a, a) => 1;");
    E6_NUM(e, "function outer() { return ((a) => a * 2)(4) } outer()", 8);
    es6_env_free(e);
}

EMB_TEST_MAIN()
