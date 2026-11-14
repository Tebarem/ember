#include "emb_test.h"

#include <math.h>
#include <string.h>

#include "canvas/canvas.h"
#include "image/png.h"

#define CV_PI 3.14159265358979323846

/* The canvas without a GPU: paths, hit testing, the state machine and its serializations, the PNG encoder. Nothing here
 * draws; tests/unit/canvas/test_draw.c checks pixels. */

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static embi_canvas *make(uint32_t w, uint32_t h)
{
    embi_canvas_env env;
    memset(&env, 0, sizeof env);
    return embi_canvas_create(&env, nullptr, w, h, true);
}

/* ------------------------------------------------------------------ paths */

EMB_TEST(path_rect_hit_test)
{
    size_t live = embi_mem_live();
    embi_cpath *p = embi_cpath_new();
    embi_cpath_rect(p, nullptr, 10, 10, 20, 20);
    EMB_CHECK(embi_cpath_contains(p, 15, 15, false));
    EMB_CHECK(!embi_cpath_contains(p, 5, 5, false));
    EMB_CHECK(!embi_cpath_contains(p, 35, 15, false));
    double b[4];
    EMB_CHECK(embi_cpath_bounds(p, b));
    EMB_CHECK(b[0] == 10 && b[1] == 10 && b[2] == 30 && b[3] == 30);
    embi_cpath_release(p);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(path_even_odd_makes_a_hole)
{
    embi_cpath *p = embi_cpath_new();
    embi_cpath_rect(p, nullptr, 0, 0, 40, 40);
    embi_cpath_rect(p, nullptr, 10, 10, 20, 20);
    EMB_CHECK(embi_cpath_contains(p, 20, 20, false)); /* both wind the same way: nonzero fills the middle */
    EMB_CHECK(!embi_cpath_contains(p, 20, 20, true));
    EMB_CHECK(embi_cpath_contains(p, 5, 5, true));
    embi_cpath_release(p);
}

EMB_TEST(path_arc_circle)
{
    embi_cpath *p = embi_cpath_new();
    EMB_CHECK_EQ_INT(embi_cpath_arc(p, nullptr, 50, 50, 10, 0, 2 * CV_PI, false), EMBI_CV_OK);
    EMB_CHECK(embi_cpath_contains(p, 50, 50, false));
    EMB_CHECK(embi_cpath_contains(p, 57, 50, false));
    EMB_CHECK(!embi_cpath_contains(p, 62, 50, false));
    EMB_CHECK(!embi_cpath_contains(p, 58, 58, false)); /* the corner of the bounding box */
    double b[4];
    EMB_CHECK(embi_cpath_bounds(p, b));
    EMB_CHECK(fabs(b[0] - 40) < 0.1 && fabs(b[2] - 60) < 0.1);
    embi_cpath_release(p);
}

EMB_TEST(path_arc_negative_radius_is_an_index_size_error)
{
    embi_cpath *p = embi_cpath_new();
    EMB_CHECK_EQ_INT(embi_cpath_arc(p, nullptr, 0, 0, -1, 0, 1, false), EMBI_CV_INDEX_SIZE);
    EMB_CHECK_EQ_INT(embi_cpath_ellipse(p, nullptr, 0, 0, 1, -1, 0, 0, 1, false), EMBI_CV_INDEX_SIZE);
    EMB_CHECK(embi_cpath_empty(p));
    embi_cpath_release(p);
}

EMB_TEST(path_arc_direction_and_sweep)
{
    /* A quarter turn clockwise from 0 to pi/2 passes (cx, cy + r); counterclockwise it takes the long way. */
    embi_cpath *a = embi_cpath_new();
    embi_cpath_move_to(a, nullptr, 50, 50);
    (void)embi_cpath_arc(a, nullptr, 50, 50, 20, 0, CV_PI / 2, false);
    embi_cpath_close(a);
    EMB_CHECK(embi_cpath_contains(a, 60, 60, false));
    EMB_CHECK(!embi_cpath_contains(a, 40, 40, false));
    embi_cpath *b = embi_cpath_new();
    embi_cpath_move_to(b, nullptr, 50, 50);
    (void)embi_cpath_arc(b, nullptr, 50, 50, 20, 0, CV_PI / 2, true);
    embi_cpath_close(b);
    EMB_CHECK(!embi_cpath_contains(b, 60, 60, false));
    EMB_CHECK(embi_cpath_contains(b, 40, 40, false));
    embi_cpath_release(a);
    embi_cpath_release(b);
}

EMB_TEST(path_svg_data)
{
    embi_cpath *p = embi_cpath_new();
    embi_cpath_parse_svg(p, S("M10 10 L 30 10 L 30 30 Z"));
    EMB_CHECK(embi_cpath_contains(p, 25, 15, false));
    EMB_CHECK(!embi_cpath_contains(p, 15, 25, false));
    embi_cpath_release(p);
    /* Shorthands, relative commands and arcs. */
    p = embi_cpath_new();
    embi_cpath_parse_svg(p, S("m0 0 h20 v20 h-20 z"));
    EMB_CHECK(embi_cpath_contains(p, 10, 10, false));
    embi_cpath_release(p);
    p = embi_cpath_new();
    embi_cpath_parse_svg(p, S("M10 50 A 20 20 0 0 1 50 50 Z"));
    double b[4];
    EMB_CHECK(embi_cpath_bounds(p, b));
    EMB_CHECK(b[1] <= 31 && b[3] >= 49); /* a half disc */
    embi_cpath_release(p);
}

EMB_TEST(path_svg_error_keeps_what_was_read)
{
    embi_cpath *p = embi_cpath_new();
    embi_cpath_parse_svg(p, S("M0 0 L 10 0 L 10 10 L oops"));
    double b[4];
    EMB_CHECK(embi_cpath_bounds(p, b));
    EMB_CHECK(b[2] == 10 && b[3] == 10);
    embi_cpath_release(p);
}

EMB_TEST(path_round_rect_cuts_the_corners)
{
    embi_cpath *p = embi_cpath_new();
    const double r[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    embi_cpath_round_rect(p, nullptr, 0, 0, 20, 20, r);
    EMB_CHECK(embi_cpath_contains(p, 10, 10, false));
    EMB_CHECK(!embi_cpath_contains(p, 0.5, 0.5, false));
    EMB_CHECK(embi_cpath_contains(p, 5, 0.5, false));
    embi_cpath_release(p);
}

EMB_TEST(path_add_applies_the_matrix)
{
    embi_cpath *a = embi_cpath_new();
    embi_cpath_rect(a, nullptr, 0, 0, 10, 10);
    embi_cpath *b = embi_cpath_new();
    const double m[6] = {2, 0, 0, 2, 100, 0};
    embi_cpath_add(b, a, m);
    EMB_CHECK(embi_cpath_contains(b, 110, 10, false));
    EMB_CHECK(!embi_cpath_contains(b, 5, 5, false));
    embi_cpath_release(a);
    embi_cpath_release(b);
}

EMB_TEST(path_non_finite_arguments_change_nothing)
{
    embi_cpath *p = embi_cpath_new();
    embi_cpath_move_to(p, nullptr, NAN, 0);
    embi_cpath_line_to(p, nullptr, 0, INFINITY);
    embi_cpath_rect(p, nullptr, 0, 0, NAN, 1);
    EMB_CHECK(embi_cpath_empty(p));
    embi_cpath_release(p);
}

/* ------------------------------------------------------------------ state */

EMB_TEST(state_defaults)
{
    size_t live = embi_mem_live();
    embi_canvas *cv = make(100, 50);
    EMB_CHECK_EQ_INT(embi_canvas_width(cv), 100);
    EMB_CHECK_EQ_INT(embi_canvas_height(cv), 50);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 1.0);
    EMB_CHECK_EQ_STR(embi_canvas_get_composite(cv), "source-over");
    EMB_CHECK(embi_canvas_get_line_width(cv) == 1.0);
    EMB_CHECK_EQ_STR(embi_canvas_get_line_cap(cv), "butt");
    EMB_CHECK_EQ_STR(embi_canvas_get_line_join(cv), "miter");
    EMB_CHECK(embi_canvas_get_miter_limit(cv) == 10.0);
    EMB_CHECK(embi_canvas_get_smoothing(cv));
    EMB_CHECK_EQ_STR(embi_canvas_get_quality(cv), "low");
    embi_sb sb = {0};
    embi_cgrad *g = nullptr;
    embi_cpattern *pt = nullptr;
    EMB_CHECK_EQ_INT(embi_canvas_get_style(cv, 0, &sb, &g, &pt), 0);
    EMB_CHECK_EQ_INT(sb.len, 7);
    EMB_CHECK(memcmp(sb.data, "#000000", 7) == 0);
    embi_sb_free(&sb);
    embi_canvas_get_text_attr(cv, 0, &sb);
    EMB_CHECK(sb.len == 15 && memcmp(sb.data, "10px sans-serif", 15) == 0);
    embi_sb_free(&sb);
    embi_canvas_destroy(cv);
    EMB_CHECK(embi_mem_live() == live);
}

static void expect_style(embi_canvas *cv, const char *set, const char *want)
{
    embi_canvas_set_color(cv, 0, S(set));
    embi_sb sb = {0};
    embi_cgrad *g = nullptr;
    embi_cpattern *pt = nullptr;
    (void)embi_canvas_get_style(cv, 0, &sb, &g, &pt);
    bool ok = sb.len == strlen(want) && memcmp(sb.data, want, sb.len) == 0;
    if (!ok) {
        SDL_Log("fillStyle %s -> %.*s (want %s)", set, (int)sb.len, sb.data, want);
    }
    EMB_CHECK(ok);
    embi_sb_free(&sb);
}

EMB_TEST(state_color_serialization)
{
    embi_canvas *cv = make(10, 10);
    expect_style(cv, "red", "#ff0000");
    expect_style(cv, "#0f0", "#00ff00");
    expect_style(cv, "rgb(1, 2, 3)", "#010203");
    expect_style(cv, "rgba(255, 0, 0, 0.5)", "rgba(255, 0, 0, 0.5)");
    expect_style(cv, "hsl(120, 100%, 50%)", "#00ff00");
    expect_style(cv, "transparent", "rgba(0, 0, 0, 0)");
    /* An invalid color leaves the value as it was. */
    expect_style(cv, "not a color", "rgba(0, 0, 0, 0)");
    expect_style(cv, "#abc", "#aabbcc");
    embi_canvas_destroy(cv);
}

EMB_TEST(state_save_restore)
{
    size_t live = embi_mem_live();
    embi_canvas *cv = make(10, 10);
    embi_canvas_set_alpha(cv, 0.5);
    embi_canvas_set_line_width(cv, 7);
    embi_canvas_translate(cv, 3, 4);
    embi_canvas_save(cv);
    embi_canvas_set_alpha(cv, 0.25);
    embi_canvas_set_line_width(cv, 2);
    embi_canvas_scale(cv, 2, 2);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 0.25);
    embi_canvas_restore(cv);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 0.5);
    EMB_CHECK(embi_canvas_get_line_width(cv) == 7);
    double m[6];
    embi_canvas_get_transform(cv, m);
    EMB_CHECK(m[0] == 1 && m[3] == 1 && m[4] == 3 && m[5] == 4);
    embi_canvas_restore(cv); /* an empty stack does nothing */
    EMB_CHECK(embi_canvas_get_alpha(cv) == 0.5);
    embi_canvas_destroy(cv);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(state_transform_order)
{
    embi_canvas *cv = make(10, 10);
    embi_canvas_translate(cv, 10, 0);
    embi_canvas_scale(cv, 2, 3);
    double m[6];
    embi_canvas_get_transform(cv, m);
    /* translate then scale: x' = 2x + 10, y' = 3y */
    EMB_CHECK(m[0] == 2 && m[3] == 3 && m[4] == 10 && m[5] == 0);
    embi_canvas_rotate(cv, CV_PI / 2);
    embi_canvas_get_transform(cv, m);
    EMB_CHECK(fabs(m[0]) < 1e-12 && fabs(m[1] - 3) < 1e-12 && fabs(m[2] + 2) < 1e-12 && fabs(m[3]) < 1e-12);
    /* A non-finite argument changes nothing. */
    double before[6];
    embi_canvas_get_transform(cv, before);
    embi_canvas_translate(cv, NAN, 1);
    embi_canvas_get_transform(cv, m);
    EMB_CHECK(memcmp(m, before, sizeof m) == 0);
    embi_canvas_destroy(cv);
}

