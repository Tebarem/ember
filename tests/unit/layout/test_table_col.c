/*
 * Column width distribution (table_col.c, W3g) on plain arrays: the auto layout of CSS Tables 3
 * section 3.5 and the fixed layout of section 3.4. The expected values are worked out by hand from
 * the algorithms, and the excess rule of the fixed layout is the one the WPT test
 * fixed-layout-excess-width-distribution-001 expects.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

EMB_TEST(table_auto_minimum_when_no_room)
{
    float mn[2] = {10, 20}, mx[2] = {30, 60}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, nullptr, nullptr, 30, out);
    EMB_CHECK(near(out[0], 10));
    EMB_CHECK(near(out[1], 20));
}

EMB_TEST(table_auto_between_min_and_max_is_proportional_to_range)
{
    /* room 60, minimum sum 30, maximum sum 90: each column takes (max - min) * 30 / 60 more. */
    float mn[2] = {10, 20}, mx[2] = {30, 60}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, nullptr, nullptr, 60, out);
    EMB_CHECK(near(out[0], 20));
    EMB_CHECK(near(out[1], 40));
}

EMB_TEST(table_auto_past_maximum_is_proportional_to_maximum)
{
    /* room 180 is 90 past the maximum sum 90, shared in proportion to the maxima 30 and 60. */
    float mn[2] = {10, 20}, mx[2] = {30, 60}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, nullptr, nullptr, 180, out);
    EMB_CHECK(near(out[0], 60));
    EMB_CHECK(near(out[1], 120));
}

EMB_TEST(table_auto_percentage_column_takes_its_share_first)
{
    /* The percentage column gets 50% of 100; the auto column takes the other 50. */
    float mn[2] = {10, 10}, mx[2] = {10, 10}, pct[2] = {0.5f, NAN}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, pct, nullptr, 100, out);
    EMB_CHECK(near(out[0], 50));
    EMB_CHECK(near(out[1], 50));
}

EMB_TEST(table_fixed_excess_goes_to_fixed_columns_in_proportion)
{
    /* The WPT case: widths 20px and 10px, a 10% column of 300px. The 240px excess goes to the two px
     * columns in the ratio 20:10 and the percentage column keeps its 30px. */
    float spec[3] = {20, 10, NAN}, pct[3] = {NAN, NAN, 0.1f}, out[3] = {0, 0, 0};
    embi_table_distribute_fixed(3, spec, pct, nullptr, 300, out);
    EMB_CHECK(near(out[0], 180));
    EMB_CHECK(near(out[1], 90));
    EMB_CHECK(near(out[2], 30));
}

EMB_TEST(table_fixed_auto_columns_share_the_rest)
{
    float spec[2] = {NAN, NAN}, out[2] = {0, 0};
    embi_table_distribute_fixed(2, spec, nullptr, nullptr, 100, out);
    EMB_CHECK(near(out[0], 50));
    EMB_CHECK(near(out[1], 50));
}

EMB_TEST(table_fixed_keeps_widths_when_there_is_no_room)
{
    float spec[2] = {80, 40}, out[2] = {0, 0};
    embi_table_distribute_fixed(2, spec, nullptr, nullptr, 100, out);
    EMB_CHECK(near(out[0], 80));
    EMB_CHECK(near(out[1], 40));
}

EMB_TEST(table_auto_constrained_columns_reach_their_maximum_before_the_auto_ones)
{
    /* Guesses: minima 10+10; the constrained column at its maximum 40 (sum 50); every column at its
     * maximum (sum 80). Room 60 is a third of the way from the third to the fourth guess: the auto
     * column goes from 10 to 40 by 10, the constrained one stays at 40. */
    float mn[2] = {10, 10}, mx[2] = {40, 40}, out[2] = {0, 0};
    uint8_t cons[2] = {1, 0};
    embi_table_distribute_auto(2, mn, mx, nullptr, cons, 60, out);
    EMB_CHECK(near(out[0], 40));
    EMB_CHECK(near(out[1], 20));
}

EMB_TEST(table_auto_percentages_over_the_room_shrink_towards_the_minima)
{
    /* Two 60% columns over room 100: the percentage guess is 60 + 60 = 120, the minimum guess 20, so
     * each column is 80 / 100 of the way: 10 + 50 * 0.8 = 50. */
    float mn[2] = {10, 10}, mx[2] = {10, 10}, pct[2] = {0.6f, 0.6f}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, pct, nullptr, 100, out);
    EMB_CHECK(near(out[0], 50));
    EMB_CHECK(near(out[1], 50));
}

EMB_TEST(table_auto_extra_goes_to_constrained_columns_when_none_is_auto)
{
    /* Both columns are constrained (maxima 20 and 40): the 140 past the maximum sum 60 is shared 1:2. */
    float mn[2] = {10, 10}, mx[2] = {20, 40}, out[2] = {0, 0};
    uint8_t cons[2] = {1, 1};
    embi_table_distribute_auto(2, mn, mx, nullptr, cons, 200, out);
    EMB_CHECK(near(out[0], 20 + 140.0 / 3));
    EMB_CHECK(near(out[1], 40 + 280.0 / 3));
}

