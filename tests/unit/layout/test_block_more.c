/*
 * Block sizing, aspect ratios, replaced sizes, container units, overflow and scrollbar gutters, and
 * the layout cost of a flat block (W3a sections 2 and 3). The expected values are the ones of CSS 2.1,
 * CSS Sizing 3 and 4, CSS Overflow 3 and CSS Containment 3, which match Chromium for these markups.
 * The margin cases with collapsing are fixtures in tests/layout/block (run by
 * test_layout_wpt.c).
 */
#include "layout_harness.h"

#include <SDL3/SDL_timer.h>
#include <math.h>
#include <string.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

static bool size_of(lh *h, const char *id, float *w, float *hh)
{
    embi_rectf r;
    embi_element *el = lh_id(h, id);
    if (el == nullptr || !embi_layout_border_box(h->eng, el, &r)) {
        return false;
    }
    *w = r.w;
    *hh = r.h;
    return true;
}

static bool client_of(lh *h, const char *id, float *w, float *hh)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_client_size(h->eng, el, w, hh);
}

static bool scroll_of(lh *h, const char *id, float *w, float *hh)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_scroll_size(h->eng, el, w, hh);
}

/* ------------------------------------------------------------------ aspect ratio */

EMB_TEST(aspect_ratio_gives_the_height_from_the_width)
{
    /* CSS Sizing 4 section 5: height = width / ratio = 100 / 2 = 50 for an auto height. */
    lh h = lh_make("<body><div id=a style='width: 100px; aspect-ratio: 2'></div>"
                   "<div id=b style='width: 40px; aspect-ratio: 1 / 2; box-sizing: border-box'></div>"
                   "<div id=c style='width: 100px; height: 20px; aspect-ratio: 2'></div></body>",
                   "body { margin: 0 }");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    /* border-box: the border box is 40 wide and 80 high (40 / 0.5). */
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 40) && near(hh, 80));
    /* An explicit height wins over the ratio. */
    EMB_CHECK(size_of(&h, "c", &w, &hh));
    EMB_CHECK(near(hh, 20));
    lh_free(&h);
}

