/*
 * Color interpolation (the gradient mixer) and deferred currentcolor colors (W4g3): missing and powerless components,
 * display-p3-linear, hue methods, and the evaluation of a relative color or color-mix() against a concrete current
 * color.
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

static bool near8(const uint8_t got[4], int r, int g, int b)
{
    bool ok = abs(got[0] - r) <= 1 && abs(got[1] - g) <= 1 && abs(got[2] - b) <= 1;
    if (!ok) {
        fprintf(stderr, "  want %d %d %d got %d %d %d\n", r, g, b, got[0], got[1], got[2]);
    }
    return ok;
}

/* Mixes two written colors at weight w in the given space and returns the sRGB8 result. */
static bool mix8(const char *ca, const char *cb, uint8_t space, uint8_t hue, double w, uint8_t out[4])
{
    embi_arena ar = {0};
    embi_arena_init(&ar, 2048);
    embi_color a, b, m;
    bool ok = parse_str(&ar, ca, &a) && parse_str(&ar, cb, &b);
    embi_color_mixer *mx = nullptr;
    ok = ok && embi_color_mixer_init(&mx, &a, &b, space, hue);
    if (ok) {
        embi_color_mixer_at(mx, w, &m);
        ok = embi_color_to_srgb8(&m, out);
        embi_color_mixer_free(mx);
    }
    embi_arena_free(&ar);
    return ok;
}

EMB_TEST(mixer_srgb_midpoint_and_ends)
{
    uint8_t px[4];
    EMB_REQUIRE(mix8("rgb(0 0 0)", "rgb(200 100 50)", EMBI_IS_SRGB, EMBI_HUE_SHORTER, 0.5, px));
    EMB_CHECK(near8(px, 100, 50, 25));
    EMB_REQUIRE(mix8("rgb(0 0 0)", "rgb(200 100 50)", EMBI_IS_SRGB, EMBI_HUE_SHORTER, 0.0, px));
    EMB_CHECK(near8(px, 0, 0, 0));
    EMB_REQUIRE(mix8("rgb(0 0 0)", "rgb(200 100 50)", EMBI_IS_SRGB, EMBI_HUE_SHORTER, 1.0, px));
    EMB_CHECK(near8(px, 200, 100, 50));
}

EMB_TEST(mixer_missing_component_takes_the_other_value)
{
    uint8_t px[4];
    /* the red channel of the first color is missing: it takes 0.8 from the second at every weight */
    EMB_REQUIRE(mix8("color(srgb none 0 0)", "color(srgb 0.8 0.4 0)", EMBI_IS_SRGB, EMBI_HUE_SHORTER, 0.0, px));
    EMB_CHECK(near8(px, 204, 0, 0));
    EMB_REQUIRE(mix8("color(srgb none 0 0)", "color(srgb 0.8 0.4 0)", EMBI_IS_SRGB, EMBI_HUE_SHORTER, 0.5, px));
    EMB_CHECK(near8(px, 204, 51, 0));
}

EMB_TEST(mixer_powerless_hue_of_a_gray_is_ignored)
{
    uint8_t px[4];
    /* hsl(120 0% 50%) is a gray: its hue does not pull the blend towards green; the midpoint has hue 0, s 50%, l 50% */
    EMB_REQUIRE(mix8("hsl(120 0% 50%)", "hsl(0 100% 50%)", EMBI_IS_HSL, EMBI_HUE_SHORTER, 0.5, px));
    EMB_CHECK(near8(px, 191, 64, 64));
}

