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

/* ------------------------------------------------------------------ constructors and prototypes */

EMB_TEST(error_constructors_and_prototype_chain)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    /* Each constructor: function, length 1, its own name. */
    T_TEXT("var names = ['Error', 'TypeError', 'RangeError', 'ReferenceError', 'SyntaxError',"
           "  'EvalError', 'URIError'];"
           "var bad = 0;"
           "for (var i = 0; i < names.length; i++) {"
           "  var C = globalThis[names[i]];"
           "  if (typeof C !== 'function' || C.length !== 1 || C.name !== names[i] ||"
           "      C.prototype.name !== names[i] || C.prototype.message !== '' ||"
           "      C.prototype.constructor !== C) bad++;"
           "}"
           "'' + bad",
           "0");
    /* NativeErrors inherit from Error, both as constructors and as prototypes. */
    T_TEXT("Object.getPrototypeOf(TypeError) === Error && "
           "Object.getPrototypeOf(TypeError.prototype) === Error.prototype",
           "true");
    T_TEXT("Object.getPrototypeOf(Error) === Function.prototype", "true");
    T_TEXT("Object.getPrototypeOf(Error.prototype) === Object.prototype", "true");
    T_TEXT("Error.prototype.hasOwnProperty('toString') + ''", "true");
    /* The prototype link is fixed; name and message are writable but hidden. */
    T_TEXT("var pd = Object.getOwnPropertyDescriptor(Error, 'prototype');"
           "'' + pd.writable + pd.enumerable + pd.configurable",
           "falsefalsefalse");
    T_TEXT("var nd = Object.getOwnPropertyDescriptor(Error.prototype, 'name');"
           "'' + nd.writable + nd.enumerable + nd.configurable",
           "truefalsetrue");
    /* Each constructor is hidden on the global object. */
    T_TEXT("var gd = Object.getOwnPropertyDescriptor(globalThis, 'RangeError');"
           "'' + gd.writable + gd.enumerable + gd.configurable",
           "truefalsetrue");
    env_free(e);
}

EMB_TEST(error_instances_and_message)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("new Error('m').message", "m");
    T_TEXT("Error('m').message", "m");
    T_TEXT("new TypeError(42).message", "42");
    T_TEXT("var noMsg = new Error(); noMsg.hasOwnProperty('message') + '' + noMsg.message + '|'",
           "false|");
    T_TEXT("new Error(undefined).hasOwnProperty('message') + ''", "false");
    T_TEXT("var me = new Error('m'); var md = Object.getOwnPropertyDescriptor(me, 'message');"
           "'' + md.writable + md.enumerable + md.configurable",
           "truefalsetrue");
    T_TEXT("new Error('x') instanceof Error", "true");
    T_TEXT("new RangeError('x') instanceof Error", "true");
    T_TEXT("new RangeError('x') instanceof TypeError", "false");
    T_TEXT("Object.prototype.toString.call(new SyntaxError('s'))", "[object Error]");
    T_TEXT("new URIError().name", "URIError");
    env_free(e);
}

/* ------------------------------------------------------------------ cause */

EMB_TEST(error_cause_option)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("new Error('m', {cause: 42}).cause + ''", "42");
    T_TEXT("new Error('m', {}).hasOwnProperty('cause') + ''", "false");
    /* The option is looked up with HasProperty, so an inherited cause counts. */
    T_TEXT("new Error('m', Object.create({cause: 'inh'})).cause", "inh");
    /* Non-object options are ignored. */
    T_TEXT("new Error('m', 5).hasOwnProperty('cause') + ''", "false");
    T_TEXT("var ce = new Error('m', {cause: undefined});"
           "ce.hasOwnProperty('cause') + ''",
           "true");
    /* The cause is non-enumerable, like message. */
    T_TEXT("var cd = Object.getOwnPropertyDescriptor(new Error('m', {cause: 1}), 'cause');"
           "'' + cd.writable + cd.enumerable + cd.configurable",
           "truefalsetrue");
    env_free(e);
}

/* ------------------------------------------------------------------ Error.prototype.toString */

EMB_TEST(error_prototype_to_string)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("new Error().toString()", "Error");
    T_TEXT("new Error('m').toString()", "Error: m");
    T_TEXT("new TypeError('bad').toString()", "TypeError: bad");
    T_TEXT("Error.prototype.toString.call({name: '', message: 'only'})", "only");
    T_TEXT("Error.prototype.toString.call({name: 'N', message: ''})", "N");
    T_TEXT("Error.prototype.toString.call({message: 'm'})", "Error: m");
    T_TEXT("Error.prototype.toString.call({name: undefined, message: undefined})", "Error");
    T_TEXT("Error.prototype.toString.call({name: 1, message: 2})", "1: 2");
    T_TEXT("var sub = Object.create(Error.prototype); sub.name = 'Sub'; sub.toString()", "Sub");
    T_THROWS("Error.prototype.toString.call(1)", "TypeError");
    T_THROWS("Error.prototype.toString.call(undefined)", "TypeError");
    env_free(e);
}

/* ------------------------------------------------------------------ engine-thrown errors */

EMB_TEST(error_engine_throws_use_realm_prototypes)
{
    env e = env_new();
    EMB_REQUIRE(e.r != nullptr);
    T_TEXT("var t; try { null.x; } catch (x) { t = x; } (t instanceof TypeError) + ''", "true");
    T_TEXT("var r; try { undefinedVariable; } catch (x) { r = x; }"
           "(r instanceof ReferenceError) + ''",
           "true");
    T_TEXT("var sy; try { throw new RangeError('r'); } catch (x) { sy = x; }"
           "(sy instanceof RangeError) + ''",
           "true");
    env_free(e);
}

EMB_TEST_MAIN()
