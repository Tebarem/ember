/* J5m2 fast paths: single-block closures with define_fresh,
 * RegExp match arrays built on cached shapes, the for-in key cache, and JSON parse and stringify
 * fast paths. Each case runs the fast path, then changes the world underneath it (shadowing,
 * prototype changes, deletes, accessors, proxies, dictionary mode, index keys) and checks the result
 * against what the generic path gives. */
#include "es6_util.h"

#include "js/heap.h"
#include "js/interp.h"
#include "js/object.h"

#include <string.h>

/* Closures: the record and the upvalues share one block, and length/name/prototype come in order. */
EMB_TEST(js_fast_m2_closure_props)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_TEXT(e, "Object.getOwnPropertyNames(function g(a, b) {}).join()", "length,name,prototype");
    E6_TEXT(e, "Object.getOwnPropertyNames(() => 1).join()", "length,name");
    E6_TEXT(e, "(() => 1).name === '' ? 'empty' : 'bad'", "empty");
    E6_NUM(e, "(function (a, b, ...c) {}).length", 2);
    E6_TEXT(e, "var f = function named() {}; f.name", "named");
    E6_TEXT(e, "typeof (function* () {}).prototype", "object");
    E6_TEXT(e, "Object.getOwnPropertyNames(function* gg() {}).join()", "length,name,prototype");
    E6_TEXT(e, "var F = function () {}; F.prototype.constructor === F ? 'ok' : 'bad'", "ok");
    E6_TEXT(e, "var F2 = function () {}; Object.getOwnPropertyDescriptor(F2, 'prototype').writable "
               "+ ',' + Object.getOwnPropertyDescriptor(F2, 'prototype').enumerable",
            "true,false");
    E6_TEXT(e, "var d = Object.getOwnPropertyDescriptor(function q() {}, 'name'); "
               "[d.writable, d.enumerable, d.configurable].join()",
            "false,false,true");
    es6_env_free(e);
}

EMB_TEST(js_fast_m2_closure_upvalues)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_NUM(e,
           "function mk() { let c = 0; return () => ++c; }"
           "var k1 = mk(), k2 = mk(); k1(); k1(); k2();"
           "k1() * 10 + k2()",
           32);
    /* Many upvalues: the block holds all of them, and each one is traced. */
    E6_NUM(e,
           "function many() { var a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9,j=10;"
           "  return function () { return a+b+c+d+e+f+g+h+i+j; }; }"
           "var mf = many(); var s = 0; for (var n = 0; n < 2000; n++) { s += mf(); }"
           "s",
           110000);
    es6_env_free(e);
}

/* RegExp match results: the own keys come in the spec order, and the values are right. */
EMB_TEST(js_fast_m2_regexp_result)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_TEXT(e, "Object.getOwnPropertyNames(/(a)(b)?/.exec('xa')).join()", "0,1,2,length,index,input,groups");
    E6_TEXT(e, "Object.getOwnPropertyNames(/(a)/d.exec('xa')).join()",
            "0,1,length,index,input,groups,indices");
    E6_NUM(e, "/(a)/.exec('xa').index", 1);
    E6_TEXT(e, "/(a)(b)?/.exec('xa')[2] === undefined ? 'u' : 'd'", "u");
    E6_TEXT(e, "'2024-01'.match(/(?<y>\\d+)-(?<m>\\d+)/).groups.m", "01");
    E6_TEXT(e, "Object.getPrototypeOf(/(?<y>a)/.exec('a').groups) === null ? 'null' : 'proto'", "null");
    E6_TEXT(e, "/(?<y>a)|(?<z>b)/d.exec('b').indices.groups.z.join()", "0,1");
    E6_TEXT(e, "String(/(a)(?<n>b)?/d.exec('a').indices.groups.n)", "undefined");
    E6_TEXT(e, "/a/d.exec('a').indices.groups === undefined ? 'u' : 'd'", "u");
    E6_TEXT(e, "/(a)(b)/d.exec('ab').indices.join('|')", "0,2|0,1|1,2");
    E6_TEXT(e, "JSON.stringify(/(x)(y)?/.exec('x'))", "[\"x\",\"x\",null]");
    E6_TEXT(e, "/x/.exec('y') === null ? 'null' : 'bad'", "null");
    /* A user index on Array.prototype breaks the protector; the captures are own properties anyway. */
    E6_TEXT(e, "Array.prototype[1] = 'P'; var mm = /(a)(b)?/.exec('a'); delete Array.prototype[1];"
               " mm[2] === undefined && mm.hasOwnProperty(2) ? 'own' : 'bad'",
            "own");
    es6_env_free(e);
}

