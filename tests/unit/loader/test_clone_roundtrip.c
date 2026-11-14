/*
 * Structured clone, successful round trips: primitives, BigInt, plain objects and arrays (holes,
 * integer keys, non-enumerable properties), cycles and shared references, Date, RegExp, Map, Set,
 * Error with cause, ArrayBuffer (resizable too), TypedArray and DataView sharing their buffer,
 * boxed primitives, and transfer detaching the source. Each case clones a value built by a script
 * and checks the copy with a script, so the checks see the engine's own view of the result.
 */
#include "loader_test.h"

#include "js/js.h"
#include "loader/clone.h"

#include <stdio.h>
#include <string.h>

static embi_js_runtime *g_rt;
static embi_js_realm *g_r;

/* Prints and clears a pending exception, so a failing case shows the reason. */
static void report_exception(void)
{
    embi_js_value exc = embi_js_exception_get(g_rt);
    embi_js_string *s = embi_js_to_string(g_rt, exc);
    embi_sb sb = {nullptr, 0, 0};
    if (s != nullptr) {
        embi_js_str_append_utf8(s, &sb);
    }
    fprintf(stderr, "    exception: %s\n", sb.data != nullptr ? embi_sb_cstr(&sb) : "(unprintable)");
    embi_sb_free(&sb);
    embi_js_exception_clear(g_rt);
}

static embi_js_value js_eval(const char *src)
{
    embi_js_value v = embi_js_eval(g_r, src, strlen(src), "clone_test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        report_exception();
        return EMBI_JS_UNDEFINED;
    }
    return v;
}

/* True when the script evaluates to true. */
static bool js_true(const char *src)
{
    embi_js_value v = embi_js_eval(g_r, src, strlen(src), "clone_test.js", 0);
    if (v == EMBI_JS_EXCEPTION) {
        report_exception();
        return false;
    }
    return v == EMBI_JS_TRUE;
}

static void set_global(const char *name, embi_js_value v)
{
    (void)embi_js_obj_set(g_rt, g_r->global, embi_js_key_c(g_rt, name), v,
                          embi_js_from_object(g_r->global));
}

static embi_js_value get_global(const char *name)
{
    return embi_js_obj_get(g_rt, g_r->global, embi_js_key_c(g_rt, name),
                           embi_js_from_object(g_r->global));
}

/* Serializes v, deserializes the bytes and returns the copy. */
static bool clone_value(embi_js_value v, embi_js_value *out)
{
    embi_sb sb = {nullptr, 0, 0};
    bool ok = embi_clone_serialize(g_r, v, nullptr, 0, &sb);
    if (!ok) {
        report_exception();
        embi_sb_free(&sb);
        return false;
    }
    ok = embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, out);
    if (!ok) {
        report_exception();
    }
    embi_sb_free(&sb);
    return ok;
}

/* Evaluates expr as globalThis.src, clones it into globalThis.c and reports success. */
static bool roundtrip(const char *expr)
{
    char src[2048];
    snprintf(src, sizeof src, "globalThis.src = (%s);", expr);
    (void)js_eval(src);
    embi_js_value v = get_global("src");
    embi_js_value copy = EMBI_JS_UNDEFINED;
    if (!clone_value(v, &copy)) {
        return false;
    }
    set_global("c", copy);
    return true;
}

static bool setup(void)
{
    g_rt = embi_js_runtime_new();
    if (g_rt == nullptr) {
        return false;
    }
    g_r = embi_js_realm_new(g_rt);
    return g_r != nullptr;
}

static void teardown(void)
{
    embi_js_realm_free(g_r);
    embi_js_runtime_free(g_rt);
    g_r = nullptr;
    g_rt = nullptr;
}

