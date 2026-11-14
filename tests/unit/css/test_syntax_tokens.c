/*
 * Tokenizer unit tests: preprocessing, escapes, names, numbers, strings, urls, unicode-range and
 * the operators. The fixture suite covers most of this against the reference data; these tests pin
 * down the details next to the code that implements them.
 */
#include "emb_test.h"

#include "css/css.h"

#include <string.h>

static embi_css_tokens tokens_of(embi_arena *a, const char *s)
{
    return embi_css_tokenize(a, (embi_str){s, (uint32_t)strlen(s)});
}

/* Compares a decoded slice with a C string. */
static bool is_str(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

/* Tokenizes s, skipping whitespace tokens. */
static embi_css_tokens significant(embi_arena *a, const char *s, embi_css_token *buf, uint32_t cap)
{
    embi_css_tokens all = tokens_of(a, s);
    uint32_t n = 0;
    for (uint32_t k = 0; k < all.n && n < cap; k++) {
        if (all.p[k].type != EMBI_CSS_WS) {
            buf[n++] = all.p[k];
        }
    }
    embi_css_tokens r = {buf, n};
    return r;
}

EMB_TEST(css_tok_ident_and_escapes)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[8];

    /* Escape of a hex value followed by one whitespace, then plain characters. */
    embi_css_tokens t = significant(&a, "\\31 23", buf, 8);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_IDENT);
    EMB_CHECK(is_str(t.p[0].value, "123"));

    /* Escaped whitespace-like character and a NUL escape, which becomes U+FFFD. */
    t = significant(&a, "a\\0 b", buf, 8);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(is_str(t.p[0].value, "a\xEF\xBF\xBD" "b"));

    /* A backslash before a newline is not an escape: delim, then whitespace. */
    t = significant(&a, "\\\nx", buf, 8);
    EMB_CHECK(t.n >= 1 && t.p[0].type == EMBI_CSS_DELIM && t.p[0].cp == '\\');

    /* Dash rules: a lone "-" is a delim, "--x" is an ident, "-1" is a number. */
    t = significant(&a, "- --x -1", buf, 8);
    EMB_REQUIRE(t.n == 3);
    EMB_CHECK(t.p[0].type == EMBI_CSS_DELIM && t.p[0].cp == '-');
    t = significant(&a, "\\-", buf, 8);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_IDENT && is_str(t.p[0].value, "-"));
    t = significant(&a, "- --x -1", buf, 8);
    EMB_CHECK(t.p[1].type == EMBI_CSS_IDENT && is_str(t.p[1].value, "--x"));
    EMB_CHECK(t.p[2].type == EMBI_CSS_NUMBER && t.p[2].num == -1.0);

    embi_arena_free(&a);
}

EMB_TEST(css_tok_preprocessing)
{
    embi_arena a;
    embi_arena_init(&a, 4096);

    /* CRLF and CR become a single newline, so the whitespace token count is the same. */
    embi_css_tokens t = tokens_of(&a, "a\r\nb");
    EMB_CHECK(t.n == 3);
    EMB_CHECK(t.p[1].type == EMBI_CSS_WS);
    EMB_CHECK(t.p[1].end - t.p[1].start == 1);

    /* A raw NUL in an ident becomes U+FFFD. */
    t = embi_css_tokenize(&a, (embi_str){"a\0b", 3});
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(is_str(t.p[0].value, "a\xEF\xBF\xBD" "b"));

    /* Invalid UTF-8 becomes U+FFFD. It is at or above U+0080, so it is a name code point. */
    t = tokens_of(&a, "\xFF");
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_IDENT && is_str(t.p[0].value, "\xEF\xBF\xBD"));

    /* An encoded surrogate (ED A0 80) is one replacement character, not three. */
    t = tokens_of(&a, "\xED\xA0\x80");
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_IDENT && is_str(t.p[0].value, "\xEF\xBF\xBD"));

    embi_arena_free(&a);
}

