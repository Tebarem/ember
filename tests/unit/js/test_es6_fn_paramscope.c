/* Function parameters with default values (J2a section 2, FunctionDeclarationInstantiation step
 * 28): the parameters live in a scope of their own, in TDZ until initialized, and the body's vars
 * live in a second scope that closures created in the defaults cannot see. */
#include "es6_util.h"

/* A default that reads a later parameter, or the parameter being initialized, hits its TDZ. */
EMB_TEST(es6_param_tdz)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "function f1(a = b, b) {} f1()", "ReferenceError");
    E6_THROWS(e, "function f2(a = a) {} f2()", "ReferenceError");
    E6_THROWS(e, "function f3(a = b, b = 1) {} f3()", "ReferenceError");
    E6_THROWS(e, "(function (a = (() => b)(), b) {})()", "ReferenceError");
    E6_THROWS(e, "((a = b, b) => 1)()", "ReferenceError");
    E6_THROWS(e, "({ m(a = b, b) {} }).m()", "ReferenceError");
    /* Earlier parameters are fine, a closure called later sees the initialized binding. */
    E6_NUM(e, "function g1(a, b = a + 1) { return b } g1(4)", 5);
    E6_NUM(e, "function g2(a = () => b, b = 7) { return a() } g2()", 7);
    E6_NUM(e, "function g3(a = 1, b = a) { return b } g3()", 1);
    /* An argument passed for the later parameter does not end its TDZ for the earlier default. */
    E6_THROWS(e, "function h1(a = b, b) {} h1(undefined, 2)", "ReferenceError");
    E6_NUM(e, "function h2(a = b, b) { return a } h2(5, 2)", 5);
    /* typeof of a parameter in TDZ throws too. */
    E6_THROWS(e, "function t1(a = typeof b, b) {} t1()", "ReferenceError");
    /* After the list all parameters are initialized, also those that were never passed. */
    E6_NUM(e, "function t2(a = 1, b) { return typeof b === 'undefined' ? 1 : 0 } t2()", 1);
    es6_env_free(e);
}

/* Closures created in a default see the parameters, not the body's vars. */
EMB_TEST(es6_param_scope_is_separate_from_body)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var x = 'outer'; function s1(a = () => x) { var x = 'inner'; return a() } s1()", "outer");
    E6_NUM(e, "function s2(a, b = () => a) { var a = 9; return b() } s2(1)", 1);
    E6_NUM(e, "function s3(a, b = () => a) { var a = 9; return a } s3(1)", 9);
    /* A body var of a parameter's name starts with the parameter's value. */
    E6_NUM(e, "function s4(a = 3) { var a; return a } s4()", 3);
    E6_NUM(e, "function s5(a = 3) { var a; return a } s5(8)", 8);
    E6_NUM(e, "function s6(a, b = () => a) { var a; a = 5; return b() + a } s6(2)", 7);
    /* A function declaration of the same name replaces the parameter value in the body. */
    E6_TEXT(e, "function s7(a = 1) { function a() {} return typeof a } s7()", "function");
    E6_NUM(e, "function s8(a = () => typeof a2, a2 = 1) { var a2 = 'x'; return a() === 'number' ? 1 : 0 } s8()", 1);
    /* Without a default the parameters and the body share one scope, as before. */
    E6_NUM(e, "function s9(a, b = 1) { var a = 4; return a } s9(1)", 4);
    E6_NUM(e, "var y = 1; function s10(a = () => y) { var y = 2; return a() } s10()", 1);
    /* Each call gets its own bindings. */
    E6_NUM(e, "function s11(a, f = () => a) { return f } var k1 = s11(1), k2 = s11(2); k1() * 10 + k2()", 12);
    /* arguments is visible in the defaults and unmapped. */
    E6_NUM(e, "function s12(a = 1, b = arguments.length) { return b } s12(undefined, undefined, 0)", 3);
    E6_NUM(e, "function s13(a = 1) { arguments[0] = 9; return a } s13(2)", 2);
    /* A lexical name of the body may not repeat a parameter. */
    E6_SYNTAX(e, "function d1(a = 1) { let a; }");
    E6_SYNTAX(e, "function d2(a, b = 1) { const b = 2; }");
    E6_NUM(e, "function d3(a = 1) { { let a = 2; } return a } d3()", 1);
    E6_NUM(e, "function d4(a = 1) { var a = 2; return a } d4()", 2);
    es6_env_free(e);
}

/* Evaluation order, once-only evaluation and the length property. */
EMB_TEST(es6_param_default_evaluation)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var log = ''; function o(a = (log += 'a', 1), b = (log += 'b', 2)) { return a + b } o(undefined, 5); log", "a");
    E6_NUM(e, "function l1(a, b = 1, c) {} l1.length", 1);
    E6_NUM(e, "var l2 = (a, b = 1) => 0; l2.length", 1);
    E6_NUM(e, "function r1(a = 1, ...r) { return r.length } r1(undefined, 1, 2)", 2);
    E6_NUM(e, "function r2(a, ...r) { return r.length } r2()", 0);
    E6_NUM(e, "function c1(a = 1) { return (() => a)() } c1(3)", 3);
    E6_NUM(e, "function c2(a = 1) { a = 5; return (() => a)() } c2()", 5);
    E6_NUM(e, "var c3 = function (a = 1) { return typeof c3 === 'function' ? a : 0 }; c3()", 1);
    /* A named function expression's own name is visible in a default. */
    E6_NUM(e, "var c4 = function nm(a = typeof nm === 'function' ? 4 : 0) { return a }; c4()", 4);
    es6_env_free(e);
}

EMB_TEST_MAIN()
