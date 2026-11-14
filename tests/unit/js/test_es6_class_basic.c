/* Classes (J2d): declarations and expressions, constructors, methods, accessors, static members,
 * computed names, the class strict body, the inner name binding, and the call-without-new error. */
#include "es6_util.h"

EMB_TEST(class_basic_decl)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class A { constructor(x) { this.x = x; } get double() { return this.x * 2; }"
              " m() { return this.x + 1; } static s() { return 7; } }"
              " var a = new A(4); a.double * 100 + a.m() * 10 + A.s()", 857);
    E6_BOOL(e, "class B {} new B() instanceof B", true);
    E6_TEXT(e, "class C {} typeof C", "function");
    E6_TEXT(e, "class D {} D.name", "D");
    E6_TEXT(e, "(class {}).name", "");
    E6_TEXT(e, "var Anon = class {}; Anon.name", "Anon");
    E6_NUM(e, "class L { constructor(a, b) {} } L.length", 2);
    E6_THROWS(e, "class NC {} NC()", "TypeError");
    E6_BOOL(e, "class P2 { m() {} } !('prototype' in P2.prototype.m)", true);
    E6_BOOL(e, "class Q { m() {} } Object.getOwnPropertyDescriptor(Q.prototype, 'm').enumerable", false);
    E6_BOOL(e, "class Q2 { m() {} } Object.getOwnPropertyDescriptor(Q2.prototype, 'm').writable", true);
    E6_NUM(e, "var cnt = 0; class S { ['m' + 1]() { cnt++; } } new S().m1(); cnt", 1);
    E6_NUM(e, "class G { static get v() { return 5; } static set v(x) { G.w = x; } }"
              " G.v + (G.v = 2, G.w)", 7);
    E6_TEXT(e, "class U2 { constructor() { return 9; } } typeof new U2()", "object");
    E6_THROWS(e, "class V { m() { undeclared_v = 1; } } new V().m()", "ReferenceError");
    E6_THROWS(e, "var WW = class W { m() { W = 1; } }; new WW().m()", "TypeError");
    E6_THROWS(e, "z1; class z1 {}", "ReferenceError");
    E6_NUM(e, "{ class Bk {} } typeof Bk === 'undefined' ? 1 : 0", 1);
    E6_NUM(e, "var made = 0; class Ct { constructor() { made++; } } new Ct(); new Ct(); made", 2);
    E6_BOOL(e, "class Ex { static isEx(o) { return o instanceof Ex; } } Ex.isEx(new Ex())", true);
    E6_NUM(e, "class Gen { *items() { yield 1; yield 2; } } var gs = 0; for (var v of new Gen().items()) gs += v; gs", 3);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_basic_expr_and_scope)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var Ce = class Named { who() { return Named === Ce ? 1 : 0; } }; new Ce().who()", 1);
    E6_NUM(e, "var outer = 3; var Cx = class { get v() { return outer; } }; new Cx().v", 3);
    E6_NUM(e, "var fns = []; for (let i = 0; i < 3; i++) { fns.push(class { k() { return i; } }); }"
              " fns[0] !== fns[1] ? new fns[2]().k() : -1", 2);
    E6_NUM(e, "var sum = 0; class Hd { static go(n) { return n <= 0 ? 0 : n + Hd.go(n - 1); } }"
              " sum = Hd.go(4); sum", 10);
    /* Repeated method and accessor names are legal; the later definition wins. */
    E6_NUM(e, "class Dup { m() { return 1; } m() { return 2; } get g() { return 3; } get g() { return 4; } }"
              " new Dup().m() * 10 + new Dup().g", 24);
    /* The heritage expression is evaluated with the class name in its TDZ. */
    E6_THROWS(e, "class Hc extends Hc {}", "ReferenceError");
    E6_THROWS(e, "var Hd2 = class Hd2n extends Hd2n {}", "ReferenceError");
    /* Fields are defined, not set: an inherited setter is not called. */
    E6_NUM(e, "var called = 0; class Bs { set x(v) { called++; } } class Ds extends Bs { x = 1; }"
              " var ds = new Ds(); called * 10 + (Object.getOwnPropertyDescriptor(ds, 'x').value)", 1);
    E6_THROWS(e, "class Two { constructor() {} constructor() {} }", "SyntaxError");
    E6_THROWS(e, "class Pr { static prototype() {} }", "SyntaxError");
    E6_THROWS(e, "class Gc { get constructor() {} }", "SyntaxError");
    E6_TEXT(e, "class Sm { static m() { return this.name; } } Sm.m()", "Sm");
    E6_NUM(e, "class Lp { static x = 1; } var lp = Lp; lp.x", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_accessor_arity_early_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class A { set x() {} }", "SyntaxError");
    E6_THROWS(e, "class A { get x(a) {} }", "SyntaxError");
    E6_THROWS(e, "class A { set x(a, b) {} }", "SyntaxError");
    E6_THROWS(e, "class A { static set x() {} }", "SyntaxError");
    E6_THROWS(e, "class A { static get x(a) {} }", "SyntaxError");
    E6_THROWS(e, "class A { set x(...a) {} }", "SyntaxError");
    E6_THROWS(e, "class A { get x(a = 1) {} }", "SyntaxError");
    E6_BOOL(e, "class G { get\n*a() {} } var A = new G(); G.prototype.hasOwnProperty('a') && A.hasOwnProperty('get')", true);
    E6_NUM(e, "class B { set x(a = 1) { this.v = a; } } var b = new B(); b.x = undefined; b.v", 1);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
