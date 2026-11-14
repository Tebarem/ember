#include "emb_test.h"

#include "js/js.h"

#include <string.h>

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    /* The Symbol global is not part of J1, so the well-known symbols reach the scripts as globals. */
    if (e.r) {
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_species"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_SPECIES), 0);
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_spreadable"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_IS_CONCAT_SPREADABLE), 0);
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_unscopables"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_UNSCOPABLES), 0);
    }
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static embi_js_value run(env e, const char *src)
{
    return embi_js_eval(e.r, src, strlen(src), "test.js", 0);
}

static bool text_is(embi_js_runtime *rt, embi_js_value v, const char *want)
{
    embi_js_string *s = embi_js_to_string(rt, v);
    if (!s) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

static bool exc_name_is(env e, const char *want)
{
    embi_js_runtime *rt = e.rt;
    embi_js_value ex = embi_js_exception_get(rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(rt, embi_js_as_object(ex), embi_js_key_c(rt, "name"));
        ok = embi_js_is_string(n) && text_is(rt, n, want);
    }
    embi_js_exception_clear(rt);
    return ok;
}

static bool check_text(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v == EMBI_JS_EXCEPTION) {
        SDL_Log("    js threw (wanted \"%s\"): %s", want, src);
        embi_js_exception_clear(e.rt);
        return false;
    }
    bool ok = text_is(e.rt, v, want);
    if (!ok) {
        SDL_Log("    js: %s", src);
    }
    return ok;
}

static bool check_throws(env e, const char *src, const char *want)
{
    embi_js_value v = run(e, src);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    js did not throw (wanted %s): %s", want, src);
        return false;
    }
    bool ok = exc_name_is(e, want);
    if (!ok) {
        SDL_Log("    js threw the wrong error (wanted %s): %s", want, src);
    }
    return ok;
}

#define T_TEXT(src_, want_) EMB_CHECK(check_text(e, (src_), (want_)))
#define T_THROWS(src_, want_) EMB_CHECK(check_throws(e, (src_), (want_)))

/* ------------------------------------------------------------------ constructor and statics */

EMB_TEST(array_constructor_forms)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Array.length + ':' + Array.name", "1:Array");
    T_TEXT("Array(3).length + ':' + new Array(3).length", "3:3");
    T_TEXT("Array('3').length + ':' + Array('3')[0]", "1:3");
    T_TEXT("new Array(1, 2, 3).join('-')", "1-2-3");
    T_TEXT("Array().length + ':' + Array.isArray(Array())", "0:true");
    T_TEXT("Array.prototype.length + ':' + Array.isArray(Array.prototype)", "0:true");
    T_THROWS("new Array(-1)", "RangeError");
    T_THROWS("new Array(1.5)", "RangeError");
    T_THROWS("new Array(4294967296)", "RangeError");
    T_TEXT("Array.isArray([]) + ',' + Array.isArray({length: 0}) + ',' + Array.isArray(null)",
           "true,false,false");
    T_TEXT("Array.of(7).length + ':' + Array.of(7)[0] + ':' + Array.of(1, 'a').join()",
           "1:7:1,a");
    T_TEXT("Array[S_species] === Array", "true");
    T_TEXT("var sd = Object.getOwnPropertyDescriptor(Array, 'prototype');"
           "'' + sd.writable + sd.enumerable + sd.configurable",
           "falsefalsefalse");
    T_TEXT("var lp = Object.getOwnPropertyDescriptor(Array.prototype, 'constructor');"
           "'' + (lp.value === Array) + lp.writable + lp.enumerable + lp.configurable",
           "truetruefalsetrue");
    env_free(e);
}

EMB_TEST(array_method_lengths_and_attributes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var want = {at:1, concat:1, copyWithin:2, every:1, fill:1, filter:1, find:1,"
           " findIndex:1, findLast:1, findLastIndex:1, flat:0, flatMap:1, forEach:1, includes:1,"
           " indexOf:1, join:1, lastIndexOf:1, map:1, pop:0, push:1, reduce:1, reduceRight:1,"
           " reverse:0, shift:0, slice:2, some:1, sort:1, splice:2, toLocaleString:0,"
           " toString:0, unshift:1};"
           "var bad = [];"
           "for (var k in want) {"
           "  var f = Array.prototype[k];"
           "  if (typeof f !== 'function' || f.length !== want[k] || f.name !== k) bad.push(k);"
           "  var d = Object.getOwnPropertyDescriptor(Array.prototype, k);"
           "  if (d.enumerable || !d.writable || !d.configurable) bad.push('attr:' + k);"
           "}"
           "bad.join(',') + '|'",
           "|");
    T_TEXT("Object.getOwnPropertyDescriptor(Array.prototype, S_unscopables).writable + ''",
           "false");
    T_TEXT("var u = Array.prototype[S_unscopables];"
           "(Object.getPrototypeOf(u) === null) + ':' + u.find + ':' + u.includes + ':' + u.sort",
           "true:true:true:undefined");
    env_free(e);
}