EMB_TEST(clone_primitives)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("undefined") && js_true("c === undefined"));
    EMB_CHECK(roundtrip("null") && js_true("c === null"));
    EMB_CHECK(roundtrip("[true, false]") && js_true("c[0] === true && c[1] === false"));
    EMB_CHECK(roundtrip("-0") && js_true("Object.is(c, -0)"));
    EMB_CHECK(roundtrip("NaN") && js_true("Number.isNaN(c)"));
    EMB_CHECK(roundtrip("2 ** 31") && js_true("c === 2147483648"));
    EMB_CHECK(roundtrip("-7") && js_true("c === -7"));
    EMB_CHECK(roundtrip("1.5e300") && js_true("c === 1.5e300"));
    EMB_CHECK(roundtrip("-Infinity") && js_true("c === -Infinity"));
    /* Lone surrogates are kept as code units. */
    EMB_CHECK(roundtrip("'h\\u00e9llo \\ud83d\\ude00 \\ud800'") &&
              js_true("c === 'h\\u00e9llo \\ud83d\\ude00 \\ud800' && c.charCodeAt(c.length - 1) === 0xD800"));
    EMB_CHECK(roundtrip("12345678901234567890123456789n") &&
              js_true("typeof c === 'bigint' && c === 12345678901234567890123456789n"));
    EMB_CHECK(roundtrip("-5n") && js_true("c === -5n"));
    teardown();
}

EMB_TEST(clone_plain_objects_and_arrays)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("({a: 1, 'b c': [1, , 3], nested: {deep: {x: 'y'}}, 7: 'seven'})"));
    EMB_CHECK(js_true("c.a === 1 && c['b c'].length === 3 && !(1 in c['b c'])"));
    EMB_CHECK(js_true("c['b c'][0] === 1 && c['b c'][2] === 3 && c.nested.deep.x === 'y'"));
    EMB_CHECK(js_true("c[7] === 'seven' && Object.keys(c).length === 4"));
    EMB_CHECK(js_true("Object.getPrototypeOf(c) === Object.prototype && Array.isArray(c['b c'])"));
    teardown();

    EMB_REQUIRE(setup());
    /* Non-enumerable properties are not copied. */
    EMB_CHECK(roundtrip("(function () { var o = {a: 1}; "
                        "Object.defineProperty(o, 'h', {value: 2, enumerable: false}); return o; })()"));
    EMB_CHECK(js_true("c.a === 1 && !('h' in c)"));
    teardown();
}

EMB_TEST(clone_cycles_and_shared_references)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("(function () { var o = {name: 'o'}; o.self = o; var s = {x: 1}; "
                        "return {o: o, arr: [o, o], s1: s, s2: s}; })()"));
    EMB_CHECK(js_true("c.o.self === c.o && c.arr[0] === c.o && c.arr[1] === c.o"));
    EMB_CHECK(js_true("c.s1 === c.s2 && c.s1 !== c.o && c.o.name === 'o'"));
    teardown();

    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("(function () { var a = [1]; a.push(a); return a; })()"));
    EMB_CHECK(js_true("c.length === 2 && c[1] === c && c[0] === 1"));
    teardown();
}

EMB_TEST(clone_date_regexp_map_set)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("new Date(1234567890123)"));
    EMB_CHECK(js_true("c instanceof Date && c.getTime() === 1234567890123"));
    EMB_CHECK(roundtrip("new Date(NaN)") && js_true("Number.isNaN(c.getTime())"));
    EMB_CHECK(roundtrip("/a(b+)c/gi") &&
              js_true("c instanceof RegExp && c.source === 'a(b+)c' && c.flags === 'gi'"));
    EMB_CHECK(js_true("c.global === true && c.lastIndex === 0"));
    teardown();

    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("(function () { var k = {}; "
                        "return {m: new Map([[1, 'a'], [k, 'b']]), s: new Set(['x', k])}; })()"));
    EMB_CHECK(js_true("c.m instanceof Map && c.m.size === 2 && c.m.get(1) === 'a'"));
    EMB_CHECK(js_true("c.s instanceof Set && c.s.size === 2 && c.s.has('x')"));
    /* The object used as a key in the Map and as a member of the Set is one object. */
    EMB_CHECK(js_true("[...c.s].find(function (x) { return typeof x === 'object'; }) === "
                      "[...c.m.keys()].find(function (x) { return typeof x === 'object'; })"));
    EMB_CHECK(js_true("[...c.m.values()][1] === 'b'"));
    teardown();
}

EMB_TEST(clone_errors_keep_name_message_cause)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("new RangeError('boom', {cause: 7})"));
    EMB_CHECK(js_true("c instanceof RangeError && c.message === 'boom' && c.cause === 7"));
    EMB_CHECK(js_true("c.name === 'RangeError' && Object.prototype.toString.call(c) === '[object Error]'"));
    teardown();

    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("new TypeError('t')") && js_true("c instanceof TypeError && c.message === 't'"));
    teardown();

    EMB_REQUIRE(setup());
    /* A cycle through the cause is kept. */
    EMB_CHECK(roundtrip("(function () { var e = new Error('outer'); e.cause = e; return e; })()"));
    EMB_CHECK(js_true("c.cause === c && c.message === 'outer'"));
    teardown();
}

