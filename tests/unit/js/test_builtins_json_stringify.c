#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* JSON.stringify through script: values, escapes, toJSON, replacer, gap, cycles, depth. */

typedef struct env {
    embi_js_runtime *rt;
    embi_js_realm *r;
} env;

static env env_new(void)
{
    env e;
    e.rt = embi_js_runtime_new();
    e.r = e.rt ? embi_js_realm_new(e.rt) : nullptr;
    if (e.r) {
        /* A symbol value for the Symbol cases (the Symbol global is not in J1). */
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_iterator"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_ITERATOR), 0);
    }
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "json_stringify.js", 0);
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

static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "json_stringify.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected %s: %s", want, src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "name"));
        if (embi_js_is_string(n)) {
            embi_js_string *ns = embi_js_as_string(n);
            ok = embi_js_str_len(ns) == strlen(want);
            for (size_t i = 0; ok && i < strlen(want); i++) {
                ok = embi_js_str_at(ns, (uint32_t)i) == (uint8_t)want[i];
            }
        }
    }
    if (!ok) {
        SDL_Log("    wrong exception (wanted %s): %s", want, src);
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

EMB_TEST(json_stringify_primitives)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify(undefined) === undefined"));
    EMB_CHECK(js_true(e, "JSON.stringify(function(){}) === undefined"));
    EMB_CHECK(js_true(e, "JSON.stringify(S_iterator) === undefined && JSON.stringify([S_iterator]) === '[null]'"));
    EMB_CHECK(js_true(e, "JSON.stringify(null) === 'null' && JSON.stringify(true) === 'true'"));
    EMB_CHECK(js_true(e, "JSON.stringify(false) === 'false' && JSON.stringify('str') === '\"str\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify(0) === '0' && JSON.stringify(-0) === '0'"));
    EMB_CHECK(js_true(e, "JSON.stringify(-17) === '-17' && JSON.stringify(2147483647) === '2147483647'"));
    EMB_CHECK(js_true(e, "JSON.stringify(-2147483648) === '-2147483648'"));
    EMB_CHECK(js_true(e, "JSON.stringify(0.1) === '0.1' && JSON.stringify(1e21) === '1e+21'"));
    EMB_CHECK(js_true(e, "JSON.stringify(1e-7) === '1e-7' && JSON.stringify(123.5) === '123.5'"));
    EMB_CHECK(js_true(e, "JSON.stringify(NaN) === 'null' && JSON.stringify(Infinity) === 'null' && JSON.stringify(-Infinity) === 'null'"));
    EMB_CHECK(js_true(e, "JSON.stringify(5e-324) === '5e-324' && JSON.stringify(1.7976931348623157e308) === '1.7976931348623157e+308'"));
    env_free(e);
}

EMB_TEST(json_stringify_strings_and_escapes)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify('') === '\"\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('a\"b') === '\"a\\\\\"b\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\\\') === '\"\\\\\\\\\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('/') === '\"/\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\b\\f\\n\\r\\t') === '\"\\\\b\\\\f\\\\n\\\\r\\\\t\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify(String.fromCharCode(1, 31)) === '\"\\\\u0001\\\\u001f\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify(String.fromCharCode(0x7f, 0xe9)) === '\"' + String.fromCharCode(0x7f, 0xe9) + '\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\ud800') === '\"\\\\ud800\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\udfff') === '\"\\\\udfff\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\ud800x') === '\"\\\\ud800x\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\udc00\\ud800') === '\"\\\\udc00\\\\ud800\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\ud83d\\ude00') === '\"\\ud83d\\ude00\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\u4e2d') === '\"\\u4e2d\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify('\\ud83d') === '\"\\\\ud83d\"'"));
    EMB_CHECK(js_true(e, "JSON.parse(JSON.stringify('\\ud800\\u0000\\u2028')) === '\\ud800\\u0000\\u2028'"));
    env_free(e);
}

