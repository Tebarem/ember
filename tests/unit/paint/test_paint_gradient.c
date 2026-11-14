/*
 * Gradient stops in the display list (W4c section 3): color hints, hard stops, offsets that never decrease, the
 * premultiplied blend of colors with different alpha, the non-sRGB interpolation spaces (sampled into extra stops),
 * and the shared stop range of tiled gradients. Expected colors are computed from the CSS formulas (Images 4 and
 * Color 4), and the oklab midpoint of red and blue was computed independently: (140, 83, 162).
 */
#include "paint_harness.h"

#include <math.h>

/* The gradient item of the first gradient in the list, or nullptr. */
static const embi_ditem *first_gradient(const ph *h)
{
    int i = ph_find(&h->dl, 0, EMBI_DI_GRADIENT);
    return i >= 0 ? &h->dl.items.data[i] : nullptr;
}

static uint32_t chan(uint32_t c, int k)
{
    return (c >> (8 * k)) & 0xFFu;
}

/* The stop of the gradient nearest to offset x (its index in the item's stop range), or -1. */
static int nearest_stop(const ph *h, const embi_ditem *it, float x)
{
    int best = -1;
    float bd = 1e9f;
    for (uint32_t i = 0; i < it->aux2; i++) {
        float d = fabsf(h->dl.stops.data[it->aux + i].offset - x);
        if (d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

static bool offsets_monotonic(const ph *h, const embi_ditem *it)
{
    for (uint32_t i = 1; i < it->aux2; i++) {
        if (h->dl.stops.data[it->aux + i].offset < h->dl.stops.data[it->aux + i - 1].offset) {
            return false;
        }
    }
    return true;
}

EMB_TEST(color_hint_follows_its_curve)
{
    /* red 0, hint at 25%, blue 100%: the hint's midpoint is at 25%, so at 50% the weight of blue is 0.5^(1/2) */
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,#f00,25%,#00f)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    /* about one sample per pixel of the visible gap (100 px) plus the two stops */
    EMB_CHECK_EQ_INT(it->aux2, 101);
    int k = nearest_stop(&h, it, 0.5f);
    EMB_REQUIRE(k >= 0);
    const embi_dstop *st = &h.dl.stops.data[it->aux + k];
    EMB_CHECK(fabsf(st->offset - 0.5f) < 0.001f);
    EMB_CHECK(fabsf((float)chan(st->color, 0) - 75.0f) <= 3.0f);
    EMB_CHECK(fabsf((float)chan(st->color, 2) - 180.0f) <= 3.0f);
    /* the hint is not a stop of its own: the first and last stops are the two colors */
    EMB_CHECK(h.dl.stops.data[it->aux].color == PH_RED);
    EMB_CHECK(h.dl.stops.data[it->aux + it->aux2 - 1].color == PH_BLUE);
    EMB_CHECK(offsets_monotonic(&h, it));
    ph_free(&h);
}

EMB_TEST(hard_stops_keep_two_stops_at_one_offset)
{
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,#f00 50%,#00f 50%)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    /* no samples between two stops at one offset */
    EMB_CHECK_EQ_INT(it->aux2, 2);
    EMB_CHECK(h.dl.stops.data[it->aux].offset == 0.5f && h.dl.stops.data[it->aux].color == PH_RED);
    EMB_CHECK(h.dl.stops.data[it->aux + 1].offset == 0.5f && h.dl.stops.data[it->aux + 1].color == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(stop_offsets_never_decrease)
{
    /* the red stop at 60% is followed by a blue one at 20%: the blue one is raised to 60%; the green one has no
     * position and goes to the end */
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,#f00 60%,#00f 20%,#0f0)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK_EQ_INT(it->aux2, 3);
    EMB_CHECK(fabsf(h.dl.stops.data[it->aux + 0].offset - 0.6f) < 0.0001f);
    EMB_CHECK(fabsf(h.dl.stops.data[it->aux + 1].offset - 0.6f) < 0.0001f);
    EMB_CHECK(h.dl.stops.data[it->aux + 2].offset == 1.0f);
    EMB_CHECK(offsets_monotonic(&h, it));
    ph_free(&h);
}

EMB_TEST(premultiplied_blend_of_transparent_and_opaque)
{
    /* transparent red to opaque blue: the midpoint is blue at half alpha (premultiplied), not a purple of red and
     * blue at half alpha */
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,rgba(255,0,0,0),#00f)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->aux2 > 2);
    int k = nearest_stop(&h, it, 0.5f);
    const embi_dstop *st = &h.dl.stops.data[it->aux + k];
    EMB_CHECK(fabsf(st->offset - 0.5f) < 0.001f);
    EMB_CHECK(chan(st->color, 0) <= 3u);
    EMB_CHECK(chan(st->color, 2) >= 252u);
    EMB_CHECK(fabsf((float)chan(st->color, 3) - 128.0f) <= 3.0f);
    ph_free(&h);
}

EMB_TEST(oklab_gradient_samples_the_oklab_midpoint)
{
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(in oklab to right,#f00,#00f)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->aux2 > 2);
    int k = nearest_stop(&h, it, 0.5f);
    const embi_dstop *st = &h.dl.stops.data[it->aux + k];
    EMB_CHECK(fabsf((float)chan(st->color, 0) - 140.0f) <= 3.0f);
    EMB_CHECK(fabsf((float)chan(st->color, 1) - 83.0f) <= 3.0f);
    EMB_CHECK(fabsf((float)chan(st->color, 2) - 162.0f) <= 3.0f);
    EMB_CHECK(chan(st->color, 3) == 255u);
    ph_free(&h);
}

EMB_TEST(hsl_gradient_takes_the_shorter_hue_arc)
{
    /* hue 0 to hue 240: the shorter arc goes through 300 (magenta), so the midpoint is magenta */
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(in hsl to right,#f00,#00f)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    int k = nearest_stop(&h, it, 0.5f);
    const embi_dstop *st = &h.dl.stops.data[it->aux + k];
    EMB_CHECK(chan(st->color, 0) >= 252u);
    EMB_CHECK(chan(st->color, 1) <= 3u);
    EMB_CHECK(chan(st->color, 2) >= 252u);
    ph_free(&h);
}

EMB_TEST(a_plain_two_color_gradient_keeps_two_stops)
{
    /* no hint, same alpha, sRGB: the renderer's own linear blend is the one of the stated space */
    ph h = ph_make("<div style=\"width:100px;height:10px;background-image:linear-gradient(to right,#f00,#00f)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK_EQ_INT(it->aux2, 2);
    ph_free(&h);
}

EMB_TEST(gradient_tiles_with_space_gaps_are_one_item_per_tile)
{
    /* space: 35 px fits 3 tiles of 10 px: a gradient cannot repeat over the gaps, so each tile is an item */
    ph h = ph_make("<div style=\"width:35px;height:10px;background-image:linear-gradient(to right,#f00,#00f);background-size:10px 10px;background-repeat:space\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_GRADIENT), 3);
    ph_free(&h);
}

EMB_TEST(conic_hint_expands_too)
{
    ph h = ph_make("<div style=\"width:20px;height:20px;background-image:conic-gradient(red,50%,blue)\"></div>", "");
    const embi_ditem *it = first_gradient(&h);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->sub == EMBI_GRAD_CONIC_DL);
    EMB_CHECK(offsets_monotonic(&h, it));
    ph_free(&h);
}

EMB_TEST_MAIN()