EMB_TEST(clone_buffers_views_share_one_buffer)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("(function () { var b = new ArrayBuffer(8); "
                        "new Uint8Array(b).set([1, 2, 3, 4, 5, 6, 7, 8]); "
                        "return {u: new Uint8Array(b, 2, 3), d: new DataView(b, 4), b: b, "
                        "f: new Float64Array(b, 0, 1), big: new BigInt64Array(1)}; })()"));
    EMB_CHECK(js_true("c.b instanceof ArrayBuffer && c.b.byteLength === 8"));
    EMB_CHECK(js_true("c.u.buffer === c.b && c.u.byteOffset === 2 && c.u.length === 3 && c.u[0] === 3"));
    EMB_CHECK(js_true("c.d.buffer === c.b && c.d.byteOffset === 4 && c.d.byteLength === 4"));
    EMB_CHECK(js_true("c.d.getUint8(0) === 5 && c.f.buffer === c.b && c.f.length === 1"));
    EMB_CHECK(js_true("c.big instanceof BigInt64Array && c.big.length === 1"));
    teardown();

    EMB_REQUIRE(setup());
    /* A length-tracking view stays length-tracking, and the resizable buffer keeps its maximum. */
    EMB_CHECK(roundtrip("(function () { var b = new ArrayBuffer(8, {maxByteLength: 16}); "
                        "return {t: new Uint8Array(b, 1), r: b}; })()"));
    EMB_CHECK(js_true("c.r.resizable === true && c.r.maxByteLength === 16 && c.t.length === 7"));
    EMB_CHECK(js_true("(c.r.resize(12), c.t.length === 11)"));
    teardown();
}

EMB_TEST(clone_boxed_primitives)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("[new Number(3), new String('s'), new Boolean(false), Object(4n)]"));
    EMB_CHECK(js_true("c[0] instanceof Number && c[0].valueOf() === 3"));
    EMB_CHECK(js_true("c[1] instanceof String && c[1].valueOf() === 's'"));
    EMB_CHECK(js_true("c[2] instanceof Boolean && c[2].valueOf() === false"));
    EMB_CHECK(js_true("typeof c[3].valueOf() === 'bigint' && c[3].valueOf() === 4n"));
    teardown();
}

EMB_TEST(clone_transfer_detaches_source)
{
    EMB_REQUIRE(setup());
    (void)js_eval("globalThis.src = {b: new ArrayBuffer(4)}; new Uint8Array(src.b)[0] = 9;");
    embi_js_value obj = get_global("src");
    embi_js_value bv = embi_js_obj_get(g_rt, embi_js_as_object(obj), embi_js_key_c(g_rt, "b"), obj);
    embi_sb sb = {nullptr, 0, 0};
    bool ok = embi_clone_serialize(g_r, obj, &bv, 1, &sb);
    EMB_CHECK(ok);
    if (!ok) {
        report_exception();
    }
    EMB_CHECK(js_true("globalThis.src.b.byteLength === 0"));
    embi_js_value copy = EMBI_JS_UNDEFINED;
    EMB_CHECK(embi_clone_deserialize(g_r, (embi_str){sb.data, sb.len}, &copy));
    set_global("c", copy);
    EMB_CHECK(js_true("c.b.byteLength === 4 && new Uint8Array(c.b)[0] === 9"));
    embi_sb_free(&sb);
    teardown();
}

EMB_TEST(clone_large_string_and_array)
{
    EMB_REQUIRE(setup());
    EMB_CHECK(roundtrip("(function () { var s = ''; for (var i = 0; i < 20000; i++) s += 'ab'; "
                        "var a = []; for (var j = 0; j < 5000; j++) a.push(j * 2); "
                        "return {s: s, a: a}; })()"));
    EMB_CHECK(js_true("c.s.length === 40000 && c.a.length === 5000 && c.a[4999] === 9998"));
    teardown();
}

LOADER_TEST_MAIN()
