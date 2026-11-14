#include "text_fixture.h"

/* Bidi and joining: NotoSansArabic-Regular.ttf (its own family, "Noto Sans Arabic") with the
 * Latin fixture NotoSans-Regular ("Noto Test"). Byte offsets: U+0627 U+0628 U+062C are two bytes
 * each, so the three letters start at bytes 0, 2 and 4. */

static embi_face *reg(embi_text *t, const char *rel, const char *family)
{
    embi_face_desc d = {0};
    d.family = tx_s(family);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, rel, &d) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

EMB_TEST(arabic_joins_and_runs_right_to_left)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *ar = reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic");
    EMB_REQUIRE(ar != nullptr);
    embi_str fam = tx_s("Noto Sans Arabic");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    /* alef, beh, jeem */
    EMB_REQUIRE(tx_shape(t, "\xD8\xA7\xD8\xA8\xD8\xAC", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].rtl, 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].level, 1);
    /* This font writes beh and jeem as a base form plus a zero-advance dot mark, so the three
     * letters make five glyphs: clusters 4, 4 (jeem), 2, 2 (beh), 0 (alef), in visual order. */
    EMB_REQUIRE(lay.glyphs.len == 5);
    uint32_t want[5] = {4, 4, 2, 2, 0};
    for (uint32_t i = 0; i < 5; i++) {
        EMB_CHECK_EQ_INT(lay.glyphs.data[i].cluster, want[i]);
    }
    /* alef follows nothing, so it is isolated (cmap glyph); beh joins alef on its right and
     * jeem, so beh is an initial form and never the isolated glyph. */
    EMB_CHECK_EQ_INT(lay.glyphs.data[4].gid, embi_font_glyph_index(&ar->font, 0x0627));
    EMB_CHECK(lay.glyphs.data[3].gid != embi_font_glyph_index(&ar->font, 0x0628));
    EMB_CHECK(lay.glyphs.data[3].advance > 0.0f);
    /* The line: one visual run, starting at x = 0. */
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 6, &vr));
    EMB_REQUIRE(vr.items.len == 1);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    EMB_CHECK(vr.width == lay.advance);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(mixed_ltr_paragraph_keeps_logical_order_across_levels)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 16.0f);
    st.direction = EMBI_DIR_LTR;
    /* "abc " + alef beh jeem + " 123": Latin, Arabic (RTL) and digits (AN, level 2). */
    const char *text = "abc \xD8\xA7\xD8\xA8\xD8\xAC 123";
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, text, &st, &lay));
    EMB_REQUIRE(lay.paras.len == 1);
    EMB_CHECK_EQ_INT(lay.paras.data[0].level, 0);
    /* The levels: Latin 0, Arabic 1, digits 2. Find the Arabic run and the digit run. */
    bool saw_ar = false, saw_digits = false;
    for (uint32_t i = 0; i < lay.runs.len; i++) {
        if (lay.runs.data[i].text_begin == 4) {
            EMB_CHECK_EQ_INT(lay.runs.data[i].level, 1);
            saw_ar = true;
        }
        if (lay.runs.data[i].text_begin == 11) {
            EMB_CHECK_EQ_INT(lay.runs.data[i].level, 2);
            saw_digits = true;
        }
    }
    EMB_CHECK(saw_ar);
    EMB_CHECK(saw_digits);
    /* Visual order: Latin first at x = 0, then the Arabic, then the digits at the right end. */
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, (uint32_t)SDL_strlen(text), &vr));
    EMB_REQUIRE(vr.items.len >= 3);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    EMB_CHECK_EQ_INT(lay.runs.data[vr.items.data[0].run].text_begin, 0);
    /* UAX #9 L2: the digits (level 2) and the Arabic (level 1) are reversed together with the
     * space between them, so the digits follow "abc" and the Arabic ends the line. */
    EMB_CHECK_EQ_INT(lay.runs.data[vr.items.data[1].run].text_begin, 11);
    EMB_CHECK_EQ_INT(lay.runs.data[vr.items.data[vr.items.len - 1].run].text_begin, 4);
    for (uint32_t i = 1; i < vr.items.len; i++) {
        EMB_CHECK(vr.items.data[i].x > vr.items.data[i - 1].x);
    }
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* bidi-box-model-014: "First " in an rtl block, shaped as its own run, ends in a space that the run alone resolves to the
 * paragraph level (the space goes left of the word). When a letter of the other direction follows in the next box
 * (bidi_after), the space lies between two LTR letters and stays in the LTR run: one visual run, the space last. */
