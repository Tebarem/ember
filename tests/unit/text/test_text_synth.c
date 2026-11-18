#include "text_fixture.h"

#include "text/embolden.h"

#include "emb_render_test.h"

#include "../gpu/render_check.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

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

/* Emboldening (text/embolden.c). The contours are given by their starts, as the font gives them. */
static void add_line(embi_qcurve *c, float x0, float y0, float x1, float y1)
{
    *c = (embi_qcurve){x0, y0, 0.5f * (x0 + x1), 0.5f * (y0 + y1), x1, y1};
}

static void extent_of(const embi_qcurve *c, uint32_t n, float b[4])
{
    b[0] = b[1] = 1e30f;
    b[2] = b[3] = -1e30f;
    for (uint32_t i = 0; i < n; i++) {
        const float xs[2] = {c[i].x0, c[i].x1}, ys[2] = {c[i].y0, c[i].y1};
        for (int k = 0; k < 2; k++) {
            b[0] = xs[k] < b[0] ? xs[k] : b[0];
            b[2] = xs[k] > b[2] ? xs[k] : b[2];
            b[1] = ys[k] < b[1] ? ys[k] : b[1];
            b[3] = ys[k] > b[3] ? ys[k] : b[3];
        }
    }
}

/* Four lines through the corners of a square, counter-clockwise or clockwise (y up); the contour is closed. */
static void add_square(embi_qcurve *c, float x0, float y0, float x1, float y1, bool ccw)
{
    if (ccw) {
        add_line(&c[0], x0, y0, x1, y0);
        add_line(&c[1], x1, y0, x1, y1);
        add_line(&c[2], x1, y1, x0, y1);
        add_line(&c[3], x0, y1, x0, y0);
    } else {
        add_line(&c[0], x0, y0, x0, y1);
        add_line(&c[1], x0, y1, x1, y1);
        add_line(&c[2], x1, y1, x1, y0);
        add_line(&c[3], x1, y0, x0, y0);
    }
}

/* Not inf and not NaN (x - x is 0 for a finite x only). */
static bool finite_f(float x)
{
    return x - x == 0.0f;
}

static bool near(float a, float b, float tol)
{
    return SDL_fabsf(a - b) <= tol;
}

EMB_TEST(embolden_grows_the_outer_contour_and_shrinks_the_hole)
{
    embi_qcurve in[8], out[8], again[8];
    add_square(in, 0, 0, 100, 100, true);
    add_square(in + 4, 30, 30, 70, 70, false);
    const uint32_t starts[2] = {0, 4};
    embi_embolden(in, 8, starts, 2, 10.0f, 10.0f, false, out);
    embi_embolden(in, 8, starts, 2, 10.0f, 10.0f, false, again);
    float b[4];
    extent_of(out, 4, b);
    EMB_CHECK(near(b[0], -5.0f, 1e-3f) && near(b[1], -5.0f, 1e-3f));
    EMB_CHECK(near(b[2], 105.0f, 1e-3f) && near(b[3], 105.0f, 1e-3f));
    extent_of(out + 4, 4, b);
    EMB_CHECK(near(b[0], 35.0f, 1e-3f) && near(b[1], 35.0f, 1e-3f));
    EMB_CHECK(near(b[2], 65.0f, 1e-3f) && near(b[3], 65.0f, 1e-3f));
    for (uint32_t i = 0; i < 8; i++) {
        EMB_CHECK(again[i].x0 == out[i].x0 && again[i].y0 == out[i].y0 && again[i].cx == out[i].cx && again[i].cy == out[i].cy &&
                  again[i].x1 == out[i].x1 && again[i].y1 == out[i].y1);
    }
    /* The joins are kept: each curve starts where the one before it ended, and each contour closes on its start. */
    EMB_CHECK(out[0].x1 == out[1].x0 && out[0].y1 == out[1].y0);
    EMB_CHECK(out[3].x1 == out[0].x0 && out[3].y1 == out[0].y0);
    EMB_CHECK(out[7].x1 == out[4].x0 && out[7].y1 == out[4].y0);
}

