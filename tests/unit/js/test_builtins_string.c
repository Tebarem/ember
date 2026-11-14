#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* String builtins, exercised through script: each check evaluates an expression that must
 * return true. Case mapping has its own file (test_builtins_string_case.c). */

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
        /* Symbols reach the scripts as globals; the Symbol constructor is not part of J1. */
        static const struct {
            const char *name;
            enum embi_js_wk wk;
        } syms[] = {
            {"S_match", EMBI_JS_WK_MATCH},
            {"S_matchAll", EMBI_JS_WK_MATCH_ALL},
            {"S_replace", EMBI_JS_WK_REPLACE},
            {"S_search", EMBI_JS_WK_SEARCH},
            {"S_split", EMBI_JS_WK_SPLIT},
            {"S_toStringTag", EMBI_JS_WK_TO_STRING_TAG},
            {"S_iterator", EMBI_JS_WK_ITERATOR},
        };
        for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
            embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, syms[i].name),
                               embi_js_wk_sym(e.rt, syms[i].wk), 0);
        }
        /* Helpers in ES5 syntax (computed keys and arrows are ES2015 syntax, not yet parsed). */
        static const char prelude[] =
            "function mkSym(k, v) { var o = {}; o[k] = v; return o; }"
            "function mkRe(v, flags) { var o = {}; o[S_match] = v; o.flags = flags; return o; }";
        embi_js_eval(e.r, prelude, sizeof prelude - 1, "string-prelude.js", 0);
        embi_js_exception_clear(e.rt);
    }
    return e;
}

static void env_free(env e)
{
    embi_js_realm_free(e.r);
    embi_js_runtime_free(e.rt);
}

/* Evaluates src. Returns true when it yields the boolean true; logs otherwise. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "string.js", 0);
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

/* Evaluates src, which must throw an error whose name is want. */
static bool js_throws(env e, const char *src, const char *want)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "string.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected %s: %s", want, src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_value n = embi_js_get(e.rt, embi_js_as_object(ex), embi_js_key_c(e.rt, "name"));
        embi_js_string *ns = embi_js_is_string(n) ? embi_js_as_string(n) : nullptr;
        if (ns && embi_js_str_len(ns) == strlen(want)) {
            ok = true;
            for (size_t i = 0; i < strlen(want); i++) {
                if (embi_js_str_at(ns, (uint32_t)i) != (uint8_t)want[i]) {
                    ok = false;
                }
            }
        }
    }
    if (!ok) {
        SDL_Log("    wrong exception (wanted %s): %s", want, src);
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

EMB_TEST(string_constructor_call_and_new)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "String(1) === '1'"));
    EMB_CHECK(js_true(e, "String() === ''"));
    EMB_CHECK(js_true(e, "String(null) === 'null' && String(undefined) === 'undefined'"));
    EMB_CHECK(js_true(e, "String(S_iterator) === 'Symbol(Symbol.iterator)'"));
    EMB_CHECK(js_true(e, "typeof new String('ab') === 'object'"));
    EMB_CHECK(js_true(e, "new String('ab').length === 2"));
    EMB_CHECK(js_true(e, "new String('ab') instanceof String"));
    EMB_CHECK(js_true(e, "new String('ab').valueOf() === 'ab'"));
    EMB_CHECK(js_true(e, "new String('ab')[1] === 'b' && new String('ab')[5] === undefined"));
    EMB_CHECK(js_throws(e, "new String(S_iterator)", "TypeError"));
    EMB_CHECK(js_true(e, "String.length === 1"));
    EMB_CHECK(js_true(e, "String.prototype.length === 0"));
    EMB_CHECK(js_true(e, "String.prototype.valueOf() === ''"));
    env_free(e);
}

