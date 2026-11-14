/* Dense and packed arrays (J5e). Each case breaks one fast path on
 * purpose (a prototype index property, a species or iterator redefinition, a hole, a freeze, a
 * non-writable length, a getter that mutates the array) and checks the result against the generic
 * semantics. The protector bits and the packed flag are checked where a case needs them. */
#include "es6_util.h"

#include "js/interp.h"
#include "js/object.h"

#include <SDL3/SDL_log.h>
#include <string.h>

/* Live bytes after a create/free cycle: lazily built process state is in place first. */
static size_t warm_base(void)
{
    es6_env w = es6_env_new();
    es6_env_free(w);
    return embi_mem_live();
}

/* The protector bits of the runtime (rt->protect). */
static uint32_t protect_bits(es6_env e)
{
    return e.rt->protect;
}

/* Whether the array held by global name is packed (no hole below its elements length). */
static bool global_is_packed(es6_env e, const char *name)
{
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, name));
    if (!embi_js_is_object(v)) {
        return false;
    }
    return embi_js_obj_is_packed(embi_js_as_object(v));
}

/* Holes read as undefined through the intrinsic chain, and the protector stays set. */
EMB_TEST(js_array_fast_hole_read_undefined)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var h = [1, , 3]; var r1 = h[1] === undefined && h[5] === undefined;"
            "var n = new Array(4); r1 && n[2] === undefined && n[0] === undefined",
            true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* An index property on Array.prototype breaks NO_PROTO_ELEMENTS: holes then read it, and a push
 * or a store to a hole goes through its setter. */
