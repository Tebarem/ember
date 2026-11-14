/*
 * Forgiving base64 (WHATWG Infra) and the standard encoder. The cases follow the rules: ASCII
 * whitespace is ignored, padding is optional when the length allows it, and a length that leaves
 * one character over is an error.
 */
#include "emb_test.h"

#include "base/base.h"

static bool decodes_to(const char *in, const char *want)
{
    embi_sb out = {nullptr, 0, 0};
    bool ok = embi_base64_decode((embi_str){in, (uint32_t)strlen(in)}, &out);
    bool same = ok && out.len == strlen(want) && (out.len == 0 || memcmp(out.data, want, out.len) == 0);
    embi_sb_free(&out);
    return same;
}

static bool decode_fails(const char *in)
{
    embi_sb out = {nullptr, 0, 0};
    bool ok = embi_base64_decode((embi_str){in, (uint32_t)strlen(in)}, &out);
    bool unchanged = out.len == 0;
    embi_sb_free(&out);
    return !ok && unchanged;
}

EMB_TEST(base64_decode_forgiving_cases)
{
    EMB_CHECK(decodes_to("Zm9vYmFy", "foobar"));
    EMB_CHECK(decodes_to("Zm9vYmE=", "fooba"));
    EMB_CHECK(decodes_to("Zm9vYg==", "foob"));
    EMB_CHECK(decodes_to("Zm9vYg", "foob"));           /* padding is optional */
    EMB_CHECK(decodes_to("Zm9v\tYmFy\n", "foobar"));   /* whitespace is removed */
    EMB_CHECK(decodes_to("Zm9v YmFy", "foobar"));
    EMB_CHECK(decodes_to("", ""));
    EMB_CHECK(decodes_to("+/+/", "\xFB\xFF\xBF"));     /* the two URL-unsafe symbols */

    EMB_CHECK(decode_fails("A"));          /* one character over a full group */
    EMB_CHECK(decode_fails("Zm9vYmFy="));  /* a stray '=' after a full group */
    EMB_CHECK(decode_fails("Zg==Zg=="));   /* '=' inside the data */
    EMB_CHECK(decode_fails("!!!!"));
    EMB_CHECK(decode_fails("Zm9-"));       /* '-' is URL-safe, not in the standard alphabet */
}

EMB_TEST(base64_encode_standard_padding)
{
    embi_sb out = {nullptr, 0, 0};
    embi_base64_encode((const unsigned char *)"foobar", 6, &out);
    EMB_CHECK(out.len == 8 && memcmp(out.data, "Zm9vYmFy", 8) == 0);
    embi_sb_free(&out);

    out = (embi_sb){nullptr, 0, 0};
    embi_base64_encode((const unsigned char *)"fooba", 5, &out);
    EMB_CHECK(out.len == 8 && memcmp(out.data, "Zm9vYmE=", 8) == 0);
    embi_sb_free(&out);

    out = (embi_sb){nullptr, 0, 0};
    embi_base64_encode((const unsigned char *)"f", 1, &out);
    EMB_CHECK(out.len == 4 && memcmp(out.data, "Zg==", 4) == 0);
    embi_sb_free(&out);

    out = (embi_sb){nullptr, 0, 0};
    embi_base64_encode((const unsigned char *)"", 0, &out);
    EMB_CHECK(out.len == 0);
    embi_sb_free(&out);
}

EMB_TEST(base64_round_trip_all_byte_values)
{
    unsigned char raw[256];
    for (int i = 0; i < 256; i++) {
        raw[i] = (unsigned char)i;
    }
    for (size_t n = 0; n <= 256; n++) {
        embi_sb enc = {nullptr, 0, 0};
        embi_base64_encode(raw, n, &enc);
        embi_sb dec = {nullptr, 0, 0};
        bool ok = embi_base64_decode((embi_str){enc.data, enc.len}, &dec);
        EMB_CHECK(ok);
        EMB_CHECK(dec.len == n && (n == 0 || memcmp(dec.data, raw, n) == 0));
        embi_sb_free(&enc);
        embi_sb_free(&dec);
    }
}

EMB_TEST(base64_allocations_balance)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 100; i++) {
        embi_sb out = {nullptr, 0, 0};
        EMB_CHECK(embi_base64_decode(EMBI_STR("Zm9vYmFy"), &out));
        embi_sb_free(&out);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST_MAIN()
