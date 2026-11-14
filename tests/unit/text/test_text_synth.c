#include "text_fixture.h"

/* Synthetic faces (W4g2): bold and oblique runs when the matched face lacks the weight or the style, the
 * font-synthesis switches, and small capitals (smcp when the face has it, else upper case letters at 0.7 of the
 * size). */

static embi_text *make(void)
{
    return embi_text_create();
}

static uint8_t run_synth(embi_text *t, const embi_text_style *st)
{
    embi_text_layout lay;
    if (!tx_shape(t, "Text", st, &lay) || lay.runs.len < 1) {
        embi_text_layout_free(&lay);
        return 0xFF;
    }
    uint8_t s = lay.runs.data[0].synth;
    embi_text_layout_free(&lay);
    return s;
}

EMB_TEST(bold_and_oblique_are_synthesized_for_a_face_without_them)
{
    embi_text *t = make();
    embi_str fam = tx_s("Noto Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.synth = EMBI_SYNTH_ALL;
    EMB_CHECK(run_synth(t, &st) == 0);
    st.weight = 700.0f;
    EMB_CHECK(run_synth(t, &st) == EMBI_RUN_SYNTH_BOLD);
    st.weight = 600.0f;
    EMB_CHECK(run_synth(t, &st) == EMBI_RUN_SYNTH_BOLD);
    st.weight = 500.0f;
    EMB_CHECK(run_synth(t, &st) == 0);
    st.weight = 700.0f;
    st.font_style = EMBI_STYLE_ITALIC;
    EMB_CHECK(run_synth(t, &st) == (EMBI_RUN_SYNTH_BOLD | EMBI_RUN_SYNTH_OBLIQUE));
    st.font_style = EMBI_STYLE_OBLIQUE;
    EMB_CHECK((run_synth(t, &st) & EMBI_RUN_SYNTH_OBLIQUE) != 0);
    embi_text_destroy(t);
}

EMB_TEST(font_synthesis_none_switches_each_synthesis_off)
{
    embi_text *t = make();
    embi_str fam = tx_s("Noto Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.weight = 700.0f;
    st.font_style = EMBI_STYLE_ITALIC;
    st.synth = EMBI_SYNTH_ALL & ~EMBI_SYNTH_WEIGHT;
    EMB_CHECK(run_synth(t, &st) == EMBI_RUN_SYNTH_OBLIQUE);
    st.synth = EMBI_SYNTH_ALL & ~EMBI_SYNTH_STYLE;
    EMB_CHECK(run_synth(t, &st) == EMBI_RUN_SYNTH_BOLD);
    st.synth = 0;
    EMB_CHECK(run_synth(t, &st) == 0);
    embi_text_destroy(t);
}

EMB_TEST(a_registered_bold_or_italic_face_is_not_synthesized)
{
    embi_text *t = make();
    embi_face_desc bold = {0};
    bold.family = tx_s("Fam");
    bold.weight_min = bold.weight_max = 700;
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &bold) == 1);
    embi_face_desc ital = {0};
    ital.family = tx_s("Fam");
    ital.weight_min = ital.weight_max = 400;
    ital.has_style = true;
    ital.style = EMBI_STYLE_ITALIC;
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &ital) == 1);
    embi_str fam = tx_s("Fam");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.synth = EMBI_SYNTH_ALL;
    st.weight = 700.0f;
    EMB_CHECK(run_synth(t, &st) == 0);
    st.weight = 400.0f;
    st.font_style = EMBI_STYLE_ITALIC;
    EMB_CHECK(run_synth(t, &st) == 0);
    embi_text_destroy(t);
}

EMB_TEST(variable_weight_axis_needs_no_synthesis)
{
    embi_text *t = make();
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", nullptr) == 1);
    embi_str fam = tx_s("Noto Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.synth = EMBI_SYNTH_ALL;
    st.weight = 700.0f;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "Text", &st, &lay));
    /* The face that matched is the variable one or the bundled default (static, 400): the variable one never synthesizes. */
    embi_face *f = lay.runs.data[0].face;
    EMB_CHECK(f->ax_wght < 0 || lay.runs.data[0].synth == 0);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* Ahem has no smcp: lower case letters become upper case letters at 0.7 of the size. */
EMB_TEST(small_caps_are_synthesized_when_the_face_has_no_smcp)
{
    embi_text *t = make();
    EMB_REQUIRE(tx_add_file(t, "tests/wpt/fonts/Ahem.ttf", nullptr) == 1);
    embi_str fam = tx_s("Ahem");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    st.synth = EMBI_SYNTH_ALL;
    st.caps = EMBI_CAPS_SMALL;
    embi_text_layout lay;
    EMB_REQUIRE(tx_shape(t, "aBc", &st, &lay));
    EMB_REQUIRE(lay.runs.len == 3);
    EMB_CHECK(lay.runs.data[0].size == 14.0f);
    EMB_CHECK(lay.runs.data[1].size == 20.0f);
    EMB_CHECK(lay.runs.data[2].size == 14.0f);
    EMB_CHECK(lay.advance == 14.0f + 20.0f + 14.0f);
    /* The text and the byte offsets are those of the source. */
    EMB_CHECK_EQ_STR(lay.text, "aBc");
    embi_text_layout_free(&lay);
    /* all-small-caps shrinks the upper case letters too. */
    st.caps = EMBI_CAPS_ALL_SMALL;
    EMB_REQUIRE(tx_shape(t, "aBc", &st, &lay));
    EMB_CHECK(lay.advance == 3.0f * 14.0f);
    embi_text_layout_free(&lay);
    /* font-synthesis-small-caps: none leaves the letters alone. */
    st.caps = EMBI_CAPS_SMALL;
    st.synth = EMBI_SYNTH_ALL & ~EMBI_SYNTH_SMALL_CAPS;
    EMB_REQUIRE(tx_shape(t, "aBc", &st, &lay));
    EMB_CHECK(lay.advance == 60.0f);
    embi_text_layout_free(&lay);
    /* Non-letters keep their size, and so do letters without a mapping of the same UTF-8 length. */
    st.synth = EMBI_SYNTH_ALL;
    EMB_REQUIRE(tx_shape(t, "a1 \xC3\xA9", &st, &lay)); /* a 1 space e-acute */
    EMB_CHECK(lay.advance == 14.0f + 20.0f + 20.0f + 14.0f);
    embi_text_layout_free(&lay);
    embi_text_destroy(t);
}

/* The bundled face has smcp: the feature is used, nothing is shrunk. */
EMB_TEST(small_caps_use_the_smcp_feature_of_the_face)
{
    embi_text *t = make();
    embi_str fam = tx_s("Noto Sans");
    embi_text_style st;
    tx_style(&st, &fam, 1, 20.0f);
    st.synth = EMBI_SYNTH_ALL;
    embi_text_layout plain, sc;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &plain));
    st.caps = EMBI_CAPS_SMALL;
    EMB_REQUIRE(tx_shape(t, "abc", &st, &sc));
    EMB_REQUIRE(sc.runs.len == 1);
    EMB_CHECK(sc.runs.data[0].size == 20.0f);
    EMB_REQUIRE(plain.glyphs.len == 3 && sc.glyphs.len == 3);
    EMB_CHECK(plain.glyphs.data[0].gid != sc.glyphs.data[0].gid);
    embi_text_layout_free(&plain);
    embi_text_layout_free(&sc);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
