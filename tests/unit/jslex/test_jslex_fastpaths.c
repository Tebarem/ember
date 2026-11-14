#include "emb_test.h"

#include "jslex/jslex.h"

#include <string.h>

/* FP1a: the lexer fast paths (the keyword switch, the plain-ASCII name path, the ASCII string path
 * and the skim scan of the parser's lookahead) must agree with the general scanner. */

#define SCRATCH_UNITS 4096u

static uint16_t scratch_a[SCRATCH_UNITS];
static uint16_t scratch_b[SCRATCH_UNITS];

static bool is_keyword_text(const char *s, size_t n);

typedef struct kwrow {
    const char *name;
    embi_jskw kw;
} kwrow;

static const kwrow kw_all[] = {
#define KW_ROW(k) {#k, EMBI_JSKW_##k},
    EMBI_JSKW_LIST(KW_ROW)
#undef KW_ROW
};

static bool is_keyword_text(const char *s, size_t n)
{
    for (size_t j = 0; j < sizeof kw_all / sizeof kw_all[0]; j++) {
        if (strlen(kw_all[j].name) == n && memcmp(kw_all[j].name, s, n) == 0) {
            return true;
        }
    }
    return false;
}

EMB_TEST(every_keyword_is_recognized)
{
    for (size_t i = 0; i < sizeof kw_all / sizeof kw_all[0]; i++) {
        const char *w = kw_all[i].name;
        size_t n = strlen(w);
        EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)w, (uint32_t)n), kw_all[i].kw);
    }
}

EMB_TEST(near_misses_are_not_keywords)
{
    for (size_t i = 0; i < sizeof kw_all / sizeof kw_all[0]; i++) {
        const char *w = kw_all[i].name;
        size_t n = strlen(w);
        char buf[32];
        /* an extension */
        memcpy(buf, w, n);
        buf[n] = 'x';
        EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)buf, (uint32_t)n + 1u), EMBI_JSKW_NONE);
        /* a prefix */
        if (n > 2) {
            embi_jskw pre = embi_jslex_keyword((const uint8_t *)w, (uint32_t)n - 1u);
            EMB_CHECK(is_keyword_text(w, n - 1u) || pre == EMBI_JSKW_NONE);
        }
        /* the first letter in upper case */
        memcpy(buf, w, n);
        buf[0] = (char)(buf[0] - 32);
        EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)buf, (uint32_t)n), EMBI_JSKW_NONE);
        /* the last letter changed */
        memcpy(buf, w, n);
        buf[n - 1u] = buf[n - 1u] == 'z' ? 'y' : 'z';
        embi_jskw alt = embi_jslex_keyword((const uint8_t *)buf, (uint32_t)n);
        EMB_CHECK(is_keyword_text(buf, n) || alt == EMBI_JSKW_NONE);
    }
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"a", 1), EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"abcdefghijk", 11), EMBI_JSKW_NONE);
}

static void expect_same(const char *src)
{
    embi_jslex a;
    embi_jslex b;
    embi_jslex_init(&a, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_init(&b, (const uint8_t *)src, (uint32_t)strlen(src), scratch_b, SCRATCH_UNITS, 0);
    b.skim = true;
    for (int i = 0; i < 200; i++) {
        embi_jstok ta;
        embi_jstok tb;
        embi_jslex_next(&a, &ta);
        embi_jslex_next(&b, &tb);
        EMB_CHECK_EQ_INT(ta.kind, tb.kind);
        EMB_CHECK_EQ_INT(ta.start, tb.start);
        EMB_CHECK_EQ_INT(ta.end, tb.end);
        EMB_CHECK_EQ_INT(ta.flags, tb.flags);
        EMB_CHECK_EQ_INT(ta.kw, tb.kw);
        if (ta.kind == EMBI_JST_EOF || ta.kind == EMBI_JST_ERROR) {
            break;
        }
    }
}

EMB_TEST(skim_scan_matches_normal_scan)
{
    expect_same("f(\"a\\n\\u00e9\", 'x') + `t${1}` /* c */ // d\n x.y >>>= 3.5e2 ? #p : 0x1f");
    expect_same("a = \"caf\xC3\xA9 \xF0\x9F\x98\x80\" + 'it\\'s' + \"\\x41\\101\"; b\n++c");
    expect_same("var \\u0061bc = 1; \\u{62}c(); /re[/]gex/g.test(s)");
    expect_same("'unterminated");
}

EMB_TEST(ascii_names_next_to_escapes_and_non_ascii)
{
    /* a$_9, an escaped name, a name with a non-ASCII tail, digits in the middle, '_' and '$' alone */
    const char *src = "a$_9 \\u0062c d\xC3\xA9 let0 _ $ \xC3\xA9";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.end - t.start, 4);
    EMB_CHECK_EQ_INT(t.kw, EMBI_JSKW_NONE);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK(t.flags & EMBI_JST_F_ESCAPED);
    EMB_CHECK_EQ_INT(t.cooked_len, 2);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.end - t.start, 3); /* d plus the two UTF-8 bytes of e-acute */
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kw, EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(t.end - t.start, 4);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_EOF);
}

