#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* Boolean builtins through script. */

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
        /* A symbol value for the truthiness check (the Symbol global is not in J1). */
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "boolean.js", 0);
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "boolean.js", 0);
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

EMB_TEST(boolean_conversion_and_wrappers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Boolean(0) === false && Boolean('0') === true"));
    EMB_CHECK(js_true(e, "Boolean() === false && Boolean('') === false && Boolean({}) === true"));
    EMB_CHECK(js_true(e, "Boolean(NaN) === false && Boolean(-0) === false && Boolean(1) === true"));
    EMB_CHECK(js_true(e, "Boolean(null) === false && Boolean(S_iterator) === true"));
    EMB_CHECK(js_true(e, "typeof new Boolean(false) === 'object'"));
    EMB_CHECK(js_true(e, "new Boolean(false) ? true : false"));
    EMB_CHECK(js_true(e, "new Boolean(false) instanceof Boolean"));
    EMB_CHECK(js_true(e, "new Boolean(false).valueOf() === false"));
    EMB_CHECK(js_true(e, "Boolean.length === 1 && Boolean.prototype.valueOf() === false"));
    EMB_CHECK(js_true(e, "Boolean.prototype.constructor === Boolean"));
    env_free(e);
}

EMB_TEST(boolean_prototype_methods)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Boolean.prototype.toString() === 'false'"));
    EMB_CHECK(js_true(e, "true.toString() === 'true' && new Boolean(true).toString() === 'true'"));
    EMB_CHECK(js_true(e, "Boolean.prototype.valueOf.call(true) === true"));
    EMB_CHECK(js_true(e, "Boolean.prototype.valueOf.call(new Boolean(true)) === true"));
    EMB_CHECK(js_throws(e, "Boolean.prototype.toString.call(1)", "TypeError"));
    EMB_CHECK(js_throws(e, "Boolean.prototype.valueOf.call('true')", "TypeError"));
    EMB_CHECK(js_throws(e, "Boolean.prototype.toString.call({})", "TypeError"));
    env_free(e);
}

EMB_TEST_MAIN()