EMB_TEST(state_composite_operation_names)
{
    static const char *const names[] = {"source-over", "source-in", "source-out", "source-atop", "destination-over",
                                        "destination-in", "destination-out", "destination-atop", "lighter", "copy",
                                        "xor", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
                                        "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
                                        "saturation", "color", "luminosity"};
    embi_canvas *cv = make(4, 4);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        embi_canvas_set_composite(cv, S(names[i]));
        EMB_CHECK_EQ_STR(embi_canvas_get_composite(cv), names[i]);
    }
    embi_canvas_set_composite(cv, S("hue"));
    embi_canvas_set_composite(cv, S("bogus"));
    EMB_CHECK_EQ_STR(embi_canvas_get_composite(cv), "hue");
    embi_canvas_destroy(cv);
}

EMB_TEST(state_line_dash_repeats_an_odd_list)
{
    embi_canvas *cv = make(4, 4);
    const double v[3] = {5, 10, 15};
    EMB_CHECK(embi_canvas_set_line_dash(cv, v, 3));
    const double *got = nullptr;
    uint32_t n = embi_canvas_get_line_dash(cv, &got);
    EMB_CHECK_EQ_INT(n, 6);
    EMB_CHECK(got[0] == 5 && got[1] == 10 && got[2] == 15 && got[3] == 5 && got[4] == 10 && got[5] == 15);
    const double bad[2] = {1, -1};
    EMB_CHECK(!embi_canvas_set_line_dash(cv, bad, 2));
    EMB_CHECK_EQ_INT(embi_canvas_get_line_dash(cv, &got), 6);
    embi_canvas_destroy(cv);
}

