/* Closure blocks without the home/initializer part (J5n2). A closure
 * record is 24 bytes; methods, field initializers, static blocks and class constructors get an extra part
 * (home object, private installer, field initializer) when they are made, and any other closure that is
 * given a home later gets it by reallocating its block. Every form that stores or reads a home or an
 * initializer is exercised here, across collections, and compared with its expected result. */
#include "es6_util.h"

#include <SDL3/SDL_log.h>

/* super in object-literal methods, accessors, arrow functions inside methods, and eval in a method. */
EMB_TEST(js_closure_ext_super_in_methods)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var base = {hi() { return 'base'; }, get g() { return 'gb'; }};"
               "var ok = true;"
               "for (var i = 0; i < 30; i++) {"
               "  var o = {__proto__: base, hi() { return 'o:' + super.hi(); }, get g() { return 'o' + super.g; },"
               "    arrow() { return (() => super.hi())(); }, ev() { return eval('super.hi()'); },"
               "    ['comp' + i]() { return super.hi(); }};"
               "  ok = ok && o.hi() === 'o:base' && o.g === 'ogb' && o.arrow() === 'base' && o.ev() === 'base' &&"
               "    o['comp' + i]() === 'base';"
               "}"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Class constructors, derived classes, fields, private methods, static blocks, accessors, with
 * collections in between. */
EMB_TEST(js_closure_ext_classes_and_collections)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var keep = []; var ok = true;"
               "function mk(i) {"
               "  class A { #p = i; f = i * 2; static s = i + 1; static { A.blk = 'b' + i; }"
               "    #m() { return this.#p; } get v() { return this.#m(); } static sm() { return 'A' + i; } }"
               "  class B extends A { g = super.v; constructor() { super(); this.h = super.v + 1; } static sm() { return super.sm() + 'B'; } }"
               "  return B;"
               "}"
               "for (var i = 0; i < 40; i++) { var B = mk(i); keep.push(B); var b = new B();"
               "  ok = ok && b.v === i && b.f === i * 2 && b.g === i && b.h === i + 1 && B.sm() === 'A' + i + 'B' &&"
               "    Object.getPrototypeOf(B).s === i + 1 && Object.getPrototypeOf(B).blk === 'b' + i; }"
               "ok", true);
    for (int i = 0; i < 4; i++) {
        embi_js_gc(e.rt);
        embi_js_gc_minor(e.rt);
        E6_BOOL(e, "for (var i = 0; i < keep.length; i++) { var b = new keep[i]();"
                   "  if (b.v !== i || b.h !== i + 1 || keep[i].sm() !== 'A' + i + 'B') { ok = false; } } ok", true);
    }
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Plain closures with and without captured variables stay correct (their blocks have no ext part). */
EMB_TEST(js_closure_ext_plain_closures)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function mk(a, b) { return [function () { return 1; }, function () { return a; },"
               "  () => a + b, function () { return a + b + arguments.length; }, function* () { yield a; yield b; }]; }"
               "var ok = true; var all = [];"
               "for (var i = 0; i < 200; i++) { all.push(mk(i, 2 * i)); }"
               "for (var i = 0; i < 200; i++) { var f = all[i];"
               "  ok = ok && f[0]() === 1 && f[1]() === i && f[2]() === 3 * i && f[3](1, 2) === 3 * i + 2 && [...f[4]()].join() === i + ',' + 2 * i; }"
               "ok", true);
    embi_js_gc(e.rt);
    E6_BOOL(e, "for (var i = 0; i < 200; i++) { var f = all[i]; ok = ok && f[1]() === i && f[2]() === 3 * i; } ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The saving: a closure without upvalues or home is a 24-byte block. */
EMB_TEST(js_closure_ext_block_sizes)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var keep = []; function mk() { return () => 1; } mk(); keep.length === 0", true);
    embi_js_gc(e.rt);
    embi_js_mem_stats a;
    embi_js_get_mem_stats(e.rt, &a);
    E6_BOOL(e, "for (var i = 0; i < 2000; i++) { keep.push(mk()); } keep.length === 2000", true);
    embi_js_gc(e.rt);
    embi_js_mem_stats b;
    embi_js_get_mem_stats(e.rt, &b);
    size_t per = (b.live_after_gc - a.live_after_gc) / 2000u;
    SDL_Log("    an arrow function costs %llu bytes (cell, closure block and its array slot)", (unsigned long long)per);
    /* 96-byte cell, 24-byte block, 8-byte array slot, with the shape shared. */
    EMB_CHECK(per <= 96u + 24u + 8u + 8u);
    es6_env_free(e);
}

EMB_TEST_MAIN()