EMB_TEST(string_static_functions)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "String.fromCharCode(72, 105) === 'Hi'"));
    EMB_CHECK(js_true(e, "String.fromCharCode(0x10041) === 'A'"));
    EMB_CHECK(js_true(e, "String.fromCharCode() === ''"));
    EMB_CHECK(js_true(e, "String.fromCodePoint(0x1F600).length === 2"));
    EMB_CHECK(js_true(e, "String.fromCodePoint(0x1F600).codePointAt(0) === 0x1F600"));
    EMB_CHECK(js_true(e, "String.fromCodePoint(65, 66) === 'AB'"));
    EMB_CHECK(js_throws(e, "String.fromCodePoint(-1)", "RangeError"));
    EMB_CHECK(js_throws(e, "String.fromCodePoint(0x110000)", "RangeError"));
    EMB_CHECK(js_throws(e, "String.fromCodePoint(1.5)", "RangeError"));
    EMB_CHECK(js_throws(e, "String.fromCodePoint(NaN)", "RangeError"));
    EMB_CHECK(js_true(e, "String.raw({raw: ['a', 'b', 'c']}, 1, 2) === 'a1b2c'"));
    EMB_CHECK(js_true(e, "String.raw({raw: ['x']}, 1, 2) === 'x'"));
    EMB_CHECK(js_true(e, "String.raw({raw: []}) === ''"));
    EMB_CHECK(js_true(e, "String.raw({raw: 'abc'}, 1, 2) === 'a1b2c'"));
    EMB_CHECK(js_throws(e, "String.raw()", "TypeError"));
    env_free(e);
}

EMB_TEST(string_char_access)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'abc'.charAt(1) === 'b' && 'abc'.charAt(9) === ''"));
    EMB_CHECK(js_true(e, "'abc'.charAt(-1) === '' && 'abc'.charAt() === 'a'"));
    EMB_CHECK(js_true(e, "'abc'.charCodeAt(0) === 97 && isNaN('abc'.charCodeAt(5))"));
    EMB_CHECK(js_true(e, "'\\uD83D\\uDE00'.codePointAt(0) === 0x1F600"));
    EMB_CHECK(js_true(e, "'\\uD83D\\uDE00'.codePointAt(1) === 0xDE00"));
    EMB_CHECK(js_true(e, "'\\uD83D'.codePointAt(0) === 0xD83D"));
    EMB_CHECK(js_true(e, "'abc'.codePointAt(3) === undefined"));
    EMB_CHECK(js_true(e, "'abc'.at(-1) === 'c' && 'abc'.at(3) === undefined && 'abc'.at(0) === 'a'"));
    EMB_CHECK(js_true(e, "'abc'.at(-4) === undefined"));
    EMB_CHECK(js_true(e, "'a'.concat(1, null, [2, 3]) === 'a1null2,3'"));
    EMB_CHECK(js_true(e, "''.concat() === ''"));
    env_free(e);
}

EMB_TEST(string_search_methods)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'abcabc'.indexOf('c', 3) === 5"));
    EMB_CHECK(js_true(e, "'abcabc'.indexOf('c') === 2 && 'abcabc'.indexOf('z') === -1"));
    EMB_CHECK(js_true(e, "'abc'.indexOf('') === 0 && 'abc'.indexOf('', 10) === 3"));
    EMB_CHECK(js_true(e, "'abcabc'.lastIndexOf('c') === 5"));
    EMB_CHECK(js_true(e, "'abcabc'.lastIndexOf('c', 4) === 2"));
    EMB_CHECK(js_true(e, "'abc'.lastIndexOf('') === 3 && 'abc'.lastIndexOf('', 1) === 1"));
    EMB_CHECK(js_true(e, "'abc'.lastIndexOf('a', NaN) === 0 && 'abc'.lastIndexOf('c', -5) === -1"));
    EMB_CHECK(js_true(e, "'abc'.lastIndexOf('abcd') === -1"));
    EMB_CHECK(js_true(e, "'abc'.includes('bc') && !'abc'.includes('bd')"));
    EMB_CHECK(js_true(e, "'abc'.includes('', 99) && !'abc'.includes('a', 1)"));
    EMB_CHECK(js_true(e, "'abc'.startsWith('b', 1) && !'abc'.startsWith('a', 1)"));
    EMB_CHECK(js_true(e, "'abc'.startsWith('') && !'abc'.startsWith('abcd')"));
    EMB_CHECK(js_true(e, "'abc'.endsWith('b', 2) && !'abc'.endsWith('c', 2)"));
    EMB_CHECK(js_true(e, "'abc'.endsWith('') && 'abc'.endsWith('abc')"));
    EMB_CHECK(js_throws(e, "'a'.includes(mkSym(S_match, true))", "TypeError"));
    EMB_CHECK(js_throws(e, "'a'.startsWith(mkSym(S_match, true))", "TypeError"));
    EMB_CHECK(js_throws(e, "'a'.endsWith(mkSym(S_match, true))", "TypeError"));
    EMB_CHECK(js_true(e,
        "var t = mkSym(S_match, false); t.toString = function() { return 'a'; };"
        "'a'.includes(t)"));
    EMB_CHECK(js_true(e, "'a'.localeCompare('b') === -1 && 'b'.localeCompare('a') === 1"));
    EMB_CHECK(js_true(e, "'a'.localeCompare('a') === 0"));
    env_free(e);
}