EMB_TEST(table_auto_extra_goes_to_percentage_columns_when_nothing_else_can_take_it)
{
    /* 20% and 30% columns over room 100: they start at 20 and 30, and the 50 left is shared 2:3. */
    float mn[2] = {1, 1}, mx[2] = {1, 1}, pct[2] = {0.2f, 0.3f}, out[2] = {0, 0};
    embi_table_distribute_auto(2, mn, mx, pct, nullptr, 100, out);
    EMB_CHECK(near(out[0], 40));
    EMB_CHECK(near(out[1], 60));
}

EMB_TEST(table_fixed_percentages_over_the_room_scale_in_proportion)
{
    /* Two 80% columns want 80 + 80 of 100: they take 50 each. */
    float spec[2] = {NAN, NAN}, pct[2] = {0.8f, 0.8f}, out[2] = {0, 0};
    embi_table_distribute_fixed(2, spec, pct, nullptr, 100, out);
    EMB_CHECK(near(out[0], 50));
    EMB_CHECK(near(out[1], 50));
}

EMB_TEST(table_fixed_percentage_column_adds_the_cell_border_and_padding)
{
    /* A 50% column with 10px of cell border and padding wants 60 of 100; the auto column gets 40. */
    float spec[2] = {NAN, NAN}, pct[2] = {0.5f, NAN}, pe[2] = {10, 0}, out[2] = {0, 0};
    embi_table_distribute_fixed(2, spec, pct, pe, 100, out);
    EMB_CHECK(near(out[0], 60));
    EMB_CHECK(near(out[1], 40));
}

EMB_TEST(table_fixed_zero_width_columns_share_the_excess_evenly)
{
    float spec[2] = {0, 0}, out[2] = {1, 1};
    embi_table_distribute_fixed(2, spec, nullptr, nullptr, 100, out);
    EMB_CHECK(near(out[0], 50));
    EMB_CHECK(near(out[1], 50));
}

EMB_TEST(table_fixed_excess_goes_to_percentage_columns_in_proportion_without_px_columns)
{
    /* 20% and 30% of 100 leave 50: shared 2:3, so 40 and 60. */
    float pct[2] = {0.2f, 0.3f}, out[2] = {0, 0};
    embi_table_distribute_fixed(2, nullptr, pct, nullptr, 100, out);
    EMB_CHECK(near(out[0], 40));
    EMB_CHECK(near(out[1], 60));
}

EMB_TEST(table_attr_box_without_element_gives_the_default)
{
    embi_box b;
    memset(&b, 0, sizeof b);
    EMB_CHECK(embi_table_attr_uint(&b, "colspan", 1, 1, 1000) == 1);
    EMB_CHECK(embi_table_attr_uint(&b, "rowspan", 1, 0, 65534) == 1);
}

/* Reads attribute `name` of element `id` the way the table code does: through a box that has the element. */
static uint32_t attr_of(lh *h, const char *id, const char *name, uint32_t dflt, uint32_t lo, uint32_t hi)
{
    embi_box b;
    memset(&b, 0, sizeof b);
    b.el = lh_id(h, id);
    return embi_table_attr_uint(&b, name, dflt, lo, hi);
}

EMB_TEST(table_attr_span_parsing_and_clamping)
{
    lh h = lh_make("<body><table><tr>"
                   "<td id=zero colspan=0 rowspan=0></td>"
                   "<td id=big colspan=5000 rowspan=70000></td>"
                   "<td id=junk colspan=' 3x' rowspan='  2abc'></td>"
                   "<td id=neg colspan=-4 rowspan=x></td>"
                   "<td id=huge colspan=99999999999999999999></td>"
                   "<td id=none></td>"
                   "<td id=plus colspan=+3 rowspan=' +2'></td>"
                   "</tr></table></body>", "");
    EMB_CHECK(attr_of(&h, "zero", "colspan", 1, 1, 1000) == 1);   /* 0 is raised to the minimum */
    EMB_CHECK(attr_of(&h, "zero", "rowspan", 1, 0, 65534) == 0);  /* rowspan=0 is allowed */
    EMB_CHECK(attr_of(&h, "big", "colspan", 1, 1, 1000) == 1000);
    EMB_CHECK(attr_of(&h, "big", "rowspan", 1, 0, 65534) == 65534);
    EMB_CHECK(attr_of(&h, "junk", "colspan", 1, 1, 1000) == 3);   /* leading space, trailing junk */
    EMB_CHECK(attr_of(&h, "junk", "rowspan", 1, 0, 65534) == 2);
    EMB_CHECK(attr_of(&h, "neg", "colspan", 1, 1, 1000) == 1);    /* not a digit: the default */
    EMB_CHECK(attr_of(&h, "neg", "rowspan", 1, 0, 65534) == 1);
    EMB_CHECK(attr_of(&h, "huge", "colspan", 1, 1, 1000) == 1000);
    EMB_CHECK(attr_of(&h, "none", "colspan", 1, 1, 1000) == 1);   /* missing attribute */
    EMB_CHECK(attr_of(&h, "plus", "colspan", 1, 1, 1000) == 3);   /* a leading + is allowed */
    EMB_CHECK(attr_of(&h, "plus", "rowspan", 1, 0, 65534) == 2);
    lh_free(&h);
}

EMB_TEST_MAIN()