EMB_TEST(aspect_ratio_transfers_a_definite_height_to_a_shrink_to_fit_width)
{
    /* An inline-block of auto width and height 20 with ratio 2 is 40 wide (CSS Sizing 4 section 5). */
    lh h = lh_make("<body style='margin:0'><div id=a style='display: inline-block; height: 20px; aspect-ratio: 2'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 40) && near(hh, 20));
    lh_free(&h);
}

EMB_TEST(replaced_box_keeps_its_natural_ratio_and_clamps_through_it)
{
    /* A canvas is 300x150 by default, a 2:1 ratio. Width 100 gives height 50. */
    lh h = lh_make("<body style='margin:0'><canvas id=a style='width: 100px'></canvas>"
                   "<canvas id=b style='max-width: 100px'></canvas>"
                   "<canvas id=c style='min-height: 200px'></canvas></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    /* max-width 100 transfers to the height: 100 x 50. */
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    /* min-height 200 is above the natural 150: the height is 200 and the width through the ratio 400. */
    EMB_CHECK(size_of(&h, "c", &w, &hh));
    EMB_CHECK(near(w, 400) && near(hh, 200));
    lh_free(&h);
}

/* ------------------------------------------------------------------ fit-content() */

EMB_TEST(fit_content_keyword_clamps_the_content_size)
{
    /* Two inline-blocks of 100px: min-content 100, max-content 200 (both on one line). The keyword
     * is min(max-content, max(min-content, available)): 200 in the 800px body, 150 in a 150px box. */
    lh h = lh_make("<body style='margin:0'>"
                   "<div id=a style='width: fit-content'><span style='display: inline-block; width: 100px'></span>"
                   "<span style='display: inline-block; width: 100px'></span></div>"
                   "<div style='width: 150px'><div id=b style='width: fit-content'>"
                   "<span style='display: inline-block; width: 100px'></span>"
                   "<span style='display: inline-block; width: 100px'></span></div></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 200));
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 150));
    lh_free(&h);
}

EMB_TEST(fit_content_function_clamps_between_min_and_max_content)
{
    /* fit-content(L) is min(max-content, max(min-content, L)) (CSS Sizing 3 5.2.2). Two inline-blocks
     * of 100px: min-content 100, max-content 200. */
#define TWO "<span style='display: inline-block; width: 100px'></span><span style='display: inline-block; width: 100px'></span>"
    lh h = lh_make("<body style='margin:0'>"
                   "<div id=a style='width: fit-content(150px)'>" TWO "</div>"
                   "<div id=b style='width: fit-content(50px)'>" TWO "</div>"
                   "<div id=c style='width: fit-content(500px)'>" TWO "</div>"
                   "<div id=d style='width: fit-content(20%)'>" TWO "</div>"
                   "<div id=e style='width: 400px; max-width: fit-content(150px)'>" TWO "</div>"
                   "<div id=f style='width: 10px; min-width: fit-content(150px)'>" TWO "</div>"
                   "<div id=g style='width: fit-content(calc(100px + 25px))'>" TWO "</div>"
                   "<div id=i style='box-sizing: border-box; padding: 0 10px; width: fit-content(150px)'>" TWO "</div>"
                   "<div id=j style='width: fit-content(-5px)'>" TWO "</div>"
                   "</body>",
                   "");
#undef TWO
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 150));
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 100));
    EMB_CHECK(size_of(&h, "c", &w, &hh));
    EMB_CHECK(near(w, 200));
    EMB_CHECK(size_of(&h, "d", &w, &hh)); /* 20% of 800 = 160 */
    EMB_CHECK(near(w, 160));
    EMB_CHECK(size_of(&h, "e", &w, &hh)); /* max-width clamps 400 to 150 */
    EMB_CHECK(near(w, 150));
    EMB_CHECK(size_of(&h, "f", &w, &hh)); /* min-width lifts 10 to 150 */
    EMB_CHECK(near(w, 150));
    EMB_CHECK(size_of(&h, "g", &w, &hh));
    EMB_CHECK(near(w, 125));
    /* border-box: the argument is a border-box size, so the content is 130px, the border box 150 */
    EMB_CHECK(size_of(&h, "i", &w, &hh));
    EMB_CHECK(near(w, 150));
    /* a negative argument is invalid: the declaration is dropped and the width is auto (800px) */
    EMB_CHECK(size_of(&h, "j", &w, &hh));
    EMB_CHECK(near(w, 800));
    lh_free(&h);
}

/* ------------------------------------------------------------------ container units */

EMB_TEST(container_units_fall_back_to_the_viewport_without_a_container)
{
    /* CSS Containment 3: with no container, cq units use the small viewport (800x600). */
    lh h = lh_make("<body style='margin:0'>"
                   "<div id=n style='width: 50cqw; height: 10cqh'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "n", &w, &hh));
    EMB_CHECK(near(w, 400));
    EMB_CHECK(near(hh, 60));
    lh_free(&h);
}

EMB_TEST(container_units_resolve_against_the_nearest_container)
{
    /* The style engine keeps cq units in the computed value, layout resolves them against the
     * content box of the nearest ancestor with a container-type once its size is known. */
    lh h = lh_make("<body style='margin:0'>"
                   "<div style='container-type: inline-size; width: 200px; padding: 0 20px'>"
                   "<div id=a style='width: 50cqw'></div>"
                   "<div id=b style='width: calc(100cqw - 20px)'></div>"
                   "<div style='container-type: size; width: 100px; height: 80px'>"
                   "<div id=c style='width: 50cqi; height: 25cqb'></div></div></div>"
                   "<div id=d style='width: 10cqmin'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "a", &w, &hh)); /* 50% of the 200px content box */
    EMB_CHECK(near(w, 100));
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 180));
    EMB_CHECK(size_of(&h, "c", &w, &hh)); /* the inner container is 100x80 */
    EMB_CHECK(near(w, 50));
    EMB_CHECK(near(hh, 20));
    EMB_CHECK(size_of(&h, "d", &w, &hh)); /* outside any container: the 600px viewport minimum */
    EMB_CHECK(near(w, 60));
    lh_free(&h);
}

/* ------------------------------------------------------------------ overflow and scrollbars */