EMB_TEST(css_tok_hash_and_at)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[8];

    embi_css_tokens t = significant(&a, "#fff #1ab @media @1", buf, 8);
    EMB_REQUIRE(t.n == 5);
    EMB_CHECK(t.p[0].type == EMBI_CSS_HASH && t.p[0].flag && is_str(t.p[0].value, "fff"));
    EMB_CHECK(t.p[1].type == EMBI_CSS_HASH && !t.p[1].flag && is_str(t.p[1].value, "1ab"));
    EMB_CHECK(t.p[2].type == EMBI_CSS_AT_KEYWORD && is_str(t.p[2].value, "media"));
    EMB_CHECK(t.p[3].type == EMBI_CSS_DELIM && t.p[3].cp == '@');

    embi_arena_free(&a);
}

EMB_TEST(css_tok_numbers)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[16];

    embi_css_tokens t = significant(&a, "+.5e1 10px 3% 1.5 2e+2 -0", buf, 16);
    EMB_REQUIRE(t.n == 6);
    EMB_CHECK(t.p[0].type == EMBI_CSS_NUMBER && t.p[0].num == 5.0 && !t.p[0].flag);
    EMB_CHECK(is_str(t.p[0].repr, "+.5e1"));
    EMB_CHECK(t.p[1].type == EMBI_CSS_DIMENSION && t.p[1].flag && t.p[1].num == 10.0);
    EMB_CHECK(is_str(t.p[1].value, "px"));
    EMB_CHECK(t.p[2].type == EMBI_CSS_PERCENTAGE && t.p[2].num == 3.0);
    EMB_CHECK(t.p[3].type == EMBI_CSS_NUMBER && t.p[3].num == 1.5 && !t.p[3].flag);
    EMB_CHECK(t.p[4].type == EMBI_CSS_NUMBER && t.p[4].num == 200.0);
    EMB_CHECK(t.p[5].type == EMBI_CSS_NUMBER && t.p[5].num == 0.0);

    /* An escaped unit: "3\65-2" is a dimension with unit "e-2". */
    t = significant(&a, "3\\65-2", buf, 16);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_DIMENSION && is_str(t.p[0].value, "e-2"));

    /* "3." is a number then a delim, because the decimal point needs a digit after it. */
    t = significant(&a, "3.", buf, 16);
    EMB_CHECK(t.n == 2 && t.p[0].type == EMBI_CSS_NUMBER && t.p[1].type == EMBI_CSS_DELIM);

    embi_arena_free(&a);
}

EMB_TEST(css_tok_strings)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[16];

    /* Escaped newline is a line continuation and disappears from the value. */
    embi_css_tokens t = significant(&a, "'a\\\nb' \"\\41\"", buf, 16);
    EMB_REQUIRE(t.n == 2);
    EMB_CHECK(t.p[0].type == EMBI_CSS_STRING && is_str(t.p[0].value, "ab"));
    EMB_CHECK(t.p[1].type == EMBI_CSS_STRING && is_str(t.p[1].value, "A"));

    /* A raw newline ends the string as bad-string and is not consumed. */
    t = significant(&a, "'a\nb'", buf, 16);
    EMB_REQUIRE(t.n >= 2);
    EMB_CHECK(t.p[0].type == EMBI_CSS_BAD_STRING);

    /* EOF inside a string: one string token, flagged. The parse error is not a token. */
    t = significant(&a, "'x", buf, 16);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_STRING && is_str(t.p[0].value, "x"));
    EMB_CHECK(t.p[0].flag);
    t = significant(&a, "'x'", buf, 16);
    EMB_CHECK(t.n == 1 && !t.p[0].flag);

    embi_arena_free(&a);
}

