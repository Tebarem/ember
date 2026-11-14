/*
 * Grid track sizing (W3f): the fr size, span sizes, base and growth limit initialization, and the
 * maximize step of CSS Grid 2 section 12. Values are derived by hand from the spec.
 */
#include "emb_test.h"
#include "layout/grid_int.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

/* A fixed track of px pixels (both sizing functions). */
static gtrack fixed_track(float px)
{
    gtrack t;
    memset(&t, 0, sizeof t);
    t.mink = GTK_FIXED;
    t.maxk = GTK_FIXED;
    t.minpx = px;
    t.maxpx = px;
    return t;
}

EMB_TEST(grid_fr_size_divides_space_by_flex_sum)
{
    /* 300px of free space shared by 3 fr: 100px per fr (section 12.7.1). */
    EMB_CHECK(near(embi_grid_fr_size(300.0f, 3.0f), 100.0f));
}

EMB_TEST(grid_fr_size_counts_a_flex_sum_below_one_as_one)
{
    /* A sum below one counts as one, so 100px of space with 0.5fr is 100px per fr. */
    EMB_CHECK(near(embi_grid_fr_size(100.0f, 0.5f), 100.0f));
}

EMB_TEST(grid_span_size_adds_the_gaps_between_tracks)
{
    /* Tracks 20, 30 and 40px with a 10px gap: the span over all three is 90 + 2 * 10 = 110px. */
    gtrack t[3];
    t[0] = fixed_track(20.0f);
    t[1] = fixed_track(30.0f);
    t[2] = fixed_track(40.0f);
    for (int i = 0; i < 3; i++) {
        t[i].base = t[i].grow = (float)(20 + 10 * i);
    }
    EMB_CHECK(near(embi_grid_span_size(t, 0, 3, 10.0f), 110.0f));
    /* A single track has no gap inside it. */
    EMB_CHECK(near(embi_grid_span_size(t, 1, 2, 10.0f), 30.0f));
}

EMB_TEST(grid_tracks_init_sets_base_and_limit_from_a_fixed_size)
{
    /* A fixed 50px track starts with base and growth limit both 50px, and a finite limit. */
    gtrack t[1];
    t[0] = fixed_track(50.0f);
    t[0].inf = true;
    embi_grid_tracks_init(t, 1);
    EMB_CHECK(near(t[0].base, 50.0f));
    EMB_CHECK(near(t[0].grow, 50.0f));
    EMB_CHECK(!t[0].inf);
}

EMB_TEST(grid_maximize_respects_each_growth_limit)
{
    /* Two auto tracks at base 10px. Growth limits: track A 50px (room 40), track B 20px (room 10).
     * With 100px of free space each gets an equal share of 50px, but B stops at its limit, so B
     * takes 10px, A takes the 40px it has room for, and 50px stays unused. */
    gtrack t[2];
    memset(t, 0, sizeof t);
    t[0].mink = t[1].mink = GTK_AUTO;
    t[0].maxk = t[1].maxk = GTK_AUTO;
    t[0].base = t[1].base = 10.0f;
    t[0].grow = 50.0f;
    t[1].grow = 20.0f;
    embi_grid_maximize(t, 2, 100.0f);
    EMB_CHECK(near(t[0].base, 50.0f));
    EMB_CHECK(near(t[1].base, 20.0f));
}

EMB_TEST(grid_expand_flex_gives_fr_tracks_the_leftover_space)
{
    /* Fixed 100px track and two fr tracks (1fr and 2fr) in 400px: the 300px left gives 100px and
     * 200px. */
    gtrack t[3];
    t[0] = fixed_track(100.0f);
    memset(&t[1], 0, sizeof t[1]);
    memset(&t[2], 0, sizeof t[2]);
    t[1].mink = GTK_AUTO;
    t[1].maxk = GTK_FR;
    t[1].fr = 1.0f;
    t[2].mink = GTK_AUTO;
    t[2].maxk = GTK_FR;
    t[2].fr = 2.0f;
    embi_grid_tracks_init(t, 3);
    embi_grid_expand_flex(t, 3, 300.0f);
    EMB_CHECK(near(t[1].base, 100.0f));
    EMB_CHECK(near(t[2].base, 200.0f));
}

