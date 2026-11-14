#include "emb_test.h"

#include "js/js.h"

#include <math.h>

/* Runtime shared by a test; every test creates and frees its own. */

static bool str_is(const embi_js_string *s, const char *ascii)
{
    size_t n = strlen(ascii);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint16_t)(unsigned char)ascii[i]) {
            return false;
        }
    }
    return true;
}

static embi_js_string *from_ascii(embi_js_runtime *rt, const char *s)
{
    return embi_js_str_new_utf8(rt, s, strlen(s));
}

static bool num_text_is(embi_js_runtime *rt, double d, const char *expect)
{
    embi_js_string *s = embi_js_number_to_str(rt, d);
    bool ok = str_is(s, expect);
    if (!ok) {
        embi_sb sb = {nullptr, 0, 0};
        embi_js_str_append_utf8(s, &sb);
        SDL_Log("    number_to_str(%.17g) = \"%s\", expected \"%s\"", d, embi_sb_cstr(&sb),
                expect);
        embi_sb_free(&sb);
    }
    return ok;
}

EMB_TEST(string_latin1_is_canonical_when_narrow)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    uint16_t units[] = {'A', 'B', 0xE9};
    embi_js_string *s = embi_js_str_new_utf16(rt, units, 3);
    EMB_CHECK(!embi_js_str_is_wide(s));
    EMB_CHECK_EQ_INT(embi_js_str_len(s), 3);
    /* The narrow form stores one byte per unit: 'A', 'B', 0xE9. */
    const uint8_t *bytes = embi_js_str_latin1(s);
    EMB_CHECK_EQ_INT(bytes[0], 'A');
    EMB_CHECK_EQ_INT(bytes[1], 'B');
    EMB_CHECK_EQ_INT(bytes[2], 0xE9);
    EMB_CHECK_EQ_INT(embi_js_str_at(s, 2), 0xE9);
    embi_js_string *wide = embi_js_str_new_utf16(rt, (const uint16_t[]){'A', 0x3B1}, 2);
    EMB_CHECK(embi_js_str_is_wide(wide));
    EMB_CHECK_EQ_INT(embi_js_str_at(wide, 1), 0x3B1);
    embi_js_runtime_free(rt);
}

EMB_TEST(string_equality_and_compare_follow_code_units)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *a = from_ascii(rt, "abc");
    embi_js_string *b = from_ascii(rt, "abc");
    embi_js_string *c = from_ascii(rt, "abd");
    embi_js_string *pre = from_ascii(rt, "ab");
    EMB_CHECK(embi_js_str_equal(a, b));
    EMB_CHECK(!embi_js_str_equal(a, c));
    EMB_CHECK(embi_js_str_compare(a, c) < 0);
    EMB_CHECK(embi_js_str_compare(c, a) > 0);
    EMB_CHECK(embi_js_str_compare(pre, a) < 0);
    EMB_CHECK_EQ_INT(embi_js_str_compare(a, b), 0);
    /* U+FFFF sorts above U+10000's high surrogate: code-unit order, not code-point order. */
    uint16_t bmp[] = {0xFFFF};
    uint16_t pair[] = {0xD800, 0xDC00};
    embi_js_string *hi = embi_js_str_new_utf16(rt, bmp, 1);
    embi_js_string *sur = embi_js_str_new_utf16(rt, pair, 2);
    EMB_CHECK(embi_js_str_compare(hi, sur) > 0);
    embi_js_runtime_free(rt);
}

EMB_TEST(string_concat_mixes_widths_and_keeps_canonical_form)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *a = from_ascii(rt, "foo");
    embi_js_string *b = from_ascii(rt, "bar");
    embi_js_string *ab = embi_js_str_concat(rt, a, b);
    EMB_CHECK(!embi_js_str_is_wide(ab));
    EMB_CHECK(str_is(ab, "foobar"));

    uint16_t w[] = {0x3B1};
    embi_js_string *gw = embi_js_str_new_utf16(rt, w, 1);
    embi_js_string *mixed = embi_js_str_concat(rt, a, gw);
    EMB_CHECK(embi_js_str_is_wide(mixed));
    EMB_CHECK_EQ_INT(embi_js_str_len(mixed), 4);
    EMB_CHECK_EQ_INT(embi_js_str_at(mixed, 3), 0x3B1);
    EMB_CHECK(embi_js_str_equal(mixed, embi_js_str_concat(rt, a, gw)));

    embi_js_string *empty = embi_js_str_concat(rt, a, from_ascii(rt, ""));
    EMB_CHECK(embi_js_str_equal(empty, a));
    embi_js_runtime_free(rt);
}

