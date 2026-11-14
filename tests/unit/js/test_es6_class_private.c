/* Classes (J2d): private names. Fields, methods, accessors, static privates, the `#x in obj`
 * brand check, brand errors, per-evaluation names, nested classes, and the early errors. */
#include "es6_util.h"

EMB_TEST(class_private_members)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class P { #x = 1; #m() { return this.#x + 10; } get #g() { return this.#x * 2; }"
              " set #g(v) { this.#x = v; } run() { this.#g = 5; return this.#m() * 100 + this.#g; } }"
              " new P().run()", 1510);
    E6_BOOL(e, "class Q { #p = 3; static has(o) { return #p in o; } } Q.has(new Q()) && !Q.has({})", true);
    E6_THROWS(e, "class R { #q = 1; static get(o) { return o.#q; } } R.get({})", "TypeError");
    E6_THROWS(e, "class S { #r() {} static set(o) { o.#r = 1; } } S.set(new S())", "TypeError");
    E6_THROWS(e, "class T { get #v() { return 1; } static w(o) { o.#v = 2; } } T.w(new T())", "TypeError");
    E6_NUM(e, "var mk = function () { return class { #id = 7; static read(o) { return o.#id; } }; };"
              " var K1 = mk(), K2 = mk();"
              " K1.read(new K1()) * 10 + (function () { try { K1.read(new K2()); return 0; }"
              " catch (err) { return 1; } })()", 71);
    E6_NUM(e, "class Outer { #o = 2; static make() { return class { get(x) { return x.#o; } }; } }"
              " var Ik = Outer.make(); new Ik().get(new Outer())", 2);
    E6_NUM(e, "class Sx { static #c = 4; static #sm() { return Sx.#c * 3; } static run() {"
              " return Sx.#sm(); } } Sx.run()", 12);
    E6_NUM(e, "class Sb { static #k = 1; static { Sb.#k = 9; } static get() { return Sb.#k; } } Sb.get()", 9);
    E6_NUM(e, "class Sub0 { #x = 1; } class Sub1 extends Sub0 { #y = 2; sum() { return this.#y; } }"
              " new Sub1().sum()", 2);
    E6_NUM(e, "class Pm { #m() { return 3; } call() { return this.#m(); } } new Pm().call()", 3);
    E6_NUM(e, "class Ps { #s = 1; bump() { return this.#s++ + ++this.#s; } } new Ps().bump()", 4);
    E6_NUM(e, "class Pc { #c = 5; cmp() { this.#c += 2; this.#c *= 3; return this.#c; } } new Pc().cmp()", 21);
    E6_NUM(e, "class Pk { #k = 0; set(v) { this.#k = v; return this.#k; } } new Pk().set(8)", 8);
    E6_BOOL(e, "class Pr { #v = 1; static same(a, b) { return a.#v === b.#v; } } Pr.same(new Pr(), new Pr())", true);
    E6_NUM(e, "class Pa { #a = 1; static sum(o) { return o.#a; } } var pa1 = new Pa(); Pa.sum(pa1) + Pa.sum(pa1) * 10", 11);
    /* A static private method called on a subclass receiver is a brand failure. */
    E6_THROWS(e, "class Sp { static #m() { return 1; } static call() { return this.#m(); } }"
                 " class Sq extends Sp {} Sq.call()", "TypeError");
    E6_NUM(e, "class Sp2 { static #m() { return 1; } static call() { return this.#m(); } } Sp2.call()", 1);
    /* Private names are not properties: no key, no JSON, and freezing does not stop them. */
    E6_BOOL(e, "class Pi { #h = 1; pub = 2; } var pi = new Pi(); Reflect.ownKeys(pi).length === 1 &&"
               " JSON.stringify(pi) === '{\"pub\":2}' && Object.getOwnPropertyNames(pi).join() === 'pub'", true);
    E6_NUM(e, "class Fz { #v = 1; inc() { return ++this.#v; } } var fz = Object.freeze(new Fz());"
              " fz.inc() * 10 + fz.inc()", 23);
    /* A Proxy is not the object that carries the private field. */
    E6_THROWS(e, "class Px { #p = 1; static get(o) { return o.#p; } } Px.get(new Proxy(new Px(), {}))",
              "TypeError");
    E6_BOOL(e, "class Px2 { #p = 1; static has(o) { return #p in o; } }"
               " var px2 = new Px2(); Px2.has(px2) && !Px2.has(new Proxy(px2, {}))", true);
    E6_BOOL(e, "class Pn { #n = 1; static test(o) { return #n in o; } } var pn = Pn.test; pn(new Pn())", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_private_logical_assign_non_tail)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Short-circuit and taken paths in non-tail positions: the base slots must be dropped. */
    E6_TEXT(e, "class L1 { #a = 1; m() { return [this.#a ||= 9, 1, this.#a].join(); } } new L1().m()", "1,1,1");
    E6_TEXT(e, "class L2 { #a = 0; m() { return [this.#a &&= 5, 1, this.#a].join(); } } new L2().m()", "0,1,0");
    E6_TEXT(e, "class L3 { #a = 2; m() { return [this.#a ?\?= 7, 1, this.#a].join(); } } new L3().m()", "2,1,2");
    E6_TEXT(e, "class L4 { #a = 0; m() { return [this.#a ||= 9, 1, this.#a].join(); } } new L4().m()", "9,1,9");
    E6_TEXT(e, "class L5 { #a = 1; m() { return [this.#a &&= 5, 1, this.#a].join(); } } new L5().m()", "5,1,5");
    E6_TEXT(e, "class L6 { #a; m() { return [this.#a ?\?= 7, 1, this.#a].join(); } } new L6().m()", "7,1,7");
    E6_NUM(e, "class L7 { #a = 1; static f(x, y, z) { return x * 100 + y * 10 + z; }"
              " m() { return L7.f(this.#a ||= 9, this.#a &&= 4, this.#a ?\?= 8); } } new L7().m()", 144);
    E6_NUM(e, "class L8 { #a = 1; #b = 2; m() { var r = [this.#a += 1, this.#b++, ++this.#a, 5];"
              " return r[0] * 1000 + r[1] * 100 + r[2] * 10 + this.#b; } } new L8().m()", 2233);
    /* Other operators on private references in non-tail positions. */
    E6_TEXT(e, "class M1 { #a = 1; m() { return [this.#a += 2, typeof this.#a, this.#a++, this.#a--, --this.#a, 9].join(); } }"
               " new M1().m()", "3,number,3,4,2,9");
    E6_TEXT(e, "class M2 { #a = 0; #b = 0; m() { [this.#a, this.#b] = [4, 5]; ({ x: this.#a, y: this.#b } = { x: 6, y: 7 });"
               " return [this.#a, this.#b, 1].join(); } } new M2().m()", "6,7,1");
    E6_TEXT(e, "class M3 { #a = 0; m() { var r = []; for (this.#a of [1, 2]) { r.push(this.#a); } for (this.#a in { k: 1 }) { r.push(this.#a); }"
               " return [r.join('|'), 3].join(); } } new M3().m()", "1|2|k,3");
    E6_TEXT(e, "class M4 { #f() { return this === m4; } #g = function () { return this === m4; }; m() { return [this.#f(), this.#g(), (this.#f)(), 1].join(); } }"
               " var m4 = new M4(); m4.m()", "true,true,true,1");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_private_errors)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class Bad { m() { return this.#nope; } }", "SyntaxError");
    E6_THROWS(e, "class Dd { #a; #a; }", "SyntaxError");
    E6_THROWS(e, "class Ee { #constructor() {} }", "SyntaxError");
    E6_THROWS(e, "class Ff { #f; static m() { delete this.#f; } }", "SyntaxError");
    E6_THROWS(e, "class Pr { #z; static t() { return #z in 5; } } Pr.t()", "TypeError");
    E6_THROWS(e, "class A { constructor(o) { if (o) return o; } }"
                 " class B extends A { #x = 1; constructor(o) { super(o); } } var pre = new B(); new B(pre)",
              "TypeError");
    E6_THROWS(e, "class Ww { #w = 1; static set(o) { o.#w = 2; } } Ww.set({})", "TypeError");
    E6_THROWS(e, "class Mm { m() { return #m in this; } }", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_private_direct_eval)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Direct eval inside a class body names the class's private names. */
    E6_NUM(e, "class Ev { #x = 4; m() { return eval('this.#x + 1'); } } new Ev().m()", 5);
    E6_NUM(e, "class Ev2 { #x = 4; m() { return (() => eval('this.#x'))(); } } new Ev2().m()", 4);
    E6_NUM(e, "class Ev3 { #x = 1; m() { eval('this.#x = 9'); return this.#x; } } new Ev3().m()", 9);
    E6_THROWS(e, "class Ev4 { m() { return eval('this.#nope'); } } new Ev4().m()", "SyntaxError");
    E6_THROWS(e, "class Ev5 { #a; m() { return eval('(0, eval)(\"this.#a\")'); } } new Ev5().m()", "SyntaxError");
    E6_THROWS(e, "eval('class Q { m(o) { return o.#zz; } }')", "SyntaxError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_private_accessor_arity_and_heritage)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_THROWS(e, "class A { set #x() {} }", "SyntaxError");
    E6_THROWS(e, "class A { get #x(a) {} }", "SyntaxError");
    E6_THROWS(e, "class A { static set #x(a, b) {} }", "SyntaxError");
    /* The class's own private names are not in scope in its heritage. */
    E6_THROWS(e, "class C extends class { x = this.#foo; } { #foo; }", "SyntaxError");
    E6_NUM(e, "class O { #foo = 4; static mk() { class C extends class { get() { return 1; } } { m() { return 2; } }"
              " return new C().m(); } } O.mk()", 2);
    /* Private elements cannot be added to a non-extensible object. */
    E6_THROWS(e, "class B { constructor(o) { return o; } } class C extends B { #v = 1; }"
                 " new C(Object.preventExtensions({}))", "TypeError");
    E6_THROWS(e, "class B2 { constructor(o) { return o; } } class C2 extends B2 { #m() {} }"
                 " new C2(Object.preventExtensions({}))", "TypeError");
    E6_THROWS(e, "class T { static #g = (Object.preventExtensions(T), 1); }", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
