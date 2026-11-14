/* Dense array builtins (J5e). Every element access of the array
 * builtins goes through the dense probes (builtins_array.c has/get/set/create helpers). Each case
 * runs the same method on a dense array and on an equal array made sparse (a sparse store takes
 * the generic path for every element), and checks that the results, the mutated receivers and the
 * key sets are identical. Holes, a callback that mutates the array, and a prototype index
 * property (which breaks NO_PROTO_ELEMENTS) are covered in the same loop. */
#include "es6_util.h"

#include "js/interp.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
#include <string.h>

static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

/* The differential harness. ops maps a method name to a function of one array; for each of the two
 * array shapes (no holes, holes) the method runs on the dense array and on the sparse copy, and the
 * snapshots (result and receiver) must match. fails collects the mismatches. */
static const char *k_differential =
    "var fails = [];"
    "function mk(sp, holes) {"
    "  var a = holes ? [3, , 4, , 5, 9] : [3, 1, 4, 1, 5, 9, 2, 6];"
    "  if (sp) { var n = a.length; a[100000] = 0; delete a[100000]; a.length = n; }"
    "  return a;"
    "}"
    "function snap(x) {"
    "  if (Array.isArray(x)) return 'A[' + Array.prototype.join.call(x, ',') + '|' + x.length + '|'"
    "    + Object.keys(x).join(',') + ']';"
    "  if (x === undefined) return 'U';"
    "  return typeof x + ':' + String(x);"
    "}"
    "var ops = {"
    "  forEach: function (a) { var r = []; a.forEach(function (v, i) { r.push(i + ':' + v); }); return r; },"
    "  map: function (a) { return a.map(function (v, i) { return v * 2 + i; }); },"
    "  filter: function (a) { return a.filter(function (v, i) { return i % 2 === 0 || v > 3; }); },"
    "  some: function (a) { return a.some(function (v) { return v === undefined; }); },"
    "  every: function (a) { return a.every(function (v) { return v !== 7; }); },"
    "  reduce: function (a) { return a.reduce(function (s, v, i) { return s + '|' + v + i; }, 'x'); },"
    "  reduceNoInit: function (a) { return a.reduce(function (s, v) { return s + v; }); },"
    "  reduceRight: function (a) { return a.reduceRight(function (s, v, i) { return s + '|' + v + i; }, 'y'); },"
    "  find: function (a) { return a.find(function (v) { return v > 3; }); },"
    "  findIndex: function (a) { return a.findIndex(function (v) { return v === undefined; }); },"
    "  findLast: function (a) { return a.findLast(function (v) { return v > 3; }); },"
    "  findLastIndex: function (a) { return a.findLastIndex(function (v) { return v === undefined; }); },"
    "  indexOf: function (a) { return [a.indexOf(1), a.indexOf(undefined), a.indexOf(4, 3)]; },"
    "  lastIndexOf: function (a) { return [a.lastIndexOf(1), a.lastIndexOf(undefined), a.lastIndexOf(9, -2)]; },"
    "  includes: function (a) { return [a.includes(undefined), a.includes(4, 5), a.includes(42)]; },"
    "  join: function (a) { return a.join('-') + '/' + String(a); },"
    "  slice: function (a) { return a.slice(1, -1).concat(a.slice(-2)); },"
    "  concat: function (a) { return a.concat([7, , 8], 9); },"
    "  reverse: function (a) { return a.reverse(); },"
    "  fill: function (a) { return a.fill(0, 1, 3); },"
    "  shift: function (a) { return [a.shift(), a.shift()]; },"
    "  unshift: function (a) { return a.unshift(7, 8); },"
    "  splice: function (a) { return a.splice(1, 2, 'x', 'y', 'z'); },"
    "  spliceTail: function (a) { return a.splice(-2); },"
    "  sort: function (a) { return a.sort(); },"
    "  sortCmp: function (a) { return a.sort(function (x, y) { return (x || 0) - (y || 0); }); },"
    "  at: function (a) { return [a.at(-1), a.at(0), a.at(99)]; },"
    "  copyWithin: function (a) { return a.copyWithin(0, 2); },"
    "  push: function (a) { return [a.push(1, 2), a.push()]; },"
    "  pop: function (a) { return [a.pop(), a.pop()]; },"
    "  toReversed: function (a) { return a.toReversed(); },"
    "  toSorted: function (a) { return a.toSorted(); },"
    "  toSpliced: function (a) { return a.toSpliced(1, 1, 'q'); },"
    "  with: function (a) { return a.with(0, 'w'); },"
    "  flat: function (a) { return a.flat(); },"
    "  flatMap: function (a) { return a.flatMap(function (v) { return [v, v]; }); },"
    "  arrayFrom: function (a) { return Array.from(a, function (v) { return v; }); },"
    "  arrayOf: function (a) { return Array.of.apply(Array, a); },"
    "  spread: function (a) { return [...a]; },"
    "  keys: function (a) { return Array.from(a.keys()); },"
    "  entries: function (a) { return Array.from(a.entries()); },"
    "  mutateLength: function (a) { return a.map(function (v, i) { if (i === 0) { a.length = 2; a[5] = 'late'; } return v; }); },"
    "  mutateDelete: function (a) { var r = []; a.forEach(function (v, i) { if (i === 1) delete a[2]; r.push(v); }); return r; },"
    "  mutateStore: function (a) { var r = []; a.forEach(function (v, i) { if (i === 0) a[3] = 'm'; r.push(v); }); return r; },"
    "  mutateFreeze: function (a) { var r = []; a.forEach(function (v, i) { if (i === 1) Object.freeze(a); r.push(v); }); return r; },"
    "  mutatePush: function (a) { var r = []; a.map(function (v, i) { if (i < 2) a.push(i); return v; }).forEach(function (v) { r.push(v); }); return r; },"
    "  mutateProto: function (a) { var r = []; a.forEach(function (v, i) { if (i === 0) Object.setPrototypeOf(a, { 2: 'P' }); r.push(v); }); return r; }"
    "};"
    "function run(label, shapes) {"
    "  for (var h = 0; h < 2; h++) {"
    "    for (var name in ops) {"
    "      var da = mk(false, h === 1), sa = mk(true, h === 1);"
    "      var rd, rs;"
    "      try { rd = snap(ops[name](da)) + '#' + snap(da); } catch (e) { rd = 'throw ' + e.constructor.name; }"
    "      try { rs = snap(ops[name](sa)) + '#' + snap(sa); } catch (e) { rs = 'throw ' + e.constructor.name; }"
    "      if (rd !== rs) fails.push(label + name + (h ? 'H' : '') + ' ' + rd + ' vs ' + rs);"
    "    }"
    "  }"
    "}"
    "run('');";