EMB_TEST(string_slicing)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'abcdef'.slice(-3, -1) === 'de' && 'abcdef'.slice(2) === 'cdef'"));
    EMB_CHECK(js_true(e, "'abcdef'.slice(4, 1) === '' && 'abcdef'.slice(-100, 2) === 'ab'"));
    EMB_CHECK(js_true(e, "'abcdef'.substring(4, 1) === 'bcd' && 'abcdef'.substring(-3) === 'abcdef'"));
    EMB_CHECK(js_true(e, "'abcdef'.substring(NaN, 2) === 'ab'"));
    EMB_CHECK(js_true(e, "'abcdef'.substr(-3, 2) === 'de' && 'bcdef' === 'abcdef'.substr(1)"));
    EMB_CHECK(js_true(e, "'abcdef'.substr(1, -1) === '' && 'abcdef'.substr(-100, 2) === 'ab'"));
    EMB_CHECK(js_true(e, "'abcdef'.substr(2, Infinity) === 'cdef'"));
    EMB_CHECK(js_true(e, "'abc'.substr(-Infinity, 2) === 'ab'"));
    EMB_CHECK(js_true(e, "'abc'.substr(-1, 1) === 'c'"));
    env_free(e);
}

EMB_TEST(string_split)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'a,b,,c'.split(',').length === 4 && 'a,b,,c'.split(',')[2] === ''"));
    EMB_CHECK(js_true(e, "'abc'.split('').length === 3 && 'abc'.split('')[1] === 'b'"));
    EMB_CHECK(js_true(e, "'a,b,c'.split(',', 2).length === 2"));
    EMB_CHECK(js_true(e, "'a,b,c'.split(',', 0).length === 0"));
    EMB_CHECK(js_true(e, "''.split('').length === 0"));
    EMB_CHECK(js_true(e, "''.split(',').length === 1 && ''.split(',')[0] === ''"));
    EMB_CHECK(js_true(e, "'ab'.split(undefined).length === 1 && 'ab'.split()[0] === 'ab'"));
    EMB_CHECK(js_true(e, "'a,b'.split(',', -1).length === 2"));
    EMB_CHECK(js_true(e, "'abc'.split('', 2).length === 2"));
    EMB_CHECK(js_true(e, "'a--b--c'.split('--').join('|') === 'a|b|c'"));
    EMB_CHECK(js_true(e, "'abc'.split('abc').length === 2"));
    env_free(e);
}

