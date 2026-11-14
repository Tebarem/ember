/*
 * W3a review fixes: intrinsic contributions clamped by min-width and max-width, input defaults,
 * recursion limits, geometry of boxes without a fragment, and offsetWidth snapping.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

#define PAGE(body) lh_make("<body>" body "</body>", "body { margin: 0 }")

EMB_TEST(shrink_to_fit_container_honours_min_width_of_a_child)
{
    lh h = PAGE("<div id=s style='display:inline-block'><div style='min-width:200px'></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "s", &r));
    EMB_CHECK(near(r.w, 200));
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_container_honours_max_width_of_a_child)
{
    lh h = PAGE("<div id=s style='display:inline-block'><div style='width:300px;max-width:100px'></div></div>"
                "<div id=t style='display:inline-block'>"
                "<div style='max-width:100px'><span style='display:inline-block;width:400px'></span></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "s", &r));
    EMB_CHECK(near(r.w, 100));
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(near(r.w, 100));
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_contribution_limits_with_border_box_padding_and_margins)
{
    /* min-width: 200px border-box with 10px padding is a 200px border box, plus 5px margins. */
    lh h = PAGE("<div id=s style='display:inline-block'>"
                "<div style='box-sizing:border-box;padding:10px;min-width:200px;margin:0 5px'></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "s", &r));
    EMB_CHECK(near(r.w, 210));
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_contribution_treats_percentage_limits_as_auto)
{
    lh h = PAGE("<div id=s style='display:inline-block'><div style='min-width:50%'></div></div>"
                "<div id=t style='display:inline-block'><div style='width:80px;max-width:50%'></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "s", &r));
    EMB_CHECK(near(r.w, 0));
    EMB_REQUIRE(rect_of(&h, "t", &r));
    EMB_CHECK(near(r.w, 80));
    lh_free(&h);
}

EMB_TEST(shrink_to_fit_min_width_wins_over_max_width_in_the_contribution)
{
    lh h = PAGE("<div id=s style='display:inline-block'><div style='min-width:200px;max-width:100px'></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "s", &r));
    EMB_CHECK(near(r.w, 200));
    lh_free(&h);
}

EMB_TEST(canvas_with_an_invalid_size_attribute_uses_the_default)
{
    lh h = PAGE("<canvas id=a width=-5></canvas><canvas id=b height=-1 width=20></canvas>"
                "<canvas id=c width=x height=40></canvas>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(near(r.w, 300) && near(r.h, 150));
    EMB_REQUIRE(rect_of(&h, "b", &r));
    EMB_CHECK(near(r.w, 20) && near(r.h, 150));
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.w, 300) && near(r.h, 40));
    lh_free(&h);
}

EMB_TEST(a_very_deep_dom_lays_out_without_overflowing_the_stack)
{
    /* 6000 nested divs, each with a text sibling (anonymous wrappers at every level). The boxes
     * below the depth cap are dropped. */
    enum { DEPTH = 6000 };
    size_t cap = (size_t)DEPTH * 24 + 64;
    char *html = SDL_malloc(cap);
    EMB_REQUIRE(html != nullptr);
    size_t n = 0;
    n += (size_t)SDL_snprintf(html + n, cap - n, "<body>");
    for (int i = 0; i < DEPTH; i++) {
        n += (size_t)SDL_snprintf(html + n, cap - n, "<div>t");
    }
    n += (size_t)SDL_snprintf(html + n, cap - n, "<div id=deep></div>");
    lh h = lh_make_n(html, n, "body { margin: 0 }");
    SDL_free(html);
    embi_rectf r;
    EMB_CHECK(!rect_of(&h, "deep", &r)); /* past the cap: no box */
    EMB_CHECK(embi_layout_root(h.eng, h.doc) != nullptr);
    lh_free(&h);
}

EMB_TEST(a_moderately_deep_dom_keeps_all_its_boxes)
{
    /* 60 levels is below the cap in every build, even under a sanitizer. */
    char html[2048];
    size_t n = 0;
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<body>");
    for (int i = 0; i < 60; i++) {
        n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<div>t");
    }
    n += (size_t)SDL_snprintf(html + n, sizeof html - n, "<div id=last style='height:7px'></div>");
    lh h = lh_make_n(html, n, "body { margin: 0 }");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "last", &r));
    EMB_CHECK(near(r.h, 7));
    lh_free(&h);
}

EMB_TEST(an_empty_inline_has_no_geometry)
{
    /* An inline with no content makes no line, so it has no fragment (it used to answer from an
     * orphan fragment). */
    lh h = PAGE("<div id=d><span id=s></span></div>");
    embi_rectf r;
    EMB_CHECK(!rect_of(&h, "s", &r));
    const embi_element *parent = nullptr;
    float x, y, w, hh;
    EMB_CHECK(!embi_layout_offset(h.eng, lh_id(&h, "s"), &parent, &x, &y, &w, &hh));
    lh_free(&h);
}

