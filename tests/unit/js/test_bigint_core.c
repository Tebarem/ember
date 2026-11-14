/* BigInt arithmetic through the C API (src/js/bigint.c): parsing, printing, operators, conversions,
 * and the multiplication and division paths above the Karatsuba threshold. */
#include "emb_test.h"

#include "js/bigint.h"
#include "js/js.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* A BigInt from a digit string in the given radix, with an optional leading '-'. */
static embi_js_value bi_parse(embi_js_runtime *rt, const char *text, unsigned radix)
{
    bool neg = text[0] == '-';
    const char *d = neg ? text + 1 : text;
    return embi_js_from_bigint(embi_js_bigint_from_digits(rt, d, strlen(d), radix, neg));
}

static embi_js_value bi_i64(embi_js_runtime *rt, int64_t v)
{
    return embi_js_from_bigint(embi_js_bigint_from_i64(rt, v));
}

static embi_js_value bi_op(embi_js_runtime *rt, enum embi_js_bigop op, embi_js_value a,
                           embi_js_value b)
{
    embi_js_value r = embi_js_bigint_binop(rt, op, a, b);
    if (embi_js_is_exception(r)) {
        embi_js_exception_clear(rt);
    }
    return r;
}

/* True when the value's text in the radix equals want. */
static bool bi_is(embi_js_runtime *rt, embi_js_value v, unsigned radix, const char *want)
{
    embi_js_string *s = embi_js_bigint_to_string(rt, embi_js_as_bigint(v), radix);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    bool ok = strcmp(embi_sb_cstr(&sb), want) == 0;
    embi_sb_free(&sb);
    return ok;
}

static bool bi_cmp_eq(embi_js_value a, embi_js_value b)
{
    return embi_js_bigint_compare(embi_js_as_bigint(a), embi_js_as_bigint(b)) == 0;
}

static embi_js_value bi_pow(embi_js_runtime *rt, embi_js_value base, uint32_t e)
{
    return bi_op(rt, EMBI_JS_BIG_EXP, base, bi_i64(rt, e));
}

/* A deterministic decimal string of n digits, first digit nonzero. */
static void lcg_digits(char *out, size_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        out[i] = (char)('0' + (i == 0 ? 1 + (x >> 24) % 9 : (x >> 24) % 10));
    }
    out[n] = '\0';
}

