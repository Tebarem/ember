#include "emb_test.h"

#include "js/js.h"

#include <string.h>

/* Math builtins through script. Results that ECMA-262 pins down exactly (round, floor, ceil,
 * trunc, sign, abs, min and max with -0 and NaN, fround, clz32, imul, pow's special cases) are
 * checked exactly; the transcendental functions are checked at points where the result is
 * exact or unambiguous. */

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
        embi_js_define_raw(e.rt, e.r->global, embi_js_key_c(e.rt, "S_toStringTag"),
                           embi_js_wk_sym(e.rt, EMBI_JS_WK_TO_STRING_TAG), 0);
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
    embi_js_value v = embi_js_eval(e.r, src, strlen(src), "math.js", 0);
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

EMB_TEST(math_object_and_constants)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "typeof Math === 'object' && Math.PI === 3.141592653589793"));
    EMB_CHECK(js_true(e, "Math.E === 2.718281828459045 && Math.LN2 === 0.6931471805599453"));
    EMB_CHECK(js_true(e, "Math.LN10 === 2.302585092994046 && Math.LOG2E === 1.4426950408889634"));
    EMB_CHECK(js_true(e, "Math.LOG10E === 0.4342944819032518 && Math.SQRT2 === 1.4142135623730951"));
    EMB_CHECK(js_true(e, "Math.SQRT1_2 === 0.7071067811865476"));
    EMB_CHECK(js_true(e, "Math[S_toStringTag] === 'Math'"));
    EMB_CHECK(js_true(e, "Math.PI = 3, Math.PI === 3.141592653589793"));
    EMB_CHECK(js_true(e, "Math.max.length === 2 && Math.abs.length === 1 && Math.random.length === 0"));
    EMB_CHECK(js_true(e, "Math.hypot.length === 2 && Math.imul.length === 2"));
    env_free(e);
}

EMB_TEST(math_round_family_is_exact)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Math.round(2.5) === 3 && Math.round(-2.5) === -2 && Math.round(0.5) === 1"));
    EMB_CHECK(js_true(e, "Math.round(-0.5000000000000001) === -1 && Math.round(1.4999) === 1"));
    EMB_CHECK(js_true(e, "Math.round(0.49999999999999994) === 0"));
    EMB_CHECK(js_true(e, "1 / Math.round(-0.4) === -Infinity && 1 / Math.round(-0) === -Infinity"));
    EMB_CHECK(js_true(e, "1 / Math.round(0.4) === Infinity && 1 / Math.round(-0.5) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.round(4503599627370497) === 4503599627370497"));
    EMB_CHECK(js_true(e, "isNaN(Math.round(NaN)) && Math.round(Infinity) === Infinity"));
    EMB_CHECK(js_true(e, "Math.floor(-0.5) === -1 && Math.floor(1.9) === 1 && Math.floor(-0) === 0"));
    EMB_CHECK(js_true(e, "1 / Math.ceil(-0.5) === -Infinity && Math.ceil(0.2) === 1"));
    EMB_CHECK(js_true(e, "1 / Math.trunc(-0.9) === -Infinity && Math.trunc(-3.7) === -3"));
    EMB_CHECK(js_true(e, "Math.sign(-3) === -1 && Math.sign(4) === 1 && 1 / Math.sign(-0) === -Infinity"));
    EMB_CHECK(js_true(e, "isNaN(Math.sign(NaN)) && Math.sign(-Infinity) === -1"));
    EMB_CHECK(js_true(e, "1 / Math.abs(-0) === Infinity && Math.abs(-7.5) === 7.5"));
    EMB_CHECK(js_true(e, "Math.abs('-2') === 2 && isNaN(Math.abs(undefined))"));
    env_free(e);
}

