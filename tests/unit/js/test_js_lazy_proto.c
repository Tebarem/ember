/* Lazy `prototype` of ordinary functions (J5n2). A new closure of an
 * ordinary function holds EMPTY in its prototype slot and a shape whose prototype node is flagged
 * EMBI_JS_ATTR_LAZY; the object is made by the first access that can see it. Each test breaks the
 * optimisation on purpose: every access path reaches the property while it is still lazy (reflection,
 * new, instanceof, assignment, defineProperty, freeze, delete, setPrototypeOf, statics added first,
 * dictionary mode, reads through an object whose prototype is the function) and compares with the
 * result of the same access on a function whose prototype was read first. Functions of one code cell
 * share a lazy shape and a realized shape, so a cache filled from one kind must never answer for the
 * other. */
#include "es6_util.h"

#include <SDL3/SDL_log.h>

/* The common prologue: mk() makes a fresh function of one code cell (lazy), real() makes one and
 * reads its prototype at once (realized). */
#define PRO                                                                                      \
    "function mk() { return function f(a, b) { this.a = a; this.b = b; }; }"                    \
    "function real() { var f = mk(); f.prototype; return f; }"

/* Reflection on a lazy function gives the same answers as on a realized one. */
EMB_TEST(js_lazy_proto_reflection_matches_realized)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "function desc(f) { var d = Object.getOwnPropertyDescriptor(f, 'prototype');"
               "  return Object.getOwnPropertyNames(f).join() + '|' + Reflect.ownKeys(f).length + '|' +"
               "    f.hasOwnProperty('prototype') + ('prototype' in f) + '|' + Object.keys(f).length + '|' +"
               "    d.writable + d.enumerable + d.configurable + (typeof d.value) +"
               "    (d.value.constructor === f) + (Object.getPrototypeOf(d.value) === Object.prototype) +"
               "    Object.getOwnPropertyNames(d.value).join(); }"
               "var ok = true; var want = desc(real());"
               "for (var i = 0; i < 30; i++) { if (desc(mk()) !== want) { ok = false; } }"
               "var f1 = mk(); var names = []; for (var k in f1) { names.push(k); }"
               "ok = ok && names.length === 0 && JSON.stringify(Object.entries(f1)) === '[]' &&"
               "  Object.keys(Object.getOwnPropertyDescriptors(mk())).join() === 'length,name,prototype' &&"
               "  JSON.stringify(f1) === undefined && Object.assign({}, f1).prototype === undefined;"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* new, instanceof, Reflect.construct with a lazy new.target, bind, and inherited methods. */
EMB_TEST(js_lazy_proto_construct_and_instanceof)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "var ok = true;"
               "for (var i = 0; i < 20; i++) {"
               "  var F = mk(); var o = new F(1, 2);"
               "  ok = ok && o instanceof F && Object.getPrototypeOf(o) === F.prototype && o.a === 1 && o.b === 2;"
               "  var G = mk(); G.prototype.m = function () { return 7; };"
               "  ok = ok && (new G).m() === 7 && F.prototype.m === undefined;"
               "  var H = mk(); var NT = mk();"
               "  var r = Reflect.construct(H, [3, 4], NT);"
               "  ok = ok && Object.getPrototypeOf(r) === NT.prototype && r.a === 3 && !(r instanceof H);"
               "  var B = mk().bind(null, 5); var bo = new B(6);"
               "  ok = ok && bo.a === 5 && bo.b === 6 && (bo instanceof B);"
               "  var C = mk(); var P = {z: 1}; C.prototype = P; ok = ok && new C(0, 0).z === 1 && (new C instanceof C);"
               "  var D = mk(); ok = ok && !({} instanceof D);"
               "}"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Writes: assignment, defineProperty (value, writable:false, accessor attempts), freeze, seal. */
