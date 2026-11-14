#include "text_fixture.h"

/* Devanagari shaping through kb (Indic reordering and conjuncts) with NotoSansDevanagari-Regular
 * ("Noto Deva", upem 1000). Glyph ids and advances below were read from the font's GSUB and cmap
 * tables (probed with this module at 1000 px, so advances equal font units):
 *   ka U+0915 gid 56 (advance 768), virama U+094D gid 103 (advance 0),
 *   vowel sign i U+093F gid 545 (advance 259), ssa U+0937 gid 86 (advance 578),
 *   ka + virama + ssa forms the conjunct gid 90 (advance 717).
 * Byte offsets: every Devanagari code point is 3 bytes in UTF-8. */

static embi_face *reg_deva(embi_text *t)
{
    embi_face_desc d = {0};
    d.family = tx_s("Noto Deva");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, "tests/fonts/NotoSansDevanagari-Regular.ttf", &d) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

static bool has_grapheme_break(const embi_text_layout *lay, uint32_t offset)
{
    for (uint32_t i = 0; i < lay->breaks.len; i++) {
        if (lay->breaks.data[i].offset == offset) {
            return (lay->breaks.data[i].flags & EMBI_BREAK_GRAPHEME) != 0;
        }
    }
    return false;
}

/* ka, virama, ssa: one conjunct glyph for the whole cluster. */
EMB_TEST(conjunct_is_one_glyph_in_one_cluster)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *deva = reg_deva(t);
    EMB_REQUIRE(deva != nullptr);
    embi_str fam = tx_s("Noto Deva");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    /* U+0915 U+094D U+0937 */
    EMB_REQUIRE(tx_shape(t, "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK(lay.runs.data[0].face == deva);
    EMB_CHECK_EQ_INT(lay.runs.data[0].text_begin, 0);
    EMB_CHECK_EQ_INT(lay.runs.data[0].text_end, 9);
    /* Three code points, fewer glyphs: the conjunct. */
    EMB_REQUIRE(lay.glyphs.len == 1);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].gid, 90);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 0);
    EMB_CHECK(lay.glyphs.data[0].advance == 717.0f);
    /* No grapheme boundary inside the conjunct: the caret does not stop after the virama. */
    EMB_CHECK(!has_grapheme_break(&lay, 3));
    EMB_CHECK(!has_grapheme_break(&lay, 6));
    EMB_CHECK(has_grapheme_break(&lay, 0));
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* ka with virama and a following ka: the first consonant takes its half form (a second glyph),
 * both glyphs belong to the cluster of their own code point. */
EMB_TEST(virama_keeps_the_clusters_apart)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg_deva(t) != nullptr);
    embi_str fam = tx_s("Noto Deva");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    /* U+0915 U+094D U+0915 */
    EMB_REQUIRE(tx_shape(t, "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x95", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 2);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 0);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].cluster, 6);
    /* UAX #29 GB9c (Unicode 15.1): a virama joins the consonants on either side, so the whole
     * sequence is one grapheme and there is no caret stop at byte 6. */
    EMB_CHECK(!has_grapheme_break(&lay, 6));
    EMB_CHECK(!lay.glyphs.data[1].safe_to_break);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* A pre-base vowel sign: ka followed by sign i (U+093F), which is drawn to the left of ka.
 * In logical order the sign comes after the consonant; in glyph (visual) order it comes first. */
EMB_TEST(pre_base_matra_is_drawn_before_its_consonant)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    EMB_REQUIRE(reg_deva(t) != nullptr);
    embi_str fam = tx_s("Noto Deva");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    /* U+0915 U+093F: ka, sign i */
    EMB_REQUIRE(tx_shape(t, "\xE0\xA4\x95\xE0\xA4\xBF", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 2);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].gid, 545);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 3);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].gid, 56);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].cluster, 0);
    /* The reordering is in the glyphs, so the run is not monotone and line.c scans it. */
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK_EQ_INT(lay.runs.data[0].mono, 0);
    /* The line puts the cluster as one visual span, with the matra first (left). */
    embi_text_vruns vr = {0};
    EMB_REQUIRE(embi_text_line(&lay, 0, 6, &vr));
    EMB_REQUIRE(vr.items.len == 1);
    EMB_CHECK_EQ_INT(vr.items.data[0].glyph_begin, 0);
    EMB_CHECK_EQ_INT(vr.items.data[0].glyph_end, 2);
    EMB_CHECK(vr.width == lay.advance);
    embi_text_vruns_free(&vr);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* Indic text with a space: the Devanagari and the space (Common script) split into runs, all in
 * the one face that covers them, and the runs keep the logical order. */
EMB_TEST(indic_runs_stay_in_one_face)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *deva = reg_deva(t);
    EMB_REQUIRE(deva != nullptr);
    embi_str fam = tx_s("Noto Deva");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    embi_text_layout lay;
    /* ksha, space, ka+sign i */
    EMB_REQUIRE(tx_shape(t, "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7 \xE0\xA4\x95\xE0\xA4\xBF", &st,
                         &lay));
    EMB_REQUIRE(lay.runs.len >= 1);
    for (uint32_t i = 0; i < lay.runs.len; i++) {
        EMB_CHECK(lay.runs.data[i].face == deva);
        EMB_CHECK_EQ_INT(lay.runs.data[i].level, 0);
    }
    EMB_CHECK_EQ_INT(lay.runs.data[0].text_begin, 0);
    EMB_CHECK_EQ_INT(lay.runs.data[lay.runs.len - 1].text_end, 16);
    /* Every glyph is a real glyph, none is .notdef. */
    for (uint32_t g = 0; g < lay.glyphs.len; g++) {
        EMB_CHECK(lay.glyphs.data[g].gid != 0);
    }
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
