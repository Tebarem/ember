#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* %TypedArray% and the concrete constructors through script (ECMA-262 23.2): element conversions,
 * the integer-indexed exotic behaviour, resizable buffers, and the prototype methods. */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "typedarray.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw: %s", src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    if (v != EMBI_JS_TRUE) {
        SDL_Log("    js not true: %s", src);
        return false;
    }
    return true;
}

EMB_TEST(typedarray_element_conversions)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "new Uint8Array([1, 2, 300])[2] === 44"));
    EMB_CHECK(js_true(e, "new Int8Array([128])[0] === -128 && new Int8Array([-129])[0] === 127"));
    EMB_CHECK(js_true(e, "new Uint8ClampedArray([1.5, 2.5, 300, -5, 254.5])"
                         ".join() === '2,2,255,0,254'"));
    EMB_CHECK(js_true(e, "new Uint16Array([65536 + 7])[0] === 7 && new Int16Array([32768])[0] === -32768"));
    EMB_CHECK(js_true(e, "new Uint32Array([-1])[0] === 4294967295 && new Int32Array([2147483648])[0] === -2147483648"));
    EMB_CHECK(js_true(e, "new Float32Array([0.1])[0] === Math.fround(0.1)"));
    EMB_CHECK(js_true(e, "Object.is(new Float64Array([-0])[0], -0) && isNaN(new Float64Array([NaN])[0])"));
    EMB_CHECK(js_true(e, "new Float16Array([65520])[0] === Infinity && new Float16Array([65519])[0] === 65504"));
    EMB_CHECK(js_true(e, "new Float16Array([0.1])[0] === 0.0999755859375"));
    EMB_CHECK(js_true(e, "new Float16Array([2.9802322387695312e-8])[0] === 0"));
    EMB_CHECK(js_true(e, "new Float16Array([5.960464477539063e-8])[0] === 5.960464477539063e-8"));
    EMB_CHECK(js_true(e, "new BigInt64Array([2n ** 63n])[0] === -(2n ** 63n)"));
    EMB_CHECK(js_true(e, "new BigUint64Array([-1n])[0] === 2n ** 64n - 1n"));
    EMB_CHECK(js_true(e, "(function(){ try { new BigInt64Array([1]); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new Uint8Array([1n]); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_constructors_and_metadata)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "Uint8Array.BYTES_PER_ELEMENT === 1 && Float64Array.prototype.BYTES_PER_ELEMENT === 8"));
    EMB_CHECK(js_true(e, "Uint8Array.length === 3 && Uint8Array.name === 'Uint8Array'"));
    EMB_CHECK(js_true(e, "(function(){ try { Uint8Array(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var TA = Object.getPrototypeOf(Int8Array);"
                         "TA.name === 'TypedArray' && Object.getPrototypeOf(Int8Array.prototype) === TA.prototype"));
    EMB_CHECK(js_true(e, "(function(){ try { new (Object.getPrototypeOf(Int8Array))(); return false; }"
                         " catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new Float32Array(1)) === '[object Float32Array]'"));
    EMB_CHECK(js_true(e, "Uint8Array.prototype.toString === Array.prototype.toString"));
    EMB_CHECK(js_true(e, "Uint8Array.prototype[Symbol.iterator] === Uint8Array.prototype.values"));
    EMB_CHECK(js_true(e, "new Uint8Array(new ArrayBuffer(8), 2, 3).byteOffset === 2 && "
                         "new Uint8Array(new ArrayBuffer(8), 2, 3).length === 3"));
    EMB_CHECK(js_true(e, "(function(){ try { new Uint16Array(new ArrayBuffer(4), 1); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { new Uint16Array(new ArrayBuffer(3)); return false; }"
                         " catch (x) { return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "Uint8Array.from([1, 2, 3], x => x * 2).join() === '2,4,6'"));
    EMB_CHECK(js_true(e, "Int16Array.of(1, -2, 3).join() === '1,-2,3'"));
    EMB_CHECK(js_true(e, "new Uint8Array(new Uint16Array([258, 3])).join() === '2,3'"));
    EMB_CHECK(js_true(e, "new Uint8Array({length: 2, 0: 7, 1: 8})[1] === 8"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_integer_indexed_keys)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = new Uint8Array(3); t[5] = 9; t[5] === undefined && !(5 in t)"));
    EMB_CHECK(js_true(e, "t['-0'] === undefined && !('-0' in t) && t['1.5'] === undefined"));
    EMB_CHECK(js_true(e, "t['1.5'] = 4, t['1.5'] === undefined && !t.hasOwnProperty('1.5')"));
    EMB_CHECK(js_true(e, "t.foo = 'x', t.foo === 'x' && Object.keys(t).join() === '0,1,2,foo'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(t, 1).writable && "
                         "Object.getOwnPropertyDescriptor(t, 1).configurable"));
    EMB_CHECK(js_true(e, "delete t[0] === false && delete t[9] === true"));
    EMB_CHECK(js_true(e, "(function(){ 'use strict'; try { t[0] = 1; t.bar = 1; "
                         "Object.defineProperty(t, 0, {value: 7}); "
                         "return t[0] === 7; } catch (x) { return false; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Object.defineProperty(t, 0, {configurable: false});"
                         " return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "Reflect.defineProperty(t, 0, {value: 3, configurable: true}) === true"));
    EMB_CHECK(js_true(e, "Reflect.defineProperty(t, 0, {value: 3}) === true && t[0] === 3"));
    EMB_CHECK(js_true(e, "Reflect.set(t, 10, 1) === true && Reflect.set(t, 1, 5) === true && t[1] === 5"));
    EMB_CHECK(js_true(e, "Object.create(t)[2] === 0 && !Object.create(t).hasOwnProperty(2)"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_views_track_resizable_buffers)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8});"
                         "var tr = new Uint8Array(rab); var fx = new Uint8Array(rab, 0, 4);"
                         "rab.resize(8); tr.length === 8 && fx.length === 4"));
    EMB_CHECK(js_true(e, "rab.resize(2); tr.length === 2 && fx.length === 0 && fx.byteLength === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { fx.fill(1); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var t2 = new Uint8Array(new ArrayBuffer(4)); t2.buffer.transfer();"
                         "t2.length === 0 && t2.byteLength === 0 && t2[0] === undefined"));
    EMB_CHECK(js_true(e, "(function(){ try { t2.values(); return false; } catch (x) { "
                         "return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_methods)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var a = new Int16Array([5, 1, 4, 2]);"
                         "a.map(x => x * 2).join() === '10,2,8,4' && a.filter(x => x > 2).join() === '5,4'"));
    EMB_CHECK(js_true(e, "a.reduce((s, x) => s + x) === 12 && a.reduceRight((s, x) => s + '' + x, '') === '2415'"));
    EMB_CHECK(js_true(e, "a.at(-1) === 2 && a.at(9) === undefined && a.includes(4) && a.indexOf(4) === 2"));
    EMB_CHECK(js_true(e, "a.lastIndexOf(5) === 0 && a.indexOf(9) === -1 && a.includes(NaN) === false"));
    EMB_CHECK(js_true(e, "a.slice(1, 3).join() === '1,4' && a.slice(-2).join() === '4,2'"));
    EMB_CHECK(js_true(e, "a.subarray(1, 3).join() === '1,4' && a.subarray(1).byteOffset === 2"));
    EMB_CHECK(js_true(e, "var s = new Uint8Array([9, 8, 7, 6]); s.copyWithin(0, 2); s.join() === '7,6,7,6'"));
    EMB_CHECK(js_true(e, "new Uint8Array(4).fill(3, 1, -1).join() === '0,3,3,0'"));
    EMB_CHECK(js_true(e, "var r = new Uint8Array([1, 2, 3]).reverse(); r.join() === '3,2,1'"));
    EMB_CHECK(js_true(e, "var f = new Float64Array([3, NaN, -0, 0, -1]); f.sort();"
                         "Object.is(f[0], -1) && Object.is(f[1], -0) && Object.is(f[2], 0) && f[3] === 3 && isNaN(f[4])"));
    EMB_CHECK(js_true(e, "new Int8Array([1, 10, 2]).sort((x, y) => y - x).join() === '10,2,1'"));
    EMB_CHECK(js_true(e, "new Int8Array([1, 2]).toSorted((x, y) => y - x).join() === '2,1'"));
    EMB_CHECK(js_true(e, "var orig = new Int8Array([1, 2]); orig.toReversed().join() === '2,1' && orig.join() === '1,2'"));
    EMB_CHECK(js_true(e, "new Int8Array([1, 2, 3]).with(-1, 9).join() === '1,2,9'"));
    EMB_CHECK(js_true(e, "(function(){ try { new Int8Array(2).with(5, 1); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "new Uint8Array([1, 2]).every(x => x > 0) && new Uint8Array([1, 2]).some(x => x > 1)"));
    EMB_CHECK(js_true(e, "new Uint8Array([1, 2, 3]).findLast(x => x < 3) === 2 && "
                         "new Uint8Array([1, 2, 3]).findLastIndex(x => x > 5) === -1"));
    EMB_CHECK(js_true(e, "new Uint8Array([1, 2]).toLocaleString() === '1,2'"));
    EMB_CHECK(js_true(e, "[...new Uint8Array([4, 5]).entries()].join('|') === '0,4|1,5'"));
    EMB_CHECK(js_true(e, "[...new Uint8Array([4, 5]).keys()].join() === '0,1'"));
    EMB_CHECK(js_true(e, "String(new Uint8Array([1, 2])) === '1,2'"));
    EMB_CHECK(js_true(e, "var it = new Uint8Array([1, 2, 3]); var sum = 0; for (var v of it) sum += v; sum === 6"));
    EMB_CHECK(js_true(e, "var set = new Uint8Array(5); set.set([1, 2], 2); set.join() === '0,0,1,2,0'"));
    EMB_CHECK(js_true(e, "var s2 = new Uint8Array(4); s2.set(new Uint8Array([1,2,3,4]).subarray(0, 2), 1);"
                         "s2.join() === '0,1,2,0'"));
    EMB_CHECK(js_true(e, "var big = new Uint8Array(4); big.set(new Int16Array([258, -1]), 1);"
                         "big.join() === '0,2,255,0'"));
    EMB_CHECK(js_true(e, "(function(){ try { new Uint8Array(2).set([1, 2, 3]); return false; } catch (x) { "
                         "return x instanceof RangeError; } })()"));
    EMB_CHECK(js_true(e, "var ov = new Uint8Array([1, 2, 3, 4]); ov.set(ov.subarray(0, 3), 1);"
                         "ov.join() === '1,1,2,3'"));
    EMB_CHECK(js_true(e, "var q = new Uint8Array([1, 2]); q.constructor === Uint8Array && "
                         "q.map(x => x).constructor === Uint8Array"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_species_and_tags)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "class MyTA extends Uint8Array {}; var m = new MyTA(3);"
                         "m.map(x => x) instanceof MyTA && m.subarray(1) instanceof MyTA && "
                         "m.slice(0) instanceof MyTA"));
    EMB_CHECK(js_true(e, "var noSpecies = new Uint8Array(2); noSpecies.constructor = {[Symbol.species]: Int8Array};"
                         "noSpecies.map(x => x) instanceof Int8Array"));
    EMB_CHECK(js_true(e, "(function(){ try { var bad = new Uint8Array(2); bad.constructor = {[Symbol.species]: "
                         "BigInt64Array}; bad.slice(0); return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "new Uint8Array(2)[Symbol.toStringTag] === 'Uint8Array' && "
                         "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "
                         "Symbol.toStringTag).get.call({}) === undefined"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "
                         "'length').get.name === 'get length'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Every mutating TypedArray method throws on an immutable buffer, before reading its arguments. */
EMB_TEST(typedarray_immutable_buffer_rejects_writes)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    const char *prelude = "var im = new Uint8Array(new ArrayBuffer(4).transferToImmutable()); true";
    EMB_CHECK(js_true(e, prelude));
    EMB_CHECK(js_true(e, "im.buffer.immutable === true && im.length === 4"));
    static const char *const calls[] = {
        "im.fill(1)",
        "im.reverse()",
        "im.copyWithin(0, 1)",
        "im.set([1])",
        "im.set(new Uint8Array(1))",
        "im.sort()",
        "im.setFromHex('01')",
        "im.setFromBase64('AA==')",
        "im[0] = 5",
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        char src[256];
        SDL_snprintf(src, sizeof src,
                     "(function(){ try { %s; return false; } catch (x) { return x instanceof TypeError; } })()",
                     calls[i]);
        EMB_CHECK(js_true(e, src));
    }
    /* Arguments are not read first: a getter on the value or the callback is never called. */
    EMB_CHECK(js_true(e, "(function(){ var hit = false; try { im.fill({ valueOf() { hit = true; return 1; } }); }"
                         " catch (x) {} return !hit && (function(){ try { im.fill(1); } catch (x) {"
                         " return x instanceof TypeError; } return false; })(); })()"));
    /* Reads are unaffected. */
    EMB_CHECK(js_true(e, "im.join() === '0,0,0,0' && im.at(0) === 0 && [...im.keys()].length === 4"));
    EMB_CHECK(js_true(e, "im.slice(0, 2).length === 2 && im.toReversed().length === 4"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* slice writes into its species result, which must not be immutable. */
EMB_TEST(typedarray_slice_rejects_immutable_species_result)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var src = new Uint8Array([1, 2, 3]);"
                         "src.constructor = {[Symbol.species]: function(n) {"
                         " return new Uint8Array(new ArrayBuffer(n).transferToImmutable()); }};"
                         "(function(){ try { src.slice(0); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* TypedArray writes on a detached buffer throw; the fill argument is not read first. */
EMB_TEST(typedarray_detached_write_methods)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var d = new Uint8Array(4); d.buffer.transfer();"
                         "d.length === 0 && d.byteLength === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { d.fill(1); return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { d.set([1]); return false; } catch (x) { return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* slice into a species result that shares the source buffer copies forward element by element. */
EMB_TEST(typedarray_slice_overlapping_species_copies_forward)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var buf = new ArrayBuffer(4); var src = new Uint8Array(buf); src.set([10, 20, 30, 40]);"
                         "src.constructor = {[Symbol.species]: function(n) { return new Uint8Array(buf, 1, n); }};"
                         "src.slice(0, 3); new Uint8Array(buf).join() === '10,10,10,10'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* Atomics write operations throw on an immutable buffer before reading index or value. */
EMB_TEST(atomics_immutable_buffer_rejects_writes)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var ai = new Int32Array(new ArrayBuffer(8).transferToImmutable());"
                         "(function(){ try { Atomics.store(ai, 0, 1); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var hit = false; try { Atomics.add(ai, {valueOf(){ hit = true; return 0; }}, 1);"
                         " return false; } catch (x) { return x instanceof TypeError && !hit; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.compareExchange(ai, 0, 0, 1); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.exchange(ai, 0, 1); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "Atomics.load(ai, 0) === 0"));
    EMB_CHECK(js_true(e, "(function(){ try { Atomics.wait(ai, 0, 0, 0); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    /* A mutable buffer still works. */
    EMB_CHECK(js_true(e, "var am = new Int32Array(2); Atomics.store(am, 1, 7) === 7 && Atomics.add(am, 1, 1) === 7"
                         " && am[1] === 8"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* ArrayIterator.next on a TypedArray throws once the view is out of bounds or detached. */
EMB_TEST(typedarray_iterator_out_of_bounds_throws)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8}); var ta = new Uint8Array(rab, 2);"
                         "var it = ta[Symbol.iterator](); it.next();"
                         "rab.resize(1); (function(){ try { it.next(); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var d = new Uint8Array(4); var di = d.keys(); d.buffer.transfer();"
                         "(function(){ try { di.next(); return false; } catch (x) { return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var s = 0; for (var v of new Uint8Array([1, 2, 3])) s += v; s === 6"));
    EMB_CHECK(js_true(e, "var r2 = new ArrayBuffer(4, {maxByteLength: 8}); var t2 = new Uint8Array(r2);"
                         "(function(){ try { for (var v of t2) { r2.resize(8); } return t2.length === 8; } catch (x) { return false; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* copyWithin clamps the count to the shrunk buffer (as the test262 snapshot expects) when the buffer shrinks during argument coercion. */
EMB_TEST(typedarray_copy_within_shrink)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8}); var ta = new Uint8Array(rab); ta.set([1,2,3,4]);"
                         "ta.copyWithin({valueOf(){ rab.resize(3); return 1; }}, 0, 3); ta.join() === '1,1,2'"));
    EMB_CHECK(js_true(e, "var rb = new ArrayBuffer(4, {maxByteLength: 8}); var tb = new Uint8Array(rb); tb.set([1,2,3,4]);"
                         "tb.copyWithin(0, {valueOf(){ rb.resize(3); return 1; }}); tb.join() === '2,3,3'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* A species or this-constructor result over an immutable buffer is rejected before any element work. */
EMB_TEST(typedarray_immutable_result_rejected_up_front)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var calls = 0; var src = new Uint8Array(2);"
                         "src.constructor = {[Symbol.species]: function(n) { return new Uint8Array(new ArrayBuffer(n).transferToImmutable()); }};"
                         "(function(){ try { src.map(function(){ calls++; return 1; }); return false; } catch (x) {"
                         " return x instanceof TypeError && calls === 0; } })()"));
    EMB_CHECK(js_true(e, "(function(){ try { src.filter(function(){ return true; }); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "(function(){ var reads = 0; function C(n) { return new Uint8Array(new ArrayBuffer(n).transferToImmutable()); }"
                         "try { Uint8Array.from.call(C, {length: 2, get 0() { reads++; return 1; }}, function(v){ reads++; return v; });"
                         " return false; } catch (x) { return x instanceof TypeError && reads === 0; } })()"));
    EMB_CHECK(js_true(e, "(function(){ function C(n) { return new Uint8Array(new ArrayBuffer(n).transferToImmutable()); }"
                         "try { Uint8Array.of.call(C, 1, 2); return false; } catch (x) { return x instanceof TypeError; } })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* GC forced between defineProperty/delete/ownKeys calls on a TypedArray keeps it intact. */
EMB_TEST(typedarray_ordinary_paths_survive_gc)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var ta = new Uint8Array([1, 2, 3]);"
                         "for (var i = 0; i < 2000; i++) { Object.defineProperty(ta, 'p' + (i % 8), {value: {x: i}, configurable: true});"
                         " delete ta['p' + ((i + 3) % 8)]; Object.getOwnPropertyNames(ta); var junk = [i, {a: i}, 'k' + i]; }"
                         "ta.length === 3 && ta[2] === 3 && Object.getOwnPropertyNames(ta).length >= 3"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

/* join / toString / toLocaleString build the result linearly (regression for a quadratic concat). */
EMB_TEST(typedarray_join_large_is_linear)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    const char *limit = EMB_TEST_ASAN ? "30000" : "10000";
    char src[512];
    SDL_snprintf(src, sizeof src,
                 "var big = new Uint8Array(200000).fill(7); var t0 = Date.now();"
                 "var j = big.join('-'); var s = String(big); var l = big.toLocaleString();"
                 "j.length === 399999 && s.length === 399999 && l.length === 399999 && Date.now() - t0 < %s", limit);
    EMB_CHECK(js_true(e, src));
    EMB_CHECK(js_true(e, "new Uint8Array(0).join() === '' && new Uint8Array(1).join('x') === '0'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_bigint_methods)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var b = new BigUint64Array([2n ** 63n, 1n, 2n ** 64n - 1n]); b.sort();"
                         "b.join() === '1,9223372036854775808,18446744073709551615'"));
    EMB_CHECK(js_true(e, "var c = new BigInt64Array(3).fill(-5n); c.includes(-5n) && !c.includes(5n) && c.indexOf(-5n) === 0"));
    EMB_CHECK(js_true(e, "new BigUint64Array(2).fill(2n ** 63n)[1] === 2n ** 63n"));
    EMB_CHECK(js_true(e, "new BigInt64Array([3n, -1n, 2n]).toSorted().join() === '-1,2,3'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_set_overlap_and_sort_detach)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var buf = new ArrayBuffer(8); var a = new Uint8Array(buf); a.set([1,2,3,4,5,6,7,8]);"
                         "a.set(new Uint16Array(buf, 0, 2), 1); a.join() === '1,1,3,4,5,6,7,8'"));
    EMB_CHECK(js_true(e, "var d = new Uint8Array([3,1,2]); var r = d.sort(function(x, y) { if (d.length) d.buffer.transfer(); return x - y; });"
                         "r === d && d.length === 0"));
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8}); var u = new Uint8Array(rab); u.set([4,3,2,1]);"
                         "u.sort(function(x, y) { rab.resize(2); return x - y; }); u.length === 2"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_species_short_and_tracking_subarray)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var s = new Uint8Array([1,2,3]);"
                         "s.constructor = {[Symbol.species]: function(n) { return new Uint8Array(1); }};"
                         "(function(){ try { s.map(function(v){ return v; }); return false; } catch (x) {"
                         " return x instanceof TypeError; } })()"));
    EMB_CHECK(js_true(e, "var rab = new ArrayBuffer(4, {maxByteLength: 8}); var t = new Uint8Array(rab);"
                         "var sub = t.subarray(1); rab.resize(8); sub.length === 7 && t.subarray(2, 4).length === 2"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(typedarray_base64_small_input_large_target)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var t = new Uint8Array(1000000); var r = t.setFromBase64('AQID'); "
                         "r.read === 4 && r.written === 3 && t[0] === 1 && t[2] === 3"));
    EMB_CHECK(js_true(e, "var h = new Uint8Array(1000000); var q = h.setFromHex('0aff'); q.written === 2 && h[1] === 255"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
