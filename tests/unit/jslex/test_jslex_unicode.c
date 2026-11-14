#include "emb_test.h"

#include "jslex/jslex.h"

#include <string.h>

#define SCRATCH_UNITS 256u

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

EMB_TEST(id_start_table_samples)
{
    EMB_CHECK(embi_jslex_is_id_start('a'));
    EMB_CHECK(embi_jslex_is_id_start('Z'));
    EMB_CHECK(!embi_jslex_is_id_start('1'));
    EMB_CHECK(!embi_jslex_is_id_start('$'));
    EMB_CHECK(!embi_jslex_is_id_start('_'));
    EMB_CHECK(!embi_jslex_is_id_start(' '));
    EMB_CHECK(embi_jslex_is_id_start(0x00E9));  /* e with acute */
    EMB_CHECK(embi_jslex_is_id_start(0x00AA));  /* feminine ordinal, Lo */
    EMB_CHECK(embi_jslex_is_id_start(0x4E00));  /* CJK ideograph */
    EMB_CHECK(embi_jslex_is_id_start(0x2118));  /* Other_ID_Start */
    EMB_CHECK(!embi_jslex_is_id_start(0x0300)); /* combining grave: continue only */
    EMB_CHECK(!embi_jslex_is_id_start(0x00B7)); /* middle dot: continue only */
    EMB_CHECK(!embi_jslex_is_id_start(0x1F600));
    EMB_CHECK(!embi_jslex_is_id_start(0x10FFFF));
}

EMB_TEST(id_continue_table_samples)
{
    EMB_CHECK(embi_jslex_is_id_continue('7'));
    EMB_CHECK(embi_jslex_is_id_continue('_'));
    EMB_CHECK(embi_jslex_is_id_continue(0x0300));
    EMB_CHECK(embi_jslex_is_id_continue(0x00B7));  /* Other_ID_Continue */
    EMB_CHECK(embi_jslex_is_id_continue(0x0660));  /* Arabic-Indic digit zero, Nd */
    EMB_CHECK(!embi_jslex_is_id_continue('-'));
    EMB_CHECK(!embi_jslex_is_id_continue(0x1F600));
    /* UCD 17 lists ZWNJ and ZWJ in ID_Continue. */
    EMB_CHECK(embi_jslex_is_id_continue(0x200C));
    EMB_CHECK(embi_jslex_is_id_continue(0x200D));
}

EMB_TEST(keyword_lookup)
{
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"function", 8), EMBI_JSKW_function);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"instanceof", 10),
                     EMBI_JSKW_instanceof);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"implements", 10),
                     EMBI_JSKW_implements);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"let", 3), EMBI_JSKW_let);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"await", 5), EMBI_JSKW_await);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"Function", 8), EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"functions", 9), EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"func", 4), EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"a", 1), EMBI_JSKW_NONE);
    EMB_CHECK_EQ_INT(embi_jslex_keyword((const uint8_t *)"", 0), EMBI_JSKW_NONE);
}

EMB_TEST(non_ascii_identifier_token)
{
    const char *src = "caf\xC3\xA9 x";
    open_src(src, 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK_EQ_INT(tok.end, 5); /* the e with acute is two bytes */
    EMB_CHECK(!(tok.flags & EMBI_JST_F_ESCAPED));
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
}

EMB_TEST(non_identifier_start_is_error)
{
    /* U+0300 (combining grave) cannot start an identifier. */
    open_src("\xCC\x80x", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_ERROR);
}

EMB_TEST(zero_width_joiner_in_identifier_via_escape)
{
    open_src("a\\u200Db", 0);
    next_tok();
    EMB_CHECK_EQ_INT(tok.kind, EMBI_JST_NAME);
    EMB_CHECK(tok.flags & EMBI_JST_F_ESCAPED);
    EMB_CHECK_EQ_INT(tok.cooked_len, 3);
    EMB_CHECK_EQ_INT(tok.cooked[1], 0x200D);
}

EMB_TEST_MAIN()