EMB_TEST(scroll_container_reserves_the_scrollbar_gutter)
{
    /* The WPT runner's scrollbar is 15px. A scroll container of 100x50 has a client box of 85x35. */
    lh h = lh_make("<body style='margin:0'><div id=s style='overflow: scroll; width: 100px; height: 50px'></div></body>",
                   "");
    lh_set_scrollbar(&h, 15);
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "s", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    EMB_CHECK(client_of(&h, "s", &w, &hh));
    EMB_CHECK(near(w, 85) && near(hh, 35));
    lh_free(&h);
}

EMB_TEST(overflow_auto_reserves_a_gutter_only_when_it_overflows)
{
    lh h = lh_make("<body style='margin:0'>"
                   "<div id=a style='overflow: auto; width: 100px; height: 50px'><div style='height: 100px'></div></div>"
                   "<div id=n style='overflow: auto; width: 100px; height: 50px'><div style='height: 10px'></div></div>"
                   "</body>",
                   "");
    lh_set_scrollbar(&h, 15);
    float w = 0, hh = 0;
    /* The first overflows vertically: its content box is 15px narrower. */
    EMB_CHECK(client_of(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 85) && near(hh, 50));
    /* The second does not overflow, and keeps its full client box. */
    EMB_CHECK(client_of(&h, "n", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    lh_free(&h);
}

EMB_TEST(clipped_child_contributes_only_its_border_box_to_the_ancestors)
{
    /* CSS Overflow 3: a child that clips does not add its contents to the parent's scrollable overflow.
     * #c is 50px tall with 300px of content; #p (no overflow) is 50px tall and its scrollHeight is 50. */
    lh h = lh_make("<body style='margin:0'><div id=p><div id=c style='overflow: hidden; height: 50px'>"
                   "<div style='height: 300px'></div></div></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(scroll_of(&h, "p", &w, &hh));
    EMB_CHECK(near(hh, 50));
    /* The clipping box itself scrolls over its 300px of content. */
    EMB_CHECK(scroll_of(&h, "c", &w, &hh));
    EMB_CHECK(near(hh, 300));
    lh_free(&h);
}

EMB_TEST(padding_end_is_part_of_the_scrollable_overflow)
{
    /* The content is 60px and the padding-bottom 20px, so the scrollable height is 80px. */
    lh h = lh_make("<body style='margin:0'><div id=p style='height: 50px; padding-bottom: 20px; overflow: auto'>"
                   "<div style='height: 60px'></div></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(scroll_of(&h, "p", &w, &hh));
    EMB_CHECK(near(hh, 80));
    lh_free(&h);
}

EMB_TEST(scroll_offset_moves_the_hit_test_of_the_contents)
{
    /* A scroll offset does not change the layout but moves the contents for hit testing. The container shows 0..50 of
     * its contents: a (0..60) then k (60..160). Unscrolled, y 40 is a. Scrolled by 30, the same point is k. Below the
     * container's padding box (y 80) nothing inside it is hit, whatever the offset (the contents are clipped). */
    lh h = lh_make("<body style='margin:0'><div id=s style='overflow: scroll; width: 100px; height: 50px'>"
                   "<div id=a style='height: 60px'></div><div id=k style='height: 100px'></div></div></body>",
                   "");
    embi_element *a = lh_id(&h, "a");
    embi_element *k = lh_id(&h, "k");
    EMB_REQUIRE(a != nullptr && a->box != nullptr && k != nullptr && k->box != nullptr);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 40) == a);
    embi_frag *sf = (embi_frag *)((const embi_box *)lh_id(&h, "s")->box)->frag;
    EMB_REQUIRE(sf != nullptr);
    lh_scroll(&h, sf, embi_frag_sx(sf), 30);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 40) == k);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 80) != k);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 80) != a);
    lh_free(&h);
}

/* ------------------------------------------------------------------ margins and boxing */