/* The for-in key cache: every call after the first may use it, and each change of the chain shows. */
EMB_TEST(js_fast_m2_forin_cache)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_TEXT(e,
            "function keys(o) { var r = []; for (var k in o) r.push(k); return r.join(); }"
            "var o = { a: 1, b: 2 }; var x1 = keys(o), x2 = keys(o), x3 = keys(o); x1 + '|' + x2 + '|' + x3",
            "a,b|a,b|a,b");
    E6_TEXT(e, "o.z = 3; keys(o)", "a,b,z");
    E6_TEXT(e, "Object.defineProperty(o, 'a', { enumerable: false }); keys(o)", "b,z");
    E6_TEXT(e, "var p = { q: 1 }; Object.setPrototypeOf(o, p); keys(o)", "b,z,q");
    E6_TEXT(e, "p.w = 1; keys(o)", "b,z,q,w");
    E6_TEXT(e, "delete p.q; keys(o)", "b,z,w");
    E6_TEXT(e, "Object.defineProperty(p, 'w', { enumerable: false }); keys(o)", "b,z");
    E6_TEXT(e, "var q2 = { s: 1 }; var o2 = Object.create(q2); keys(o2)", "s");
    E6_TEXT(e, "Object.defineProperty(q2, 's', { enumerable: false }); keys(o2)", "");
    E6_TEXT(e, "o2.s = 5; keys(o2)", "s");
    E6_TEXT(e, "var p3 = {}; var o3 = Object.create(p3); keys(o3) + '|' + keys(o3)", "|");
    E6_TEXT(e, "p3[5] = 'x'; keys(o3)", "5");
    E6_TEXT(e, "var sy = Symbol('t'); var o4 = { [sy]: 1, a: 2 }; keys(o4) + keys(o4)", "aa");
    E6_TEXT(e, "keys([1, 2]) + '|' + keys([1, 2])", "0,1|0,1");
    E6_TEXT(e, "var arrx = [1]; arrx.extra = 1; keys(arrx)", "0,extra");
    E6_TEXT(e, "keys(new Proxy({ a: 1, b: 2 }, {})) + keys(new Proxy({ a: 1 }, {}))", "a,ba");
    /* Deletes during the loop are skipped, as before the cache existed. */
    E6_TEXT(e,
            "var d = { a: 1, b: 2, c: 3 }; var seen = []; for (var k in d) { seen.push(k); delete d.b; }"
            "seen.join()",
            "a,c");
    /* Dictionary mode: more than 64 named keys, so the object never uses the cache. */
    E6_TEXT(e,
            "var big = {}; for (var i = 0; i < 70; i++) big['k' + i] = i; var n1 = keys(big).split(',').length;"
            "var n2 = keys(big).split(',').length; n1 + ',' + n2",
            "70,70");
    es6_env_free(e);
}

EMB_TEST(js_fast_m2_json_parse)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_TEXT(e, "Object.is(JSON.parse('-0'), -0) ? 'neg' : 'pos'", "neg");
    E6_TEXT(e, "Object.is(JSON.parse('0'), 0) ? 'pos' : 'bad'", "pos");
    E6_TEXT(e, "[JSON.parse('12'), JSON.parse('-7'), JSON.parse('1.5'), JSON.parse('1e2')].join()",
            "12,-7,1.5,100");
    E6_TEXT(e, "String(JSON.parse('999999999999999'))", "999999999999999");
    E6_TEXT(e, "String(JSON.parse('1234567890123456789'))", "1234567890123456800");
    E6_TEXT(e, "String(JSON.parse('-123456789012345'))", "-123456789012345");
    E6_TEXT(e, "String(JSON.parse('00012.5e-1'.replace('00012', '12')))", "1.25");
    E6_TEXT(e, "String(JSON.parse('\"abc\"'))", "abc");
    E6_NUM(e, "JSON.parse('\"h\u00e9llo\"').length", 5);
    E6_NUM(e, "JSON.parse('\"\u20acx\"').length", 2);
    E6_NUM(e, "JSON.parse('\"\"').length", 0);
    E6_NUM(e, "JSON.parse('\"a\\\\nb\"').length", 3);
    E6_TEXT(e, "Object.keys(JSON.parse('{\"b\":1,\"a\":2,\"0\":3}')).join()", "0,b,a");
    E6_TEXT(e, "String(JSON.parse('{\"a\":1,\"a\":2}').a)", "2");
    E6_TEXT(e, "Object.keys(JSON.parse('{\"a\":1,\"a\":2}')).join()", "a");
    E6_TEXT(e, "Object.keys(JSON.parse('{\"__proto__\":1}')).join()", "__proto__");
    E6_TEXT(e, "Object.getPrototypeOf(JSON.parse('{\"x\":1}')) === Object.prototype ? 'op' : 'bad'", "op");
    E6_TEXT(e, "var r = JSON.parse('[1,{\"k\":[2,\"s\"]}]'); r[1].k[1]", "s");
    E6_TEXT(e, "JSON.parse('{\"a\\\\u0041\":1}').aA === undefined ? 'missing' : 'got'", "got");
    E6_TEXT(e, "JSON.parse('{}', function (k, v) { return v; }) + ''", "[object Object]");
    /* Numbers and strings through the reviver keep their source text (json-parse-with-source). */
    E6_TEXT(e, "JSON.parse('[1.0,\"x\"]', (k, v, c) => (c && c.source) || v).join()", "1.0,\"x\"");
    es6_env_free(e);
}