/* Plain dense results: the method on a dense array (no sparse twin) gives the expected value. */
EMB_TEST(js_array_fast_builtins_differential)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    es6_run(e, k_differential);
    E6_BOOL(e, "fails.length === 0", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* The same loop with an index property on Array.prototype: NO_PROTO_ELEMENTS is cleared, so the holes
 * are read through the prototype on both arrays, and the results must still agree. */
EMB_TEST(js_array_fast_builtins_proto_index)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    es6_run(e, k_differential);
    E6_BOOL(e, "Array.prototype[1] = 'P'; Array.prototype[3] = 'Q'; fails = []; run('p:'); true", true);
    EMB_CHECK(!(e.rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    E6_BOOL(e, "delete Array.prototype[1]; delete Array.prototype[3]; fails.length === 0", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A hole read after a prototype index exists sees it; after a prototype change to a fresh object the
 * holes read through the new chain, which a builtin must see too. */
EMB_TEST(js_array_fast_builtins_proto_change)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var pa = [1, , 3]; var mapped = pa.map(function (v) { return v; });"
            "!(1 in mapped) && mapped.length === 3",
            true);
    E6_BOOL(e,
            "var saved = Object.getPrototypeOf(Array.prototype);"
            "Object.setPrototypeOf(Array.prototype, { 1: 'chain' });"
            "var ch = [1, , 3]; var got = ch.join(',') === '1,chain,3' && ch.indexOf('chain') === 1"
            "&& ch.includes('chain') && [ , 'z'].at(0) === undefined;"
            "Object.setPrototypeOf(Array.prototype, saved); got",
            true);
    EMB_CHECK(!(e.rt->protect & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Builtins that create arrays (map, filter, slice, concat, splice, from, of) keep the species
 * contract when the element stores go direct: a species constructor still receives the length. */
EMB_TEST(js_array_fast_builtins_species_direct)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var seen = []; function Sub(n) { seen.push(n); }"
            "var sa = [1, 2, 3]; sa.constructor = {}; sa.constructor[Symbol.species] = Sub;"
            "var m = sa.map(function (v) { return v + 1; });"
            "m instanceof Sub && m[2] === 4 && seen[0] === 3 && seen.length === 1",
            true);
    E6_BOOL(e,
            "var fa = [1, 2, 3, 4]; var sub = fa.filter(function (v) { return v % 2; });"
            "sub.length === 2 && sub[1] === 3 && Object.getPrototypeOf(sub) === Array.prototype",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