EMB_TEST(string_substring_clamps_and_narrows)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    embi_js_string *s = from_ascii(rt, "hello");
    EMB_CHECK(str_is(embi_js_str_substring(rt, s, 1, 4), "ell"));
    EMB_CHECK(str_is(embi_js_str_substring(rt, s, 3, 99), "lo"));
    EMB_CHECK_EQ_INT(embi_js_str_len(embi_js_str_substring(rt, s, 4, 2)), 0);
    EMB_CHECK(embi_js_str_substring(rt, s, 0, 5) == s);

    /* A wide string whose slice has only Latin-1 units becomes narrow again. */
    uint16_t u[] = {'x', 0x3B1, 'y'};
    embi_js_string *w = embi_js_str_new_utf16(rt, u, 3);
    EMB_CHECK(embi_js_str_is_wide(w));
    embi_js_string *tail = embi_js_str_substring(rt, w, 2, 3);
    EMB_CHECK(!embi_js_str_is_wide(tail));
    EMB_CHECK(str_is(tail, "y"));
    embi_js_runtime_free(rt);
}

EMB_TEST(string_utf8_round_trip_with_surrogates_and_invalid_input)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    /* U+1F600 needs a surrogate pair, U+00E9 is Latin-1 and U+20AC is wide. */
    const char *text = "a\xF0\x9F\x98\x80" "\xC3\xA9\xE2\x82\xAC";
    embi_js_string *s = embi_js_str_new_utf8(rt, text, strlen(text));
    EMB_CHECK(embi_js_str_is_wide(s));
    EMB_CHECK_EQ_INT(embi_js_str_len(s), 5); /* a, D83D, DE00, e-acute, euro */
    EMB_CHECK_EQ_INT(embi_js_str_at(s, 1), 0xD83D);
    EMB_CHECK_EQ_INT(embi_js_str_at(s, 2), 0xDE00);
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &sb);
    EMB_CHECK_EQ_INT(sb.len, strlen(text));
    EMB_CHECK(memcmp(sb.data, text, sb.len) == 0);
    embi_sb_free(&sb);

    /* Invalid bytes become U+FFFD; a surrogate encoded in UTF-8 is invalid too. */
    const char bad[] = {'\xFF', 'x', '\xED', '\xA0', '\x80'};
    embi_js_string *b = embi_js_str_new_utf8(rt, bad, sizeof bad);
    EMB_CHECK_EQ_INT(embi_js_str_at(b, 0), 0xFFFD);
    EMB_CHECK_EQ_INT(embi_js_str_at(b, 1), 'x');
    for (uint32_t i = 2; i < embi_js_str_len(b); i++) {
        EMB_CHECK_EQ_INT(embi_js_str_at(b, i), 0xFFFD);
    }

    /* Lone surrogate units become U+FFFD on the way out. */
    uint16_t lone[] = {0xD800, 'z'};
    embi_js_string *ls = embi_js_str_new_utf16(rt, lone, 2);
    embi_sb out = {nullptr, 0, 0};
    embi_js_str_append_utf8(ls, &out);
    EMB_CHECK_EQ_INT(out.len, 4); /* EF BF BD 'z' */
    EMB_CHECK((unsigned char)out.data[0] == 0xEF && out.data[3] == 'z');
    embi_sb_free(&out);
    embi_js_runtime_free(rt);
}

