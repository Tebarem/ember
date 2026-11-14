/* Closure creation through the per-code shape cache (J5n1). The
 * first closure of a code cell appends length, name and prototype one by one and records the shapes it
 * reached; later closures write the slots and switch shape at once. Every closure made either way must
 * look the same through the generic reflection paths, survive collections that free unrelated shapes,
 * and not share state with its siblings. */
#include "es6_util.h"

#include <SDL3/SDL_log.h>

/* A function expression, an arrow, a method, a generator and a class constructor, each made many times
 * (the first through the slow path, the rest through the cache) and compared with the first. */
EMB_TEST(js_closure_shape_cached_closures_match)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function desc(f) {"
               "  return Object.getOwnPropertyNames(f).join() + '|' + JSON.stringify(["
               "    Object.getOwnPropertyDescriptor(f, 'length'), Object.getOwnPropertyDescriptor(f, 'name'),"
               "    Object.getOwnPropertyDescriptor(f, 'prototype') ? ["
               "      Object.getOwnPropertyDescriptor(f, 'prototype').writable,"
               "      Object.getOwnPropertyDescriptor(f, 'prototype').enumerable,"
               "      Object.getOwnPropertyDescriptor(f, 'prototype').configurable] : null]);"
               "}"
               "var makers = ["
               "  function (a, b) { return function named(x, y) { return a + b + x + y; }; },"
               "  function () { return (x) => x; },"
               "  function () { return {m(a, b, c) { return a; }}.m; },"
               "  function () { return function* gen(a) { yield a; }; },"
               "  function () { return class K { constructor(a) { this.a = a; } }; }];"
               "var ok = true;"
               "for (var m = 0; m < makers.length; m++) {"
               "  var first = desc(makers[m](1, 2));"
               "  for (var i = 0; i < 50; i++) { if (desc(makers[m](1, 2)) !== first) { ok = false; } }"
               "}"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A prototype object made by the cached path is a fresh object with its own constructor link. */
EMB_TEST(js_closure_shape_prototype_objects_are_independent)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function mk() { return function () {}; }"
               "var fs = []; for (var i = 0; i < 20; i++) { fs.push(mk()); }"
               "var ok = true;"
               "for (var i = 0; i < 20; i++) {"
               "  if (fs[i].prototype.constructor !== fs[i]) { ok = false; }"
               "  if (Object.getPrototypeOf(fs[i].prototype) !== Object.prototype) { ok = false; }"
               "  for (var j = i + 1; j < 20; j++) { if (fs[i].prototype === fs[j].prototype) { ok = false; } }"
               "  if (Object.keys(fs[i].prototype).length !== 0) { ok = false; }"
               "}"
               "fs[3].prototype.x = 1; fs[3].extra = 2; delete fs[4].name;"
               "ok = ok && fs[5].prototype.x === undefined && fs[5].extra === undefined && fs[5].name === '';"
               "var late = mk(); ok = ok && late.name === '' && late.length === 0 && late.prototype.constructor === late &&"
               "  Object.getOwnPropertyNames(late).join() === Object.getOwnPropertyNames(fs[5]).join();"
               "ok", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* The cached shapes are traced: closures made after collections that run between creations still get
 * correct properties (a freed shape would crash under ASan or give wrong keys), and no memory is leaked
 * (the caller's embi_js_mem_live check at teardown). */
EMB_TEST(js_closure_shape_survives_collections)
{
    size_t mem0 = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "function mk(n) { return function (a) { return a + n; }; }"
               "var base = Object.getOwnPropertyNames(mk(0)).join(); var ok = true;"
               "var keep = mk(1); ok = ok && keep(1) === 2; ok", true);
    for (int i = 0; i < 6; i++) {
        embi_js_gc(e.rt);
        E6_BOOL(e, "for (var i = 0; i < 40; i++) { var f = mk(i);"
                   "  if (Object.getOwnPropertyNames(f).join() !== base || f(1) !== 1 + i ||"
                   "      f.prototype.constructor !== f) { ok = false; } }"
                   "ok && keep(2) === 3", true);
    }
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