EMB_TEST(js_fast_m2_json_stringify)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    E6_TEXT(e, "JSON.stringify({ b: 1, a: 'x\"y', c: [1, null, undefined], d: -0, e: 1.5 })",
            "{\"b\":1,\"a\":\"x\\\"y\",\"c\":[1,null,null],\"d\":0,\"e\":1.5}");
    E6_TEXT(e, "JSON.stringify({ '\\ud800': 1, a: '\\u0001' })", "{\"\\ud800\":1,\"a\":\"\\u0001\"}");
    E6_TEXT(e, "JSON.stringify('\u20ac\"')", "\"\u20ac\\\"\"");
    E6_TEXT(e, "JSON.stringify({ a: 1, b: undefined, c: function () {} })", "{\"a\":1}");
    E6_TEXT(e, "JSON.stringify(Object.defineProperty({ a: 1 }, 'h', { value: 2, enumerable: false }))", "{\"a\":1}");
    E6_TEXT(e, "JSON.stringify(Object.defineProperty({}, 'g', { get() { return 7; }, enumerable: true }))",
            "{\"g\":7}");
    E6_TEXT(e, "JSON.stringify({ [Symbol('s')]: 1, a: 2 })", "{\"a\":2}");
    E6_TEXT(e, "var o = { a: 1, b: { toJSON() { delete o.c; return 2; } }, c: 3 };"
               "JSON.stringify(o)",
            "{\"a\":1,\"b\":2}");
    E6_TEXT(e, "JSON.stringify({ a: [1, { b: 'c' }] }, null, 1)",
            "{\n \"a\": [\n  1,\n  {\n   \"b\": \"c\"\n  }\n ]\n}");
    E6_TEXT(e, "JSON.stringify({ a: 1, b: 2, c: 3 }, ['c', 'a'])", "{\"c\":3,\"a\":1}");
    E6_TEXT(e, "JSON.stringify([new Number(3), new String('s'), new Boolean(false)])", "[3,\"s\",false]");
    E6_TEXT(e, "JSON.stringify(Object.create({ inherited: 1 }))", "{}");
    E6_TEXT(e, "var arr = [1, 2]; arr.x = 3; JSON.stringify(arr)", "[1,2]");
    E6_TEXT(e, "JSON.stringify({ '1': 'one', b: 'bee', '0': 'zero' })", "{\"0\":\"zero\",\"1\":\"one\",\"b\":\"bee\"}");
    E6_TEXT(e, "JSON.stringify(Object.assign(Object.create(null), { k: 'v' }))", "{\"k\":\"v\"}");
    E6_TEXT(e, "JSON.stringify(JSON.parse('{\"p\":[1.25,-0.5,\"a\\\\\\\\b\"]}'))",
            "{\"p\":[1.25,-0.5,\"a\\\\b\"]}");
    es6_env_free(e);
}

/* The fast paths allocate and free through the same sizes: a runtime that ran all of them releases
 * every byte. The single-block closure is freed with its upvalue array's size. */
EMB_TEST(js_fast_m2_releases_all_memory)
{
    size_t before = embi_mem_live();
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    /* Per iteration: (i + 1) + 3 (the keys) + 4 (index 1 and three indices) + 3 (the parsed array)
     * = i + 11, so the sum over 0..299 is 44850 + 3300. */
    E6_NUM(e,
           "var acc = 0; for (var i = 0; i < 300; i++) {"
           "  var n = i; var f = () => n + 1; acc += f();"
           "  var o = { a: 1, b: 2, c: i }; for (var k in o) acc += k.length;"
           "  var m = /(a)(?<g>b)?/d.exec('xab'); acc += m.index + m.indices.length;"
           "  acc += JSON.parse(JSON.stringify({ p: [i, 1.5, 'q'] })).p.length;"
           "}"
           "acc",
           48150);
    es6_env_free(e);
    EMB_CHECK(embi_mem_live() == before);
}

/* The same fast paths under a collection before every allocation (the test hook of heap.h). Per
 * iteration: i (the closure) + 3 (the keys) + 4 (the match) + 1 (the parsed value) + the length of
 * '{"z":i}' (7, or 8 for i >= 10): 66 + 36 + 48 + 12 + 86 = 248. */
EMB_TEST(js_fast_m2_gc_stress)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt && e.r);
    embi_js_gc_set_stress(e.rt, 1);
    E6_NUM(e,
           "function keys(o) { var r = []; for (var k in o) r.push(k); return r.join(); }"
           "var acc = 0; for (var i = 0; i < 12; i++) {"
           "  var f = (function (v) { return () => v; })(i); acc += f();"
           "  var o = { x: i, y: 2 }; acc += keys(o).length;"
           "  var m = /(a)(?<g>b)?/d.exec('xab'); acc += m.indices.length + m.index;"
           "  acc += JSON.parse('{\"k\":[1,\"s\"]}').k[0] + JSON.stringify({ z: i }).length;"
           "}"
           "acc",
           248);
    embi_js_gc_set_stress(e.rt, 0);
    es6_env_free(e);
}

EMB_TEST_MAIN()
