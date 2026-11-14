/*
 * R3a effects in the display list: box-shadow items (outer and inset, spread radii, order), filter lists as filtered
 * groups with their steps, drop-shadow, backdrop-filter items, blurred text-shadow groups, the group mode of a filtered
 * group, the damage rule for pages with filters, and that a page without effects stores nothing for them.
 */
#include "paint_harness.h"

#include "paint/damage.h"
#include "paint/layer.h"

#include <math.h>

#define BOX "position:absolute;left:20px;top:30px;width:100px;height:50px;"

static int nth_of(const embi_dl *d, uint8_t k, uint32_t n)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == k) {
            if (n == 0) {
                return (int)i;
            }
            n--;
        }
    }
    return -1;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

EMB_TEST(a_page_without_effects_stores_nothing_for_them)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00\">x</div>", "");
    EMB_CHECK_EQ_INT(h.dl.shadows.len, 0);
    EMB_CHECK_EQ_INT(h.dl.fx.len, 0);
    EMB_CHECK_EQ_INT(h.dl.fx_items, 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_SHADOW), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    ph_free(&h);
}

EMB_TEST(outer_shadow_is_drawn_before_the_background)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;box-shadow:6px 8px 10px 2px #00f\"></div>", "");
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    int f = ph_fill_of(&h.dl, PH_RED);
    EMB_REQUIRE(s >= 0 && f >= 0);
    EMB_CHECK(s < f);
    const embi_ditem *it = &h.dl.items.data[s];
    EMB_CHECK(it->sub == EMBI_SHADOW_OUTER);
    EMB_CHECK(it->colors[0] == PH_BLUE);
    const embi_dshadow *sh = &h.dl.shadows.data[it->aux];
    /* The box moved by (6, 8) and grown by the spread 2 on every side. */
    EMB_CHECK(near(sh->shape[0], 20 + 6 - 2) && near(sh->shape[1], 30 + 8 - 2));
    EMB_CHECK(near(sh->shape[2], 104) && near(sh->shape[3], 54));
    /* The blur radius 10 is a sigma of 5. */
    EMB_CHECK(near(sh->sigma, 5.0f));
    /* The border box is cut out. */
    EMB_CHECK(near(sh->clip[0], 20) && near(sh->clip[1], 30) && near(sh->clip[2], 100) && near(sh->clip[3], 50));
    /* The item covers the shape grown by 3.5 sigma plus a pixel. */
    EMB_CHECK(it->rect[0] < sh->shape[0] - 17.0f && it->rect[2] > sh->shape[2] + 34.0f);
    ph_free(&h);
}

EMB_TEST(first_box_shadow_is_on_top)
{
    ph h = ph_make("<div style=\"" BOX "box-shadow:1px 0 #f00, 2px 0 #0f0\"></div>", "");
    int a = nth_of(&h.dl, EMBI_DI_SHADOW, 0);
    int b = nth_of(&h.dl, EMBI_DI_SHADOW, 1);
    EMB_REQUIRE(a >= 0 && b >= 0);
    EMB_CHECK(h.dl.items.data[a].colors[0] == PH_GREEN);
    EMB_CHECK(h.dl.items.data[b].colors[0] == PH_RED);
    ph_free(&h);
}

EMB_TEST(spread_adjusts_the_radii_as_css_backgrounds_says)
{
    ph h = ph_make("<div style=\"" BOX "border-radius:10px 0 2px 30px;box-shadow:0 0 0 4px #000\"></div>", "");
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    EMB_REQUIRE(s >= 0);
    const embi_dshadow *sh = &h.dl.shadows.data[h.dl.items.data[s].aux];
    EMB_CHECK(near(sh->shape_radii[0], 14.0f));
    /* A square corner stays square. */
    EMB_CHECK(near(sh->shape_radii[1], 0.0f));
    /* A radius smaller than the spread: 2 + 4 * (1 + (0.5 - 1)^3) = 2 + 4 * 0.875 = 5.5. */
    EMB_CHECK(near(sh->shape_radii[2], 5.5f));
    EMB_CHECK(near(sh->shape_radii[3], 34.0f));
    /* The cut-out keeps the border box radii. */
    EMB_CHECK(near(sh->clip_radii[0], 10.0f) && near(sh->clip_radii[2], 2.0f));
    ph_free(&h);
}

EMB_TEST(negative_spread_shrinks_the_shape_and_its_radii)
{
    ph h = ph_make("<div style=\"" BOX "border-radius:10px;box-shadow:3px 3px 0 -4px #000\"></div>", "");
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    EMB_REQUIRE(s >= 0);
    const embi_dshadow *sh = &h.dl.shadows.data[h.dl.items.data[s].aux];
    EMB_CHECK(near(sh->shape[2], 92.0f) && near(sh->shape[3], 42.0f));
    EMB_CHECK(near(sh->shape_radii[0], 6.0f));
    ph_free(&h);
}