EMB_TEST(string_replace_family)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'aXbX'.replace('X', '-') === 'a-bX'"));
    EMB_CHECK(js_true(e, "'abc'.replace('b', '[$&$`$\\']') === 'a[bac]c'"));
    EMB_CHECK(js_true(e, "'abc'.replace('b', '$1$$$<x>') === 'a$1$$<x>c'"));
    EMB_CHECK(js_true(e, "'abc'.replace('z', 'y') === 'abc'"));
    EMB_CHECK(js_true(e,
        "'abc'.replace('b', function(m, p, s) { return m + p + s; }) === 'ab1abcc'"));
    EMB_CHECK(js_true(e, "'abc'.replace('b', function() { return 5; }) === 'a5c'"));
    EMB_CHECK(js_true(e, "'aXbX'.replaceAll('X', '-') === 'a-b-'"));
    EMB_CHECK(js_true(e, "'abc'.replaceAll('', '-') === '-a-b-c-'"));
    EMB_CHECK(js_true(e, "'aaaa'.replaceAll('aa', 'b') === 'bb'"));
    EMB_CHECK(js_true(e, "'abab'.replaceAll('b', function(m, p) { return p; }) === 'a1a3'"));
    EMB_CHECK(js_true(e, "'xyz'.replaceAll('q', 'r') === 'xyz'"));
    EMB_CHECK(js_throws(e, "'a'.replaceAll(mkSym(S_match, true), 'x')", "TypeError"));
    EMB_CHECK(js_true(e,
        "var t = mkSym(S_match, false); t.toString = function() { return 'a'; };"
        "'a'.replaceAll(t, 'x') === 'x'"));
    env_free(e);
}

EMB_TEST(string_symbol_dispatch)
{
    env e = env_new();
    /* match, search, replace, split and matchAll go to the argument's Symbol method. */
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_match] = function(s) { return 'm:' + s; };"
        "'abc'.match(o) === 'm:abc'"));
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_search] = function(s) { return 's:' + s; };"
        "'abc'.search(o) === 's:abc'"));
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_replace] = function(s, r) { return 'r:' + s + r; };"
        "'abc'.replace(o, 'X') === 'r:abcX'"));
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_split] = function(s, l) { return ['split', s, l]; };"
        "'abc'.split(o, 7)[2] === 7 && 'abc'.split(o, 7)[1] === 'abc'"));
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_match] = null; o[S_split] = undefined;"
        "'abc'.split(o).length === 1 && 'abc'.split(o)[0] === 'abc'"));
    EMB_CHECK(js_true(e,
        "var o = {}; o[S_matchAll] = function(s) { return 'ma:' + s; };"
        "o[S_match] = true; o.flags = 'g';"
        "'a'.matchAll(o) === 'ma:a'"));
    /* A RegExp-like argument without the g flag is rejected before the Symbol method runs. */
    EMB_CHECK(js_throws(e, "'a'.matchAll(mkRe(true, 'i'))", "TypeError"));
    EMB_CHECK(js_throws(e, "'a'.replaceAll(mkRe(true, ''), 'x')", "TypeError"));
    EMB_CHECK(js_true(e, "String.prototype.replace.length === 2"));
    env_free(e);
}

EMB_TEST(string_pad_repeat_trim)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'5'.padStart(3, '0') === '005'"));
    EMB_CHECK(js_true(e, "'abc'.padEnd(6, '12') === 'abc121'"));
    EMB_CHECK(js_true(e, "'abc'.padStart(2) === 'abc' && 'x'.padStart(4, '') === 'x'"));
    EMB_CHECK(js_true(e, "'x'.padStart(3) === '  x' && 'x'.padEnd(3) === 'x  '"));
    EMB_CHECK(js_true(e, "'ab'.repeat(3) === 'ababab' && 'a'.repeat(0) === ''"));
    EMB_CHECK(js_true(e, "''.repeat(5) === '' && 'ab'.repeat(2.9) === 'abab'"));
    EMB_CHECK(js_throws(e, "'a'.repeat(-1)", "RangeError"));
    EMB_CHECK(js_throws(e, "'a'.repeat(Infinity)", "RangeError"));
    EMB_CHECK(js_throws(e, "''.repeat(Infinity)", "RangeError"));
    EMB_CHECK(js_true(e, "' \\u00A0a\\uFEFF\\n'.trim() === 'a'"));
    EMB_CHECK(js_true(e, "'  a '.trimStart() === 'a ' && '  a '.trimEnd() === '  a'"));
    EMB_CHECK(js_true(e, "String.prototype.trimLeft === String.prototype.trimStart"));
    EMB_CHECK(js_true(e, "String.prototype.trimRight === String.prototype.trimEnd"));
    EMB_CHECK(js_true(e, "'\\u2028\\u3000x'.trim() === 'x' && '\\u180Ex'.trim() === '\\u180Ex'"));
    EMB_CHECK(js_true(e, "'a'.normalize() === 'a' && 'a'.normalize('NFD') === 'a'"));
    EMB_CHECK(js_throws(e, "'a'.normalize('NFX')", "RangeError"));
    EMB_CHECK(js_throws(e, "String.prototype.padStart.call(null)", "TypeError"));
    env_free(e);
}