EMB_TEST(mixer_hue_methods)
{
    uint8_t px[4];
    /* hue 10 to 350: shorter goes through 0 (red), longer through 180 (cyan) */
    EMB_REQUIRE(mix8("hsl(10 100% 50%)", "hsl(350 100% 50%)", EMBI_IS_HSL, EMBI_HUE_SHORTER, 0.5, px));
    EMB_CHECK(near8(px, 255, 0, 0));
    EMB_REQUIRE(mix8("hsl(10 100% 50%)", "hsl(350 100% 50%)", EMBI_IS_HSL, EMBI_HUE_LONGER, 0.5, px));
    EMB_CHECK(near8(px, 0, 255, 255));
    /* increasing from 10 to 350 passes through 180 too; decreasing goes through 0 */
    EMB_REQUIRE(mix8("hsl(10 100% 50%)", "hsl(350 100% 50%)", EMBI_IS_HSL, EMBI_HUE_INCREASING, 0.5, px));
    EMB_CHECK(near8(px, 0, 255, 255));
    EMB_REQUIRE(mix8("hsl(10 100% 50%)", "hsl(350 100% 50%)", EMBI_IS_HSL, EMBI_HUE_DECREASING, 0.5, px));
    EMB_CHECK(near8(px, 255, 0, 0));
}

EMB_TEST(display_p3_linear_white_and_black)
{
    embi_arena ar = {0};
    embi_arena_init(&ar, 1024);
    embi_color c;
    uint8_t px[4];
    EMB_REQUIRE(parse_str(&ar, "color(display-p3-linear 1 1 1)", &c));
    EMB_CHECK(c.space == EMBI_CS_DISPLAY_P3_LINEAR);
    EMB_REQUIRE(embi_color_to_srgb8(&c, px));
    EMB_CHECK(near8(px, 255, 255, 255));
    EMB_REQUIRE(parse_str(&ar, "color(display-p3-linear 0 0 0)", &c));
    EMB_REQUIRE(embi_color_to_srgb8(&c, px));
    EMB_CHECK(near8(px, 0, 0, 0));
    /* linear 0.5 is brighter than half the display-p3 encoded value 0.5 (the transfer curve is absent) */
    EMB_REQUIRE(parse_str(&ar, "color(display-p3-linear 0.5 0.5 0.5)", &c));
    EMB_REQUIRE(embi_color_to_srgb8(&c, px));
    EMB_CHECK(near8(px, 188, 188, 188));
    embi_sb sb = {0};
    embi_color_serialize(&sb, &c);
    EMB_CHECK(sb.len > 0 && strstr(sb.data, "display-p3-linear") != nullptr);
    embi_sb_free(&sb);
    embi_arena_free(&ar);
}

static bool deferred8(const char *text, const char *current, uint8_t out[4])
{
    embi_arena ar = {0};
    embi_arena_init(&ar, 2048);
    embi_color c, cur, r;
    bool ok = parse_str(&ar, text, &c) && parse_str(&ar, current, &cur);
    ok = ok && c.space == EMBI_CS_DEFERRED && embi_color_resolve_deferred(&c, &cur, &r) &&
         embi_color_to_srgb8(&r, out);
    embi_arena_free(&ar);
    return ok;
}

EMB_TEST(deferred_relative_color_resolves_against_current_color)
{
    uint8_t px[4];
    EMB_REQUIRE(deferred8("rgb(from currentcolor r g b)", "rgb(10 20 30)", px));
    EMB_CHECK(near8(px, 10, 20, 30));
    EMB_REQUIRE(deferred8("rgb(from currentcolor 255 g b)", "rgb(10 20 30)", px));
    EMB_CHECK(near8(px, 255, 20, 30));
    EMB_REQUIRE(deferred8("rgb(from currentcolor r g b / 0.5)", "rgb(10 20 30)", px));
    EMB_CHECK(near8(px, 10, 20, 30));
    EMB_CHECK(abs(px[3] - 128) <= 1);
}

EMB_TEST(deferred_color_mix_resolves_against_current_color)
{
    uint8_t px[4];
    EMB_REQUIRE(deferred8("color-mix(in srgb, currentcolor 50%, rgb(0 0 255))", "rgb(255 0 0)", px));
    EMB_CHECK(near8(px, 128, 0, 128));
    /* the same text with another current color gives another result */
    EMB_REQUIRE(deferred8("color-mix(in srgb, currentcolor 50%, rgb(0 0 255))", "rgb(0 255 0)", px));
    EMB_CHECK(near8(px, 0, 128, 128));
}

EMB_TEST_MAIN()
