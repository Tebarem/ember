/* Classes (J2d): subclassing the builtins. Every native constructor takes new_target and creates
 * its object from new_target's prototype (GetPrototypeFromConstructor), so the subclass prototype
 * is kept; Error subclasses keep a stack; the call form of each constructor is unchanged. */
#include "es6_util.h"

EMB_TEST(class_builtins_error)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "class MyErr extends Error {} var me = new MyErr('boom');"
               " me instanceof MyErr && me instanceof Error", true);
    E6_TEXT(e, "class MyErr2 extends Error { constructor(m) { super(m); this.name = 'MyErr2'; } }"
               " new MyErr2('x').message", "x");
    E6_TEXT(e, "class MyErr3 extends TypeError {} new MyErr3('m').name", "TypeError");
    E6_BOOL(e, "class MyE4 extends Error {} var st; try { throw new MyE4('q'); } catch (x) { st = x; }"
               " typeof st.stack === 'string' && st instanceof MyE4", true);
    E6_BOOL(e, "Error('x') instanceof Error && RangeError('r') instanceof RangeError", true);
    E6_BOOL(e, "class MyE5 extends Error {} Object.getPrototypeOf(MyE5.prototype) === Error.prototype", true);
    E6_NUM(e, "class MyE6 extends Error {} new MyE6('m', { cause: 4 }).cause", 4);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_builtins_array_object_function)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "class MyArr extends Array {} var ma = new MyArr(); ma.push(1, 2);"
              " ma.length * 10 + (ma instanceof MyArr ? 1 : 0) + (Array.isArray(ma) ? 100 : 0)", 121);
    E6_NUM(e, "class MyArr2 extends Array {} new MyArr2(3).length", 3);
    E6_BOOL(e, "class MyArr3 extends Array {} MyArr3.of(1, 2, 3) instanceof MyArr3 && MyArr3.of(1, 2, 3).length === 3", true);
    E6_BOOL(e, "class MyArr4 extends Array {} new MyArr4(1, 2) instanceof MyArr4 && Array(2).length === 2", true);
    E6_NUM(e, "class MyObj extends Object { constructor() { super(); this.z = 2; } } new MyObj().z", 2);
    E6_BOOL(e, "class MyObj2 extends Object {} new MyObj2() instanceof MyObj2", true);
    E6_BOOL(e, "class MyObj3 extends Object {} Object.getPrototypeOf(new MyObj3()) === MyObj3.prototype", true);
    E6_BOOL(e, "var F5 = function () {}; var rc = Reflect.construct(Array, [], F5);"
               " Object.getPrototypeOf(rc) === F5.prototype", true);
    E6_BOOL(e, "class MyFn extends Function {} var mf = new MyFn('return 3'); mf() === 3 && mf instanceof MyFn",
            true);
    E6_BOOL(e, "Function('return 4')() === 4 && typeof Function('') === 'function'", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_builtins_primitives_and_collections)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "class MyNum extends Number {} new MyNum(4) + 1 === 5 && new MyNum(4) instanceof MyNum", true);
    E6_NUM(e, "class MyStr extends String {} new MyStr('abc').length", 3);
    E6_BOOL(e, "class MyBool extends Boolean {} new MyBool(false) instanceof MyBool", true);
    E6_BOOL(e, "class MyMap extends Map {} var mm = new MyMap([[1, 2]]); mm.get(1) === 2 && mm instanceof MyMap",
            true);
    E6_BOOL(e, "class MySet extends Set {} new MySet([1, 1]).size === 1", true);
    E6_BOOL(e, "class MyIt extends Iterator { next() { return { done: true }; } } new MyIt() instanceof Iterator",
            true);
    E6_THROWS(e, "class X extends Iterator {} new Iterator()", "TypeError");
    E6_THROWS(e, "Iterator()", "TypeError");
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* GetPrototypeFromConstructor runs exactly once: a Proxy new.target with a get trap sees one read
 * of "prototype" per Reflect.construct, for every native constructor (the call convention passes
 * new_target alone, with no pre-made object). */
EMB_TEST(class_builtins_proto_read_once)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var reads = 0, order = '';"
              " function mkNT() { reads = 0; order = '';"
              "   return new Proxy(function () {}, { get(t, k, r) {"
              "     if (k === 'prototype') { reads++; order += 'p'; } return Reflect.get(t, k, r); } }); }"
              " var GF = Object.getPrototypeOf(function* () {}).constructor;"
              " var ctors = [[Array, []], [Array, [3]], [Object, []], [Function, ['return 1']],"
              "   [Number, [1]], [String, ['s']], [Boolean, [true]], [Map, []], [Set, []],"
              "   [WeakMap, []], [WeakSet, []], [Error, ['m']], [TypeError, []], [RangeError, ['x']],"
              "   [GF, ['yield 1']], [Iterator, []]];"
              " var bad = 0;"
              " for (var i = 0; i < ctors.length; i++) { var nt = mkNT();"
              "   var o = Reflect.construct(ctors[i][0], ctors[i][1], nt);"
              "   if (reads !== 1) bad++;"
              "   if (Object.getPrototypeOf(o) !== nt.prototype) bad += 100; }"
              " bad", 0);
    /* The prototype is read before the message is converted, and only once: p, m. */
    E6_TEXT(e, "var log = ''; var nt2 = new Proxy(function () {}, { get(t, k, r) {"
               "   if (k === 'prototype') log += 'p'; return Reflect.get(t, k, r); } });"
               " Reflect.construct(Error, [{ toString() { log += 'm'; return 'x'; } }], nt2); log", "pm");
    /* A throwing prototype getter propagates and no object leaks into the result. */
    E6_THROWS(e, "var nt3 = new Proxy(function () {}, { get(t, k) {"
                 "   if (k === 'prototype') throw new RangeError('boom'); } });"
                 " Reflect.construct(Array, [], nt3)", "RangeError");
    /* A call (no new.target) never reads a prototype of anything. */
    E6_BOOL(e, "Array(2).length === 2 && Object(1) instanceof Number && Error('x').message === 'x'"
               " && typeof Function('return 1') === 'function'", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(class_object_ctor_new_target)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* NewTarget other than the active function: OrdinaryCreateFromConstructor, even when the
     * resolved prototype is Object.prototype. */
    E6_TEXT(e, "function F() {} F.prototype = Object.prototype; typeof Reflect.construct(Object, [1], F)", "object");
    E6_BOOL(e, "function G() {} G.prototype = Object.prototype; Reflect.construct(Object, [1], G) instanceof Number",
            false);
    E6_BOOL(e, "new Object(1) instanceof Number", true);
    E6_BOOL(e, "class S extends Object {} new S(1) instanceof Number", false);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