EMB_TEST(a_shadow_larger_than_nothing_is_dropped_when_the_spread_eats_the_box)
{
    ph h = ph_make("<div style=\"" BOX "box-shadow:0 0 0 -60px #000\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_SHADOW), 0);
    ph_free(&h);
}

EMB_TEST(transparent_shadows_are_dropped)
{
    ph h = ph_make("<div style=\"" BOX "box-shadow:5px 5px 4px rgba(0,0,0,0)\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_SHADOW), 0);
    ph_free(&h);
}

EMB_TEST(shadow_without_blur_has_sigma_zero_and_no_group)
{
    ph h = ph_make("<div style=\"" BOX "box-shadow:5px 5px #000\"></div>", "");
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    EMB_REQUIRE(s >= 0);
    EMB_CHECK(h.dl.shadows.data[h.dl.items.data[s].aux].sigma == 0.0f);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    ph_free(&h);
}

EMB_TEST(inset_shadow_is_between_the_background_and_the_border)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;border:4px solid #0f0;border-radius:12px;"
                   "box-shadow:inset 5px 6px 8px 1px #00f\"></div>",
                   "");
    int f = ph_fill_of(&h.dl, PH_RED);
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    int b = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(f >= 0 && s >= 0 && b >= 0);
    EMB_CHECK(f < s && s < b);
    const embi_ditem *it = &h.dl.items.data[s];
    EMB_CHECK(it->sub == EMBI_SHADOW_INSET);
    const embi_dshadow *sh = &h.dl.shadows.data[it->aux];
    /* The border box is 108 x 58 (content-box sizing), so the padding box is (24, 34, 100, 50). The hole moves by the
     * offset and shrinks by the spread. */
    EMB_CHECK(near(sh->clip[0], 24) && near(sh->clip[1], 34) && near(sh->clip[2], 100) && near(sh->clip[3], 50));
    EMB_CHECK(near(sh->shape[0], 24 + 5 + 1) && near(sh->shape[1], 34 + 6 + 1));
    EMB_CHECK(near(sh->shape[2], 98) && near(sh->shape[3], 48));
    /* The padding radii are 12 - 4 = 8; the hole's are smaller by the spread. */
    EMB_CHECK(near(sh->clip_radii[0], 8.0f) && near(sh->shape_radii[0], 7.0f));
    EMB_CHECK(near(sh->sigma, 4.0f));
    ph_free(&h);
}

EMB_TEST(inset_shadow_with_a_spread_that_fills_the_box_has_no_hole)
{
    ph h = ph_make("<div style=\"" BOX "box-shadow:inset 0 0 0 60px #000\"></div>", "");
    int s = ph_find(&h.dl, 0, EMBI_DI_SHADOW);
    EMB_REQUIRE(s >= 0);
    const embi_dshadow *sh = &h.dl.shadows.data[h.dl.items.data[s].aux];
    EMB_CHECK(sh->shape[2] == 0.0f && sh->shape[3] == 0.0f);
    ph_free(&h);
}

/* ------------------------------------------------------------------ filter */

static int begin_of(const embi_dl *d)
{
    return ph_find(d, 0, EMBI_DI_LAYER_BEGIN);
}

EMB_TEST(blur_filter_makes_a_filtered_group_even_with_one_item)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:blur(4px)\"></div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    const embi_ditem *it = &h.dl.items.data[b];
    EMB_CHECK(it->sub == EMBI_LAYER_FILTERED);
    EMB_CHECK_EQ_INT(it->aux3, 1);
    EMB_CHECK(h.dl.fx.data[it->aux2].kind == EMBI_FX_BLUR);
    EMB_CHECK(near(h.dl.fx.data[it->aux2].sigma, 4.0f));
    EMB_CHECK(near(it->f[0], 1.0f));
    EMB_CHECK(h.dl.items.data[it->aux].kind == EMBI_DI_LAYER_END);
    EMB_CHECK_EQ_INT(h.dl.fx_items, 1);
    EMB_CHECK_EQ_INT(embi_layer_group_mode(&h.dl, (uint32_t)b, it->aux), EMBI_GROUP_TARGET);
    ph_free(&h);
}

EMB_TEST(filter_and_opacity_share_one_group)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;opacity:.5;filter:grayscale(1)\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 1);
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK(near(h.dl.items.data[b].f[0], 0.5f));
    EMB_CHECK(h.dl.items.data[b].sub == EMBI_LAYER_FILTERED);
    ph_free(&h);
}

EMB_TEST(filter_none_and_url_filters_make_no_group)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:none\"></div>"
                   "<div style=\"" BOX "background:#0f0;filter:url(#nothing)\"></div>",
                   "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    EMB_CHECK_EQ_INT(h.dl.fx.len, 0);
    ph_free(&h);
}

