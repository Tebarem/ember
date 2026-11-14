#include "text_fixture.h"

/* unicode-bidi: bidi-override (EMBI_DIR_OVERRIDE, W4g5): every character takes the paragraph's direction, whatever
 * its own bidi class. Latin: NotoSans-Regular ("Noto Test"); Arabic: NotoSansArabic-Regular. */

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

EMB_TEST(rtl_override_makes_latin_text_one_rtl_run_in_reverse_order)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.kerning = EMBI_KERN_NONE;
    st.direction = EMBI_DIR_RTL | EMBI_DIR_OVERRIDE;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].rtl, 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].level, 1);
    EMB_REQUIRE(lay.glyphs.len == 3);
    /* visual order: the last character first */
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 2);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].cluster, 1);
    EMB_CHECK_EQ_INT(lay.glyphs.data[2].cluster, 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(without_the_override_latin_in_an_rtl_paragraph_stays_left_to_right)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.kerning = EMBI_KERN_NONE;
    st.direction = EMBI_DIR_RTL;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].rtl, 0); /* the letters are an ltr run (level 2) */
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(ltr_override_keeps_arabic_letters_in_logical_order)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic") != nullptr);
    embi_str fam = tx_s("Noto Sans Arabic");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.direction = EMBI_DIR_LTR | EMBI_DIR_OVERRIDE;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "\xD8\xA7\xD8\xA8\xD8\xAC", &st, &lay));
    EMB_REQUIRE(lay.runs.len >= 1);
    for (uint32_t i = 0; i < lay.runs.len; i++) {
        EMB_CHECK_EQ_INT(lay.runs.data[i].rtl, 0);
        EMB_CHECK_EQ_INT(lay.runs.data[i].level, 0);
    }
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 0); /* logical order, not reversed */
    EMB_CHECK(lay.bidi == nullptr);                  /* all levels are 0: no line order cache */
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(an_out_of_range_direction_with_the_override_bit_is_sanitized)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test") != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.direction = (uint8_t)(0x0F | EMBI_DIR_OVERRIDE); /* base 15: not a direction */
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &lay)); /* falls back to auto and does not crash */
    EMB_CHECK(lay.runs.len >= 1);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
