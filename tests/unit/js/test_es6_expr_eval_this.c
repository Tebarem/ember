/* Arrow functions and eval, optional-call receivers, and Function source text (J2a section 2):
 * a direct eval inside an arrow function sees the enclosing this, arguments and new.target;
 * (a?.b)() keeps its receiver; Function.prototype.toString returns the exact source slice. */
#include "es6_util.h"

EMB_TEST(es6_eval_in_arrow_this)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var o = { f() { return (() => eval('this'))() } }; o.f() === o", true);
    E6_BOOL(e, "var o2 = { f() { return (() => eval('(() => this)()'))() } }; o2.f() === o2", true);
    E6_BOOL(e, "var o3 = { f() { return (() => (() => eval('this'))())() } }; o3.f() === o3", true);
    E6_BOOL(e, "var o4 = { f() { return (() => eval('eval(\"this\")'))() } }; o4.f() === o4", true);
    E6_NUM(e, "function t1() { return (() => eval('this.v'))() } t1.call({ v: 6 })", 6);
    /* An arrow at the top of a script has the global this. */
    E6_BOOL(e, "(() => eval('this'))() === globalThis", true);
    /* Strict code: this is not coerced. */
    E6_BOOL(e, "function t2() { 'use strict'; return (() => eval('this'))() } t2.call(5) === 5", true);
    es6_env_free(e);
}

EMB_TEST(es6_eval_in_arrow_arguments)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "function a1() { return (() => eval('arguments[0]'))() } a1(7)", 7);
    E6_NUM(e, "function a2() { return (() => eval('arguments.length'))() } a2(1, 2, 3)", 3);
    E6_NUM(e, "function a3() { return (() => (() => eval('arguments[1]'))())() } a3(1, 8)", 8);
    E6_NUM(e, "function a4() { return eval('(() => arguments[0])()') } a4(9)", 9);
    /* The arrow's own parameters shadow nothing named arguments; the enclosing object is used. */
    E6_NUM(e, "function a5(x) { return ((y) => eval('arguments[0] + y'))(10) } a5(1)", 11);
    es6_env_free(e);
}

EMB_TEST(es6_eval_new_target)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function n1() { return eval('new.target') } new n1() === n1", true);
    E6_BOOL(e, "function n2() { return eval('new.target') } n2() === undefined", true);
    E6_BOOL(e, "function n3() { return (() => eval('new.target'))() } new n3() === n3", true);
    E6_BOOL(e, "function n4() { return eval('(() => new.target)()') } new n4() === n4", true);
    E6_BOOL(e, "function n5() { return eval('eval(\"new.target\")') } new n5() === n5", true);
    E6_BOOL(e, "function n6() { return (() => (() => new.target)())() } new n6() === n6", true);
    /* Outside any function the early errors apply, also for arrows and indirect eval. */
    E6_SYNTAX(e, "eval('new.target')");
    E6_SYNTAX(e, "(0, eval)('new.target')");
    E6_SYNTAX(e, "(() => new.target)");
    E6_SYNTAX(e, "(() => eval('new.target'))()");
    E6_SYNTAX(e, "new.target");
    E6_SYNTAX(e, "function ok() { (0, eval)('new.target') } ok()");
    es6_env_free(e);
}

/* Evaluating a var in a parameter default, and eval var conflicts with lexical names. */
EMB_TEST(es6_eval_declaration_conflicts)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_SYNTAX(e, "(function (p = eval('var arguments')) {})()");
    E6_SYNTAX(e, "function f(a = eval('var a = 1')) {} f()");
    E6_SYNTAX(e, "var g = () => { let x; eval('var x'); }; g()");
    E6_SYNTAX(e, "(function () { { let y; { eval('var y'); } } })()");
    E6_SYNTAX(e, "(function () { let z; eval('var z'); })()");
    E6_NUM(e, "(function () { try { throw 1 } catch (c) { eval('var c = 5'); } return 1 })()", 1);
    E6_NUM(e, "(function () { var w = 1; eval('var w = 2'); return w })()", 2);
    E6_NUM(e, "(function (p = eval('var q1 = 4; q1')) { return p })()", 4);
    es6_env_free(e);
}

EMB_TEST(es6_parenthesized_optional_call)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "const a = { b() { return this._b }, _b: { c: 42 } }; (a?.b)().c", 42);
    E6_NUM(e, "(a?.b)?.().c", 42);
    E6_NUM(e, "(a?.['b'])().c", 42);
    E6_NUM(e, "(a.b)().c", 42);
    E6_NUM(e, "a.b?.().c", 42);
    E6_NUM(e, "a?.b().c", 42);
    E6_NUM(e, "((a?.b))().c", 42);
    E6_NUM(e, "(a?.b.constructor === Function) ? 1 : 0", 1);
    /* Short circuits: the call target is undefined, so a plain call throws, an optional one not. */
    E6_THROWS(e, "var nn = null; (nn?.b)()", "TypeError");
    E6_BOOL(e, "(nn?.b)?.() === undefined", true);
    E6_BOOL(e, "nn?.b() === undefined", true);
    E6_BOOL(e, "typeof (nn?.b) === 'undefined'", true);
    /* A parenthesized chain ends the short circuit: the next link runs on undefined. */
    E6_THROWS(e, "(nn?.b).c", "TypeError");
    E6_BOOL(e, "nn?.b.c === undefined", true);
    E6_BOOL(e, "(nn?.b ?? 5) === 5", true);
    E6_NUM(e, "var zz = (nn?.b, 3); zz", 3);
    /* It is not an assignment target. */
    E6_SYNTAX(e, "(a?.b) = 1");
    E6_SYNTAX(e, "(a?.b)++");
    E6_SYNTAX(e, "a?.b = 1");
    es6_env_free(e);
}

EMB_TEST(es6_function_source_text)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "new Function('a', 'b', 'return a').toString()",
            "function anonymous(a,b\n) {\nreturn a\n}");
    E6_TEXT(e, "new Function().toString()", "function anonymous(\n) {\n\n}");
    E6_TEXT(e, "Function('return 1').toString()", "function anonymous(\n) {\nreturn 1\n}");
    E6_TEXT(e, "(new Function('x', 'return x')).name", "anonymous");
    E6_NUM(e, "new Function('a', 'b', 'return a + b')(1, 2)", 3);
    /* The name anonymous binds nothing inside the body. */
    E6_TEXT(e, "typeof new Function('return typeof anonymous')()", "string");
    E6_TEXT(e, "new Function('return typeof anonymous')()", "undefined");
    E6_TEXT(e, "(function  foo ( a ,b )  { return a }).toString()", "function  foo ( a ,b )  { return a }");
    E6_TEXT(e, "(a => a  +  1).toString()", "a => a  +  1");
    E6_TEXT(e, "((a, b)=>{ return a }).toString()", "(a, b)=>{ return a }");
    E6_TEXT(e, "({ m( x ) { return x } }).m.toString()", "m( x ) { return x }");
    E6_TEXT(e, "({ get  g () { return 1 } }).__lookupGetter__('g').toString()",
            "get  g () { return 1 }");
    E6_TEXT(e, "({ ['c' + 1]( ) { } }).c1.toString()", "['c' + 1]( ) { }");
    E6_TEXT(e, "function outerf() { function inner() { return 1 } return inner } outerf().toString()",
            "function inner() { return 1 }");
    E6_TEXT(e, "outerf.toString()",
            "function outerf() { function inner() { return 1 } return inner }");
    E6_TEXT(e, "Math.max.toString().indexOf('native code') > 0", "true");
    es6_env_free(e);
}

EMB_TEST_MAIN()