EMB_TEST(state_line_style_validation)
{
    embi_canvas *cv = make(4, 4);
    embi_canvas_set_line_width(cv, 0);
    embi_canvas_set_line_width(cv, -3);
    embi_canvas_set_line_width(cv, NAN);
    EMB_CHECK(embi_canvas_get_line_width(cv) == 1.0);
    embi_canvas_set_line_cap(cv, S("round"));
    embi_canvas_set_line_cap(cv, S("nonsense"));
    EMB_CHECK_EQ_STR(embi_canvas_get_line_cap(cv), "round");
    embi_canvas_set_line_join(cv, S("bevel"));
    EMB_CHECK_EQ_STR(embi_canvas_get_line_join(cv), "bevel");
    embi_canvas_set_miter_limit(cv, 0);
    EMB_CHECK(embi_canvas_get_miter_limit(cv) == 10.0);
    embi_canvas_set_alpha(cv, 2.0);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 1.0);
    embi_canvas_set_alpha(cv, -1.0);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 1.0);
    embi_canvas_destroy(cv);
}

EMB_TEST(state_font_shorthand)
{
    embi_canvas *cv = make(4, 4);
    embi_sb sb = {0};
    embi_canvas_set_text_attr(cv, 0, S("bold 20px Arial"));
    embi_canvas_get_text_attr(cv, 0, &sb);
    EMB_CHECK(sb.len == 15 && memcmp(sb.data, "bold 20px Arial", 15) == 0);
    embi_sb_free(&sb);
    embi_canvas_set_text_attr(cv, 0, S("italic 12px serif"));
    embi_canvas_set_text_attr(cv, 0, S("garbage"));
    embi_canvas_get_text_attr(cv, 0, &sb);
    EMB_CHECK(sb.len == 17 && memcmp(sb.data, "italic 12px serif", 17) == 0);
    embi_sb_free(&sb);
    embi_canvas_destroy(cv);
}

