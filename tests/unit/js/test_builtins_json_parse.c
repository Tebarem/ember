#include "emb_test.h"

#include "js/js.h"

#include <stdlib.h>
#include <string.h>

/* JSON.parse through script: grammar, numbers, strings, key order, reviver, deep nesting. */

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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "json_parse.js", 0);
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

/* True when the script throws an error whose name is want. */
/* Logs one string-valued property of an exception object (diagnostics for failed checks). */
static void log_prop(env e, embi_js_value ex, const char *prop)
{
    if (!embi_js_is_object(ex)) {
        SDL_Log("    %s: (not an object)", prop);
        return;
    }
    embi_js_value v = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, prop));
    if (!embi_js_is_string(v)) {
        SDL_Log("    %s: (not a string)", prop);
        return;
    }
    embi_js_string *s = embi_js_as_string(v);
    char buf[256];
    uint32_t n = embi_js_str_len(s) < 255 ? embi_js_str_len(s) : 255;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = embi_js_str_at(s, i);
        buf[i] = c < 128 ? (char)c : '?';
    }
    buf[n] = (char)0;
    SDL_Log("    %s: %s", prop, buf);
}

static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "json_parse.js", 0);
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
        log_prop(e, ex, "name");
        log_prop(e, ex, "message");
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

/* Defines a global string `name` holding the n bytes at p (Latin-1 units). */
static void define_bytes(env e, const char *name, const char *p, size_t n)
{
    embi_js_string *s = embi_js_str_new_latin1(e.rt, (const uint8_t *)p, n);
    embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, name), embi_js_from_string(s),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

/* Defines a global string `name` holding `count` copies of unit (unit_len bytes). */
static void define_repeat(env e, const char *name, const char *unit, size_t unit_len, size_t count)
{
    size_t total = unit_len * count;
    char *buf = malloc(total + 1);
    for (size_t i = 0; i < count; i++) {
        memcpy(buf + i * unit_len, unit, unit_len);
    }
    define_bytes(e, name, buf, total);
    free(buf);
}

EMB_TEST(json_parse_numbers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.parse('1') === 1 && JSON.parse('-7') === -7"));
    EMB_CHECK(js_true(e, "1 / JSON.parse('-0') === -Infinity"));
    EMB_CHECK(js_true(e, "JSON.parse('1e3') === 1000 && JSON.parse('1E+2') === 100"));
    EMB_CHECK(js_true(e, "JSON.parse('1.5e-3') === 0.0015 && JSON.parse('0.1') === 0.1"));
    EMB_CHECK(js_true(e, "JSON.parse('1e400') === Infinity && JSON.parse('-1e400') === -Infinity"));
    EMB_CHECK(js_true(e, "JSON.parse('5e-324') > 0 && JSON.parse('1e-400') === 0"));
    EMB_CHECK(js_true(e, "JSON.parse('123456789012345678901234567890') === 123456789012345678901234567890"));
    EMB_CHECK(js_true(e, "JSON.parse('0.30000000000000004') === 0.1 + 0.2"));
    EMB_CHECK(js_true(e, "JSON.parse('2147483648') === 2147483648"));
    EMB_CHECK(js_true(e, "JSON.parse('[0, 10, 1.25]').join() === '0,10,1.25'"));
    env_free(e);
}

EMB_TEST(json_parse_literals_and_whitespace)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.parse('true') === true && JSON.parse('false') === false"));
    EMB_CHECK(js_true(e, "JSON.parse('null') === null"));
    EMB_CHECK(js_true(e, "JSON.parse(null) === null && JSON.parse(1) === 1"));
    EMB_CHECK(js_true(e, "JSON.parse(' \\t\\n\\r[ 1 , 2 ]\\r\\n ').length === 2"));
    EMB_CHECK(js_true(e, "JSON.parse('{ }').constructor === Object"));
    EMB_CHECK(js_true(e, "JSON.parse('[]').length === 0"));
    EMB_CHECK(js_true(e, "JSON.parse('[[],{}]')[1] instanceof Object"));
    EMB_CHECK(js_throws(e, "JSON.parse('\\u00a01')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('\\f1')", "SyntaxError"));
    env_free(e);
}

EMB_TEST(json_parse_strings)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.parse('\"abc\"') === 'abc' && JSON.parse('\"\"') === ''"));
    /* JSON text "aA\n" */
    EMB_CHECK(js_true(e, "JSON.parse('\"a\\\\u0041\\\\n\"') === 'aA\\n'"));
    /* JSON text "\"\\\/" then \b \f \r \t: decodes to quote, backslash, slash, BS, FF, CR, TAB */
    define_bytes(e, "esc", "\"\\\"\\\\\\/\\b\\f\\r\\t\"", 16);
    EMB_CHECK(js_true(e, "JSON.parse(esc) === String.fromCharCode(34, 92, 47, 8, 12, 13, 9)"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\u00E9\\\\u4e2D\"') === '\\u00e9\\u4e2d'"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\ud83d\\\\ude00\"') === '\\ud83d\\ude00'"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\ud800\"').length === 1"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\ud800\"').charCodeAt(0) === 0xD800"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\udc00x\"').charCodeAt(0) === 0xDC00"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\u0000\"').charCodeAt(0) === 0"));
    EMB_CHECK(js_true(e, "JSON.parse('\"\\\\uD83D\\\\uDE00\"') === '\\uD83D\\uDE00'"));
    EMB_CHECK(js_throws(e, "JSON.parse('\"\t\"')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('\"\\\\x\"')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('\"\\\\u12G4\"')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('\"\\\\u12\"')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('\"abc')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse(\"'a'\")", "SyntaxError"));
    env_free(e);
}

