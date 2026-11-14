/*
 * Streaming decode: a stream split at every byte boundary decodes the same as one call, BOM
 * handling, the U+FFFD error rules of the UTF-8 and UTF-16 decoders, fatal mode, and the
 * replacement and x-user-defined encodings.
 */
#include "loader_test.h"

#include "base/base.h"
#include "loader/loader.h"

#include <stdio.h>

/* Decodes bytes in one call (flush at the end). */
static bool decode_all(const embi_encoding *enc, const unsigned char *p, size_t n, bool fatal,
                       bool ignore_bom, embi_sb *out)
{
    embi_decoder d;
    embi_decoder_init(&d, enc, fatal, ignore_bom);
    return embi_decode(&d, p, n, true, out);
}

/* Decodes with a split at k: the first part is not a flush, the second is. */
static void decode_split(const embi_encoding *enc, const unsigned char *p, size_t n, size_t k,
                         embi_sb *out)
{
    embi_decoder d;
    embi_decoder_init(&d, enc, false, false);
    (void)embi_decode(&d, p, k, false, out);
    (void)embi_decode(&d, p + k, n - k, true, out);
}

static bool sb_is(const embi_sb *s, const char *want)
{
    return s->len == strlen(want) && (s->len == 0 || memcmp(s->data, want, s->len) == 0);
}

/* Every split point gives the same text as the whole stream. */
static void check_every_split(const embi_encoding *enc, const unsigned char *p, size_t n,
                              const char *want)
{
    embi_sb whole = {nullptr, 0, 0};
    EMB_CHECK(decode_all(enc, p, n, false, false, &whole));
    EMB_CHECK(sb_is(&whole, want));
    for (size_t k = 0; k <= n; k++) {
        embi_sb parts = {nullptr, 0, 0};
        decode_split(enc, p, n, k, &parts);
        if (!sb_is(&parts, want)) {
            fprintf(stderr, "  %s split at %zu differs\n", enc->name, k);
        }
        EMB_CHECK(sb_is(&parts, want));
        embi_sb_free(&parts);
    }
    embi_sb_free(&whole);
}

EMB_TEST(encoding_utf8_streams_at_every_split)
{
    static const unsigned char text[] = "a\xE2\x82\xAC\xF0\x9D\x84\x9E" "z";
    static const char want[] = "a\xE2\x82\xAC\xF0\x9D\x84\x9E" "z";
    check_every_split(embi_encoding_named("utf-8"), text, sizeof text - 1, want);
}

EMB_TEST(encoding_utf8_bom_and_ignore_bom)
{
    static const unsigned char bom[] = {0xEF, 0xBB, 0xBF, 'h', 'i'};
    const embi_encoding *u8 = embi_encoding_named("utf-8");
    check_every_split(u8, bom, sizeof bom, "hi");

    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(embi_decode(&(embi_decoder){.enc = u8, .ignore_bom = true, .lead_byte = -1}, bom,
                          sizeof bom, true, &out));
    EMB_CHECK(sb_is(&out, "\xEF\xBB\xBFhi"));
    embi_sb_free(&out);

    /* A short prefix of a BOM at the end is data, not a BOM. */
    static const unsigned char partial[] = {0xEF, 0xBB};
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(decode_all(u8, partial, sizeof partial, false, false, &out));
    EMB_CHECK(out.len == 3 && (unsigned char)out.data[0] == 0xEF);
    embi_sb_free(&out);
}

EMB_TEST(encoding_utf8_errors_follow_the_standard)
{
    const embi_encoding *u8 = embi_encoding_named("utf-8");
    embi_sb out = {nullptr, 0, 0};
    /* A stray continuation byte is one error. */
    static const unsigned char stray[] = {'a', 0xFF, 'b'};
    EMB_CHECK(decode_all(u8, stray, sizeof stray, false, false, &out));
    EMB_CHECK(sb_is(&out, "a\xEF\xBF\xBD" "b"));
    embi_sb_free(&out);

    /* A truncated sequence at the end is one error. */
    static const unsigned char trunc[] = {'a', 0xE2, 0x82};
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(decode_all(u8, trunc, sizeof trunc, false, false, &out));
    EMB_CHECK(sb_is(&out, "a\xEF\xBF\xBD"));
    embi_sb_free(&out);

    /* An overlong lead C0 is an error for the lead, then AF is an error of its own. */
    static const unsigned char overlong[] = {0xC0, 0xAF};
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(decode_all(u8, overlong, sizeof overlong, false, false, &out));
    EMB_CHECK(sb_is(&out, "\xEF\xBF\xBD\xEF\xBF\xBD"));
    embi_sb_free(&out);

    /* A surrogate in UTF-8 is three errors (ED's upper bound is 9F). */
    static const unsigned char surrogate[] = {0xED, 0xA0, 0x80};
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(decode_all(u8, surrogate, sizeof surrogate, false, false, &out));
    EMB_CHECK(sb_is(&out, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
    embi_sb_free(&out);

    /* Fatal mode stops at the first error and reports it. */
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!decode_all(u8, stray, sizeof stray, true, false, &out));
    EMB_CHECK(sb_is(&out, "a"));
    embi_sb_free(&out);
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!decode_all(u8, trunc, sizeof trunc, true, false, &out));
    embi_sb_free(&out);
}