/* ------------------------------------------------------------------ mutators */

EMB_TEST(array_push_pop_shift_unshift)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var a = [1]; (a.push(2, 3)) + ':' + a.join()", "3:1,2,3");
    T_TEXT("var b = []; b.pop() === undefined && b.length === 0", "true");
    T_TEXT("var c = [1, 2]; (c.pop()) + ':' + c.length", "2:1");
    T_TEXT("var d = [1, 2, 3]; (d.shift()) + ':' + d.join()", "1:2,3");
    T_TEXT("var f = []; (f.shift() === undefined) + ':' + f.length", "true:0");
    T_TEXT("var g = [2, 3]; (g.unshift(0, 1)) + ':' + g.join()", "4:0,1,2,3");
    T_TEXT("var h = [,2]; h.unshift(0); (0 in h) + ',' + (1 in h) + ',' + h.length", "true,false,3");
    T_TEXT("var r = [1, 2, 3, 4]; r.reverse() === r && r.join() === '4,3,2,1'", "true");
    T_TEXT("var rh = [1, , 3]; rh.reverse(); (0 in rh) + ',' + (1 in rh) + ',' + rh[0]", "true,false,3");
    /* Holes are kept as holes by shift. */
    T_TEXT("var sh = [1, , 3]; sh.shift(); (0 in sh) + ',' + (1 in sh) + ',' + sh.length",
           "false,true,2");
    env_free(e);
}

EMB_TEST(array_splice_and_copy_within_and_fill)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var a = [1, 2, 3, 4, 5]; var r = a.splice(1, 2); r.join() + '|' + a.join()",
           "2,3|1,4,5");
    T_TEXT("var b = [1, 2, 3]; var s = b.splice(1, 0, 'x', 'y'); s.length + '|' + b.join()",
           "0|1,x,y,2,3");
    T_TEXT("var c = [1, 2, 3]; c.splice(-1); c.join()", "1,2");
    T_TEXT("var d = [1, 2, 3]; d.splice(); d.length + ':' + d.join()", "3:1,2,3");
    T_TEXT("var f = [1, 2, 3]; f.splice(1).join() + '|' + f.join()", "2,3|1");
    T_TEXT("[1, 2, 3, 4, 5].copyWithin(0, 3).join()", "4,5,3,4,5");
    T_TEXT("[1, 2, 3, 4, 5].copyWithin(1, 0, 3).join()", "1,1,2,3,5");
    T_TEXT("[1, 2, 3].fill(0, 1).join()", "1,0,0");
    T_TEXT("[1, 2, 3, 4].fill(9, -2, -1).join()", "1,2,9,4");
    T_TEXT("var z = [1, 2, 3]; z.fill(7) === z && z.join() === '7,7,7'", "true");
    env_free(e);
}

/* ------------------------------------------------------------------ accessors and search */

EMB_TEST(array_concat_join_slice_to_string)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("[1, 2].concat([3], 4, [[5]]).length + ':' + [1].concat([2, 3]).join()", "5:1,2,3");
    T_TEXT("var holes = [1, , 3].concat([4]); (1 in holes) + ',' + holes.length", "false,4");
    T_TEXT("var spread = {length: 2, 0: 'a', 1: 'b'}; spread[S_spreadable] = true;"
           "[].concat(spread).join()",
           "a,b");
    T_TEXT("var noSpread = [1, 2]; noSpread[S_spreadable] = false;"
           "[].concat(noSpread).length + ''",
           "1");
    T_TEXT("[1, 2, 3, 4].slice(1, -1).join()", "2,3");
    T_TEXT("[1, 2, 3].slice(-2).join() + '|' + [1, 2, 3].slice(5).length", "2,3|0");
    T_TEXT("[null, undefined, 1, , 'x'].join('-')", "--1--x");
    T_TEXT("[1, [2, [3]]].toString()", "1,2,3");
    T_TEXT("[1, 2].join(undefined)", "1,2");
    T_TEXT("Array.prototype.toString.call({join: function () { return 'custom'; }})", "custom");
    T_TEXT("Array.prototype.toString.call({})", "[object Object]");
    T_TEXT("[{toLocaleString: function () { return 'x'; }}, null, {toLocaleString: function () { return 'y'; }}].toLocaleString()", "x,,y");
    T_TEXT("var cyc = [1]; cyc.push(cyc); try { cyc.join(); 'no' } catch (x) {"
           " (x instanceof RangeError) + '' }",
           "true");
    env_free(e);
}