/* A clockwise outer with a counter-clockwise hole (the CFF winding) grows and shrinks the same way. */
EMB_TEST(embolden_handles_a_clockwise_outer_and_a_counter_clockwise_hole)
{
    embi_qcurve in[8], out[8];
    add_square(in, 0, 0, 100, 100, false);
    add_square(in + 4, 30, 30, 70, 70, true);
    const uint32_t starts[2] = {0, 4};
    embi_embolden(in, 8, starts, 2, 10.0f, 10.0f, false, out);
    float b[4];
    extent_of(out, 4, b);
    EMB_CHECK(near(b[0], -5.0f, 1e-3f) && near(b[1], -5.0f, 1e-3f) && near(b[2], 105.0f, 1e-3f) && near(b[3], 105.0f, 1e-3f));
    extent_of(out + 4, 4, b);
    EMB_CHECK(near(b[0], 35.0f, 1e-3f) && near(b[1], 35.0f, 1e-3f) && near(b[2], 65.0f, 1e-3f) && near(b[3], 65.0f, 1e-3f));
}

/* Two separate outer contours of opposite winding (nesting depth 0 for both) both grow. */
EMB_TEST(embolden_grows_two_outer_contours_of_opposite_winding)
{
    embi_qcurve in[8], out[8];
    add_square(in, 0, 0, 100, 100, true);
    add_square(in + 4, 200, 0, 300, 100, false);
    const uint32_t starts[2] = {0, 4};
    embi_embolden(in, 8, starts, 2, 10.0f, 10.0f, false, out);
    float b[4];
    extent_of(out, 4, b);
    EMB_CHECK(near(b[0], -5.0f, 1e-3f) && near(b[2], 105.0f, 1e-3f));
    extent_of(out + 4, 4, b);
    EMB_CHECK(near(b[0], 195.0f, 1e-3f) && near(b[2], 305.0f, 1e-3f) && near(b[1], -5.0f, 1e-3f) && near(b[3], 105.0f, 1e-3f));
}

/* A curve with an off-midpoint control: the dome's middle moves out, and the straight base moves down by half the strength. */
EMB_TEST(embolden_moves_a_curved_edge_outward_with_an_off_midpoint_control)
{
    embi_qcurve in[2], out[2];
    /* (0,0) -> control (30,100) -> (100,0), then the base back to (0,0). The contour is clockwise, a single outer. */
    in[0] = (embi_qcurve){0, 0, 30, 100, 100, 0};
    add_line(&in[1], 100, 0, 0, 0);
    const uint32_t starts[1] = {0};
    embi_embolden(in, 2, starts, 1, 10.0f, 10.0f, false, out);
    EMB_CHECK(out[0].x1 == out[1].x0 && out[0].y1 == out[1].y0);
    /* The base's control (50,0) sits between two horizontal edges: it moves exactly 5 down. */
    EMB_CHECK(near(out[1].cx, 50.0f, 1e-3f) && near(out[1].cy, -5.0f, 1e-3f));
    /* The curve's midpoint (40, 50) moves outward, which is up for this dome. */
    const float my = 0.25f * out[0].y0 + 0.5f * out[0].cy + 0.25f * out[0].y1;
    SDL_Log("dome midpoint y 50 -> %.3f", (double)my);
    EMB_CHECK(my - 50.0f > 2.0f);
}

/* A contour that passes through its own start point mid-way is split only where the caller's starts say: a single contour
 * and two contours (cut at the touch) give different shifts. */
EMB_TEST(embolden_uses_the_given_contour_starts_not_coincident_points)
{
    /* One contour: the triangle (0,0) (100,0) (100,100), then a loop back through (0,0): (-50,50), (-50,-50), (0,0). */
    embi_qcurve in[6], one[6], split[6];
    add_line(&in[0], 0, 0, 100, 0);
    add_line(&in[1], 100, 0, 100, 100);
    add_line(&in[2], 100, 100, 0, 0);
    add_line(&in[3], 0, 0, -50, 50);
    add_line(&in[4], -50, 50, -50, -50);
    add_line(&in[5], -50, -50, 0, 0);
    const uint32_t one_start[1] = {0};
    const uint32_t two_starts[2] = {0, 3};
    embi_embolden(in, 6, one_start, 1, 10.0f, 10.0f, false, one);
    embi_embolden(in, 6, two_starts, 2, 10.0f, 10.0f, false, split);
    for (uint32_t i = 0; i < 6; i++) {
        EMB_CHECK(one[i].x1 == one[(i + 1) % 6].x0 && one[i].y1 == one[(i + 1) % 6].y0);
    }
    bool differs = false;
    for (uint32_t i = 0; i < 6; i++) {
        differs = differs || one[i].x0 != split[i].x0 || one[i].y0 != split[i].y0;
    }
    EMB_CHECK(differs);
}

