/*
 * Margin collapsing (CSS 2.1 section 8.3.1, W3.md section 3) and the automatic minimum height of
 * ratio boxes. Every expectation is derived from the CSS 2.1 collapsing rules; the three probes of
 * the W3a review (a height:0 box between siblings, a padded parent with an empty child, a parent with
 * padding-top only) are Chromium results. The page has margin 0 on body, so a y is a document y.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static float ytop(lh *h, const char *id)
{
    embi_rectf r = {0, 0, 0, 0};
    embi_element *el = lh_id(h, id);
    if (el == nullptr || !embi_layout_border_box(h->eng, el, &r)) {
        return NAN;
    }
    return r.y;
}

static float hgt(lh *h, const char *id)
{
    embi_rectf r = {0, 0, 0, 0};
    embi_element *el = lh_id(h, id);
    if (el == nullptr || !embi_layout_border_box(h->eng, el, &r)) {
        return NAN;
    }
    return r.h;
}

#define PAGE(body) lh_make("<body>" body "</body>", "body { margin: 0 }")

/* ------------------------------------------------------------------ negative margins */

EMB_TEST(margin_positive_and_negative_siblings_add_the_extremes)
{
    /* 30px and -10px: the largest positive plus the most negative, 20px. */
    lh h = PAGE("<div style='height:10px;margin-bottom:30px'></div>"
                "<div id=b style='height:10px;margin-top:-10px'></div>");
    EMB_CHECK(near(ytop(&h, "b"), 30));
    lh_free(&h);
}

EMB_TEST(margin_two_negative_siblings_take_the_most_negative)
{
    lh h = PAGE("<div style='height:10px;margin-bottom:-10px'></div>"
                "<div id=b style='height:10px;margin-top:-20px'></div>");
    EMB_CHECK(near(ytop(&h, "b"), -10));
    lh_free(&h);
}

EMB_TEST(margin_negative_first_child_margin_collapses_with_the_parent)
{
    /* Parent 20px, child -5px: 15px, and the child sits at the top of the parent. */
    lh h = PAGE("<div id=p style='margin-top:20px'><div id=c style='margin-top:-5px;height:10px'></div></div>");
    EMB_CHECK(near(ytop(&h, "p"), 15));
    EMB_CHECK(near(ytop(&h, "c"), 15));
    lh_free(&h);
}

/* ------------------------------------------------------------------ parent and first child */

EMB_TEST(margin_parent_first_child_blocked_by_border_or_padding)
{
    lh a = PAGE("<div id=p style='border-top:1px solid'><div id=c style='margin-top:20px;height:10px'></div></div>");
    EMB_CHECK(near(ytop(&a, "p"), 0));
    EMB_CHECK(near(ytop(&a, "c"), 21));
    lh_free(&a);
    lh b = PAGE("<div id=p style='padding-top:1px'><div id=c style='margin-top:20px;height:10px'></div></div>");
    EMB_CHECK(near(ytop(&b, "p"), 0));
    EMB_CHECK(near(ytop(&b, "c"), 21));
    lh_free(&b);
    /* Without them the margin moves the parent. */
    lh c = PAGE("<div id=p><div id=c style='margin-top:20px;height:10px'></div></div>");
    EMB_CHECK(near(ytop(&c, "p"), 20));
    EMB_CHECK(near(ytop(&c, "c"), 20));
    lh_free(&c);
}

EMB_TEST(margin_parent_first_child_blocked_by_a_bfc_root)
{
    static const char *styles[] = {"overflow:hidden", "display:flow-root", "display:inline-block",
                                   "overflow:auto"};
    for (size_t i = 0; i < sizeof styles / sizeof styles[0]; i++) {
        char html[256];
        SDL_snprintf(html, sizeof html,
                     "<body><div id=p style='%s'><div id=c style='margin-top:20px;height:10px'></div></div></body>",
                     styles[i]);
        lh h = lh_make(html, "body { margin: 0 }");
        EMB_CHECK(near(ytop(&h, "p"), 0));
        EMB_CHECK(near(ytop(&h, "c"), 20));
        EMB_CHECK(near(hgt(&h, "p"), 30));
        lh_free(&h);
    }
}

EMB_TEST(margin_nested_chain_of_first_children_collapses_to_the_largest)
{
    lh h = PAGE("<div id=a style='margin-top:10px'><div id=b style='margin-top:30px'>"
                "<div id=c style='margin-top:20px;height:10px'></div></div></div>");
    EMB_CHECK(near(ytop(&h, "a"), 30));
    EMB_CHECK(near(ytop(&h, "b"), 30));
    EMB_CHECK(near(ytop(&h, "c"), 30));
    lh_free(&h);
}