EMB_TEST(escaped_keyword_still_reports_its_keyword)
{
    const char *src = "\\u0069f";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK(t.flags & EMBI_JST_F_ESCAPED);
    EMB_CHECK_EQ_INT(t.kw, EMBI_JSKW_if);
}

EMB_TEST(line_and_column_after_long_ascii_runs)
{
    const char *src = "abcdefghij klmnopqrst\n  uvw \xC3\xA9 x\n\ty";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.line, 1);
    EMB_CHECK_EQ_INT(t.col, 1);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.line, 1);
    EMB_CHECK_EQ_INT(t.col, 12);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.line, 2);
    EMB_CHECK_EQ_INT(t.col, 3);
    embi_jslex_next(&lx, &t); /* the e-acute is one UTF-16 unit */
    EMB_CHECK_EQ_INT(t.line, 2);
    EMB_CHECK_EQ_INT(t.col, 7);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.line, 2);
    EMB_CHECK_EQ_INT(t.col, 9);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.line, 3);
    EMB_CHECK_EQ_INT(t.col, 2);
}

/* FP2a: the line and column of every token of a long source of mixed content (printable runs of every length around a
 * multiple of eight, tabs, control bytes, newlines, CRLF, two-byte and four-byte characters) match a byte-by-byte count. */
EMB_TEST(line_and_column_word_at_a_time_matches_a_plain_count)
{
    static uint8_t src[6000];
    size_t n = 0;
    uint32_t seed = 12345;
    while (n + 40 < sizeof src) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t run = 1 + (seed >> 24) % 21; /* identifier letters */
        for (uint32_t i = 0; i < run; i++) {
            src[n++] = (uint8_t)('a' + (seed >> (i % 16)) % 26);
        }
        seed = seed * 1664525u + 1013904223u;
        switch ((seed >> 20) % 8) {
        case 0: src[n++] = '\n'; break;
        case 1: src[n++] = '\t'; break;
        case 2: src[n++] = '\r'; src[n++] = '\n'; break;
        case 3: src[n++] = 0xC3; src[n++] = 0xA9; src[n++] = ' '; break; /* e-acute, one unit */
        case 4: /* a block comment holding a character of two UTF-16 units */
            src[n++] = '/'; src[n++] = '*'; src[n++] = 0xF0; src[n++] = 0x9F; src[n++] = 0x98; src[n++] = 0x80;
            src[n++] = '*'; src[n++] = '/'; src[n++] = ' ';
            break;
        case 5: src[n++] = 0x0B; break; /* vertical tab, whitespace below 0x20 */
        default: src[n++] = ' '; break;
        }
    }
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, src, (uint32_t)n, scratch_a, SCRATCH_UNITS, 0);
    int bad = 0;
    int tokens = 0;
    for (;;) {
        embi_jslex_next(&lx, &t);
        if (t.kind == EMBI_JST_EOF || t.kind == EMBI_JST_ERROR) {
            break;
        }
        tokens++;
        /* the plain count up to t.start */
        uint32_t line = 1, col = 0;
        for (uint32_t p = 0; p < t.start;) {
            uint8_t c = src[p];
            if (c == '\n') {
                line++; col = 0; p++;
            } else if (c == '\r') {
                if (p + 1 < n && src[p + 1] == '\n') { p++; } else { line++; col = 0; p++; }
            } else if (c < 0x80) {
                col++; p++;
            } else if (c == 0xC3) {
                col++; p += 2;
            } else { /* the four-byte character: two units */
                col += 2; p += 4;
            }
        }
        if (t.line != line || t.col != col + 1) {
            bad++;
        }
    }
    EMB_CHECK(tokens > 300);
    EMB_CHECK_EQ_INT(bad, 0);
}

