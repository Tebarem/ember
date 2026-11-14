/*
 * Hit testing through transforms (R3c): 2D transforms, perspective, 3D rendering contexts (the plane nearest the
 * viewer wins) and backface-visibility.
 */
#include "layout_harness.h"

#include <stdio.h>

static embi_element *hit_at(lh *h, float x, float y)
{
    return embi_layout_hit_test(h->eng, h->doc, x, y);
}

EMB_TEST(translated_box_is_hit_at_its_new_place)
{
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;transform:translate(200px,0)'></div></body>", "");
    EMB_CHECK(hit_at(&h, 10, 10) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 210, 10) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(rotated_box_inverse_maps_the_point)
{
    /* rotate(45deg) about the center (50,50): the corner (0,0) moves out of the box, the center stays in. */
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;transform:rotate(45deg)'></div></body>", "");
    EMB_CHECK(hit_at(&h, 50, 50) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 2, 2) != lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 50, 2) == lh_id(&h, "a")); /* the rotated top corner is at (50, -20); (50,2) is inside */
    lh_free(&h);
}

EMB_TEST(perspective_foreshortens_the_hit_area)
{
    lh h = lh_make("<body><div style='perspective:200px;width:200px;height:200px'>"
                   "<div id=a style='width:200px;height:200px;transform:rotateY(60deg)'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 100, 100) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 2, 100) != lh_id(&h, "a")); /* the left edge swings away from x = 0 */
    lh_free(&h);
}

EMB_TEST(singular_transform_is_not_a_target)
{
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;transform:scale3d(2,2,0)'></div></body>", "");
    EMB_CHECK(hit_at(&h, 10, 10) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(backface_hidden_is_not_a_target)
{
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;backface-visibility:hidden;"
                   "transform:rotateY(180deg)'></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 50, 50) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(backface_visible_is_a_target)
{
    lh h = lh_make("<body><div id=a style='width:100px;height:100px;transform:rotateY(180deg)'></div></body>", "");
    EMB_CHECK(hit_at(&h, 50, 50) == lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(preserve_3d_nearer_plane_wins_whatever_the_dom_order)
{
    lh h = lh_make("<body><div style='perspective:500px;width:200px;height:200px'>"
                   "<div style='transform-style:preserve-3d;width:200px;height:200px'>"
                   "<div id=near style='position:absolute;width:200px;height:200px;transform:translateZ(50px)'></div>"
                   "<div id=far style='position:absolute;width:200px;height:200px;transform:translateZ(-50px)'></div>"
                   "</div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 100, 100) == lh_id(&h, "near"));
    lh_free(&h);
}

EMB_TEST(flat_nested_transform_composes)
{
    lh h = lh_make("<body><div style='width:10px;height:10px;transform:translate(100px,0)'>"
                   "<div id=a style='width:50px;height:50px;transform:translate(0,100px)'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 120, 120) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 20, 120) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(intersecting_planes_flip_at_the_line_of_intersection)
{
    /* rotateY(30deg) swings the right half away and the left half toward the viewer; rotateY(-30deg) does the opposite.
     * The two planes cross on the vertical line x = 100: left of it a is nearer, right of it b is, whatever the DOM
     * order says. */
    lh h = lh_make("<body><div style='perspective:600px;width:200px;height:200px'>"
                   "<div style='transform-style:preserve-3d;width:200px;height:200px'>"
                   "<div id=a style='position:absolute;width:200px;height:200px;transform:rotateY(30deg)'></div>"
                   "<div id=b style='position:absolute;width:200px;height:200px;transform:rotateY(-30deg)'></div>"
                   "</div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 60, 100) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 140, 100) == lh_id(&h, "b"));
    lh_free(&h);
}

EMB_TEST(perspective_origin_moves_the_vanishing_point)
{
    /* A box 100px toward the viewer under perspective 300px is 1.5 times larger about the perspective origin. */
    lh h = lh_make("<body><div style='perspective:300px;perspective-origin:0 0;width:200px;height:200px'>"
                   "<div id=a style='width:200px;height:200px;transform:translateZ(100px)'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 280, 100) == lh_id(&h, "a")); /* 200 x 1.5 = 300 wide from the corner */
    lh_free(&h);
    lh c = lh_make("<body><div style='perspective:300px;width:200px;height:200px'>"
                   "<div id=a style='width:200px;height:200px;transform:translateZ(100px)'></div></div></body>",
                   "");
    EMB_CHECK(hit_at(&c, 280, 100) != lh_id(&c, "a")); /* about the center it spans -50 .. 250 */
    EMB_CHECK(hit_at(&c, 240, 100) == lh_id(&c, "a"));
    lh_free(&c);
}

EMB_TEST(transform_origin_z_changes_the_projection)
{
    /* Turning 60deg about an axis 100px behind the box swings its center to x = 174 on screen; about its own center it
     * stays at x = 100. */
    lh h = lh_make("<body><div style='perspective:300px;width:400px;height:200px'>"
                   "<div id=a style='width:200px;height:200px;transform-origin:50% 50% -100px;transform:rotateY(60deg)'>"
                   "</div></div></body>",
                   "");
    EMB_CHECK(hit_at(&h, 170, 100) == lh_id(&h, "a"));
    EMB_CHECK(hit_at(&h, 120, 100) != lh_id(&h, "a"));
    lh_free(&h);
}

EMB_TEST(preserve_3d_through_a_wrapper_keeps_the_later_planes)
{
    /* A preserve-3d box (p2) that is a descendant of the outer plane through a wrapper must not disturb the search for
     * the planes of the outer context that come after it in the tree. */
    const char *pre = "<body><div style='perspective:500px;width:200px;height:200px'>"
                      "<div id=p1 style='transform-style:preserve-3d;width:200px;height:200px'>"
                      "<div id=w style='width:200px;height:200px'>"
                      "<div id=p2 style='position:absolute;transform-style:preserve-3d;width:200px;height:200px;"
                      "transform:translateZ(10px)'>"
                      "<div id=in style='position:absolute;width:200px;height:200px;transform:translateZ(20px)'></div>"
                      "</div></div>";
    char buf[1024];
    snprintf(buf, sizeof buf, "%s<div id=late style='position:absolute;top:0;left:0;width:200px;height:200px;transform:translateZ(%s)'></div>"
                              "</div></div></body>", pre, "40px");
    lh h = lh_make(buf, "");
    EMB_CHECK(hit_at(&h, 100, 100) == lh_id(&h, "late"));
    lh_free(&h);
    snprintf(buf, sizeof buf, "%s<div id=late style='position:absolute;top:0;left:0;width:200px;height:200px;transform:translateZ(%s)'></div>"
                              "</div></div></body>", pre, "-40px");
    lh g = lh_make(buf, "");
    EMB_CHECK(hit_at(&g, 100, 100) == lh_id(&g, "in"));
    lh_free(&g);
}

EMB_TEST(untransformed_document_does_not_walk_for_transforms)
{
    lh h = lh_make("<body><div id=a style='width:100px;height:100px'></div></body>", "");
    EMB_CHECK(!embi_lay_doc_has_transform(h.eng, h.doc));
    EMB_CHECK(hit_at(&h, 10, 10) == lh_id(&h, "a"));
    lh_free(&h);
    lh t = lh_make("<body><div id=a style='width:100px;height:100px;transform:rotate(1deg)'></div></body>", "");
    EMB_CHECK(embi_lay_doc_has_transform(t.eng, t.doc));
    lh_free(&t);
}

EMB_TEST_MAIN()