/* ------------------------------------------------------------------ parent and last child */

EMB_TEST(margin_parent_last_child_bottom_collapses_for_auto_height)
{
    lh h = PAGE("<div id=p><div style='height:10px;margin-bottom:20px'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(hgt(&h, "p"), 10));
    EMB_CHECK(near(ytop(&h, "n"), 30));
    lh_free(&h);
}

EMB_TEST(margin_parent_last_child_bottom_blocked_by_height_padding_border)
{
    /* The child's 20px stays inside the parent, in its content height, when the parent's bottom is
     * not open: a min-height, a height, padding or a border. */
    static const char *styles[] = {"min-height:15px", "padding-bottom:1px", "border-bottom:1px solid"};
    static const float ph[] = {30, 31, 31};
    for (size_t i = 0; i < 3; i++) {
        char html[320];
        SDL_snprintf(html, sizeof html,
                     "<body><div id=p style='%s'><div style='height:10px;margin-bottom:20px'></div></div>"
                     "<div id=n style='height:10px'></div></body>",
                     styles[i]);
        lh h = lh_make(html, "body { margin: 0 }");
        EMB_CHECK(near(hgt(&h, "p"), ph[i]));
        EMB_CHECK(near(ytop(&h, "n"), ph[i]));
        lh_free(&h);
    }
    /* height: 30px keeps the box at 30 and the margin collapses nowhere. */
    lh h = PAGE("<div id=p style='height:30px'><div style='height:10px;margin-bottom:20px'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(hgt(&h, "p"), 30));
    EMB_CHECK(near(ytop(&h, "n"), 30));
    lh_free(&h);
}

EMB_TEST(margin_parent_last_child_blocked_by_a_bfc_root)
{
    lh h = PAGE("<div id=p style='overflow:hidden'><div style='height:10px;margin-bottom:20px'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(hgt(&h, "p"), 30));
    EMB_CHECK(near(ytop(&h, "n"), 30));
    lh_free(&h);
    lh f = PAGE("<div id=p style='display:flow-root'><div style='height:10px;margin-bottom:20px'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(hgt(&f, "p"), 30));
    lh_free(&f);
}

/* ------------------------------------------------------------------ empty blocks */

EMB_TEST(margin_height_zero_box_between_siblings_collapses_through)
{
    /* Chromium: the height:0 box is at y=30 and the last box at y=40. */
    lh h = PAGE("<div style='height:10px'></div>"
                "<div id=z style='height:0;margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&h, "z"), 30));
    EMB_CHECK(near(ytop(&h, "l"), 40));
    lh_free(&h);
}

EMB_TEST(margin_border_box_height_zero_collapses_through_too)
{
    lh h = PAGE("<div style='height:10px'></div>"
                "<div style='height:0;box-sizing:border-box;margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&h, "l"), 40));
    lh_free(&h);
}

EMB_TEST(margin_empty_auto_block_between_siblings_collapses_through)
{
    /* 10, then an empty block with 20px margins, then 30: all of them collapse to 30. */
    lh h = PAGE("<div style='height:10px;margin-bottom:10px'></div>"
                "<div style='margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&h, "l"), 40));
    lh_free(&h);
    /* The empty block's own margins can be the largest: they separate the neighbours. */
    lh g = PAGE("<div style='height:10px;margin-bottom:10px'></div>"
                "<div style='margin:50px 0 5px'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&g, "l"), 60));
    lh_free(&g);
}

EMB_TEST(margin_empty_block_with_padding_border_or_min_height_does_not_collapse_through)
{
    /* 1px of padding: top 20 (nothing before), 2px tall, bottom 20 collapses with 30 -> 30. */
    lh a = PAGE("<div style='height:10px'></div>"
                "<div id=m style='padding:1px 0;margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&a, "m"), 30));
    EMB_CHECK(near(hgt(&a, "m"), 2));
    EMB_CHECK(near(ytop(&a, "l"), 62));
    lh_free(&a);
    lh b = PAGE("<div style='height:10px'></div>"
                "<div id=m style='border-top:1px solid;margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&b, "m"), 30));
    EMB_CHECK(near(ytop(&b, "l"), 10 + 20 + 1 + 30));
    lh_free(&b);
    lh c = PAGE("<div style='height:10px'></div>"
                "<div id=m style='min-height:5px;margin:20px 0'></div>"
                "<div id=l style='height:10px;margin-top:30px'></div>");
    EMB_CHECK(near(ytop(&c, "m"), 30));
    EMB_CHECK(near(ytop(&c, "l"), 10 + 20 + 5 + 30));
    lh_free(&c);
}

EMB_TEST(margin_self_collapsing_sibling_between_real_blocks)
{
    lh h = PAGE("<div style='height:10px;margin-bottom:15px'></div>"
                "<div id=e style='margin:25px 0'></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&h, "l"), 35));
    EMB_CHECK(near(ytop(&h, "e"), 35));
    lh_free(&h);
}

/* ------------------------------------------------------------------ the review probes */

EMB_TEST(margin_padded_parent_keeps_the_margin_of_an_empty_child)
{
    /* Chromium: the parent is 22px tall and the child sits at y=21. */
    lh h = PAGE("<div id=p style='padding:1px 0'><div id=c style='margin:20px 0'></div></div>");
    EMB_CHECK(near(hgt(&h, "p"), 22));
    EMB_CHECK(near(ytop(&h, "c"), 21));
    lh_free(&h);
}

EMB_TEST(margin_parent_with_padding_top_only_passes_the_empty_childs_margin_to_its_bottom)
{
    lh h = PAGE("<div id=p style='padding-top:1px'><div id=c style='margin:20px 0'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(hgt(&h, "p"), 1));
    EMB_CHECK(near(ytop(&h, "c"), 21));
    EMB_CHECK(near(ytop(&h, "n"), 21));
    lh_free(&h);
}

EMB_TEST(margin_parent_with_padding_bottom_only_passes_the_empty_childs_margin_to_its_top)
{
    lh h = PAGE("<div id=p style='padding-bottom:1px'><div id=c style='margin:20px 0'></div></div>"
                "<div id=n style='height:10px'></div>");
    EMB_CHECK(near(ytop(&h, "p"), 20));
    EMB_CHECK(near(hgt(&h, "p"), 1));
    EMB_CHECK(near(ytop(&h, "c"), 20));
    EMB_CHECK(near(ytop(&h, "n"), 21));
    lh_free(&h);
}

EMB_TEST(margin_empty_children_with_padding_on_both_sides_sum_inside)
{
    /* Two empty children inside a padded parent: 20 and 30 collapse to 30 inside. */
    lh h = PAGE("<div id=p style='padding:1px 0'><div style='margin:20px 0'></div>"
                "<div id=c style='margin:30px 0'></div></div>");
    EMB_CHECK(near(hgt(&h, "p"), 32));
    EMB_CHECK(near(ytop(&h, "c"), 31));
    lh_free(&h);
}

/* ------------------------------------------------------------------ self-collapsing placement */

EMB_TEST(margin_collapsed_through_box_sits_at_its_top_margin_only)
{
    /* CSS 2.1 section 8.3.1: only the top margin collapses with the margin before the box, so the
     * empty box (margin 5 0 50) sits 5px below the 10px box; its bottom margin of 50 then separates
     * the next one. Chromium: z at 15, l at 60. */
    lh h = PAGE("<div style='height:10px'></div>"
                "<div id=z style='margin:5px 0 50px'></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&h, "z"), 15));
    EMB_CHECK(near(ytop(&h, "l"), 60));
    lh_free(&h);
    /* A larger margin before it wins over the top margin: 30 then 5 is 30. Chromium: z at 40. */
    lh a = PAGE("<div style='height:10px;margin-bottom:30px'></div>"
                "<div id=z style='margin:5px 0 50px'></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&a, "z"), 40));
    EMB_CHECK(near(ytop(&a, "l"), 60));
    lh_free(&a);
    /* The top margin larger than the bottom one. */
    lh b = PAGE("<div style='height:10px'></div>"
                "<div id=z style='margin:50px 0 5px'></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&b, "z"), 60));
    EMB_CHECK(near(ytop(&b, "l"), 60));
    lh_free(&b);
}

EMB_TEST(margin_collapsed_through_box_at_the_top_of_a_padded_parent)
{
    /* With padding above, nothing collapses with the parent: z sits 5px below the padding edge. */
    lh h = PAGE("<div style='padding-top:1px'><div id=z style='margin:5px 0 50px'></div></div>");
    EMB_CHECK(near(ytop(&h, "z"), 6));
    lh_free(&h);
}

EMB_TEST(margin_empty_box_nested_in_an_empty_parent_takes_the_top_margin_for_its_position)
{
    /* p is empty and so is z: p's margins are z's, and p sits at the top margin (5), z with it. The 50
     * below them separates the next block (inside a padded box, so nothing collapses out of it). */
    lh h = PAGE("<div style='padding-top:1px'><div id=p><div id=z style='margin:5px 0 50px'></div></div>"
                "<div id=l style='height:10px'></div></div>");
    EMB_CHECK(near(ytop(&h, "p"), 6));
    EMB_CHECK(near(ytop(&h, "z"), 6));
    EMB_CHECK(near(ytop(&h, "l"), 51));
    lh_free(&h);
    /* Alone in the document the margins collapse through the body and the root's first child sits at 5. */
    lh a = PAGE("<div id=p><div id=z style='margin:5px 0 50px'></div></div>");
    EMB_CHECK(near(ytop(&a, "p"), 5));
    EMB_CHECK(near(ytop(&a, "z"), 5));
    lh_free(&a);
    /* The same after a block with a margin of its own. */
    lh g = PAGE("<div style='height:10px;margin-bottom:20px'></div>"
                "<div id=p style='margin-top:8px'><div id=z style='margin:5px 0 50px'></div></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&g, "p"), 30));
    EMB_CHECK(near(ytop(&g, "z"), 30));
    EMB_CHECK(near(ytop(&g, "l"), 60));
    lh_free(&g);
}

/* ------------------------------------------------------------------ containment */

EMB_TEST(margin_layout_containment_and_containers_are_bfc_roots)
{
    /* contain: layout, paint, content, strict and container-type: size or inline-size stop the
     * first child's margin from collapsing through: c stays at 35 under a 35px block. */
    static const char *styles[] = {"contain:layout", "contain:paint", "contain:content",
                                   "contain:strict", "contain:size layout",
                                   "container-type:inline-size", "container-type:size"};
    for (size_t i = 0; i < sizeof styles / sizeof styles[0]; i++) {
        char html[400];
        SDL_snprintf(html, sizeof html,
                     "<body><div style='height:35px'></div><div id=c style='%s'>"
                     "<div id=k style='margin-top:20px;height:5px'></div></div></body>",
                     styles[i]);
        lh h = lh_make(html, "body { margin: 0 }");
        EMB_CHECK(near(ytop(&h, "c"), 35));
        EMB_CHECK(near(ytop(&h, "k"), 55));
        lh_free(&h);
    }
    /* contain: style does not make a formatting context root. */
    lh n = PAGE("<div style='height:35px'></div><div id=c style='contain:style'>"
                "<div id=k style='margin-top:20px;height:5px'></div></div>");
    EMB_CHECK(near(ytop(&n, "c"), 55));
    lh_free(&n);
}

EMB_TEST(contain_paint_clips)
{
    lh h = PAGE("<div id=c style='contain:paint;height:10px'><div style='height:50px'></div></div>");
    const embi_element *c = lh_id(&h, "c");
    EMB_REQUIRE(c != nullptr && c->box != nullptr);
    EMB_CHECK((((const embi_box *)c->box)->flags & EMBI_BF_CLIP) != 0);
    lh_free(&h);
}

/* ------------------------------------------------------------------ aspect-ratio minimum height */

/* ------------------------------------------------------------------ mixed-sign struts */

EMB_TEST(margin_mixed_sign_first_child_joins_the_strut_as_a_set)
{
    /* 15 below the first block, then a parent with margin-top 20 whose first child has -10: the set is
     * {15, 20, -10}: the largest positive plus the most negative is 10, so the parent
     * and its child sit at 10 + 10 = 20 (the signed sum 15 + 10 would give 25). */
    lh h = PAGE("<div style='height:10px;margin-bottom:15px'></div>"
                "<div id=p style='margin-top:20px'><div id=c style='margin-top:-10px;height:10px'></div></div>");
    EMB_CHECK(near(ytop(&h, "p"), 20));
    EMB_CHECK(near(ytop(&h, "c"), 20));
    lh_free(&h);
}

EMB_TEST(margin_mixed_sign_empty_block_between_siblings)
{
    /* margin 20 0 -10 on an empty block: the set {15, 20, -10} collapses with both neighbours. */
    lh h = PAGE("<div style='height:10px;margin-bottom:15px'></div>"
                "<div id=z style='margin:20px 0 -10px'></div>"
                "<div id=l style='height:10px'></div>");
    EMB_CHECK(near(ytop(&h, "z"), 30)); /* only its top margin: 10 + 20 */
    EMB_CHECK(near(ytop(&h, "l"), 20));
    lh_free(&h);
}

EMB_TEST(margin_mixed_sign_last_child_collapses_with_the_parent_bottom)
{
    /* The parent has +20 at the bottom and its last child -10: 20 + (-10) = 10 below the parent. */
    lh h = PAGE("<div id=p style='margin-bottom:20px'><div style='height:10px;margin-bottom:-10px'></div></div>"
                "<div id=n style='height:5px'></div>");
    EMB_CHECK(near(hgt(&h, "p"), 10));
    EMB_CHECK(near(ytop(&h, "n"), 20));
    lh_free(&h);
}

EMB_TEST(margin_mixed_sign_siblings_through_a_parent_boundary)
{
    /* The previous block's +15 meets the +20/-10 set of a nested first child chain. */
    lh h = PAGE("<div style='height:10px;margin-bottom:15px'></div>"
                "<div id=a style='margin-top:5px'><div id=b style='margin-top:-30px'>"
                "<div id=c style='margin-top:12px;height:4px'></div></div></div>");
    /* set {15, 5, -30, 12}: 15 + (-30) = -15, so c sits at 10 - 15 = -5 */
    EMB_CHECK(near(ytop(&h, "a"), -5));
    EMB_CHECK(near(ytop(&h, "c"), -5));
    lh_free(&h);
}

EMB_TEST(ratio_box_grows_to_its_content_with_min_height_auto)
{
    /* CSS Sizing 4 section 5.1: the content-based minimum. Chromium: 300. */
    lh h = PAGE("<div id=a style='width:100px;aspect-ratio:1/1'><div style='height:300px'></div></div>");
    EMB_CHECK(near(hgt(&h, "a"), 300));
    lh_free(&h);
    lh s = PAGE("<div id=a style='width:100px;aspect-ratio:1/1'><div style='height:30px'></div></div>");
    EMB_CHECK(near(hgt(&s, "a"), 100));
    lh_free(&s);
}

EMB_TEST(ratio_box_keeps_the_ratio_height_for_scroll_containers_and_explicit_min_height)
{
    lh h = PAGE("<div id=a style='width:100px;aspect-ratio:1/1;overflow:hidden'><div style='height:300px'></div></div>"
                "<div id=b style='width:100px;aspect-ratio:1/1;overflow:auto'><div style='height:300px'></div></div>"
                "<div id=c style='width:100px;aspect-ratio:1/1;min-height:0'><div style='height:300px'></div></div>");
    EMB_CHECK(near(hgt(&h, "a"), 100));
    EMB_CHECK(near(hgt(&h, "b"), 100));
    EMB_CHECK(near(hgt(&h, "c"), 100));
    lh_free(&h);
}

/* ------------------------------------------------------------------ scroll container details */

EMB_TEST(scrollbar_gutter_stable_applies_to_scroll_containers_not_to_overflow_clip)
{
    lh h = PAGE("<div id=a style='width:100px;height:50px;overflow:hidden;scrollbar-gutter:stable'></div>"
                "<div id=b style='width:100px;height:50px;overflow:clip;scrollbar-gutter:stable'></div>");
    lh_set_scrollbar(&h, 15);
    float w = 0, hh = 0;
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "a"), &w, &hh));
    EMB_CHECK(near(w, 85));
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "b"), &w, &hh));
    EMB_CHECK(near(w, 100));
    lh_free(&h);
}

EMB_TEST(nested_overflow_auto_boxes_lay_out_without_exponential_work)
{
    /* 24 nested overflow:auto boxes that each overflow vertically: re-laying out every level twice
     * from scratch would be 2^24 layouts. */
    char html[8192];
    size_t n = 0;
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<body>");
    for (int i = 0; i < 24; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n,
                                  "<div id=n%d style='overflow:auto;height:%dpx;width:%dpx'>", i,
                                  1000 - 30 * i,
                                  400 - 8 * i);
    }
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<div style='height:5000px'></div>");
    for (int i = 0; i < 24; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "</div>");
    }
    SDL_snprintf(html + n, sizeof html - n, "</body>");
    lh h = lh_make_cfg(html, strlen(html), "body { margin: 0 }", &(embi_layout_config){nullptr, nullptr, 15});
    float w = 0, hh = 0;
    EMB_CHECK(embi_layout_client_size(h.eng, embi_document_element(h.doc), &w, &hh));
    EMB_CHECK(near(w, 800));
    /* Only the innermost box overflows (5000px in a 310px box): it alone has the 15px gutter. A scroll
     * container's border box is all that its parent sees, so no other level has one. */
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "n0"), &w, &hh));
    EMB_CHECK(near(w, 400));
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "n5"), &w, &hh));
    EMB_CHECK(near(w, 360));
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "n22"), &w, &hh));
    EMB_CHECK(near(w, 224));
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "n23"), &w, &hh));
    EMB_CHECK(near(w, 216 - 15));
    EMB_CHECK(near(hh, 310));
    lh_free(&h);
}

EMB_TEST_MAIN()
