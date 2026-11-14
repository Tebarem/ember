#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* Number builtins through script. toString(radix), toFixed, toExponential and toPrecision are
 * checked against the exact outputs ECMA-262 specifies. */

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
        /* A symbol value for the ToNumber TypeError checks (the Symbol global is not in J1). */
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "number.js", 0);
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "number.js", 0);
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

EMB_TEST(number_constructor_forms)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Number('12') === 12 && Number() === 0"));
    EMB_CHECK(js_true(e, "isNaN(Number(undefined)) && Number(null) === 0 && Number(true) === 1"));
    EMB_CHECK(js_true(e, "Number('0x1f') === 31 && isNaN(Number('1x'))"));
    EMB_CHECK(js_true(e, "typeof new Number(1) === 'object' && new Number(5) + 1 === 6"));
    EMB_CHECK(js_true(e, "new Number(2) instanceof Number && new Number(2).valueOf() === 2"));
    EMB_CHECK(js_true(e, "Number.length === 1 && Number.prototype.valueOf() === 0"));
    EMB_CHECK(js_true(e, "Number.prototype.constructor === Number"));
    EMB_CHECK(js_throws(e, "Number(S_iterator)", "TypeError"));
    env_free(e);
}

EMB_TEST(number_constants_and_statics)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Number.MAX_SAFE_INTEGER === 9007199254740991"));
    EMB_CHECK(js_true(e, "Number.MIN_SAFE_INTEGER === -9007199254740991"));
    EMB_CHECK(js_true(e, "Number.MIN_VALUE === 5e-324 && Number.MIN_VALUE > 0"));
    EMB_CHECK(js_true(e, "Number.MAX_VALUE === 1.7976931348623157e308"));
    EMB_CHECK(js_true(e, "Number.EPSILON === 2.220446049250313e-16"));
    EMB_CHECK(js_true(e, "isNaN(Number.NaN) && Number.POSITIVE_INFINITY === Infinity"));
    EMB_CHECK(js_true(e, "Number.NEGATIVE_INFINITY === -Infinity"));
    EMB_CHECK(js_true(e, "Number.isFinite(1) && !Number.isFinite('1') && !Number.isFinite(Infinity)"));
    EMB_CHECK(js_true(e, "Number.isInteger(5) && Number.isInteger(-0) && !Number.isInteger(5.5)"));
    EMB_CHECK(js_true(e, "!Number.isInteger('5') && !Number.isInteger(Infinity)"));
    EMB_CHECK(js_true(e, "Number.isNaN(NaN) && !Number.isNaN('NaN') && !Number.isNaN(1)"));
    EMB_CHECK(js_true(e, "Number.isSafeInteger(9007199254740991) && !Number.isSafeInteger(9007199254740992)"));
    EMB_CHECK(js_true(e, "Number.isSafeInteger(-9007199254740991) && !Number.isSafeInteger(1.5)"));
    EMB_CHECK(js_true(e, "Number.parseFloat === parseFloat && Number.parseInt === parseInt"));
    EMB_CHECK(js_true(e, "Number.parseFloat('3.5x') === 3.5 && Number.parseInt('0x1F') === 31"));
    env_free(e);
}

EMB_TEST(number_to_string_radix)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "(255).toString() === '255' && (255).toString(16) === 'ff'"));
    EMB_CHECK(js_true(e, "(-255).toString(2) === '-11111111'"));
    EMB_CHECK(js_true(e, "(0.5).toString(2) === '0.1'"));
    EMB_CHECK(js_true(e, "(3.75).toString(16) === '3.c'"));
    EMB_CHECK(js_true(e, "(35).toString(36) === 'z' && (0).toString(2) === '0'"));
    EMB_CHECK(js_true(e, "NaN.toString(2) === 'NaN' && (-Infinity).toString(16) === '-Infinity'"));
    EMB_CHECK(js_true(e, "(1e21).toString() === '1e+21' && (1e-7).toString() === '1e-7'"));
    EMB_CHECK(js_true(e, "(-0).toString() === '0' && (0.1).toString() === '0.1'"));
    EMB_CHECK(js_true(e, "(123.456).toString(10) === '123.456'"));
    EMB_CHECK(js_true(e, "(10).toString(undefined) === '10'"));
    EMB_CHECK(js_throws(e, "(1).toString(1)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toString(37)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toString(0)", "RangeError"));
    EMB_CHECK(js_throws(e, "Number.prototype.toString.call('1')", "TypeError"));
    env_free(e);
}