EMB_TEST(offset_size_snaps_against_the_rounded_edges)
{
    /* The box is at y = 10.4 and 10.4px tall: the edges 10.4 and 20.8 round to 10 and 21. */
    lh h = PAGE("<div style='height:10.4px'></div><div id=d style='height:10.4px;width:10.4px'></div>");
    const embi_element *parent = nullptr;
    float x, y, w, hh;
    EMB_REQUIRE(embi_layout_offset(h.eng, lh_id(&h, "d"), &parent, &x, &y, &w, &hh));
    EMB_CHECK(near(y, 10));
    EMB_CHECK(near(hh, 11));
    EMB_CHECK(near(w, 10));
    lh_free(&h);
}

EMB_TEST(cq_units_in_a_shrink_to_fit_box_resolve_against_the_container)
{
    /* The inline-block's max-content width is 50cqw of the 200px container, not of the viewport. The
     * intrinsic size is cached per container size. Chromium: 100. */
    lh h = PAGE("<div id=c style='width:200px;container-type:inline-size'>"
                "<span id=ib style='display:inline-block'><div style='width:50cqw;height:5px'></div></span>"
                "</div>"
                "<div id=d style='width:300px;container-type:inline-size'>"
                "<span id=jb style='display:inline-block'><div style='width:50cqw;height:5px'></div></span>"
                "</div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "ib", &r));
    EMB_CHECK(near(r.w, 100));
    EMB_REQUIRE(rect_of(&h, "jb", &r));
    EMB_CHECK(near(r.w, 150));
    lh_free(&h);
}

EMB_TEST(an_inline_size_container_has_no_content_based_width)
{
    /* Inline-size containment: the shrink-to-fit width ignores the content. */
    lh h = PAGE("<div id=c style='display:inline-block;container-type:inline-size'>"
                "<div style='width:300px;height:5px'></div></div>"
                "<div id=n style='display:inline-block'><div style='width:300px;height:5px'></div></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.w, 0));
    EMB_REQUIRE(rect_of(&h, "n", &r));
    EMB_CHECK(near(r.w, 300));
    lh_free(&h);
}

EMB_TEST(client_size_of_an_inline_box_is_zero)
{
    lh h = PAGE("<div><span id=s style='padding:3px'>x</span></div>");
    float w = 1, hh = 1;
    EMB_CHECK(embi_layout_client_size(h.eng, lh_id(&h, "s"), &w, &hh));
    EMB_CHECK(near(w, 0) && near(hh, 0));
    lh_free(&h);
}

EMB_TEST(client_and_scroll_size_of_the_root_are_the_viewport)
{
    lh h = lh_make("<!doctype html><body><div style='height:44px'></div></body>", "body { margin: 0 }");
    float w = 0, hh = 0;
    embi_element *root = embi_document_element(h.doc);
    EMB_CHECK(embi_layout_client_size(h.eng, root, &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 600));
    EMB_CHECK(embi_layout_scroll_size(h.eng, root, &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 600));
    lh_free(&h);
    /* Taller content: the viewport shows a vertical scrollbar, and the scroll height is the content. */
    lh t = lh_make("<!doctype html><body><div style='height:2000px'></div></body>", "body { margin: 0 }");
    lh_set_scrollbar(&t, 15);
    root = embi_document_element(t.doc);
    EMB_CHECK(embi_layout_client_size(t.eng, root, &w, &hh));
    EMB_CHECK(near(w, 785) && near(hh, 600));
    EMB_CHECK(embi_layout_scroll_size(t.eng, root, &w, &hh));
    EMB_CHECK(near(hh, 2000));
    lh_free(&t);
    /* In quirks mode (no doctype) the body stands for the viewport and the root is an ordinary box. */
    lh q = lh_make("<body id=bd><div style='height:44px'></div></body>", "body { margin: 0 }");
    EMB_CHECK(embi_layout_client_size(q.eng, embi_document_element(q.doc), &w, &hh));
    EMB_CHECK(near(hh, 44));
    EMB_CHECK(embi_layout_client_size(q.eng, lh_id(&q, "bd"), &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 600));
    lh_free(&q);
}

EMB_TEST(client_size_of_the_root_honours_overflow_of_the_viewport)
{
    float w = 0, hh = 0;
    /* overflow:hidden on the root: no scrollbar even though the content is taller. */
    lh a = lh_make("<!doctype html><html style='overflow:hidden'><body><div style='height:2000px'></div></body></html>",
                   "body { margin: 0 }");
    lh_set_scrollbar(&a, 15);
    EMB_CHECK(embi_layout_client_size(a.eng, embi_document_element(a.doc), &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 600));
    lh_free(&a);
    /* overflow:hidden on the body propagates to the viewport when the root is visible. */
    lh b = lh_make("<!doctype html><body style='overflow:hidden'><div style='height:2000px'></div></body>",
                   "body { margin: 0 }");
    lh_set_scrollbar(&b, 15);
    EMB_CHECK(embi_layout_client_size(b.eng, embi_document_element(b.doc), &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 600));
    lh_free(&b);
    /* overflow:scroll shows the bars whatever the content. */
    lh c = lh_make("<!doctype html><html style='overflow:scroll'><body><div style='height:4px'></div></body></html>",
                   "body { margin: 0 }");
    lh_set_scrollbar(&c, 15);
    EMB_CHECK(embi_layout_client_size(c.eng, embi_document_element(c.doc), &w, &hh));
    EMB_CHECK(near(w, 785) && near(hh, 585));
    lh_free(&c);
}

EMB_TEST(a_query_through_an_engine_without_a_layout_of_the_document_fails)
{
    lh h = PAGE("<div id=d style='height:10px'></div>");
    embi_layout_engine *other = embi_layout_engine_create(h.text, h.style, nullptr);
    float w = 1, hh = 1;
    embi_rectf r;
    const embi_element *parent = nullptr;
    float x, y;
    embi_element *d = lh_id(&h, "d");
    EMB_CHECK(!embi_layout_client_size(other, d, &w, &hh));
    EMB_CHECK(!embi_layout_scroll_size(other, d, &w, &hh));
    EMB_CHECK(!embi_layout_offset(other, d, &parent, &x, &y, &w, &hh));
    EMB_CHECK(!embi_layout_border_box(other, d, &r));
    embi_layout_engine_destroy(other);
    /* Forgetting the document clears its box slots. */
    embi_layout_forget_document(h.eng, h.doc);
    EMB_CHECK(d->box == nullptr);
    lh_free(&h);
}

EMB_TEST(a_replaced_block_wider_than_its_container_overflows_at_the_end_edge)
{
    /* CSS 2.1 section 10.3.4: the over-constrained left margin takes the difference when the CONTAINING
     * BLOCK is rtl, whatever the box's own direction. */
    lh h = lh_make("<body style='margin:0;direction:rtl'>"
                   "<canvas id=c style='display:block;width:3000px;height:10px'></canvas>"
                   "<canvas id=d style='display:block;width:3000px;height:10px;direction:ltr'></canvas></body>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.x, 800 - 3000));
    EMB_REQUIRE(rect_of(&h, "d", &r));
    EMB_CHECK(near(r.x, 800 - 3000));
    lh_free(&h);
    lh l = lh_make("<body style='margin:0'>"
                   "<canvas id=c style='display:block;width:3000px;height:10px;direction:rtl'></canvas>"
                   "<div id=e style='width:300px;direction:rtl'></div></body>",
                   "");
    EMB_REQUIRE(rect_of(&l, "c", &r));
    EMB_CHECK(near(r.x, 0));
    EMB_REQUIRE(rect_of(&l, "e", &r));
    EMB_CHECK(near(r.x, 0));
    lh_free(&l);
    /* Blocks too: a fixed-width block in an rtl container sits at the right. */
    lh b = lh_make("<body style='margin:0;direction:rtl'><div id=e style='width:300px'></div></body>", "");
    EMB_REQUIRE(rect_of(&b, "e", &r));
    EMB_CHECK(near(r.x, 500));
    lh_free(&b);
}

EMB_TEST(aspect_ratio_property_relates_border_boxes_under_border_box_sizing)
{
    /* A 100px wide border box with 30px side and 10px vertical padding: the ratio 1/1 makes the border
     * box 100px tall. The content-box ratio would give 60. */
    lh h = PAGE("<canvas id=c style='display:block;box-sizing:border-box;width:100px;"
                "padding:10px 30px;aspect-ratio:1/1'></canvas>"
                "<canvas id=d style='display:block;width:100px;padding:10px 30px;aspect-ratio:1/1'></canvas>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(near(r.w, 100) && near(r.h, 100));
    EMB_REQUIRE(rect_of(&h, "d", &r));
    EMB_CHECK(near(r.w, 160) && near(r.h, 120));
    lh_free(&h);
}

#if EMBER_SVG
EMB_TEST(non_finite_svg_lengths_are_ignored)
{
    lh h = PAGE("<svg id=a width=inf height=50></svg><svg id=b width=1e999 height=nan></svg>"
                "<svg id=c width=1e30 height=10></svg>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(isfinite(r.w) && isfinite(r.h));
    EMB_REQUIRE(rect_of(&h, "b", &r));
    EMB_CHECK(isfinite(r.w) && isfinite(r.h));
    EMB_REQUIRE(rect_of(&h, "c", &r));
    EMB_CHECK(isfinite(r.w) && isfinite(r.h));
    lh_free(&h);
}
#endif

EMB_TEST(text_indent_takes_room_at_the_start_edge_in_rtl)
{
    lh h = PAGE("<div style='width:300px;text-indent:50px'><span id=a>x</span></div>"
                "<div style='width:300px;text-indent:50px;direction:rtl'><span id=b>x</span></div>");
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "a", &r));
    EMB_CHECK(near(r.x, 50));
    EMB_REQUIRE(rect_of(&h, "b", &r));
    EMB_CHECK(near(r.x + r.w, 250));
    lh_free(&h);
}

EMB_TEST(percent_height_of_a_child_excludes_the_horizontal_scrollbar)
{
    lh h = PAGE("<div style='height:100px;overflow-x:scroll'><div id=k style='height:50%'></div></div>");
    lh_set_scrollbar(&h, 15);
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "k", &r));
    EMB_CHECK(near(r.h, 42.5));
    lh_free(&h);
}

EMB_TEST_MAIN()
