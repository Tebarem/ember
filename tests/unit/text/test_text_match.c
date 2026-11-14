#include "text_fixture.h"

/* CSS Fonts 4 section 5.2 matching and the fallback candidate list. The faces are one fixture
 * file registered under synthetic descriptors (family "Fam"), so each expected winner is the
 * face named by its descriptor. The variable-axis checks use NotoSans-VF (wght 100..900, wdth
 * 62.5..100). */

typedef struct fam_set {
    embi_face *normal400, *bold700, *italic400, *light300, *condensed75;
} fam_set;

static embi_face *last_face(embi_text *t)
{
    embi_fontdb *db = embi_text_fonts(t);
    return db->faces.data[db->faces.len - 1];
}

static void add_fam(embi_text *t, const char *rel_family, uint16_t wmin, uint16_t wmax,
                    uint8_t style, float smin, float smax, embi_face **out)
{
    embi_face_desc d = {0};
    d.family = tx_s(rel_family);
    d.weight_min = wmin;
    d.weight_max = wmax;
    d.has_style = true;
    d.style = style;
    d.stretch_min = smin;
    d.stretch_max = smax;
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) == 1);
    *out = last_face(t);
}

static void build_fam(embi_text *t, fam_set *s)
{
    add_fam(t, "Fam", 400, 400, EMBI_STYLE_NORMAL, 100, 100, &s->normal400);
    add_fam(t, "Fam", 700, 700, EMBI_STYLE_NORMAL, 100, 100, &s->bold700);
    add_fam(t, "Fam", 400, 400, EMBI_STYLE_ITALIC, 100, 100, &s->italic400);
    add_fam(t, "Fam", 300, 300, EMBI_STYLE_NORMAL, 100, 100, &s->light300);
    add_fam(t, "Fam", 400, 400, EMBI_STYLE_NORMAL, 75, 75, &s->condensed75);
}

/* The best face of family "Fam" for the request. */
static embi_face *best(embi_text *t, float weight, uint8_t style, float stretch)
{
    embi_str fam = tx_s("Fam");
    embi_style_match m = {&fam, 1, weight, style, stretch};
    embi_face *out[8];
    uint32_t n = embi_fontdb_candidates(embi_text_fonts(t), &m, out, 8);
    return n > 0 ? out[0] : nullptr;
}

EMB_TEST(weight_above_500_prefers_heavier_then_lighter)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    /* 700 exact. */
    EMB_CHECK(best(t, 700.0f, EMBI_STYLE_NORMAL, 100.0f) == s.bold700);
    /* 600 (over 500): 700 is checked first (ascending), 400 is never needed. */
    EMB_CHECK(best(t, 600.0f, EMBI_STYLE_NORMAL, 100.0f) == s.bold700);
    embi_text_destroy(t);
}

EMB_TEST(weight_below_400_prefers_lighter_then_heavier)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    /* 350: weights at or below 350 first (descending): 300 wins over 400 and 700. */
    EMB_CHECK(best(t, 350.0f, EMBI_STYLE_NORMAL, 100.0f) == s.light300);
    /* 200: nothing at or below, so the nearest above in ascending order: 300. */
    EMB_CHECK(best(t, 200.0f, EMBI_STYLE_NORMAL, 100.0f) == s.light300);
    embi_text_destroy(t);
}

EMB_TEST(weight_between_400_and_500_checks_up_to_500_first)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    /* 450: the range [450, 500] has no face, then weights below 450 (400 wins), then above 500
     * (700). */
    EMB_CHECK(best(t, 450.0f, EMBI_STYLE_NORMAL, 100.0f) == s.normal400);
    embi_text_destroy(t);
}

EMB_TEST(style_italic_then_oblique_then_normal)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    EMB_CHECK(best(t, 400.0f, EMBI_STYLE_ITALIC, 100.0f) == s.italic400);
    /* Oblique is requested, no oblique face: italic is next in the list. */
    EMB_CHECK(best(t, 400.0f, EMBI_STYLE_OBLIQUE, 100.0f) == s.italic400);
    /* Normal: the normal faces; italic only as a last resort. */
    EMB_CHECK(best(t, 400.0f, EMBI_STYLE_NORMAL, 100.0f) == s.normal400);
    embi_text_destroy(t);
}

EMB_TEST(stretch_is_matched_before_weight)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    /* 75% exact: the condensed face, even though its weight is not the best match. */
    EMB_CHECK(best(t, 700.0f, EMBI_STYLE_NORMAL, 75.0f) == s.condensed75);
    /* 90%: values at or below 90 first (descending), so the 75% face. */
    EMB_CHECK(best(t, 400.0f, EMBI_STYLE_NORMAL, 90.0f) == s.condensed75);
    /* 150%: values at or above 150 first (none), then below descending: 100% beats 75%. */
    EMB_CHECK(best(t, 400.0f, EMBI_STYLE_NORMAL, 150.0f) == s.normal400);
    embi_text_destroy(t);
}

EMB_TEST(candidates_end_with_fallback_and_default)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    fam_set s;
    build_fam(t, &s);
    embi_str fams[2] = {tx_s("Missing"), tx_s("Fam")};
    embi_str fb[1] = {tx_s("Noto Sans")};
    EMB_REQUIRE(embi_fontdb_set_fallback(embi_text_fonts(t), fb, 1));
    embi_style_match m = {fams, 2, 400.0f, EMBI_STYLE_NORMAL, 100.0f};
    embi_face *out[8];
    uint32_t n = embi_fontdb_candidates(embi_text_fonts(t), &m, out, 8);
    /* "Missing" has no face. "Fam" gives normal400, then the fallback family "Noto Sans" gives
     * the bundled default (its own family), which is also the default face (listed once). */
    EMB_CHECK(out[0] == s.normal400);
    if (embi_text_has_default_font()) {
        EMB_REQUIRE(n == 2);
        EMB_CHECK(out[1] == embi_fontdb_default_face(embi_text_fonts(t)));
    } else {
        /* EMBER_NO_DEFAULT_FONT: no bundled face is appended. */
        EMB_REQUIRE(n == 1);
    }
    embi_text_destroy(t);
}

EMB_TEST(variable_face_takes_axis_values_from_the_style)
{
    embi_text *t = embi_text_create();
    EMB_REQUIRE(t != nullptr);
    uint32_t before = embi_fontdb_face_count(embi_text_fonts(t));
    EMB_REQUIRE(tx_add_file(t, "tests/fonts/NotoSans-VF.ttf", nullptr) == 1);
    embi_face *f = embi_text_fonts(t)->faces.data[before];
    embi_text_style st;
    embi_str fam = tx_s("Noto Sans");
    tx_style(&st, &fam, 1, 16.0f);
    st.weight = 700.0f;
    st.stretch = 80.0f;
    float user[2];
    embi_face_user_coords(f, &st, user);
    EMB_CHECK(user[f->ax_wght] == 700.0f);
    EMB_CHECK(user[f->ax_wdth] == 80.0f);
    /* font-variation-settings wins over the style, and is clamped to the axis. */
    embi_text_feature var[1] = {{EMBI_TAG('w', 'g', 'h', 't'), 0, 250.0f}};
    st.variations = var;
    st.variation_count = 1;
    embi_face_user_coords(f, &st, user);
    EMB_CHECK(user[f->ax_wght] == 250.0f);
    /* opsz is not an axis of this face, so it leaves the defaults alone. */
    EMB_CHECK(f->ax_opsz == -1);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
