/*
 * Multi-column layout (CSS Multi-column Layout 1, multicol.c): the used column count and width, balancing, blocks cut across
 * columns (truncated margins, sliced borders), forced and avoided breaks, column-span: all, column-fill: auto with a fixed
 * height, right-to-left order. Expected values are derived by hand from the spec.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.02;
}

/* The rectangles of element id (one per fragment); returns their number. */
static uint32_t rects(lh *h, const char *id, embi_rectf *out, uint32_t cap)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr ? embi_layout_client_rects(h->eng, el, out, cap) : 0;
}

/* The first fragment of the element (a box that a column break cuts has one rectangle per column). */
static bool first_rect(lh *h, const char *id, embi_rectf *out)
{
    return rects(h, id, out, 1) != 0;
}

static bool rect1(lh *h, const char *id, embi_rectf *out)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, out);
}

EMB_TEST(column_count_and_width_follow_the_pseudo_algorithm)
{
    lh h = lh_make("<body>"
                   "<div id=m1 style='width:300px;column-count:3;column-gap:0'><div id=c1 style='height:10px'></div></div>"
                   "<div id=m2 style='width:350px;column-width:100px;column-gap:0'><div id=c2 style='height:10px'></div></div>"
                   "<div id=m3 style='width:500px;column-width:100px;column-count:2;column-gap:0'><div id=c3 style='height:10px'></div></div>"
                   "<div id=m4 style='width:300px;column-count:3'><div id=c4 style='height:10px'></div></div>"
                   "<div id=m5 style='width:100px;column-width:200px;column-gap:0'><div id=c5 style='height:10px'></div></div>"
                   "</body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(first_rect(&h, "c1", &r));
    EMB_CHECK(near(r.w, 100));
    EMB_REQUIRE(first_rect(&h, "c2", &r));
    EMB_CHECK(near(r.w, 350.0 / 3)); /* floor(350 / 100) = 3 columns, the extra space is shared */
    EMB_REQUIRE(first_rect(&h, "c3", &r));
    EMB_CHECK(near(r.w, 250)); /* column-count caps the number of columns */
    EMB_REQUIRE(first_rect(&h, "c4", &r));
    EMB_CHECK(near(r.w, (300 - 2 * 16) / 3.0)); /* column-gap: normal is 1em */
    EMB_REQUIRE(first_rect(&h, "c5", &r));
    EMB_CHECK(near(r.w, 100)); /* one column, narrower than column-width */
    lh_free(&h);
}

EMB_TEST(blocks_balance_over_the_columns)
{
    lh h = lh_make("<body><div id=m style='width:300px;column-count:3;column-gap:0'>"
                   "<div id=b1 style='height:20px'></div><div id=b2 style='height:20px'></div>"
                   "<div id=b3 style='height:20px'></div><div id=b4 style='height:20px'></div>"
                   "<div id=b5 style='height:20px'></div><div id=b6 style='height:20px'></div></div></body>",
                   "");
    embi_rectf m, r;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 40));
    const double ex[6][2] = {{0, 0}, {0, 20}, {100, 0}, {100, 20}, {200, 0}, {200, 20}};
    for (int i = 0; i < 6; i++) {
        char id[4] = {'b', (char)('1' + i), 0, 0};
        EMB_REQUIRE(rect1(&h, id, &r));
        EMB_CHECK(near(r.x, ex[i][0]) && near(r.y, ex[i][1]));
    }
    lh_free(&h);
}

