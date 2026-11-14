#include "emb_test.h"

#include "jslex/jslex.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCRATCH_UNITS 256u

static uint16_t scratch[SCRATCH_UNITS];
static embi_jslex lx;
static embi_jstok tok;

static void open_src(const char *s, uint32_t flags)
{
    embi_jslex_init(&lx, (const uint8_t *)s, (uint32_t)strlen(s), scratch, SCRATCH_UNITS, flags);
}

static void next_tok(void)
{
    embi_jslex_next(&lx, &tok);
}

/* Lexes text as one NUMBER token and reports whether it is exactly the value want. */
static bool number_is(const char *text, double want)
{
    open_src(text, 0);
    next_tok();
    if (tok.kind != EMBI_JST_NUMBER) {
        return false;
    }
    if (tok.num != want && !(isnan(tok.num) && isnan(want))) {
        return false;
    }
    next_tok();
    return tok.kind == EMBI_JST_EOF;
}

static bool is_error(const char *text)
{
    open_src(text, 0);
    next_tok();
    return tok.kind == EMBI_JST_ERROR;
}

EMB_TEST(decimal_and_radix_values)
{
    EMB_CHECK(number_is("0", 0.0));
    EMB_CHECK(number_is("1_000", 1000.0));
    EMB_CHECK(number_is("0x1F", 31.0));
    EMB_CHECK(number_is("0XfF", 255.0));
    EMB_CHECK(number_is("0o17", 15.0));
    EMB_CHECK(number_is("0b101", 5.0));
    EMB_CHECK(number_is("0b1_0", 2.0));
    EMB_CHECK(number_is("1e3", 1000.0));
    EMB_CHECK(number_is("1E+3", 1000.0));
    EMB_CHECK(number_is(".5", 0.5));
    EMB_CHECK(number_is("5.", 5.0));
    EMB_CHECK(number_is("1.5e-3", 1.5e-3));
    EMB_CHECK(number_is("0.000001", 1e-6));
    EMB_CHECK(number_is("0e0", 0.0));
    /* "1." is a number, so "1..x" lexes as NUMBER DOT NAME. */
    open_src("1..x", 0);
    next_tok();
    EMB_CHECK(tok.kind == EMBI_JST_NUMBER && tok.num == 1.0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_DOT);
}

EMB_TEST(legacy_octal_and_nonoctal_decimal)
{
    open_src("017", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NUMBER);
    EMB_CHECK(tok.num == 15.0);
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);
    EMB_CHECK_EQ_INT(tok.radix, 8);

    open_src("08", 0);
    next_tok();
    EMB_CHECK(tok.num == 8.0);
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);

    open_src("09.5", 0);
    next_tok();
    EMB_CHECK(tok.num == 9.5);
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);

    open_src("0", 0);
    next_tok();
    EMB_CHECK(!(tok.flags & EMBI_JST_F_LEGACY));
}

EMB_TEST(numeric_literal_errors)
{
    const char *bad[] = {
        "0x",    /* missing digits */
        "0x_1",  /* separator after prefix */
        "1__0",  /* doubled separator */
        "1_",    /* trailing separator */
        "1_.5",  /* separator before point */
        "0_1",   /* separator after leading zero */
        "3in",   /* identifier right after a number */
        "0b12",  /* digit out of range */
        "0o8",   /* digit out of range */
        "1e",    /* missing exponent */
        "1e+",   /* missing exponent */
        "08n",   /* BigInt cannot be legacy */
        "01n",   /* BigInt cannot be legacy */
        "1.toString", /* point then identifier */
        "1$",    /* dollar right after a number */
        "1\\u0061", /* escape right after a number */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EMB_CHECK(is_error(bad[i]));
    }
}

EMB_TEST(bigint_literals_keep_digits)
{
    char digits[32];

    open_src("123n", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_BIGINT);
    uint32_t n = embi_jslex_bigint_digits(&lx, &tok, digits, sizeof digits);
    EMB_CHECK_EQ_INT(n, 3);
    EMB_CHECK(memcmp(digits, "123", 3) == 0);
    EMB_CHECK_EQ_INT(tok.radix, 10);

    open_src("0xFFn", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_BIGINT);
    EMB_CHECK_EQ_INT(tok.radix, 16);
    n = embi_jslex_bigint_digits(&lx, &tok, digits, sizeof digits);
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(memcmp(digits, "FF", 2) == 0);

    open_src("0b1_0n", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_BIGINT);
    n = embi_jslex_bigint_digits(&lx, &tok, digits, sizeof digits);
    EMB_CHECK_EQ_INT(n, 2);
    EMB_CHECK(memcmp(digits, "10", 2) == 0);
}