EMB_TEST(json_stringify_objects_and_arrays)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify({a:[1,{b:2}],c:'x'}) === '{\"a\":[1,{\"b\":2}],\"c\":\"x\"}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({}) === '{}' && JSON.stringify([]) === '[]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:undefined,b:function(){},c:1}) === '{\"c\":1}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([undefined, function(){}, null]) === '[null,null,null]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({b:1, 2:2, a:3, 1:4}) === '{\"1\":4,\"2\":2,\"b\":1,\"a\":3}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({x:NaN, y:-0, z:[Infinity]}) === '{\"x\":null,\"y\":0,\"z\":[null]}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([new Number(1), new String('a'), new Boolean(false)]) === '[1,\"a\",false]'"));
    EMB_CHECK(js_true(e, "JSON.stringify(new Number(NaN)) === 'null' && JSON.stringify(new Boolean(true)) === 'true'"));
    EMB_CHECK(js_true(e, "JSON.stringify([,1]) === '[null,1]'"));
    EMB_CHECK(js_true(e, "var a = []; a.length = 3; JSON.stringify(a) === '[null,null,null]'"));
    EMB_CHECK(js_true(e, "var s = {}; JSON.stringify([s, s]) === '[{},{}]'"));
    EMB_CHECK(js_true(e, "JSON.stringify(Object.create({inherited: 1})) === '{}'"));
    EMB_CHECK(js_true(e, "var h = {}; Object.defineProperty(h, 'hidden', {value: 1, enumerable: false}); h.shown = 2; JSON.stringify(h) === '{\"shown\":2}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({get g() { return 7; }}) === '{\"g\":7}'"));
    EMB_CHECK(js_throws(e, "JSON.stringify({get g() { throw new RangeError('x'); }})", "RangeError"));
    env_free(e);
}

EMB_TEST(json_stringify_tojson)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify({x: {toJSON: function(k) { return 'k=' + k; }}}) === '{\"x\":\"k=x\"}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({toJSON: function(k) { return k === '' ? 'root' : 'bad'; }}) === '\"root\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify([{toJSON: function(k) { return k; }}]) === '[\"0\"]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({toJSON: function() { return undefined; }}) === undefined"));
    EMB_CHECK(js_true(e, "JSON.stringify({toJSON: 5, a: 1}) === '{\"toJSON\":5,\"a\":1}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({toJSON: function() { return {n: 2}; }}) === '{\"n\":2}'"));
    EMB_CHECK(js_true(e, "var p = {toJSON: function() { return 'proto'; }}; JSON.stringify(Object.create(p)) === '\"proto\"'"));
    EMB_CHECK(js_true(e, "JSON.stringify(Object.create({toJSON: function(){ return 'i'; }})) === '\"i\"'"));
    EMB_CHECK(js_throws(e, "JSON.stringify({toJSON: function() { throw new TypeError('t'); }})", "TypeError"));
    env_free(e);
}

EMB_TEST(json_stringify_replacer_function)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify({a:1,b:2}, function(k,v){ return k === 'a' ? undefined : v; }) === '{\"b\":2}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, function(k,v){ return k === '' ? [v] : v; }) === '[{\"a\":1}]'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1,2], function(k,v){ return typeof v === 'number' ? v + 1 : v; }) === '[2,3]'"));
    EMB_CHECK(js_true(e, "var seen = []; JSON.stringify({x:{y:1}}, function(k,v){ seen.push(k); return v; }); seen.join() === ',x,y'"));
    EMB_CHECK(js_true(e, "var self = null; JSON.stringify({q:1}, function(k,v){ if (k === 'q') self = this; return v; }); self.q === 1"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, function(k, v) { return v; }) === '{\"a\":1}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({toJSON: function(){ return 'T'; }}, function(k, v){ return v + '!'; }) === '\"T!\"'"));
    EMB_CHECK(js_throws(e, "JSON.stringify({a:1}, function(){ throw new RangeError('r'); })", "RangeError"));
    env_free(e);
}

EMB_TEST(json_stringify_replacer_array)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify({a:1,b:2,c:{a:3,d:4}}, ['a','c']) === '{\"a\":1,\"c\":{\"a\":3}}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({1:'x',2:'y'}, [2]) === '{\"2\":\"y\"}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1,b:2}, ['b','a','b']) === '{\"b\":2,\"a\":1}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, ['z']) === '{}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({1:'one'}, [1]) === '{\"1\":\"one\"}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({1:'one', b:2}, [new Number(1), new String('b')]) === '{\"1\":\"one\",\"b\":2}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:{b:1,c:2}}, ['a','b']) === '{\"a\":{\"b\":1}}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([{a:1,b:2}], ['b']) === '[{\"b\":2}]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, [{}, true, null, undefined]) === '{}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1,b:2}, ['b'], null) === '{\"b\":2}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, ['a'], 0) === '{\"a\":1}'"));
    env_free(e);
}