EMB_TEST(array_index_of_and_includes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("[1, 2, 1].indexOf(1) + ':' + [1, 2, 1].indexOf(1, 1) + ':' + [1].indexOf(2)",
           "0:2:-1");
    T_TEXT("[1, 2, 1].lastIndexOf(1) + ':' + [1, 2, 1].lastIndexOf(1, 1) + ':' + "
           "[1, 2, 1].lastIndexOf(1, -2)",
           "2:0:0");
    T_TEXT("[NaN].indexOf(NaN) + ':' + [NaN].includes(NaN) + ':' + [, 1].includes(undefined)",
           "-1:true:true");
    T_TEXT("[, 1].indexOf(undefined) + ':' + [].lastIndexOf(1)", "-1:-1");
    T_TEXT("[1, 2, 3].includes(3, -1) + ':' + [1, 2, 3].includes(1, 1)", "true:false");
    T_TEXT("[0].includes(-0) + ':' + [-0].indexOf(0)", "true:0");
    T_TEXT("var f = [1, 2, 3]; f.find(function (x) { return x > 1; }) + ':' + "
           "f.findIndex(function (x) { return x > 5; }) + ':' + "
           "f.findLast(function (x) { return x < 3; }) + ':' + "
           "f.findLastIndex(function (x) { return x < 3; })",
           "2:-1:2:1");
    T_TEXT("[1, , 3].find(function (x) { return x === undefined; }) === undefined && "
           "[1, , 3].findIndex(function (x) { return x === undefined; }) === 1",
           "true");
    T_TEXT("[1, 2, 3].at(-1) + ':' + [1, 2, 3].at(0) + ':' + ([1].at(3) === undefined)",
           "3:1:true");
    env_free(e);
}

/* ------------------------------------------------------------------ iteration */

EMB_TEST(array_iteration_methods_skip_holes_except_find)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var n = 0; [1, , 3].forEach(function () { n++; }); n + ''", "2");
    T_TEXT("var seen = []; [1, , 3].find(function (x, i) { seen.push(i); return false; });"
           "seen.join()",
           "0,1,2");
    T_TEXT("[1, 2, 3].map(function (x) { return x * 2; }).join()", "2,4,6");
    T_TEXT("var m = [1, , 3].map(function (x) { return x; }); (1 in m) + ',' + m.length",
           "false,3");
    T_TEXT("[1, 2, 3, 4].filter(function (x) { return x % 2; }).join()", "1,3");
    T_TEXT("[1, 2, 3].some(function (x) { return x > 2; }) + ':' + "
           "[1, 2, 3].every(function (x) { return x > 2; })",
           "true:false");
    T_TEXT("[].every(function () { return false; }) + ':' + [].some(function () { return true; })",
           "true:false");
    T_TEXT("[1, 2, 3].reduce(function (a, b) { return a + b; })", "6");
    T_TEXT("[1, 2, 3].reduceRight(function (a, b) { return a + '' + b; }, '')", "321");
    T_TEXT("[, , 5].reduce(function (a, b) { return a + b; })", "5");
    T_THROWS("[].reduce(function () {})", "TypeError");
    T_THROWS("[, ,].reduceRight(function () {})", "TypeError");
    T_THROWS("[1].map(3)", "TypeError");
    T_TEXT("var ctx = {k: 10}; [1].map(function (x) { return x + this.k; }, ctx)[0] + ''", "11");
    T_TEXT("[[1, 2], [3], 4].flat().join()", "1,2,3,4");
    T_TEXT("[1, [2, [3, [4]]]].flat(2).length + ':' + [1, [2, [3]]].flat(Infinity).join()",
           "4:1,2,3");
    T_TEXT("[1, 2].flatMap(function (x) { return [x, x * 10]; }).join()", "1,10,2,20");
    T_TEXT("[1, , 2].flat().length + ':' + [1, , 2].flat().hasOwnProperty(1)", "2:true");
    env_free(e);
}

/* ------------------------------------------------------------------ sort */

