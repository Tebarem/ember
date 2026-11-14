#include "text_fixture.h"

/* Font fallback (CSS family list, then the embedder fallback families, then the bundled default,
 * then .notdef) and itemization by font. The faces: NotoSans-Regular as "Noto Test" (Latin only),
 * NotoSansArabic-Regular as "Noto Sans Arabic", and Noto-COLRv1 as "Noto Color Emoji". */

/* Registers rel under the family name, or under the font's own name when family is nullptr. */
static embi_face *reg(embi_text *t, const char *rel, const char *family)
{
    embi_face_desc d = {0};
    if (family != nullptr) {
        d.family = tx_s(family);
    }
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, rel, family != nullptr ? &d : nullptr) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

EMB_TEST(fallback_family_supplies_a_missing_script)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *latin = reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test");
    embi_face *ar = reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic");
    EMB_REQUIRE(latin != nullptr && ar != nullptr);
    embi_str fb[1] = {tx_s("Noto Sans Arabic")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    /* alef: not in the Latin face, so the fallback face serves it. */
    EMB_REQUIRE(tx_shape(t, "\xD8\xA7", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK(lay.runs.data[0].face == ar);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].gid, embi_font_glyph_index(&ar->font, 0x0627));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(without_a_covering_face_the_first_family_gives_notdef)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *latin = reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test");
    EMB_REQUIRE(latin != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "\xD8\xA7", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK(lay.runs.data[0].face == latin);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].gid, 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(itemization_splits_runs_at_font_changes)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *latin = reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test");
    embi_face *ar = reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic");
    EMB_REQUIRE(latin != nullptr && ar != nullptr);
    embi_str fb[1] = {tx_s("Noto Sans Arabic")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    /* 'a' in the Latin face, alef in the Arabic fallback. */
    EMB_REQUIRE(tx_shape(t, "a\xD8\xA7", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 2);
    EMB_CHECK(lay.runs.data[0].face == latin);
    EMB_CHECK(lay.runs.data[1].face == ar);
    EMB_CHECK_EQ_INT(lay.runs.data[1].text_begin, 1);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(grapheme_cluster_is_never_split_across_fonts)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test");
    embi_face *ar = reg(t, "tests/fonts/NotoSansArabic-Regular.ttf", "Noto Sans Arabic");
    EMB_REQUIRE(ar != nullptr);
    embi_str fb[1] = {tx_s("Noto Sans Arabic")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    /* beh followed by fatha (U+064E, a combining mark): one cluster, one font. */
    EMB_REQUIRE(tx_shape(t, "\xD8\xA8\xD9\x8E", &st, &lay));
    EMB_CHECK_EQ_INT(lay.runs.len, 1);
    EMB_CHECK(lay.runs.data[0].face == ar);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(colour_emoji_falls_back_to_the_emoji_face)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    reg(t, "tests/fonts/NotoSans-Regular.ttf", "Noto Test");
    embi_face *emoji = reg(t, "tests/fonts/Noto-COLRv1.ttf", nullptr);
    EMB_REQUIRE(emoji != nullptr);
    embi_str fb[1] = {tx_s("Noto Color Emoji")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lay;
    /* U+1F600 GRINNING FACE, a CJK-style gap in the primary face. */
    EMB_REQUIRE(tx_shape(t, "\xF0\x9F\x98\x80", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK(lay.runs.data[0].face == emoji);
    EMB_CHECK(lay.glyphs.data[0].gid != 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
