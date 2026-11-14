#include "emb_test.h"

#include "base/base.h"
#include "js/js.h"

#include <string.h>

/* RegExp fast and generic paths (J4c). The fast paths of test, @@match, @@replace and @@split must be
 * invisible: each case runs once on a plain RegExp (fast path) and once on an equivalent object that
 * has an own exec (generic path), and the results must agree. Overridden steps must force the
 * generic path. Species, throwing reads, non-writable lastIndex and the engine's error mapping are
 * covered here too. */

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

/* True when v is a string equal to want. */
static bool str_is(embi_js_value v, const char *want)
{
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_as_string(v);
    if (embi_js_str_len(s) != strlen(want)) {
        return false;
    }
    for (size_t i = 0; i < strlen(want); i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint8_t)want[i]) {
            return false;
        }
    }
    return true;
}

/* Evaluates src. Returns true when it yields the boolean true; logs otherwise. */
static bool js_true(env e, const char *src)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "regexp_paths.js", 0);
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

/* Evaluates src, which must throw an error with the given name and, when msg is not nullptr, the
 * given message. */
static bool js_throws(env e, const char *src, const char *name, const char *msg)
{
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "regexp_paths.js", 0);
    if (v != EMBI_JS_EXCEPTION) {
        SDL_Log("    expected %s: %s", name, src);
        return false;
    }
    embi_js_value ex = embi_js_exception_get(e.rt);
    bool ok = false;
    if (embi_js_is_object(ex)) {
        embi_js_object *eo = embi_js_as_object(ex);
        ok = str_is(embi_js_get(e.rt, eo, embi_js_key_c(e.rt, "name")), name) &&
             (msg == nullptr || str_is(embi_js_get(e.rt, eo, embi_js_key_c(e.rt, "message")), msg));
    }
    if (!ok) {
        SDL_Log("    wrong exception (wanted %s): %s", name, src);
    }
    embi_js_exception_clear(e.rt);
    return ok;
}

/* The interrupt hook: asks the engine or the interpreter to stop from its fourth poll on. */
static bool stop_after_polls(embi_js_runtime *rt, void *user)
{
    (void)rt;
    int *n = user;
    *n += 1;
    return *n > 3;
}