/* A narrow counter (2 units across) does not invert: its shift is clamped to 0.45 of its smaller extent. */
EMB_TEST(embolden_does_not_invert_a_narrow_hole)
{
    embi_qcurve in[8], out[8];
    add_square(in, 0, 0, 100, 100, true);
    add_square(in + 4, 49, 49, 51, 51, false);
    const uint32_t starts[2] = {0, 4};
    embi_embolden(in, 8, starts, 2, 10.0f, 10.0f, false, out);
    float b[4];
    extent_of(out + 4, 4, b);
    EMB_CHECK(b[2] > b[0] && b[3] > b[1]);
    EMB_CHECK(b[2] - b[0] < 2.0f && b[2] - b[0] > 0.0f);
}

/* The bottom-left anchor keeps the left edge and the baseline and grows right by xstr and up by ystr. */
EMB_TEST(embolden_anchored_grows_right_and_up_from_the_bottom_left)
{
    embi_qcurve in[4], out[4];
    add_square(in, 0, 0, 100, 100, true);
    const uint32_t starts[1] = {0};
    embi_embolden(in, 4, starts, 1, 10.0f, 4.0f, true, out);
    float b[4];
    extent_of(out, 4, b);
    EMB_CHECK(near(b[0], 0.0f, 1e-3f) && near(b[1], 0.0f, 1e-3f));
    EMB_CHECK(near(b[2], 110.0f, 1e-3f) && near(b[3], 104.0f, 1e-3f));
}

/* A control point folding back on its edge gives finite curves (the incoming edge's normal is used). */
EMB_TEST(embolden_a_folded_corner_stays_finite)
{
    embi_qcurve in[2], out[2];
    in[0] = (embi_qcurve){0, 0, 100, 0, 50, 0};
    add_line(&in[1], 50, 0, 0, 0);
    const uint32_t starts[1] = {0};
    embi_embolden(in, 2, starts, 1, 10.0f, 10.0f, false, out);
    for (uint32_t i = 0; i < 2; i++) {
        EMB_CHECK(finite_f(out[i].x0) && finite_f(out[i].y0) && finite_f(out[i].cx) && finite_f(out[i].cy) &&
                  finite_f(out[i].x1) && finite_f(out[i].y1));
    }
}

/* ------------------------------------------------------------------ rendered synthetic bold */

#define RS_W 256
#define RS_H 180

typedef struct rs_item {
    const char *str;
    float weight, size, x, y;
    uint32_t color;
} rs_item;

typedef struct rs_scene {
    embi_text *t;
    const char *family;
    const rs_item *items;
    uint32_t n;
    uint32_t bg;
} rs_scene;

static void rs_draw(embi_frame *f, void *user)
{
    rs_scene *sc = user;
    const float page[4] = {0, 0, (float)RS_W, (float)RS_H};
    const float square[4] = {0, 0, 0, 0};
    embi_frame_fill(f, page, square, sc->bg, 0, 0);
    embi_frame_set_text_aa(f, EMBI_TEXT_AA_GRAY, false);
    embi_str fam = tx_s(sc->family);
    for (uint32_t i = 0; i < sc->n; i++) {
        const rs_item *it = &sc->items[i];
        embi_text_style st;
        tx_style(&st, &fam, 1, it->size);
        st.weight = it->weight;
        st.synth = EMBI_SYNTH_ALL;
        embi_text_layout lay;
        if (!tx_shape(sc->t, it->str, &st, &lay)) {
            continue;
        }
        embi_text_vruns vr = {0};
        if (embi_text_line(&lay, 0, lay.text_len, &vr)) {
            embi_text_draw(sc->t, f, &lay, &vr, it->x, it->y, it->color, 0, 0);
            embi_text_vruns_free(&vr);
        }
        embi_text_layout_free(&lay);
    }
}

/* Renders the items into RS_W x RS_H pixels (RGBA8, SDL_free them); nullptr when there is no GPU device. */
static uint8_t *rs_render(embi_text *t, const rs_item *items, uint32_t n, uint32_t bg)
{
    rs_scene sc = {t, "Synth Test", items, n, bg};
    return emb_test_render(RS_W, RS_H, rs_draw, &sc);
}

