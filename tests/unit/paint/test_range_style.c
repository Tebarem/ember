/*
 * A range input styled the Chromium way (appearance: none with ::-webkit-slider-runnable-track and ::-webkit-slider-thumb):
 * the display list of the track and thumb, their places by the value, and the pseudo-elements' selectors. No GPU needed.
 */
#include "paint_harness.h"

#include <math.h>
#include <stdio.h>

static const char k_css[] =
    ":root{--accent:#3a79bb}"
    "input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:18px;margin:0;background:transparent;"
    "cursor:pointer;--pct:50%}"
    "input[type=range]:focus{outline:none}"
    "input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;"
    "background:linear-gradient(to right,var(--accent) 0 var(--pct),#4a4a4a var(--pct) 100%)}"
    "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:12px;height:12px;margin-top:-4px;"
    "border:0;border-radius:50%;background:#e6e6e6;box-shadow:0 0 0 1px rgba(0,0,0,.45),0 1px 3px rgba(0,0,0,.5)}";

/* The FILL item of a 12 by 12 box, or -1. */
static int thumb_fill(const embi_dl *d)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_FILL && fabsf(it->rect[2] - 12.0f) < 0.01f && fabsf(it->rect[3] - 12.0f) < 0.01f) {
            return (int)i;
        }
    }
    return -1;
}

EMB_TEST(styled_range_paints_the_track_and_thumb_from_their_styles)
{
    ph h = ph_make("<input type=range id=r min=0 max=100 value=50 style='--pct:50%;width:200px'>", k_css);
    int g = ph_find(&h.dl, 0, EMBI_DI_GRADIENT);
    EMB_REQUIRE(g >= 0);
    const embi_ditem *tr = &h.dl.items.data[g];
    /* the 4px track spans the content box and is centred on its 18px */
    EMB_CHECK(fabsf(tr->rect[0]) < 0.01f);
    EMB_CHECK(fabsf(tr->rect[2] - 200.0f) < 0.01f);
    EMB_CHECK(fabsf(tr->rect[3] - 4.0f) < 0.01f);
    EMB_CHECK(fabsf(tr->rect[1] - 7.0f) < 0.01f);
    /* the rounded corners clip the gradient */
    EMB_REQUIRE(tr->clip != 0);
    EMB_CHECK(fabsf(h.dl.clips.data[tr->clip].radii[0] - 2.0f) < 0.01f);
    int t = thumb_fill(&h.dl);
    EMB_REQUIRE(t >= 0);
    const embi_ditem *th = &h.dl.items.data[t];
    /* the thumb: left = frac * (track width - thumb width), top = track top + margin-top (-4) */
    EMB_CHECK(fabsf(th->rect[0] - 94.0f) < 0.01f);
    EMB_CHECK(fabsf(th->rect[1] - 3.0f) < 0.01f);
    EMB_CHECK(fabsf(th->radii[0] - 6.0f) < 0.01f);
    EMB_CHECK(th->colors[0] == 0xFFE6E6E6u);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_SHADOW) == 2);
    ph_free(&h);
}

EMB_TEST(styled_range_thumb_follows_the_value)
{
    const char *vals[3] = {"0", "37", "100"};
    float want[3] = {0.0f, 0.37f * 188.0f, 188.0f};
    for (int i = 0; i < 3; i++) {
        char html[200];
        snprintf(html, sizeof html, "<input type=range min=0 max=100 value=%s style='width:200px'>", vals[i]);
        ph h = ph_make(html, k_css);
        int t = thumb_fill(&h.dl);
        EMB_REQUIRE(t >= 0);
        EMB_CHECK(fabsf(h.dl.items.data[t].rect[0] - want[i]) < 0.01f);
        ph_free(&h);
    }
}

EMB_TEST(range_with_appearance_none_and_no_part_styles_draws_nothing)
{
    ph h = ph_make("<input type=range value=30 style='appearance:none;width:100px'>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_FILL) == 0);
    ph_free(&h);
}

EMB_TEST(native_range_ignores_the_part_styles)
{
    ph h = ph_make("<input type=range value=30 style='width:100px'>",
                   "input::-webkit-slider-thumb{width:30px;height:30px;background:red}");
    EMB_CHECK(thumb_fill(&h.dl) < 0);
    EMB_CHECK(ph_fill_of(&h.dl, PH_RED) < 0);
    ph_free(&h);
}

EMB_TEST_MAIN()