EMB_TEST(regexp_fast_and_generic_paths_agree)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    /* slow(r) is the same pattern and flags with an own exec, which forces the generic path. */
    EMB_CHECK(js_true(e,
        "function slow(r) { var s = new RegExp(r.source, r.flags); s.exec = RegExp.prototype.exec; return s; }"
        "var pats = [/(a)(b)?/g, /x*/g, /(?<n>\\d)/g, /b/y, /(\\d)/, /(?:)/g, /(?:)/u, /./gu, /\\d+/g,"
        "            /^/gm, /(?<=a)b/g, /a/gy, /\\uD83D/u, /[^]/g];"
        "var inputs = ['xabaab', 'axxb', 'a1b2', 'ab', '9 8', '', 'a\\uD83D\\uDE00x', 'bab'];"
        "var reps = ['[$&]', '<$1|$2>', '$<n>', '$`$\\'', function (m, a) { return '{' + m + (a === undefined ? 'U' : a) + '}'; }];"
        "var ok = true;"
        "for (var i = 0; i < pats.length; i++) for (var j = 0; j < inputs.length; j++) {"
        "  var f = pats[i], s = slow(f), x = inputs[j];"
        "  for (var k = 0; k < reps.length; k++) ok = ok && f[Symbol.replace](x, reps[k]) === s[Symbol.replace](x, reps[k]);"
        "  ok = ok && JSON.stringify(x.split(f)) === JSON.stringify(x.split(s));"
        "  ok = ok && JSON.stringify(x.match(f)) === JSON.stringify(x.match(s));"
        "  ok = ok && f.test(x) === s.test(x);"
        "  ok = ok && x.search(f) === x.search(s);"
        "  ok = ok && f.lastIndex === s.lastIndex;"
        "}"
        "ok"));
    EMB_CHECK(js_true(e, "'aXbX'.replace(/X/g, function (m, p, str) { return String(p + str.length); }) === 'a5b7'"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_overridden_steps_force_generic_path)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    /* An own exec on the object: test, replace and match must call it. */
    EMB_CHECK(js_true(e,
        "(function () { var r = /x/g, calls = 0;"
        "  r.exec = function () { calls++; return null; };"
        "  return 'axbx'.replace(r, 'Y') === 'axbx' && calls === 1 && r.test('x') === false && calls === 2"
        "    && 'x'.match(r) === null && calls === 3; })()"));
    /* An own exec returning a fake result. */
    EMB_CHECK(js_true(e,
        "(function () { var r = /x/g, calls = 0;"
        "  r.exec = function () { calls++; if (calls === 1) return { 0: 'x', index: 1, length: 1 }; return null; };"
        "  return 'axbx'.replace(r, 'Y') === 'aYbx' && calls === 2; })()"));
    /* An overridden exec on the prototype is seen by the Symbol methods and by test. */
    EMB_CHECK(js_true(e,
        "(function () { var orig = RegExp.prototype.exec, n = 0;"
        "  RegExp.prototype.exec = function (s) { n++; return orig.call(this, s); };"
        "  var r = /a/.test('a') && 'aXa'.replace(/a/g, 'b') === 'bXb' && n === 4;"
        "  RegExp.prototype.exec = orig; return r; })()"));
    /* An overridden flag getter on the prototype: replace reads flags, so global is false. */
    EMB_CHECK(js_true(e,
        "(function () { var orig = Object.getOwnPropertyDescriptor(RegExp.prototype, 'global'), n = 0;"
        "  Object.defineProperty(RegExp.prototype, 'global', { get: function () { n++; return false; }, configurable: true });"
        "  var r = 'aa'.replace(/a/g, 'b') === 'ba' && n > 0;"
        "  Object.defineProperty(RegExp.prototype, 'global', orig); return r; })()"));
    /* An own flags getter that drops g: match takes the non-global branch, which returns the exec result. */
    EMB_CHECK(js_true(e,
        "(function () { var r = /a/g; Object.defineProperty(r, 'flags', { get: function () { return 'y'; } });"
        "  var m = 'aa'.match(r); return m !== null && m[0] === 'a'; })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_species_and_subclass)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_true(e,
        "(function () { var made = 0;"
        "  class Sp extends RegExp { constructor(p, f) { made++; super(p, f); } }"
        "  var parts = 'a-b-c'.split(new Sp('-'));"
        "  return made >= 2 && parts.join('|') === 'a|b|c'; })()"));
    EMB_CHECK(js_true(e,
        "(function () { var made = 0;"
        "  class Sp extends RegExp { constructor(p, f) { made++; super(p, f); } }"
        "  var got = Array.from('a1a2'.matchAll(new Sp('a', 'g')));"
        "  return got.length === 2 && got[1].index === 2 && made >= 2; })()"));
    /* A species that is RegExp keeps split on the fast path with the same results. */
    EMB_CHECK(js_true(e,
        "(function () { class NoSp extends RegExp { static get [Symbol.species]() { return RegExp; } }"
        "  return 'a-b'.split(new NoSp('-')).join('|') === 'a|b' && 'a-b'.split(new NoSp('-', 'y')).length === 2; })()"));
    /* A species that returns a constructor which ignores its arguments. */
    EMB_CHECK(js_true(e,
        "(function () { var r = /-/; r.constructor = { [Symbol.species]: function () { return /x/; } };"
        "  return 'a-b'.split(r).length === 1; })()"));
    /* compile on a subclass instance is not an Annex B legacy call. */
    EMB_CHECK(js_throws(e, "class Sub extends RegExp {} new Sub('a').compile('b')", "TypeError", nullptr));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_user_reads_that_throw)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    EMB_CHECK(js_throws(e,
        "var r = /a/g; Object.defineProperty(r, 'flags', { get: function () { throw new SyntaxError('f'); } }); 'a'.replace(r, 'b')",
        "SyntaxError", "f"));
    EMB_CHECK(js_throws(e,
        "var q = /a/; Object.defineProperty(q, 'source', { get: function () { throw new EvalError('s'); } }); String(q)",
        "EvalError", "s"));
    EMB_CHECK(js_throws(e,
        "var x = /a/; x.exec = function () { return { 0: 'a', get index() { throw new URIError('i'); }, length: 1 }; };"
        "'a'.replace(x, 'b')",
        "URIError", "i"));
    EMB_CHECK(js_throws(e,
        "var z = /a/; z.exec = function () { return { get 0() { throw new TypeError('z0'); }, index: 0, length: 1 }; };"
        "'a'.replace(z, 'b')",
        "TypeError", "z0"));
    EMB_CHECK(js_throws(e,
        "var w = /a/; w.exec = function () { return { 0: 'a', index: 0, length: 4294967296 }; }; 'a'.replace(w, 'b')",
        "RangeError", nullptr));
    /* A non-writable lastIndex: a global replace writes it first, a sticky test writes it on a match,
     * a plain test does not write it. */
    EMB_CHECK(js_throws(e,
        "var g = /a/g; Object.defineProperty(g, 'lastIndex', { writable: false }); 'aa'.replace(g, 'b')",
        "TypeError", nullptr));
    EMB_CHECK(js_throws(e,
        "var y = /a/y; Object.defineProperty(y, 'lastIndex', { writable: false }); y.test('a')",
        "TypeError", nullptr));
    EMB_CHECK(js_true(e,
        "var p = /a/; Object.defineProperty(p, 'lastIndex', { writable: false }); p.test('a') === true"));
    /* lastIndex as an object: ToLength runs its valueOf, which replaces the prototype's exec. The exec
     * was looked up before that, so this test still runs the built-in exec and sees a match. */
    EMB_CHECK(js_true(e,
        "(function () { var orig = RegExp.prototype.exec, r = /a/g, hit = 0;"
        "  r.lastIndex = { valueOf: function () { RegExp.prototype.exec = function () { hit++; return null; }; return 0; } };"
        "  var out = r.test('a'); var changed = RegExp.prototype.exec !== orig; RegExp.prototype.exec = orig;"
        "  return out === true && changed && hit === 0; })()"));
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST(regexp_engine_limits_map_to_js_errors)
{
    size_t mem0 = embi_mem_live();
    env e = env_new();
    /* The interrupt hook stops a catastrophic pattern: the error text is the interpreter's own. */
    int polls = 0;
    embi_js_set_interrupt(e.rt, stop_after_polls, &polls);
    EMB_CHECK(js_throws(e, "var s = 'a'.repeat(40); /(a+)+b/.test(s)", "Error", "Script was interrupted"));
    EMB_CHECK(polls > 3);
    env_free(e);
    EMB_CHECK(embi_mem_live() == mem0);

    /* The backtracking memory cap throws a RangeError ("regular expression too complex"). */
    env f = env_new();
    EMB_CHECK(js_throws(f, "var t = 'a'.repeat(2000000); /(?:a|b)*c/.test(t)", "RangeError",
                        "regular expression too complex"));
    env_free(f);
    EMB_CHECK(embi_mem_live() == mem0);
}

EMB_TEST_MAIN()