EMB_TEST(grid_track_gutters_skip_collapsed_tracks)
{
    /* Tracks 10px, collapsed, 20px with a 5px gap: the collapsed track has no gutters, so the span is 30 +
     * one gutter = 35. */
    gtrack t[3];
    t[0] = fixed_track(10.0f);
    t[1] = fixed_track(0.0f);
    t[1].collapsed = true;
    t[2] = fixed_track(20.0f);
    embi_grid_tracks_init(t, 3);
    EMB_CHECK(near(embi_grid_gutters(t, 0, 3, 5.0f), 5.0f));
    EMB_CHECK(near(embi_grid_span_size(t, 0, 3, 5.0f), 35.0f));
}

EMB_TEST(grid_track_find_fr_freezes_a_track_above_its_share)
{
    /* Two fr tracks with base sizes 60 and 0 and 100px of leftover: the first track's share (50) is below
     * its base, so it is frozen at 60, and the second takes the rest, 40 (section 12.7.1). */
    gtrack t[2];
    memset(t, 0, sizeof t);
    t[0].mink = GTK_AUTO;
    t[0].maxk = GTK_FR;
    t[0].fr = 1.0f;
    t[0].base = 60.0f;
    t[1].mink = GTK_AUTO;
    t[1].maxk = GTK_FR;
    t[1].fr = 1.0f;
    t[1].base = 0.0f;
    float fr = embi_grid_find_fr(t, 2, 100.0f);
    EMB_CHECK(near(fr, 40.0f));
}

EMB_TEST(grid_track_span_item_over_fr_does_not_grow_the_fr_base)
{
    /* A fixed 10px track and an fr track; one item spans both with min-content 100px. Spanning items that
     * cross a flexible track are left to the flex steps, so the fr track's base stays 0 (its size comes from
     * the free space) and the fixed track keeps 10px. */
    gtrack t[2];
    memset(t, 0, sizeof t);
    t[0] = fixed_track(10.0f);
    t[1].mink = GTK_AUTO;
    t[1].maxk = GTK_FR;
    t[1].fr = 1.0f;
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 100.0f, 100.0f, 0.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 0.0f, 0);
    EMB_CHECK(near(t[0].base, 10.0f));
    EMB_CHECK(near(t[1].base, 0.0f));
}

EMB_TEST(grid_track_span_one_item_sizes_an_fr_track)
{
    /* A single item in an fr track is not spanning, so it sizes the track's base from its min-content. */
    gtrack t[1];
    memset(t, 0, sizeof t);
    t[0].mink = GTK_AUTO;
    t[0].maxk = GTK_FR;
    t[0].fr = 1.0f;
    embi_grid_tracks_init(t, 1);
    gsitem it[1] = {{0, 1, 40.0f, 40.0f, 40.0f}};
    embi_grid_resolve_intrinsic(t, 1, it, 1, 0.0f, 0);
    EMB_CHECK(near(t[0].base, 40.0f));
}

EMB_TEST(grid_track_fr_indef_spanning_item_uses_flex_factors)
{
    /* Indefinite free space: two fr tracks (1fr and 1fr) and one item spanning both with max-content 100px
     * gives 100 / (1 + 1) = 50 per fr (section 12.7.1). */
    gtrack t[2];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 2; i++) {
        t[i].mink = GTK_AUTO;
        t[i].maxk = GTK_FR;
        t[i].fr = 1.0f;
    }
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 50.0f, 100.0f, 0.0f}};
    float fr = embi_grid_fr_indef(t, 2, it, 1, 0.0f);
    EMB_CHECK(near(fr, 50.0f));
}

