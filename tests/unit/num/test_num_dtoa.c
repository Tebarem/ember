#include "emb_test.h"

#include "num/num.h"

#include <string.h>

static void expect_str(double v, const char *want)
{
    char buf[EMBI_NUM_STR_MAX];
    embi_num_to_str(v, buf);
    EMB_CHECK_EQ_STR(buf, want);
}

static void expect_radix(double v, unsigned radix, const char *want)
{
    char buf[EMBI_NUM_STR_MAX];
    embi_num_to_radix_str(v, radix, buf);
    EMB_CHECK_EQ_STR(buf, want);
}

EMB_TEST(test_num_dtoa_ecma_formatting)
{
    expect_str(1e21, "1e+21");
    expect_str(1e20, "100000000000000000000");
    expect_str(1e-7, "1e-7");
    expect_str(1e-6, "0.000001");
    expect_str(0.000001234, "0.000001234");
    expect_str(-0.0, "0");
    expect_str(0.0, "0");
    expect_str(123456789012345680000.0, "123456789012345680000");
    expect_str(0.1, "0.1");
    expect_str(0.1 + 0.2, "0.30000000000000004");
    expect_str(-1.5, "-1.5");
    expect_str(100.0, "100");
    expect_str(123.456, "123.456");
    expect_str(4.35, "4.35");
    expect_str(1e23, "1e+23");
    expect_str(1.5e300, "1.5e+300");
    expect_str(1.7976931348623157e308, "1.7976931348623157e+308");
    expect_str(5e-324, "5e-324");
    expect_str(2.2250738585072014e-308, "2.2250738585072014e-308");
    expect_str(9007199254740992.0, "9007199254740992");
    expect_str(2.0 * 9007199254740992.0, "18014398509481984");
    expect_str(1.0, "1");
    expect_str(999999999999999999999.0, "1e+21");
}

EMB_TEST(test_num_dtoa_special_values)
{
    double inf = 1.0 / 0.0;
    double nan = 0.0 / 0.0;
    expect_str(inf, "Infinity");
    expect_str(-inf, "-Infinity");
    expect_str(nan, "NaN");
}

EMB_TEST(test_num_dtoa_fixed_exponent_boundaries)
{
    /* n == 21 is the last positional form; 1e21 switches to exponent form. */
    expect_str(123456789012345678901.0, "123456789012345680000");
    expect_str(1.2345e-6, "0.0000012345");
    expect_str(1.2345e-7, "1.2345e-7");
}

EMB_TEST(test_num_dtoa_radix_output)
{
    expect_radix(255.0, 16, "ff");
    expect_radix(0.5, 2, "0.1");
    expect_radix(-255.0, 2, "-11111111");
    expect_radix(35.0, 36, "z");
    expect_radix(3.5, 2, "11.1");
    expect_radix(0.0, 2, "0");
    expect_radix(0.1, 2, "0.0001100110011001100110011001100110011001100110011001101");
    expect_radix(1e21, 10, "1e+21");
}

EMB_TEST_MAIN()