EMB_TEST(box_sizing_border_box_includes_padding_and_border)
{
    lh h = lh_make("<body style='margin:0'><div id=b style='box-sizing: border-box; width: 100px; height: 50px; "
                   "padding: 10px; border: 2px solid'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "b", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 50));
    lh_free(&h);
}

EMB_TEST(min_and_max_height_clamp_the_height)
{
    lh h = lh_make("<body style='margin:0'><div id=m style='height: 50px; max-height: 30px'></div>"
                   "<div id=n style='height: 10px; min-height: 40px'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "m", &w, &hh));
    EMB_CHECK(near(hh, 30));
    EMB_CHECK(size_of(&h, "n", &w, &hh));
    EMB_CHECK(near(hh, 40));
    lh_free(&h);
}

EMB_TEST(over_constrained_margin_is_ignored_on_the_end_of_the_direction)
{
    /* CSS 2.1 10.3.3: width 200 + margins 100 and 700 is 1000 in an 800 block, which is over-constrained.
     * In an rtl containing block the left margin is the one ignored (it takes the rest, -200): the
     * box starts at -100. The containing block's direction decides, not the box's own. */
    lh h = lh_make("<body style='margin:0;direction: rtl'><div id=d style='width: 200px; margin-left: 100px; "
                   "margin-right: 700px'></div></body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&h, "d", &w, &hh));
    embi_rectf r;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "d"), &r));
    EMB_CHECK(near(r.x, -100));
    EMB_CHECK(near(w, 200));
    lh_free(&h);
}

EMB_TEST(flat_block_layout_is_linear_in_its_children)
{
    /* A block with N children is laid out in O(N): the time for 20k children is far less than the 16x
     * a quadratic walk would take over 5k. The ratio limit is loose, and its slack scales under ASan (EMB_TEST_ASAN). */
    enum { SMALL = 5000, BIG = 20000 };
    char *html_small = SDL_malloc(SMALL * 40 + 64);
    char *html_big = SDL_malloc(BIG * 40 + 64);
    size_t ls = 0, lb = 0;
    ls += (size_t)SDL_snprintf(html_small, SMALL * 40 + 64, "<body><div id=r>");
    for (int i = 0; i < SMALL; i++) {
        ls += (size_t)SDL_snprintf(html_small + ls, SMALL * 40 + 64 - ls, "<div style='height: 1px'></div>");
    }
    ls += (size_t)SDL_snprintf(html_small + ls, SMALL * 40 + 64 - ls, "</div></body>");
    lb += (size_t)SDL_snprintf(html_big, BIG * 40 + 64, "<body><div id=r>");
    for (int i = 0; i < BIG; i++) {
        lb += (size_t)SDL_snprintf(html_big + lb, BIG * 40 + 64 - lb, "<div style='height: 1px'></div>");
    }
    lb += (size_t)SDL_snprintf(html_big + lb, BIG * 40 + 64 - lb, "</div></body>");

    lh a = lh_make_n(html_small, ls, "body { margin: 0 }");
    lh b = lh_make_n(html_big, lb, "body { margin: 0 }");
    /* Warm up, then take the best of several relayouts of each: a scheduling hiccup on a loaded
     * machine inflates one run, not the minimum. */
    lh_relayout(&a);
    lh_relayout(&b);
    double freq = (double)SDL_GetPerformanceFrequency();
    double ms_small = 1e30, ms_big = 1e30;
    for (int run = 0; run < 5; run++) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        lh_relayout(&a);
        uint64_t t1 = SDL_GetPerformanceCounter();
        lh_relayout(&b);
        uint64_t t2 = SDL_GetPerformanceCounter();
        double ms_a = (double)(t1 - t0) * 1000.0 / freq;
        double ms_b = (double)(t2 - t1) * 1000.0 / freq;
        ms_small = ms_a < ms_small ? ms_a : ms_small;
        ms_big = ms_b < ms_big ? ms_b : ms_big;
    }

    /* The last child sits at y = BIG (each is 1px tall). */
    embi_element *last = lh_id(&b, "r");
    EMB_REQUIRE(last != nullptr);
    float w = 0, hh = 0;
    EMB_CHECK(size_of(&b, "r", &w, &hh));
    EMB_CHECK(near(hh, BIG));
    SDL_Log("flat block: %d children %.2f ms, %d children %.2f ms", SMALL, ms_small, BIG, ms_big);
    double limit = ms_small * 9.0 + (EMB_TEST_ASAN ? 250.0 : 25.0);
    EMB_CHECK(ms_big < limit);
    lh_free(&a);
    lh_free(&b);
    SDL_free(html_small);
    SDL_free(html_big);
}

EMB_TEST_MAIN()
