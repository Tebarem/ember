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
        SDL_Log("    js: %s", src);
    }
    return ok;
}

#define T_TEXT(src_, want_) EMB_CHECK(check_text(e, (src_), (want_)))
#define T_THROWS(src_, want_) EMB_CHECK(check_throws(e, (src_), (want_)))

/* ------------------------------------------------------------------ parseInt */

EMB_TEST(global_parse_int_basics)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("parseInt('42') + ''", "42");
    T_TEXT("parseInt('  42px') + ''", "42");
    T_TEXT("parseInt('\\t\\n\\u00A0\\uFEFF-7') + ''", "-7");
    T_TEXT("parseInt('+5') + ''", "5");
    T_TEXT("parseInt('1e3') + ''", "1");
    T_TEXT("parseInt('3.9') + ''", "3");
    T_TEXT("parseInt('') + ''", "NaN");
    T_TEXT("parseInt('-') + ''", "NaN");
    T_TEXT("parseInt('abc') + ''", "NaN");
    /* The sign of zero is kept. */
    T_TEXT("(1 / parseInt('-0')) + ''", "-Infinity");
    T_TEXT("(1 / parseInt('0')) + ''", "Infinity");
    /* Missing or non-string input converts with ToString first. */
    T_TEXT("parseInt() + ''", "NaN");
    T_TEXT("parseInt(null, 36) + ''", "1112745");
    T_TEXT("parseInt(1e21) + ''", "1");
    /* Large values are correctly rounded. */
    T_TEXT("(parseInt('123456789012345678901234567890') === 1.2345678901234568e+29) + ''",
           "true");
    env_free(e);
}

EMB_TEST(global_parse_int_radix)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("parseInt('0x1F') + ''", "31");
    T_TEXT("parseInt('0X1f', 16) + ''", "31");
    T_TEXT("parseInt('0x1F', 10) + ''", "0");
    T_TEXT("parseInt('0x') + ''", "NaN");
    T_TEXT("parseInt('0x', 16) + ''", "NaN");
    T_TEXT("parseInt('11', 2) + ''", "3");
    T_TEXT("parseInt('z', 36) + ''", "35");
    T_TEXT("parseInt('Z', 36) + ''", "35");
    T_TEXT("parseInt('777', 8) + ''", "511");
    T_TEXT("parseInt('10', 0) + ''", "10");
    T_TEXT("parseInt('10', 1) + ''", "NaN");
    T_TEXT("parseInt('10', 37) + ''", "NaN");
    /* The radix goes through ToInt32, so 2^32 + 16 is 16. */
    T_TEXT("parseInt('10', 4294967312) + ''", "16");
    T_TEXT("parseInt('2', 2) + ''", "NaN");
    env_free(e);
}

/* ------------------------------------------------------------------ parseFloat */

EMB_TEST(global_parse_float)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("parseFloat('3.14abc') + ''", "3.14");
    T_TEXT("parseFloat('  +1.5') + ''", "1.5");
    T_TEXT("parseFloat('.5') + ''", "0.5");
    T_TEXT("parseFloat('-.5e2') + ''", "-50");
    T_TEXT("parseFloat('1e') + ''", "1");
    T_TEXT("parseFloat('1e+') + ''", "1");
    T_TEXT("parseFloat('1.e1') + ''", "10");
    T_TEXT("parseFloat('1.') + ''", "1");
    T_TEXT("parseFloat('e1') + ''", "NaN");
    T_TEXT("parseFloat('.') + ''", "NaN");
    T_TEXT("parseFloat('') + ''", "NaN");
    T_TEXT("parseFloat('0x10') + ''", "0");
    T_TEXT("parseFloat('1e400') + ''", "Infinity");
    T_TEXT("parseFloat('-1e400') + ''", "-Infinity");
    T_TEXT("parseFloat('Infinity') + ''", "Infinity");
    T_TEXT("parseFloat('-Infinity!') + ''", "-Infinity");
    T_TEXT("parseFloat('Infinit') + ''", "NaN");
    T_TEXT("(1 / parseFloat('-0')) + ''", "-Infinity");
    T_TEXT("parseFloat('0.1') === 0.1 ? 'y' : 'n'", "y");
    T_TEXT("parseFloat('5e-324') === 5e-324 ? 'y' : 'n'", "y");
    T_TEXT("parseFloat('\\u00A0 12') + ''", "12");
    env_free(e);
}

/* ------------------------------------------------------------------ isNaN and isFinite */

