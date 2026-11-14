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

static bool span_is(const char *src, uint32_t from, uint32_t to, const char *want)
{
    uint32_t n = (uint32_t)strlen(want);
    return to - from == n && memcmp(src + from, want, n) == 0;
}

EMB_TEST(punctuator_kinds)
{
    static const embi_jstok_kind want[] = {
        EMBI_JST_LBRACE,   EMBI_JST_RBRACE,   EMBI_JST_LPAREN,   EMBI_JST_RPAREN,
        EMBI_JST_LBRACK,   EMBI_JST_RBRACK,   EMBI_JST_DOT,      EMBI_JST_ELLIPSIS,
        EMBI_JST_SEMI,     EMBI_JST_COMMA,    EMBI_JST_LT,       EMBI_JST_GT,
        EMBI_JST_LE,       EMBI_JST_GE,       EMBI_JST_EQ,       EMBI_JST_NE,
        EMBI_JST_SEQ,      EMBI_JST_SNE,      EMBI_JST_ADD,      EMBI_JST_SUB,
        EMBI_JST_MUL,      EMBI_JST_MOD,      EMBI_JST_EXP,      EMBI_JST_INC,
        EMBI_JST_DEC,      EMBI_JST_SHL,      EMBI_JST_SAR,      EMBI_JST_SHR,
        EMBI_JST_BAND,     EMBI_JST_BOR,      EMBI_JST_BXOR,     EMBI_JST_NOT,
        EMBI_JST_BNOT,     EMBI_JST_LAND,     EMBI_JST_LOR,      EMBI_JST_NULLISH,
        EMBI_JST_QUESTION, EMBI_JST_OPTCHAIN, EMBI_JST_COLON,    EMBI_JST_ASSIGN,
        EMBI_JST_ADD_EQ,   EMBI_JST_SUB_EQ,   EMBI_JST_MUL_EQ,   EMBI_JST_MOD_EQ,
        EMBI_JST_EXP_EQ,   EMBI_JST_SHL_EQ,   EMBI_JST_SAR_EQ,   EMBI_JST_SHR_EQ,
        EMBI_JST_BAND_EQ,  EMBI_JST_BOR_EQ,   EMBI_JST_BXOR_EQ,  EMBI_JST_LAND_EQ,
        EMBI_JST_LOR_EQ,   EMBI_JST_NULLISH_EQ, EMBI_JST_ARROW,  EMBI_JST_DIV,
        EMBI_JST_DIV_EQ,
    };
    open_src("{ } ( ) [ ] . ... ; , < > <= >= == != === !== + - * % ** ++ -- << >> >>> & | "
             "^ ! ~ && || ?? ? ?. : = += -= *= %= **= <<= >>= >>>= &= |= ^= &&= ||= ?\?= => "
             "/ /=",
             0);
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        next_tok();
        EMB_CHECK_EQ_INT(tok.kind, want[i]);
    }
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_EOF);
}

EMB_TEST(optional_chain_before_digit_is_question)
{
    open_src("a?.5:1", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_QUESTION);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NUMBER);
    EMB_CHECK(tok.num == 0.5);
}

EMB_TEST(newline_flags_and_asi_input)
{
    open_src("a\nb /* x */ c /*\n*/ d e", 0);
    next_tok();
    EMB_CHECK(!(tok.flags & EMBI_JST_F_NL_BEFORE));
    next_tok();
    EMB_CHECK(tok.flags & EMBI_JST_F_NL_BEFORE);
    next_tok();
    EMB_CHECK(!(tok.flags & EMBI_JST_F_NL_BEFORE));
    next_tok();
    EMB_CHECK(tok.flags & EMBI_JST_F_NL_BEFORE);
    next_tok();
    EMB_CHECK(!(tok.flags & EMBI_JST_F_NL_BEFORE));
}

