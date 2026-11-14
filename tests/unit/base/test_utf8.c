#include "emb_test.h"

#include "base/base.h"

/* Decodes the whole buffer and returns the number of code points. Stores up to cap of them. */
static size_t decode_all(const char *s, size_t n, uint32_t *out, size_t cap)
{
    const char *p = s;
    const char *end = s + n;
    size_t count = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (count < cap) {
            out[count] = cp;
        }
        count++;
    }
    return count;
}

EMB_TEST(utf8_decodes_ascii_and_multibyte)
{
    /* 'A', e-acute, euro sign, grinning face */
    const char s[] = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    uint32_t cps[8];
    size_t n = decode_all(s, sizeof s - 1, cps, 8);
    EMB_REQUIRE(n == 4);
    EMB_CHECK_EQ_INT(cps[0], 0x41);
    EMB_CHECK_EQ_INT(cps[1], 0xE9);
    EMB_CHECK_EQ_INT(cps[2], 0x20AC);
    EMB_CHECK_EQ_INT(cps[3], 0x1F600);
}

EMB_TEST(utf8_max_and_min_code_points)
{
    const char max[] = "\xF4\x8F\xBF\xBF";
    const char min2[] = "\xC2\x80";
    const char *p = max;
    uint32_t cp = embi_utf8_decode(&p, max + 4);
    EMB_CHECK_EQ_INT(cp, 0x10FFFF);
    EMB_CHECK(p == max + 4);
    p = min2;
    cp = embi_utf8_decode(&p, min2 + 2);
    EMB_CHECK_EQ_INT(cp, 0x80);
    EMB_CHECK(p == min2 + 2);
}

EMB_TEST(utf8_overlong_sequences_are_rejected)
{
    /* C0 and C1 can never lead. E0 80 80 and F0 80 80 80 are overlong encodings. */
    const char c0[] = "\xC0\x80";
    const char e0[] = "\xE0\x80\x80";
    const char f0[] = "\xF0\x80\x80\x80";
    uint32_t cps[8];
    EMB_CHECK_EQ_INT(decode_all(c0, 2, cps, 8), 2);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
    EMB_CHECK_EQ_INT(cps[1], 0xFFFD);
    /* After E0, only A0..BF is allowed, so E0 alone is one error and each 80 is another. */
    EMB_CHECK_EQ_INT(decode_all(e0, 3, cps, 8), 3);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
    /* After F0, only 90..BF is allowed, so the same shape applies with four bytes. */
    EMB_CHECK_EQ_INT(decode_all(f0, 4, cps, 8), 4);
    for (int i = 0; i < 4; i++) {
        EMB_CHECK_EQ_INT(cps[i], 0xFFFD);
    }
}

EMB_TEST(utf8_surrogates_are_rejected)
{
    /* ED A0 80 would be U+D800, which is not valid UTF-8. */
    const char s[] = "\xED\xA0\x80";
    uint32_t cps[8];
    size_t n = decode_all(s, 3, cps, 8);
    EMB_CHECK_EQ_INT(n, 3);
    for (size_t i = 0; i < n && i < 8; i++) {
        EMB_CHECK_EQ_INT(cps[i], 0xFFFD);
    }
}

EMB_TEST(utf8_above_max_is_rejected)
{
    uint32_t cps[8];
    const char over[] = "\xF4\x90\x80\x80"; /* U+110000 */
    size_t n = decode_all(over, 4, cps, 8);
    EMB_CHECK_EQ_INT(n, 4);
    for (size_t i = 0; i < n && i < 8; i++) {
        EMB_CHECK_EQ_INT(cps[i], 0xFFFD);
    }
    const char f5[] = "\xF5\x80\x80\x80";
    EMB_CHECK_EQ_INT(decode_all(f5, 4, cps, 8), 4);
    const char ff[] = "\xFF";
    EMB_CHECK_EQ_INT(decode_all(ff, 1, cps, 8), 1);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
}

EMB_TEST(utf8_truncated_sequences_at_end)
{
    /* E2 82 is a truncated euro sign: one replacement that consumes both bytes. */
    const char e2[] = "\xE2\x82";
    uint32_t cps[8];
    size_t n = decode_all(e2, 2, cps, 8);
    EMB_CHECK_EQ_INT(n, 1);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
    /* A lone lead at the end. */
    const char f0[] = "\xF0";
    EMB_CHECK_EQ_INT(decode_all(f0, 1, cps, 8), 1);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
}