EMB_TEST(state_text_attributes)
{
    embi_canvas *cv = make(4, 4);
    embi_sb sb = {0};
    embi_canvas_get_text_attr(cv, 1, &sb);
    EMB_CHECK(sb.len == 5 && memcmp(sb.data, "start", 5) == 0);
    embi_sb_free(&sb);
    embi_canvas_set_text_attr(cv, 1, S("center"));
    embi_canvas_set_text_attr(cv, 2, S("middle"));
    embi_canvas_set_text_attr(cv, 3, S("rtl"));
    embi_canvas_get_text_attr(cv, 1, &sb);
    EMB_CHECK(sb.len == 6 && memcmp(sb.data, "center", 6) == 0);
    embi_sb_free(&sb);
    embi_canvas_get_text_attr(cv, 2, &sb);
    EMB_CHECK(sb.len == 6 && memcmp(sb.data, "middle", 6) == 0);
    embi_sb_free(&sb);
    embi_canvas_get_text_attr(cv, 3, &sb);
    EMB_CHECK(sb.len == 3 && memcmp(sb.data, "rtl", 3) == 0);
    embi_sb_free(&sb);
    embi_canvas_set_text_attr(cv, 4, S("2px"));
    embi_canvas_get_text_attr(cv, 4, &sb);
    EMB_CHECK(sb.len == 3 && memcmp(sb.data, "2px", 3) == 0);
    embi_sb_free(&sb);
    embi_canvas_destroy(cv);
}

