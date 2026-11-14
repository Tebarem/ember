#include "emb_test.h"

#include "num/num.h"

#include <string.h>

static void expect_fixed(double v, unsigned digits, const char *want)
{
    char buf[EMBI_NUM_STR_MAX];
    embi_num_to_fixed(v, digits, buf);
    EMB_CHECK_EQ_STR(buf, want);
}

static void expect_exp(double v, int digits, const char *want)
{
    char buf[EMBI_NUM_STR_MAX];
    embi_num_to_exponential(v, digits, buf);
    EMB_CHECK_EQ_STR(buf, want);
}

static void expect_prec(double v, unsigned p, const char *want)
{
    char buf[EMBI_NUM_STR_MAX];
    embi_num_to_precision(v, p, buf);
    EMB_CHECK_EQ_STR(buf, want);
}

EMB_TEST(test_num_fixed_exact_rounding)
{
    expect_fixed(1.005, 2, "1.00");   /* 1.00499999999999989... */
    expect_fixed(1.45, 1, "1.4");     /* 1.4499999999999999556... */
    expect_fixed(0.5, 0, "1");        /* tie: larger n */
    expect_fixed(2.5, 0, "3");
    expect_fixed(-2.5, 0, "-3");
    expect_fixed(123.456, 10, "123.4560000000");
    expect_fixed(0.0, 2, "0.00");
    expect_fixed(-0.0, 2, "0.00");
    expect_fixed(-0.0000001, 2, "-0.00");
    expect_fixed(1e21, 2, "1e+21");
    expect_fixed(1000000000000000128.0, 0, "1000000000000000128");
    expect_fixed(0.000001, 7, "0.0000010");
    expect_fixed(1.23e-10, 2, "0.00");
    expect_fixed(99.995, 2, "100.00"); /* 99.99500000000000454... */
    expect_fixed(42.0, 0, "42");
    expect_fixed(0.1, 20, "0.10000000000000000555");
    expect_fixed(-1.5, 3, "-1.500");
}

EMB_TEST(test_num_exponential_formatting)
{
    expect_exp(123456.0, 2, "1.23e+5");
    expect_exp(0.0, -1, "0e+0");
    expect_exp(0.0, 2, "0.00e+0");
    expect_exp(123.456, -1, "1.23456e+2");
    expect_exp(0.00001, 1, "1.0e-5");
    expect_exp(9.99, 1, "1.0e+1");
    expect_exp(1.5, 0, "2e+0");
    expect_exp(2.5, 0, "3e+0");
    expect_exp(-2.5, 0, "-3e+0");
    expect_exp(5e-324, -1, "5e-324");
    expect_exp(1e21, -1, "1e+21");
    expect_exp(1.0 / 0.0, 3, "Infinity");
    expect_exp(0.0 / 0.0, 3, "NaN");
}

EMB_TEST(test_num_precision_formatting)
{
    expect_prec(123.456, 4, "123.5");
    expect_prec(0.000123, 2, "0.00012");
    expect_prec(123456.0, 2, "1.2e+5");
    expect_prec(1.5, 1, "2");
    expect_prec(25.0, 1, "3e+1"); /* exponent 1 is not below precision 1 */
    expect_prec(0.0, 3, "0.00");
    expect_prec(1e-7, 1, "1e-7");
    expect_prec(1e21, 3, "1.00e+21");
    expect_prec(123.0, 3, "123");
    expect_prec(99.99, 3, "100");
    expect_prec(-1.5, 2, "-1.5");
    expect_prec(0.000001, 2, "0.0000010");
    expect_prec(1234.5, 6, "1234.50");
}

EMB_TEST(test_num_parse_radix_exact)
{
    size_t n = 0;
    EMB_CHECK(embi_num_parse_radix("ff", 2, 16, &n) == 255.0);
    EMB_CHECK(n == 2);
    EMB_CHECK(embi_num_parse_radix("zz", 2, 36, &n) == 1295.0);
    /* 0x20000000000001 == 2^53 + 1 is a tie, rounds to even (2^53). */
    EMB_CHECK(embi_num_parse_radix("20000000000001", 14, 16, &n) == 9007199254740992.0);
    /* 2^53 + 3 is a tie between 2^53 + 2 and 2^53 + 4, rounds to 2^53 + 4. */
    EMB_CHECK(embi_num_parse_radix("20000000000003", 14, 16, &n) == 9007199254740996.0);
    /* Stops at the first digit that is invalid for the radix. */
    EMB_CHECK(embi_num_parse_radix("1012", 4, 2, &n) == 5.0);
    EMB_CHECK(n == 3);
    double v = embi_num_parse_radix("", 0, 16, &n);
    EMB_CHECK(v != v);
    EMB_CHECK(n == 0);
    /* 2^1200 is far above DBL_MAX, so the result is infinite. */
    char big[1202];
    big[0] = '1';
    memset(big + 1, '0', 1200);
    v = embi_num_parse_radix(big, 1201, 2, &n);
    EMB_CHECK(v == 1.0 / 0.0);
}

EMB_TEST_MAIN()