EMB_TEST(grid_track_stretch_auto_shares_free_space_equally)
{
    /* Two auto-max tracks with bases 10 and 20 and 30px of free space: each takes 15 (section 12.8), so the
     * bases become 25 and 35. A fixed track is not stretched. */
    gtrack t[3];
    memset(t, 0, sizeof t);
    t[0].mink = GTK_FIXED;
    t[0].maxk = GTK_AUTO;
    t[0].minpx = 10.0f;
    t[1] = t[0];
    t[1].minpx = 10.0f;
    t[2].mink = GTK_FIXED;
    t[2].maxk = GTK_FIXED;
    t[2].minpx = 7.0f;
    t[2].maxpx = 7.0f;
    embi_grid_tracks_init(t, 3);
    t[1].base = 20.0f;
    embi_grid_stretch_auto(t, 3, 30.0f);
    EMB_CHECK(near(t[0].base, 25.0f));
    EMB_CHECK(near(t[1].base, 35.0f));
    EMB_CHECK(near(t[2].base, 7.0f));
}

/* An auto track (auto minimum, auto maximum). */
static gtrack auto_track(void)
{
    gtrack t;
    memset(&t, 0, sizeof t);
    t.mink = GTK_AUTO;
    t.maxk = GTK_AUTO;
    return t;
}

/* An fr track with the given flex factor. */
static gtrack fr_track(float fr)
{
    gtrack t;
    memset(&t, 0, sizeof t);
    t.mink = GTK_AUTO;
    t.maxk = GTK_FR;
    t.fr = fr;
    return t;
}

EMB_TEST(grid_track_span_two_item_splits_equally_over_auto_tracks)
{
    /* One item over two auto tracks with a 100px contribution: both infinite growth limits, so each track
     * takes an equal 50px (section 12.5 step 2.3). */
    gtrack t[2] = {auto_track(), auto_track()};
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 100.0f, 100.0f, 100.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 50.0f));
    EMB_CHECK(near(t[1].base, 50.0f));
    EMB_CHECK(t[0].grow >= t[0].base && t[1].grow >= t[1].base);
}

EMB_TEST(grid_track_span_two_item_fills_growth_limits_first)
{
    /* minmax(auto, 30px) and auto, one item of 100px over both. The equal share is 50px, but the first
     * track stops at its 30px growth limit and the unused 20px goes to the other: 30 and 70. */
    gtrack t[2];
    t[0] = auto_track();
    t[0].maxk = GTK_FIXED;
    t[0].maxpx = 30.0f;
    t[1] = auto_track();
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 100.0f, 100.0f, 100.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 30.0f));
    EMB_CHECK(near(t[1].base, 70.0f));
}

EMB_TEST(grid_track_span_two_item_beyond_limits_goes_to_intrinsic_maximums)
{
    /* Both tracks have a finite 20px growth limit; the first has a fixed maximum, the second an auto one.
     * 100px over both: each takes its 20px up to the limit, and the 60px beyond the limits goes only to
     * the track with an intrinsic maximum (section 12.5, "beyond growth limits"): 20 and 80. */
    gtrack t[2];
    t[0] = auto_track();
    t[0].maxk = GTK_FIXED;
    t[0].maxpx = 20.0f;
    t[1] = auto_track();
    embi_grid_tracks_init(t, 2);
    t[1].inf = false;
    t[1].grow = 20.0f;
    gsitem it[1] = {{0, 2, 100.0f, 100.0f, 100.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 20.0f));
    EMB_CHECK(near(t[1].base, 80.0f));
}

EMB_TEST(grid_track_smaller_spans_first_and_planned_increases_take_the_maximum)
{
    /* Two auto tracks. Span-1 items: 30 and 50 in the first track (the planned increase is the maximum,
     * 50, not the sum), 10 in the second. Then the span-2 item of 100: the sizes are 50 + 10 = 60, so 40 is
     * shared equally, 20 each: 70 and 30. Had the span-2 item been taken first the result would be 50
     * and 50. */
    gtrack t[2] = {auto_track(), auto_track()};
    embi_grid_tracks_init(t, 2);
    gsitem it[4] = {
        {0, 2, 100.0f, 100.0f, 100.0f},
        {0, 1, 30.0f, 30.0f, 30.0f},
        {1, 2, 10.0f, 10.0f, 10.0f},
        {0, 1, 50.0f, 50.0f, 50.0f},
    };
    embi_grid_resolve_intrinsic(t, 2, it, 4, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 70.0f));
    EMB_CHECK(near(t[1].base, 30.0f));
}