EMB_TEST(js_array_fast_proto_index_breaks_holes)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var hh = [1, , 3]; hh[1] === undefined", true);
    E6_BOOL(e, "Array.prototype[1] = 'proto'; hh[1] === 'proto'", true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    E6_BOOL(e, "delete Array.prototype[1]; hh[1] === undefined", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A setter for an index on Array.prototype is called by a push and by a store to a hole. */
EMB_TEST(js_array_fast_proto_setter_called)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* The setter counts into a plain object: an array sink would hit the same setter again. */
    E6_BOOL(e,
            "var seen = { n: 0, last: undefined }; var pa = [];"
            "Object.defineProperty(Array.prototype, 0, {"
            "  set(v) { seen.n++; seen.last = v; }, get() { return 'g'; }, configurable: true });"
            "pa.push(5);"
            "pa.length === 1 && seen.n === 1 && seen.last === 5",
            true);
    E6_BOOL(e,
            "var hl = new Array(2); hl[0] = 7; seen.n === 2 && seen.last === 7 && hl.length === 2",
            true);
    E6_BOOL(e, "delete Array.prototype[0]; true", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Appends on a packed array: length and values match the generic path. */
EMB_TEST(js_array_fast_push_pop_packed)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var p = []; for (var i = 0; i < 100; i++) p.push(i * 2);"
            "var s = 0; for (var j = 0; j < p.length; j++) s += p[j];"
            "p.length === 100 && s === 9900 && p.push(1, 2) === 102 && p[101] === 2",
            true);
    EMB_CHECK(global_is_packed(e, "p"));
    E6_BOOL(e,
            "var q = p.pop(); var r = p.pop(); q === 2 && r === 1 && p.length === 100"
            "&& p[99] === 198",
            true);
    E6_BOOL(e, "var emp = []; emp.pop() === undefined && emp.length === 0", true);
    E6_BOOL(e, "var pp = [1]; pp.length = 3; pp.push(4); pp.length === 4 && pp[3] === 4 && pp[1] === undefined",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A delete leaves a hole, so the array is no longer packed and a push goes to length. */
EMB_TEST(js_array_fast_delete_clears_packed)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var d = [1, 2, 3]; delete d[1]; d[1] === undefined && d.length === 3", true);
    EMB_CHECK(!global_is_packed(e, "d"));
    E6_BOOL(e, "d.push(9); d.length === 4 && d[3] === 9 && !(1 in d)", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A write past the end leaves a gap: the gap is holes and the array is not packed. */
EMB_TEST(js_array_fast_gap_store)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var g = []; g[3] = 'x'; g.length === 4 && g[0] === undefined && g[3] === 'x'", true);
    EMB_CHECK(!global_is_packed(e, "g"));
    E6_BOOL(e, "g.push('y'); g.length === 5 && g[4] === 'y'", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A non-writable length makes an append throw in push and fail in a store (sloppy: ignored). */
EMB_TEST(js_array_fast_readonly_length)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var ro = [1, 2]; Object.defineProperty(ro, 'length', { writable: false });"
            "var thr = false; try { ro.push(3); } catch (x) { thr = x instanceof TypeError; }"
            "thr && ro.length === 2 && ro[2] === undefined",
            true);
    EMB_CHECK(!global_is_packed(e, "ro"));
    E6_BOOL(e, "ro[2] = 5; ro.length === 2 && ro[2] === undefined", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A frozen array (non-default element attributes) and a non-extensible one refuse appends. */
EMB_TEST(js_array_fast_frozen_and_sealed)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var fz = [1, 2]; Object.freeze(fz); var thr = false;"
            "try { fz.push(3); } catch (x) { thr = x instanceof TypeError; }"
            "thr && fz.length === 2 && fz[0] === 1",
            true);
    E6_BOOL(e,
            "var ne = [1]; Object.preventExtensions(ne); ne[1] = 2; ne.length === 1 && ne[1] === undefined",
            true);
    E6_BOOL(e, "var fx = [4, 5]; fx[0] = 9; fx[0] === 9 && fx[1] === 5", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A length increase makes holes at the end; a shrink truncates. */
EMB_TEST(js_array_fast_length_grow_shrink)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var lg = [1, 2, 3]; lg.length = 6; lg[5] === undefined && lg.length === 6"
            "&& lg[2] === 3 && lg[4] === undefined",
            true);
    E6_BOOL(e, "lg.length = 2; lg.length === 2 && lg[2] === undefined && lg[1] === 2", true);
    E6_BOOL(e, "lg.push(7); lg.length === 3 && lg[2] === 7", true);
    EMB_CHECK(!global_is_packed(e, "lg"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Array literals with elisions, spreads and trailing commas keep their lengths. */
EMB_TEST(js_array_fast_literals)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var l1 = [1, 2, 3]; l1.length === 3 && l1[2] === 3", true);
    E6_BOOL(e, "var l2 = [1, , 3]; l2.length === 3 && l2[1] === undefined && !(1 in l2)", true);
    E6_BOOL(e, "var l3 = [1, , ]; l3.length === 2 && l3[0] === 1", true);
    E6_BOOL(e, "var src = [4, 5]; var l4 = [...src, 6]; l4.join() === '4,5,6'", true);
    E6_BOOL(e, "var l5 = [0, ...src, ...[], 9]; l5.length === 4 && l5[3] === 9", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Redefining Array[Symbol.species] clears ARRAY_SPECIES: map and filter then use the new species. */
EMB_TEST(js_array_fast_species_redefined)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var sp1 = [1, 2, 3].map(function (x) { return x; }); Array.isArray(sp1)", true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_SPECIES);
    E6_BOOL(e,
            "function Mine(n) { this.made = n; }"
            "Object.defineProperty(Array, Symbol.species, { value: Mine, configurable: true });"
            "var sp2 = [1, 2].map(function (x) { return x; }); sp2 instanceof Mine && sp2.made === 2",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_SPECIES));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Replacing Array.prototype.constructor clears ARRAY_SPECIES too. */
EMB_TEST(js_array_fast_constructor_replaced)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function Cs(n) { this.n = n; }"
            "Cs[Symbol.species] = Cs; var savec = Array.prototype.constructor;"
            "Array.prototype.constructor = Cs;"
            "var sc = [1].filter(function () { return true; }); sc instanceof Cs",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_SPECIES));
    E6_BOOL(e, "Array.prototype.constructor = savec; true", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Redefining %ArrayIteratorPrototype%.next clears ARRAY_ITERATOR: for-of calls the new next. */
EMB_TEST(js_array_fast_iterator_next_redefined)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var ita = 0; for (var x of [1, 2]) ita += x; ita === 3",
            true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR);
    E6_BOOL(e,
            "var AIP = Object.getPrototypeOf([][Symbol.iterator]());"
            "var origNext = AIP.next; var calls = 0;"
            "AIP.next = function () { calls++; return origNext.call(this); };"
            "var itb = 0; for (var y of [4, 5]) itb += y; itb === 9 && calls === 3",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Redefining Array.prototype[Symbol.iterator] makes for-of use the user method. */
EMB_TEST(js_array_fast_iterator_method_redefined)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var mix = 0; Array.prototype[Symbol.iterator] = function* () { yield 40; yield 2; };"
            "for (var z of [9]) mix += z; mix === 42",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_ARRAY_ITERATOR));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A getter that mutates the array during a callback loop is seen by the loop. */
EMB_TEST(js_array_fast_mutation_during_map)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var mu = [1, 2, 3]; var res = mu.map(function (x, i) {"
            "  if (i === 0) mu.push(10);"
            "  return x * 2; });"
            "res.length === 3 && res[0] === 2 && res[2] === 6 && mu.length === 4",
            true);
    E6_BOOL(e,
            "var mp = [1, 2]; var out = [];"
            "mp.forEach(function (x, i) { if (i === 0) mp.length = 1; out.push(x); });"
            "out.join() === '1'",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Index get and set through computed keys agree with string keys (canonical index strings and
 * numbers, -0, fractional and out-of-range keys). */
EMB_TEST(js_array_fast_key_forms)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var kf = [10, 20, 30]; var k0 = -0; var r = [];"
            "r.push(kf[k0], kf['1'], kf[2.0], kf[1.5], kf[-1], kf['01']);"
            "r.join() === '10,20,30,,,'",
            true);
    E6_BOOL(e, "kf[1.0] = 21; kf['2'] = 31; kf[1] === 21 && kf[2] === 31 && kf.length === 3", true);
    E6_BOOL(e, "kf[-0] = 11; kf[0] === 11 && kf['-0'] === undefined", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A sparse array (a far write) is generic: its values read the same way. */
EMB_TEST(js_array_fast_sparse_generic)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var sa = []; sa[100000] = 'far'; sa[0] = 'near';"
            "sa.length === 100001 && sa[100000] === 'far' && sa[0] === 'near' && sa[5] === undefined",
            true);
    EMB_CHECK(!global_is_packed(e, "sa"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Spread calls read the array elements (CALL_SPREAD through spread_list). */
EMB_TEST(js_array_fast_spread_call)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "function sum3(a, b, c) { return a + b + c; } var sargs = [1, 2, 3];"
            "sum3(...sargs) === 6 && isNaN(Math.max(...[4, , 6]))",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Index properties on Object.prototype break NO_PROTO_ELEMENTS: a hole of an array reads it. */
EMB_TEST(js_array_fast_object_proto_index)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var oh = [1, , 3]; oh[1] === undefined", true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS);
    E6_BOOL(e, "Object.prototype[1] = 'o'; oh[1] === 'o' && oh.indexOf('o') === 1", true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    E6_BOOL(e, "delete Object.prototype[1]; oh[1] === undefined", true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Replacing the prototype of Array.prototype clears NO_PROTO_ELEMENTS (the intrinsic chain changed). */
EMB_TEST(js_array_fast_setproto_array_proto)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var sp0 = [, 2]; sp0[0] === undefined", true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS);
    E6_BOOL(e,
            "Object.setPrototypeOf(Array.prototype, { 0: 'chain' });"
            "sp0[0] === 'chain' && [ , 'b'].indexOf('chain') === 0 && [, 'q'].at(0) === 'chain'",
            true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* An array used as a prototype breaks the protector when an index is stored on it, and a child
 * then sees the element. Creating the child alone leaves the protector set. */
EMB_TEST(js_array_fast_array_becomes_proto)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var pr = []; var ch = Object.create(pr); ch.length === 0", true);
    EMB_CHECK(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS);
    E6_BOOL(e, "pr[2] = 'k'; ch[2] === 'k' && !(0 in ch) && ch.length === 3", true);
    EMB_CHECK(!(protect_bits(e) & EMBI_JS_PROTECT_NO_PROTO_ELEMENTS));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* Sealed arrays are sparse (non-default attributes): stores to present elements work, appends throw. */
EMB_TEST(js_array_fast_sealed_array)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var sl = Object.seal([1, 2]); sl[0] = 9; var thr = false;"
            "try { sl.push(3); } catch (x) { thr = x instanceof TypeError; }"
            "thr && sl[0] === 9 && sl[1] === 2 && sl.length === 2 && sl[2] === undefined",
            true);
    EMB_CHECK(!global_is_packed(e, "sl"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* new Array(n) starts with length n and no elements, so it is not packed. Filling it sequentially
 * to its length makes it packed (mark_packed_if_complete in object.c); a partial fill does not. */
EMB_TEST(js_array_fast_new_array_filled)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var na = new Array(3); na[0] = 1; na[1] = 2; na[2] = 3; na.length === 3 && na[2] === 3", true);
    EMB_CHECK(global_is_packed(e, "na"));
    E6_BOOL(e, "var n5 = new Array(5); n5.length === 5 && n5[4] === undefined", true);
    EMB_CHECK(!global_is_packed(e, "n5"));
    E6_BOOL(e, "na.push(4); na.length === 4 && na[3] === 4 && [].length === 0", true);
    EMB_CHECK(!global_is_packed(e, "n5"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A computed key that is a canonical index string takes the element path; other strings stay named. */
EMB_TEST(js_array_fast_string_index_keys)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var sk = [10, 20, , 40];"
            "sk['1'] === 20 && sk['2'] === undefined && sk['3'] === 40 && sk['0'] === 10",
            true);
    E6_BOOL(e,
            "sk['01'] = 5; sk['01'] === 5 && sk.length === 4 && sk['1'] === 20"
            "&& Object.keys(sk).join() === '0,1,3,01'",
            true);
    E6_BOOL(e, "sk['4'] = 'n'; sk.length === 5 && sk[4] === 'n' && sk['4'] === 'n'", true);
    E6_BOOL(e, "sk['4294967295'] = 'big'; sk.length === 5 && sk['4294967295'] === 'big'", true);
    E6_BOOL(e, "var fx = [function () { return this; }]; fx['0']() === fx", true);
    EMB_CHECK(!global_is_packed(e, "sk"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A getter on an element makes the array sparse, so the spread reads the getter. */
EMB_TEST(js_array_fast_spread_accessor)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e,
            "var gt = [1, 2]; Object.defineProperty(gt, 1, { get() { return 'g'; }, configurable: true });"
            "[...gt].join() === '1,g' && [0, ...gt, 3].length === 4",
            true);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

/* A literal without elisions is packed; an elision leaves a hole and the flag clear. push and pop
 * on a literal take the fast path (elements_len follows length). */
EMB_TEST(js_array_fast_literal_packed)
{
    size_t base = warm_base();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var k1 = [1, 2, 3]; var k2 = [1, , 3]; var k3 = [1, , ]; var k4 = [, 1];"
               "var k5 = [...[7, 8], 9]; var k6 = []; k1.length === 3", true);
    EMB_CHECK(global_is_packed(e, "k1"));
    EMB_CHECK(!global_is_packed(e, "k2"));
    EMB_CHECK(!global_is_packed(e, "k3"));
    EMB_CHECK(!global_is_packed(e, "k4"));
    EMB_CHECK(global_is_packed(e, "k5"));
    EMB_CHECK(global_is_packed(e, "k6"));
    E6_BOOL(e, "k1.push(4) === 4 && k1.length === 4 && k1[3] === 4", true);
    embi_js_value v = embi_js_get(e.rt, e.r->global, embi_js_key_c(e.rt, "k1"));
    EMB_REQUIRE(embi_js_is_object(v));
    EMB_CHECK(embi_js_obj_is_packed(embi_js_as_object(v)));
    EMB_CHECK(embi_js_obj_elems_len(embi_js_as_object(v)) == 4);
    E6_BOOL(e, "k1.pop() === 4 && k1.length === 3", true);
    EMB_CHECK(embi_js_obj_elems_len(embi_js_as_object(v)) == 3);
    E6_BOOL(e, "k2.push(4) === 4 && k2.length === 4 && k2[1] === undefined && k2[3] === 4", true);
    EMB_CHECK(!global_is_packed(e, "k2"));
    /* A non-writable length keeps a completed literal off the fast path. */
    E6_BOOL(e, "var k7 = [1, 2]; Object.defineProperty(k7, 'length', { writable: false });"
               "var thr = false; try { 'use strict'; k7.push(3); } catch (x) { thr = true; }"
               "thr && k7.length === 2", true);
    EMB_CHECK(!global_is_packed(e, "k7"));
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