EMB_TEST(number_to_fixed)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "(1.005).toFixed(2) === '1.00'"));
    EMB_CHECK(js_true(e, "(1.45).toFixed(1) === '1.4'"));
    EMB_CHECK(js_true(e, "(0).toFixed(2) === '0.00' && (0).toFixed() === '0'"));
    EMB_CHECK(js_true(e, "(-1.5).toFixed(0) === '-2' && (2.5).toFixed(0) === '3'"));
    EMB_CHECK(js_true(e, "(1e21).toFixed(2) === '1e+21' && (-1e21).toFixed(2) === '-1e+21'"));
    EMB_CHECK(js_true(e, "(123.456).toFixed(10) === '123.4560000000'"));
    EMB_CHECK(js_true(e, "(0.000001).toFixed(7) === '0.0000010'"));
    EMB_CHECK(js_true(e, "NaN.toFixed(2) === 'NaN' && (Infinity).toFixed(2) === 'Infinity'"));
    EMB_CHECK(js_true(e, "(1.23).toFixed(undefined) === '1'"));
    EMB_CHECK(js_true(e, "(1.23).toFixed(100).length === 102"));
    EMB_CHECK(js_throws(e, "(1).toFixed(101)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toFixed(-1)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toFixed(Infinity)", "RangeError"));
    EMB_CHECK(js_throws(e, "Number.prototype.toFixed.call({}, 1)", "TypeError"));
    env_free(e);
}

EMB_TEST(number_to_exponential)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "(123456).toExponential(2) === '1.23e+5'"));
    EMB_CHECK(js_true(e, "(0).toExponential() === '0e+0' && (0).toExponential(2) === '0.00e+0'"));
    EMB_CHECK(js_true(e, "(123).toExponential() === '1.23e+2'"));
    EMB_CHECK(js_true(e, "(-0.00015).toExponential(1) === '-1.5e-4'"));
    EMB_CHECK(js_true(e, "(1.5).toExponential(0) === '2e+0'"));
    EMB_CHECK(js_true(e, "NaN.toExponential(2) === 'NaN' && (-Infinity).toExponential() === '-Infinity'"));
    EMB_CHECK(js_throws(e, "(1).toExponential(101)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toExponential(-1)", "RangeError"));
    EMB_CHECK(js_throws(e, "Number.prototype.toExponential.call(1, Infinity)", "RangeError"));
    env_free(e);
}

EMB_TEST(number_to_precision)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "(123.456).toPrecision(4) === '123.5'"));
    EMB_CHECK(js_true(e, "(0.000123).toPrecision(2) === '0.00012'"));
    EMB_CHECK(js_true(e, "(1e21).toPrecision(3) === '1.00e+21'"));
    EMB_CHECK(js_true(e, "(123.456).toPrecision() === '123.456'"));
    EMB_CHECK(js_true(e, "(0).toPrecision(3) === '0.00'"));
    EMB_CHECK(js_true(e, "(1e-7).toPrecision(1) === '1e-7'"));
    EMB_CHECK(js_true(e, "NaN.toPrecision(3) === 'NaN'"));
    EMB_CHECK(js_throws(e, "(1).toPrecision(0)", "RangeError"));
    EMB_CHECK(js_throws(e, "(1).toPrecision(101)", "RangeError"));
    EMB_CHECK(js_throws(e, "Number.prototype.toPrecision.call(1, Infinity)", "RangeError"));
    env_free(e);
}

EMB_TEST(number_value_of_and_locale)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "(7).valueOf() === 7 && new Number(7).valueOf() === 7"));
    EMB_CHECK(js_true(e, "(1234).toLocaleString() === '1,234'"));
    EMB_CHECK(js_throws(e, "Number.prototype.valueOf.call('1')", "TypeError"));
    EMB_CHECK(js_throws(e, "Number.prototype.toLocaleString.call({})", "TypeError"));
    env_free(e);
}

EMB_TEST_MAIN()