EMB_TEST(array_sort_default_and_comparator)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("[10, 9, 1, 100].sort().join()", "1,10,100,9");
    T_TEXT("[3, undefined, 1, , 2].sort().join() + '|' + [3, undefined, 1, , 2].sort().length",
           "1,2,3,,|5");
    T_TEXT("var s = [3, , undefined, 1]; s.sort(); (s.length) + ':' + (3 in s) + ':' + "
           "(2 in s) + ':' + (s[0]) + ':' + (s[2] === undefined) + ':' + (s[3] === undefined)",
           "4:false:true:1:true:true");
    T_TEXT("[3, 1, 2].sort(function (a, b) { return b - a; }).join()", "3,2,1");
    T_TEXT("[3, 1, NaN, 2].sort(function () { return NaN; }).join()", "3,1,NaN,2");
    /* Stable: equal keys keep their original order. */
    T_TEXT("var st = [];"
           "for (var i = 0; i < 40; i++) st.push({k: i % 3, i: i});"
           "st.sort(function (x, y) { return x.k - y.k; });"
           "var ok = true; for (var j = 1; j < st.length; j++) {"
           "  if (st[j - 1].k === st[j].k && st[j - 1].i > st[j].i) ok = false; }"
           "ok + ':' + st.length",
           "true:40");
    T_THROWS("[1, 2].sort(1)", "TypeError");
    T_TEXT("var thrower = [2, 1]; try { thrower.sort(function () { throw new RangeError('x'); }) }"
           " catch (x) { (x instanceof RangeError) + '' }",
           "true");
    /* An inconsistent comparator still terminates and keeps the same elements. */
    T_TEXT("var big = []; for (var q = 0; q < 200; q++) big.push(q);"
           "big.sort(function (a, b) { return (a * 31 + b * 17) % 3 - 1; });"
           "var sum = 0; for (var w = 0; w < big.length; w++) sum += big[w];"
           "big.length + ':' + sum",
           "200:19900");
    env_free(e);
}

/* ------------------------------------------------------------------ generic and exotic receivers */

EMB_TEST(array_methods_are_generic_over_array_likes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var o = {length: 2, 0: 'a', 1: 'b'}; Array.prototype.push.call(o, 'c');"
           "o.length + ':' + o[2]",
           "3:c");
    T_TEXT("Array.prototype.join.call({length: 3, 1: 'x'}, '+')", "+x+");
    T_TEXT("Array.prototype.map.call('abc', function (c) { return c + '!'; }).join('')",
           "a!b!c!");
    T_TEXT("Array.prototype.indexOf.call('hello', 'l') + ''", "2");
    T_TEXT("var rv = Array.prototype.reverse.call({length: 2, 0: 1}); rv[1] + ':' + (0 in rv)",
           "1:false");
    T_TEXT("var sl = Array.prototype.slice.call({length: 3, 0: 'p', 2: 'q'}); sl.length + ':' + "
           "(1 in sl)",
           "3:false");
    T_TEXT("Array.prototype.join.call(undefined === undefined ? 5 : 0)", "");
    T_THROWS("Array.prototype.join.call(null)", "TypeError");
    T_THROWS("Array.prototype.push.call({length: 9007199254740991}, 1)", "TypeError");
    env_free(e);
}

EMB_TEST(array_prototype_holes_and_accessors_are_seen)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("Array.prototype[1] = 'p'; var a = [0, , 2]; var r = a.join() + '|' + a.indexOf('p');"
           "delete Array.prototype[1]; r",
           "0,p,2|1");
    T_TEXT("Array.prototype[1] = 'q'; var seen = 0; [0, , 2].forEach(function (v, i) {"
           " if (i === 1 && v === 'q') seen++; }); delete Array.prototype[1]; seen + ''",
           "1");
    T_TEXT("var g = [1, 2, 3]; var calls = 0;"
           "Object.defineProperty(g, 1, {get: function () { calls++; return 9; }, configurable: true});"
           "g.map(function (x) { return x; }).join() + ':' + calls",
           "1,9,3:1");
    T_TEXT("var lst = [1, 2, 3]; Object.defineProperty(lst, 'length', {writable: false});"
           "try { lst.push(4); 'no' } catch (x) { (x instanceof TypeError) + '' }",
           "true");
    T_TEXT("var big = {length: 4294967296, 4294967295: 'last'};"
           "Array.prototype.pop.call(big) + ':' + big.length",
           "last:4294967295");
    env_free(e);
}

EMB_TEST(array_species_create_is_used)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var made = 0;"
           "function Sub(n) { made++; this.n = n; }"
           "Sub.prototype = Object.create(Array.prototype);"
           "var src = [1, 2, 3]; src.constructor = {};"
           "src.constructor[S_species] = Sub;"
           "var out = src.map(function (x) { return x; });"
           "(out instanceof Sub) + ':' + made + ':' + out.n",
           "true:1:3");
    T_TEXT("var base = [1, 2]; base.constructor = undefined;"
           "Array.isArray(base.slice()) + ''",
           "true");
    env_free(e);
}

EMB_TEST_MAIN()
