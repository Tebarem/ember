#include "text_fixture.h"

/* Shaping with kb_text_shape over NotoSans-Regular.ttf (upem 1000). The face is registered under
 * the family "Noto Test" so that the bundled default face cannot take its place. Expected values:
 *   'A' gid 36, advance 639; space gid 3, advance 260 (test_font_metrics, same fixture).
 * Kerning and ligature expectations are relative (kerned < unkerned, 'fi' one glyph), since
 * the exact values belong to the font's GPOS and GSUB tables. */

static embi_face *register_test_face(embi_text *t)
{
    embi_face_desc d = {0};
    d.family = tx_s("Noto Test");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    if (tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) != 1) {
        return nullptr;
    }
    return embi_text_fonts(t)->faces.data[before];
}

static float glyph_adv(const embi_face *f, const char *cp_str)
{
    uint32_t cp = (uint32_t)(uint8_t)cp_str[0];
    uint32_t gid = embi_font_glyph_index(&f->font, cp);
    float adv = 0.0f;
    embi_font_glyph_advance(&f->font, gid, nullptr, &adv);
    return adv;
}

EMB_TEST(latin_glyphs_advances_and_clusters)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *f = register_test_face(t);
    EMB_REQUIRE(f != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f); /* 1 px per font unit */
    st.kerning = EMBI_KERN_NONE;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "A ", &st, &lay));
    EMB_REQUIRE(lay.glyphs.len == 2);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].gid, 36);
    EMB_CHECK(lay.glyphs.data[0].advance == 639.0f);
    EMB_CHECK_EQ_INT(lay.glyphs.data[0].cluster, 0);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].gid, 3);
    EMB_CHECK(lay.glyphs.data[1].advance == 260.0f);
    EMB_CHECK_EQ_INT(lay.glyphs.data[1].cluster, 1);
    EMB_CHECK(lay.advance == 899.0f);
    EMB_REQUIRE(lay.runs.len == 1);
    EMB_CHECK(lay.runs.data[0].face == f);
    EMB_CHECK_EQ_INT(lay.runs.data[0].rtl, 0);
    EMB_CHECK_EQ_INT(lay.runs.data[0].glyph_end, 2);
    /* Caret stops: both clusters start a grapheme. */
    EMB_CHECK(lay.glyphs.data[0].safe_to_break);
    EMB_CHECK(lay.glyphs.data[1].safe_to_break);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(size_scales_advances)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    register_test_face(t);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.kerning = EMBI_KERN_NONE;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "A", &st, &lay));
    /* 639 / 1000 * 16 px. */
    float want = 639.0f * 16.0f / 1000.0f;
    float got = lay.glyphs.data[0].advance;
    EMB_CHECK(got - want < 1e-3f && want - got < 1e-3f);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(kerning_none_is_sum_of_advances_and_normal_differs)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *f = register_test_face(t);
    EMB_REQUIRE(f != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    float sum = glyph_adv(f, "A") + glyph_adv(f, "V");
    st.kerning = EMBI_KERN_NONE;
    embi_text_layout off;
    EMB_REQUIRE(tx_shape(t, "AV", &st, &off));
    EMB_CHECK(off.advance == sum);
    st.kerning = EMBI_KERN_NORMAL;
    embi_text_layout on;
    EMB_REQUIRE(tx_shape(t, "AV", &st, &on));
    /* Noto Sans kerns A-V: the pair is tighter with kerning on. */
    EMB_CHECK(on.advance < off.advance);
    /* font-feature-settings "kern" 0 is the same as kerning none. */
    embi_text_feature feat[1] = {{EMBI_TAG('k', 'e', 'r', 'n'), 0, 0.0f}};
    st.kerning = EMBI_KERN_AUTO;
    st.features = feat;
    st.feature_count = 1;
    embi_text_layout via_feature;
    EMB_REQUIRE(tx_shape(t, "AV", &st, &via_feature));
    EMB_CHECK(via_feature.advance == sum);
    embi_text_layout_free(&off);
    embi_text_layout_free(&on);
    embi_text_layout_free(&via_feature);
    embi_text_destroy(t);
}

EMB_TEST(fi_ligature_collapses_unless_disabled)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    register_test_face(t);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout lig;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &lig));
    EMB_CHECK_EQ_INT(lig.glyphs.len, 1);
    EMB_CHECK_EQ_INT(lig.glyphs.data[0].cluster, 0);
    /* font-variant-ligatures: none. */
    st.ligatures = false;
    embi_text_layout nolig;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &nolig));
    EMB_CHECK_EQ_INT(nolig.glyphs.len, 2);
    EMB_CHECK_EQ_INT(nolig.glyphs.data[0].cluster, 0);
    EMB_CHECK_EQ_INT(nolig.glyphs.data[1].cluster, 1);
    /* letter-spacing != 0 disables the optional ligatures as well (CSS Text 3 section 8.2), even
     * with font-variant-ligatures at its default. */
    st.ligatures = true;
    st.letter_spacing = 2.0f;
    embi_text_layout spaced;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &spaced));
    EMB_CHECK_EQ_INT(spaced.glyphs.len, 2);
    EMB_CHECK_EQ_INT(spaced.glyphs.data[1].cluster, 1);
    /* Spacing back at 0: the ligature is used again. */
    st.letter_spacing = 0.0f;
    embi_text_layout back;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &back));
    EMB_CHECK_EQ_INT(back.glyphs.len, 1);
    /* An explicit font-feature-settings "liga" 1 wins over the letter-spacing rule. */
    embi_text_feature liga_on[1] = {{EMBI_TAG('l', 'i', 'g', 'a'), 1, 0.0f}};
    st.letter_spacing = 2.0f;
    st.features = liga_on;
    st.feature_count = 1;
    embi_text_layout forced;
    EMB_REQUIRE(tx_shape(t, "fi", &st, &forced));
    EMB_CHECK_EQ_INT(forced.glyphs.len, 1);
    embi_text_layout_free(&lig);
    embi_text_layout_free(&nolig);
    embi_text_layout_free(&spaced);
    embi_text_layout_free(&back);
    embi_text_layout_free(&forced);
    embi_text_destroy(t);
}