EMB_TEST(four_blocks_in_three_columns_use_two)
{
    lh h = lh_make("<body><div id=m style='width:300px;column-count:3;column-gap:0'>"
                   "<div id=b1 style='height:20px'></div><div id=b2 style='height:20px'></div>"
                   "<div id=b3 style='height:20px'></div><div id=b4 style='height:20px'></div></div></body>",
                   "");
    embi_rectf m, r;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 40));
    EMB_REQUIRE(rect1(&h, "b3", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    lh_free(&h);
}

EMB_TEST(text_lines_flow_into_three_balanced_columns)
{
    lh h = lh_make("<body><div id=m style='width:300px;column-count:3;column-gap:0;font:16px/20px sans-serif'>"
                   "<p id=p style='margin:0'>a<br>a<br>a<br>a<br>a<br>a<br>a<br>a<br>a<br>a<br>a<br>a</p></div></body>",
                   "");
    embi_rectf m, rs[4];
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 80)); /* 12 lines of 20px, 4 in each column */
    EMB_CHECK_EQ_INT((int)rects(&h, "p", rs, 4), 3);
    for (int i = 0; i < 3; i++) {
        EMB_CHECK(near(rs[i].x, 100.0 * i) && near(rs[i].y, 0) && near(rs[i].h, 80) && near(rs[i].w, 100));
    }
    lh_free(&h);
}

EMB_TEST(a_block_is_cut_with_truncated_margins_and_sliced_borders)
{
    lh h = lh_make("<body><div id=m style='width:200px;column-count:2;column-gap:0;column-fill:auto;height:130px'>"
                   "<div id=box style='border:2px solid;padding:5px'>"
                   "<div id=k1 style='height:40px;margin-top:20px'></div>"
                   "<div id=k2 style='height:40px;margin-top:20px'></div>"
                   "<div id=k3 style='height:40px;margin-top:20px'></div></div></div></body>",
                   "");
    embi_rectf rs[3], r;
    EMB_CHECK_EQ_INT((int)rects(&h, "box", rs, 3), 2);
    /* the first piece ends at its last child: the top border stays, the bottom border and padding go to the last piece */
    EMB_CHECK(near(rs[0].x, 0) && near(rs[0].y, 0) && near(rs[0].w, 100) && near(rs[0].h, 127));
    EMB_CHECK(near(rs[1].x, 100) && near(rs[1].y, 0) && near(rs[1].w, 100) && near(rs[1].h, 47));
    EMB_REQUIRE(rect1(&h, "k2", &r));
    EMB_CHECK(near(r.x, 7) && near(r.y, 87));
    /* the 20px margin above k3 is truncated at the break: it starts the second column */
    EMB_REQUIRE(rect1(&h, "k3", &r));
    EMB_CHECK(near(r.x, 107) && near(r.y, 0) && near(r.w, 86));
    lh_free(&h);
}

