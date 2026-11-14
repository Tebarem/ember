/* Classes (J2d): super property and super call references, derived constructors (this before and
 * after super(), the return rules, the default constructor, extends null), [[HomeObject]] of
 * object-literal methods and their arrows, and new.target. */
#include "es6_util.h"

EMB_TEST(class_super_members)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class A1 { m() { return 1; } get g() { return 10; } }"
              " class B1 extends A1 { m() { return super.m() + 1; } get g() { return super.g + 1; } }"
              " new B1().m() * 100 + new B1().g", 211);
    E6_NUM(e, "class A2 { constructor() { this.v = 3; } } class B2 extends A2 {"
              " constructor() { super(); this.v *= 2; } } new B2().v", 6);
    E6_NUM(e, "class A3 { constructor(a, b) { this.s = a + b; } } class B3 extends A3 {}"
              " new B3(2, 3).s", 5);
    E6_THROWS(e, "class A4 {} class B4 extends A4 { constructor() { this.x = 1; } } new B4()", "ReferenceError");
    E6_THROWS(e, "class A5 {} class B5 extends A5 { constructor() { super(); super(); } } new B5()",
              "ReferenceError");
    E6_THROWS(e, "class A6 {} class B6 extends A6 { constructor() { } } new B6()", "ReferenceError");
    E6_NUM(e, "class A7 {} class B7 extends A7 { constructor() { return 1; } }"
              " try { new B7(); 0 } catch (x) { 1 }", 1);
    E6_NUM(e, "class A8 {} class B8 extends A8 { constructor() { return { q: 4 }; } } new B8().q", 4);
    E6_TEXT(e, "class N9 extends null { constructor() { return Object.create(N9.prototype); } }"
               " typeof new N9()", "object");
    E6_THROWS(e, "function Fk() {} Fk.prototype = 3; class Z extends Fk {}", "TypeError");
    E6_THROWS(e, "class Z2 extends 5 {}", "TypeError");
    E6_TEXT(e, "class P0 { static who() { return 'P0'; } } class C0 extends P0 {"
               " static who() { return super.who() + 'C0'; } } C0.who()", "P0C0");
    E6_NUM(e, "var base = { hi() { return 5; } }; var obj = { __proto__: base, hi() { return super.hi() * 2; } };"
              " obj.hi()", 10);
    E6_NUM(e, "var base2 = { v: 7 }; var o2 = { __proto__: base2, get() { return (() => super.v)(); } };"
              " o2.get()", 7);
    E6_NUM(e, "class A9 {} class B9 extends A9 { set(x) { super.val = x; return this.val; } }"
              " new B9().set(3)", 3);
    E6_NUM(e, "class A10 { who() { return this.n; } } class B10 extends A10 { constructor() { super();"
              " this.n = 42; } who() { return super.who(); } } new B10().who()", 42);
    E6_NUM(e, "class A11 { k() { return 2; } } class B11 extends A11 { f = super.k() + 1; } new B11().f", 3);
    E6_NUM(e, "class A13 {} A13.prototype.q = 1; class B13 extends A13 { t() { super.q += 4; return this.q; } }"
              " new B13().t()", 5);
    E6_NUM(e, "var kb = { ['k' + 1]: 6 }; var ob = { __proto__: kb, f() { return super['k' + 1]; } }; ob.f()", 6);
    E6_NUM(e, "var kc = { n: 1 }; var oc = { __proto__: kc, f() { super.n++; return this.n; } }; oc.f()", 2);
    E6_NUM(e, "var ki = { n: 1 }; var oi = { __proto__: ki, f() { super['n'] = 9; return this.n; } }; oi.f()", 9);
    E6_NUM(e, "class Dl { m() { return 1; } } class Dm extends Dl { n() { return [super.m, this.m].length; } }"
              " new Dm().n()", 2);
    E6_BOOL(e, "class Ce { m() { return super.toString === Object.prototype.toString; } } new Ce().m()", true);
    E6_NUM(e, "var ob2 = { m() { return 4; } }; var oo = { __proto__: ob2, call() { return super.m.call(this); } };"
              " oo.call()", 4);
    E6_NUM(e, "var cb = { g() { return 1; } }; var cc = { __proto__: cb, h: { m() { return 2; } } }; "
              " cc.h.m() + (function () { return 3; })()", 5);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_super_logical_assign_non_tail)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_TEXT(e, "var o = { __proto__: { x: 0 }, m() { return [super.x ||= 3, super.x ?\?= 4, super.x &&= 5, 2].join(); } };"
               " o.m()", "3,0,0,2");
    E6_TEXT(e, "class B { } B.prototype.x = 3; class C extends B { m() { return [super.x ||= 9, 1].join(); } }"
               " new C().m()", "3,1");
    E6_TEXT(e, "var k = 'y'; var o2 = { __proto__: { y: 0, z: 2 }, m() {"
               " return [super[k] ||= 6, super['z'] &&= 8, super[k] ?\?= 1, super['w'] ?\?= 4, 7].join(); } };"
               " o2.m()", "6,8,0,4,7");
    E6_TEXT(e, "var o3 = { __proto__: { y: 5 }, m() { return [super['y'] ||= 6, super['y'] ?\?= 7, 1].join(); } };"
               " o3.m()", "5,5,1");
    E6_TEXT(e, "var o4 = { __proto__: { y: 0 }, m() { return [super['y'] &&= 6, 1, this.y].join(); } };"
               " o4.m()", "0,1,0");
    E6_NUM(e, "var o5 = { __proto__: { x: 1 }, f(a, b, c) { return a * 100 + b * 10 + c; },"
              " m() { return this.f(super.x ||= 9, super.x &&= 4, super.x ?\?= 8); } }; o5.m()", 141);
    /* Other operators on super references in non-tail positions. */
    E6_TEXT(e, "var q1 = { __proto__: { x: 1, f() { return this === q1; } }, m() { return [super.x += 2, typeof super.x, super.x++, ++super.x, --super['x'], super['x']--, super.f(), super['f'](), 9].join(); } };"
               " q1.m()", "3,number,1,2,0,1,true,true,9");
    E6_TEXT(e, "var q2 = { __proto__: {}, m() { [super.a, super['b']] = [1, 2]; ({ p: super.c } = { p: 3 }); return [this.a, this.b, this.c, 7].join(); } };"
               " q2.m()", "1,2,3,7");
    E6_TEXT(e, "var q3 = { __proto__: {}, m() { var r = []; for (super.v of [1, 2]) { r.push(this.v); } for (super['w'] in { k: 1 }) { r.push(this.w); }"
               " return [r.join('|'), 3].join(); } }; q3.m()", "1|2|k,3");
    E6_TEXT(e, "var q4 = { __proto__: {}, m() { var r = [], k = 'u'; for (super[k] of [1, 2]) { r.push(this.u); } for (super['w' + 1] in { z: 1 }) { r.push(this.w1); }"
               " return [r.join('|'), 3].join(); } }; q4.m()", "1|2|z,3");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_super_errors_and_new_target)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class A12 { constructor() { super(); } }", "SyntaxError");
    E6_THROWS(e, "function nf() { return super.x; }", "SyntaxError");
    E6_THROWS(e, "class A14 { m() { super(); } }", "SyntaxError");
    E6_THROWS(e, "var o3 = { m() { delete super.x; } }; o3.m()", "ReferenceError");
    E6_BOOL(e, "class NT { constructor() { this.ok = new.target === NT; } } new NT().ok", true);
    E6_BOOL(e, "class NA { constructor() { this.t = new.target; } } class NB extends NA {}"
               " new NB().t === NB", true);
    E6_BOOL(e, "function NF() { return new.target === undefined; } NF()", true);
    E6_NUM(e, "class Ctr { static count = 0; constructor() { Ctr.count++; } } class Sub extends Ctr {}"
              " new Sub(); new Sub(); Ctr.count", 2);
    E6_THROWS(e, "class Cl { m() {} } Cl.prototype.m.call(1); class Cc extends Cl {} Cc.call(null)",
              "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_super_direct_eval)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* super.x inside direct eval in a method sees the method's [[HomeObject]]. */
    E6_NUM(e, "class B { m() { return 2; } } class D extends B { m() { return eval('super.m()') + 1; } }"
              " new D().m()", 3);
    E6_NUM(e, "var o = { __proto__: { v: 5 }, g() { return eval('super.v'); } }; o.g()", 5);
    E6_NUM(e, "class B2 { m() { return 7; } } class D2 extends B2 { m() { return (() => eval('super.m()'))(); } }"
              " new D2().m()", 7);
    E6_THROWS(e, "function f() { return eval('super.x'); } f()", "SyntaxError");
    E6_THROWS(e, "(0, eval)('super.x')", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_derived_return_rules_at_function_end)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* The result check runs after every finally block: a TypeError for return 0 is not catchable
     * by the constructor's own try/catch. */
    E6_TEXT(e, "class A {} class C extends A { constructor() { super(); try { return 0; } catch (e) { return; } } }"
               " try { new C(); 'no'; } catch (x) { x.name; }", "TypeError");
    /* this is bound by the time the function ends. */
    E6_TEXT(e, "class A2 {} class C2 extends A2 { constructor() { try { return; } finally { super(); } } }"
               " typeof new C2()", "object");
    E6_TEXT(e, "class A3 {} class C3 extends A3 { constructor() { var f = () => { super(); };"
               " try { return; } finally { f(); } } } typeof new C3()", "object");
    E6_TEXT(e, "class A4 {} class C4 extends A4 { constructor() { super(); for (var x of [1, 2]) { return; } } }"
               " typeof new C4()", "object");
    E6_THROWS(e, "class A5 {} class C5 extends A5 { constructor() { for (var x of [1]) { return; } } } new C5()",
              "ReferenceError");
    E6_THROWS(e, "class A6 {} class C6 extends A6 { constructor() { try { return; } finally { } } } new C6()",
              "ReferenceError");
    E6_NUM(e, "class A7 {} class C7 extends A7 { constructor() { try { return { v: 3 }; } finally { } } }"
              " new C7().v", 3);
    /* A finally that overrides the return value is the one checked. */
    E6_THROWS(e, "class A8 {} class C8 extends A8 { constructor() { super(); try { return {}; } finally { return 1; } } }"
                 " new C8()", "TypeError");
    /* An iterator close error wins over the result check (the check runs after the loop closes). */
    E6_TEXT(e, "var it = { [Symbol.iterator]() { return this; }, next() { return { done: false }; },"
               " return() { throw 'closed'; } }; class A10 {} class C10 extends A10 {"
               " constructor() { super(); for (var k of it) { return 0; } } }"
               " try { new C10(); 'no'; } catch (x) { String(x); }", "closed");
    /* super[key]: the super base is read before the key converts. */
    E6_TEXT(e, "var pr = { p: 'ok' }, pr2 = { p: 'bad' }; var ob = { __proto__: pr, m() { return super[key]; } };"
               " var key = { toString() { Object.setPrototypeOf(ob, pr2); return 'p'; } }; ob.m()", "ok");
    /* super() is not available in direct eval (pinned gap). */
    E6_THROWS(e, "class A9 {} class C9 extends A9 { constructor() { eval('super()'); } } new C9()", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
