/* Numeric fast paths of the interpreter (J5b §6): int32 and double inline handlers for ADD, SUB,
 * MUL, DIV, MOD, the bitwise and shift ops, the relational and equality ops, the unary ops, INC/DEC
 * and truthiness jumps. Every result must equal the generic path: the cases cover int32 overflow
 * into doubles, -0, NaN, exact and inexact division, mixed int and double operands and the
 * slow paths (strings, BigInt, objects with valueOf) that the fast paths hand off. */
#include "es6_util.h"

#include <math.h>

/* Results that are -0 are checked through 1/x, since 0 === -0 in both engines. */
#define NEG_ZERO(e, expr) E6_NUM((e), "1 / (" expr ")", -INFINITY)

EMB_TEST(js_numeric_add_sub)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "2147483647 + 1", 2147483648.0);
    E6_NUM(e, "-2147483648 + -1", -2147483649.0);
    E6_NUM(e, "3 + 4", 7);
    E6_NUM(e, "1.5 + 2", 3.5);
    E6_NUM(e, "2147483647 + 2147483647", 4294967294.0);
    E6_NUM(e, "-2147483648 - 1", -2147483649.0);
    E6_NUM(e, "0 - 2147483648", -2147483648.0);
    E6_NUM(e, "0 - 0", 0);
    E6_NUM(e, "5.25 - 0.25", 5);
    E6_TEXT(e, "1 + '2'", "12");
    E6_NUM(e, "'3' - '4'", -1);
    E6_NUM(e, "null + 1", 1);
    E6_BOOL(e, "(undefined + 1) !== (undefined + 1)", true);
    E6_TEXT(e, "String(1n + 2n)", "3");
    E6_BOOL(e, "1n + 1n === 2n", true);
    es6_env_free(e);
}

EMB_TEST(js_numeric_mul)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "3 * 4", 12);
    E6_NUM(e, "-3 * -4", 12);
    E6_NUM(e, "2.5 * 2", 5);
    E6_NUM(e, "65536 * 65536", 4294967296.0);
    E6_NUM(e, "-46341 * 46341", -2147488281.0);
    E6_NUM(e, "1073741824 * 4", 4294967296.0);
    NEG_ZERO(e, "0 * -5");
    NEG_ZERO(e, "-5 * 0");
    E6_BOOL(e, "1 / (0 * 5) === Infinity", true);
    E6_NUM(e, "'3' * '4'", 12);
    es6_env_free(e);
}

/* MUL overflow in a loop: the product leaves the int32 range and stays exact in a double. */
EMB_TEST(js_numeric_mul_loop)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var t = 1; for (var q = 0; q < 40; q++) t = t * 2; t", 1099511627776.0);
    E6_NUM(e, "var s = 0; for (var i = 0; i < 100000; i++) s += i; s", 4999950000.0);
    es6_env_free(e);
}

EMB_TEST(js_numeric_div)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "6 / 3", 2);
    E6_NUM(e, "7 / 2", 3.5);
    E6_NUM(e, "-2147483648 / -1", 2147483648.0);
    E6_NUM(e, "-2147483648 / 2", -1073741824);
    NEG_ZERO(e, "0 / -3");
    E6_BOOL(e, "1 / 0 === Infinity", true);
    E6_BOOL(e, "-1 / 0 === -Infinity", true);
    E6_BOOL(e, "(0 / 0) !== (0 / 0)", true);
    E6_NUM(e, "1.5 / 0.5", 3);
    E6_NUM(e, "'8' / '2'", 4);
    es6_env_free(e);
}

EMB_TEST(js_numeric_mod)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "7 % 3", 1);
    E6_NUM(e, "-7 % 3", -1);
    E6_NUM(e, "7 % -3", 1);
    E6_NUM(e, "2147483647 % 2", 1);
    E6_NUM(e, "5.5 % 2", 1.5);
    NEG_ZERO(e, "-6 % 3");
    NEG_ZERO(e, "-2147483648 % -1");
    E6_BOOL(e, "(5 % 0) !== (5 % 0)", true);
    E6_NUM(e, "'7' % '4'", 3);
    es6_env_free(e);
}

