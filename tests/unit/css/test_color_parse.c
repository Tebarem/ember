/*
 * Color parsing and serialization beyond the css-parsing-tests suite (W2c): keywords, the
 * currentcolor and system colors, color-mix(), conversions to sRGB, clamping and the invalid
 * forms. The suite in test_color_suite.c covers the conformance cases.
 */
#include "emb_test.h"

#include "css/props.h"

#include <string.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

/* Parses a color text. Returns true on success and writes the serialization into *sb. */
static bool color_text(const char *s, embi_sb *sb)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c;
    bool ok = embi_color_parse(text_of(&a, s), &c);
    if (ok) {
        embi_color_serialize(sb, &c);
    }
    embi_arena_free(&a);
    return ok;
}

/* True when the text parses as one color. */
static bool color_ok(const char *s)
{
    embi_sb sb = {0};
    bool ok = color_text(s, &sb);
    embi_sb_free(&sb);
    return ok;
}

static bool serializes_to(const char *input, const char *want)
{
    embi_sb sb = {0};
    bool ok = color_text(input, &sb);
    bool same = ok && sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    embi_sb_free(&sb);
    return same;
}

EMB_TEST(color_hex_and_named)
{
    EMB_CHECK(serializes_to("#f00", "rgb(255, 0, 0)"));
    EMB_CHECK(serializes_to("#11223344", "rgba(17, 34, 51, 0.266667)"));
    EMB_CHECK(serializes_to("#ABCDEF", "rgb(171, 205, 239)"));
    EMB_CHECK(serializes_to("RebeccaPurple", "rgb(102, 51, 153)"));
    EMB_CHECK(serializes_to("transparent", "rgba(0, 0, 0, 0)"));
    EMB_CHECK(!color_ok("#ff"));
    EMB_CHECK(!color_ok("#gggggg"));
    EMB_CHECK(!color_ok("notacolor"));
    EMB_CHECK(!color_ok("red blue"));
}

EMB_TEST(color_keywords_system_and_currentcolor)
{
    EMB_CHECK(serializes_to("currentcolor", "currentcolor"));
    EMB_CHECK(serializes_to("CurrentColor", "currentcolor"));
    EMB_CHECK(serializes_to("CanvasText", "CanvasText"));
    EMB_CHECK(serializes_to("canvastext", "CanvasText"));
    EMB_CHECK(serializes_to("  red  ", "rgb(255, 0, 0)"));
}

EMB_TEST(color_legacy_and_modern_functions)
{
    EMB_CHECK(serializes_to("rgb(255 0 0 / 50%)", "rgba(255, 0, 0, 0.5)"));
    EMB_CHECK(serializes_to("rgb(300, -5, 0)", "rgb(255, 0, 0)"));
    EMB_CHECK(serializes_to("rgba(0, 0, 255, 0.25)", "rgba(0, 0, 255, 0.25)"));
    EMB_CHECK(serializes_to("hsl(120 100% 50%)", "rgb(0, 255, 0)"));
    EMB_CHECK(serializes_to("hwb(0 0% 0%)", "rgb(255, 0, 0)"));
    EMB_CHECK(serializes_to("lab(50 40 30)", "lab(50 40 30)"));
    EMB_CHECK(serializes_to("lab(50% 0% 0%)", "lab(50 0 0)"));
    EMB_CHECK(serializes_to("lch(50 40 30deg / 0.5)", "lch(50 40 30 / 0.5)"));
    EMB_CHECK(serializes_to("oklch(0.5 0.1 400)", "oklch(0.5 0.1 40)"));
    EMB_CHECK(serializes_to("oklab(0.5 0.1 -0.1)", "oklab(0.5 0.1 -0.1)"));
    EMB_CHECK(serializes_to("color(display-p3 1 0 0)", "color(display-p3 1 0 0)"));
    EMB_CHECK(serializes_to("color(xyz-d50 0.5 0.5 0.5)", "color(xyz-d50 0.5 0.5 0.5)"));
    EMB_CHECK(serializes_to("color(srgb 0.25 none 1)", "color(srgb 0.25 none 1)"));
    EMB_CHECK(serializes_to("device-cmyk(0 100% 0% 0% / 1)", "color(device-cmyk 0 1 0 0)"));

    /* Legacy rgb() cannot mix numbers and percentages. */
    EMB_CHECK(!color_ok("rgb(0, 0%, 0)"));
    /* Legacy hsl() needs percentages for saturation and lightness. */
    EMB_CHECK(!color_ok("hsl(0, 0, 0)"));
    /* Modern syntax allows none; legacy does not. */
    EMB_CHECK(!color_ok("rgb(none, 0, 0)"));
    /* Unknown colorspaces and malformed functions. */
    EMB_CHECK(!color_ok("color(unknown-space 0 0 0)"));
    EMB_CHECK(!color_ok("rgb(1, 2)"));
    EMB_CHECK(!color_ok("lab(50%)"));
    EMB_CHECK(!color_ok("rgb(0 0 0 0 0)"));
    EMB_CHECK(!color_ok("hwb(0 0% 0% / )"));
}

