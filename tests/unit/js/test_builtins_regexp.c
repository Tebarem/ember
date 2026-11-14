#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"

#include <string.h>

/* RegExp objects, the Symbol methods and their String entry points (J4c), exercised through
 * script: each check evaluates an expression that must return true. */

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
            {"S_species", EMBI_JS_WK_SPECIES},
        };
        for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
            embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, syms[i].name),
                               embi_js_wk_sym(e.rt, syms[i].wk), 0);
        }
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "regexp.js", 0);
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "regexp.js", 0);
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

EMB_TEST(regexp_constructor_and_source_flags)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "new RegExp('a/b').source === 'a\\\\/b'"));
    EMB_CHECK(js_true(e, "new RegExp('').source === '(?:)' && RegExp.prototype.source === '(?:)'"));
    EMB_CHECK(js_true(e, "new RegExp('\\n').source === '\\\\n'"));
    EMB_CHECK(js_true(e, "new RegExp('a', 'dgimsy').flags === 'dgimsy'"));
    EMB_CHECK(js_true(e, "new RegExp('a', 'v').unicodeSets === true && new RegExp('a').unicode === false"));
    EMB_CHECK(js_true(e, "new RegExp('a', 'gi').global === true && new RegExp('a', 'gi').ignoreCase === true"));
    EMB_CHECK(js_true(e, "RegExp.prototype.global === undefined && RegExp.prototype.flags === ''"));
    EMB_CHECK(js_true(e, "new RegExp('x').lastIndex === 0"));
    EMB_CHECK(js_true(e, "String(new RegExp('a+', 'g')) === '/a+/g'"));
    EMB_CHECK(js_true(e, "Object.getOwnPropertyDescriptor(new RegExp('x'), 'lastIndex').writable === true"));
    EMB_CHECK(js_true(e, "RegExp[S_species] === RegExp"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(/a/) === '[object RegExp]'"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call(new (class extends RegExp {})('a')) === '[object RegExp]'"));
    EMB_CHECK(js_throws(e, "new RegExp('(')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "new RegExp('a', 'gg')", "SyntaxError"));
    EMB_CHECK(js_throws(e, "new RegExp('a', 'uv')", "SyntaxError"));
    EMB_CHECK(js_true(e, "(function(){ var r = /a/g; return RegExp(r) === r; })()"));
    EMB_CHECK(js_true(e, "new RegExp(/a/gi).flags === 'gi' && new RegExp(/a/g, 'y').flags === 'y'"));
    EMB_CHECK(js_throws(e, "RegExp.prototype.exec.call({}, 'x')", "TypeError"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_exec_and_lastindex)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var m = /(a)(?<n>b)?(c)/.exec('xxacd'); m.index === 2 && m[0] === 'ac' && m[1] === 'a' && m[2] === undefined && m.groups.n === undefined"));
    EMB_CHECK(js_true(e, "var r = /a/g; r.exec('aa'); r.lastIndex === 1 && r.exec('aa').index === 1 && r.exec('aa') === null && r.lastIndex === 0"));
    EMB_CHECK(js_true(e, "var s = /a/y; s.lastIndex = 1; s.exec('ba') !== null && s.lastIndex === 2"));
    EMB_CHECK(js_true(e, "var t = /a/y; t.lastIndex = 0; t.exec('ba') === null && t.lastIndex === 0"));
    EMB_CHECK(js_true(e, "/a/.test('cat') && !/z/.test('cat')"));
    EMB_CHECK(js_true(e, "var d = /(?<y>\\d{4})/d.exec('in 2026'); d.indices[0][0] === 3 && d.indices.groups.y[1] === 7"));
    EMB_CHECK(js_true(e, "/(?<a>x)|(?<a>y)/.exec('y').groups.a === 'y'"));
    EMB_CHECK(js_true(e, "var l = /a/g; l.lastIndex = 10; l.exec('aaa') === null && l.lastIndex === 0"));
    EMB_CHECK(js_true(e, "/\\u{1F600}/u.exec('x\\u{1F600}').index === 1"));
    EMB_CHECK(js_true(e, "/./u.exec('\\uD83D\\uDE00')[0].length === 2"));
    EMB_CHECK(js_true(e, "/(?<=\\$)\\d+/.exec('cost $42')[0] === '42'"));
    EMB_CHECK(js_true(e, "/\\bfoo\\b/i.test('a FOO b')"));
    EMB_CHECK(js_true(e, "/^b/m.test('a\\nb')"));
    EMB_CHECK(js_true(e, "/a.c/s.test('a\\nc') && !/a.c/.test('a\\nc')"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_symbol_match_replace_search_split)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "'a1b22c333'.match(/\\d+/g).join(',') === '1,22,333'"));
    EMB_CHECK(js_true(e, "'abc'.match(/z/g) === null && 'abc'.match(/b/).index === 1"));
    EMB_CHECK(js_true(e, "'aaa'.replace(/a/g, 'b') === 'bbb' && 'aaa'.replace(/a/, 'b') === 'baa'"));
    EMB_CHECK(js_true(e, "'john smith'.replace(/(\\w+)\\s(\\w+)/, '$2, $1') === 'smith, john'"));
    EMB_CHECK(js_true(e, "'x'.replace(/x/, '[$&|$`|$\\']') === '[x||]'"));
    EMB_CHECK(js_true(e, "'2026-11'.replace(/(?<y>\\d+)-(?<m>\\d+)/, '$<m>/$<y>') === '11/2026'"));
    EMB_CHECK(js_true(e, "'abc'.replace(/b/, '$1$$') === 'a$1$c'"));
    EMB_CHECK(js_true(e, "'abc'.replace(/(b)/, function (m, p1, off) { return p1 + off + m; }) === 'ab1bc'"));
    EMB_CHECK(js_true(e, "'a-b_c'.replace(/[-_]/g, function (m) { return '+'; }) === 'a+b+c'"));
    EMB_CHECK(js_true(e, "'abc'.search(/c/) === 2 && 'abc'.search(/z/) === -1"));
    EMB_CHECK(js_true(e, "'a1b2c'.split(/\\d/).join('|') === 'a|b|c'"));
    EMB_CHECK(js_true(e, "'a1b2c'.split(/(\\d)/).join('|') === 'a|1|b|2|c'"));
    EMB_CHECK(js_true(e, "''.split(/x/).length === 1 && ''.split(/(?:)/).length === 0"));
    EMB_CHECK(js_true(e, "'abc'.split(/(?:)/).join('|') === 'a|b|c'"));
    EMB_CHECK(js_true(e, "'a,b,c'.split(/,/, 2).length === 2"));
    EMB_CHECK(js_true(e, "/x/[S_replace]('axb', 'y') === 'ayb'"));
    EMB_CHECK(js_true(e, "/x/[S_split]('axb').length === 2"));
    EMB_CHECK(js_true(e, "/a/[S_match]('a') !== null"));
    EMB_CHECK(js_true(e, "/a/[S_search]('ba') === 1"));
    EMB_CHECK(js_true(e, "'\\uD83D\\uDE00\\uD83D\\uDE00'.split(/(?:)/u).length === 2"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_matchall_iterator_and_strings)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "var out = []; for (var m of 'a1b2'.matchAll(/[a-z](\\d)/g)) out.push(m[1] + '@' + m.index); out.join(',') === '1@0,2@2'"));
    EMB_CHECK(js_throws(e, "'a'.matchAll(/a/)", "TypeError"));
    EMB_CHECK(js_true(e, "Object.prototype.toString.call('a'.matchAll(/a/g)) === '[object RegExp String Iterator]'"));
    EMB_CHECK(js_true(e, "'aXbX'.replaceAll(/X/g, '-') === 'a-b-'"));
    EMB_CHECK(js_throws(e, "'aXb'.replaceAll(/X/, '-')", "TypeError"));
    EMB_CHECK(js_true(e, "'ab'.replace('b', '$&$&') === 'abb'"));
    EMB_CHECK(js_true(e, "'x'.match('x') !== null"));
    EMB_CHECK(js_true(e, "'Ab'.search('b') === 1"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_escape_and_compile)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e, "RegExp.escape('a.b') === '\\\\x61\\\\.b'"));
    EMB_CHECK(js_true(e, "RegExp.escape('.*') === '\\\\.\\\\*'"));
    EMB_CHECK(js_true(e, "RegExp.escape('1') === '\\\\x31'"));
    EMB_CHECK(js_throws(e, "RegExp.escape(1)", "TypeError"));
    EMB_CHECK(js_true(e, "var c = /a/g; c.compile('b', 'i'); c.source === 'b' && c.flags === 'i' && c.lastIndex === 0"));
    EMB_CHECK(js_throws(e, "/a/.compile(/b/, 'g')", "TypeError"));
    EMB_CHECK(js_true(e, "var rr = /x/; rr.lastIndex = 3; rr.compile('y'); rr.lastIndex === 0"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_iterator_survives_gc)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e,
        "var total = 0; for (var k = 0; k < 40; k++) {"
        "  var s = ''; for (var j = 0; j < 200; j++) s += 'ab';"
        "  var it = s.matchAll(/a(b)/g); for (var m of it) total += m[1].length; }"
        "total === 8000"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