EMB_TEST(bigint_print_small_and_extremes)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(bi_is(rt, bi_i64(rt, 0), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, -1), 10, "-1"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, 1234567890123LL), 10, "1234567890123"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, INT64_MIN), 10, "-9223372036854775808"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, INT64_MAX), 10, "9223372036854775807"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, 255), 16, "ff"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, 255), 2, "11111111"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, 255), 36, "73"));
    EMB_CHECK(bi_is(rt, bi_i64(rt, -255), 8, "-377"));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_parse_radixes_and_round_trip)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_value v = bi_parse(rt, "ffffffffffffffffffffffffffffffff", 16);
    EMB_CHECK(bi_is(rt, v, 10, "340282366920938463463374607431768211455"));
    EMB_CHECK(bi_is(rt, bi_parse(rt, "-1010", 2), 10, "-10"));
    EMB_CHECK(bi_is(rt, bi_parse(rt, "0", 10), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_parse(rt, "000000000000000000000000000000000000042", 10), 10, "42"));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_shift_and_power_values)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_value one = bi_i64(rt, 1);
    embi_js_value p100 = bi_op(rt, EMBI_JS_BIG_SHL, one, bi_i64(rt, 100));
    EMB_CHECK(bi_is(rt, p100, 10, "1267650600228229401496703205376"));
    EMB_CHECK(bi_is(rt, bi_pow(rt, bi_i64(rt, 2), 64), 10, "18446744073709551616"));
    EMB_CHECK(bi_is(rt, bi_pow(rt, bi_i64(rt, -2), 3), 10, "-8"));
    EMB_CHECK(bi_is(rt, bi_pow(rt, bi_i64(rt, 0), 0), 10, "1"));
    EMB_CHECK(bi_is(rt, bi_pow(rt, bi_i64(rt, -1), 7), 10, "-1"));
    EMB_CHECK(bi_is(rt, bi_pow(rt, bi_i64(rt, 7), 1), 10, "7"));
    /* Right shifts round toward negative infinity. */
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SAR, bi_i64(rt, -5), bi_i64(rt, 1)), 10, "-3"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SAR, bi_i64(rt, -1), bi_i64(rt, 100)), 10, "-1"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SAR, bi_i64(rt, 5), bi_i64(rt, 100)), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SAR, bi_i64(rt, 5), bi_i64(rt, -1)), 10, "10"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SHL, bi_i64(rt, -3), bi_i64(rt, 2)), 10, "-12"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 0), bi_i64(rt, 1000)), 10, "0"));
    /* A negative exponent and a shift past the size limit throw. */
    embi_js_value bad = embi_js_bigint_binop(rt, EMBI_JS_BIG_EXP, bi_i64(rt, 2), bi_i64(rt, -1));
    EMB_CHECK(embi_js_is_exception(bad));
    embi_js_exception_clear(rt);
    bad = embi_js_bigint_binop(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 1), bi_i64(rt, 1LL << 40));
    EMB_CHECK(embi_js_is_exception(bad));
    embi_js_exception_clear(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_division_truncates_toward_zero)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_DIV, bi_i64(rt, -7), bi_i64(rt, 2)), 10, "-3"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_MOD, bi_i64(rt, -7), bi_i64(rt, 2)), 10, "-1"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_MOD, bi_i64(rt, 7), bi_i64(rt, -2)), 10, "1"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_DIV, bi_i64(rt, 7), bi_i64(rt, -2)), 10, "-3"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_DIV, bi_i64(rt, 3), bi_i64(rt, 5)), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_MOD, bi_i64(rt, 3), bi_i64(rt, 5)), 10, "3"));
    embi_js_value zero = bi_i64(rt, 0);
    embi_js_value dz = embi_js_bigint_binop(rt, EMBI_JS_BIG_DIV, bi_i64(rt, 1), zero);
    EMB_CHECK(embi_js_is_exception(dz));
    embi_js_exception_clear(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_bitwise_two_complement)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_AND, bi_i64(rt, -6), bi_i64(rt, 3)), 10, "2"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_OR, bi_i64(rt, -6), bi_i64(rt, 3)), 10, "-5"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_XOR, bi_i64(rt, -6), bi_i64(rt, 3)), 10, "-7"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_AND, bi_i64(rt, -1), bi_i64(rt, 0)), 10, "0"));
    embi_js_value big = bi_op(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 1), bi_i64(rt, 80));
    embi_js_value neg_big = embi_js_bigint_neg(rt, big);
    /* -2^80 & (2^80 - 1) == 0; -2^80 | 1 == -(2^80 - 1). */
    embi_js_value mask = bi_op(rt, EMBI_JS_BIG_SUB, big, bi_i64(rt, 1));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_AND, neg_big, mask), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_OR, neg_big, bi_i64(rt, 1)), 10,
                    "-1208925819614629174706175"));
    /* ~x == -x - 1 */
    EMB_CHECK(bi_is(rt, embi_js_bigint_not(rt, bi_i64(rt, 5)), 10, "-6"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_not(rt, bi_i64(rt, -1)), 10, "0"));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_as_int_n_and_as_uint_n)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 255), 8, true), 10, "-1"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 255), 8, false), 10, "255"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, -1), 8, false), 10, "255"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 127), 8, true), 10, "127"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 128), 8, true), 10, "-128"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 5), 0, false), 10, "0"));
    embi_js_value two63 = bi_op(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 1), bi_i64(rt, 63));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, two63, 64, true), 10, "-9223372036854775808"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, -5), 1000, true), 10, "-5"));
    embi_js_value huge = embi_js_bigint_as_n(rt, bi_i64(rt, -1), (uint64_t)1 << 40, false);
    EMB_CHECK(embi_js_is_exception(huge));
    embi_js_exception_clear(rt);
    huge = embi_js_bigint_as_n(rt, bi_i64(rt, -1), ((uint64_t)1 << 53) - 1, false);
    EMB_CHECK(embi_js_is_exception(huge));
    embi_js_exception_clear(rt);
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, 5), ((uint64_t)1 << 53) - 1, false), 10, "5"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_as_n(rt, bi_i64(rt, -5), ((uint64_t)1 << 53) - 1, true), 10, "-5"));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_karatsuba_and_long_division_identities)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    /* About 3000 digits each: the products go through the Karatsuba split. */
    char da[3001];
    char db[2501];
    lcg_digits(da, 3000, 12345u);
    lcg_digits(db, 2500, 987654u);
    embi_js_value a = bi_parse(rt, da, 10);
    embi_js_value b = bi_parse(rt, db, 10);
    embi_js_value p = bi_op(rt, EMBI_JS_BIG_MUL, a, b);
    EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_DIV, p, a), b));
    EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_MOD, p, a), bi_i64(rt, 0)));
    EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_DIV, p, b), a));
    /* (a * b) + 1 has remainder 1 modulo b. */
    embi_js_value p1 = bi_op(rt, EMBI_JS_BIG_ADD, p, bi_i64(rt, 1));
    EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_MOD, p1, b), bi_i64(rt, 1)));
    /* Commutativity across the balanced and unbalanced paths. */
    embi_js_value tiny = bi_i64(rt, 1234567);
    EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_MUL, a, tiny), bi_op(rt, EMBI_JS_BIG_MUL, tiny, a)));
    /* a - a is zero and a + (-a) is zero. */
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_SUB, a, a), 10, "0"));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_ADD, a, embi_js_bigint_neg(rt, a)), 10, "0"));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_large_print_round_trips)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    /* 3^20000 has floor(20000 * log10(3)) + 1 = 9543 decimal digits. */
    embi_js_value v = bi_pow(rt, bi_i64(rt, 3), 20000);
    embi_js_string *s = embi_js_bigint_to_string(rt, embi_js_as_bigint(v), 10);
    EMB_CHECK_EQ_INT((int)embi_js_str_len(s), 9543);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    embi_js_value back = bi_parse(rt, embi_sb_cstr(&sb), 10);
    EMB_CHECK(bi_cmp_eq(back, v));
    embi_sb_free(&sb);
    /* The same value in hex round-trips too. */
    embi_js_string *hex = embi_js_bigint_to_string(rt, embi_js_as_bigint(v), 16);
    embi_sb hb = {nullptr, 0, 0};
    embi_js_str_append_utf8(hex, &hb);
    EMB_CHECK(bi_cmp_eq(bi_parse(rt, embi_sb_cstr(&hb), 16), v));
    embi_sb_free(&hb);
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_comparisons_with_numbers)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_value two53 = bi_op(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 1), bi_i64(rt, 53));
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(two53), 9007199254740992.0) == 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, 2)), 2.5) < 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, 2)), 1.5) > 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, -2)), -2.5) > 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, -2)), -1.5) < 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, 0)), -0.0) == 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, 0)), NAN) == 2);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(bi_i64(rt, 5)), INFINITY) < 0);
    embi_js_value huge = bi_pow(rt, bi_i64(rt, 10), 400);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(huge), 1e300) > 0);
    EMB_CHECK(embi_js_bigint_compare_double(embi_js_as_bigint(huge), 1e308) > 0);
    /* Number(bigint): the nearest double, and infinity past the range. */
    EMB_CHECK(embi_js_bigint_to_double(embi_js_as_bigint(bi_pow(rt, bi_i64(rt, 10), 30))) == 1e30);
    EMB_CHECK(embi_js_bigint_to_double(embi_js_as_bigint(bi_i64(rt, -12))) == -12.0);
    /* 10^400 is above the largest double: infinity. */
    EMB_CHECK(isinf(embi_js_bigint_to_double(embi_js_as_bigint(huge))));
    EMB_CHECK(embi_js_bigint_to_double(embi_js_as_bigint(bi_pow(rt, bi_i64(rt, 2), 1023))) ==
              ldexp(1.0, 1023));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_string_conversion_rules)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_bigint *out = nullptr;
    embi_js_string *s = nullptr;
    struct {
        const char *text;
        bool ok;
        const char *want;
    } cases[] = {
        {"  0x1F  ", true, "31"}, {"-0b101", false, nullptr}, {"+12", true, "12"},
        {"", true, "0"},         {"   ", true, "0"},      {"0o17", true, "15"},
        {"1e3", false, nullptr}, {"1.5", false, nullptr}, {"-0x10", false, nullptr},
        {"0x", false, nullptr},  {"1n", false, nullptr},  {"12 34", false, nullptr},
        {"-", false, nullptr},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        s = embi_js_str_new_utf8(rt, cases[i].text, strlen(cases[i].text));
        bool ok = embi_js_string_to_bigint(rt, s, &out);
        EMB_CHECK(ok == cases[i].ok);
        if (ok && cases[i].want) {
            EMB_CHECK(out != nullptr);
            EMB_CHECK(bi_is(rt, embi_js_from_bigint(out), 10, cases[i].want));
        }
    }
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_increment_negation_hash_and_zero)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    EMB_CHECK(bi_is(rt, embi_js_bigint_inc(rt, bi_i64(rt, 0), -1), 10, "-1"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_inc(rt, bi_i64(rt, -1), 1), 10, "0"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_inc(rt, bi_i64(rt, 9), 1), 10, "10"));
    EMB_CHECK(bi_is(rt, embi_js_bigint_neg(rt, bi_i64(rt, 0)), 10, "0"));
    EMB_CHECK(embi_js_bigint_hash(embi_js_as_bigint(bi_i64(rt, 77))) ==
              embi_js_bigint_hash(embi_js_as_bigint(bi_parse(rt, "77", 10))));
    EMB_CHECK(embi_js_bigint_is_zero(embi_js_as_bigint(bi_op(rt, EMBI_JS_BIG_SUB, bi_i64(rt, 4),
                                                              bi_i64(rt, 4)))));
    EMB_CHECK(embi_js_bigint_is_zero(embi_js_as_bigint(bi_i64(rt, 0))));
    EMB_CHECK(!embi_js_bigint_is_neg(embi_js_as_bigint(bi_op(rt, EMBI_JS_BIG_SUB, bi_i64(rt, 4),
                                                              bi_i64(rt, 4)))));
    embi_js_runtime_free(rt);
}