EMB_TEST(color_functions_are_matrices_and_bounded_ones_merge)
{
    /* grayscale cannot leave 0..1, so the following sepia merges into it: one step. */
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:grayscale(.5) sepia(.3)\"></div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK_EQ_INT(h.dl.items.data[b].aux3, 1);
    EMB_CHECK(h.dl.fx.data[h.dl.items.data[b].aux2].kind == EMBI_FX_MATRIX);
    ph_free(&h);
    /* brightness above 1 can: the next function stays a step of its own. */
    ph g = ph_make("<div style=\"" BOX "background:#f00;filter:brightness(1.5) contrast(.5)\"></div>", "");
    b = begin_of(&g.dl);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK_EQ_INT(g.dl.items.data[b].aux3, 2);
    ph_free(&g);
}

EMB_TEST(invert_matrix_flips_the_color_channels)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:invert(1)\"></div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    const embi_dfx *x = &h.dl.fx.data[h.dl.items.data[b].aux2];
    EMB_CHECK(near(x->m[0], -1.0f) && near(x->m[4], 1.0f) && near(x->m[6], -1.0f) && near(x->m[9], 1.0f));
    EMB_CHECK(near(x->m[12], -1.0f) && near(x->m[14], 1.0f));
    EMB_CHECK(near(x->m[18], 1.0f) && near(x->m[19], 0.0f));
    ph_free(&h);
}

EMB_TEST(hue_rotate_zero_is_the_identity_and_drops_out)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:hue-rotate(0deg)\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    ph_free(&h);
}

EMB_TEST(hue_rotate_without_an_argument_is_the_identity)
{
    size_t live = embi_mem_live();
    {
        ph h = ph_make("<div style=\"" BOX "background:#f00;filter:hue-rotate()\"></div>", "");
        EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
        ph_free(&h);
    }
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(huge_blur_lengths_build_without_overflow_and_leak_nothing)
{
    size_t live = embi_mem_live();
    {
        ph h = ph_make("<div style=\"" BOX "background:#f00;filter:blur(1e10px)\"></div>"
                       "<div style=\"" BOX "background:#0f0;filter:drop-shadow(1e10px 1e10px 1e10px #000)\"></div>",
                       "");
        EMB_CHECK(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN) >= 1);
        ph_free(&h);
    }
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(drop_shadow_function_keeps_offset_blur_and_color)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:drop-shadow(10px 5px 4px #00f)\"></div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    const embi_dfx *x = &h.dl.fx.data[h.dl.items.data[b].aux2];
    EMB_CHECK(x->kind == EMBI_FX_SHADOW);
    EMB_CHECK(near(x->dx, 10.0f) && near(x->dy, 5.0f) && near(x->sigma, 4.0f));
    EMB_CHECK(x->color == PH_BLUE);
    ph_free(&h);
}

EMB_TEST(filter_chain_keeps_its_order)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;filter:blur(2px) invert(1) drop-shadow(1px 1px 0 #000)\"></div>",
                   "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    const embi_ditem *it = &h.dl.items.data[b];
    EMB_REQUIRE(it->aux3 == 3);
    EMB_CHECK(h.dl.fx.data[it->aux2].kind == EMBI_FX_BLUR);
    EMB_CHECK(h.dl.fx.data[it->aux2 + 1].kind == EMBI_FX_MATRIX);
    EMB_CHECK(h.dl.fx.data[it->aux2 + 2].kind == EMBI_FX_SHADOW);
    ph_free(&h);
}

EMB_TEST(filtered_group_records_the_transform_of_the_box)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;transform:scale(2);filter:blur(1px)\"></div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK(h.dl.items.data[b].xform != 0);
    ph_free(&h);
}

/* ------------------------------------------------------------------ backdrop-filter */

EMB_TEST(backdrop_filter_item_comes_before_the_box_paints)
{
    ph h = ph_make("<div style=\"" BOX "background:rgba(255,0,0,.5);border-radius:9px;backdrop-filter:blur(6px) saturate(2)\"></div>",
                   "");
    int k = ph_find(&h.dl, 0, EMBI_DI_BACKDROP);
    int f = ph_find(&h.dl, 0, EMBI_DI_FILL);
    EMB_REQUIRE(k >= 0 && f >= 0);
    EMB_CHECK(k < f);
    const embi_ditem *it = &h.dl.items.data[k];
    EMB_CHECK(near(it->rect[0], 20) && near(it->rect[2], 100) && near(it->rect[3], 50));
    EMB_CHECK(near(it->radii[0], 9.0f));
    EMB_CHECK_EQ_INT(it->aux3, 2);
    EMB_CHECK(h.dl.fx.data[it->aux2].kind == EMBI_FX_BLUR);
    EMB_CHECK(h.dl.fx.data[it->aux2 + 1].kind == EMBI_FX_MATRIX);
    EMB_CHECK_EQ_INT(h.dl.fx_items, 1);
    ph_free(&h);
}

