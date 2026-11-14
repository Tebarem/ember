/*
 * Grid containers (W3f), end to end through the layout harness: track sizes, placement, gaps,
 * spans, named areas and self alignment. Expected positions are derived by hand from CSS Grid 2 and
 * CSS Box Alignment 3. Every test also checks that no layout memory is left behind.
 */
#include "layout_harness.h"

#include <math.h>


static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static size_t g_mem_before;

static lh gd_make(const char *html, const char *css)
{
    static bool warmed;
    if (!warmed) {
        lh w = lh_make("<body><div style='display:flex'>x <b>y</b></div></body>", "");
        lh_free(&w);
        warmed = true;
    }
    g_mem_before = embi_mem_live();
    return lh_make(html, css);
}

static void gd_free(lh *h)
{
    lh_free(h);
    EMB_CHECK(embi_mem_live() == g_mem_before);
}

static bool rect_of(lh *h, const char *id, embi_rectf *out)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, out);
}

EMB_TEST(grid_fixed_columns_wrap_items_to_the_next_row)
{
    /* Two 100px columns: the third item starts row 2 at x=0, below the first row (height 20px). */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:100px 100px'>"
                   "<div id=a style='height:20px'></div>"
                   "<div id=b style='height:20px'></div>"
                   "<div id=c style='height:20px'></div></div></body>",
                   "");
    embi_rectf a, b, c;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(a.x, 0) && near(a.y, 0));
    EMB_CHECK(near(b.x, 100) && near(b.y, 0));
    EMB_CHECK(near(c.x, 0) && near(c.y, 20));
    gd_free(&h);
}

EMB_TEST(grid_fr_columns_share_the_leftover_space)
{
    /* 300px container with 1fr and 2fr columns: the first column is 100px, the second 200px. */
    lh h = gd_make("<body><div id=g style='display:grid;width:300px;grid-template-columns:1fr 2fr'>"
                   "<div id=a style='height:10px'></div>"
                   "<div id=b style='height:10px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 100));
    EMB_CHECK(near(b.x, 100) && near(b.w, 200));
    gd_free(&h);
}

EMB_TEST(grid_gap_separates_tracks)
{
    /* Two 50px columns with a 10px column gap: the second column starts at 60px. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:50px 50px;column-gap:10px'>"
                   "<div id=a style='height:10px'></div>"
                   "<div id=b style='height:10px'></div></div></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.x, 60));
    gd_free(&h);
}

EMB_TEST(grid_column_span_covers_two_tracks)
{
    /* An item spanning two 100px columns is 200px wide. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:100px 100px 100px'>"
                   "<div id=a style='grid-column:span 2;height:10px'></div>"
                   "<div id=b style='height:10px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 200));
    EMB_CHECK(near(b.x, 200) && near(b.y, 0));
    gd_free(&h);
}

EMB_TEST(grid_justify_self_end_aligns_within_the_area)
{
    /* A 50px item with justify-self: end in a 100px column sits at x=50. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:100px'>"
                   "<div id=a style='width:50px;height:10px;justify-self:end'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.x, 50));
    EMB_CHECK(near(a.w, 50));
    gd_free(&h);
}

EMB_TEST(grid_named_area_places_the_item)
{
    /* grid-template-areas "a b" "c d": item b is in row 0, column 1. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:100px 100px;"
                   "grid-template-areas:\"a b\" \"c d\"'>"
                   "<div id=b style='grid-area:b;height:10px'></div></div></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.x, 100) && near(b.y, 0));
    gd_free(&h);
}

EMB_TEST(grid_auto_fit_collapsed_track_takes_no_gutter)
{
    /* repeat(auto-fit, 50px) with a 10px gap in 200px: floor((200+10)/(50+10)) = 3 tracks. Only the first
     * is covered, so tracks 2 and 3 collapse. The item in track 3 starts after one gutter, at 50 + 10 = 60
     * (the gutters of a collapsed track collapse with it), not at 70. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:repeat(auto-fit,50px);"
                   "column-gap:10px;width:200px'>"
                   "<div id=a style='height:10px'></div>"
                   "<div id=b style='height:10px;grid-column:3'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 0) && near(a.w, 50));
    EMB_CHECK(near(b.x, 60) && near(b.w, 50));
    gd_free(&h);
}

EMB_TEST(grid_auto_fill_count_counts_the_gap)
{
    /* repeat(auto-fill, 50px) with a 10px gap in 200px: floor((200+10)/(50+10)) = 3 tracks, so a item
     * spanning all of them is 3*50 + 2*10 = 170 wide. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:repeat(auto-fill,50px);"
                   "column-gap:10px;width:200px'>"
                   "<div id=a style='height:10px;grid-column:1 / -1'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 170));
    gd_free(&h);
}

EMB_TEST(grid_percentage_row_in_auto_height_grid_is_auto)
{
    /* grid-template-rows: 50% in a grid whose height is auto has an indefinite basis, so the row is auto
     * and sizes to its 20px item (not to a fixed zero-pixel track). */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-rows:50%;width:100px'>"
                   "<div id=a style='height:20px'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 20));
    gd_free(&h);
}

EMB_TEST(grid_both_spans_keep_the_start_span)
{
    /* grid-column: span 2 / span 3 keeps the start span (2 columns, the end span is removed), so the item
     * is 100px wide in 50px columns. */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:50px 50px 50px'>"
                   "<div id=a style='height:10px;grid-column:span 2 / span 3'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 100));
    gd_free(&h);
}

EMB_TEST(grid_column_locked_item_after_auto_item_uses_the_shared_cursor)
{
    /* Columns 50px 50px, sparse. A is auto and takes (row 0, column 0), so the cursor is at column 1 of
     * row 0. B is locked to column 1, which is behind the cursor, so B moves to row 1. The cursor then
     * is at column 0 of row 1 (where B is), so C (auto) searches on from there and lands at row 1, column 1.
     * Row heights are 20px:
     * A (0,0), B (x 0, y 20), C (x 50, y 20). */
    lh h = gd_make("<body><div id=g style='display:grid;grid-template-columns:50px 50px'>"
                   "<div id=a style='height:20px'></div>"
                   "<div id=b style='height:20px;grid-column:1'></div>"
                   "<div id=c style='height:20px'></div></div></body>",
                   "");
    embi_rectf a, b, c;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(a.x, 0) && near(a.y, 0));
    EMB_CHECK(near(b.x, 0) && near(b.y, 20));
    EMB_CHECK(near(c.x, 50) && near(c.y, 20));
    gd_free(&h);
}

