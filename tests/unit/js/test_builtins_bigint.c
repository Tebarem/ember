/* BigInt through the engine: literals, operators, comparisons, conversions, the BigInt builtins,
 * JSON and property keys. Each script returns the labels of the checks
 * that failed, so an empty string means every check passed. */
#include "es6_util.h"

#include "js/bigint.h"

#include <string.h>

static const char *bigint_script =
    "var fails = [];\n"
    "function c(n, v) { if (!v) fails.push(n); }\n"
    "function throws(f, E) { try { f(); return false; } catch (e) { return e instanceof E; } }\n"
    "c('typeof', typeof 1n === 'bigint' && typeof Object(1n) === 'object');\n"
    "c('literal_forms', 0x1Fn === 31n && 0o17n === 15n && 0b101n === 5n && 1_000n === 1000n);\n"
    "c('negative_literal', -0x10n === -16n && -0n === 0n);\n"
    "c('add', 1n + 2n === 3n && 5n - 7n === -2n);\n"
    "c('mul', 123456789n * 987654321n === 121932631112635269n);\n"
    "c('div_mod', -7n / 2n === -3n && -7n % 2n === -1n && 7n % -2n === 1n);\n"
    "c('exp', 2n ** 64n === 18446744073709551616n && (-2n) ** 3n === -8n && 0n ** 0n === 1n);\n"
    "c('exp_neg_throws', throws(function () { return 2n ** -1n; }, RangeError));\n"
    "c('div_zero_throws', throws(function () { return 1n / 0n; }, RangeError));\n"
    "c('shifts', 1n << 70n === 1180591620717411303424n && -5n >> 1n === -3n && 5n >> -1n === 10n);\n"
    "c('ushr_throws', throws(function () { return 1n >>> 0n; }, TypeError));\n"
    "c('bitwise', (-6n & 3n) === 2n && (-6n | 3n) === -5n && (-6n ^ 3n) === -7n);\n"
    "c('bnot_neg', ~5n === -6n && -(5n) === -5n);\n"
    "c('mix_throws', throws(function () { return 1n + 1; }, TypeError) &&\n"
    "    throws(function () { return 2n * 1.5; }, TypeError) &&\n"
    "    throws(function () { return +1n; }, TypeError) &&\n"
    "    throws(function () { return Math.abs(1n); }, TypeError) &&\n"
    "    throws(function () { return Object(1n) + 1; }, TypeError));\n"
    "c('concat_template', 'x' + 1n === 'x1' && `${12n}` === '12' && String(3n) === '3');\n"
    "c('loose_eq', 1n == 1 && !(1n === 1) && 1n == '1' && 2n != '3' && 10n == ' 10 ');\n"
    "c('loose_bool', 1n == true && 0n == false && !(2n == true));\n"
    "c('loose_nan_undefined', !(1n == NaN) && !(1n < NaN) && !(1n > NaN) && !(1n >= NaN));\n"
    "c('cmp_number', 2n > 1 && 1n < 1.5 && 2n >= 2 && !(3n <= 2.5) && -1n < -0.5);\n"
    "c('cmp_infinity', 1n < Infinity && !(1n > Infinity) && -1n > -Infinity);\n"
    "c('cmp_string', 10n > '9' && !(10n < 'abc') && '1' < 2n && !('x' > 1n));\n"
    "c('cmp_huge', 2n ** 100n > 2 ** 99 && 2n ** 100n < 2 ** 101);\n"
    "c('same_value', Object.is(1n, 1n) && Object.is(0n, -0n) && !Object.is(1n, 2n));\n"
    "c('inc_dec', (function () { var x = 1n; x++; var y = 5n; var z = y--;\n"
    "    var w = -1n; return x === 2n && z === 5n && y === 4n && ++w === 0n; })());\n"
    "c('compound', (function () { var x = 7n; x %= 4n; x <<= 2n; x |= 1n;\n"
    "    var y = 3n; y **= 3n; return x === 13n && y === 27n; })());\n"
    "c('compound_mix_throws', throws(function () { var x = 1n; x += 1; }, TypeError));\n"
    "c('truthiness', !0n && !!1n && !!-1n && !''.length);\n"
    "c('switch_strict', (function () { switch (2n) { case 2: return false; case 2n: return true; }\n"
    "    return false; })());\n"
    "c('to_string', (255n).toString(16) === 'ff' && (10n).toString() === '10' &&\n"
    "    (-8n).toString(2) === '-1000' && (10n).toString(36) === 'a');\n"
    "c('to_string_radix_range', throws(function () { return (1n).toString(1); }, RangeError) &&\n"
    "    throws(function () { return (1n).toString(37); }, RangeError));\n"
    "c('this_checks', throws(function () { return BigInt.prototype.toString.call(1); }, TypeError) &&\n"
    "    throws(function () { return BigInt.prototype.valueOf.call('1'); }, TypeError));\n"
    "c('value_of_wrapper', Object(7n).valueOf() === 7n && -Object(2n) === -2n);\n"
    "c('wrapper_proto', Object(1n) instanceof BigInt &&\n"
    "    Object.prototype.toString.call(1n) === '[object BigInt]' &&\n"
    "    BigInt.prototype.toLocaleString.call(5n) === '5');\n"
    "c('call_conversions', BigInt(42) === 42n && BigInt('0x10') === 16n && BigInt(' 12 ') === 12n &&\n"
    "    BigInt(true) === 1n && BigInt(false) === 0n && BigInt('') === 0n &&\n"
    "    BigInt(2 ** 60) === 1152921504606846976n);\n"
    "c('call_throws', throws(function () { return BigInt(1.5); }, RangeError) &&\n"
    "    throws(function () { return BigInt(NaN); }, RangeError) &&\n"
    "    throws(function () { return BigInt('1.5'); }, SyntaxError) &&\n"
    "    throws(function () { return BigInt(undefined); }, TypeError) &&\n"
    "    throws(function () { return BigInt(null); }, TypeError) &&\n"
    "    throws(function () { return BigInt(Symbol()); }, TypeError) &&\n"
    "    throws(function () { return new BigInt(1); }, TypeError));\n"
    "c('as_int_n', BigInt.asIntN(8, 255n) === -1n && BigInt.asIntN(8, 127n) === 127n &&\n"
    "    BigInt.asIntN(64, 2n ** 63n) === -(2n ** 63n) && BigInt.asIntN(3, 4n) === -4n);\n"
    "c('as_n_huge', throws(function () { BigInt.asUintN(2 ** 40, -1n); }, RangeError) &&\n"
    "    throws(function () { BigInt.asUintN(2 ** 53 - 1, -1n); }, RangeError) &&\n"
    "    BigInt.asUintN(2 ** 53 - 1, 5n) === 5n && BigInt.asIntN(2 ** 53 - 1, -5n) === -5n);\n"
    "c('as_uint_n', BigInt.asUintN(8, -1n) === 255n && BigInt.asUintN(0, 5n) === 0n &&\n"
    "    BigInt.asUintN(64, -1n) === 18446744073709551615n);\n"
    "c('as_n_throws', throws(function () { return BigInt.asIntN(-1, 0n); }, RangeError) &&\n"
    "    throws(function () { return BigInt.asIntN(2, 1); }, TypeError));\n"
    "c('shape', BigInt.length === 1 && BigInt.name === 'BigInt' && BigInt.asIntN.length === 2 &&\n"
    "    BigInt.prototype.toString.length === 0);\n"
    "c('json_throws', throws(function () { return JSON.stringify({ a: 1n }); }, TypeError));\n"
    "c('json_to_json', (function () { BigInt.prototype.toJSON = function () { return 'big'; };\n"
    "    var r = JSON.stringify([5n]); delete BigInt.prototype.toJSON; return r === '[\"big\"]'; })());\n"
    "c('property_keys', ({ 1n: 'a' })['1'] === 'a' && (1n in { '1': 0 }) &&\n"
    "    [5, 6, 7][1n] === 6 && (function () { var o = {}; o[10n] = 3; return o['10'] === 3; })());\n"
    "c('array_join', [1n, 2n].join('-') === '1-2');\n"
    "c('large_value', (2n ** 4000n).toString().length === 1205);\n"
    "c('number_of_bigint', Number(2n ** 53n + 1n) === 9007199254740992 &&\n"
    "    Number(2n ** 53n + 3n) === 9007199254740996 && Number(2n ** 1024n) === Infinity &&\n"
    "    Number(-(2n ** 1024n)) === -Infinity && typeof Number(1n) === 'number' &&\n"
    "    Object.is(Number(-0n), 0) && Number(-7n) === -7 && Number(10n ** 30n) === 1e30);\n"
    "c('number_wrapper_of_bigint', typeof new Number(1n).valueOf() === 'number' &&\n"
    "    new Number(5n) + 1 === 6 && typeof new Number(5n) === 'object');\n"
    "c('map_set_keys_by_value', new Map([[1n, 'a']]).get(1n) === 'a' &&\n"
    "    new Set([1n, 1n]).size === 1 && new Set([1n, 1]).size === 2 &&\n"
    "    new Map([[2n ** 70n, 'x']]).has(2n ** 70n) && new Set([2n ** 70n, 2n ** 70n]).size === 1 &&\n"
    "    new Set([0n]).has(-0n) &&\n"
    "    (function () { var m = new Map(); m.set(5n, 1); m.set(5n, 2); m.delete(5n);\n"
    "        return m.size === 0 && !m.has(5n) && m.get(5n) === undefined; })());\n"
    "fails.join(',');\n";

