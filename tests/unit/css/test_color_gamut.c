/*
 * Color 4/5 behavior beyond the conformance files (W2c): relative color syntax, gamut mapping and
 * 8-bit sRGB output, wide-gamut conversions, color-mix in hsl/hwb/wide spaces and its hue methods.
 */
#include "emb_test.h"

#include "css/props.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool parse_str(embi_arena *a, const char *s, embi_color *out)
{
    embi_css_slice cvs = embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
    return embi_color_parse(cvs, out);
}

static void serialize(const embi_color *c, char *buf, size_t cap)
{
    embi_sb sb = {0};
    embi_color_serialize(&sb, c);
    size_t n = sb.len < cap - 1 ? sb.len : cap - 1;
    memcpy(buf, sb.data ? sb.data : "", n);
    buf[n] = '\0';
    embi_sb_free(&sb);
}

static bool expect_text(const char *input, const char *want)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c;
    bool ok = parse_str(&a, input, &c);
    char got[256] = "";
    if (ok) {
        serialize(&c, got, sizeof got);
    }
    if (!ok || strcmp(got, want) != 0) {
        fprintf(stderr, "  %s: want %s got %s\n", input, want, ok ? got : "(invalid)");
    }
    embi_arena_free(&a);
    return ok && strcmp(got, want) == 0;
}

static bool srgb8_of(const char *input, uint8_t rgba[4])
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c;
    bool ok = parse_str(&a, input, &c) && embi_color_to_srgb8(&c, rgba);
    embi_arena_free(&a);
    return ok;
}

/* ------------------------------------------------------------------ relative color syntax */

EMB_TEST(relative_rgb_keeps_channels)
{
    EMB_CHECK(expect_text("rgb(from red r g b)", "rgb(255, 0, 0)"));
    EMB_CHECK(expect_text("rgb(from #336699 r g b / 0.5)", "rgba(51, 102, 153, 0.5)"));
    EMB_CHECK(expect_text("rgb(from red r g b / alpha)", "rgb(255, 0, 0)"));
    EMB_CHECK(expect_text("rgb(from rgb(10 20 30 / 0.25) r g b / calc(alpha * 2))",
                          "rgba(10, 20, 30, 0.5)"));
}

EMB_TEST(relative_hsl_substitutes_channels)
{
    /* Red is hsl(0 100% 50%). Lightness 50 + 10 gives hsl(0 100% 60%) = rgb(255 51 51). */
    EMB_CHECK(expect_text("hsl(from red h s calc(l + 10))", "rgb(255, 51, 51)"));
    EMB_CHECK(expect_text("hsl(from red h s l)", "rgb(255, 0, 0)"));
}

EMB_TEST(relative_color_function_uses_its_space)
{
    EMB_CHECK(expect_text("color(from red srgb r g b)", "color(srgb 1 0 0)"));
}

EMB_TEST(relative_lab_and_oklch_round_trip)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color base, rel;
    EMB_REQUIRE(parse_str(&a, "oklch(0.7 0.1 120)", &base));
    EMB_REQUIRE(parse_str(&a, "oklch(from oklch(0.7 0.1 120) l c h)", &rel));
    /* The OKLab matrices carry ten digits, so a round trip is good to about 1e-8. */
    EMB_CHECK(fabs(rel.c[0] - base.c[0]) < 1e-6);
    EMB_CHECK(fabs(rel.c[1] - base.c[1]) < 1e-6);
    EMB_CHECK(fabs(rel.c[2] - base.c[2]) < 1e-4);
    /* Changing one channel keeps the others. */
    EMB_REQUIRE(parse_str(&a, "oklch(from oklch(0.7 0.1 120) calc(l + 0.1) c h)", &rel));
    EMB_CHECK(fabs(rel.c[0] - 0.8) < 1e-6);
    EMB_CHECK(fabs(rel.c[2] - 120) < 1e-4);
    embi_arena_free(&a);
}

EMB_TEST(relative_invalid_forms_are_rejected)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c;
    EMB_CHECK(!parse_str(&a, "rgb(from red r g z)", &c));
    EMB_CHECK(!parse_str(&a, "color(from red nope r g b)", &c));
    /* currentcolor as the origin is accepted and resolved at computed-value time (a deferred color) */
    EMB_CHECK(parse_str(&a, "rgb(from currentcolor r g b)", &c));
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ gamut mapping and 8-bit */

EMB_TEST(in_gamut_colors_convert_exactly)
{
    uint8_t px[4];
    EMB_CHECK(srgb8_of("rgb(10, 20, 30)", px));
    EMB_CHECK(px[0] == 10 && px[1] == 20 && px[2] == 30 && px[3] == 255);
    EMB_CHECK(srgb8_of("rgb(0 0 0 / 0.5)", px));
    EMB_CHECK(px[3] == 128 || px[3] == 127);
    EMB_CHECK(srgb8_of("#ff8000", px));
    EMB_CHECK(px[0] == 255 && px[1] == 128 && px[2] == 0);
}

