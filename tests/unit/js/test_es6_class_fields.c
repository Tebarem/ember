/* Classes (J2d): public instance and static fields, computed field names, field initializer order,
 * the this and function naming in initializers, static blocks, and fields of derived classes. */
#include "es6_util.h"

EMB_TEST(class_fields_basic)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class F { x = 1; y = this.x + 1; static s = 10; ['c' + 'k'] = 5; }"
              " var f = new F(); f.x * 1000 + f.y * 100 + F.s * 10 + f.ck", 1305);
    E6_TEXT(e, "var order = ''; class O { a = (order += 'a', 1); constructor() { order += 'c'; }"
               " b = (order += 'b', 2); } new O(); order", "abc");
    E6_NUM(e, "class Base { z = 1; } class Der extends Base { w = this.z + 1;"
              " constructor() { super(); this.v = this.w + 10; } } var d = new Der();"
              " d.z * 100 + d.w * 10 + d.v", 132);
    E6_NUM(e, "class Cnt { static n = 0; static { Cnt.n = 5; } static { Cnt.n += 2; } } Cnt.n", 7);
    E6_TEXT(e, "class SB { static s1 = 'a'; static { this.s2 = this.s1 + 'b'; } } SB.s2", "ab");
    E6_BOOL(e, "class Ar { f = () => this; } var ar = new Ar(); ar.f() === ar", true);
    E6_TEXT(e, "class NM { f = function () {}; g = () => 1; } new NM().f.name + '|' + new NM().g.name",
            "f|g");
    E6_BOOL(e, "class Own { x = 1; } Object.keys(new Own()).indexOf('x') === 0", true);
    E6_BOOL(e, "class Nf { x = 1; } Object.getOwnPropertyDescriptor(new Nf(), 'x').writable", true);
    E6_BOOL(e, "class Nd { x; } 'x' in new Nd() && new Nd().x === undefined", true);
    E6_NUM(e, "class Sf { static a = 1; static b = Sf.a + 1; } Sf.b", 2);
    E6_TEXT(e, "class Ss { static ['x' + 1] = 'v'; } Ss.x1", "v");
    E6_NUM(e, "var k = 0; class Ck { [(k++, 'p')] = k; } new Ck().p * 10 + k", 11);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_fields_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class Fa { x = arguments; }", "SyntaxError");
    E6_THROWS(e, "class Fb { static { arguments; } }", "SyntaxError");
    E6_THROWS(e, "class Fc { constructor = 1; }", "SyntaxError");
    E6_THROWS(e, "class Fd { static prototype = 1; }", "SyntaxError");
    E6_THROWS(e, "class Fe { static { return; } }", "SyntaxError");
    E6_THROWS(e, "class Ff { x = 1; y }; new Ff().y.z", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_static_block_await)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* ClassStaticBlockBody has [+Await]: await is neither a reference nor a binding there. */
    E6_SYNTAX(e, "class A1 { static { await; } }");
    E6_SYNTAX(e, "class A2 { static { var await; } }");
    E6_SYNTAX(e, "class A3 { static { let await; } }");
    E6_SYNTAX(e, "class A4 { static { const await = 0; } }");
    E6_SYNTAX(e, "class A5 { static { function await() {} } }");
    E6_SYNTAX(e, "class A6 { static { class await {} } }");
    E6_SYNTAX(e, "class A7 { static { (class await {}); } }");
    E6_SYNTAX(e, "class A8 { static { (await => 0); } }");
    E6_SYNTAX(e, "class A9 { static { ((x = await) => 0); } }");
    E6_SYNTAX(e, "class B1 { static { ({ await }); } }");
    E6_SYNTAX(e, "class B2 { static { try {} catch (await) {} } }");
    E6_SYNTAX(e, "class B3 { static { await: 1; } }");
    E6_SYNTAX(e, "class B4 { static { var { await } = {}; } }");
    E6_SYNTAX(e, "class B5 { static { for (var await of []) {} } }");
    /* An arrow body or an ordinary function nested in the block is not [+Await]. */
    E6_NUM(e, "var r = 0; class C1 { static { (function await(await) { r += 1; })(); } } r", 1);
    E6_NUM(e, "var r2 = 0; class C2 { static { (() => { var await = 2; r2 = await; })(); } } r2", 2);
    E6_NUM(e, "var r3 = 0; class C3 { static { (() => { class await {} r3 = 3; })(); } } r3", 3);
    E6_NUM(e, "var r4 = 0; class C4 { static { function f() { var await = 4; return await; } r4 = f(); } } r4", 4);
    E6_NUM(e, "var await = 5, r5; class C5 { static { new (class { constructor(x = await) { r5 = x; } }); } } r5", 5);
    E6_NUM(e, "var await = 6; (class { static m() { return await; } }).m()", 6);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_field_define_throws)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Fields use CreateDataPropertyOrThrow. */
    E6_THROWS(e, "class F1 { static ['prototype'] = 1; }", "TypeError");
    E6_THROWS(e, "class B2 { constructor(o) { return o; } } class C2 extends B2 { x = 1; }"
                 " new C2(Object.freeze({}))", "TypeError");
    E6_THROWS(e, "class B3 { constructor(o) { return o; } } class C3 extends B3 { x = 5; }"
                 " new C3(Object.defineProperty({}, 'x', { value: 1 }))", "TypeError");
    E6_NUM(e, "class B4 { constructor(o) { return o; } } class C4 extends B4 { x = 5; }"
              " new C4(Object.defineProperty({}, 'x', { value: 1, configurable: true })).x", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_field_eval_arguments)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class C1 { x = eval('arguments'); } new C1()", "SyntaxError");
    E6_THROWS(e, "class C2 { x = () => eval('arguments'); } new C2().x()", "SyntaxError");
    E6_THROWS(e, "class C3 { x = eval(\'eval(\\'arguments\\')\'); } new C3()", "SyntaxError");
    E6_THROWS(e, "class C4 { static { eval('arguments'); } }", "SyntaxError");
    E6_THROWS(e, "class C5 { #p = eval('arguments'); } new C5()", "SyntaxError");
    /* A function inside the eval code has its own arguments. */
    E6_NUM(e, "class C6 { x = eval('(function () { return arguments.length; })(1, 2)'); } new C6().x", 2);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
