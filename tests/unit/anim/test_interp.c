/*
 * Interpolation tables (W6a): numbers and integers, lengths and calc() mixes, colors in
 * premultiplied Oklab, visibility, transform lists (pairwise and 2D decomposition), shadow and
 * filter lists with padding, positions, basic shapes, lists of lengths, discrete values, and the
 * additive composition. The expected values follow the specs' rules, not the implementation.
 */
#include "emb_test.h"

#include "anim_fixture.h"

EMB_TEST(numbers_integers_and_font_weight)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "opacity", "0", "1", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_NUMBER && anim_near(v.num, 0.25, 1e-9));

    v = anim_mix(&a, "font-weight", "400", "700", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_NUMBER && anim_near(v.num, 550.0, 1e-9));

    /* z-index is an integer: 2.5 rounds to the nearest integer, half up. */
    v = anim_mix(&a, "z-index", "1", "4", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_INTEGER && v.num == 3.0);
    v = anim_mix(&a, "z-index", "0", "-3", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_INTEGER && v.num == -1.0);

    /* auto is not a number: discrete. */
    v = anim_mix(&a, "z-index", "auto", "3", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_KEYWORD && v.kw == EMBI_KW_auto);
    v = anim_mix(&a, "z-index", "auto", "3", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_INTEGER && v.num == 3.0);
    embi_arena_free(&a);
}

EMB_TEST(lengths_percentages_and_calc)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* Same unit: a plain length. */
    embi_value v = anim_mix(&a, "width", "10px", "30px", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_LENGTH && v.unit == EMBI_U_PX && anim_near(v.num, 20.0, 1e-9));

    /* Different units: a calc(): 10px at 0.5 and 50% of 200px at 0.5 is 55px. */
    v = anim_mix(&a, "left", "10px", "50%", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_CALC && anim_near(anim_px(&v), 55.0, 1e-9));

    /* em and px: 1em is 16px here, mixed with 10px at 0.5 gives 13px. */
    v = anim_mix(&a, "margin-left", "1em", "10px", 0.5, &ok);
    EMB_CHECK(ok && anim_near(anim_px(&v), 13.0, 1e-9));

    /* Percentages on their own stay percentages. */
    v = anim_mix(&a, "left", "0%", "100%", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_PERCENT && anim_near(v.num, 50.0, 1e-9));

    /* line-height mixes numbers with numbers; numbers and lengths are discrete. */
    v = anim_mix(&a, "line-height", "1", "2", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_NUMBER && anim_near(v.num, 1.5, 1e-9));
    v = anim_mix(&a, "line-height", "1", "10px", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_NUMBER && v.num == 1.0);
    v = anim_mix(&a, "line-height", "1", "10px", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_LENGTH && v.num == 10.0);

    /* Extrapolation outside 0..1 keeps the linear law. */
    v = anim_mix(&a, "width", "10px", "30px", 1.5, &ok);
    EMB_CHECK(ok && anim_near(v.num, 40.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(colors_mix_in_premultiplied_oklab)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* A transparent start contributes no color: the result is red at half alpha. */
    embi_value v = anim_mix(&a, "color", "transparent", "red", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_COLOR);
    EMB_CHECK(anim_near(v.color.alpha, 0.5, 1e-6));
    EMB_CHECK(anim_near(v.color.c[0], 1.0, 1e-4) && anim_near(v.color.c[1], 0.0, 1e-4) &&
              anim_near(v.color.c[2], 0.0, 1e-4));

    /* The endpoints are exact. */
    v = anim_mix(&a, "color", "red", "blue", 0.0, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 1.0, 1e-9) && anim_near(v.color.c[2], 0.0, 1e-9));
    v = anim_mix(&a, "color", "red", "blue", 1.0, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 0.0, 1e-9) && anim_near(v.color.c[2], 1.0, 1e-9));

    /* A midpoint between two opaque colors is opaque and between the two in red and blue. */
    v = anim_mix(&a, "color", "red", "blue", 0.5, &ok);
    EMB_CHECK(ok && anim_near(v.color.alpha, 1.0, 1e-9));
    EMB_CHECK(v.color.c[0] > 0.0 && v.color.c[0] < 1.0 && v.color.c[2] > 0.0 && v.color.c[2] < 1.0);
    embi_arena_free(&a);
}