/* ------------------------------------------------------------------ text-shadow */

EMB_TEST(blurred_text_shadow_is_a_filtered_group_of_the_shadow_text)
{
    ph h = ph_make("<div style=\"color:#000;text-shadow:3px 4px 6px #00f\">t</div>", "");
    int b = begin_of(&h.dl);
    EMB_REQUIRE(b >= 0);
    const embi_ditem *it = &h.dl.items.data[b];
    EMB_CHECK(it->sub == EMBI_LAYER_FILTERED);
    EMB_CHECK(near(h.dl.fx.data[it->aux2].sigma, 3.0f));
    /* One text item inside, in the shadow color; the plain text follows the group. */
    EMB_CHECK(h.dl.items.data[b + 1].kind == EMBI_DI_TEXT);
    EMB_CHECK(h.dl.items.data[b + 1].colors[0] == PH_BLUE);
    EMB_CHECK(h.dl.items.data[it->aux + 1].kind == EMBI_DI_TEXT);
    EMB_CHECK(h.dl.items.data[it->aux + 1].colors[0] == PH_BLACK);
    ph_free(&h);
}

EMB_TEST(unblurred_text_shadow_makes_no_group)
{
    ph h = ph_make("<div style=\"color:#000;text-shadow:3px 4px #00f\">t</div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN), 0);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 2);
    ph_free(&h);
}

/* ------------------------------------------------------------------ damage and hashing */

EMB_TEST(a_change_on_a_page_with_a_filter_redraws_everything)
{
    ph a = ph_make("<div style=\"" BOX "background:#f00;filter:blur(2px)\"></div><div style=\"position:absolute;left:300px;top:300px;width:20px;height:20px;background:#00f\"></div>", "");
    ph b = ph_make("<div style=\"" BOX "background:#f00;filter:blur(2px)\"></div><div style=\"position:absolute;left:300px;top:300px;width:20px;height:20px;background:#0f0\"></div>", "");
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a.dl, &b.dl, 1.0f, 800, 600, false);
    EMB_CHECK(dm.any && dm.full);
    /* Identical lists change nothing. */
    embi_damage_compute(&dm, &a.dl, &a.dl, 1.0f, 800, 600, false);
    EMB_CHECK(!dm.any);
    embi_damage_free(&dm);
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(a_change_on_a_page_without_filters_stays_partial)
{
    ph a = ph_make("<div style=\"" BOX "background:#f00\"></div><div style=\"position:absolute;left:300px;top:300px;width:20px;height:20px;background:#00f\"></div>", "");
    ph b = ph_make("<div style=\"" BOX "background:#f00\"></div><div style=\"position:absolute;left:300px;top:300px;width:20px;height:20px;background:#0f0\"></div>", "");
    embi_damage dm;
    embi_damage_init(&dm);
    embi_damage_compute(&dm, &a.dl, &b.dl, 1.0f, 800, 600, false);
    EMB_CHECK(dm.any && !dm.full);
    embi_damage_free(&dm);
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(item_hashes_see_the_filter_and_the_shadow)
{
    ph a = ph_make("<div style=\"" BOX "background:#f00;filter:blur(2px);box-shadow:3px 3px 4px #000\"></div>", "");
    ph b = ph_make("<div style=\"" BOX "background:#f00;filter:blur(3px);box-shadow:3px 3px 5px #000\"></div>", "");
    int ba = begin_of(&a.dl), bb = begin_of(&b.dl);
    EMB_REQUIRE(ba >= 0 && bb >= 0);
    EMB_CHECK(embi_layer_item_hash(&a.dl, (uint32_t)ba, 0, 0) != embi_layer_item_hash(&b.dl, (uint32_t)bb, 0, 0));
    int sa = ph_find(&a.dl, 0, EMBI_DI_SHADOW), sb = ph_find(&b.dl, 0, EMBI_DI_SHADOW);
    EMB_REQUIRE(sa >= 0 && sb >= 0);
    EMB_CHECK(embi_layer_item_hash(&a.dl, (uint32_t)sa, 0, 0) != embi_layer_item_hash(&b.dl, (uint32_t)sb, 0, 0));
    ph_free(&a);
    ph_free(&b);
}

EMB_TEST(hidden_boxes_cast_no_shadow)
{
    ph h = ph_make("<div style=\"" BOX "visibility:hidden;box-shadow:3px 3px 4px #000\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_SHADOW), 0);
    ph_free(&h);
}

EMB_TEST_MAIN()