EMB_TEST(math_min_max_hypot)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Math.max() === -Infinity && Math.min() === Infinity"));
    EMB_CHECK(js_true(e, "Math.max(1, 3, 2) === 3 && Math.min(1, 3, 2) === 1"));
    EMB_CHECK(js_true(e, "isNaN(Math.max(NaN, 1)) && isNaN(Math.min(1, 'x'))"));
    EMB_CHECK(js_true(e, "1 / Math.max(-0, 0) === Infinity && 1 / Math.max(0, -0) === Infinity"));
    EMB_CHECK(js_true(e, "1 / Math.min(0, -0) === -Infinity && 1 / Math.min(-0, 0) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.max('3', 2) === 3 && Math.min(null, 1) === 0"));
    EMB_CHECK(js_true(e, "Math.hypot() === 0 && Math.hypot(3, 4) === 5"));
    EMB_CHECK(js_true(e, "Math.hypot(Infinity, NaN) === Infinity && isNaN(Math.hypot(NaN, 1))"));
    EMB_CHECK(js_true(e, "1 / Math.hypot(-0) === Infinity && Math.abs(Math.hypot(5, 12) - 13) < 1e-12"));
    EMB_CHECK(js_true(e, "Math.hypot(-3, 0, 4) === 5"));
    env_free(e);
}

EMB_TEST(math_integer_helpers)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Math.clz32(1) === 31 && Math.clz32(0) === 32 && Math.clz32(-1) === 0"));
    EMB_CHECK(js_true(e, "Math.clz32(0x80000000) === 0 && Math.clz32(2 ** 32 + 1) === 31"));
    EMB_CHECK(js_true(e, "Math.imul(0xffffffff, 5) === -5 && Math.imul(3, 4) === 12"));
    EMB_CHECK(js_true(e, "Math.imul(0x7fffffff, 0x7fffffff) === 1"));
    EMB_CHECK(js_true(e, "Math.imul(2 ** 31, 2) === 0"));
    EMB_CHECK(js_true(e, "Math.fround(5.5) === 5.5 && Math.fround(5.05) === 5.050000190734863"));
    EMB_CHECK(js_true(e, "isNaN(Math.fround(NaN)) && Math.fround(2 ** 128) === Infinity"));
    env_free(e);
}

EMB_TEST(math_pow_special_cases)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Math.pow(2, 10) === 1024 && Math.pow(NaN, 0) === 1"));
    EMB_CHECK(js_true(e, "isNaN(Math.pow(1, Infinity)) && isNaN(Math.pow(-1, -Infinity))"));
    EMB_CHECK(js_true(e, "isNaN(Math.pow(-8, 1 / 3)) && isNaN(Math.pow(2, NaN))"));
    EMB_CHECK(js_true(e, "Math.pow(0, 0) === 1 && Math.pow(-0, 3) === -0"));
    EMB_CHECK(js_true(e, "1 / Math.pow(-0, 3) === -Infinity && Math.pow(-0, -3) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.pow(Infinity, -1) === 0 && Math.pow(0.5, -Infinity) === Infinity"));
    EMB_CHECK(js_true(e, "Math.pow(-2, 3) === -8"));
    env_free(e);
}