/* A text system with the regular face under the family "Synth Test" (nullptr when it does not open). */
static embi_text *rs_text(void)
{
    embi_text *t = embi_text_create();
    if (t == nullptr) {
        return nullptr;
    }
    embi_face_desc d = {0};
    d.family = tx_s("Synth Test");
    if (tx_add_file(t, "tests/fonts/NotoSans-Regular.ttf", &d) != 1u) {
        embi_text_destroy(t);
        return nullptr;
    }
    return t;
}

/* Black ink coverage of a pixel of an opaque white render. */
static float rs_cov(const uint8_t *px, int x, int y)
{
    return (255.0f - (float)px[((size_t)y * RS_W + (size_t)x) * 4u]) / 255.0f;
}

/* The sum of coverage over the pixel row y, columns [x0, x1). */
static float rs_row_sum(const uint8_t *px, int y, int x0, int x1)
{
    float s = 0.0f;
    for (int x = x0; x < x1; x++) {
        s += rs_cov(px, x, y);
    }
    return s;
}

/* A translucent bold run has no pixel above its colour alpha: the copies of the old four-copy path overlapped and blended. */
EMB_TEST(synthetic_bold_has_no_overlap_darkening)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = rs_text();
    EMB_REQUIRE(t != nullptr);
    const rs_item items[] = {{"Hlo", 700.0f, 16.0f, 4.0f, 28.0f, RGBA(0, 0, 0, 128)},
                             {"Hlo", 700.0f, 48.0f, 4.0f, 120.0f, RGBA(0, 0, 0, 128)}};
    uint8_t *px = rs_render(t, items, 2, 0);
    EMB_RENDER_OR_SKIP(px);
    int max_a = 0, inked = 0;
    for (size_t i = 0; i < (size_t)RS_W * RS_H; i++) {
        const int a = px[i * 4u + 3u];
        max_a = a > max_a ? a : max_a;
        inked += a > 0;
    }
    SDL_Log("translucent bold: max alpha %d, inked pixels %d", max_a, inked);
    EMB_CHECK(inked > 0);
    EMB_CHECK(max_a <= 129);
    SDL_free(px);
    embi_text_destroy(t);
}

/* The stem of 'l' grows by the emboldening (size / 24 at 16 px and 24 px, 1/32 from 36 px, Skia's fake bold) against the
 * regular glyph drawn in the same mode. */
EMB_TEST(synthetic_bold_grows_the_stem_by_its_strength)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = rs_text();
    EMB_REQUIRE(t != nullptr);
    const float sizes[3] = {16.0f, 24.0f, 48.0f};
    const float baselines[3] = {28.0f, 72.0f, 160.0f};
    rs_item items[6];
    for (uint32_t k = 0; k < 3; k++) {
        items[2 * k] = (rs_item){"l", 400.0f, sizes[k], 4.0f, baselines[k], RGBA(0, 0, 0, 255)};
        items[2 * k + 1] = (rs_item){"l", 700.0f, sizes[k], 124.0f, baselines[k], RGBA(0, 0, 0, 255)};
    }
    uint8_t *px = rs_render(t, items, 6, RGBA(255, 255, 255, 255));
    EMB_RENDER_OR_SKIP(px);
    for (uint32_t k = 0; k < 3; k++) {
        const float size = sizes[k];
        /* Skia's fake bold: a 24th of the em up to 9 px, a 32nd from 36 px, linear between. */
        const float kk = size <= 9.0f ? 1.0f / 24.0f : (size >= 36.0f ? 1.0f / 32.0f : 1.0f / 24.0f - (size - 9.0f) / 27.0f * (1.0f / 24.0f - 1.0f / 32.0f));
        const float want = size * kk;
        /* A row well inside the stem, away from the foot and the top. */
        const int y = (int)SDL_floorf(baselines[k] - 0.3f * size);
        const float reg = rs_row_sum(px, y, 0, 120);
        const float bold = rs_row_sum(px, y, 120, RS_W);
        SDL_Log("stem %.0fpx: regular %.3f bold %.3f growth %.3f want %.3f", (double)size, (double)reg, (double)bold,
                (double)(bold - reg), (double)want);
        EMB_CHECK(SDL_fabsf((bold - reg) - want) <= 0.35f);
    }
    SDL_free(px);
    embi_text_destroy(t);
}