EMB_TEST(string_this_checks_and_wrappers)
{
    env e = env_new();
    EMB_CHECK(js_throws(e, "String.prototype.toString.call(1)", "TypeError"));
    EMB_CHECK(js_throws(e, "String.prototype.valueOf.call({})", "TypeError"));
    EMB_CHECK(js_throws(e, "String.prototype.charAt.call(null, 0)", "TypeError"));
    EMB_CHECK(js_throws(e, "String.prototype.at.call(undefined, 0)", "TypeError"));
    EMB_CHECK(js_throws(e, "String.prototype.split.call(undefined)", "TypeError"));
    EMB_CHECK(js_true(e, "String.prototype.toUpperCase.call(12) === '12'"));
    EMB_CHECK(js_true(e, "String.prototype.toString.call(new String('q')) === 'q'"));
    EMB_CHECK(js_true(e, "String.prototype.valueOf.call(new String('q')) === 'q'"));
    EMB_CHECK(js_true(e, "String.prototype.charAt.call(12, 0) === '1'"));
    EMB_CHECK(js_true(e, "String.prototype.trim.name === 'trim'"));
    EMB_CHECK(js_true(e, "Object.keys(String.prototype).length === 0"));
    env_free(e);
}

EMB_TEST(string_annex_b_html_methods)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'a'.anchor('\"x\"') === '<a name=\"&quot;x&quot;\">a</a>'"));
    EMB_CHECK(js_true(e, "'a'.big() === '<big>a</big>' && 'a'.blink() === '<blink>a</blink>'"));
    EMB_CHECK(js_true(e, "'a'.bold() === '<b>a</b>' && 'a'.fixed() === '<tt>a</tt>'"));
    EMB_CHECK(js_true(e, "'a'.fontcolor('red') === '<font color=\"red\">a</font>'"));
    EMB_CHECK(js_true(e, "'a'.fontsize(7) === '<font size=\"7\">a</font>'"));
    EMB_CHECK(js_true(e, "'a'.italics() === '<i>a</i>' && 'a'.small() === '<small>a</small>'"));
    EMB_CHECK(js_true(e, "'a'.link('u') === '<a href=\"u\">a</a>'"));
    EMB_CHECK(js_true(e, "'a'.strike() === '<strike>a</strike>'"));
    EMB_CHECK(js_true(e, "'a'.sub() === '<sub>a</sub>' && 'a'.sup() === '<sup>a</sup>'"));
    EMB_CHECK(js_throws(e, "String.prototype.bold.call(null)", "TypeError"));
    env_free(e);
}