EMB_TEST(utf8_invalid_continuation_is_not_consumed)
{
    /* E2 28 A1: 28 is not a continuation byte, so E2 is one error and the paren still decodes. */
    const char s[] = "\xE2\x28\xA1";
    uint32_t cps[8];
    size_t n = decode_all(s, 3, cps, 8);
    EMB_CHECK_EQ_INT(n, 3);
    EMB_CHECK_EQ_INT(cps[0], 0xFFFD);
    EMB_CHECK_EQ_INT(cps[1], 0x28);
    EMB_CHECK_EQ_INT(cps[2], 0xFFFD);
}

EMB_TEST(utf8_encode_lengths_and_replacement)
{
    char buf[4];
    EMB_CHECK_EQ_INT(embi_utf8_encode(0x41, buf), 1);
    EMB_CHECK_EQ_INT(embi_utf8_encode(0x7FF, buf), 2);
    EMB_CHECK_EQ_INT(embi_utf8_encode(0xFFFF, buf), 3);
    EMB_CHECK_EQ_INT(embi_utf8_encode(0x10FFFF, buf), 4);
    EMB_CHECK_EQ_INT(embi_utf8_encode(0xD800, buf), 3);
    EMB_CHECK_EQ_INT((unsigned char)buf[0], 0xEF);
    EMB_CHECK_EQ_INT((unsigned char)buf[1], 0xBF);
    EMB_CHECK_EQ_INT((unsigned char)buf[2], 0xBD);
    EMB_CHECK_EQ_INT(embi_utf8_encode(0x110000, buf), 3);
}

EMB_TEST(utf16_to_utf8_pairs_and_lone_surrogates)
{
    uint16_t pair[] = {0xD83D, 0xDE00};    /* U+1F600 */
    uint16_t lone_hi[] = {0xD83D, 0x0041}; /* high surrogate then 'A' */
    uint16_t lone_lo[] = {0xDC00};
    uint16_t trailing_hi[] = {0x0041, 0xD800};
    char out[16];
    EMB_CHECK_EQ_INT(embi_utf16_to_utf8(pair, 2, out, sizeof out), 4);
    EMB_CHECK_EQ_INT((unsigned char)out[0], 0xF0);
    EMB_CHECK_EQ_INT((unsigned char)out[3], 0x80);
    EMB_CHECK_EQ_INT(embi_utf16_to_utf8(lone_hi, 2, out, sizeof out), 4);
    EMB_CHECK_EQ_INT((unsigned char)out[0], 0xEF);
    EMB_CHECK_EQ_INT(out[3], 'A');
    EMB_CHECK_EQ_INT(embi_utf16_to_utf8(lone_lo, 1, out, sizeof out), 3);
    EMB_CHECK_EQ_INT(embi_utf16_to_utf8(trailing_hi, 2, out, sizeof out), 4);
    EMB_CHECK_EQ_INT(out[0], 'A');
}

EMB_TEST(utf8_to_utf16_counts_and_caps)
{
    /* 'A' then U+1F600 */
    embi_str s = EMBI_STR("A\xF0\x9F\x98\x80");
    EMB_CHECK_EQ_INT(embi_utf8_to_utf16(s, nullptr, 0), 3);
    uint16_t units[4] = {0};
    EMB_CHECK_EQ_INT(embi_utf8_to_utf16(s, units, 4), 3);
    EMB_CHECK_EQ_INT(units[0], 0x41);
    EMB_CHECK_EQ_INT(units[1], 0xD83D);
    EMB_CHECK_EQ_INT(units[2], 0xDE00);
    uint16_t small[1] = {0};
    EMB_CHECK_EQ_INT(embi_utf8_to_utf16(s, small, 1), 3);
    EMB_CHECK_EQ_INT(small[0], 0x41);
}

EMB_TEST(utf16_roundtrip_to_utf8_matches_source)
{
    embi_str src = EMBI_STR("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80!");
    uint16_t units[32];
    size_t nu = embi_utf8_to_utf16(src, units, 32);
    char back[64];
    size_t nb = embi_utf16_to_utf8(units, nu, back, sizeof back);
    EMB_REQUIRE(nb == src.len);
    EMB_CHECK(SDL_memcmp(back, src.ptr, nb) == 0);
}

EMB_TEST_MAIN()