EMB_TEST(grid_track_span_two_item_counts_the_gap_inside_its_span)
{
    /* A 10px gap lies inside the span: 100px of contribution leaves 90px for the tracks, 45 each. */
    gtrack t[2] = {auto_track(), auto_track()};
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 100.0f, 100.0f, 100.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 10.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 45.0f));
    EMB_CHECK(near(t[1].base, 45.0f));
}

EMB_TEST(grid_track_min_content_constraint_uses_min_contributions_only)
{
    /* Sizing under a min-content constraint: an auto track of an item with min 40 and max 100 is 40
     * (the max-content only sets the growth limit, which is not maximized under a min-content
     * constraint). */
    gtrack t[1] = {auto_track()};
    gtrack t2[1] = {auto_track()};
    gsitem it[1] = {{0, 1, 40.0f, 100.0f, 40.0f}};
    gsz z = {NAN, 0.0f, INFINITY, GSZ_MINC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t, 1, it, 1, 0.0f, &z), 40.0f));
    EMB_CHECK(near(t[0].base, 40.0f));
    /* Under a max-content constraint the same track takes the max-content contribution. */
    gsz z2 = {NAN, 0.0f, INFINITY, GSZ_MAXC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t2, 1, it, 1, 0.0f, &z2), 100.0f));
    EMB_CHECK(near(t2[0].base, 100.0f));
}

EMB_TEST(grid_track_max_content_contribution_is_limited_by_a_fixed_maximum)
{
    /* minmax(auto, 50px) with an item of min 30 and max 100. Under a max-content constraint the limited
     * contribution is 50 (the sum of the fixed maxima), floored by the minimum contribution 30: 50. Under
     * a min-content constraint the track is 30. */
    gtrack t[1] = {auto_track()};
    t[0].maxk = GTK_FIXED;
    t[0].maxpx = 50.0f;
    gtrack t2[1] = {t[0]};
    gsitem it[1] = {{0, 1, 30.0f, 100.0f, 30.0f}};
    gsz z = {NAN, 0.0f, INFINITY, GSZ_MAXC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t, 1, it, 1, 0.0f, &z), 50.0f));
    gsz z2 = {NAN, 0.0f, INFINITY, GSZ_MINC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t2, 1, it, 1, 0.0f, &z2), 30.0f));
}

EMB_TEST(grid_track_fit_content_clamps_the_growth_limit)
{
    /* fit-content(60px): an item with max-content 100 would grow the track to 100, the limit clamps it to
     * 60; a max-content of 40 gives 40; a min-content above the limit (80) gives 80, since the base size
     * is never below the minimum contribution. */
    gsz z = {NAN, 0.0f, INFINITY, GSZ_NORMAL, true};
    gtrack a[1] = {auto_track()};
    a[0].maxk = GTK_FIT;
    a[0].maxpx = 60.0f;
    gsitem i1[1] = {{0, 1, 20.0f, 100.0f, 20.0f}};
    EMB_CHECK(near(embi_grid_size_tracks(a, 1, i1, 1, 0.0f, &z), 60.0f));
    gtrack b[1] = {auto_track()};
    b[0].maxk = GTK_FIT;
    b[0].maxpx = 60.0f;
    gsitem i2[1] = {{0, 1, 20.0f, 40.0f, 20.0f}};
    EMB_CHECK(near(embi_grid_size_tracks(b, 1, i2, 1, 0.0f, &z), 40.0f));
    gtrack c[1] = {auto_track()};
    c[0].maxk = GTK_FIT;
    c[0].maxpx = 60.0f;
    gsitem i3[1] = {{0, 1, 80.0f, 100.0f, 80.0f}};
    EMB_CHECK(near(embi_grid_size_tracks(c, 1, i3, 1, 0.0f, &z), 80.0f));
}

EMB_TEST(grid_track_span_item_over_fr_tracks_grows_them_by_flex_factor)
{
    /* An auto track and two flexible tracks (1fr and 3fr); an item spans the two flexible ones with a
     * minimum contribution of 80. Only flexible tracks grow, in proportion to their flex factors:
     * 20 and 60. The auto track stays 0. */
    gtrack t[3] = {auto_track(), fr_track(1.0f), fr_track(3.0f)};
    embi_grid_tracks_init(t, 3);
    gsitem it[1] = {{1, 3, 80.0f, 80.0f, 80.0f}};
    embi_grid_resolve_intrinsic(t, 3, it, 1, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 0.0f));
    EMB_CHECK(near(t[1].base, 20.0f));
    EMB_CHECK(near(t[2].base, 60.0f));
}

