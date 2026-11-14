#include "emb_test.h"

#include "base/base.h"

EMB_TEST(str_eq_exact_and_ascii_insensitive)
{
    embi_str a = EMBI_STR("Hello");
    embi_str b = EMBI_STR("hello");
    embi_str c = EMBI_STR("Hell");
    EMB_CHECK(!embi_str_eq(a, b));
    EMB_CHECK(embi_str_ieq(a, b));
    EMB_CHECK(!embi_str_ieq(a, c));
    EMB_CHECK(embi_str_eq(c, EMBI_STR("Hell")));
    embi_str empty = EMBI_STR("");
    EMB_CHECK(embi_str_eq(empty, (embi_str){nullptr, 0}));
    EMB_CHECK(embi_str_ieq(empty, (embi_str){nullptr, 0}));
}

EMB_TEST(str_icmp_orders_case_folded)
{
    EMB_CHECK(embi_str_icmp(EMBI_STR("abc"), EMBI_STR("ABD")) < 0);
    EMB_CHECK(embi_str_icmp(EMBI_STR("ABD"), EMBI_STR("abc")) > 0);
    EMB_CHECK(embi_str_icmp(EMBI_STR("ab"), EMBI_STR("ABC")) < 0);
    EMB_CHECK(embi_str_icmp(EMBI_STR("ABC"), EMBI_STR("ab")) > 0);
    EMB_CHECK_EQ_INT(embi_str_icmp(EMBI_STR("MiXeD"), EMBI_STR("mixed")), 0);
    /* Bytes above 0x7F are not folded. */
    EMB_CHECK(embi_str_icmp(EMBI_STR("\xC3\x89"), EMBI_STR("\xC3\xA9")) < 0);
}

EMB_TEST(sb_appends_text_and_numbers)
{
    embi_sb sb = {0};
    embi_sb_append(&sb, EMBI_STR("n="));
    embi_sb_append_i64(&sb, -9223372036854775807ll - 1);
    embi_sb_append_char(&sb, ' ');
    embi_sb_append_u64(&sb, 18446744073709551615ull);
    embi_sb_append_char(&sb, ' ');
    embi_sb_append_i64(&sb, 0);
    EMB_CHECK(embi_str_eq((embi_str){sb.data, sb.len},
                          EMBI_STR("n=-9223372036854775808 18446744073709551615 0")));
    embi_sb_free(&sb);
    EMB_CHECK(sb.data == nullptr);
    EMB_CHECK_EQ_INT(sb.len, 0);
}

EMB_TEST(sb_float_and_codepoint)
{
    embi_sb sb = {0};
    embi_sb_append_f64(&sb, 0.1);
    embi_sb_append_char(&sb, '|');
    embi_sb_append_f64(&sb, 2.5);
    embi_sb_append_char(&sb, '|');
    embi_sb_append_cp(&sb, 0x20AC);
    embi_sb_append_cp(&sb, 0x41);
    EMB_CHECK(embi_str_eq((embi_str){sb.data, sb.len},
                          EMBI_STR("0.10000000000000001|2.5|\xE2\x82\xAC" "A")));
    embi_sb_free(&sb);
}

EMB_TEST(sb_cstr_is_nul_terminated_and_not_counted)
{
    embi_sb sb = {0};
    const char *empty = embi_sb_cstr(&sb);
    EMB_REQUIRE(empty != nullptr);
    EMB_CHECK_EQ_INT(empty[0], 0);
    EMB_CHECK_EQ_INT(sb.len, 0);
    embi_sb_append(&sb, EMBI_STR("abc"));
    const char *s = embi_sb_cstr(&sb);
    EMB_CHECK_EQ_STR(s, "abc");
    EMB_CHECK_EQ_INT(sb.len, 3);
    embi_sb_clear(&sb);
    EMB_CHECK_EQ_INT(sb.len, 0);
    embi_sb_free(&sb);
}

EMB_TEST(sb_grows_for_large_input)
{
    size_t base = embi_mem_live();
    embi_sb sb = {0};
    for (int i = 0; i < 5000; i++) {
        embi_sb_append(&sb, EMBI_STR("0123456789"));
    }
    EMB_CHECK_EQ_INT(sb.len, 50000);
    EMB_CHECK(sb.data[49999] == '9');
    embi_sb_free(&sb);
    EMB_CHECK_EQ_INT(embi_mem_live(), base);
}

EMB_TEST_MAIN()