EMB_TEST(trailing_space_before_a_letter_of_the_other_direction_keeps_the_text_level)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.direction = EMBI_DIR_RTL;
    embi_text_layout lay;
    embi_text_vruns vr = {0};
    EMB_REQUIRE(tx_shape(t, "First ", &st, &lay));
    EMB_REQUIRE(embi_text_line(&lay, 0, 6, &vr));
    EMB_CHECK(vr.items.len >= 2); /* alone: the space is a run of its own at the left */
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    st.bidi_after = true;
    EMB_REQUIRE(tx_shape(t, "First ", &st, &lay));
    EMB_REQUIRE(embi_text_line(&lay, 0, 6, &vr));
    EMB_REQUIRE(vr.items.len == 1);
    EMB_CHECK_EQ_INT(vr.items.data[0].level, 2);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* A trailing space after right-to-left text in a left-to-right paragraph: alone it takes the paragraph level (0); when a
 * right-to-left letter follows in the next box (bidi_after) it lies between two RTL letters and stays at level 1. This
 * needs the sentinel appended after the text to be a strong R (U+200F) for an LTR base. */
EMB_TEST(trailing_space_after_rtl_text_in_an_ltr_paragraph_keeps_the_text_level)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 16.0f);
    st.direction = EMBI_DIR_LTR;
    embi_text_layout lay;
    embi_text_vruns vr = {0};
    EMB_REQUIRE(tx_shape(t, "\330\247\330\250 ", &st, &lay));
    EMB_REQUIRE(embi_text_line(&lay, 0, 5, &vr));
    EMB_REQUIRE(vr.items.len >= 2);
    EMB_CHECK_EQ_INT(vr.items.data[vr.items.len - 1].level, 0); /* alone: the space is at the paragraph level, right of the word */
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    st.bidi_after = true;
    EMB_REQUIRE(tx_shape(t, "\330\247\330\250 ", &st, &lay));
    EMB_REQUIRE(embi_text_line(&lay, 0, 5, &vr));
    for (uint32_t i = 0; i < vr.items.len; i++) {
        EMB_CHECK_EQ_INT(vr.items.data[i].level, 1);
    }
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(rtl_paragraph_reverses_visual_order)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 16.0f);
    st.direction = EMBI_DIR_RTL;
    /* "ab " + alef beh jeem in an RTL paragraph: the Arabic is on the left, "ab" on the right. */
    const char *text = "ab \xD8\xA7\xD8\xA8\xD8\xAC";
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, text, &st, &lay));
    EMB_CHECK_EQ_INT(lay.paras.data[0].level, 1);
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, (uint32_t)SDL_strlen(text), &vr));
    EMB_REQUIRE(vr.items.len >= 2);
    /* L2 reverses the whole line (every level is >= 1): the Arabic (byte 3) comes first, then
     * the space (byte 2), then "ab" (byte 0). */
    EMB_CHECK_EQ_INT(lay.runs.data[vr.items.data[0].run].text_begin, 3);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    EMB_CHECK_EQ_INT(lay.runs.data[vr.items.data[vr.items.len - 1].run].text_begin, 0);
    EMB_CHECK(vr.width == lay.advance);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(hard_break_splits_paragraphs)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_str fam = tx_s("Noto Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "a\nb", &st, &lay));
    EMB_CHECK_EQ_INT(lay.paras.len, 2);
    EMB_CHECK_EQ_INT(lay.paras.data[0].end, 1);
    EMB_CHECK_EQ_INT(lay.paras.data[1].begin, 2);
    bool hard = false;
    for (uint32_t i = 0; i < lay.breaks.len; i++) {
        if (lay.breaks.data[i].offset == 2 && (lay.breaks.data[i].flags & EMBI_BREAK_HARD)) {
            hard = true;
        }
    }
    EMB_CHECK(hard);
    /* A line may not cross the paragraph break. */
    embi_text_vruns vr = {0};
    EMB_CHECK(!embi_text_line(&lay, 0, 3, &vr));
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* Blank lines: "a\n\nb" has three paragraphs, the middle one holding only its separator. The
 * separator bytes belong to no run and draw no glyph (no .notdef box on blank lines). sep1 is the
 * end of the first paragraph, sep2 the begin of the blank one, last_begin the begin of the last. */
static void check_blank_lines(const char *text, uint32_t sep1, uint32_t sep2, uint32_t last_begin)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, text, &st, &lay));
    EMB_REQUIRE(lay.paras.len == 3);
    EMB_CHECK_EQ_INT(lay.paras.data[0].begin, 0);
    EMB_CHECK_EQ_INT(lay.paras.data[0].end, sep1);
    EMB_CHECK_EQ_INT(lay.paras.data[1].begin, sep2);
    EMB_CHECK_EQ_INT(lay.paras.data[1].end, sep2);
    EMB_CHECK_EQ_INT(lay.paras.data[2].begin, last_begin);
    EMB_CHECK_EQ_INT(lay.paras.data[2].end, last_begin + 1);
    /* Two runs: 'a' and 'b'. Nothing for the blank paragraph. */
    EMB_REQUIRE(lay.runs.len == 2);
    EMB_CHECK_EQ_INT(lay.runs.data[0].text_begin, 0);
    EMB_CHECK_EQ_INT(lay.runs.data[0].text_end, sep1);
    EMB_CHECK_EQ_INT(lay.runs.data[1].text_begin, last_begin);
    EMB_CHECK_EQ_INT(lay.glyphs.len, 2);
    for (uint32_t g = 0; g < lay.glyphs.len; g++) {
        EMB_CHECK(lay.glyphs.data[g].gid != 0);
    }
    /* The blank paragraph has an empty line; the separator alone is an empty line too. */
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, sep2, sep2, &vr));
    EMB_CHECK_EQ_INT(vr.items.len, 0);
    EMB_REQUIRE(embi_text_line(&lay, sep1, sep2, &vr));
    EMB_CHECK_EQ_INT(vr.items.len, 0);
    EMB_CHECK(vr.width == 0.0f);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(blank_line_is_an_empty_paragraph)
{
    /* "a" "\n" "\n" "b": paragraphs 0..1, 2..2 and 3..4. */
    check_blank_lines("a\n\nb", 1, 2, 3);
}