EMB_TEST(letter_and_word_spacing_are_added)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face *f = register_test_face(t);
    EMB_REQUIRE(f != nullptr);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    st.kerning = EMBI_KERN_NONE;
    st.letter_spacing = 10.0f;
    st.word_spacing = 50.0f;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "A A", &st, &lay));
    /* Each character gets letter-spacing after it; the space also gets word-spacing. */
    float a = glyph_adv(f, "A");
    float sp = glyph_adv(f, " ");
    EMB_CHECK(lay.advance == 2.0f * a + sp + 3.0f * 10.0f + 50.0f);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

EMB_TEST(empty_and_invalid_utf8)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    register_test_face(t);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout e;
    EMB_REQUIRE(tx_shape(t, "", &st, &e));
    EMB_CHECK_EQ_INT(e.glyphs.len, 0);
    EMB_CHECK_EQ_INT(e.runs.len, 0);
    EMB_CHECK_EQ_INT(e.paras.len, 0);
    embi_text_layout_free(&e);
    /* A lone continuation byte shapes as U+FFFD: one glyph, one cluster. */
    embi_text_layout bad;
    EMB_REQUIRE(tx_shape(t, "\x80", &st, &bad));
    EMB_CHECK_EQ_INT(bad.glyphs.len, 1);
    EMB_CHECK_EQ_INT(bad.glyphs.data[0].cluster, 0);
    embi_text_layout_free(&bad);
    embi_text_destroy(t);
}

EMB_TEST(shaping_cache_hits_and_respects_budget)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    register_test_face(t);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    embi_text_layout a, b;
    uint64_t h0, m0, h1, m1;
    size_t bytes;
    EMB_REQUIRE(tx_shape(t, "cache me", &st, &a));
    embi_shape_stats(t, &h0, &m0, &bytes);
    EMB_CHECK(m0 >= 1);
    EMB_REQUIRE(tx_shape(t, "cache me", &st, &b));
    embi_shape_stats(t, &h1, &m1, &bytes);
    EMB_CHECK(h1 > h0);
    EMB_CHECK(m1 == m0);
    /* Same glyphs at another size (cache is in font units). */
    st.size = 32.0f;
    embi_text_layout c;
    EMB_REQUIRE(tx_shape(t, "cache me", &st, &c));
    EMB_CHECK(c.glyphs.data[0].advance == 2.0f * a.glyphs.data[0].advance);
    /* A tiny budget keeps the cache small: only the newest entry may exceed it. */
    embi_text_set_shape_cache_budget(t, 512);
    for (int i = 0; i < 40; i++) {
        char text[32];
        SDL_snprintf(text, sizeof text, "run %d text", i);
        embi_text_layout x;
        EMB_REQUIRE(tx_shape(t, text, &st, &x));
        embi_text_layout_free(&x);
    }
    embi_shape_stats(t, nullptr, nullptr, &bytes);
    EMB_CHECK(bytes <= 512 + 1024);
    embi_text_layout_free(&a);
    embi_text_layout_free(&b);
    embi_text_layout_free(&c);
    embi_text_destroy(t);
}

EMB_TEST(many_short_ascii_runs_never_crash)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    register_test_face(t);
    embi_str fam = tx_s("Noto Test");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    for (int i = 0; i < 400; i++) {
        char text[48];
        SDL_snprintf(text, sizeof text, "run %d text %d", i, i * 7);
        embi_text_layout x;
        EMB_REQUIRE(tx_shape(t, text, &st, &x));
        EMB_CHECK(x.glyphs.len > 0);
        embi_text_layout_free(&x);
    }
    embi_text_destroy(t);
}

EMB_TEST(shaping_releases_everything)
{
    size_t base = embi_mem_live();
    {
        embi_text *t = embi_text_create();
        EMB_REQUIRE(t != nullptr);
        register_test_face(t);
        embi_str fam = tx_s("Noto Test");
        embi_text_style st;
        tx_style(&st, &fam, 1, 16.0f);
        embi_text_layout lay;
        EMB_REQUIRE(tx_shape(t, "Hello, world", &st, &lay));
        embi_text_layout_free(&lay);
        embi_text_destroy(t);
    }
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST_MAIN()