EMB_TEST(string_method_lengths_and_names)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "String.prototype.concat.length === 1"));
    EMB_CHECK(js_true(e, "String.prototype.replace.length === 2"));
    EMB_CHECK(js_true(e, "String.prototype.slice.length === 2"));
    EMB_CHECK(js_true(e, "String.prototype.toUpperCase.length === 0"));
    EMB_CHECK(js_true(e, "String.prototype.padEnd.length === 1"));
    EMB_CHECK(js_true(e, "String.fromCharCode.length === 1"));
    EMB_CHECK(js_true(e, "String.raw.length === 1"));
    EMB_CHECK(js_true(e, "String.prototype.at.name === 'at'"));
    EMB_CHECK(js_true(e, "String.prototype.constructor === String"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(String.prototype, 'at').enumerable === false"));
    EMB_CHECK(js_true(e, "'abc'.hasOwnProperty('length') && 'abc'.length === 3"));
    env_free(e);
}

/* The same operations with a collection after almost every allocation: values held only in C
 * locals or arguments must survive, and the results must not change. */
EMB_TEST(string_survives_frequent_gc)
{
    env e = env_new();
    embi_js_set_gc_params(e.rt, 1, 0.0);
    EMB_CHECK(js_true(e,
        "var s = '';"
        "for (var i = 0; i < 200; i++) { s = ('ab' + i).toUpperCase() + s.slice(0, 40); }"
        "s.length > 40 && s.slice(0, 5) === 'AB199' && s.indexOf('AB198') === 5"));
    EMB_CHECK(js_true(e,
        "var parts = 'a-b-c-d-e-f-g-h'.split('-'); var out = [];"
        "for (var k = 0; k < 50; k++) { out.push(parts.join('+').replace('+', '=')); }"
        "out[49] === 'a=b+c+d+e+f+g+h'"));
    EMB_CHECK(js_true(e,
        "var t = 'x'; for (var j = 0; j < 10; j++) { t = t.concat(t); }"
        "t.length === 1024 && t.charAt(1023) === 'x'"));
    EMB_CHECK(js_true(e,
        "'\\u0391\\u03A3\\u0391'.toLowerCase().replace('\\u03C3', function(m) {"
        "  return m.toUpperCase(); }) === '\\u03B1\\u03A3\\u03B1'"));
    EMB_CHECK(js_true(e,
        "var u = ''; for (var q = 0; q < 300; q++) { u = String.fromCharCode(0x3B1 + (q % 20)); }"
        "u.length === 1 && u.charCodeAt(0) === 0x3B1 + (299 % 20)"));
    env_free(e);
}

/* Gaps found in review: own-property order of String objects, ToUint32 wrap of the split limit,
 * and a replace callback that allocates enough to collect while the match is held in C. */
EMB_TEST(string_wrapper_own_property_order)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Object.getOwnPropertyNames(new String('ab')).join() === '0,1,length'"));
    env_free(e);
}

EMB_TEST(string_split_limit_uses_touint32)
{
    env e = env_new();
    /* ToUint32(2^32 + 1) is 1, ToUint32(2^32) is 0. */
    EMB_CHECK(js_true(e, "'a,b'.split(',', 4294967297).length === 1"));
    EMB_CHECK(js_true(e, "'a,b'.split(',', 4294967296).length === 0"));
    env_free(e);
}

EMB_TEST(string_replace_callback_survives_gc)
{
    env e = env_new();
    embi_js_set_gc_params(e.rt, 1, 0.0);
    EMB_CHECK(js_true(e,
        "'abcabc'.replace('b', function(m, pos) {"
        "  var junk = []; for (var i = 0; i < 50; i++) { junk.push({ v: i + m }); }"
        "  return m.toUpperCase() + pos + junk[49].v; }) === 'aB149bcabc'"));
    env_free(e);
}

/* Wide and Latin-1 results of repeat and padStart/padEnd, built in place. */
EMB_TEST(string_repeat_pad_widths)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "'\u1234a'.repeat(2) === '\u1234a\u1234a'"));
    EMB_CHECK(js_true(e, "'a'.padEnd(5, '\u1234') === 'a\u1234\u1234\u1234\u1234'"));
    EMB_CHECK(js_true(e, "'x'.padStart(6, 'ab') === 'ababax' && 'x'.padEnd(6, 'ab') === 'xababa'"));
    EMB_CHECK(js_true(e, "'a'.padEnd(2, 'b\u1234') === 'ab'"));
    EMB_CHECK(js_true(e, "'\u1234'.padStart(3, 'a') === 'aa\u1234' && ''.repeat(5) === ''"));
    EMB_CHECK(js_true(e, "'ab'.repeat(3).length === 6 && 'x'.repeat(1000).length === 1000"));
    env_free(e);
}

EMB_TEST_MAIN()
