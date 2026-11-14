/*
 * Opacity group decisions of W4d (layer.c): empty, folded (one item or non-overlapping items), target (overlapping,
 * nested, or a triangle), with the bounds taken through transforms and clips, and the same decisions on real pages.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/layer.h"
#include "paint/paint_int.h"

/* Opens a group: a LAYER_BEGIN with opacity 0.5. Returns its index. */
static uint32_t open_group(embi_dl *d)
{
    embi_ditem o;
    memset(&o, 0, sizeof o);
    o.kind = EMBI_DI_LAYER_BEGIN;
    o.f[0] = 0.5f;
    uint32_t at = d->items.len;
    embi_vec_push(&d->items, o);
    return at;
}

/* Closes the group opened at begin: a LAYER_END whose aux is the begin (paint.c's convention). Returns its index. */
static uint32_t close_group(embi_dl *d, uint32_t begin)
{
    embi_ditem e;
    memset(&e, 0, sizeof e);
    e.kind = EMBI_DI_LAYER_END;
    e.aux = begin;
    uint32_t at = d->items.len;
    embi_vec_push(&d->items, e);
    return at;
}

static void fill(embi_pst *p, float x, float y, float w, float h, uint32_t xf, uint32_t clip)
{
    float r[4] = {x, y, w, h};
    float radii[4] = {0, 0, 0, 0};
    EMB_CHECK(embi_pb_fill(p, r, radii, PH_RED, xf, clip));
}

EMB_TEST(group_without_drawable_items_is_empty)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    uint32_t b = open_group(&d);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_EMPTY);
    embi_dl_free(&d);
}

EMB_TEST(group_of_one_item_folds)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_FOLD);
    embi_dl_free(&d);
}

EMB_TEST(group_of_disjoint_items_folds_each)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    fill(&p, 20, 0, 10, 10, 0, 0);
    /* Sharing an edge is not an overlap. */
    fill(&p, 30, 0, 10, 10, 0, 0);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_FOLD);
    embi_dl_free(&d);
}

EMB_TEST(group_of_overlapping_items_needs_a_target)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    fill(&p, 5, 5, 10, 10, 0, 0);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_TARGET);
    embi_dl_free(&d);
}

EMB_TEST(group_overlap_is_measured_through_transforms)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    /* A translation by 9 px moves the second item over the first. */
    const float t9[6] = {1, 0, 0, 1, 9, 0};
    uint32_t x9 = embi_pb_xform(&p, t9);
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    fill(&p, 0, 0, 10, 10, x9, 0);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_TARGET);
    embi_dl_free(&d);

    /* Translated by exactly the width: they only touch. */
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    p.d = &d;
    const float t10[6] = {1, 0, 0, 1, 10, 0};
    uint32_t x10 = embi_pb_xform(&p, t10);
    b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    fill(&p, 0, 0, 10, 10, x10, 0);
    e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_FOLD);
    embi_dl_free(&d);
}

EMB_TEST(group_overlap_ignores_items_clipped_away)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    /* The second item lies over the first, but its clip (far away) leaves it nothing to paint. */
    float cr[4] = {50, 50, 10, 10};
    float zero[4] = {0, 0, 0, 0};
    uint32_t clip = embi_pb_clip(&p, cr, zero, 0, 0);
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    fill(&p, 2, 2, 10, 10, 0, clip);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_FOLD);
    embi_dl_free(&d);
}

EMB_TEST(group_with_a_nested_group_needs_a_target)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    uint32_t b = open_group(&d);
    fill(&p, 0, 0, 10, 10, 0, 0);
    uint32_t ib = open_group(&d);
    fill(&p, 100, 100, 10, 10, 0, 0);
    close_group(&d, ib);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_TARGET);
    embi_dl_free(&d);
}

EMB_TEST(group_with_a_triangle_needs_a_target)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    float r[4] = {0, 0, 8, 8};
    uint32_t b = open_group(&d);
    embi_pb_triangle(&p, r, EMBI_TRI_RIGHT, PH_RED, 0, 0);
    uint32_t e = close_group(&d, b);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&d, b, e), EMBI_GROUP_TARGET);
    embi_dl_free(&d);
}

EMB_TEST(page_group_text_over_background_needs_a_target)
{
    ph h = ph_make("<div style=\"opacity:0.5;background:#f00;height:20px\">text</div>", "");
    int b = ph_find(&h.dl, 0, EMBI_DI_LAYER_BEGIN);
    EMB_REQUIRE(b >= 0);
    int e = ph_find(&h.dl, (uint32_t)b + 1, EMBI_DI_LAYER_END);
    EMB_REQUIRE(e >= 0);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&h.dl, (uint32_t)b, (uint32_t)e), EMBI_GROUP_TARGET);
    ph_free(&h);
}

EMB_TEST(page_group_of_separate_boxes_folds)
{
    ph h = ph_make("<div style=\"opacity:0.5;height:20px\">"
                   "<div style=\"background:#f00;height:10px\"></div>"
                   "<div style=\"background:#00f;height:10px\"></div></div>",
                   "");
    int b = ph_find(&h.dl, 0, EMBI_DI_LAYER_BEGIN);
    EMB_REQUIRE(b >= 0);
    int e = ph_find(&h.dl, (uint32_t)b + 1, EMBI_DI_LAYER_END);
    EMB_REQUIRE(e >= 0);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&h.dl, (uint32_t)b, (uint32_t)e), EMBI_GROUP_FOLD);
    ph_free(&h);
}

EMB_TEST(page_group_of_one_box_folds_into_its_color)
{
    ph h = ph_make("<div style=\"opacity:0.5;background:#f00;height:10px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    ph_free(&h);
}

/* opacity-overlapping-letters: opacity applies to the text as one group. Text whose glyphs overlap (negative
 * letter-spacing) stays in a layer; the alpha is not folded into the one text item, where overlaps would darken. */
EMB_TEST(page_group_of_overlapping_letters_is_not_folded_into_the_text)
{
    ph h = ph_make("<div style=\"opacity:0.5;letter-spacing:-0.6em;font:50px sans-serif\">XXXXX</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 1);
    int b = ph_find(&h.dl, 0, EMBI_DI_LAYER_BEGIN);
    int e = b >= 0 ? ph_find(&h.dl, (uint32_t)b + 1, EMBI_DI_LAYER_END) : -1;
    EMB_REQUIRE(b >= 0 && e >= 0);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&h.dl, (uint32_t)b, (uint32_t)e), EMBI_GROUP_TARGET);
    ph_free(&h);
    h = ph_make("<div style=\"opacity:0.5;letter-spacing:-0.02em;font:50px sans-serif\">XXXXX</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0); /* a hairline overlap still folds */
    ph_free(&h);
    h = ph_make("<div style=\"opacity:0.5;font:50px sans-serif\">XXXXX</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0); /* ordinary text still folds */
    ph_free(&h);
}

EMB_TEST_MAIN()