EMB_TEST(global_is_nan_and_is_finite)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("isNaN('abc') + ''", "true");
    T_TEXT("isNaN('1') + ''", "false");
    T_TEXT("isNaN() + ''", "true");
    T_TEXT("isNaN(null) + ''", "false");
    T_TEXT("isNaN(NaN) + ''", "true");
    T_TEXT("isFinite('1') + ''", "true");
    T_TEXT("isFinite(Infinity) + ''", "false");
    T_TEXT("isFinite(null) + ''", "true");
    T_TEXT("isFinite() + ''", "false");
    T_TEXT("isFinite('x') + ''", "false");
    T_THROWS("isNaN(Object.create(null))", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ URI functions */

EMB_TEST(global_encode_uri)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("encodeURI('http://a b/\\u00FC?x=1&y=\\u00E9#f')",
           "http://a%20b/%C3%BC?x=1&y=%C3%A9#f");
    T_TEXT("encodeURI(';/?:@&=+$,#')", ";/?:@&=+$,#");
    T_TEXT("encodeURI('-_.!~*\\'()')", "-_.!~*'()");
    T_TEXT("encodeURIComponent('a/b?c d&e=f#g')", "a%2Fb%3Fc%20d%26e%3Df%23g");
    T_TEXT("encodeURIComponent(';/?:@&=+$,#')", "%3B%2F%3F%3A%40%26%3D%2B%24%2C%23");
    /* Supplementary characters take four bytes, from a surrogate pair. */
    T_TEXT("encodeURI('\\uD83D\\uDE00')", "%F0%9F%98%80");
    T_TEXT("encodeURIComponent('\\u20AC')", "%E2%82%AC");
    T_TEXT("encodeURI('\\u0000\\u007F')", "%00%7F");
    T_TEXT("encodeURI(12) + ''", "12");
    /* Lone surrogates are URIErrors. */
    T_THROWS("encodeURI('\\uD800')", "URIError");
    T_THROWS("encodeURIComponent('\\uDC00')", "URIError");
    T_THROWS("encodeURI('a\\uD800b')", "URIError");
    env_free(e);
}

EMB_TEST(global_decode_uri)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("decodeURI('%41%2F%23')", "A%2F%23");
    T_TEXT("decodeURIComponent('%41%2F%23')", "A/#");
    T_TEXT("decodeURI('%C3%BC%20x')", "\xC3\xBC x");
    T_TEXT("decodeURIComponent('%F0%9F%98%80').length + ''", "2");
    T_TEXT("decodeURIComponent('%F0%9F%98%80') === '\\uD83D\\uDE00' ? 'y' : 'n'", "y");
    T_TEXT("decodeURI('plain text') + ''", "plain text");
    T_TEXT("decodeURI('%7e%2a') + ''", "~*");
    T_TEXT("decodeURI('%E2%82%AC') === '\\u20AC' ? 'y' : 'n'", "y");
    /* Malformed escapes and sequences throw URIError. */
    T_THROWS("decodeURI('%')", "URIError");
    T_THROWS("decodeURI('%4')", "URIError");
    T_THROWS("decodeURI('%G1')", "URIError");
    T_THROWS("decodeURI('%C3')", "URIError");
    T_THROWS("decodeURI('%80')", "URIError");
    T_THROWS("decodeURI('%C3%28')", "URIError");
    T_THROWS("decodeURI('%C0%AF')", "URIError");
    T_THROWS("decodeURI('%ED%A0%80')", "URIError");
    T_THROWS("decodeURI('%F4%90%80%80')", "URIError");
    T_THROWS("decodeURI('%F8%80%80%80%80')", "URIError");
    T_THROWS("decodeURIComponent('%E2%82')", "URIError");
    env_free(e);
}

/* ------------------------------------------------------------------ globals and attributes */

EMB_TEST(global_bindings_and_attributes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("globalThis === this ? 'y' : 'n'", "y");
    T_TEXT("globalThis.globalThis === globalThis ? 'y' : 'n'", "y");
    T_TEXT("typeof print + typeof eval", "functionfunction");
    T_TEXT("eval('1 + 1') + ''", "2");
    T_TEXT("var gx = 5; eval('gx * 2') + ''", "10");
    T_TEXT("var seen = 0; for (var fk in globalThis) {"
           "  if (fk === 'parseInt' || fk === 'isNaN') seen++;"
           "} seen + ''",
           "0");
    /* Value properties are not writable, enumerable or configurable. */
    T_TEXT("var ud = Object.getOwnPropertyDescriptor(globalThis, 'undefined');"
           "'' + ud.writable + ud.enumerable + ud.configurable",
           "falsefalsefalse");
    T_TEXT("var nd = Object.getOwnPropertyDescriptor(globalThis, 'NaN');"
           "'' + nd.writable + nd.enumerable + nd.configurable",
           "falsefalsefalse");
    T_TEXT("var idesc = Object.getOwnPropertyDescriptor(globalThis, 'Infinity');"
           "'' + idesc.writable + idesc.enumerable + idesc.configurable",
           "falsefalsefalse");
    T_TEXT("undefined = 3; NaN = 4; typeof undefined + (NaN !== NaN)", "undefinedtrue");
    /* Function properties: writable and configurable, not enumerable, with their lengths. */
    T_TEXT("var fspec = {parseInt: 2, parseFloat: 1, isNaN: 1, isFinite: 1, encodeURI: 1,"
           "  encodeURIComponent: 1, decodeURI: 1, decodeURIComponent: 1};"
           "var bad = 0;"
           "for (var k in fspec) {"
           "  var f = globalThis[k];"
           "  var d = Object.getOwnPropertyDescriptor(globalThis, k);"
           "  if (typeof f !== 'function' || f.length !== fspec[k] || f.name !== k ||"
           "      d.enumerable || !d.writable || !d.configurable) bad++;"
           "}"
           "'' + bad",
           "0");
    env_free(e);
}

EMB_TEST_MAIN()
