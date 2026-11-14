#include "emb_test.h"

#include "jslex/jslex.h"

#include <string.h>

#define SCRATCH_UNITS 4096u

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

static bool cooked_eq(const uint16_t *want, uint32_t n)
{
    if (tok.cooked == nullptr || tok.cooked_len != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (tok.cooked[i] != want[i]) {
            return false;
        }
    }
    return true;
}

static bool cooked_ascii(const char *s)
{
    uint16_t buf[64];
    uint32_t n = (uint32_t)strlen(s);
    if (n > 64) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (uint16_t)(unsigned char)s[i];
    }
    return cooked_eq(buf, n);
}

static bool span_is(const char *src, uint32_t from, uint32_t to, const char *want)
{
    uint32_t n = (uint32_t)strlen(want);
    return to - from == n && memcmp(src + from, want, n) == 0;
}

EMB_TEST(string_simple_escapes)
{
    open_src("'a\\nb'", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_STRING);
    const uint16_t want[] = {'a', 10, 'b'};
    EMB_CHECK(cooked_eq(want, 3));
    EMB_CHECK_EQ_INT(tok.flags & EMBI_JST_F_LEGACY, 0);
}

EMB_TEST(string_hex_unicode_and_astral_escapes)
{
    open_src("\"\\x41\\u0042\\u{43}\\u{1F600}\"", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_STRING);
    const uint16_t want[] = {'A', 'B', 'C', 0xD83D, 0xDE00};
    EMB_CHECK(cooked_eq(want, 5));
}

EMB_TEST(string_lone_surrogate_escape)
{
    open_src("'\\uD800'", 0);
    next_tok();
    const uint16_t want[] = {0xD800};
    EMB_CHECK(cooked_eq(want, 1));
}

EMB_TEST(string_legacy_octal_and_nonoctal_escapes)
{
    open_src("'\\0'", 0);
    next_tok();
    const uint16_t zero[] = {0};
    EMB_CHECK(cooked_eq(zero, 1));
    EMB_CHECK_EQ_INT(tok.flags & EMBI_JST_F_LEGACY, 0);

    open_src("'\\01'", 0);
    next_tok();
    const uint16_t one[] = {1};
    EMB_CHECK(cooked_eq(one, 1));
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);

    open_src("'\\8'", 0);
    next_tok();
    const uint16_t eight[] = {'8'};
    EMB_CHECK(cooked_eq(eight, 1));
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);

    open_src("'\\08'", 0);
    next_tok();
    const uint16_t zero_eight[] = {0, '8'};
    EMB_CHECK(cooked_eq(zero_eight, 2));
    EMB_CHECK(tok.flags & EMBI_JST_F_LEGACY);

    open_src("'\\377'", 0);
    next_tok();
    const uint16_t ff[] = {255};
    EMB_CHECK(cooked_eq(ff, 1));
}

EMB_TEST(string_line_continuation)
{
    open_src("'a\\\nb'", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_STRING);
    EMB_CHECK(cooked_ascii("ab"));

    open_src("'a\\\r\nb'", 0);
    next_tok();
    EMB_CHECK(cooked_ascii("ab"));
}

EMB_TEST(string_errors)
{
    const char *bad[] = {
        "'abc",         /* unterminated */
        "'a\nb'",       /* raw line feed */
        "'\\xZZ'",      /* bad hex escape */
        "'\\u{110000}'", /* code point too large */
        "'\\u12'",      /* short unicode escape */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        open_src(bad[i], 0);
        next_tok();
        EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
    }
}

EMB_TEST(template_cooked_and_raw_normalize_crlf)
{
    const char *src = "`a\r\nb`";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_NOSUB);
    const uint16_t want[] = {'a', 10, 'b'};
    EMB_CHECK(cooked_eq(want, 3));

    uint16_t raw[16];
    uint32_t n = embi_jslex_raw16(&lx, &tok, raw, 16);
    EMB_CHECK_EQ_INT(n, 3);
    EMB_CHECK(raw[0] == 'a' && raw[1] == 10 && raw[2] == 'b');
}

EMB_TEST(template_raw_keeps_backslashes_and_invalid_escape_is_flagged)
{
    const char *src = "`\\unicode`";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_NOSUB);
    EMB_CHECK(tok.flags & EMBI_JST_F_BAD_ESCAPE);
    EMB_CHECK(tok.cooked == nullptr);
    uint16_t raw[16];
    uint32_t n = embi_jslex_raw16(&lx, &tok, raw, 16);
    EMB_CHECK_EQ_INT(n, 8);
    EMB_CHECK(raw[0] == '\\' && raw[1] == 'u' && raw[7] == 'e');
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_EOF);

    open_src("`\\01`", 0);
    next_tok();
    EMB_CHECK(tok.flags & EMBI_JST_F_BAD_ESCAPE);

    open_src("`\\0`", 0);
    next_tok();
    EMB_CHECK(!(tok.flags & EMBI_JST_F_BAD_ESCAPE));
    const uint16_t zero[] = {0};
    EMB_CHECK(cooked_eq(zero, 1));
}

EMB_TEST(template_head_and_middle_with_escape_before_substitution)
{
    const char *src = "`\\u0041${x}\\n${y}`";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_HEAD);
    EMB_CHECK(cooked_ascii("A"));
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_RBRACE);
    embi_jslex_rescan_template(&lx, &tok);
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_MIDDLE);
    const uint16_t nl[] = {10};
    EMB_CHECK(cooked_eq(nl, 1));
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_RBRACE);
    embi_jslex_rescan_template(&lx, &tok);
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_TAIL);
    EMB_CHECK_EQ_INT(tok.cooked_len, 0);
}

EMB_TEST(identifier_unicode_escapes)
{
    open_src("\\u0061bc", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK(tok.flags & EMBI_JST_F_ESCAPED);
    EMB_CHECK(cooked_ascii("abc"));
    EMB_CHECK_EQ_INT(tok.kw, EMBI_JSKW_NONE);

    open_src("\\u0069f", 0);
    next_tok();
    EMB_CHECK(tok.flags & EMBI_JST_F_ESCAPED);
    EMB_CHECK_EQ_INT(tok.kw, EMBI_JSKW_if);

    open_src("a\\u{62}", 0);
    next_tok();
    EMB_CHECK(cooked_ascii("ab"));

    open_src("\\u0030x", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
}

EMB_TEST(regex_body_and_flags)
{
    const char *src = "/a[/]b\\/c/gi";
    open_src(src, 0);
    lx.regex_ok = true;
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_REGEX);
    EMB_CHECK(span_is(src, tok.start + 1, tok.aux_end, "a[/]b\\/c"));
    EMB_CHECK(span_is(src, tok.aux_end + 1, tok.end, "gi"));
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_EOF);

    open_src("/x/dgimsvy", 0);
    lx.regex_ok = true;
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_REGEX);
    EMB_CHECK(span_is("/x/dgimsvy", tok.aux_end + 1, tok.end, "dgimsvy"));
}

EMB_TEST(regex_flag_errors)
{
    const char *bad[] = {
        "/a/gg",     /* duplicate flag */
        "/a/uv",     /* u and v */
        "/a/x",      /* unknown flag */
        "/[/",       /* unterminated class */
        "/a\n/",     /* line terminator in body */
        "/a/\\u0067", /* escape in flags */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        open_src(bad[i], 0);
        lx.regex_ok = true;
        next_tok();
        EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
    }
}

EMB_TEST_MAIN()
