#include "text_fixture.h"

/* Variable fonts: NotoSans-VF.ttf (fvar wght 100..400..900, wdth 62.5..100..100). The advance of
 * 'A' follows the axes (HVAR), and each distinct coordinate set is its own instance. */

static float adv_of(const embi_text_layout *lay)
{
    return lay->advance;
}

EMB_TEST(weight_axis_changes_the_advance)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Test VF");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", &d) == 1);
    embi_face *f = embi_text_fonts(t)->faces.data[before];

    embi_str fam = tx_s("Test VF");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f); /* 1 px per font unit */
    st.kerning = EMBI_KERN_NONE;

    st.weight = 100.0f;
    embi_text_layout thin;
    EMB_REQUIRE(tx_shape(t, "A", &st, &thin));
    st.weight = 900.0f;
    embi_text_layout black;
    EMB_REQUIRE(tx_shape(t, "A", &st, &black));
    /* Heavier 'A' is wider in Noto Sans. */
    EMB_CHECK(adv_of(&black) > adv_of(&thin));
    EMB_CHECK(thin.runs.data[0].inst != black.runs.data[0].inst);
    EMB_CHECK(f->inst_count >= 2);
    /* The instance at 100 is the axis minimum: its coordinates are the normalized -1. */
    const int16_t *c = embi_face_inst_coords(f, thin.runs.data[0].inst);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK_EQ_INT(c[f->ax_wght], -16384);
    embi_text_layout_free(&thin);
    embi_text_layout_free(&black);
    embi_text_destroy(t);
}

EMB_TEST(width_axis_narrows_the_advance)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Test VF");
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", &d) == 1);

    embi_str fam = tx_s("Test VF");
    embi_text_style st;
    tx_style(&st, &fam, 1, 1000.0f);
    st.kerning = EMBI_KERN_NONE;

    st.stretch = 100.0f;
    embi_text_layout normal;
    EMB_REQUIRE(tx_shape(t, "A", &st, &normal));
    /* 62.5% is the axis minimum, so the condensed advance is narrower. */
    st.stretch = 62.5f;
    embi_text_layout condensed;
    EMB_REQUIRE(tx_shape(t, "A", &st, &condensed));
    EMB_CHECK(adv_of(&condensed) < adv_of(&normal));
    embi_text_layout_free(&normal);
    embi_text_layout_free(&condensed);
    embi_text_destroy(t);
}

EMB_TEST(same_coordinates_share_an_instance)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    embi_face_desc d = {0};
    d.family = tx_s("Test VF");
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", &d) == 1);
    embi_face *f = embi_text_fonts(t)->faces.data[before];

    embi_str fam = tx_s("Test VF");
    embi_text_style st;
    tx_style(&st, &fam, 1, 16.0f);
    st.weight = 650.0f;
    embi_text_layout a, b;
    EMB_REQUIRE(tx_shape(t, "AB", &st, &a));
    uint32_t count = f->inst_count;
    EMB_REQUIRE(tx_shape(t, "AB", &st, &b));
    EMB_CHECK_EQ_INT(f->inst_count, count);
    EMB_CHECK_EQ_INT(a.runs.data[0].inst, b.runs.data[0].inst);
    embi_text_layout_free(&a);
    embi_text_layout_free(&b);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