EMB_TEST(legacy_colors_mix_premultiplied_in_srgb_and_quantize)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* Two legacy-syntax colors mix premultiplied in sRGB: red at alpha 0.5 and opaque blue, half way. The premultiplied
     * channels are (0.25, 0, 0) and (0, 0, 0.5) at alpha 0.75, so the un-premultiplied result is (1/3, 0, 2/3). It is
     * quantized to 8 bits: 85 and 170. */
    embi_value v = anim_mix(&a, "color", "rgba(255, 0, 0, 0.5)", "blue", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_COLOR && v.color.space == EMBI_CS_SRGB);
    EMB_CHECK(anim_near(v.color.alpha, 0.75, 1e-9));
    EMB_CHECK(anim_near(v.color.c[0], 85.0 / 255.0, 1e-9));
    EMB_CHECK(anim_near(v.color.c[1], 0.0, 1e-9));
    EMB_CHECK(anim_near(v.color.c[2], 170.0 / 255.0, 1e-9));

    /* Black to white is a gray ramp in sRGB; 0.2 gives 51 and 0.5 rounds 127.5 up to 128. */
    v = anim_mix(&a, "color", "black", "white", 0.2, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 51.0 / 255.0, 1e-9) && anim_near(v.color.c[1], 51.0 / 255.0, 1e-9));
    v = anim_mix(&a, "color", "black", "white", 0.5, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 128.0 / 255.0, 1e-9) && anim_near(v.color.c[2], 128.0 / 255.0, 1e-9));

    /* Red to blue is the sRGB middle (128, 0, 128), not the Oklab one. */
    v = anim_mix(&a, "color", "red", "blue", 0.5, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 128.0 / 255.0, 1e-9) && anim_near(v.color.c[2], 128.0 / 255.0, 1e-9));

    /* The alpha keeps three digits. */
    v = anim_mix(&a, "color", "rgba(0, 0, 0, 1)", "rgba(0, 0, 0, 0)", 1.0 / 3.0, &ok);
    EMB_CHECK(ok && anim_near(v.color.alpha, 0.667, 1e-9));

    /* A fully transparent result has no color. */
    v = anim_mix(&a, "color", "transparent", "transparent", 0.5, &ok);
    EMB_CHECK(ok && v.color.alpha == 0.0 && v.color.c[0] == 0.0);

    /* The endpoints stay exact. */
    v = anim_mix(&a, "color", "rgb(10, 20, 30)", "rgb(200, 100, 50)", 0.0, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 10.0 / 255.0, 1e-9) && anim_near(v.color.c[2], 30.0 / 255.0, 1e-9));
    v = anim_mix(&a, "color", "rgb(10, 20, 30)", "rgb(200, 100, 50)", 1.0, &ok);
    EMB_CHECK(ok && anim_near(v.color.c[0], 200.0 / 255.0, 1e-9) && anim_near(v.color.c[1], 100.0 / 255.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(color_functions_still_mix_in_oklab)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* A color() function is not a legacy syntax: no 8-bit quantization of the channels. */
    embi_value v = anim_mix(&a, "color", "color(srgb 1 0 0)", "color(srgb 0 0 1)", 0.37, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_COLOR);
    double q = v.color.c[0] * 255.0;
    EMB_CHECK(fabs(q - round(q)) > 1e-6 || fabs(v.color.c[2] * 255.0 - round(v.color.c[2] * 255.0)) > 1e-6);
    embi_arena_free(&a);
}

EMB_TEST(visibility_is_discrete_with_visible_end)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "visibility", "visible", "hidden", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_KEYWORD && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "visibility", "visible", "hidden", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "visibility", "hidden", "visible", 0.75, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "visibility", "hidden", "visible", 1.0, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "visibility", "hidden", "collapse", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_hidden);
    v = anim_mix(&a, "visibility", "hidden", "collapse", 0.5, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_collapse);
    embi_arena_free(&a);
}