EMB_TEST(js_lazy_proto_writes)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "var ok = true; var P = {tag: 1};"
               "var a = mk(); a.prototype = P; ok = ok && a.prototype === P && Object.getOwnPropertyNames(a).join() === 'length,name,prototype';"
               "var b = mk(); Object.defineProperty(b, 'prototype', {writable: false});"
               "var bp = b.prototype; b.prototype = P;"
               "ok = ok && b.prototype === bp && bp.constructor === b && Object.getOwnPropertyDescriptor(b, 'prototype').writable === false;"
               "var c = mk(); Object.defineProperty(c, 'prototype', {value: P});"
               "ok = ok && c.prototype === P && Object.getOwnPropertyDescriptor(c, 'prototype').writable === true;"
               "var t = false; try { Object.defineProperty(mk(), 'prototype', {get: function () {}}); } catch (x) { t = x instanceof TypeError; }"
               "ok = ok && t;"
               "var d = mk(); Object.freeze(d); var dp = d.prototype; d.prototype = P;"
               "ok = ok && dp.constructor === d && d.prototype === dp && Object.isFrozen(d);"
               "var s = mk(); Object.seal(s); ok = ok && s.prototype.constructor === s && Object.isSealed(s);"
               "var dl = mk(); ok = ok && (delete dl.prototype) === false && dl.prototype.constructor === dl;"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Properties added before the prototype is read, dictionary mode, delete of another property,
 * setPrototypeOf, preventExtensions: the shape rebuild keeps keys, order and values. */
EMB_TEST(js_lazy_proto_shape_rebuild)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "var ok = true;"
               "var a = mk(); a.s1 = 1; a.s2 = 2;"
               "ok = ok && a.prototype.constructor === a && Object.getOwnPropertyNames(a).join() === 'length,name,prototype,s1,s2' &&"
               "  a.s1 === 1 && a.s2 === 2;"
               "var b = mk(); b.s = 1; delete b.s; ok = ok && b.prototype.constructor === b && Object.getOwnPropertyNames(b).join() === 'length,name,prototype';"
               "var c = mk(); c.x = 1; delete c.name; ok = ok && c.prototype.constructor === c && c.x === 1 &&"
               "  Object.getOwnPropertyNames(c).join() === 'length,prototype,x';"
               "var d = mk(); for (var i = 0; i < 80; i++) { d['k' + i] = i; }"
               "ok = ok && d.prototype.constructor === d && d.k79 === 79 && Object.getOwnPropertyNames(d).length === 83;"
               "var base = {inherited: 5}; var f = mk(); Object.setPrototypeOf(f, base); f.own = 1;"
               "ok = ok && f.inherited === 5 && f.prototype.constructor === f && Object.getPrototypeOf(f) === base && f.own === 1;"
               "var g = mk(); Object.preventExtensions(g); ok = ok && g.prototype.constructor === g && !Object.isExtensible(g);"
               "var h = mk(); Object.setPrototypeOf(h, null); ok = ok && h.prototype.constructor === h && h.call === undefined;"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Inline caches: one read site and one write site see lazy and realized functions of the same code
 * cell, in both orders, and an object whose prototype is a lazy function. */
EMB_TEST(js_lazy_proto_inline_caches)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "function read(f) { return f.prototype; }"
               "function write(f, v) { f.prototype = v; return f.prototype; }"
               "function readLen(f) { return f.length + f.name; }"
               "var ok = true; var P = {};"
               "for (var round = 0; round < 6; round++) {"
               "  var rs = real(); var rp = read(rs); ok = ok && rp.constructor === rs && read(rs) === rp;"
               "  var lz = mk(); var lp = read(lz); ok = ok && lp.constructor === lz && lp !== rp;"
               "  var lz2 = mk(); ok = ok && write(lz2, P) === P && read(lz2) === P;"
               "  var lz3 = mk(); ok = ok && readLen(lz3) === '2f' && read(lz3).constructor === lz3;"
               "  var rs2 = real(); ok = ok && write(rs2, P) === P;"
               "  var inh = mk(); var o = Object.create(inh);"
               "  ok = ok && read(o).constructor === inh && read(o) === read(inh);"
               "  var inh2 = mk(); var o2 = Object.create(inh2);"
               "  ok = ok && o2.prototype === inh2.prototype && o2.prototype.constructor === inh2;"
               "}"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The sites stay correct when they are made hot before any function is realized, and megamorphic. */