EMB_TEST(string_array_index_detection)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    uint32_t idx = 0;
    EMB_CHECK(embi_js_str_is_array_index(from_ascii(rt, "0"), &idx) && idx == 0);
    EMB_CHECK(embi_js_str_is_array_index(from_ascii(rt, "4294967294"), &idx) &&
              idx == 4294967294u);
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, "4294967295"), &idx));
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, "05"), &idx));
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, ""), &idx));
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, "-1"), &idx));
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, "1.0"), &idx));
    EMB_CHECK(!embi_js_str_is_array_index(from_ascii(rt, "12345678901"), &idx));
    embi_js_runtime_free(rt);
}

EMB_TEST(string_number_to_string_matches_js)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK(num_text_is(rt, 0.0, "0"));
    EMB_CHECK(num_text_is(rt, -0.0, "0"));
    EMB_CHECK(num_text_is(rt, 1.0, "1"));
    EMB_CHECK(num_text_is(rt, -42.0, "-42"));
    EMB_CHECK(num_text_is(rt, 2147483647.0, "2147483647"));
    EMB_CHECK(num_text_is(rt, -2147483648.0, "-2147483648"));
    EMB_CHECK(num_text_is(rt, 100.0, "100"));
    EMB_CHECK(num_text_is(rt, 1e20, "100000000000000000000"));
    EMB_CHECK(num_text_is(rt, 1e21, "1e+21"));
    EMB_CHECK(num_text_is(rt, 0.5, "0.5"));
    EMB_CHECK(num_text_is(rt, 0.1 + 0.2, "0.30000000000000004"));
    EMB_CHECK(num_text_is(rt, 123.456, "123.456"));
    EMB_CHECK(num_text_is(rt, 1e-6, "0.000001"));
    EMB_CHECK(num_text_is(rt, 1e-7, "1e-7"));
    EMB_CHECK(num_text_is(rt, 5e-324, "5e-324"));
    EMB_CHECK(num_text_is(rt, 1.7976931348623157e308, "1.7976931348623157e+308"));
    /* The literal 2.2250738585072011e-308 parses to the largest subnormal, whose shortest form is
     * shorter than the literal. DBL_MIN is the smallest normal. */
    EMB_CHECK(num_text_is(rt, 2.2250738585072011e-308, "2.225073858507201e-308"));
    EMB_CHECK(num_text_is(rt, 2.2250738585072014e-308, "2.2250738585072014e-308"));
    EMB_CHECK(num_text_is(rt, -1.5e-10, "-1.5e-10"));
    EMB_CHECK(num_text_is(rt, NAN, "NaN"));
    EMB_CHECK(num_text_is(rt, INFINITY, "Infinity"));
    EMB_CHECK(num_text_is(rt, -INFINITY, "-Infinity"));
    embi_js_runtime_free(rt);
}

static double to_num(embi_js_runtime *rt, const char *text)
{
    return embi_js_str_to_number(from_ascii(rt, text));
}

/* Powers of two have an asymmetric rounding interval, so the correctly rounded p-digit decimal can
 * fail to round-trip while a neighbouring p-digit decimal succeeds. The expected strings are the
 * shortest round-trip forms (ECMA-262 Number::toString requires the fewest digits). */
EMB_TEST(string_number_shortest_for_power_of_two_boundaries)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -24), "5.960464477539063e-8"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -44), "5.684341886080802e-14"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -77), "6.617444900424222e-24"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, 89), "6.189700196426902e+26"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -1017), "7.120236347223045e-307"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, 976), "6.386688990511104e+293"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -1007), "7.291122019556398e-304"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -957), "8.209073602596753e-289"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -808), "5.858190679279809e-244"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -140), "7.174648137343064e-43"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -97), "6.310887241768095e-30"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -1074), "5e-324"));
    EMB_CHECK(num_text_is(rt, ldexp(1.0, -1), "0.5"));
    embi_js_runtime_free(rt);
}