EMB_TEST(bigint_runtime_releases_all_memory)
{
    size_t before = embi_mem_live();
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_value a = bi_pow(rt, bi_i64(rt, 3), 3000);
    embi_js_value b = bi_pow(rt, bi_i64(rt, 7), 2000);
    embi_js_value p = bi_op(rt, EMBI_JS_BIG_MUL, a, b);
    embi_js_value q = bi_op(rt, EMBI_JS_BIG_DIV, p, a);
    EMB_CHECK(bi_cmp_eq(q, b));
    EMB_CHECK(bi_is(rt, bi_op(rt, EMBI_JS_BIG_MOD, p, bi_i64(rt, 1)), 10, "0"));
    embi_js_runtime_free(rt);
    EMB_CHECK(embi_mem_live() == before);
}

/* Knuth algorithm D with the add-back step: the quotient estimate is one too large and the divisor
 * is added back. Quotients and remainders were computed independently (Python integers), and the
 * first four vectors take the add-back branch. The last one is an ordinary case. */
EMB_TEST(bigint_division_knuth_add_back_vectors)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    struct {
        const char *u;
        const char *v;
        const char *q;
        const char *r;
    } cases[] = {
        {"7fffffff00008000000080000000ffff", "800000000000800080000000", "fffffffd",
         "7fffffff800200018000ffff"},
        {"7ffffffffffffffffffffffefffe0000", "800000008000000080000000", "fffffffe",
         "800000007ffffffffffe0000"},
        {"fffffffe80000000fffffffe80000001", "ffffffff80000000fffe0000", "fffffffe",
         "ffffffff000200007ffc0001"},
        {"800000000000ffff7fffffff00010000", "8000800000007fffffffffff", "ffff0000",
         "800080000000000000000000"},
        {"80000000fffe00000000", "8000ffffffff", "fffe0005", "7ff9fffe0005"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_js_value u = bi_parse(rt, cases[i].u, 16);
        embi_js_value v = bi_parse(rt, cases[i].v, 16);
        embi_js_value q = bi_op(rt, EMBI_JS_BIG_DIV, u, v);
        embi_js_value r = bi_op(rt, EMBI_JS_BIG_MOD, u, v);
        EMB_CHECK(bi_is(rt, q, 16, cases[i].q));
        EMB_CHECK(bi_is(rt, r, 16, cases[i].r));
        /* u == q * v + r */
        EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_ADD, bi_op(rt, EMBI_JS_BIG_MUL, q, v), r), u));
        /* Truncation toward zero: (-u) / v == -q and (-u) % v == -r. */
        embi_js_value nu = embi_js_bigint_neg(rt, u);
        EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_DIV, nu, v), embi_js_bigint_neg(rt, q)));
        EMB_CHECK(bi_cmp_eq(bi_op(rt, EMBI_JS_BIG_MOD, nu, v), embi_js_bigint_neg(rt, r)));
    }
    embi_js_runtime_free(rt);
}