EMB_TEST(js_lazy_proto_hot_sites)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO
               "function read(f) { return f.prototype; }"
               "var ok = true; var fs = [];"
               "for (var i = 0; i < 300; i++) { var f = mk(); fs.push(f); if (i % 3 === 0) { read(f); } }"
               "for (var i = 0; i < 300; i++) { if (read(fs[i]).constructor !== fs[i]) { ok = false; } }"
               "var shapes = [function () {}, function (x) {}, mk, {prototype: 1}, [], mk(), class A {}];"
               "for (var r = 0; r < 5; r++) { for (var i = 0; i < shapes.length; i++) { var v = read(shapes[i]);"
               "  if (typeof shapes[i] === 'function' && v.constructor !== shapes[i] && !(shapes[i].toString().indexOf('class') === 0)) { ok = false; } } }"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Classes: the class setup replaces the prototype without making the wasted object. */
EMB_TEST(js_lazy_proto_classes)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var ok = true;"
               "for (var i = 0; i < 20; i++) {"
               "  class K { constructor(a) { this.a = a; } m() { return 1; } static s() { return 2; } }"
               "  class L extends K { n() { return 3; } }"
               "  var d = Object.getOwnPropertyDescriptor(K, 'prototype');"
               "  ok = ok && d.writable === false && d.enumerable === false && d.configurable === false;"
               "  ok = ok && K.prototype.constructor === K && new L(4).a === 4 && new L(4).m() === 1 && L.s() === 2 &&"
               "    Object.getPrototypeOf(L.prototype) === K.prototype && Object.getPrototypeOf(L) === K &&"
               "    Object.getOwnPropertyNames(K).join() === 'length,name,prototype,s';"
               "}"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Collections between creation and first use, with the generational collector on; the realized object
 * is reachable from its function and the memory returns to the baseline. */
EMB_TEST(js_lazy_proto_survives_collections)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO "var keep = []; for (var i = 0; i < 200; i++) { keep.push(mk()); } var ok = true; var i0 = 0; ok", true);
    for (int i = 0; i < 5; i++) {
        embi_js_gc(e.rt);
        embi_js_gc_minor(e.rt);
        E6_BOOL(e, "for (var i = i0; i < i0 + 40; i++) {"
                   "  var f = keep[i]; var p = f.prototype; if (p.constructor !== f || keep[i].prototype !== p) { ok = false; }"
                   "  var g = mk(); var q = new g(1, 2); if (!(q instanceof g)) { ok = false; } }"
                   "i0 = i; ok", true);
    }
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The point of it: a function that is never used with new costs no prototype object. */
EMB_TEST(js_lazy_proto_saves_memory)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, PRO "var keep = []; mk(); keep.length === 0", true);
    embi_js_gc(e.rt);
    embi_js_mem_stats a;
    embi_js_get_mem_stats(e.rt, &a);
    E6_BOOL(e, "for (var i = 0; i < 1000; i++) { keep.push(mk()); } keep.length === 1000", true);
    embi_js_gc(e.rt);
    embi_js_mem_stats b;
    embi_js_get_mem_stats(e.rt, &b);
    size_t lazy = b.live_after_gc - a.live_after_gc;
    E6_BOOL(e, "for (var i = 0; i < 1000; i++) { keep[i].prototype; } true", true);
    embi_js_gc(e.rt);
    embi_js_mem_stats c;
    embi_js_get_mem_stats(e.rt, &c);
    size_t real = c.live_after_gc - a.live_after_gc;
    SDL_Log("    1000 functions: %llu bytes lazy, %llu bytes with the prototypes made", (unsigned long long)lazy,
            (unsigned long long)real);
    /* The array of 1000 values is in both; the prototype objects are the difference. */
    EMB_CHECK(real > lazy + 1000u * 80u);
    EMB_CHECK(lazy < 1000u * 200u);
    es6_env_free(e);
}

EMB_TEST_MAIN()