EMB_TEST(out_of_gamut_colors_are_mapped)
{
    /* Display P3 red is outside sRGB. Gamut mapping keeps it red, but less saturated: green and
     * blue move up from zero, and the result is inside the 8-bit range. */
    uint8_t px[4];
    EMB_CHECK(srgb8_of("color(display-p3 1 0 0)", px));
    EMB_CHECK(px[0] >= 250);
    EMB_CHECK(px[1] > 0 && px[2] > 0);
    EMB_CHECK(px[1] < 120 && px[2] < 120);
    /* A color beyond white maps to white; beyond black maps to black. */
    EMB_CHECK(srgb8_of("oklch(1.2 0 0)", px));
    EMB_CHECK(px[0] == 255 && px[1] == 255 && px[2] == 255);
    EMB_CHECK(srgb8_of("oklch(-0.1 0 0)", px));
    EMB_CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0);
    /* Mapped results never leave the gamut: the sRGB float path returns components in 0..1. */
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c, s;
    EMB_REQUIRE(parse_str(&a, "oklch(0.7 0.4 150)", &c));
    EMB_REQUIRE(embi_color_to_srgb(&c, &s));
    EMB_CHECK(s.c[0] >= 0 && s.c[0] <= 1 && s.c[1] >= 0 && s.c[1] <= 1 && s.c[2] >= 0 && s.c[2] <= 1);
    embi_arena_free(&a);
}

EMB_TEST(xyz_white_maps_to_srgb_white)
{
    /* D65 white in XYZ lands on sRGB (1, 1, 1). The gamut mapping returns the clipped color within
     * its chroma search tolerance (1e-4), so the result is within 2e-4 of white. */
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c, s;
    EMB_REQUIRE(parse_str(&a, "color(xyz 0.9504559 1 1.0890578)", &c));
    EMB_REQUIRE(embi_color_to_srgb(&c, &s));
    EMB_CHECK(fabs(s.c[0] - 1.0) < 2e-4 && fabs(s.c[1] - 1.0) < 2e-4 && fabs(s.c[2] - 1.0) < 2e-4);
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ color-mix */

EMB_TEST(color_mix_hsl_and_hwb_values)
{
    /* Red and blue in hsl take the shorter hue arc through magenta. */
    EMB_CHECK(expect_text("color-mix(in hsl, red, blue)", "rgb(255, 0, 255)"));
    EMB_CHECK(expect_text("color-mix(in hwb, red, blue)", "rgb(255, 0, 255)"));
    /* The longer arc goes through green. */
    EMB_CHECK(expect_text("color-mix(in hsl longer hue, red, blue)", "rgb(0, 255, 0)"));
    /* Increasing: from 0 up to 240 passes through green; decreasing passes through magenta. */
    EMB_CHECK(expect_text("color-mix(in hsl increasing hue, red, blue)", "rgb(0, 255, 0)"));
    EMB_CHECK(expect_text("color-mix(in hsl decreasing hue, red, blue)", "rgb(255, 0, 255)"));
}

EMB_TEST(color_mix_wide_gamut_spaces)
{
    /* Mixing a color with itself returns it in any space. */
    EMB_CHECK(expect_text("color-mix(in display-p3, color(display-p3 0.5 0.2 0.8), "
                          "color(display-p3 0.5 0.2 0.8))",
                          "color(display-p3 0.5 0.2 0.8)"));
    EMB_CHECK(expect_text("color-mix(in rec2020, color(rec2020 0.3 0.6 0.9), "
                          "color(rec2020 0.3 0.6 0.9))",
                          "color(rec2020 0.3 0.6 0.9)"));
    EMB_CHECK(expect_text("color-mix(in a98-rgb, color(a98-rgb 0.2 0.4 0.6), "
                          "color(a98-rgb 0.2 0.4 0.6))",
                          "color(a98-rgb 0.2 0.4 0.6)"));
    EMB_CHECK(expect_text("color-mix(in prophoto-rgb, color(prophoto-rgb 0.2 0.4 0.6), "
                          "color(prophoto-rgb 0.2 0.4 0.6))",
                          "color(prophoto-rgb 0.2 0.4 0.6)"));
}

EMB_TEST(color_mix_wide_gamut_midpoint)
{
    /* Halfway between two sRGB primaries in display-p3 is a closed-form blend of the gamma
     * encoded p3 channels: 0.5 and 0.5 mix to 0.5 for the same channel. */
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_color c;
    EMB_REQUIRE(parse_str(&a,
                          "color-mix(in display-p3, color(display-p3 1 0 0), "
                          "color(display-p3 0 0 1))",
                          &c));
    EMB_CHECK(fabs(c.c[0] - 0.5) < 1e-9);
    EMB_CHECK(fabs(c.c[1]) < 1e-9);
    EMB_CHECK(fabs(c.c[2] - 0.5) < 1e-9);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