EMB_TEST(unterminated_block_comment_is_error)
{
    open_src("a /* never closed", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
}

EMB_TEST(html_like_comments)
{
    open_src("a <!-- x\nb", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK(span_is("a <!-- x\nb", tok.start, tok.end, "b"));
    EMB_CHECK(tok.flags & EMBI_JST_F_NL_BEFORE);

    open_src("x\n--> y\nz", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK(span_is("x\n--> y\nz", tok.start, tok.end, "z"));

    open_src("x\n-->", EMBI_JSLEX_MODULE);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_DEC);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_GT);
}

EMB_TEST(hashbang_comment)
{
    const char *src = "#!/usr/bin/env x\nfoo";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK(span_is(src, tok.start, tok.end, "foo"));
    EMB_CHECK_EQ_INT(tok.line, 2);
    EMB_CHECK_EQ_INT(tok.col, 1);
}

EMB_TEST(regex_versus_division)
{
    open_src("a / b / c", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_DIV);

    const char *src = "x = /a+/g";
    open_src(src, 0);
    next_tok();
    next_tok();
    lx.regex_ok = true;
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_REGEX);
    EMB_CHECK(span_is(src, tok.start + 1, tok.aux_end, "a+"));
    EMB_CHECK(span_is(src, tok.aux_end + 1, tok.end, "g"));
}

EMB_TEST(rescan_slash_as_regex)
{
    const char *src = "/x/i";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_DIV);
    embi_jslex_rescan_regex(&lx, &tok);
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_REGEX);
    EMB_CHECK(span_is(src, tok.aux_end + 1, tok.end, "i"));
    EMB_CHECK_EQ_INT(tok.start, 0);
}

EMB_TEST(template_parts)
{
    const char *src = "`a${x}b`";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_HEAD);
    EMB_CHECK(tok.cooked_len == 1 && tok.cooked[0] == 'a');
    EMB_CHECK(span_is(src, tok.start + 1, tok.aux_end, "a"));
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_RBRACE);
    embi_jslex_rescan_template(&lx, &tok);
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_TAIL);
    EMB_CHECK(tok.cooked_len == 1 && tok.cooked[0] == 'b');
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_EOF);

    open_src("``", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_TPL_NOSUB);
    EMB_CHECK_EQ_INT(tok.cooked_len, 0);
}

EMB_TEST(line_and_column_positions)
{
    open_src("a\r\nb", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.line, 2);
    EMB_CHECK_EQ_INT(tok.col, 1);

    /* U+00E9 (2 bytes) is an identifier, then U+2028 (3 bytes) starts a new line. */
    open_src("\xC3\xA9\xE2\x80\xA8" "b", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    next_tok();
    EMB_CHECK_EQ_INT(tok.line, 2);
    EMB_CHECK_EQ_INT(tok.col, 1);

    /* U+00E9 counts as one column. */
    open_src("\xC3\xA9 b", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.col, 3);

    /* U+1F600 is two UTF-16 units. A comment holding it, then b, puts b at column 7. */
    open_src("/*\xF0\x9F\x98\x80" "*/b", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.line, 1);
    EMB_CHECK_EQ_INT(tok.col, 7);

    open_src("a /*\n\n*/ b", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.line, 3);
    EMB_CHECK_EQ_INT(tok.col, 4);
}

EMB_TEST(private_names_and_keywords)
{
    const char *src = "#x function Function";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_PRIVATE_NAME);
    EMB_CHECK(span_is(src, tok.start + 1, tok.end, "x"));
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(tok.kw, EMBI_JSKW_function);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kw, EMBI_JSKW_NONE);
}

EMB_TEST(invalid_character_is_sticky_error)
{
    open_src("a @ b", 0);
    next_tok();
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
    EMB_CHECK_EQ_INT(tok.start, 2);
    EMB_CHECK(lx.err != nullptr);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
}

EMB_TEST(empty_input_is_eof)
{
    open_src("", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_EOF);
    EMB_CHECK_EQ_INT(tok.start, 0);
}

EMB_TEST(scratch_too_small_is_error)
{
    static uint16_t tiny[2];
    embi_jslex_init(&lx, (const uint8_t *)"'abcdef'", 8, tiny, 2, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
}

EMB_TEST_MAIN()
