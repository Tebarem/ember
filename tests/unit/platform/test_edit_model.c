/*
 * The text editing model of the form controls (W5e, platform/edit.c): values and UTF-16 offsets, selections and their
 * direction, insertion with maxlength, deletion by character and word, caret movement with and without extension, and
 * newline stripping. Memory returns to its baseline.
 */
#include "emb_test.h"

#include "base/base.h"
#include "platform/edit.h"

#include <string.h>

static bool value_is(const embi_edit *e, const char *want)
{
    embi_str v = embi_edit_value(e);
    size_t n = strlen(want);
    return v.len == n && (n == 0 || memcmp(v.ptr, want, n) == 0);
}

static void sel(const embi_edit *e, uint32_t *a, uint32_t *b, int *dir)
{
    embi_edit_get_selection(e, a, b, dir);
}

EMB_TEST(edit_set_value_and_caret_at_end)
{
    size_t before = embi_mem_live();
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("abc")));
    EMB_CHECK(value_is(&e, "abc"));
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 3);
    EMB_CHECK_EQ_INT(b, 3);
    EMB_CHECK_EQ_INT(dir, EMBI_EDIT_DIR_NONE);
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("")));
    EMB_CHECK(value_is(&e, ""));
    embi_edit_free(&e);
    EMB_CHECK_EQ_INT(embi_mem_live(), before);
}

EMB_TEST(edit_utf16_offsets)
{
    /* U+00E9 is two bytes and one unit; U+1F600 is four bytes and two units. */
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("\xC3\xA9" "\xF0\x9F\x98\x80" "x")));
    EMB_CHECK_EQ_INT(embi_edit_len16(&e), 4);
    EMB_CHECK_EQ_INT(embi_edit_to16(&e, 2), 1);
    EMB_CHECK_EQ_INT(embi_edit_to16(&e, 6), 3);
    EMB_CHECK_EQ_INT(embi_edit_to_byte(&e, 3), 6);
    /* The middle of the surrogate pair rounds down to the start of the character. */
    EMB_CHECK_EQ_INT(embi_edit_to_byte(&e, 2), 2);
    EMB_CHECK_EQ_INT(embi_edit_to_byte(&e, 99), 7);
    embi_edit_free(&e);
}

EMB_TEST(edit_selection_and_direction)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("hello world")));
    embi_edit_set_selection(&e, 6, 2, EMBI_EDIT_DIR_BACKWARD); /* reversed: start 2, end 6 */
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 2);
    EMB_CHECK_EQ_INT(b, 6);
    EMB_CHECK_EQ_INT(dir, EMBI_EDIT_DIR_BACKWARD);
    embi_str s = embi_edit_selected(&e);
    EMB_CHECK(s.len == 4 && memcmp(s.ptr, "llo ", 4) == 0);
    /* Clamped to the length. */
    embi_edit_set_selection(&e, 0, 1000, EMBI_EDIT_DIR_FORWARD);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(b, 11);
    EMB_CHECK_EQ_INT(dir, EMBI_EDIT_DIR_FORWARD);
    embi_edit_select_all(&e);
    EMB_CHECK(embi_edit_has_selection(&e));
    embi_edit_free(&e);
}

EMB_TEST(edit_insert_replaces_selection_and_respects_maxlength)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("abcdef")));
    embi_edit_set_selection(&e, 1, 3, EMBI_EDIT_DIR_NONE);
    EMB_CHECK(embi_edit_insert(&e, EMBI_STR("XY"), 0));
    EMB_CHECK(value_is(&e, "aXYdef"));
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 3);
    EMB_CHECK_EQ_INT(b, 3);
    /* maxlength 8 with 6 units present and nothing selected: only two more units fit. */
    EMB_CHECK(embi_edit_insert(&e, EMBI_STR("123456"), 8));
    EMB_CHECK(value_is(&e, "aXY12def"));
    /* Full: nothing changes. */
    EMB_CHECK(!embi_edit_insert(&e, EMBI_STR("z"), 8));
    EMB_CHECK(value_is(&e, "aXY12def"));
    embi_edit_free(&e);
}

EMB_TEST(edit_maxlength_cuts_at_code_points)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("")));
    /* A surrogate pair needs two free units: with one unit left it is not inserted at all. */
    EMB_CHECK(embi_edit_insert(&e, EMBI_STR("a\xF0\x9F\x98\x80"), 2));
    EMB_CHECK(value_is(&e, "a"));
    EMB_CHECK(embi_edit_insert(&e, EMBI_STR("\xF0\x9F\x98\x80"), 3));
    EMB_CHECK_EQ_INT(embi_edit_len16(&e), 3);
    embi_edit_free(&e);
}