EMB_TEST(grid_subgrid_items_share_the_parent_tracks)
{
    /* Parent columns 100px 50px, 10px gap. The subgrid spans both columns, so its two children sit at
     * x=0 (w 100) and x=110 (w 50). */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:100px 50px;column-gap:10px'>"
                   "<div id=s style='display:grid;grid-column:1 / 3;grid-template-columns:subgrid'>"
                   "<div id=a style='height:5px'></div><div id=b style='height:5px'></div></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 0) && near(a.w, 100));
    EMB_CHECK(near(b.x, 110) && near(b.w, 50));
    gd_free(&h);
}

EMB_TEST(grid_min_content_width_uses_a_zero_flex_fraction)
{
    /* width:min-content on a 1fr 2fr grid with items of 10px and 30px is 40px, not 45px. */
    lh h = gd_make("<body><div id=g style='display:grid;width:min-content;grid-template-columns:1fr 2fr'>"
                   "<div style='width:10px;height:5px'></div><div style='width:30px;height:5px'></div>"
                   "</div></body>",
                   "");
    embi_rectf g;
    EMB_REQUIRE(rect_of(&h, "g", &g));
    EMB_CHECK(near(g.w, 40));
    gd_free(&h);
}

EMB_TEST(grid_container_baseline_uses_the_first_row_non_baseline_item)
{
    /* Row 1 holds a plain item with a baseline at 20 (an inline-block of height 20); row 2 holds two
     * baseline-aligned items. The container's first baseline comes from row 1, so in a flex row with
     * align-items:baseline an empty sibling (baseline at its bottom edge) sits at g.y + 20. */
    lh h = gd_make("<body><div style='display:flex;align-items:baseline;line-height:0'>"
                   "<div id=g style='display:grid;grid-template-columns:50px 50px;"
                   "grid-template-rows:20px 40px'>"
                   "<div><span style='display:inline-block;width:5px;height:20px'></span></div><div></div>"
                   "<div id=b style='align-self:baseline;height:10px'></div>"
                   "<div id=c style='align-self:baseline;height:30px'></div></div>"
                   "<div id=s style='width:5px;height:0'></div></div></body>",
                   "");
    embi_rectf g, s, b;
    EMB_REQUIRE(rect_of(&h, "g", &g));
    EMB_REQUIRE(rect_of(&h, "s", &s));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(s.y, g.y + 20));
    EMB_CHECK(near(b.y, g.y + 40));
    gd_free(&h);
}