EMB_TEST(js_numeric_bitwise)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "5 & 3", 1);
    E6_NUM(e, "5 | 3", 7);
    E6_NUM(e, "5 ^ 3", 6);
    E6_NUM(e, "-1 & 255", 255);
    E6_NUM(e, "1 << 31", -2147483648.0);
    E6_NUM(e, "1 << 32", 1);
    E6_NUM(e, "-1 >> 1", -1);
    E6_NUM(e, "-8 >> 1", -4);
    E6_NUM(e, "-1 >>> 0", 4294967295.0);
    E6_NUM(e, "-1 >>> 31", 1);
    E6_NUM(e, "-8 >>> 1", 2147483644.0);
    E6_NUM(e, "2147483647 | 0", 2147483647.0);
    E6_NUM(e, "2147483648 | 0", -2147483648.0);
    E6_NUM(e, "1.9 | 0", 1);
    E6_NUM(e, "-1.9 | 0", -1);
    E6_NUM(e, "3 << '2'", 12);
    E6_NUM(e, "~5", -6);
    E6_NUM(e, "~2147483647", -2147483648.0);
    E6_NUM(e, "~1.5", -2);
    E6_TEXT(e, "String(1n << 3n)", "8");
    E6_TEXT(e, "String(5n & 3n)", "1");
    es6_env_free(e);
}

EMB_TEST(js_numeric_compare)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "1 < 2", true);
    E6_BOOL(e, "2 < 2", false);
    E6_BOOL(e, "2 <= 2", true);
    E6_BOOL(e, "3 > 2", true);
    E6_BOOL(e, "-1 >= -1", true);
    E6_BOOL(e, "1.5 > 1", true);
    E6_BOOL(e, "2147483647 < 2147483648", true);
    E6_BOOL(e, "(0 / 0) < 1", false);
    E6_BOOL(e, "(0 / 0) >= 0", false);
    E6_BOOL(e, "(0 / 0) <= 0", false);
    E6_BOOL(e, "(0 / 0) > 0", false);
    E6_BOOL(e, "'10' < '9'", true);
    E6_BOOL(e, "'10' < 9", false);
    E6_BOOL(e, "1n < 2", true);
    E6_BOOL(e, "2 <= 1n", false);
    E6_BOOL(e, "0 == -0", true);
    es6_env_free(e);
}

EMB_TEST(js_numeric_equality)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "1 == 1.0", true);
    E6_BOOL(e, "1 != 1.5", true);
    E6_BOOL(e, "(0 / 0) == (0 / 0)", false);
    E6_BOOL(e, "(0 / 0) != (0 / 0)", true);
    E6_BOOL(e, "'1' == 1", true);
    E6_BOOL(e, "1 == '1.0'", true);
    E6_BOOL(e, "null == undefined", true);
    E6_BOOL(e, "0 == false", true);
    E6_BOOL(e, "1n == 1", true);
    E6_BOOL(e, "1 === 1.0", true);
    E6_BOOL(e, "1 === '1'", false);
    E6_BOOL(e, "1 !== 2", true);
    E6_BOOL(e, "(0 / 0) === (0 / 0)", false);
    E6_BOOL(e, "0 === -0", true);
    E6_BOOL(e, "2147483648 === 2147483648", true);
    E6_BOOL(e, "2147483648 === 4294967296 / 2", true);
    E6_BOOL(e, "'a' === 'a'", true);
    E6_BOOL(e, "'a' !== 'b'", true);
    E6_BOOL(e, "null === null", true);
    E6_BOOL(e, "({}) === ({})", false);
    es6_env_free(e);
}