EMB_TEST(grid_track_span_item_over_fixed_and_fr_grows_only_the_fr_track)
{
    /* A 10px fixed track and a 1fr track, an item over both with a minimum contribution of 50: the fixed
     * track keeps 10px and the fr track takes the remaining 40. */
    gtrack t[2];
    t[0] = fixed_track(10.0f);
    t[1] = fr_track(1.0f);
    embi_grid_tracks_init(t, 2);
    gsitem it[1] = {{0, 2, 50.0f, 50.0f, 50.0f}};
    embi_grid_resolve_intrinsic(t, 2, it, 1, 0.0f, GSZ_NORMAL);
    EMB_CHECK(near(t[0].base, 10.0f));
    EMB_CHECK(near(t[1].base, 40.0f));
}

EMB_TEST(grid_track_min_size_of_an_indefinite_container_redoes_the_flex_step)
{
    /* Two 1fr tracks, an item of 30px in the first, an indefinite container with min size 200. The
     * first pass gives 30 + 30 = 60, below the minimum, so the flexible tracks are expanded again against
     * 200: 100 each. */
    gtrack t[2] = {fr_track(1.0f), fr_track(1.0f)};
    gsitem it[1] = {{0, 1, 30.0f, 30.0f, 30.0f}};
    gsz z = {NAN, 200.0f, INFINITY, GSZ_NORMAL, true};
    EMB_CHECK(near(embi_grid_size_tracks(t, 2, it, 1, 0.0f, &z), 200.0f));
    EMB_CHECK(near(t[0].base, 100.0f));
    EMB_CHECK(near(t[1].base, 100.0f));
}

EMB_TEST(grid_track_max_size_of_an_indefinite_container_redoes_the_flex_step)
{
    /* Two 1fr tracks, an item with min 20 and max 90 in the first. The fr size from the item is 90, so the
     * first pass gives 180, above the maximum of 100: expanded against 100 the fr size is 50, 50 each. */
    gtrack t[2] = {fr_track(1.0f), fr_track(1.0f)};
    gsitem it[1] = {{0, 1, 20.0f, 90.0f, 20.0f}};
    gsz z = {NAN, 0.0f, 100.0f, GSZ_NORMAL, true};
    EMB_CHECK(near(embi_grid_size_tracks(t, 2, it, 1, 0.0f, &z), 100.0f));
    EMB_CHECK(near(t[0].base, 50.0f));
    EMB_CHECK(near(t[1].base, 50.0f));
}

EMB_TEST(grid_track_min_content_constraint_has_a_zero_flex_fraction)
{
    /* Grid 2 12.7.1: under a min-content constraint the used flex fraction is zero, so [1fr, 2fr] with
     * items of 10 and 30 stay at their base sizes (total 40); under max-content the fr size is 15. */
    gtrack t[2] = {fr_track(1.0f), fr_track(2.0f)};
    gtrack t2[2] = {fr_track(1.0f), fr_track(2.0f)};
    gsitem it[2] = {{0, 1, 10.0f, 10.0f, 10.0f}, {1, 2, 30.0f, 30.0f, 30.0f}};
    gsz z = {NAN, 0.0f, INFINITY, GSZ_MINC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t, 2, it, 2, 0.0f, &z), 40.0f));
    EMB_CHECK(near(t[0].base, 10.0f));
    EMB_CHECK(near(t[1].base, 30.0f));
    gsz z2 = {NAN, 0.0f, INFINITY, GSZ_MAXC, true};
    EMB_CHECK(near(embi_grid_size_tracks(t2, 2, it, 2, 0.0f, &z2), 45.0f));
    EMB_CHECK(near(t2[0].base, 15.0f));
    EMB_CHECK(near(t2[1].base, 30.0f));
}

EMB_TEST_MAIN()