/* Number(bigint) and the ties: the nearest double, ties to even, with a sticky bit below a tie. */
EMB_TEST(bigint_to_double_rounds_ties_to_even)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    struct {
        const char *dec;
        double want;
    } cases[] = {
        {"9007199254740993", 9007199254740992.0},          /* 2^53 + 1: tie, down to even */
        {"9007199254740995", 9007199254740996.0},          /* 2^53 + 3: tie, up to even */
        {"18446744073709553664", 18446744073709551616.0},  /* 2^64 + 2^11: tie, down to 2^64 */
        {"18446744073709553665", 18446744073709555712.0},  /* 2^64 + 2^11 + 1: sticky, up */
        {"18446744073709557760", 18446744073709559808.0},  /* 2^64 + 2^12 + 2^11: tie, up */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        embi_js_value v = bi_parse(rt, cases[i].dec, 10);
        EMB_CHECK(embi_js_bigint_to_double(embi_js_as_bigint(v)) == cases[i].want);
        embi_js_value nv = embi_js_bigint_neg(rt, v);
        EMB_CHECK(embi_js_bigint_to_double(embi_js_as_bigint(nv)) == -cases[i].want);
    }
    embi_js_runtime_free(rt);
}

/* Operations on values of up to 8 limbs make no heap limb buffer: the only allocation is the
 * result cell. The counter is read before any printing, which may allocate. */