EMB_TEST(correctly_rounded_known_cases)
{
    EMB_CHECK(number_is("4.9406564584124654e-324", 4.9406564584124654e-324));
    EMB_CHECK(number_is("2.4703282292062328e-324", 4.9406564584124654e-324));
    EMB_CHECK(number_is("2.4703282292062327e-324", 0.0));
    EMB_CHECK(number_is("2.2250738585072011e-308", 2.2250738585072011e-308));
    EMB_CHECK(number_is("2.2250738585072014e-308", 2.2250738585072014e-308));
    EMB_CHECK(number_is("1.7976931348623157e308", 1.7976931348623157e308));
    EMB_CHECK(number_is("1.7976931348623158e308", 1.7976931348623157e308));
    EMB_CHECK(number_is("1.7976931348623159e308", __builtin_inf()));
    EMB_CHECK(number_is("1e400", __builtin_inf()));
    EMB_CHECK(number_is("1e-400", 0.0));
    EMB_CHECK(number_is("0.1", 0.1));
    EMB_CHECK(number_is("0.30000000000000004", 0.30000000000000004));
    EMB_CHECK(number_is("9007199254740993", 9007199254740992.0));
    EMB_CHECK(number_is("9007199254740995", 9007199254740996.0));
    EMB_CHECK(number_is("9007199254740992.000000000000000000000000000001", 9007199254740992.0));
    EMB_CHECK(number_is("9007199254740993.000000000000000000000001", 9007199254740994.0));
    EMB_CHECK(number_is("1e23", 1e23));
    EMB_CHECK(number_is("123456789012345678901234567890", 1.2345678901234568e29));
    EMB_CHECK(number_is("0.1000000000000000055511151231257827021181583404541015625", 0.1));
    EMB_CHECK(number_is("1e308", 1e308));
    EMB_CHECK(number_is("5e-324", 4.9406564584124654e-324));
}

EMB_TEST(correctly_rounded_radix_cases)
{
    EMB_CHECK(number_is("0x1fffffffffffff", 9007199254740991.0));
    EMB_CHECK(number_is("0x20000000000001", 9007199254740992.0));
    EMB_CHECK(number_is("0x20000000000003", 9007199254740996.0));
    EMB_CHECK(number_is("0xFFFFFFFFFFFFFFFFF", 295147905179352825856.0));
    /* 16^256 = 2^1024 and above is infinite. */
    char big[300];
    big[0] = '0';
    big[1] = 'x';
    big[2] = '1';
    memset(big + 3, '0', 256);
    big[259] = '\0';
    EMB_CHECK(number_is(big, __builtin_inf()));
    /* 16^256 - 1 rounds up to 2^1024 as well. */
    memset(big + 3, 'f', 256);
    EMB_CHECK(number_is(big, __builtin_inf()));
    /* 0xFFFFFFFFFFFFF800 = 2^64 - 2^11 is exactly representable. */
    EMB_CHECK(number_is("0xFFFFFFFFFFFFF800", 18446744073709549568.0));
}

EMB_TEST(random_doubles_round_trip_through_printf)
{
    /* For each double d, "%.17g" gives text that reads back as exactly d. */
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t bad = 0;
    for (int i = 0; i < 2000; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t bits = state & 0x7FFFFFFFFFFFFFFFull;
        union {
            uint64_t u;
            double d;
        } x;
        x.u = bits;
        double d = x.d;
        if (!(d == d) || d == __builtin_inf()) {
            continue;
        }
        char buf[64];
        int n = snprintf(buf, sizeof buf, "%.17g", d);
        if (n <= 0 || n >= (int)sizeof buf) {
            bad++;
            continue;
        }
        double back = embi_jslex_decimal_to_double(buf, (uint32_t)n);
        if (back != d) {
            bad++;
        }
    }
    EMB_CHECK_EQ_INT(bad, 0);
}

EMB_TEST(random_subnormals_round_trip_through_printf)
{
    /* Random subnormal doubles: exponent field zero, nonzero mantissa. */
    uint64_t state = 0xD1B54A32D192ED03ull;
    uint32_t bad = 0;
    for (int i = 0; i < 2000; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t mant = state & 0x000FFFFFFFFFFFFFull;
        if (mant == 0) {
            continue;
        }
        union {
            uint64_t u;
            double d;
        } x;
        x.u = mant;
        char buf[64];
        int n = snprintf(buf, sizeof buf, "%.17g", x.d);
        if (n <= 0 || n >= (int)sizeof buf) {
            bad++;
            continue;
        }
        if (embi_jslex_decimal_to_double(buf, (uint32_t)n) != x.d) {
            bad++;
        }
    }
    EMB_CHECK_EQ_INT(bad, 0);
}

EMB_TEST(random_integers_round_trip_through_hex)
{
    uint64_t state = 0x2545F4914F6CDD1Dull;
    uint32_t bad = 0;
    for (int i = 0; i < 2000; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%llx", (unsigned long long)state);
        double got = embi_jslex_radix_to_double(buf, (uint32_t)n, 16);
        if (got != (double)state) {
            bad++;
        }
    }
    EMB_CHECK_EQ_INT(bad, 0);
}

EMB_TEST_MAIN()