EMB_TEST(crlf_blank_line_is_an_empty_paragraph)
{
    /* "a" "\r\n" "\r\n" "b": paragraphs 0..1, 3..3 and 5..6. */
    check_blank_lines("a\r\n\r\nb", 1, 3, 5);
}

EMB_TEST(second_rtl_paragraph_uses_absolute_offsets)
{
    /* "ab\n" then alef beh jeem: the Arabic paragraph is bytes 3..9 and is RTL (auto direction). */
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 16.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "ab\n\xD8\xA7\xD8\xA8\xD8\xAC", &st, &lay));
    EMB_REQUIRE(lay.paras.len == 2);
    EMB_CHECK_EQ_INT(lay.paras.data[1].begin, 3);
    EMB_CHECK_EQ_INT(lay.paras.data[1].level, 1);
    EMB_CHECK(lay.bidi != nullptr);
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 3, 9, &vr));
    EMB_REQUIRE(vr.items.len >= 1);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    EMB_CHECK(vr.width == embi_text_measure(&lay, 3, 9));
    /* Visual order of the Arabic: the jeem (bytes 7..9) is the left-most glyph. */
    EMB_CHECK_EQ_INT(lay.glyphs.data[vr.items.data[0].glyph_begin].cluster, 7);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(line_inside_rtl_run_is_clamped_to_its_range)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fams[2] = {tx_s("Noto Test"), tx_s("Noto Sans Arabic")};
    embi_text_style st;
    tx_style(&st, fams, 2, 16.0f);
    st.direction = EMBI_DIR_LTR;
    /* "abc " + alef beh jeem (bytes 4..10): the line 4..10 is the Arabic alone, level 1. */
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "abc \xD8\xA7\xD8\xA8\xD8\xAC", &st, &lay));
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 4, 10, &vr));
    EMB_REQUIRE(vr.items.len == 1);
    EMB_CHECK_EQ_INT(vr.items.data[0].level, 1);
    EMB_CHECK(vr.items.data[0].x == 0.0f);
    EMB_CHECK(vr.width == embi_text_measure(&lay, 4, 10));
    /* A range that stops inside a run keeps only its glyphs. */
    embi_text_vruns part = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 2, &part));
    EMB_REQUIRE(part.items.len == 1);
    EMB_CHECK(part.width == embi_text_measure(&lay, 0, 2));
    embi_text_vruns_free(&part);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(ltr_text_keeps_no_bidi_cache)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "Hello,\nworld", &st, &lay));
    EMB_CHECK(lay.bidi == nullptr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