EMB_TEST(color_mix_in_srgb_and_others)
{
    EMB_CHECK(serializes_to("color-mix(in srgb, red, blue)", "rgb(127.5, 0, 127.5)"));
    EMB_CHECK(serializes_to("color-mix(in srgb, red 25%, blue)", "rgb(63.75, 0, 191.25)"));
    /* With both percentages missing or summing under 100%, the result is scaled by the sum. */
    EMB_CHECK(serializes_to("color-mix(in srgb, red 20%, blue 20%)", "rgba(127.5, 0, 127.5, 0.4)"));
    EMB_CHECK(serializes_to("color-mix(in srgb-linear, #fff, #000)", "color(srgb-linear 0.5 0.5 0.5)"));
    EMB_CHECK(color_ok("color-mix(in oklab, white, black)"));
    EMB_CHECK(color_ok("color-mix(in hsl shorter hue, red, blue)"));
    EMB_CHECK(color_ok("color-mix(in lch, red, blue 30%)"));
    /* Invalid: missing space, unknown space, bad hue method, zero total weight. */
    EMB_CHECK(!color_ok("color-mix(red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in nowhere, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in hsl longest hue, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in srgb, red 0%, blue 0%)"));
}

EMB_TEST(color_conversion_to_srgb)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color lab;
    EMB_REQUIRE(embi_color_parse(text_of(&a, "lab(100 0 0)"), &lab));
    embi_color out;
    EMB_REQUIRE(embi_color_to_srgb(&lab, &out));
    EMB_CHECK_EQ_INT(out.space, EMBI_CS_SRGB);
    /* D50 white maps to sRGB white within rounding. */
    EMB_CHECK(__builtin_fabs(out.c[0] - 1.0) < 1e-3);
    EMB_CHECK(__builtin_fabs(out.c[2] - 1.0) < 1e-3);

    embi_color red;
    EMB_REQUIRE(embi_color_parse(text_of(&a, "red"), &red));
    EMB_REQUIRE(embi_color_to_srgb(&red, &out));
    EMB_CHECK(out.c[0] == 1.0 && out.c[1] == 0.0);
    embi_arena_free(&a);
}

EMB_TEST(color_value_serialization_rounding)
{
    embi_sb sb = {0};
    embi_css_append_num(&sb, 0.1 + 0.2);
    EMB_CHECK(sb.len == 3 && memcmp(sb.data, "0.3", 3) == 0);
    embi_sb_free(&sb);
    sb = (embi_sb){0};
    embi_css_append_num(&sb, -0.0000001);
    EMB_CHECK(sb.len == 1 && sb.data[0] == '0');
    embi_sb_free(&sb);
    sb = (embi_sb){0};
    embi_css_append_num(&sb, 123456.7890123);
    EMB_CHECK(sb.len == 13 && memcmp(sb.data, "123456.789012", 13) == 0);
    embi_sb_free(&sb);
    sb = (embi_sb){0};
    embi_css_append_num(&sb, 999.9999999);
    EMB_CHECK(sb.len == 4 && memcmp(sb.data, "1000", 4) == 0);
    embi_sb_free(&sb);
}

/* Lab and LCH are relative to the D50 white (x 0.3457, y 0.3585: X 0.96429567643, Z 0.82510460251)
 * and reach sRGB through the Bradford adaptation. The reference white must come out exactly
 * neutral, and L 50 must give the known mid gray. */
EMB_TEST(lab_uses_the_exact_d50_white)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    static const char *const grays[] = {"lab(100 0 0)", "lab(50 0 0)", "lab(0 0 0)",
                                        "lch(100 0 0)", "lch(50 0 0)",
                                        "color(xyz-d50 0.96429567643 1 0.82510460251)"};
    static const double want[] = {1.0, 0.46626, 0.0, 1.0, 0.46626, 1.0};
    for (size_t i = 0; i < sizeof grays / sizeof grays[0]; i++) {
        embi_color c, out;
        EMB_REQUIRE(embi_color_parse(text_of(&a, grays[i]), &c));
        EMB_REQUIRE(embi_color_to_srgb(&c, &out));
        EMB_CHECK(__builtin_fabs(out.c[0] - out.c[1]) < 1e-9);
        EMB_CHECK(__builtin_fabs(out.c[1] - out.c[2]) < 1e-9);
        EMB_CHECK(__builtin_fabs(out.c[0] - want[i]) < 1e-4);
    }
    /* A chromatic value: sRGB red expressed in Lab D50 converts back to red. */
    embi_color c, out;
    EMB_REQUIRE(embi_color_parse(text_of(&a, "lab(54.2905 80.8049 69.8910)"), &c));
    EMB_REQUIRE(embi_color_to_srgb(&c, &out));
    EMB_CHECK(__builtin_fabs(out.c[0] - 1.0) < 2e-3);
    EMB_CHECK(__builtin_fabs(out.c[1]) < 2e-3);
    EMB_CHECK(__builtin_fabs(out.c[2]) < 2e-3);
    embi_arena_free(&a);
}

/* color-mix() and gradients share the interpolation spaces; an unknown one is invalid. */
EMB_TEST(color_mix_rejects_unknown_spaces)
{
    EMB_CHECK(!color_ok("color-mix(in lab-d65, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in rgb, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in display-p4, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in custom-profile, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in, red, blue)"));
    EMB_CHECK(!color_ok("color-mix(in oklab longer hue, red, blue)"));
    EMB_CHECK(color_ok("color-mix(in xyz-d50, red, blue)"));
    EMB_CHECK(color_ok("color-mix(in oklch longer hue, red, blue)"));
}

EMB_TEST_MAIN()