EMB_TEST(transform_pairwise_and_decomposition)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* Matching function lists mix pairwise. */
    embi_value v = anim_mix(&a, "transform", "translateX(0px)", "translateX(100px)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1);
    EMB_CHECK(v.xf.p[0].fn == EMBI_XF_TRANSLATEX && anim_near(anim_px(&v.xf.p[0].args[0]), 25.0, 1e-9));

    /* Different functions decompose to matrices: translate(10px) to rotate(90deg) at 0.5 is a
     * translation of 5px with a 45 degree rotation. */
    v = anim_mix(&a, "transform", "translate(10px, 0px)", "rotate(90deg)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_MATRIX);
    const embi_value *m = v.xf.p[0].args;
    double s = 0.70710678118;
    EMB_CHECK(anim_near(m[0].num, s, 1e-6) && anim_near(m[1].num, s, 1e-6));
    EMB_CHECK(anim_near(m[2].num, -s, 1e-6) && anim_near(m[3].num, s, 1e-6));
    EMB_CHECK(anim_near(m[4].num, 5.0, 1e-6) && anim_near(m[5].num, 0.0, 1e-6));

    /* none is the identity list: translate(10px) from none at 0.5 pairs with a zero translate, so
     * it is a translation of 5px, not a matrix. */
    v = anim_mix(&a, "transform", "none", "translate(10px, 0px)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.p[0].fn == EMBI_XF_TRANSLATE);
    EMB_CHECK(anim_near(anim_px(&v.xf.p[0].args[0]), 5.0, 1e-9));

    /* 3D functions that are not flat do not decompose: discrete. */
    v = anim_mix(&a, "transform", "translateZ(10px)", "rotate(90deg)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.p[0].fn == EMBI_XF_TRANSLATEZ);
    embi_arena_free(&a);
}

EMB_TEST(discrete_properties_flip_at_half_and_families_normalize)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "pointer-events", "auto", "none", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_auto);
    v = anim_mix(&a, "pointer-events", "auto", "none", 0.5, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_none);
    v = anim_mix(&a, "overflow-x", "visible", "hidden", 0.6, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_hidden);
    EMB_CHECK(embi_anim_animatable(anim_prop("overflow-x")));
    EMB_CHECK(!embi_anim_animatable(anim_prop("margin")));
    v = anim_mix(&a, "orphans", "2", "6", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_INTEGER && v.num == 4.0);
    v = anim_mix(&a, "widows", "1", "5", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_INTEGER && v.num == 2.0);

    v = anim_mix(&a, "transform", "translate(10px)", "translateX(30px)", 0.5, &ok);
    EMB_CHECK(ok && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_TRANSLATE &&
              anim_near(anim_px(&v.xf.p[0].args[0]), 20.0, 1e-9));
    v = anim_mix(&a, "transform", "scale(2)", "scaleY(4)", 0.5, &ok);
    EMB_CHECK(ok && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_SCALE &&
              anim_near(v.xf.p[0].args[0].num, 1.5, 1e-9) &&
              anim_near(v.xf.p[0].args[1].num, 3.0, 1e-9));
    v = anim_mix(&a, "transform", "rotate(0deg)", "rotateZ(90deg)", 0.5, &ok);
    EMB_CHECK(ok && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_ROTATE &&
              anim_near(v.xf.p[0].args[0].num, 45.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(shadows_pad_with_transparent_zero_shadows)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "box-shadow", "0px 0px 0px red", "10px 0px 0px red", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHADOWS && v.shadows.n == 1);
    EMB_CHECK(anim_near(anim_px(&v.shadows.p[0].x), 5.0, 1e-9));

    /* none is an empty list: the shadow pads with a transparent zero shadow. */
    v = anim_mix(&a, "box-shadow", "none", "2px 2px 0px blue", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHADOWS && v.shadows.n == 1);
    EMB_CHECK(anim_near(anim_px(&v.shadows.p[0].x), 1.0, 1e-9));
    EMB_CHECK(anim_near(v.shadows.p[0].color.color.alpha, 0.5, 1e-6));

    /* Lists of different lengths pad the shorter one at the end. */
    v = anim_mix(&a, "box-shadow", "1px 0px 0px red, 4px 0px 0px red", "3px 0px 0px red", 0.5, &ok);
    EMB_CHECK(ok && v.shadows.n == 2);
    EMB_CHECK(anim_near(anim_px(&v.shadows.p[0].x), 2.0, 1e-9));
    EMB_CHECK(anim_near(anim_px(&v.shadows.p[1].x), 2.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(filters_blur_and_numbers)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "filter", "blur(0px)", "blur(10px)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_FILTERS && v.filters.n == 1);
    EMB_CHECK(anim_near(anim_px(&v.filters.p[0].arg), 5.0, 1e-9));

    v = anim_mix(&a, "filter", "brightness(1)", "brightness(0.5)", 0.5, &ok);
    EMB_CHECK(ok && anim_near(v.filters.p[0].arg.num, 0.75, 1e-9));

    /* A URL filter never mixes: discrete. */
    v = anim_mix(&a, "filter", "url(#a)", "url(#b)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_FILTERS && v.filters.p[0].fn == EMBI_FILTER_URL);
    embi_arena_free(&a);
}

EMB_TEST(positions_shapes_and_lists)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* Percentages mix at the start edge. */
    embi_value v = anim_mix(&a, "transform-origin", "0% 0%", "100% 100%", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_POSITION);
    EMB_CHECK(v.pos->x.off.kind == EMBI_V_PERCENT && anim_near(v.pos->x.off.num, 50.0, 1e-9));
    EMB_CHECK(v.pos->y.off.kind == EMBI_V_PERCENT && anim_near(v.pos->y.off.num, 50.0, 1e-9));

    /* Basic shapes of one kind mix their arguments. */
    v = anim_mix(&a, "clip-path", "inset(0px)", "inset(10px)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHAPE && v.shape->kind == EMBI_SHAPE_INSET);
    EMB_CHECK(anim_near(anim_px(&v.shape->box[0]), 5.0, 1e-9));

    /* A shape and none are discrete. */
    v = anim_mix(&a, "clip-path", "none", "inset(10px)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_KEYWORD && v.kw == EMBI_KW_none);

    /* Radii are two-item lists, mixed item by item. */
    v = anim_mix(&a, "border-top-left-radius", "0px 0px", "10px 20px", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_LIST && v.list.n == 2);
    EMB_CHECK(anim_near(anim_px(&v.list.items[0]), 5.0, 1e-9) && anim_near(anim_px(&v.list.items[1]), 10.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(discrete_and_additive_composition)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* display is discrete: from below one half, to from one half. */
    embi_value v = anim_mix(&a, "display", "block", "inline", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_block);
    v = anim_mix(&a, "display", "block", "inline", 0.75, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_inline);

    /* p = 0 and p = 1 give the endpoints exactly, for every property. */
    v = anim_mix(&a, "width", "10px", "30px", 0.0, &ok);
    EMB_CHECK(ok && v.num == 10.0);
    v = anim_mix(&a, "width", "10px", "30px", 1.0, &ok);
    EMB_CHECK(ok && v.num == 30.0);

    /* add: base plus value, scaled. */
    bool okb = false;
    embi_value base = anim_parse(&a, "opacity", "0.5", &okb);
    embi_value val = anim_parse(&a, "opacity", "0.25", &okb);
    embi_value out;
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("opacity"), &base, &val, 1.0, &out));
    EMB_CHECK(anim_near(out.num, 0.75, 1e-9));

    base = anim_parse(&a, "width", "10px", &okb);
    val = anim_parse(&a, "width", "5px", &okb);
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("width"), &base, &val, 2.0, &out));
    EMB_CHECK(anim_near(anim_px(&out), 20.0, 1e-9));

    /* A length plus a percentage is a calc(). */
    base = anim_parse(&a, "left", "10px", &okb);
    val = anim_parse(&a, "left", "50%", &okb);
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("left"), &base, &val, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_CALC && anim_near(anim_px(&out), 110.0, 1e-9));

    /* Transform lists add by concatenation. */
    base = anim_parse(&a, "transform", "translateX(1px)", &okb);
    val = anim_parse(&a, "transform", "rotate(90deg)", &okb);
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("transform"), &base, &val, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_TRANSFORM && out.xf.n == 2);

    /* Non-animatable properties are reported as such. */
    EMB_CHECK(!embi_anim_animatable(anim_prop("will-change")));
    EMB_CHECK(embi_anim_animatable(anim_prop("opacity")));
    EMB_CHECK(embi_anim_interp_of(anim_prop("visibility")) == EMBI_IP_VIS);
    embi_arena_free(&a);
}

EMB_TEST(none_pads_transform_lists_with_identities)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* none -> rotate(360deg) at 0.25: rotate(0deg) to rotate(360deg) pairwise, so 90deg. */
    embi_value v = anim_mix(&a, "transform", "none", "rotate(360deg)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_ROTATE);
    EMB_CHECK(anim_near(v.xf.p[0].args[0].num, 90.0, 1e-9));

    /* The same goes the other way round: rotate(270deg) -> none at 0.5 is 135deg, not the long way. */
    v = anim_mix(&a, "transform", "rotate(270deg)", "none", 0.5, &ok);
    EMB_CHECK(ok && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_ROTATE);
    EMB_CHECK(anim_near(v.xf.p[0].args[0].num, 135.0, 1e-9));

    /* scale(0) -> none at 0.5 is scale(0.5): the singular matrix never decomposes. */
    v = anim_mix(&a, "transform", "scale(0)", "none", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_SCALE);
    EMB_CHECK(anim_near(v.xf.p[0].args[0].num, 0.5, 1e-9));

    /* The rotate property: none -> 360deg at 0.25 is a 90deg rotation about the z axis. */
    v = anim_mix(&a, "rotate", "none", "360deg", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_ROTATE3D);
    EMB_CHECK(anim_near(v.xf.p[0].args[3].num, 90.0, 1e-9));

    /* A list whose functions are a prefix of the other pads at its end: the padded rotate(0deg)
     * mixes with rotate(90deg) at 0.5 to 45deg, and the translate pairs as before. */
    v = anim_mix(&a, "transform", "translateX(10px)", "translateX(10px) rotate(90deg)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 2);
    EMB_CHECK(v.xf.p[0].fn == EMBI_XF_TRANSLATEX && anim_near(anim_px(&v.xf.p[0].args[0]), 10.0, 1e-9));
    EMB_CHECK(v.xf.p[1].fn == EMBI_XF_ROTATE && anim_near(v.xf.p[1].args[0].num, 45.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(shortest_path_rotation_when_decomposed)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* The functions differ, so the matrices decompose. The rotation from 170deg to -170deg takes
     * the 20deg way through 180deg, so the midpoint is a half turn, not the identity. */
    embi_value v = anim_mix(&a, "transform", "rotate(170deg)", "skewX(0deg) rotate(-170deg)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_MATRIX);
    const embi_value *m = v.xf.p[0].args;
    EMB_CHECK(anim_near(m[0].num, -1.0, 1e-6) && anim_near(m[3].num, -1.0, 1e-6));
    EMB_CHECK(anim_near(m[1].num, 0.0, 1e-6) && anim_near(m[2].num, 0.0, 1e-6));
    embi_arena_free(&a);
}

EMB_TEST(matrix_pairs_decompose)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* A quarter turn to the identity at 0.5 is a rotation of 45deg; componentwise it would be
     * matrix(0.5, 0.5, -0.5, 0.5) with a determinant of 0.5. */
    embi_value v = anim_mix(&a, "transform", "matrix(0, 1, -1, 0, 0, 0)", "matrix(1, 0, 0, 1, 0, 0)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_TRANSFORM && v.xf.n == 1 && v.xf.p[0].fn == EMBI_XF_MATRIX);
    const embi_value *m = v.xf.p[0].args;
    double s = 0.70710678118;
    EMB_CHECK(anim_near(m[0].num, s, 1e-6) && anim_near(m[3].num, s, 1e-6));
    EMB_CHECK(anim_near(m[1].num, s, 1e-6) && anim_near(m[2].num, -s, 1e-6));
    embi_arena_free(&a);
}

EMB_TEST(circle_position_mixes_with_and_without_an_explicit_at)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* The from side has no `at`: the center (50% 50%) moves to 0px 0px, so at 0.5 it is 25%, which
     * is 50px of the 200px reference box. The position is written, so it serializes. */
    embi_value v = anim_mix(&a, "clip-path", "circle(50%)", "circle(50% at 0px 0px)", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHAPE && v.shape->kind == EMBI_SHAPE_CIRCLE);
    EMB_CHECK(v.shape->has_at);
    EMB_CHECK(anim_near(anim_px(&v.shape->at.x.off), 50.0, 1e-6));

    /* The from side has the position and the to side does not: still written. */
    v = anim_mix(&a, "clip-path", "circle(50% at 0px 0px)", "circle(50%)", 0.5, &ok);
    EMB_CHECK(ok && v.shape->has_at && anim_near(anim_px(&v.shape->at.x.off), 50.0, 1e-6));

    /* Neither side has one: no position is written. */
    v = anim_mix(&a, "clip-path", "circle(10px)", "circle(30px)", 0.5, &ok);
    EMB_CHECK(ok && v.shape->kind == EMBI_SHAPE_CIRCLE && !v.shape->has_at);
    EMB_CHECK(anim_near(v.shape->r[0].num, 20.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(currentcolor_resolves_before_mixing)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value cur_v = anim_parse(&a, "color", "blue", &ok);
    EMB_CHECK(ok && cur_v.kind == EMBI_V_COLOR);
    embi_value from = anim_parse(&a, "border-top-color", "currentcolor", &ok);
    embi_value to = anim_parse(&a, "border-top-color", "red", &ok);
    EMB_CHECK(ok && from.kind == EMBI_V_COLOR && from.color.space == EMBI_CS_CURRENT);

    /* Unresolved, currentcolor cannot mix: the value flips at half way. */
    embi_value out;
    embi_anim_mix(&a, anim_prop("border-top-color"), &from, &to, 0.25, &out);
    EMB_CHECK(out.color.space == EMBI_CS_CURRENT);

    /* Resolved against blue, the mix is a real color between blue and red. */
    embi_value resolved;
    embi_anim_resolve_color(&a, &from, &cur_v.color, &resolved);
    EMB_CHECK(resolved.color.space != EMBI_CS_CURRENT);
    embi_anim_mix(&a, anim_prop("border-top-color"), &resolved, &to, 0.5, &out);
    EMB_CHECK(out.kind == EMBI_V_COLOR && out.color.c[2] > 0.0 && out.color.c[0] < 1.0);
    EMB_CHECK(anim_near(out.color.alpha, 1.0, 1e-9));

    /* box-shadow with an implicit currentcolor: the blur mixes (5px to 10px is 7.5px at half way),
     * which needs the shadow color resolved too. */
    embi_value sf = anim_parse(&a, "box-shadow", "0px 0px 5px", &ok);
    embi_value st = anim_parse(&a, "box-shadow", "0px 0px 10px red", &ok);
    embi_value sr;
    embi_anim_resolve_color(&a, &sf, &cur_v.color, &sr);
    embi_anim_mix(&a, anim_prop("box-shadow"), &sr, &st, 0.5, &out);
    EMB_CHECK(out.kind == EMBI_V_SHADOWS && anim_near(out.shadows.p[0].blur.num, 7.5, 1e-9));
    EMB_CHECK(out.shadows.p[0].color.color.space != EMBI_CS_CURRENT);

    /* No current color: resolve copies. */
    embi_anim_resolve_color(&a, &from, nullptr, &resolved);
    EMB_CHECK(resolved.color.space == EMBI_CS_CURRENT);
    embi_arena_free(&a);
}

EMB_TEST(colors_stay_in_gamut_when_extrapolated_through_transparent)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    for (int i = 1; i < 10; i++) {
        double p = 0.1 * i;
        embi_value v = anim_mix(&a, "color", "transparent", "red", p, &ok);
        EMB_CHECK(ok && v.color.c[0] >= 0.0 && v.color.c[0] <= 1.0);
        EMB_CHECK(v.color.c[1] >= 0.0 && v.color.c[2] >= 0.0);
        EMB_CHECK(v.color.alpha >= 0.0 && v.color.alpha <= 1.0);
    }
    embi_arena_free(&a);
}

EMB_TEST(add_composes_colors_shadows_shapes_and_lists)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok1, ok2;
    embi_value red = anim_parse(&a, "color", "red", &ok1);
    embi_value blue = anim_parse(&a, "color", "blue", &ok2);
    EMB_CHECK(ok1 && ok2);
    embi_value out;
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("color"), &red, &blue, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_COLOR && anim_near(out.color.c[0], 1.0, 1e-9) &&
              anim_near(out.color.c[2], 1.0, 1e-9));

    /* Shadows add their offsets pairwise. */
    embi_value s1 = anim_parse(&a, "box-shadow", "1px 0px 0px red", &ok1);
    embi_value s2 = anim_parse(&a, "box-shadow", "2px 0px 0px red", &ok2);
    EMB_CHECK(ok1 && ok2 && embi_anim_add_scaled(&a, anim_prop("box-shadow"), &s1, &s2, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_SHADOWS && anim_near(anim_px(&out.shadows.p[0].x), 3.0, 1e-9));

    /* Circles add their radii. */
    embi_value c1 = anim_parse(&a, "clip-path", "circle(10px)", &ok1);
    embi_value c2 = anim_parse(&a, "clip-path", "circle(5px)", &ok2);
    EMB_CHECK(ok1 && ok2 && embi_anim_add_scaled(&a, anim_prop("clip-path"), &c1, &c2, 2.0, &out));
    EMB_CHECK(out.kind == EMBI_V_SHAPE && anim_near(out.shape->r[0].num, 20.0, 1e-9));

    /* Filter lists concatenate. */
    embi_value f1 = anim_parse(&a, "filter", "blur(1px)", &ok1);
    embi_value f2 = anim_parse(&a, "filter", "brightness(2)", &ok2);
    EMB_CHECK(ok1 && ok2 && embi_anim_add_scaled(&a, anim_prop("filter"), &f1, &f2, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_FILTERS && out.filters.n == 2);
    embi_arena_free(&a);
}

EMB_TEST(easing_parses_linear_and_steps)
{
    embi_arena a;
    anim_arena_open(&a);
    /* linear() with a middle stop at 25% that reaches 1: 0 -> 1 over the first quarter. */
    const embi_easing *lin = anim_easing(&a, "linear(0, 1 25%, 1)");
    EMB_CHECK(lin != nullptr && lin->kind == EMBI_EASE_LINEAR_FN);
    EMB_CHECK(anim_near(embi_easing_eval(lin, 0.125), 0.5, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(lin, 0.6), 1.0, 1e-9));
    /* steps(4, jump-end) is parsed with all four steps and the jump term. */
    const embi_easing *st = anim_easing(&a, "steps(4, jump-end)");
    EMB_CHECK(st != nullptr && st->kind == EMBI_EASE_STEPS && st->steps == 4 && st->jump == EMBI_JUMP_END);
    EMB_CHECK(anim_near(embi_easing_eval(st, 0.3), 0.25, 1e-9));
    const embi_easing *sn = anim_easing(&a, "steps(4, jump-none)");
    EMB_CHECK(sn != nullptr && sn->jump == EMBI_JUMP_NONE && anim_near(embi_easing_eval(sn, 0.5), 2.0 / 3.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(shape_boxes_and_box_only_values)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* A geometry-box-only value has no shape: against a circle it is discrete, and does not crash. */
    embi_value v = anim_mix(&a, "clip-path", "border-box", "circle(30px)", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHAPE && v.shape == nullptr);
    v = anim_mix(&a, "clip-path", "border-box", "circle(30px)", 0.75, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHAPE && v.shape != nullptr);
    /* Two different boxes: discrete. */
    v = anim_mix(&a, "clip-path", "border-box", "padding-box", 0.25, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_SHAPE && v.shape == nullptr && v.kw == EMBI_KW_border_box);
    v = anim_mix(&a, "clip-path", "border-box", "padding-box", 0.75, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_padding_box);
    /* Equal box-only values stay put. */
    v = anim_mix(&a, "clip-path", "border-box", "border-box", 0.5, &ok);
    EMB_CHECK(ok && v.shape == nullptr && v.kw == EMBI_KW_border_box);
    /* The same reference box on both sides: shapes mix and the box is kept. */
    v = anim_mix(&a, "clip-path", "circle(10px) padding-box", "circle(30px) padding-box", 0.5, &ok);
    EMB_CHECK(ok && v.shape != nullptr && v.kw == EMBI_KW_padding_box);
    EMB_CHECK(anim_near(v.shape->r[0].num, 20.0, 1e-9));
    /* Different reference boxes: discrete. */
    v = anim_mix(&a, "clip-path", "circle(10px) padding-box", "circle(30px) border-box", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_padding_box && anim_near(v.shape->r[0].num, 10.0, 1e-9));
    /* Additive composition never dereferences a missing shape and respects the boxes. */
    bool o1, o2;
    embi_value b1 = anim_parse(&a, "clip-path", "border-box", &o1);
    embi_value c1 = anim_parse(&a, "clip-path", "circle(5px)", &o2);
    embi_value out;
    EMB_CHECK(o1 && o2 && !embi_anim_add_scaled(&a, anim_prop("clip-path"), &b1, &c1, 1.0, &out));
    embi_value p1 = anim_parse(&a, "clip-path", "circle(5px) padding-box", &o1);
    EMB_CHECK(o1 && !embi_anim_add_scaled(&a, anim_prop("clip-path"), &p1, &c1, 1.0, &out));
    EMB_CHECK(embi_anim_add_scaled(&a, anim_prop("clip-path"), &p1, &p1, 1.0, &out) &&
              out.kw == EMBI_KW_padding_box && anim_near(out.shape->r[0].num, 10.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(display_and_content_visibility_none_rules)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "display", "none", "block", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_block);
    v = anim_mix(&a, "display", "block", "none", 0.75, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_block);
    v = anim_mix(&a, "display", "block", "none", 1.0, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_none);
    v = anim_mix(&a, "display", "none", "block", 0.0, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_none);
    v = anim_mix(&a, "content-visibility", "hidden", "visible", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "content-visibility", "visible", "hidden", 0.75, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_visible);
    v = anim_mix(&a, "content-visibility", "visible", "hidden", 1.0, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_hidden);
    /* Neither end visible: the plain flip. */
    v = anim_mix(&a, "content-visibility", "hidden", "auto", 0.25, &ok);
    EMB_CHECK(ok && v.kw == EMBI_KW_hidden);
    embi_arena_free(&a);
}

EMB_TEST(ratio_mixes_and_border_image_lists)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "aspect-ratio", "1 / 1", "4 / 1", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_RATIO && anim_near(v.ratio.a / v.ratio.b, 2.0, 1e-9));
    v = anim_mix(&a, "aspect-ratio", "auto", "4 / 1", 0.25, &ok);
    EMB_CHECK(ok && v.kind != EMBI_V_RATIO);
    v = anim_mix(&a, "border-image-width", "1 2 3 4", "3 4 5 6", 0.5, &ok);
    EMB_CHECK(ok && v.kind == EMBI_V_LIST && v.list.n == 4 && anim_near(v.list.items[0].num, 2.0, 1e-9) &&
              anim_near(v.list.items[3].num, 5.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(add_composes_integers_positions_and_lists)
{
    embi_arena a;
    anim_arena_open(&a);
    bool o1, o2;
    embi_value out;
    embi_value z1 = anim_parse(&a, "z-index", "2", &o1);
    embi_value z2 = anim_parse(&a, "z-index", "3", &o2);
    EMB_CHECK(o1 && o2 && embi_anim_add_scaled(&a, anim_prop("z-index"), &z1, &z2, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_INTEGER && out.num == 5.0);
    embi_value p1 = anim_parse(&a, "object-position", "10px 20px", &o1);
    embi_value p2 = anim_parse(&a, "object-position", "5px 5px", &o2);
    EMB_CHECK(o1 && o2 && embi_anim_add_scaled(&a, anim_prop("object-position"), &p1, &p2, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_POSITION && anim_near(anim_px(&out.pos->x.off), 15.0, 1e-9) &&
              anim_near(anim_px(&out.pos->y.off), 25.0, 1e-9));
    embi_value l1 = anim_parse(&a, "border-image-width", "1 2 3 4", &o1);
    embi_value l2 = anim_parse(&a, "border-image-width", "1 1 1 1", &o2);
    EMB_CHECK(o1 && o2 && embi_anim_add_scaled(&a, anim_prop("border-image-width"), &l1, &l2, 1.0, &out));
    EMB_CHECK(out.kind == EMBI_V_LIST && out.list.n == 4 && anim_near(out.list.items[2].num, 4.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(mask_position_lists_mix_and_add)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    embi_value v = anim_mix(&a, "mask-position", "0% 0%", "100% 100%", 0.25, &ok);
    EMB_CHECK(ok && v.kind != EMBI_V_NONE);
    uint32_t n;
    const embi_value *it = embi_value_items(&v, &n);
    EMB_CHECK(n == 1 && it[0].kind == EMBI_V_POSITION);
    EMB_CHECK(anim_near(anim_px(&it[0].pos->x.off), 50.0, 1e-9) &&
              anim_near(anim_px(&it[0].pos->y.off), 50.0, 1e-9));
    v = anim_mix(&a, "mask-position", "0px 0px, 10px 20px", "100px 100px, 30px 40px", 0.5, &ok);
    it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 2 && it[0].kind == EMBI_V_POSITION);
    EMB_CHECK(anim_near(anim_px(&it[0].pos->x.off), 50.0, 1e-9) &&
              anim_near(anim_px(&it[1].pos->y.off), 30.0, 1e-9));
    /* 1 vs 2 items repeats to 2. */
    v = anim_mix(&a, "mask-position", "0px 0px", "10px 10px, 20px 20px", 0.5, &ok);
    it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 2 && anim_near(anim_px(&it[1].pos->x.off), 10.0, 1e-9));

    bool o1, o2;
    embi_value out;
    embi_value b = anim_parse(&a, "mask-position", "10px 20px, 1px 2px", &o1);
    embi_value d = anim_parse(&a, "mask-position", "5px 5px", &o2);
    EMB_CHECK(o1 && o2 && embi_anim_add_scaled(&a, anim_prop("mask-position"), &b, &d, 1.0, &out));
    it = embi_value_items(&out, &n);
    EMB_CHECK(n == 2 && anim_near(anim_px(&it[0].pos->x.off), 15.0, 1e-9) &&
              anim_near(anim_px(&it[1].pos->y.off), 7.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(repeatable_lists_repeat_to_the_least_common_multiple)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    uint32_t n;
    embi_value v = anim_mix(&a, "background-position-x", "10px, 5px", "30px", 0.5, &ok);
    const embi_value *it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 2 && it[0].kind == EMBI_V_POS_AXIS);
    EMB_CHECK(anim_near(anim_px(&it[0].axis->off), 20.0, 1e-9) &&
              anim_near(anim_px(&it[1].axis->off), 17.5, 1e-9));
    v = anim_mix(&a, "background-position-x", "10px, 20px", "0px, 10px, 20px", 0.5, &ok);
    it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 6 && anim_near(anim_px(&it[1].axis->off), 15.0, 1e-9) &&
              anim_near(anim_px(&it[5].axis->off), 20.0, 1e-9));
    v = anim_mix(&a, "background-size", "10px 10px, 20px 20px", "30px 30px", 0.5, &ok);
    it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 2 && it[0].kind == EMBI_V_LIST && anim_near(anim_px(&it[1].list.items[0]), 25.0, 1e-9));
    v = anim_mix(&a, "mask-size", "10px, 20px", "30px, 40px, 50px", 0.5, &ok);
    it = embi_value_items(&v, &n);
    EMB_CHECK(ok && n == 6);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
