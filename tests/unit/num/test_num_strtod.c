#include "emb_test.h"

#include "num/num.h"

#include <string.h>

static uint64_t dbits(double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static void expect_parse(const char *s, double want)
{
    size_t n = 0;
    double v = embi_num_parse_decimal(s, strlen(s), &n);
    EMB_CHECK(dbits(v) == dbits(want));
    EMB_CHECK(n == strlen(s));
}

EMB_TEST(test_num_strtod_hard_cases)
{
    expect_parse("5e-324", 4.9406564584124654e-324);
    expect_parse("2.4703282292062328e-324", 4.9406564584124654e-324); /* just above half */
    expect_parse("2.4703282292062327e-324", 0.0);                       /* just below half */
    expect_parse("1.7976931348623157e308", 1.7976931348623157e308);
    size_t n = 0;
    /* Midpoint between DBL_MAX and 2^1024 is 1.797693134862315807937...e308. */
    double below = embi_num_parse_decimal("1.7976931348623158e308", 22, &n);
    EMB_CHECK(dbits(below) == dbits(1.7976931348623157e308));
    double over = embi_num_parse_decimal("1.7976931348623159e308", 22, &n);
    EMB_CHECK(over == 1.0 / 0.0);
    expect_parse("2.2250738585072011e-308", 2.2250738585072011e-308);
    expect_parse("2.2250738585072014e-308", 2.2250738585072014e-308);
    expect_parse("0.30000000000000004", 0.30000000000000004);
    expect_parse("0.1", 0.1);
    expect_parse("1e23", 1e23);
    expect_parse("9007199254740993", 9007199254740992.0); /* tie, to even */
    expect_parse("9007199254740995", 9007199254740996.0); /* tie, to even */
    expect_parse("123456789012345678901234567890", 123456789012345678901234567890.0);
    expect_parse("0.000000000000000000000000000001", 1e-30);
    /* Long mantissas with a negative exponent take the bignum quotient path. */
    expect_parse("12345678901234567890123456789012345678901234567890e-5",
                 12345678901234567890123456789012345678901234567890e-5);
    expect_parse("99999999999999999999e-300", 99999999999999999999e-300);
    expect_parse("123456789012345678901234567890123456789012345678901234567890e-30",
                 123456789012345678901234567890123456789012345678901234567890e-30);
    expect_parse("1.00000000000000011102230246251565404236316680908203125",
                 1.00000000000000011102230246251565404236316680908203125);
    expect_parse("4.9406564584124654e-324", 4.9406564584124654e-324);
    expect_parse("2.2250738585072009e-308", 2.2250738585072009e-308);
}

EMB_TEST(test_num_strtod_syntax_and_consumed)
{
    size_t n = 0;
    double v = embi_num_parse_decimal("1.5e3xyz", 8, &n);
    EMB_CHECK(v == 1500.0);
    EMB_CHECK(n == 5);

    v = embi_num_parse_decimal("+.5", 3, &n);
    EMB_CHECK(v == 0.5);
    EMB_CHECK(n == 3);

    v = embi_num_parse_decimal("1e", 2, &n);
    EMB_CHECK(v == 1.0);
    EMB_CHECK(n == 1);

    v = embi_num_parse_decimal("1e+", 3, &n);
    EMB_CHECK(v == 1.0);
    EMB_CHECK(n == 1);

    v = embi_num_parse_decimal("1.", 2, &n);
    EMB_CHECK(v == 1.0);
    EMB_CHECK(n == 2);

    v = embi_num_parse_decimal(".", 1, &n);
    EMB_CHECK(v != v);
    EMB_CHECK(n == 0);

    v = embi_num_parse_decimal("-", 1, &n);
    EMB_CHECK(v != v);
    EMB_CHECK(n == 0);

    v = embi_num_parse_decimal("-0", 2, &n);
    EMB_CHECK(dbits(v) == dbits(-0.0));
    EMB_CHECK(n == 2);

    v = embi_num_parse_decimal("1e400", 5, &n);
    EMB_CHECK(v == 1.0 / 0.0);
    v = embi_num_parse_decimal("-1e400", 6, &n);
    EMB_CHECK(v == -1.0 / 0.0);
    v = embi_num_parse_decimal("1e-400", 6, &n);
    EMB_CHECK(v == 0.0);
    v = embi_num_parse_decimal("1e99999999999999999999", 22, &n);
    EMB_CHECK(v == 1.0 / 0.0);
    v = embi_num_parse_decimal("0e99999999999", 13, &n);
    EMB_CHECK(v == 0.0);
    const char *tiny = "0.0000000000000000000000000000000000000000001e-5";
    v = embi_num_parse_decimal(tiny, strlen(tiny), &n);
    EMB_CHECK(v == 1e-48);
    EMB_CHECK(n == strlen(tiny));
}

/* Random finite doubles from a fixed-seed xorshift64. Each must print and parse back to the
 * same bits. */
EMB_TEST(test_num_roundtrip_one_million_random)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    int checked = 0;
    int failures = 0;
    char buf[EMBI_NUM_STR_MAX];
    for (int i = 0; i < 1000000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        double v;
        memcpy(&v, &x, sizeof v);
        if (v != v || v == 1.0 / 0.0 || v == -1.0 / 0.0 || v == 0.0) {
            continue;
        }
        size_t len = embi_num_to_str(v, buf);
        size_t used = 0;
        double back = embi_num_parse_decimal(buf, len, &used);
        checked++;
        if (dbits(back) != dbits(v) || used != len) {
            failures++;
            if (failures <= 5) {
                SDL_Log("    roundtrip mismatch: %s -> %a", buf, v);
            }
        }
    }
    EMB_CHECK(checked > 900000);
    EMB_CHECK(failures == 0);
}

/* Random doubles with a moderate exponent, so that more values have short decimal forms. */
EMB_TEST(test_num_roundtrip_moderate_exponents)
{
    uint64_t x = 0x0123456789ABCDEFull;
    char buf[EMBI_NUM_STR_MAX];
    int failures = 0;
    for (int i = 0; i < 100000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t mant = x & 0x000FFFFFFFFFFFFFull;
        uint64_t biased = 1023 + (x >> 52) % 80 - 40; /* exponents in about [2^-40, 2^40) */
        uint64_t bits = (biased << 52) | mant;
        double v;
        memcpy(&v, &bits, sizeof v);
        size_t len = embi_num_to_str(v, buf);
        size_t used = 0;
        double back = embi_num_parse_decimal(buf, len, &used);
        if (dbits(back) != dbits(v)) {
            failures++;
        }
    }
    EMB_CHECK(failures == 0);
}

EMB_TEST_MAIN()