EMB_TEST(separators_inside_strings_and_crlf_lines)
{
    /* U+2028 (E2 80 A8) and U+2029 (E2 80 A9) are legal inside a string and end a line for the
     * position of what follows. */
    const char *src = "\"a\xE2\x80\xA8z\" b \"q\xE2\x80\xA9\" c";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_STRING);
    EMB_CHECK_EQ_INT(t.line, 1);
    embi_jslex_next(&lx, &t); /* b, after the first separator */
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.line, 2);
    EMB_CHECK_EQ_INT(t.col, 4);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_STRING);
    EMB_CHECK_EQ_INT(t.line, 2);
    embi_jslex_next(&lx, &t); /* c, after the second */
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.line, 3);
    EMB_CHECK_EQ_INT(t.col, 3);
    expect_same(src);

    /* CRLF counts as one line break; a lone CR and a lone LF each count one. */
    const char *crlf = "a\r\n  b\r\r\nc\n d";
    embi_jslex_init(&lx, (const uint8_t *)crlf, (uint32_t)strlen(crlf), scratch_a, SCRATCH_UNITS, 0);
    uint32_t want_line[] = {1, 2, 4, 5};
    uint32_t want_col[] = {1, 3, 1, 2};
    for (int i = 0; i < 4; i++) {
        embi_jslex_next(&lx, &t);
        EMB_CHECK_EQ_INT(t.line, want_line[i]);
        EMB_CHECK_EQ_INT(t.col, want_col[i]);
    }
    expect_same(crlf);
}

EMB_TEST(surrogate_pairs_next_to_ascii_runs)
{
    /* U+1F600 is one code point and two UTF-16 units: the column after it moves by two. */
    const char *src = "\"abc\xF0\x9F\x98\x80xyz\" id\xF0\x90\x90\x80 next";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_STRING);
    EMB_CHECK_EQ_INT(t.cooked_len, 8); /* abc + pair + xyz */
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.col, 12); /* 1 + 1 quote + 8 units + 1 quote + 1 space */
    EMB_CHECK_EQ_INT(t.end - t.start, 6); /* id and U+10400 (4 bytes) */
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.col, 17); /* col 12 + id (2) + U+10400 (2) + a space */
    expect_same(src);
}

EMB_TEST(private_names_with_non_ascii_and_escapes)
{
    const char *src = "#a #caf\xC3\xA9 #\\u0062c #\xF0\x90\x90\x80 #x1";
    embi_jslex lx;
    embi_jstok t;
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    uint32_t want_len[] = {2, 6, 8, 5, 3};
    for (int i = 0; i < 5; i++) {
        embi_jslex_next(&lx, &t);
        EMB_CHECK_EQ_INT(t.kind, EMBI_JST_PRIVATE_NAME);
        EMB_CHECK_EQ_INT(t.end - t.start, want_len[i]);
    }
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_EOF);
    expect_same(src);
}

EMB_TEST(numeric_separators_next_to_identifiers)
{
    embi_jslex lx;
    embi_jstok t;
    const char *src = "1_000 a_1 _1 1_0n 0x1_f x";
    embi_jslex_init(&lx, (const uint8_t *)src, (uint32_t)strlen(src), scratch_a, SCRATCH_UNITS, 0);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NUMBER);
    EMB_CHECK_EQ_INT(t.end - t.start, 5);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(t.end - t.start, 3);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME); /* _1 is an identifier, not a number */
    EMB_CHECK_EQ_INT(t.end - t.start, 2);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_BIGINT);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NUMBER);
    embi_jslex_next(&lx, &t);
    EMB_CHECK_EQ_INT(t.kind, EMBI_JST_NAME);
    expect_same(src);
    /* a separator that is not between digits is an error, and an identifier cannot follow a number */
    const char *bad[] = {"1__0", "1_", "1_a"};
    for (int i = 0; i < 3; i++) {
        embi_jslex_init(&lx, (const uint8_t *)bad[i], (uint32_t)strlen(bad[i]), scratch_a, SCRATCH_UNITS, 0);
        embi_jslex_next(&lx, &t);
        EMB_CHECK_EQ_INT(t.kind, EMBI_JST_ERROR);
    }
}

EMB_TEST_MAIN()