EMB_TEST(json_parse_objects_and_arrays)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Object.keys(JSON.parse('{\"b\":1,\"2\":2,\"a\":3,\"1\":4}')).join() === '1,2,b,a'"));
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":1,\"a\":2}').a === 2"));
    EMB_CHECK(js_true(e, "JSON.parse('{\"__proto__\":1}').hasOwnProperty('__proto__')"));
    EMB_CHECK(js_true(e, "Object.getPrototypeOf(JSON.parse('{\"__proto__\":{}}')) === Object.prototype"));
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":{\"b\":[1,{\"c\":null}]}}').a.b[1].c === null"));
    EMB_CHECK(js_true(e, "var d = Object.getOwnPropertyDescriptor(JSON.parse('{\"x\":5}'), 'x'); d.writable && d.enumerable && d.configurable && d.value === 5"));
    EMB_CHECK(js_throws(e, "JSON.parse('')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('   ')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse(undefined)", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('01')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('1.')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('.1')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('+1')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('1e')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('-')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('[1,]')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('[,1]')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('{\"a\":1,}')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('{a:1}')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('{\"a\" 1}')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('{\"a\":1')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('[1] x')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('tru')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('nul')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('True')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "JSON.parse('[1 2]')", "SyntaxError"));
    env_free(e);
}

EMB_TEST(json_parse_reviver)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":1,\"b\":[1,2]}', function(k,v){ return typeof v === 'number' ? v*2 : v; }).b[1] === 4"));
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":1,\"b\":2}', function(k,v){ return k === 'a' ? undefined : v; }).hasOwnProperty('a') === false"));
    EMB_CHECK(js_true(e, "JSON.parse('[1,2,3]', function(k,v){ return k === '1' ? undefined : v; }).hasOwnProperty(1) === false"));
    EMB_CHECK(js_true(e, "var keys = []; JSON.parse('{\"x\":{\"y\":1},\"z\":2}', function(k,v){ keys.push(k); return v; }); keys.join() === 'y,x,z,'"));
    EMB_CHECK(js_true(e, "var rootKey = 'unset'; JSON.parse('1', function(k,v){ rootKey = k; return v; }); rootKey === ''"));
    EMB_CHECK(js_true(e, "var holderOk = false; JSON.parse('[5]', function(k,v){ if (k === '0') holderOk = Array.isArray(this); return v; }); holderOk"));
    EMB_CHECK(js_true(e, "JSON.parse('{\"a\":1}', 5).a === 1"));
    EMB_CHECK(js_throws(e, "JSON.parse('{\"a\":1}', function(){ throw new RangeError('boom'); })", "RangeError"));
    env_free(e);
}

EMB_TEST(json_parse_deep_nesting_is_range_error)
{
    env e = env_new();
    /* 100k open brackets or members: a RangeError, never a C stack overflow. */
    define_repeat(e, "deep", "[", 1, 100000);
    EMB_CHECK(js_throws(e, "JSON.parse(deep)", "RangeError"));
    define_repeat(e, "deep", "{\"a\":", 5, 100000);
    EMB_CHECK(js_throws(e, "JSON.parse(deep)", "RangeError"));
    /* Balanced nesting of the same depth hits the same limit. */
    define_repeat(e, "deep", "[", 1, 100000);
    define_repeat(e, "closer", "]", 1, 100000);
    EMB_CHECK(js_true(e, "var text = deep + closer; text.length === 200000"));
    EMB_CHECK(js_throws(e, "JSON.parse(text)", "RangeError"));
    env_free(e);
}

EMB_TEST(json_parse_moderate_nesting_ok)
{
    env e = env_new();
    /* Depth 500 is well inside the native stack budget. */
    EMB_CHECK(js_true(e, "var s = ''; for (var i = 0; i < 500; i++) s += '['; s += '1'; for (var j = 0; j < 500; j++) s += ']'; var v = JSON.parse(s); for (var k = 0; k < 500; k++) v = v[0]; v === 1"));
    env_free(e);
}

EMB_TEST(json_parse_large_round_trip)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "var arr = []; for (var i = 0; i < 20000; i++) arr.push({id: i, name: 'item' + i, tags: [i, 'x']}); var back = JSON.parse(JSON.stringify(arr)); back.length === 20000 && back[19999].name === 'item19999' && back[12345].tags[0] === 12345"));
    env_free(e);
}

EMB_TEST(json_parse_properties)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "JSON.parse.length === 2"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(JSON) === '[object JSON]'"));
    EMB_CHECK(js_true(e, "typeof JSON.parse === 'function' && typeof JSON.stringify === 'function'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(JSON, 'parse').enumerable === false"));
    env_free(e);
}

EMB_TEST_MAIN()