EMB_TEST(css_tok_urls)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[16];

    embi_css_tokens t = significant(&a, "url(a\\ b) url( 'x' ) URL(foo)", buf, 16);
    EMB_REQUIRE(t.n == 5);
    EMB_CHECK(t.p[0].type == EMBI_CSS_URL && is_str(t.p[0].value, "a b"));
    EMB_CHECK(t.p[1].type == EMBI_CSS_FUNCTION && is_str(t.p[1].value, "url"));
    EMB_CHECK(t.p[2].type == EMBI_CSS_STRING && is_str(t.p[2].value, "x"));
    EMB_CHECK(t.p[3].type == EMBI_CSS_RPAREN);
    EMB_CHECK(t.p[4].type == EMBI_CSS_URL && is_str(t.p[4].value, "foo"));

    /* A space inside an unquoted url makes it bad, and the remnants are skipped. */
    t = significant(&a, "url(a b) x", buf, 16);
    EMB_REQUIRE(t.n == 2);
    EMB_CHECK(t.p[0].type == EMBI_CSS_BAD_URL);
    EMB_CHECK(t.p[1].type == EMBI_CSS_IDENT);

    /* EOF inside an unquoted url: one url token, flagged, with the value read so far. */
    t = significant(&a, "url(ab  ", buf, 16);
    EMB_REQUIRE(t.n == 1);
    EMB_CHECK(t.p[0].type == EMBI_CSS_URL && t.p[0].flag && is_str(t.p[0].value, "ab"));

    embi_arena_free(&a);
}

EMB_TEST(css_tok_unicode_range)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[8];

    embi_css_tokens t = significant(&a, "U+0025-00FF u+4??", buf, 8);
    EMB_REQUIRE(t.n == 2);
    EMB_CHECK(t.p[0].type == EMBI_CSS_UNICODE_RANGE);
    EMB_CHECK(t.p[0].cp == 0x25 && t.p[0].cp_end == 0xFF);
    EMB_CHECK(t.p[1].type == EMBI_CSS_UNICODE_RANGE);
    EMB_CHECK(t.p[1].cp == 0x400 && t.p[1].cp_end == 0x4FF);

    /* Without hex or '?' after "U+", it is an ident. */
    t = significant(&a, "U+ x", buf, 8);
    EMB_CHECK(t.n >= 1 && t.p[0].type == EMBI_CSS_IDENT);

    embi_arena_free(&a);
}

EMB_TEST(css_tok_operators_and_comments)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_token buf[16];

    embi_css_tokens t = significant(&a, "~= |= ^= $= *= || <!-- --> /* c */ a/**/b", buf, 16);
    EMB_REQUIRE(t.n == 10);
    EMB_CHECK(t.p[0].type == EMBI_CSS_INCLUDE_MATCH);
    EMB_CHECK(t.p[1].type == EMBI_CSS_DASH_MATCH);
    EMB_CHECK(t.p[2].type == EMBI_CSS_PREFIX_MATCH);
    EMB_CHECK(t.p[3].type == EMBI_CSS_SUFFIX_MATCH);
    EMB_CHECK(t.p[4].type == EMBI_CSS_SUBSTRING_MATCH);
    EMB_CHECK(t.p[5].type == EMBI_CSS_COLUMN);
    EMB_CHECK(t.p[6].type == EMBI_CSS_CDO);
    EMB_CHECK(t.p[7].type == EMBI_CSS_CDC);
    EMB_CHECK(t.p[8].type == EMBI_CSS_IDENT && is_str(t.p[8].value, "a"));
    EMB_CHECK(t.p[9].type == EMBI_CSS_IDENT && is_str(t.p[9].value, "b"));

    /* An unterminated comment runs to EOF with no token. */
    t = significant(&a, "a /* never closed", buf, 16);
    EMB_CHECK(t.n == 1);

    embi_arena_free(&a);
}

EMB_TEST(css_tok_offsets)
{
    embi_arena a;
    embi_arena_init(&a, 4096);
    embi_css_tokens t = tokens_of(&a, "ab cd");
    EMB_REQUIRE(t.n == 3);
    EMB_CHECK(t.p[0].start == 0 && t.p[0].end == 2);
    EMB_CHECK(t.p[2].start == 3 && t.p[2].end == 5);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