EMB_TEST(grid_nested_subgrids_share_the_root_tracks_and_restore_state)
{
    /* A subgrid inside a subgrid, followed by a sibling item after the nested one: all of them
     * resolve against the root's 100px 50px columns. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:100px 50px;column-gap:10px'>"
                   "<div style='display:grid;grid-column:1 / 3;grid-template-columns:subgrid'>"
                   "<div style='display:grid;grid-column:2 / 3;grid-template-columns:subgrid'>"
                   "<div id=a style='height:5px'></div></div>"
                   "<div id=b style='grid-column:1;height:5px'></div></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.x, 110) && near(a.w, 50));
    EMB_CHECK(near(b.x, 0) && near(b.w, 100));
    gd_free(&h);
}

EMB_TEST(grid_abspos_child_uses_its_grid_area_as_the_containing_block)
{
    /* Columns 40px 60px, rows 30px 30px; the container is positioned. The abspos child is placed in
     * columns 2 and row 2, so its area starts at (40, 30) and a stretched box is 60 x 30. */
    lh h = gd_make("<body><div style='position:relative;display:grid;grid-template-columns:40px 60px;"
                   "grid-template-rows:30px 30px'><div id=a style='position:absolute;grid-column:2 / 3;"
                   "grid-row:2 / 3;left:0;right:0;top:0;bottom:0'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.x, 40) && near(a.y, 30) && near(a.w, 60) && near(a.h, 30));
    gd_free(&h);
}

EMB_TEST(grid_auto_fill_count_accounts_for_gaps)
{
    /* 100px wide, 30px tracks with a 10px gap: n tracks need 30n + 10(n-1) <= 100, so n = 2 and the
     * third item starts a new row at y=20 (10px item, 10px gap). */
    lh h = gd_make("<body><div style='display:grid;width:100px;grid-template-columns:repeat(auto-fill,30px);"
                   "gap:10px'><div style='height:10px'></div><div style='height:10px'></div>"
                   "<div id=c style='height:10px'></div></div></body>",
                   "");
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.x, 0) && near(c.y, 20));
    gd_free(&h);
}

EMB_TEST(grid_area_creates_implicit_start_and_end_line_names)
{
    /* The area "m" spans column 2 (x 50..100); grid-column: m-start / m-end covers exactly it. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:50px 50px 50px;"
                   "grid-template-areas:\"a m c\"'><div id=i style='grid-column:m-start / m-end;"
                   "height:5px'></div></div></body>",
                   "");
    embi_rectf i;
    EMB_REQUIRE(rect_of(&h, "i", &i));
    EMB_CHECK(near(i.x, 50) && near(i.w, 50));
    gd_free(&h);
}

EMB_TEST(grid_container_min_height_grows_stretched_auto_rows)
{
    /* Two auto rows with 10px content in a container of min-height 100: the 80px of free space is
     * shared equally, so the rows are 50px each and the second item starts at y=50. */
    lh h = gd_make("<body><div style='display:grid;min-height:100px'><div style='height:10px'></div>"
                   "<div id=b style='height:10px'></div></div></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.y, 50));
    gd_free(&h);
}

EMB_TEST(grid_content_distribution_space_between_moves_tracks_apart)
{
    /* 200px wide, two 50px columns, justify-content: space-between: the second starts at 150. */
    lh h = gd_make("<body><div style='display:grid;width:200px;grid-template-columns:50px 50px;"
                   "justify-content:space-between'><div style='height:5px'></div>"
                   "<div id=b style='height:5px'></div></div></body>",
                   "");
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.x, 150));
    gd_free(&h);
}