EMB_TEST(encoding_utf16_streams_and_errors)
{
    /* FF FE (LE BOM), 'a', U+20AC, U+1D11E as a surrogate pair. */
    static const unsigned char le[] = {0xFF, 0xFE, 'a', 0x00, 0xAC, 0x20, 0x34, 0xD8, 0x1E, 0xDD};
    const embi_encoding *u16le = embi_encoding_named("utf-16le");
    check_every_split(u16le, le, sizeof le, "a\xE2\x82\xAC\xF0\x9D\x84\x9E");

    /* The same text big-endian, with the BE BOM. */
    static const unsigned char be[] = {0xFE, 0xFF, 0x00, 'a', 0x20, 0xAC, 0xD8, 0x34, 0xDD, 0x1E};
    const embi_encoding *u16be = embi_encoding_named("utf-16be");
    check_every_split(u16be, be, sizeof be, "a\xE2\x82\xAC\xF0\x9D\x84\x9E");

    /* An unpaired high surrogate is an error, and the next unit decodes normally. */
    static const unsigned char lone[] = {0x00, 0xD8, 'a', 0x00};
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(decode_all(u16le, lone, sizeof lone, false, false, &out));
    EMB_CHECK(sb_is(&out, "\xEF\xBF\xBD" "a"));
    embi_sb_free(&out);

    /* A dangling odd byte at the end is one error. */
    static const unsigned char odd[] = {'a', 0x00, 'b'};
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(decode_all(u16le, odd, sizeof odd, false, false, &out));
    EMB_CHECK(sb_is(&out, "a\xEF\xBF\xBD"));
    embi_sb_free(&out);
}

EMB_TEST(encoding_utf16_end_of_stream_is_one_error)
{
    static const unsigned char b[] = {0xD8, 0x00, 0x00};
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(decode_all(embi_encoding_named("utf-16be"), b, sizeof b, false, false, &out));
    EMB_CHECK(sb_is(&out, "�"));
    embi_sb_free(&out);
}

EMB_TEST(encoding_single_byte_index_hole_is_an_error)
{
    static const unsigned char hole[] = {0xA5};
    const embi_encoding *l3 = embi_encoding_named("iso-8859-3");
    EMB_REQUIRE(l3 != nullptr);
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(decode_all(l3, hole, sizeof hole, false, false, &out));
    EMB_CHECK(sb_is(&out, "�"));
    embi_sb_free(&out);
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!decode_all(l3, hole, sizeof hole, true, false, &out));
    embi_sb_free(&out);
}

EMB_TEST(encoding_single_byte_split_and_user_defined)
{
    static const unsigned char bytes[] = {'x', 0x80, 0xE9, 'y'};
    const embi_encoding *w1252 = embi_encoding_named("windows-1252");
    check_every_split(w1252, bytes, sizeof bytes, "x\xE2\x82\xAC\xC3\xA9y");

    /* x-user-defined maps 0x80..0xFF to U+F780..U+F7FF. */
    static const unsigned char ud[] = {0x80, 0xFF};
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(decode_all(embi_encoding_named("x-user-defined"), ud, sizeof ud, false, false, &out));
    EMB_CHECK(sb_is(&out, "\xEF\x9E\x80\xEF\x9F\xBF"));
    embi_sb_free(&out);
}

EMB_TEST(encoding_replacement_reports_once)
{
    static const unsigned char bytes[] = {'a', 'b', 'c'};
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(decode_all(embi_encoding_named("replacement"), bytes, sizeof bytes, false, false, &out));
    EMB_CHECK(sb_is(&out, "\xEF\xBF\xBD"));
    embi_sb_free(&out);

    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(!decode_all(embi_encoding_named("replacement"), bytes, sizeof bytes, true, false, &out));
    embi_sb_free(&out);
}

EMB_TEST(encoding_flush_resets_the_decoder)
{
    const embi_encoding *u8 = embi_encoding_named("utf-8");
    embi_decoder d;
    embi_decoder_init(&d, u8, true, false);
    embi_sb out = {nullptr, 0, 0};
    EMB_CHECK(!embi_decode(&d, (const unsigned char *)"\xFF", 1, false, &out));
    EMB_CHECK(!embi_decode(&d, (const unsigned char *)"a", 1, false, &out)); /* sticky until flush */
    EMB_CHECK(!embi_decode(&d, nullptr, 0, true, &out));
    /* The flush call itself still reports the failure, and then the decoder is usable again. */
    out.len = 0;
    EMB_CHECK(embi_decode(&d, (const unsigned char *)"ok", 2, true, &out));
    EMB_CHECK(out.len == 2 && memcmp(out.data, "ok", 2) == 0);
    embi_sb_free(&out);

    const embi_encoding *rep = embi_encoding_named("replacement");
    embi_decoder_init(&d, rep, false, false);
    out = (embi_sb){nullptr, 0, 0};
    EMB_CHECK(embi_decode(&d, (const unsigned char *)"x", 1, true, &out));
    size_t first = out.len;
    EMB_CHECK(embi_decode(&d, (const unsigned char *)"x", 1, true, &out));
    EMB_CHECK(first == 3 && out.len == 6); /* one U+FFFD per stream */
    embi_sb_free(&out);
}

EMB_TEST(encoding_decoder_allocations_balance)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 50; i++) {
        embi_sb out = {nullptr, 0, 0};
        static const unsigned char bytes[] = {'a', 0xE2, 0x82, 0xAC, 0xFF};
        (void)decode_all(embi_encoding_named("utf-8"), bytes, sizeof bytes, false, false, &out);
        embi_sb_free(&out);
    }
    EMB_CHECK(embi_mem_live() == before);
}

LOADER_TEST_MAIN()