EMB_TEST(bigint_js_checks_pass)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    embi_js_value v = es6_run(e, bigint_script);
    EMB_REQUIRE(!embi_js_is_exception(v));
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(embi_js_to_string(e.rt, v), &sb);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&sb), "");
    embi_sb_free(&sb);
    es6_env_free(e);
}

EMB_TEST(bigint_js_error_messages_and_realm_state)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.rt != nullptr && e.r != nullptr);
    /* Mixing reports the spec message; an invalid string to BigInt() is a SyntaxError. */
    embi_js_value v = es6_run(e, "try { 1n + 1 } catch (x) { x.message }");
    EMB_CHECK(es6_text_is(e.rt, v, "Cannot mix BigInt and other types, use explicit conversions"));
    v = es6_run(e, "try { BigInt('zz') } catch (x) { x.name }");
    EMB_CHECK(es6_text_is(e.rt, v, "SyntaxError"));
    v = es6_run(e, "typeof BigInt.prototype.valueOf.call(Object(9n))");
    EMB_CHECK(es6_text_is(e.rt, v, "bigint"));
    /* A BigInt literal in a parenthesized expression and a call argument keeps its value. */
    v = es6_run(e, "(function (x) { return x * 2n; })((21n))");
    EMB_CHECK(es6_text_is(e.rt, v, "42"));
    es6_env_free(e);
}

EMB_TEST_MAIN()