EMB_TEST(json_stringify_gap)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify({a:[1,2],b:{}}, null, 2) === '{\\n  \"a\": [\\n    1,\\n    2\\n  ],\\n  \"b\": {}\\n}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, '--') === '[\\n--1\\n]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:[]}, null, 2) === '{\\n  \"a\": []\\n}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, null, 1) === '{\\n \"a\": 1\\n}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:1}, null, 0) === '{\"a\":1}' && JSON.stringify({a:1}, null, -5) === '{\"a\":1}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, 20) === JSON.stringify([1], null, 10) && JSON.stringify([1], null, 10).length === 1 + 1 + 10 + 1 + 1 + 1"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, 'abcdefghijklmnop') === '[\\nabcdefghij1\\n]'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, new Number(2)) === '[\\n  1\\n]'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, new String('--')) === '[\\n--1\\n]'"));
    EMB_CHECK(js_true(e, "JSON.stringify([1], null, '') === '[1]'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:undefined, b:1}, null, 2) === '{\\n  \"b\": 1\\n}'"));
    EMB_CHECK(js_true(e, "JSON.stringify({a:undefined}, null, 2) === '{}'"));
    EMB_CHECK(js_true(e, "JSON.stringify([undefined], null, 2) === '[\\n  null\\n]'"));
    env_free(e);
}

EMB_TEST(json_stringify_cycles)
{
    env e = env_new();
    EMB_CHECK(js_throws(e, "var o = {}; o.self = o; JSON.stringify(o)", "TypeError"));
    EMB_CHECK(js_throws(e, "var a = []; a.push(a); JSON.stringify(a)", "TypeError"));
    EMB_CHECK(js_throws(e, "var x = {y: [{}]}; x.y[0].back = x; JSON.stringify(x)", "TypeError"));
    EMB_CHECK(js_throws(e, "var c = {}; c.t = {toJSON: function() { return c; }}; JSON.stringify(c)", "TypeError"));
    EMB_CHECK(js_true(e, "var ok = {}; var d = {a: ok, b: ok}; JSON.stringify(d) === '{\"a\":{},\"b\":{}}'"));
    /* A cycle reached only through a replacer result is still a cycle. */
    EMB_CHECK(js_throws(e, "var r = {}; JSON.stringify({k: r}, function(k, v) { if (k === 'k') { v = {p: r}; r.q = v; } return v; })", "TypeError"));
    env_free(e);
}

EMB_TEST(json_stringify_deep_nesting_is_range_error)
{
    env e = env_new();
    /* 100k nested arrays: a RangeError, never a C stack overflow. */
    EMB_CHECK(js_true(e, "var deepArr = []; for (var i = 0; i < 100000; i++) deepArr = [deepArr]; true"));
    EMB_CHECK(js_throws(e, "JSON.stringify(deepArr)", "RangeError"));
    EMB_CHECK(js_true(e, "var deepObj = {}; for (var j = 0; j < 100000; j++) deepObj = {n: deepObj}; true"));
    EMB_CHECK(js_throws(e, "JSON.stringify(deepObj, null, 2)", "RangeError"));
    env_free(e);
}

EMB_TEST(json_stringify_moderate_nesting_ok)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var m = []; for (var i = 0; i < 500; i++) m = [m]; JSON.stringify(m).length === 1002"));
    env_free(e);
}

EMB_TEST(json_stringify_properties)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.stringify.length === 3"));
    EMB_CHECK(js_true(e, "JSON.stringify({a: 'x'}) === JSON.stringify({a: 'x'})"));
    EMB_CHECK(js_true(e, "JSON.stringify(JSON.parse('{\"k\":[true,false,null,1.5,\"s\"]}')) === '{\"k\":[true,false,null,1.5,\"s\"]}'"));
    env_free(e);
}

EMB_TEST_MAIN()
