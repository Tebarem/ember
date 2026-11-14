/*
 * Hit testing in paint order (W5d2): the topmost element under a point follows the order
 * src/paint emits (CSS 2.1 Appendix E): stacking contexts and z-index, positioned boxes in tree order, floats above
 * blocks, and the clip of an overflow container. pointer-events: none and visibility hidden remove a box as a target
 * and not its visible children. Every case is laid out by hand from those rules; the boxes are 100px squares at the
 * origin of the viewport, so the point (10, 10) is inside all of them.
 */
#include "layout_harness.h"

#include <stdint.h>

static embi_element *hit_at(lh *h, float x, float y)
{
    return embi_layout_hit_test(h->eng, h->doc, x, y);
}

EMB_TEST(later_positioned_box_is_on_top_at_equal_z)
{
    lh h = lh_make("<body><div id=a style='position:absolute;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=b style='position:absolute;left:0;top:0;width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "b"));
    lh_free(&h);
}

EMB_TEST(z_index_beats_tree_order)
{
    lh h = lh_make("<body><div id=a style='position:absolute;z-index:2;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=b style='position:absolute;z-index:1;left:0;top:0;width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(negative_z_index_is_below_the_flow)
{
    /* The in-flow block paints after the negative z-index context of the root, so it is the target. */
    lh h = lh_make("<body><div id=n style='position:absolute;z-index:-1;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=f style='width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "f"));
    lh_free(&h);
}

EMB_TEST(positioned_box_is_above_the_flow_blocks)
{
    lh h = lh_make("<body><div id=f style='width:100px;height:100px'></div>"
                   "<div id=p style='position:absolute;left:0;top:0;width:50px;height:50px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "p"));
    lh_free(&h);
}

EMB_TEST(floats_are_above_block_boxes_whatever_the_tree_order)
{
    lh h = lh_make("<body><div id=fl style='float:left;width:50px;height:50px'></div>"
                   "<div id=b style='width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "fl"));
    EMB_CHECK(hit_at(&h, 80, 80) == lh_id(&h, "b"));
    lh_free(&h);
}

EMB_TEST(pointer_events_none_passes_the_point_to_the_box_below)
{
    lh h = lh_make("<body><div id=lo style='position:absolute;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=hi style='position:absolute;left:0;top:0;width:100px;height:100px;pointer-events:none'></div>"
                   "</body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "lo"));
    lh_free(&h);
}

EMB_TEST(pointer_events_auto_child_of_a_none_parent_is_hit)
{
    lh h = lh_make("<body><div id=lo style='position:absolute;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=pa style='position:absolute;left:0;top:0;width:100px;height:100px;pointer-events:none'>"
                   "<div id=ch style='pointer-events:auto;width:50px;height:50px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "ch"));
    EMB_CHECK(hit_at(&h, 80, 80) == lh_id(&h, "lo"));
    lh_free(&h);
}

EMB_TEST(visibility_hidden_box_is_not_a_target_but_its_visible_child_is)
{
    lh h = lh_make("<body><div id=lo style='position:absolute;left:0;top:0;width:100px;height:100px'></div>"
                   "<div id=hi style='position:absolute;left:0;top:0;width:100px;height:100px;visibility:hidden'>"
                   "<span id=vis style='display:block;visibility:visible;width:50px;height:50px'></span></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "vis"));
    EMB_CHECK(hit_at(&h, 80, 80) == lh_id(&h, "lo"));
    lh_free(&h);
}

EMB_TEST(child_stacking_context_stays_inside_its_parent_context)
{
    /* sp (z 1) holds sc (z 100). s5 (z 5) is a later sibling of sp: it is above the whole of sp, sc included. */
    lh h = lh_make("<body><div id=sp style='position:relative;z-index:1;width:100px;height:100px'>"
                   "<div id=sc style='position:absolute;z-index:100;left:0;top:0;width:100px;height:100px'></div></div>"
                   "<div id=s5 style='position:absolute;z-index:5;left:0;top:0;width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "s5"));
    lh_free(&h);

    lh g = lh_make("<body><div id=sp style='position:relative;z-index:1;width:100px;height:100px'>"
                   "<div id=sc style='position:absolute;z-index:100;left:0;top:0;width:100px;height:100px'></div></div>"
                   "</body>",
                   "");
    EMB_CHECK(hit_at(&g, 10, 10) == lh_id(&g, "sc"));
    lh_free(&g);
}

EMB_TEST(positioned_unit_content_is_above_an_earlier_unit_and_below_a_later_one)
{
    /* p1 (positioned, z auto) holds c1 (positioned, z auto): c1 is painted with p1, after p1's own box. p2 comes later, so
     * it is above both. Without p2, c1 is the target. */
    lh h = lh_make("<body><div id=p1 style='position:absolute;left:0;top:0;width:100px;height:100px'>"
                   "<div id=c1 style='position:absolute;left:0;top:0;width:50px;height:50px'></div></div>"
                   "<div id=p2 style='position:absolute;left:0;top:0;width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "p2"));
    lh_free(&h);

    lh g = lh_make("<body><div id=p1 style='position:absolute;left:0;top:0;width:100px;height:100px'>"
                   "<div id=c1 style='position:absolute;left:0;top:0;width:50px;height:50px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&g, 10, 10) == lh_id(&g, "c1"));
    lh_free(&g);
}

EMB_TEST(overflow_clip_hides_the_contents_outside_the_box)
{
    lh h = lh_make("<body><div id=cl style='overflow:hidden;width:50px;height:50px'>"
                   "<div id=in style='width:100px;height:100px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "in"));
    EMB_CHECK(hit_at(&h, 80, 80) != lh_id(&h, "in"));
    lh_free(&h);
}

EMB_TEST(an_abspos_box_escapes_the_overflow_of_a_non_positioned_parent)
{
    /* Its containing block is the viewport, not the overflow:hidden parent, so the parent's clip does not apply (CSS 2.1
     * 11.1.1): the box is hit outside the parent's 50px too. */
    lh h = lh_make("<body><div id=cl style='overflow:hidden;width:50px;height:50px'>"
                   "<div id=ab style='position:absolute;left:0;top:0;width:100px;height:100px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 80, 80) == lh_id(&h, "ab"));
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "ab"));
    lh_free(&h);
}

EMB_TEST(scrolled_viewport_hits_the_content_at_the_scrolled_position)
{
    /* far sits at document y 1000. Unscrolled, viewport y 100 is the spacer; with the viewport scrolled to y 900 the same
     * viewport point is far, as paint draws it (the point is in viewport coordinates, the root's offset is applied once). */
    lh h = lh_make("<body style='margin:0'><div style='height:1000px'></div>"
                   "<div id=far style='width:100px;height:100px'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 100) != lh_id(&h, "far"));
    embi_layout_frag_set_scroll(h.eng, h.doc, (embi_frag *)embi_layout_root(h.eng, h.doc), 0.0f, 900.0f);
    EMB_CHECK(hit_at(&h, 10, 100) == lh_id(&h, "far"));
    EMB_CHECK(hit_at(&h, 10, 10) != lh_id(&h, "far"));
    lh_free(&h);
}

EMB_TEST(contain_paint_clips_the_contents_to_the_padding_box)
{
    lh h = lh_make("<body style='margin:0'><div id=cl style='contain:paint;width:50px;height:50px'>"
                   "<div id=in style='width:100px;height:100px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "in"));
    EMB_CHECK(hit_at(&h, 80, 80) != lh_id(&h, "in"));
    lh_free(&h);
}

EMB_TEST(legacy_clip_rect_bounds_an_abspos_box)
{
    /* clip: rect(0, 50px, 50px, 0) keeps the box's own hit area to its top-left 50 px square. */
    lh h = lh_make("<body style='margin:0'><div id=ab style='position:absolute;left:0;top:0;width:100px;height:100px;"
                   "clip:rect(0px,50px,50px,0px)'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "ab"));
    EMB_CHECK(hit_at(&h, 80, 80) != lh_id(&h, "ab"));
    lh_free(&h);
}

EMB_TEST(relayout_clamps_the_kept_viewport_scroll_to_the_new_range)
{
    /* A 1000px spacer and a 100px box: the document is 1100px, the viewport 600px (max scroll 500 here, so the page is
     * scrolled to its maximum). After the spacer shrinks to 100px the document fits the viewport: the kept offset must be
     * clamped to the new range (0), as a browser does after layout, and not stay out of range. */
    lh h = lh_make("<body style='margin:0'><div id=sp style='height:1000px'></div>"
                   "<div id=far style='width:100px;height:100px'></div></body>",
                   "");
    embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_REQUIRE(root != nullptr);
    float ov[4];
    embi_frag_ov(root, ov);
    float max0 = ov[3] - root->h;
    EMB_REQUIRE(max0 > 100.0f);
    embi_layout_frag_set_scroll(h.eng, h.doc, root, 0.0f, max0);
    EMB_CHECK(embi_frag_sy(root) == max0);
    embi_element *sp = lh_id(&h, "sp");
    EMB_REQUIRE(sp != nullptr);
    EMB_REQUIRE(embi_el_set_attribute(sp, EMBI_STR("style"), embi_dstr_from_utf8(h.doc->slab, EMBI_STR("height:100px"))) ==
                EMBI_DOM_OK);
    lh_relayout(&h);
    root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_REQUIRE(root != nullptr);
    embi_frag_ov(root, ov);
    float max1 = ov[3] - root->h > 0.0f ? ov[3] - root->h : 0.0f;
    EMB_CHECK(max1 < max0);
    EMB_CHECK(embi_frag_sy(root) <= max1);
    EMB_CHECK(embi_frag_sy(root) >= 0.0f);
    lh_free(&h);
}

EMB_TEST(flex_order_decides_which_overlapping_item_is_on_top)
{
    /* b is laid out first (order 0) and the margin pulls a over it; a has order 2, so it is painted last, whatever its
     * tree position. */
    lh h = lh_make("<body style='margin:0'><div style='display:flex;width:300px'>"
                   "<div id=a style='order:2;width:100px;height:100px'></div>"
                   "<div id=b style='margin-right:-100px;width:100px;height:100px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(flex_order_orders_overlapping_positioned_items)
{
    lh h = lh_make("<body style='margin:0'><div style='display:flex;width:300px'>"
                   "<div id=a style='position:relative;order:2;width:100px;height:100px'></div>"
                   "<div id=b style='position:relative;margin-right:-100px;width:100px;height:100px'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST_MAIN()
