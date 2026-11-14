/*
 * Smooth programmatic scrolls, the overlay scrollbar fade and the scrollbar thumb geometry (W4d, scroll.c). All on
 * plain numbers, so they run without a page.
 */
#include "paint_harness.h"

#include "paint/scroll.h"

#include <math.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

EMB_TEST(smooth_scroll_runs_150_ms_with_an_ease_out_curve)
{
    embi_scroll_anim a;
    memset(&a, 0, sizeof a);
    embi_scroll_anim_start(&a, 0.0f, 0.0f, 0.0f, 100.0f, 1000.0);
    float x = -1, y = -1;
    /* Halfway in time is 87.5% of the way: ease-out cubic, 1 - (1 - 0.5)^3. */
    EMB_CHECK(embi_scroll_anim_sample(&a, 1075.0, &x, &y));
    EMB_CHECK(near(y, 87.5f));
    EMB_CHECK(near(x, 0.0f));
    /* Ahead of the curve's first half it is already past the middle. */
    EMB_CHECK(embi_scroll_anim_sample(&a, 1037.5, &x, &y));
    EMB_CHECK(y > 50.0f);
    /* At 150 ms the target is reached and the animation ends. */
    EMB_CHECK(!embi_scroll_anim_sample(&a, 1150.0, &x, &y));
    EMB_CHECK(near(y, 100.0f));
    EMB_CHECK(!a.active);
    /* Later samples stay at the target. */
    EMB_CHECK(!embi_scroll_anim_sample(&a, 5000.0, &x, &y));
    EMB_CHECK(near(y, 100.0f));
}

EMB_TEST(ease_out_curve_endpoints)
{
    EMB_CHECK(near(embi_scroll_ease_out(0.0f), 0.0f));
    EMB_CHECK(near(embi_scroll_ease_out(1.0f), 1.0f));
    EMB_CHECK(embi_scroll_ease_out(0.25f) > 0.25f);
}

EMB_TEST(restarting_a_smooth_scroll_replaces_it)
{
    embi_scroll_anim a;
    memset(&a, 0, sizeof a);
    embi_scroll_anim_start(&a, 0.0f, 0.0f, 0.0f, 100.0f, 0.0);
    float x = -1, y = -1;
    EMB_CHECK(embi_scroll_anim_sample(&a, 50.0, &x, &y));
    /* A new target from where the scroll is now. */
    embi_scroll_anim_start(&a, x, y, 0.0f, 20.0f, 50.0);
    EMB_CHECK(!embi_scroll_anim_sample(&a, 300.0, &x, &y));
    EMB_CHECK(near(y, 20.0f));
}

EMB_TEST(scrollbar_fades_one_second_after_the_last_scroll)
{
    embi_sb_fade f;
    memset(&f, 0, sizeof f);
    EMB_CHECK(near(embi_sb_alpha(&f, 0.0), 0.0f));
    embi_sb_note_scroll(&f, 100.0);
    EMB_CHECK(near(embi_sb_alpha(&f, 500.0), 1.0f));
    EMB_CHECK(near(embi_sb_alpha(&f, 1100.0), 1.0f));
    /* Half of the fade (200 ms after the visible second). */
    EMB_CHECK(near(embi_sb_alpha(&f, 1200.0), 0.5f));
    EMB_CHECK(near(embi_sb_alpha(&f, 1300.0), 0.0f));
    /* A new scroll shows it again. */
    embi_sb_note_scroll(&f, 2000.0);
    EMB_CHECK(near(embi_sb_alpha(&f, 2100.0), 1.0f));
}

EMB_TEST(scrollbar_thumb_is_proportional_and_moves_with_the_offset)
{
    float start = -1, len = -1;
    /* A 100 px track over 50 of 200: a quarter of the track, at the middle of the offset range. */
    EMB_CHECK(embi_sb_thumb(100.0f, 50.0f, 200.0f, 75.0f, &start, &len));
    EMB_CHECK(near(len, 25.0f));
    EMB_CHECK(near(start, 37.5f));
    /* At the end of the range the thumb is at the end of the track. */
    EMB_CHECK(embi_sb_thumb(100.0f, 50.0f, 200.0f, 1000.0f, &start, &len));
    EMB_CHECK(near(start + len, 100.0f));
    /* A tiny thumb keeps its minimum length. */
    EMB_CHECK(embi_sb_thumb(100.0f, 50.0f, 100000.0f, 0.0f, &start, &len));
    EMB_CHECK(near(len, EMBI_SB_MIN_THUMB));
    /* Nothing to scroll: no bar. */
    EMB_CHECK(!embi_sb_thumb(100.0f, 50.0f, 50.0f, 0.0f, &start, &len));
}

EMB_TEST(scrollbar_defaults_are_an_overlay_thumb)
{
    embi_sb_style s = embi_sb_style_default();
    EMB_CHECK(near(s.width, 8.0f));
    EMB_CHECK((s.thumb >> 24) != 0);
    EMB_CHECK((s.track >> 24) == 0);
}

EMB_TEST_MAIN()