/* A bold 'o' keeps its hole at 16 px, and the emboldening is deterministic (two renders are equal). */
EMB_TEST(synthetic_bold_o_keeps_its_hole_and_is_deterministic)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = rs_text();
    EMB_REQUIRE(t != nullptr);
    const rs_item items[] = {{"o", 700.0f, 16.0f, 4.0f, 28.0f, RGBA(0, 0, 0, 255)}};
    uint8_t *a = rs_render(t, items, 1, RGBA(255, 255, 255, 255));
    EMB_RENDER_OR_SKIP(a);
    uint8_t *b = rs_render(t, items, 1, RGBA(255, 255, 255, 255));
    EMB_RENDER_OR_SKIP(b);
    EMB_CHECK(SDL_memcmp(a, b, (size_t)RS_W * RS_H * 4u) == 0);
    /* The middle row of the x-height: ink, a run of at least 2 empty columns (the counter), then ink again. */
    const int y = (int)SDL_floorf(28.0f - 0.27f * 16.0f);
    int first = -1, last = -1;
    for (int x = 0; x < 60; x++) {
        if (rs_cov(a, x, y) > 0.8f) {
            first = first < 0 ? x : first;
            last = x;
        }
    }
    EMB_REQUIRE(first >= 0 && last > first + 4);
    int gap = 0, widest = 0;
    for (int x = first; x <= last; x++) {
        if (rs_cov(a, x, y) < 0.05f) {
            gap++;
            widest = gap > widest ? gap : widest;
        } else {
            gap = 0;
        }
    }
    SDL_Log("bold o at 16 px: widest counter gap %d columns", widest);
    EMB_CHECK(widest >= 2);
    SDL_free(a);
    SDL_free(b);
    embi_text_destroy(t);
}

/* The top of a hinted bold H is the strength rounded to whole device px (1 px at 16 and 24 px) above the regular H's top, and the
 * baseline row is the same. */
static int rs_top_row(const uint8_t *px, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (rs_cov(px, x, y) > 0.5f) {
                return y;
            }
        }
    }
    return -1;
}

static int rs_bottom_row(const uint8_t *px, int x0, int x1, int y0, int y1)
{
    for (int y = y1 - 1; y >= y0; y--) {
        for (int x = x0; x < x1; x++) {
            if (rs_cov(px, x, y) > 0.5f) {
                return y;
            }
        }
    }
    return -1;
}

EMB_TEST(synthetic_bold_grows_hinted_tops_by_whole_device_px)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_text *t = rs_text();
    EMB_REQUIRE(t != nullptr);
    const rs_item items[] = {{"H", 400.0f, 16.0f, 4.0f, 30.0f, RGBA(0, 0, 0, 255)},
                             {"H", 700.0f, 16.0f, 64.0f, 30.0f, RGBA(0, 0, 0, 255)},
                             {"H", 400.0f, 24.0f, 4.0f, 90.0f, RGBA(0, 0, 0, 255)},
                             {"H", 700.0f, 24.0f, 64.0f, 90.0f, RGBA(0, 0, 0, 255)}};
    uint8_t *px = rs_render(t, items, 4, RGBA(255, 255, 255, 255));
    EMB_RENDER_OR_SKIP(px);
    const int bands[2][2] = {{5, 40}, {45, 100}};
    const float sizes[2] = {16.0f, 24.0f};
    for (int k = 0; k < 2; k++) {
        const int reg_top = rs_top_row(px, 0, 60, bands[k][0], bands[k][1]);
        const int bold_top = rs_top_row(px, 60, 120, bands[k][0], bands[k][1]);
        const int reg_bot = rs_bottom_row(px, 0, 60, bands[k][0], bands[k][1]);
        const int bold_bot = rs_bottom_row(px, 60, 120, bands[k][0], bands[k][1]);
        SDL_Log("hinted H %.0fpx: regular rows %d..%d, bold rows %d..%d", (double)sizes[k], reg_top, reg_bot, bold_top, bold_bot);
        EMB_REQUIRE(reg_top >= 0 && bold_top >= 0);
        EMB_CHECK(reg_top - bold_top == 1);
        EMB_CHECK(reg_bot == bold_bot);
    }
    SDL_free(px);
    embi_text_destroy(t);
}

EMB_TEST_MAIN()