EMB_TEST(js_numeric_unary)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "-(-2147483648)", 2147483648.0);
    E6_NUM(e, "-(0.5)", -0.5);
    E6_NUM(e, "-(7)", -7);
    E6_NUM(e, "-'3'", -3);
    NEG_ZERO(e, "-(0)");
    E6_NUM(e, "+'12'", 12);
    E6_BOOL(e, "!0", true);
    E6_BOOL(e, "!!''", false);
    E6_NUM(e, "var z = -0; z--; z", -1);
    E6_NUM(e, "var i = 2147483647; i++; i", 2147483648.0);
    E6_NUM(e, "var j = -2147483648; j--; j", -2147483649.0);
    E6_NUM(e, "var k = 1.5; ++k", 2.5);
    E6_NUM(e, "var m = '3'; m++; m", 4);
    E6_TEXT(e, "var bi = 5n; bi++; String(bi)", "6");
    E6_NUM(e, "var c = 0; c++; c += 1; c", 2);
    es6_env_free(e);
}

/* Truthiness in jumps: booleans, int32s, doubles, NaN, strings, nullish, objects. */
EMB_TEST(js_numeric_jump_truthiness)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e, "var r = 0; if (1) r += 1; if (0) r += 10; if (-1) r += 100; if (0.5) r += 1000; "
              "if (0 / 0) r += 10000; r",
           1101);
    E6_NUM(e, "var t = 0; if (true) t += 1; if (false) t += 2; while (t < 5) t += 1; t", 5);
    E6_TEXT(e, "true && 'x'", "x");
    E6_TEXT(e, "0 || 'y'", "y");
    E6_NUM(e, "1 ? 2 : 3", 2);
    E6_NUM(e, "0 ? 2 : 3", 3);
    E6_NUM(e, "'' || 4", 4);
    E6_NUM(e, "null ?? 5", 5);
    E6_NUM(e, "(0 || null) ?? 6", 6);
    E6_NUM(e, "var g = 0; for (var h = 3; h; h--) g += h; g", 6);
    E6_NUM(e, "var w = 0; do { w++ } while (w < 0 || w < 3 && 0); w", 1);
    E6_NUM(e, "var u = 0; if ({}) u = 7; u", 7);
    E6_NUM(e, "var v = 0; if (1n) v = 8; v", 8);
    E6_NUM(e, "var q = 0; if (0n) q = 9; q", 0);
    es6_env_free(e);
}

/* Slow paths run user code (valueOf, toString) once, in order, and the results match. */
EMB_TEST(js_numeric_slow_paths)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_BOOL(e, "var cnt = 0; var o = { valueOf() { cnt++; return 5 } }; "
               "(o < 6) && (o + 1 === 6) && (-o === -5) && (o * 2 === 10) && cnt === 4",
            true);
    E6_BOOL(e, "var c2 = 0; var o2 = { valueOf() { c2++; return 1 } }; "
               "o2 == 1 && (o2 | 0) === 1 && (o2 >>> 0) === 1 && c2 === 3",
            true);
    E6_NUM(e, "var n3 = 0; var o3 = { valueOf() { n3++; return 2 } }; o3++; n3 * 10 + o3", 13);
    E6_THROWS(e, "var o4 = { valueOf() { throw new RangeError('x') } }; 1 + o4", "RangeError");
    E6_THROWS(e, "var o5 = { valueOf() { throw new RangeError('y') } }; o5 < 1", "RangeError");
    E6_THROWS(e, "var o6 = { valueOf() { throw new RangeError('z') } }; ~o6", "RangeError");
    E6_THROWS(e, "1n + 1", "TypeError");
    E6_THROWS(e, "var s1 = Symbol(); s1 * 2", "TypeError");
    es6_env_free(e);
}

/* A loop whose int fast paths keep the frame consistent across a call and a return. */
EMB_TEST(js_numeric_fast_loop_calls)
{
    es6_env e = es6_env_new();
    EMB_REQUIRE(e.r != nullptr);
    E6_NUM(e,
           "function sq(x) { return x * x } var acc = 0; "
           "for (var i = 0; i < 1000; i++) { acc = (acc + sq(i % 7)) | 0 } acc",
           12977);
    E6_NUM(e,
           "var arr = []; for (var k = 0; k < 50; k++) arr.push(k * k - k); "
           "var tot = 0; for (var z = 0; z < arr.length; z++) tot += arr[z]; tot",
           40425 - 1225);
    es6_env_free(e);
}

EMB_TEST_MAIN()