EMB_TEST(bigint_small_operations_allocate_only_the_result)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    embi_js_value a = bi_i64(rt, 123456789012LL);  /* two limbs */
    embi_js_value b = bi_i64(rt, -987654321LL);    /* one limb */
    embi_js_value c2 = bi_i64(rt, 4294967297LL);   /* 2^32 + 1: two limbs */
    embi_js_value i7 = bi_i64(rt, 7);              /* int32-sized */
    embi_js_value i3 = bi_i64(rt, 3);
    embi_js_value sh = bi_i64(rt, 37);
    embi_js_value res[20];
    uint64_t before = embi_js_bigint_temp_allocs();
    res[0] = bi_op(rt, EMBI_JS_BIG_ADD, a, b);
    res[1] = bi_op(rt, EMBI_JS_BIG_SUB, i7, i3);
    res[2] = bi_op(rt, EMBI_JS_BIG_MUL, a, c2);
    res[3] = bi_op(rt, EMBI_JS_BIG_DIV, a, c2);   /* two-limb divisor: Knuth D on stack buffers */
    res[4] = bi_op(rt, EMBI_JS_BIG_MOD, a, c2);
    res[5] = bi_op(rt, EMBI_JS_BIG_MOD, b, i7);
    res[6] = bi_op(rt, EMBI_JS_BIG_EXP, i7, i3);
    res[7] = bi_op(rt, EMBI_JS_BIG_SHL, a, i3);
    res[8] = bi_op(rt, EMBI_JS_BIG_SAR, a, sh);
    res[9] = bi_op(rt, EMBI_JS_BIG_AND, b, a);
    res[10] = bi_op(rt, EMBI_JS_BIG_OR, b, a);
    res[11] = bi_op(rt, EMBI_JS_BIG_XOR, b, i7);
    res[12] = embi_js_bigint_neg(rt, a);
    res[13] = embi_js_bigint_not(rt, b);
    res[14] = embi_js_bigint_not(rt, i7);
    res[15] = embi_js_bigint_inc(rt, i7, 1);
    res[16] = embi_js_bigint_inc(rt, i7, -1);
    res[17] = embi_js_bigint_as_n(rt, b, 8, true);
    res[18] = embi_js_bigint_as_n(rt, b, 40, false);
    res[19] = bi_op(rt, EMBI_JS_BIG_SHL, i7, sh);
    uint64_t used = embi_js_bigint_temp_allocs() - before;
    EMB_CHECK_EQ_INT((int)used, 0);
    /* The results are right too. */
    EMB_CHECK(bi_is(rt, res[0], 10, "122469134691"));
    EMB_CHECK(bi_is(rt, res[1], 10, "4"));
    EMB_CHECK(bi_is(rt, res[6], 10, "343"));
    EMB_CHECK(bi_is(rt, res[13], 10, "987654320"));
    /* -987654321 mod 256 is 79 (its low byte is 0xB1 = 177, so the complement is 79). */
    EMB_CHECK(bi_is(rt, res[17], 10, "79"));
    EMB_CHECK(bi_is(rt, res[14], 10, "-8"));
    /* The counter is not vacuous: a product of two 15-limb values needs heap buffers. */
    embi_js_value big = bi_pow(rt, bi_i64(rt, 3), 300);
    uint64_t mark = embi_js_bigint_temp_allocs();
    (void)bi_op(rt, EMBI_JS_BIG_MUL, big, big);
    EMB_CHECK(embi_js_bigint_temp_allocs() > mark);
    embi_js_runtime_free(rt);
}

/* Results above the size limit throw before their cell is allocated. These cases are rejected
 * from the operand sizes alone, so they return at once. */
EMB_TEST(bigint_size_limit_rejects_oversize_results)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_REQUIRE(rt != nullptr);
    /* 2^(2^30) has 2^30 + 1 bits; (2^30 + 1 bits) is over the limit. */
    embi_js_value p = embi_js_bigint_binop(rt, EMBI_JS_BIG_EXP, bi_i64(rt, 2),
                                           bi_i64(rt, 1073741824LL));
    EMB_CHECK(embi_js_is_exception(p));
    embi_js_exception_clear(rt);
    /* 1 << 2^30 has 2^30 + 1 bits. */
    embi_js_value s = embi_js_bigint_binop(rt, EMBI_JS_BIG_SHL, bi_i64(rt, 1),
                                           bi_i64(rt, 1073741824LL));
    EMB_CHECK(embi_js_is_exception(s));
    embi_js_exception_clear(rt);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