EMB_TEST(state_resize_resets_everything)
{
    size_t live = embi_mem_live();
    embi_canvas *cv = make(10, 10);
    embi_canvas_set_alpha(cv, 0.5);
    embi_canvas_save(cv);
    embi_canvas_set_line_width(cv, 9);
    embi_canvas_resize(cv, 20, 30);
    EMB_CHECK_EQ_INT(embi_canvas_width(cv), 20);
    EMB_CHECK_EQ_INT(embi_canvas_height(cv), 30);
    EMB_CHECK(embi_canvas_get_alpha(cv) == 1.0);
    EMB_CHECK(embi_canvas_get_line_width(cv) == 1.0);
    embi_canvas_restore(cv); /* the stack was cleared too */
    EMB_CHECK(embi_canvas_get_line_width(cv) == 1.0);
    embi_canvas_destroy(cv);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(state_gradient_and_pattern_styles)
{
    size_t live = embi_mem_live();
    embi_canvas *cv = make(10, 10);
    const double g4[6] = {0, 0, 10, 0, 0, 0};
    embi_cgrad *g = embi_cgrad_new(EMBI_CG_LINEAR, g4);
    EMB_CHECK_EQ_INT(embi_cgrad_add_stop(g, cv, 0.0, S("red")), EMBI_CV_OK);
    EMB_CHECK_EQ_INT(embi_cgrad_add_stop(g, cv, 1.0, S("blue")), EMBI_CV_OK);
    EMB_CHECK_EQ_INT(embi_cgrad_add_stop(g, cv, 1.5, S("blue")), EMBI_CV_INDEX_SIZE);
    EMB_CHECK_EQ_INT(embi_cgrad_add_stop(g, cv, 0.5, S("not a color")), EMBI_CV_SYNTAX);
    embi_canvas_set_gradient(cv, 1, g);
    embi_sb sb = {0};
    embi_cgrad *got = nullptr;
    embi_cpattern *pt = nullptr;
    EMB_CHECK_EQ_INT(embi_canvas_get_style(cv, 1, &sb, &got, &pt), 1);
    EMB_CHECK(got == g);
    embi_sb_free(&sb);
    embi_cgrad_release(g); /* the state keeps its own reference */
    EMB_CHECK_EQ_INT(embi_canvas_get_style(cv, 1, &sb, &got, &pt), 1);
    embi_canvas_set_color(cv, 1, S("lime"));
    EMB_CHECK_EQ_INT(embi_canvas_get_style(cv, 1, &sb, &got, &pt), 0);
    embi_sb_free(&sb);
    embi_canvas_destroy(cv);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(state_measure_text_without_a_text_system_is_empty)
{
    embi_canvas *cv = make(10, 10);
    embi_text_metrics m;
    EMB_CHECK(!embi_canvas_measure_text(cv, S("hello"), &m));
    embi_canvas_destroy(cv);
}

/* ------------------------------------------------------------------ png */

EMB_TEST(png_round_trip)
{
    size_t live = embi_mem_live();
    enum { W = 7, H = 5 };
    uint8_t px[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        px[4 * i + 0] = (uint8_t)(i * 7);
        px[4 * i + 1] = (uint8_t)(255 - i * 3);
        px[4 * i + 2] = (uint8_t)(i * 31);
        px[4 * i + 3] = (uint8_t)(i % 3 == 0 ? 255 : i * 5);
    }
    embi_sb png = {0};
    embi_png_encode_rgba(px, W, H, &png);
    EMB_CHECK(png.len > 8);
    EMB_CHECK(memcmp(png.data, "\x89PNG\r\n\x1a\n", 8) == 0);
    embi_pixels out;
    EMB_REQUIRE(embi_png_decode((const uint8_t *)png.data, png.len, 1u << 20, false, &out));
    EMB_CHECK_EQ_INT(out.w, W);
    EMB_CHECK_EQ_INT(out.h, H);
    EMB_CHECK(memcmp(out.px, px, sizeof px) == 0);
    embi_pixels_free(&out);
    embi_sb_free(&png);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(png_large_image_has_valid_checksums)
{
    enum { W = 300, H = 200 };
    uint8_t *px = embi_calloc(W * H, 4);
    for (int i = 0; i < W * H; i++) {
        px[4 * i + 0] = (uint8_t)(i & 255);
        px[4 * i + 1] = (uint8_t)((i >> 8) & 255);
        px[4 * i + 3] = 255;
    }
    embi_sb png = {0};
    embi_png_encode_rgba(px, W, H, &png);
    embi_pixels out;
    EMB_REQUIRE(embi_png_decode((const uint8_t *)png.data, png.len, 1u << 20, false, &out));
    EMB_CHECK(memcmp(out.px, px, (size_t)W * H * 4) == 0);
    embi_pixels_free(&out);
    embi_sb_free(&png);
    embi_free(px, (size_t)W * H * 4);
}

EMB_TEST(nogpu_canvas_keeps_no_commands)
{
    /* A canvas with no GPU has nothing to draw into: draw calls must not pile up commands (memory grows without bound). */
    embi_canvas *cv = make(200, 100);
    size_t live = embi_mem_live();
    embi_canvas_set_color(cv, 0, S("#f00"));
    for (int i = 0; i < 20000; i++) {
        embi_canvas_fill_rect(cv, i % 100, 5, 10, 10);
        embi_canvas_begin_path(cv);
        embi_cpath_rect(embi_canvas_path(cv), nullptr, 1, 2, 30, 40);
        embi_canvas_fill(cv, nullptr, false);
    }
    EMB_CHECK(embi_mem_live() < live + 4096);
    embi_canvas_destroy(cv);
}

EMB_TEST(size_limit_is_enforced_everywhere)
{
    size_t live = embi_mem_live();
    EMB_CHECK(embi_canvas_size_ok(16384, 16384));
    EMB_CHECK(!embi_canvas_size_ok(16385, 1));
    EMB_CHECK(!embi_canvas_size_ok(100000, 100000));
    EMB_CHECK(!embi_canvas_size_ok(0x80000000u, 0x80000000u));
    /* An image beyond the area limit is empty, not a wrapped allocation. */
    embi_cimage *im = embi_cimage_new(0x80000000u, 0x80000000u);
    EMB_CHECK(im->w == 0 && im->h == 0);
    embi_cimage_release(im);
    im = embi_cimage_new(100000, 100000);
    EMB_CHECK(im->w == 0 && im->h == 0);
    embi_cimage_release(im);
    im = embi_cimage_new(8, 4);
    EMB_CHECK(im->w == 8 && im->h == 4);
    embi_cimage_release(im);
    /* A canvas past the limit has no bitmap: no PNG, an empty snapshot, no pattern. */
    embi_canvas *cv = make(100000, 100000);
    embi_sb png = {0};
    EMB_CHECK(!embi_canvas_to_png(cv, &png));
    embi_sb_free(&png);
    embi_cimage *sn = embi_canvas_snapshot(cv, 0, 0, 100000, 100000);
    EMB_CHECK(sn->w == 0);
    embi_cimage_release(sn);
    EMB_CHECK(embi_canvas_pattern_from(cv, nullptr, cv, EMBI_CP_REPEAT) == nullptr);
    uint8_t px[16];
    EMB_CHECK(!embi_canvas_read(cv, 0, 0, 100000, 100000, px));
    embi_canvas_destroy(cv);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST_MAIN()