EMB_TEST(break_before_column_forces_a_new_column)
{
    lh h = lh_make("<body><div id=m style='width:200px;column-count:2;column-gap:0'>"
                   "<div id=b1 style='height:20px'></div><div id=b2 style='height:20px;break-before:column'></div>"
                   "<div id=b3 style='height:20px'></div><div id=b4 style='height:20px'></div></div></body>",
                   "");
    embi_rectf m, r;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 60)); /* b1 alone, then b2 b3 b4: without the forced break the columns would be 40 tall */
    EMB_REQUIRE(rect1(&h, "b2", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    EMB_REQUIRE(rect1(&h, "b4", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 40));
    lh_free(&h);
}

EMB_TEST(break_inside_avoid_moves_a_box_to_the_next_column)
{
    const char *page = "<body><div id=m style='width:200px;column-count:2;column-gap:0;column-fill:auto;height:100px'>"
                       "<div style='height:70px'></div>"
                       "<div style='%s'><div id=v1 style='height:30px'></div><div id=v2 style='height:30px'></div></div>"
                       "</div></body>";
    char html[512];
    SDL_snprintf(html, sizeof html, page, "break-inside:avoid");
    lh h = lh_make(html, "");
    embi_rectf r;
    EMB_REQUIRE(rect1(&h, "v1", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    EMB_REQUIRE(rect1(&h, "v2", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 30));
    lh_free(&h);
    SDL_snprintf(html, sizeof html, page, "");
    h = lh_make(html, "");
    EMB_REQUIRE(rect1(&h, "v1", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 70)); /* without avoid it is cut between its children */
    EMB_REQUIRE(rect1(&h, "v2", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    lh_free(&h);
}

EMB_TEST(column_span_all_makes_two_column_sets)
{
    lh h = lh_make("<body><div id=m style='width:200px;column-count:2;column-gap:0'>"
                   "<div id=a style='height:20px;overflow:hidden'></div>"
                   "<div id=s style='height:10px;column-span:all'></div>"
                   "<div id=b style='height:20px;overflow:hidden'></div><div id=c style='height:20px;overflow:hidden'></div></div></body>",
                   "");
    embi_rectf m, r;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 50));
    EMB_REQUIRE(rect1(&h, "a", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 0) && near(r.w, 100));
    EMB_REQUIRE(rect1(&h, "s", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 20) && near(r.w, 200));
    EMB_REQUIRE(rect1(&h, "b", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 30));
    EMB_REQUIRE(rect1(&h, "c", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 30));
    lh_free(&h);
}

EMB_TEST(column_fill_auto_with_a_height_overflows_in_the_inline_direction)
{
    lh h = lh_make("<body><div id=m style='width:200px;column-count:2;column-gap:0;column-fill:auto;height:50px'>"
                   "<div id=d1 style='height:20px'></div><div id=d2 style='height:20px'></div>"
                   "<div id=d3 style='height:20px'></div><div id=d4 style='height:20px'></div>"
                   "<div id=d5 style='height:20px'></div></div></body>",
                   "");
    embi_rectf m, r;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.h, 50));
    EMB_REQUIRE(rect1(&h, "d2", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 20));
    EMB_REQUIRE(rect1(&h, "d3", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    EMB_REQUIRE(rect1(&h, "d5", &r));
    EMB_CHECK(near(r.x, 200) && near(r.y, 0)); /* a third column, outside the container */
    lh_free(&h);
}

EMB_TEST(right_to_left_containers_start_at_the_right)
{
    lh h = lh_make("<body><div id=m style='width:200px;column-count:2;column-gap:0;direction:rtl'>"
                   "<div id=d1 style='height:20px'></div><div id=d2 style='height:20px'></div></div></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect1(&h, "d1", &r));
    EMB_CHECK(near(r.x, 100) && near(r.y, 0));
    EMB_REQUIRE(rect1(&h, "d2", &r));
    EMB_CHECK(near(r.x, 0) && near(r.y, 0));
    lh_free(&h);
}

EMB_TEST(a_multicol_container_shrinks_to_fit_its_columns)
{
    lh h = lh_make("<body><div id=m style='float:left;column-count:2;column-gap:10px'>"
                   "<div id=a style='width:60px;height:10px'></div></div></body>",
                   "");
    embi_rectf m;
    EMB_REQUIRE(rect1(&h, "m", &m));
    EMB_CHECK(near(m.w, 2 * 60 + 10)); /* max-content: the columns side by side with the gap */
    lh_free(&h);
}

EMB_TEST(hit_testing_finds_the_fragment_in_each_column)
{
    lh h = lh_make("<body><div id=m style='width:300px;column-count:3;column-gap:0'>"
                   "<div id=box style='height:90px;background:#ccc'></div></div></body>",
                   "");
    /* a 90px box in three columns of 30px: pieces at x 0, 100 and 200 */
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 10) == lh_id(&h, "box"));
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 110, 10) == lh_id(&h, "box"));
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 210, 25) == lh_id(&h, "box"));
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 50) != lh_id(&h, "box")); /* below the first piece */
    lh_free(&h);
}

EMB_TEST(a_tall_empty_box_is_cut_at_the_end_of_each_column)
{
    lh h = lh_make("<body><div style='width:100px;columns:4;gap:0;column-fill:auto;height:100px'>"
                   "<div style='width:345px'><div id=t style='height:400px'></div></div></div></body>",
                   "");
    embi_rectf rs[5];
    EMB_CHECK_EQ_INT((int)rects(&h, "t", rs, 5), 4);
    for (int i = 0; i < 4; i++) {
        EMB_CHECK(near(rs[i].x, 25.0 * i) && near(rs[i].y, 0) && near(rs[i].w, 345) && near(rs[i].h, 100));
    }
    lh_free(&h);
}

EMB_TEST_MAIN()