EMB_TEST(grid_percentage_padding_resolves_against_the_grid_area_width)
{
    /* A 200px column: padding 10% is 20px, so the content box of the item starts 20px in. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:200px'>"
                   "<div id=a style='padding:10%'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 200) && near(a.h, 40));
    gd_free(&h);
}

EMB_TEST(grid_auto_margins_center_an_item_in_its_area)
{
    /* A 100px column, a 20px wide item with auto side margins sits at x=40. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:100px'>"
                   "<div id=a style='width:20px;height:5px;margin:0 auto'></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.x, 40) && near(a.w, 20));
    gd_free(&h);
}

EMB_TEST(grid_aspect_ratio_item_in_an_auto_row_sizes_its_auto_column_from_the_row)
{
    /* Two auto columns. The second item is 100px tall, so the row is 100px. The first item is stretched
     * in the row (align-self: stretch) and has aspect-ratio 1 / 1, so its column contribution follows the
     * row size: the columns are sized again after the rows (section 12.1 step 3) and the first column is
     * 100px, putting the second item at x=100. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:auto auto;justify-content:start'>"
                   "<div id=a style='aspect-ratio:1/1;align-self:stretch'></div>"
                   "<div id=b style='width:10px;height:100px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 100) && near(a.h, 100));
    EMB_CHECK(near(b.x, 100) && near(b.y, 0));
    gd_free(&h);
}

EMB_TEST(grid_aspect_ratio_item_with_a_height_contributes_the_transferred_width)
{
    /* height: 30px with aspect-ratio 2 / 1 gives a 60px wide item, and so a 60px auto column. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:auto auto;justify-content:start'>"
                   "<div id=a style='height:30px;aspect-ratio:2/1'></div>"
                   "<div id=b style='height:5px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 60) && near(a.h, 30));
    EMB_CHECK(near(b.x, 60));
    gd_free(&h);
}

EMB_TEST(grid_ratio_height_is_clamped_before_the_width_is_derived)
{
    /* height 25px raised by min-height 30px gives a 30px tall item; with ratio 2 / 1 it is 60px wide, but
     * max-width: 50px limits that to 50px. The column is 50px. */
    lh h = gd_make("<body><div style='display:grid;grid-template-columns:auto auto;justify-content:start'>"
                   "<div id=a style='height:25px;min-height:30px;max-width:50px;aspect-ratio:2/1'></div>"
                   "<div id=b style='height:5px'></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 50) && near(a.h, 30));
    EMB_CHECK(near(b.x, 50));
    gd_free(&h);
}

EMB_TEST(grid_min_height_wins_over_max_height)
{
    /* min-height 100px and max-height 50px: the minimum wins, for an item and for the container. */
    lh h = gd_make("<body><div id=g style='display:grid'>"
                   "<div id=a style='min-height:100px;max-height:50px'></div></div>"
                   "<div id=g2 style='display:grid;min-height:100px;max-height:50px'>"
                   "<div id=b style='height:10px'></div></div></body>",
                   "");
    embi_rectf a, g2;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "g2", &g2));
    EMB_CHECK(near(a.h, 100));
    EMB_CHECK(near(g2.h, 100));
    gd_free(&h);
}

EMB_TEST(grid_abspos_grid_container_fills_the_height_between_top_and_bottom)
{
    /* An absolutely positioned display: grid box with top: 0 and bottom: 0 and an auto height fills its
     * 200px containing block, whatever its content. */
    lh h = gd_make("<body><div style='position:relative;width:300px;height:200px'>"
                   "<div id=a style='position:absolute;display:grid;top:0;bottom:0;left:0;width:50px'>"
                   "<div style='height:10px'></div></div></div></body>",
                   "");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 200) && near(a.w, 50));
    gd_free(&h);
}

EMB_TEST(grid_bare_text_becomes_an_anonymous_item_with_a_line)
{
    /* Text directly inside a grid container is wrapped in an anonymous grid item that holds its line
     * (W3h: the container used to keep no line for it and was 0 high). */
    lh h = gd_make("<body><div id=g style='display:grid;font:16px/20px sans-serif'>x</div></body>", "");
    embi_rectf g;
    EMB_REQUIRE(rect_of(&h, "g", &g));
    EMB_CHECK(near(g.h, 20));
    gd_free(&h);
}

EMB_TEST(grid_inline_grid_aligns_on_its_first_baseline)
{
    /* Two inline grids on one line. A has two 20px rows (40px high), B one 30px row. An inline-grid
     * aligns on its FIRST baseline, synthesized from the first row's item (its bottom edge), so
     * A.y + 20 == B.y + 30. A last baseline would give A.y + 40 == B.y + 30. */
    lh h = gd_make("<body><div style='font-size:0'><div id=a style='display:inline-grid;width:10px'>"
                   "<div style='height:20px'></div><div style='height:20px'></div></div>"
                   "<div id=b style='display:inline-grid;width:10px'><div style='height:30px'></div></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.h, 40));
    EMB_CHECK(near(b.h, 30));
    EMB_CHECK(near(a.y + 20, b.y + 30));
    gd_free(&h);
}

EMB_TEST(grid_container_baseline_uses_the_first_row_with_items_in_column_order)
{
    /* The first baseline comes from the item at the lowest column of the first row (grid order), not
     * from the item that comes first in the source: B is first in the source but in column 2. */
    lh h = gd_make("<body><div style='font-size:0'><div id=a style='display:inline-grid;grid-template-columns:10px 10px;"
                   "align-items:start'>"
                   "<div style='grid-area:1/2;height:40px'></div><div style='grid-area:1/1;height:20px'></div></div>"
                   "<div id=b style='display:inline-grid;width:10px'><div style='height:30px'></div></div></div></body>",
                   "");
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    /* A's baseline is the bottom of its column-1 item, 20px down; B's is 30px down. */
    EMB_CHECK(near(a.y + 20, b.y + 30));
    gd_free(&h);
}

EMB_TEST_MAIN()