EMB_TEST(math_transcendental_points)
{
    env e = env_new();
    /* Correctly rounded (sqrt) and exact-at-zero results are compared exactly; the rest within
     * a tolerance, since libm and SDL may differ in the last bit. */
    EMB_CHECK(js_true(e, "Math.sqrt(2) === 1.4142135623730951 && isNaN(Math.sqrt(-1))"));
    EMB_CHECK(js_true(e,
        "function near(a, b) { return Math.abs(a - b) < 1e-12; }"
        "near(Math.atan2(1, 1) * 4, Math.PI) && Math.atan2(0, -1) === Math.PI"));
    EMB_CHECK(js_true(e,
        "function near2(a, b) { return Math.abs(a - b) < 1e-12; }"
        "near2(Math.log2(8), 3) && near2(Math.log10(1000), 3) && Math.log(1) === 0"));
    EMB_CHECK(js_true(e, "Math.cbrt(27) === 3 || Math.abs(Math.cbrt(27) - 3) < 1e-12"));
    EMB_CHECK(js_true(e, "Math.cbrt(-8) === -2 || Math.abs(Math.cbrt(-8) + 2) < 1e-12"));
    EMB_CHECK(js_true(e, "Math.exp(0) === 1 && Math.expm1(0) === 0 && Math.log1p(0) === 0"));
    EMB_CHECK(js_true(e, "Math.sin(0) === 0 && Math.cos(0) === 1 && Math.tan(0) === 0"));
    EMB_CHECK(js_true(e,
        "Math.abs(Math.asin(1) - Math.PI / 2) < 1e-12 && Math.acos(1) === 0 && Math.atan(0) === 0"));
    EMB_CHECK(js_true(e, "Math.sinh(0) === 0 && Math.cosh(0) === 1 && Math.tanh(0) === 0"));
    EMB_CHECK(js_true(e, "Math.asinh(0) === 0 && Math.acosh(1) === 0 && Math.atanh(0) === 0"));
    EMB_CHECK(js_true(e, "isNaN(Math.acosh(0.5)) && isNaN(Math.log(-1)) && Math.log(0) === -Infinity"));
    EMB_CHECK(js_true(e,
        "Math.tanh(Infinity) === 1 && Math.abs(Math.atan(Infinity) - Math.PI / 2) < 1e-12"));
    env_free(e);
}

/* 1000 draws: all in [0, 1), not all equal, and a mean near 0.5 (the standard error of the mean
 * is about 0.009, so the bounds are far outside normal variation). */
EMB_TEST(math_random_range)
{
    env e = env_new();
    EMB_CHECK(js_true(e,
        "var ok = true, first = Math.random(), diff = false, sum = 0;"
        "for (var i = 0; i < 1000; i++) {"
        "  var x = Math.random(); ok = ok && x >= 0 && x < 1; diff = diff || x !== first; sum += x;"
        "}"
        "ok && diff && sum > 400 && sum < 600"));
    env_free(e);
}

EMB_TEST(math_coercion_order)
{
    env e = env_new();
    /* Every argument of max and hypot is converted, even after a NaN. */
    EMB_CHECK(js_true(e,
        "var n = 0; var o = { valueOf: function() { n++; return 1; } };"
        "Math.max(NaN, o, o) !== Math.max(NaN, o, o); n === 4"));
    EMB_CHECK(js_true(e,
        "var m = 0; var p = { valueOf: function() { m++; return 2; } };"
        "Math.hypot(Infinity, p, p) === Infinity && m === 2"));
    env_free(e);
}

EMB_TEST(math_f16round_rounding)
{
    env e = env_new();
    EMB_CHECK(js_true(e, "Math.f16round(1.337) === 1.3369140625"));
    EMB_CHECK(js_true(e, "Math.f16round(-1.337) === -1.3369140625"));
    EMB_CHECK(js_true(e, "Math.f16round(65504) === 65504 && Math.f16round(65519) === 65504"));
    EMB_CHECK(js_true(e, "Math.f16round(65520) === Infinity && Math.f16round(-1e10) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.f16round(Math.pow(2, -24)) === Math.pow(2, -24)"));
    EMB_CHECK(js_true(e, "Math.f16round(Math.pow(2, -25)) === 0"));
    EMB_CHECK(js_true(e, "Math.f16round(3 * Math.pow(2, -25)) === Math.pow(2, -23)"));
    EMB_CHECK(js_true(e, "1 / Math.f16round(-Math.pow(2, -26)) === -Infinity"));
    EMB_CHECK(js_true(e, "Math.f16round(2049) === 2048 && Math.f16round(2051) === 2052"));
    EMB_CHECK(js_true(e, "isNaN(Math.f16round(NaN)) && Math.f16round(0.5) === 0.5"));
    env_free(e);
}

EMB_TEST_MAIN()
