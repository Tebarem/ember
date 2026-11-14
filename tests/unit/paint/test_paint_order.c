/*
 * Paint order (CSS 2.1 Appendix E, W4c section 3): stacking contexts, z-index layers, positioned and floated
 * boxes, opacity layers and folding, 2D transforms and overflow clips, all checked on the display list.
 */
#include "paint_harness.h"

#include <math.h>
#include <stdio.h>

/* The index of the fill of color c, failing the test when there is none. */
static int need_fill(const embi_dl *d, uint32_t c)
{
    int i = ph_fill_of(d, c);
    EMB_CHECK(i >= 0);
    return i;
}

EMB_TEST(tree_order_for_plain_blocks)
{
    ph h = ph_make("<div style=\"background:#f00;height:10px\"></div><div style=\"background:#00f;height:10px\"></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(r < b);
    /* the root (initial containing block) and the html element, which is a stacking context */
    EMB_CHECK(h.dl.ctxs.len >= 1);
    ph_free(&h);
}

EMB_TEST(block_backgrounds_paint_before_inline_content)
{
    /* the text of the first block is painted after the background of the second block (step 5 after step 3) */
    ph h = ph_make("<div style=\"color:#000\">text</div><div style=\"background:#f00;height:10px\"></div>", "");
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    int r = need_fill(&h.dl, PH_RED);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(r < t);
    ph_free(&h);
}

EMB_TEST(floats_paint_after_block_backgrounds_and_before_inline_content)
{
    ph h = ph_make("<div style=\"float:left;background:#f00;width:10px;height:10px\"></div>"
                   "<div style=\"background:#00f;height:10px\">t</div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    /* the float is first in the tree, but the block's background is step 3 and the float step 4 */
    EMB_CHECK(b < r);
    EMB_REQUIRE(t >= 0);
    EMB_CHECK(r < t);
    ph_free(&h);
}

EMB_TEST(negative_z_paints_below_in_flow_content)
{
    /* the negative z-index box is later in the tree, yet it paints before the in-flow block (step 2) */
    ph h = ph_make("<div style=\"background:#00f;height:10px\"></div>"
                   "<div style=\"position:relative;z-index:-1;background:#f00;height:10px\"></div>", "");
    int b = need_fill(&h.dl, PH_BLUE), r = need_fill(&h.dl, PH_RED);
    EMB_CHECK(r < b);
    /* the negative z-index box is its own stacking context, and the root context is the parent */
    EMB_CHECK(h.dl.ctxs.len >= 2);
    ph_free(&h);
}

EMB_TEST(z_auto_positioned_paints_after_flow_and_before_positive_z)
{
    ph h = ph_make("<div style=\"position:absolute;z-index:1;width:10px;height:10px;background:#00f\"></div>"
                   "<div style=\"position:relative;width:10px;height:10px;background:#0f0\"></div>"
                   "<div style=\"background:#f00;height:10px\"></div>", "");
    int red = need_fill(&h.dl, PH_RED), green = need_fill(&h.dl, PH_GREEN), blue = need_fill(&h.dl, PH_BLUE);
    /* flow (red) < z-auto positioned (green) < z-index 1 (blue), whatever the tree order */
    EMB_CHECK(red < green);
    EMB_CHECK(green < blue);
    ph_free(&h);
}

EMB_TEST(positive_z_in_tree_order_of_z_value)
{
    ph h = ph_make("<div style=\"position:relative;z-index:3;width:5px;height:5px;background:#00f\"></div>"
                   "<div style=\"position:relative;z-index:2;width:5px;height:5px;background:#f00\"></div>", "");
    int blue = need_fill(&h.dl, PH_BLUE), red = need_fill(&h.dl, PH_RED);
    EMB_CHECK(red < blue);
    ph_free(&h);
}

EMB_TEST(stacking_context_is_atomic_for_its_children)
{
    /* the child with z-index 5 is inside a z-index 1 context, so it cannot paint over a z-index 2 sibling */
    ph h = ph_make("<div style=\"position:relative;z-index:1;width:10px;height:10px\">"
                   "<div style=\"position:relative;z-index:5;width:5px;height:5px;background:#f00\"></div></div>"
                   "<div style=\"position:relative;z-index:2;width:10px;height:10px;background:#00f\"></div>", "");
    int red = need_fill(&h.dl, PH_RED), blue = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(red < blue);
    ph_free(&h);
}

EMB_TEST(opacity_with_one_item_folds_into_its_color)
{
    ph h = ph_make("<div style=\"opacity:0.5;background:#f00;height:10px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_END), 0);
    int i = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(i >= 0);
    uint32_t a = h.dl.items.data[i].colors[0] >> 24;
    EMB_CHECK(a == 128u);
    EMB_CHECK((h.dl.items.data[i].colors[0] & 0x00FFFFFFu) == 0x000000FFu);
    ph_free(&h);
}

EMB_TEST(opacity_with_several_items_keeps_a_layer)
{
    ph h = ph_make("<div style=\"opacity:0.5\"><div style=\"background:#f00;height:5px\"></div>"
                   "<div style=\"background:#00f;height:5px\"></div></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 1);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_END), 1);
    int open = ph_find(&h.dl, 0, EMBI_DI_LAYER_BEGIN);
    EMB_REQUIRE(open >= 0);
    EMB_CHECK(h.dl.items.data[open].f[0] > 0.49f && h.dl.items.data[open].f[0] < 0.51f);
    int close = ph_find(&h.dl, 0, EMBI_DI_LAYER_END);
    EMB_CHECK(h.dl.items.data[close].aux == (uint32_t)open);
    /* the group's context covers its two fills */
    EMB_CHECK(h.dl.ctxs.len >= 2);
    ph_free(&h);
}

EMB_TEST(opacity_with_no_items_drops_its_layer)
{
    ph h = ph_make("<div style=\"opacity:0.2;width:5px;height:5px\"></div>", "");
    EMB_CHECK_EQ_INT(h.dl.items.len, 0);
    ph_free(&h);
}

EMB_TEST(transform_translate_sets_the_item_transform)
{
    ph h = ph_make("<div style=\"transform:translate(10px,4px);background:#f00;width:10px;height:10px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_REQUIRE(it->xform != 0);
    const float *m = h.dl.xforms.data[it->xform].m;
    EMB_CHECK(m[0] == 1.0f && m[3] == 1.0f);
    EMB_CHECK(m[4] > 9.99f && m[4] < 10.01f);
    EMB_CHECK(m[5] > 3.99f && m[5] < 4.01f);
    /* the rect stays in the untransformed coordinates */
    EMB_CHECK(it->rect[0] == 0.0f && it->rect[1] == 0.0f);
    ph_free(&h);
}

EMB_TEST(transform_origin_keeps_the_center_fixed)
{
    /* rotate 90 degrees about the center of a 10 x 10 box: the origin maps to itself */
    ph h = ph_make("<div style=\"transform:rotate(90deg);background:#f00;width:10px;height:10px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    /* the affine map of (5, 5) is (5, 5) */
    float x = m[0] * 5.0f + m[2] * 5.0f + m[4];
    float y = m[1] * 5.0f + m[3] * 5.0f + m[5];
    EMB_CHECK(x > 4.99f && x < 5.01f);
    EMB_CHECK(y > 4.99f && y < 5.01f);
    ph_free(&h);
}

EMB_TEST(nested_transforms_compose_with_the_parent_first)
{
    ph h = ph_make("<div style=\"transform:translateX(10px)\"><div style=\"transform:translateX(3px);background:#f00;width:4px;height:4px\"></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    EMB_CHECK(m[4] > 12.99f && m[4] < 13.01f);
    ph_free(&h);
}

EMB_TEST(overflow_hidden_clips_its_content_to_the_padding_box)
{
    ph h = ph_make("<div style=\"overflow:hidden;width:20px;height:20px;padding:2px\">"
                   "<div style=\"height:50px;background:#f00\"></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    uint32_t c = h.dl.items.data[i].clip;
    EMB_REQUIRE(c != 0);
    const embi_dclip *cl = &h.dl.clips.data[c];
    /* the clip is the padding box: the 20 px content box plus 2 px of padding on each side */
    EMB_CHECK(cl->rect[2] == 24.0f);
    EMB_CHECK(cl->rect[3] == 24.0f);
    ph_free(&h);
}

EMB_TEST(visibility_on_a_positioned_box_hides_only_it)
{
    ph h = ph_make("<div style=\"position:absolute;visibility:hidden;background:#f00;width:5px;height:5px\"></div>"
                   "<div style=\"background:#00f;width:5px;height:5px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_fill_of(&h.dl, PH_RED), -1);
    need_fill(&h.dl, PH_BLUE);
    ph_free(&h);
}

EMB_TEST(inline_box_background_precedes_its_text)
{
    ph h = ph_make("<p>a<span style=\"background:#f00\">bc</span></p>", "");
    int r = need_fill(&h.dl, PH_RED);
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(t >= 0);
    /* the text "a" is before the span, the span's background is before the span's own text */
    int bc = -1;
    for (uint32_t i = (uint32_t)r + 1; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind == EMBI_DI_TEXT && it->aux2 - it->aux3 == 2) {
            bc = (int)i;
            break;
        }
    }
    EMB_REQUIRE(bc >= 0);
    EMB_CHECK(r < bc);
    ph_free(&h);
}

EMB_TEST(positioned_inside_a_negative_context_stays_inside_it)
{
    ph h = ph_make("<div style=\"position:relative;z-index:-1\">"
                   "<div style=\"position:absolute;top:0;left:0;width:5px;height:5px;background:#f00\"></div></div>"
                   "<div style=\"background:#00f;height:5px\"></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(r < b);
    ph_free(&h);
}

EMB_TEST(every_item_refers_to_a_context_range)
{
    ph h = ph_make("<div style=\"opacity:.5\"><div style=\"position:relative;z-index:1;background:#f00;height:5px\"></div>"
                   "<div style=\"background:#00f;height:5px\"></div></div>", "");
    for (uint32_t i = 0; i < h.dl.ctxs.len; i++) {
        const embi_dctx *c = &h.dl.ctxs.data[i];
        EMB_CHECK(c->first <= c->end);
        EMB_CHECK(c->end <= h.dl.items.len);
        if (i > 0) {
            EMB_CHECK(c->parent < i);
        }
    }
    ph_free(&h);
}

/* ------------------------------------------------------------------ flex and grid order, z-index layers */

EMB_TEST(flex_items_paint_in_order_property_order)
{
    /* the red item is first in the tree with order 2: blue (order 1) paints first */
    ph h = ph_make("<div style=\"display:flex\"><div style=\"order:2;background:#f00;width:5px;height:5px\"></div>"
                   "<div style=\"order:1;background:#00f;width:5px;height:5px\"></div></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(b < r);
    ph_free(&h);
}

EMB_TEST(flex_items_with_equal_order_keep_tree_order)
{
    ph h = ph_make("<div style=\"display:flex\"><div style=\"order:1;background:#f00;width:5px;height:5px\"></div>"
                   "<div style=\"order:1;background:#00f;width:5px;height:5px\"></div></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(r < b);
    ph_free(&h);
}

EMB_TEST(grid_items_paint_in_order_property_order)
{
    ph h = ph_make("<div style=\"display:grid\"><div style=\"order:2;background:#f00;width:5px;height:5px\"></div>"
                   "<div style=\"order:-1;background:#00f;width:5px;height:5px\"></div></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(b < r);
    ph_free(&h);
}

EMB_TEST(flex_item_with_z_index_is_a_stacking_context_in_order)
{
    /* the z-index 2 item is first in the tree, but it paints after the z-index 1 item: both are contexts */
    ph h = ph_make("<div style=\"display:flex\"><div style=\"position:relative;z-index:2;background:#f00;width:5px;height:5px\"></div>"
                   "<div style=\"position:relative;z-index:1;background:#00f;width:5px;height:5px\"></div></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(b < r);
    /* the root context, the flex items' contexts */
    EMB_CHECK(h.dl.ctxs.len >= 3);
    ph_free(&h);
}

#define BOX(extra, color) "<div style=\"" extra ";width:5px;height:5px;background:" color "\"></div>"

/* Checks that the fill of color first paints before the fill of color second. */
static void check_before_at(int line, const char *html, const char *css, uint32_t first, uint32_t second)
{
    (void)line;
    ph h = ph_make(html, css);
    int a = ph_fill_of(&h.dl, first), b = ph_fill_of(&h.dl, second);
    EMB_CHECK(a >= 0);
    EMB_CHECK(b >= 0);
    EMB_CHECK(a < b);
    ph_free(&h);
}

#define check_before(...) check_before_at(__LINE__, __VA_ARGS__)

EMB_TEST(zero_z_positioned_paints_in_tree_order_with_auto_ones)
{
    /* z-index 0 and auto positioned boxes share step 6 of their context: tree order decides, in both orders. The
     * abspos boxes have the viewport as their containing block (children of the root fragment, not of html). */
    check_before(BOX("position:relative;z-index:0", "#00f") BOX("position:absolute;top:0;left:0", "#f00"), "",
                 PH_BLUE, PH_RED);
    check_before(BOX("position:absolute;top:0;left:0", "#f00") BOX("position:relative;z-index:0", "#00f"), "",
                 PH_RED, PH_BLUE);
    /* the same inside a positioned wrapper */
    check_before("<div style=\"position:relative\">" BOX("position:relative;z-index:0", "#00f")
                     BOX("position:absolute;top:0;left:0", "#f00") "</div>", "", PH_BLUE, PH_RED);
}

EMB_TEST(viewport_abspos_orders_by_z_index_against_relative_boxes)
{
    /* z-index 5 abspos (containing block: the viewport) below a z-index 10 relative box, tree order either way */
    check_before(BOX("position:absolute;z-index:5", "#f00") BOX("position:relative;z-index:10", "#00f"), "", PH_RED,
                 PH_BLUE);
    check_before(BOX("position:relative;z-index:10", "#00f") BOX("position:absolute;z-index:5", "#f00"), "", PH_RED,
                 PH_BLUE);
    /* z-index 1 above z-index auto: the abspos auto box is step 6, the z-index 1 one step 7 */
    check_before(BOX("position:relative;z-index:1", "#00f") BOX("position:absolute", "#f00"), "", PH_RED, PH_BLUE);
    check_before(BOX("position:absolute", "#f00") BOX("position:relative;z-index:1", "#00f"), "", PH_RED, PH_BLUE);
    /* negative z-index abspos paints below the in-flow content of the document element */
    check_before(BOX("", "#00f") BOX("position:absolute;z-index:-1", "#f00"), "", PH_RED, PH_BLUE);
    /* equal z-index: tree order, whatever the fragment order */
    check_before(BOX("position:absolute;z-index:3", "#f00") BOX("position:relative;z-index:3", "#00f"), "", PH_RED,
                 PH_BLUE);
}

EMB_TEST(html_opacity_applies_to_the_whole_root_context)
{
    ph h = ph_make(BOX("position:absolute;z-index:2", "#f00"), "html{opacity:.5}");
    bool layered = false;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_LAYER_BEGIN || (h.dl.items.data[i].kind == EMBI_DI_FILL &&
                                                                 (h.dl.items.data[i].colors[0] >> 24) < 0xFF)) {
            layered = true;
        }
    }
    EMB_CHECK(layered);
    ph_free(&h);
}

EMB_TEST(positioned_flex_items_keep_order_modified_order)
{
    /* position:relative items with z-index auto, then z-index 0: order decides, not tree order */
    check_before("<div style=\"display:flex\">" BOX("position:relative;order:2", "#f00")
                     BOX("position:relative;order:1", "#00f") "</div>", "", PH_BLUE, PH_RED);
    check_before("<div style=\"display:flex\">" BOX("position:relative;z-index:0;order:2", "#f00")
                     BOX("position:relative;z-index:0;order:1", "#00f") "</div>", "", PH_BLUE, PH_RED);
    check_before("<div style=\"display:flex\">" BOX("position:relative;order:1", "#f00")
                     BOX("position:relative;order:1", "#00f") "</div>", "", PH_RED, PH_BLUE);
    /* a flex container nested in a block, with a positioned sibling after it */
    check_before("<div><div style=\"display:flex\">" BOX("position:relative;order:3", "#f00")
                     BOX("position:relative;order:-1", "#00f") "</div></div>", "", PH_BLUE, PH_RED);
}

EMB_TEST(positioned_grid_items_keep_order_modified_order)
{
    check_before("<div><div style=\"display:grid\">" BOX("position:relative;order:2", "#f00")
                     BOX("position:relative;order:1", "#00f") "</div></div>", "", PH_BLUE, PH_RED);
    check_before("<div><div style=\"display:grid\">" BOX("position:relative;z-index:0;order:2", "#f00")
                     BOX("position:relative;z-index:0;order:1", "#00f") "</div></div>", "", PH_BLUE, PH_RED);
}

EMB_TEST(many_positioned_siblings_are_sorted_in_document_order)
{
    char html[16384];
    size_t n = 0;
    n += (size_t)snprintf(html + n, sizeof html - n, "<div style=\"position:relative\">");
    for (int i = 0; i < 100; i++) {
        n += (size_t)snprintf(html + n, sizeof html - n,
                              "<div style=\"position:absolute;top:%dpx;left:0;width:5px;height:1px;background:#%s\"></div>", i,
                              i == 0 ? "f00" : i == 99 ? "00f" : "0f0");
    }
    snprintf(html + n, sizeof html - n, "</div>");
    check_before(html, "", PH_RED, PH_BLUE);
}

EMB_TEST(generated_boxes_order_around_the_dom_children)
{
    const char *kids_z0 = "<div class=\"w\" style=\"position:relative\">" BOX("position:relative;z-index:0", "#00f") "</div>";
    const char *kids_z1 = "<div class=\"w\" style=\"position:relative\">" BOX("position:relative;z-index:1", "#00f") "</div>";
    /* step 6: ::after follows a z-index 0 child, ::before precedes it */
    check_before(kids_z0, ".w::after{content:'';position:absolute;width:5px;height:5px;background:#f00}", PH_BLUE, PH_RED);
    check_before(kids_z0, ".w::before{content:'';position:absolute;width:5px;height:5px;background:#f00}", PH_RED, PH_BLUE);
    check_before(kids_z0, ".w::after{content:'';display:block;position:relative;z-index:0;width:5px;height:5px;background:#f00}", PH_BLUE,
                 PH_RED);
    /* step 7: equal positive z-index keeps tree order, with the generated boxes at the ends */
    check_before(kids_z1, ".w::after{content:'';position:absolute;z-index:1;width:5px;height:5px;background:#f00}", PH_BLUE,
                 PH_RED);
    check_before(kids_z1, ".w::before{content:'';position:absolute;z-index:1;width:5px;height:5px;background:#f00}", PH_RED,
                 PH_BLUE);
    /* a flex container's pseudo items use their own order */
    check_before("<div class=\"w\" style=\"display:flex;position:relative\">" BOX("position:relative;z-index:0;order:1", "#00f")
                 "</div>",
                 ".w::before{content:'';position:relative;z-index:0;order:2;width:5px;height:5px;background:#f00}", PH_BLUE,
                 PH_RED);
}

EMB_TEST(nested_contexts_stay_inside_their_parents_range)
{
    ph h = ph_make("<div style=\"position:relative;z-index:1\"><div style=\"position:relative;z-index:2;background:#f00;height:5px\"></div>"
                   "<div style=\"position:relative;z-index:-1;background:#00f;height:5px\"></div></div>", "");
    /* the context of the z-index 1 box contains the ranges of its two children */
    bool nested = false;
    for (uint32_t i = 1; i < h.dl.ctxs.len; i++) {
        const embi_dctx *c = &h.dl.ctxs.data[i];
        if (c->parent != 0) {
            const embi_dctx *p = &h.dl.ctxs.data[c->parent];
            EMB_CHECK(c->first >= p->first && c->end <= p->end);
            nested = true;
        }
    }
    EMB_CHECK(nested);
    ph_free(&h);
}

EMB_TEST(float_paints_before_inline_block)
{
    /* a float is step 4 and an inline-block is an atomic pseudo context in the inline step 5 */
    ph h = ph_make("<div><span style=\"display:inline-block;background:#f00;width:5px;height:5px\"></span></div>"
                   "<div style=\"float:left;background:#00f;width:5px;height:5px\"></div>", "");
    int r = need_fill(&h.dl, PH_RED), b = need_fill(&h.dl, PH_BLUE);
    EMB_CHECK(b < r);
    ph_free(&h);
}

EMB_TEST(collapsed_table_borders_paint_after_cell_backgrounds)
{
    ph h = ph_make("<table style=\"border-collapse:collapse\"><tr><td style=\"background:#f00\">a</td></tr></table>",
                   "td{border:2px solid #00f;padding:0}");
    int r = need_fill(&h.dl, PH_RED);
    int b = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK(r < b);
    ph_free(&h);
}

EMB_TEST(nested_overflow_clips_with_radii_chain_to_their_parent)
{
    ph h = ph_make("<div style=\"overflow:hidden;border-radius:10px;width:40px;height:40px\">"
                   "<div style=\"overflow:hidden;border-radius:4px;width:20px;height:20px;padding:0\">"
                   "<div style=\"height:50px;background:#f00\"></div></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    uint32_t c = h.dl.items.data[i].clip;
    EMB_REQUIRE(c != 0);
    const embi_dclip *inner = &h.dl.clips.data[c];
    EMB_REQUIRE(inner->parent != 0);
    const embi_dclip *outer = &h.dl.clips.data[inner->parent];
    /* the inner clip has the 4 px radius, the outer one the 10 px radius */
    EMB_CHECK(fabsf(inner->radii[0] - 4.0f) < 0.001f);
    EMB_CHECK(fabsf(outer->radii[0] - 10.0f) < 0.001f);
    EMB_CHECK(outer->rect[2] == 40.0f);
    ph_free(&h);
}

EMB_TEST(clip_rect_on_an_absolute_box_clips_it)
{
    ph h = ph_make("<div style=\"position:absolute;clip:rect(0,5px,5px,0);width:10px;height:10px;background:#f00\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    uint32_t c = h.dl.items.data[i].clip;
    EMB_REQUIRE(c != 0);
    const embi_dclip *cl = &h.dl.clips.data[c];
    EMB_CHECK(fabsf(cl->rect[2] - 5.0f) < 0.001f && fabsf(cl->rect[3] - 5.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(clip_rect_with_auto_edges_keeps_the_box_edge)
{
    ph h = ph_make("<div style=\"position:absolute;clip:rect(2px,auto,auto,3px);width:10px;height:10px;background:#f00\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const embi_dclip *cl = &h.dl.clips.data[h.dl.items.data[i].clip];
    /* left 3, top 2, right is the box's width (10), bottom its height (10) */
    EMB_CHECK(fabsf(cl->rect[0] - 3.0f) < 0.001f && fabsf(cl->rect[1] - 2.0f) < 0.001f);
    EMB_CHECK(fabsf(cl->rect[2] - 7.0f) < 0.001f && fabsf(cl->rect[3] - 8.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(contain_paint_clips_to_the_padding_box)
{
    ph h = ph_make("<div style=\"contain:paint;width:10px;height:10px;padding:2px\">"
                   "<div style=\"height:50px;background:#f00\"></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    uint32_t c = h.dl.items.data[i].clip;
    EMB_REQUIRE(c != 0);
    const embi_dclip *cl = &h.dl.clips.data[c];
    EMB_CHECK(cl->rect[2] == 14.0f && cl->rect[3] == 14.0f);
    ph_free(&h);
}

EMB_TEST(overflow_clip_margin_grows_an_overflow_clip)
{
    ph h = ph_make("<div style=\"overflow:clip;overflow-clip-margin:3px;width:10px;height:10px\">"
                   "<div style=\"height:50px;background:#f00\"></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const embi_dclip *cl = &h.dl.clips.data[h.dl.items.data[i].clip];
    EMB_CHECK(fabsf(cl->rect[0] + 3.0f) < 0.001f && fabsf(cl->rect[2] - 16.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(overflow_clip_margin_does_not_apply_to_hidden)
{
    ph h = ph_make("<div style=\"overflow:hidden;overflow-clip-margin:3px;width:10px;height:10px\">"
                   "<div style=\"height:50px;background:#f00\"></div></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const embi_dclip *cl = &h.dl.clips.data[h.dl.items.data[i].clip];
    EMB_CHECK(cl->rect[2] == 10.0f);
    ph_free(&h);
}

/* ------------------------------------------------------------------ transforms: 3D flattening */

EMB_TEST(matrix3d_with_an_affine_projection_is_exact)
{
    ph h = ph_make("<div style=\"transform:matrix3d(1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,0,1);background:#f00;width:4px;height:4px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    EMB_CHECK(m[0] == 1.0f && m[3] == 1.0f && m[1] == 0.0f && m[2] == 0.0f);
    EMB_CHECK(fabsf(m[4] - 10.0f) < 0.001f && fabsf(m[5] - 20.0f) < 0.001f);
    ph_free(&h);
}

EMB_TEST(perspective_alone_is_the_identity_on_the_plane)
{
    /* perspective changes only points that leave the plane, so the 2D map of the plane is the identity */
    ph h = ph_make("<div style=\"transform:perspective(100px);background:#f00;width:4px;height:4px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    EMB_CHECK(fabsf(m[0] - 1.0f) < 1e-5f && fabsf(m[3] - 1.0f) < 1e-5f);
    EMB_CHECK(fabsf(m[1]) < 1e-5f && fabsf(m[2]) < 1e-5f && fabsf(m[4]) < 1e-5f && fabsf(m[5]) < 1e-5f);
    ph_free(&h);
}

EMB_TEST(rotate_x_flattens_to_its_affine_part)
{
    /* rotateX(90deg) squashes the plane's y axis to 0 in 2D (its z part is dropped): d = cos 90 = 0, a = 1 */
    ph h = ph_make("<div style=\"transform:rotateX(90deg);background:#f00;width:4px;height:4px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    EMB_CHECK(fabsf(m[0] - 1.0f) < 1e-4f);
    EMB_CHECK(fabsf(m[3]) < 1e-4f);
    ph_free(&h);
}

EMB_TEST(rotate3d_about_z_is_the_2d_rotation)
{
    ph h = ph_make("<div style=\"transform:rotate3d(0,0,1,90deg);background:#f00;width:4px;height:4px\"></div>", "");
    int i = need_fill(&h.dl, PH_RED);
    const float *m = h.dl.xforms.data[h.dl.items.data[i].xform].m;
    EMB_CHECK(fabsf(m[0]) < 1e-4f && fabsf(m[1] - 1.0f) < 1e-4f && fabsf(m[2] + 1.0f) < 1e-4f);
    ph_free(&h);
}

/* ------------------------------------------------------------------ reuse and leaks */

EMB_TEST(repeated_builds_on_one_list_leave_no_live_memory)
{
    /* a warm build first (the process caches), then repeated builds into the same list: the same items every time,
     * and the live bytes stay at the baseline (the reset path keeps the arrays) */
    ph h = ph_make("<div style=\"opacity:.5\"><div style=\"background:#f00;height:5px\"></div>"
                   "<div style=\"position:relative;z-index:1;background:#00f;height:5px\"></div></div>", "");
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_paint_document(&h.dl, h.eng, h.doc, &cfg);
    uint32_t items = h.dl.items.len, ctxs = h.dl.ctxs.len;
    size_t before = embi_mem_live();
    for (int r = 0; r < 20; r++) {
        embi_paint_document(&h.dl, h.eng, h.doc, &cfg);
        EMB_CHECK_EQ_INT(h.dl.items.len, items);
        EMB_CHECK_EQ_INT(h.dl.ctxs.len, ctxs);
    }
    EMB_CHECK(embi_mem_live() == before);
    ph_free(&h);
}

EMB_TEST(paint_order_builds_leave_no_live_memory)
{
    static const char *docs[] = {
        "<div style=\"display:flex\"><div style=\"order:2;background:#f00\"></div><div style=\"order:1\"></div></div>",
        "<div style=\"opacity:.5\"><div style=\"position:relative;z-index:1;background:#f00\"></div></div>",
        "<div style=\"overflow:hidden;border-radius:4px;width:4px;height:4px\"><div style=\"height:9px;background:#f00\"></div></div>",
        "<ul style=\"list-style-type:circle\"><li>one</li></ul>",
        "<div style=\"background-image:linear-gradient(in oklab to right,#f00 25%,#00f)\"></div>",
    };
    {
        ph warm = ph_make("<div>warm</div>", "");
        ph_free(&warm);
    }
    size_t before = embi_mem_live();
    for (uint32_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        ph h = ph_make(docs[i], "");
        ph_free(&h);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST_MAIN()