EMB_TEST(edit_delete_by_character_and_word)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("one two  three")));
    EMB_CHECK(embi_edit_delete(&e, false)); /* backspace at the end */
    EMB_CHECK(value_is(&e, "one two  thre"));
    embi_edit_set_selection(&e, 0, 0, EMBI_EDIT_DIR_NONE);
    EMB_CHECK(embi_edit_delete(&e, true));
    EMB_CHECK(value_is(&e, "ne two  thre"));
    embi_edit_set_selection(&e, 0, 0, EMBI_EDIT_DIR_NONE);
    EMB_CHECK(!embi_edit_delete(&e, false)); /* nothing before the start */
    /* Ctrl+Delete from the start of "ne" removes the word. */
    EMB_CHECK(embi_edit_delete_word(&e, true));
    EMB_CHECK(value_is(&e, " two  thre"));
    embi_edit_set_selection(&e, 10, 10, EMBI_EDIT_DIR_NONE);
    EMB_CHECK(embi_edit_delete_word(&e, false)); /* Ctrl+Backspace at the end removes "thre" */
    EMB_CHECK(value_is(&e, " two  "));
    embi_edit_free(&e);
}

EMB_TEST(edit_move_collapse_and_extend)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("abc def")));
    embi_edit_move(&e, EMBI_EDIT_TEXT_START, false);
    embi_edit_move(&e, EMBI_EDIT_WORD_RIGHT, false);
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 3);
    /* Shift+Right extends, direction becomes forward. */
    embi_edit_move(&e, EMBI_EDIT_CHAR_RIGHT, true);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 3);
    EMB_CHECK_EQ_INT(b, 4);
    EMB_CHECK_EQ_INT(dir, EMBI_EDIT_DIR_FORWARD);
    /* Left without Shift collapses to the near end instead of moving. */
    embi_edit_move(&e, EMBI_EDIT_CHAR_LEFT, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 3);
    EMB_CHECK_EQ_INT(b, 3);
    embi_edit_move(&e, EMBI_EDIT_TEXT_END, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 7);
    embi_edit_move(&e, EMBI_EDIT_WORD_LEFT, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 4);
    embi_edit_free(&e);
}

EMB_TEST(edit_lines_in_textarea)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("first\nsecond\nthird")));
    embi_edit_set_selection(&e, 9, 9, EMBI_EDIT_DIR_NONE); /* inside "second" */
    embi_edit_move(&e, EMBI_EDIT_LINE_START, false);
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 6);
    embi_edit_move(&e, EMBI_EDIT_LINE_END, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 12);
    embi_edit_free(&e);
}

EMB_TEST(edit_strip_newlines_keeps_selection)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("a\nb\r\nc")));
    embi_edit_set_selection(&e, 2, 5, EMBI_EDIT_DIR_NONE); /* bytes 2..5: "b\r\n" */
    EMB_CHECK(embi_edit_strip_newlines(&e));
    EMB_CHECK(value_is(&e, "abc"));
    uint32_t a, b;
    int dir;
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 1);
    EMB_CHECK_EQ_INT(b, 2);
    EMB_CHECK(!embi_edit_strip_newlines(&e)); /* nothing left to strip */
    embi_edit_free(&e);
}

EMB_TEST(edit_line_up_and_down_keep_the_column)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("abcd\nef\nghijk")));
    uint32_t a, b;
    int dir;
    embi_edit_set_selection(&e, 3, 3, EMBI_EDIT_DIR_NONE); /* column 3 of line 0 */
    embi_edit_move(&e, EMBI_EDIT_LINE_DOWN, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 7); /* line 1 has two characters: the end of "ef" */
    EMB_CHECK_EQ_INT(b, 7);
    embi_edit_move(&e, EMBI_EDIT_LINE_DOWN, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 10); /* column 2 of "ghijk": the column is the one the caret has now (no goal column) */
    embi_edit_move(&e, EMBI_EDIT_LINE_UP, false);
    embi_edit_move(&e, EMBI_EDIT_LINE_UP, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 2);
    /* up on the first line goes to the start, down on the last line to the end */
    embi_edit_move(&e, EMBI_EDIT_LINE_UP, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 0);
    embi_edit_set_selection(&e, 10, 10, EMBI_EDIT_DIR_NONE);
    embi_edit_move(&e, EMBI_EDIT_LINE_DOWN, false);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 13);
    embi_edit_free(&e);
}

EMB_TEST(edit_line_motion_extends_the_selection)
{
    embi_edit e = {0};
    EMB_CHECK(embi_edit_set_value(&e, EMBI_STR("ab\ncd")));
    uint32_t a, b;
    int dir;
    embi_edit_set_selection(&e, 1, 1, EMBI_EDIT_DIR_NONE);
    embi_edit_move(&e, EMBI_EDIT_LINE_DOWN, true);
    sel(&e, &a, &b, &dir);
    EMB_CHECK_EQ_INT(a, 1);
    EMB_CHECK_EQ_INT(b, 4);
    EMB_CHECK_EQ_INT(dir, EMBI_EDIT_DIR_FORWARD);
    embi_edit_free(&e);
}

EMB_TEST_MAIN()