EMB_TEST(string_to_number_follows_stringtonumber)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK(to_num(rt, "") == 0.0);
    EMB_CHECK(to_num(rt, "   ") == 0.0);
    EMB_CHECK(to_num(rt, " 42 ") == 42.0);
    EMB_CHECK(to_num(rt, "\t\n\v\f\r 7\xC2\xA0") == 7.0);
    EMB_CHECK(to_num(rt, "-3.5") == -3.5);
    EMB_CHECK(to_num(rt, "+.5") == 0.5);
    EMB_CHECK(to_num(rt, "5.") == 5.0);
    EMB_CHECK(to_num(rt, "1e3") == 1000.0);
    EMB_CHECK(to_num(rt, "1E-2") == 0.01);
    EMB_CHECK(to_num(rt, "0x1F") == 31.0);
    EMB_CHECK(to_num(rt, "0XfF") == 255.0);
    EMB_CHECK(to_num(rt, "0o17") == 15.0);
    EMB_CHECK(to_num(rt, "0b101") == 5.0);
    EMB_CHECK(to_num(rt, "Infinity") == INFINITY);
    EMB_CHECK(to_num(rt, "-Infinity") == -INFINITY);
    EMB_CHECK(to_num(rt, "+Infinity") == INFINITY);
    EMB_CHECK(to_num(rt, "-0") == 0.0 && 1.0 / to_num(rt, "-0") < 0);
    EMB_CHECK(to_num(rt, "1e400") == INFINITY);
    EMB_CHECK(isnan(to_num(rt, ".")));
    EMB_CHECK(isnan(to_num(rt, "e5")));
    EMB_CHECK(isnan(to_num(rt, "1e")));
    EMB_CHECK(isnan(to_num(rt, "1_0")));
    EMB_CHECK(isnan(to_num(rt, "0x")));
    EMB_CHECK(isnan(to_num(rt, "0x G")));
    EMB_CHECK(isnan(to_num(rt, "-0x10")));
    EMB_CHECK(isnan(to_num(rt, "0b2")));
    EMB_CHECK(isnan(to_num(rt, "infinity")));
    EMB_CHECK(isnan(to_num(rt, "12abc")));
    embi_js_runtime_free(rt);
}

EMB_TEST(string_hex_conversion_rounds_correctly)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    EMB_CHECK(to_num(rt, "0xFFFFFFFFFFFFFFFF") == 18446744073709551616.0);
    /* 2^53 + 1 is a tie between 2^53 and 2^53 + 2; ties go to even, which is 2^53. */
    EMB_CHECK(to_num(rt, "0x20000000000001") == 9007199254740992.0);
    /* 2^53 + 3 is a tie between 2^53 + 2 and 2^53 + 4; ties go to even, which is 2^53 + 4. */
    EMB_CHECK(to_num(rt, "0x20000000000003") == 9007199254740996.0);
    /* 2^64 has ulp 4096 above it. 2^64 + 2048 is a tie and goes to even (2^64); 2^64 + 2049 is
     * above half an ulp and rounds up to 2^64 + 4096. */
    EMB_CHECK(to_num(rt, "0x10000000000000800") == 18446744073709551616.0);
    EMB_CHECK(to_num(rt, "0x10000000000000801") == 18446744073709555712.0);
    embi_js_runtime_free(rt);
}

EMB_TEST(string_numbers_round_trip_through_text)
{
    embi_js_runtime *rt = embi_js_runtime_new();
    /* A fixed-seed walk over bit patterns: every finite double must survive toString then
     * StringToNumber unchanged. */
    uint64_t state = 0x9E3779B97F4A7C15ull;
    int checked = 0;
    for (int i = 0; checked < 2000 && i < 100000; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        uint64_t bits = state & ~(1ull << 63);
        if ((bits >> 52) == 0x7FF) {
            continue;
        }
        double d;
        memcpy(&d, &bits, sizeof d);
        embi_js_string *s = embi_js_number_to_str(rt, d);
        double back = embi_js_str_to_number(s);
        if (back != d) {
            SDL_Log("    round trip failed for bits 0x%016llx", (unsigned long long)bits);
        }
        EMB_CHECK(back == d);
        checked++;
    }
    EMB_CHECK_EQ_INT(checked, 2000);
    embi_js_runtime_free(rt);
}

EMB_TEST_MAIN()
