/*
 * R3b compositing groups in the display list: mix-blend-mode, isolation, clip-path and mask-image as
 * EMBI_LS_COMPOSITE and EMBI_LS_MASK groups, the retroactive isolated group of a context that holds a blend, the clip-path
 * outlines (rrect, polygon, path), the fx_items rule and the item hash.
 */
#include "paint_harness.h"

#include "paint/layer.h"

#include <math.h>

#define BOX "position:absolute;left:20px;top:30px;width:100px;height:50px;"

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* The index of the n-th LAYER_BEGIN with the given sub (n from 0), or -1. */
static int begin_sub(const embi_dl *d, uint8_t sub, uint32_t n)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == sub) {
            if (n == 0) {
                return (int)i;
            }
            n--;
        }
    }
    return -1;
}

EMB_TEST(a_page_without_composite_effects_has_no_composite_group)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00\"></div>", "");
    EMB_CHECK_EQ_INT(begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0), -1);
    EMB_CHECK_EQ_INT(h.dl.cpaths.len, 0);
    EMB_CHECK_EQ_INT(h.dl.curves.len, 0);
    EMB_CHECK_EQ_INT(h.dl.fx_items, 0);
    ph_free(&h);
}

EMB_TEST(mix_blend_mode_is_a_composite_group_with_the_mode)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;mix-blend-mode:multiply\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_ditem *it = &h.dl.items.data[b];
    EMB_CHECK_EQ_INT(it->colors[0], EMBI_BM_MULTIPLY);
    EMB_CHECK(it->aux > (uint32_t)b && h.dl.items.data[it->aux].kind == EMBI_DI_LAYER_END);
    EMB_CHECK_EQ_INT(h.dl.items.data[it->aux].aux, (uint32_t)b);
    EMB_CHECK(h.dl.fx_items > 0);
    ph_free(&h);
}

EMB_TEST(every_blend_keyword_maps_to_its_own_mode)
{
    static const char *const names[] = {"multiply",   "screen",     "overlay",     "darken",     "lighten",
                                        "color-dodge", "color-burn", "hard-light",  "soft-light", "difference",
                                        "exclusion",   "hue",        "saturation",  "color",      "luminosity",
                                        "plus-lighter"};
    uint32_t seen[32] = {0};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char html[256];
        SDL_snprintf(html, sizeof html, "<div style=\"" BOX "background:#f00;mix-blend-mode:%s\"></div>", names[i]);
        ph h = ph_make(html, "");
        int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
        EMB_REQUIRE(b >= 0);
        uint32_t m = h.dl.items.data[b].colors[0];
        EMB_CHECK(m != EMBI_BM_NORMAL && m < 32);
        EMB_CHECK_EQ_INT(seen[m], 0);
        seen[m] = 1;
        ph_free(&h);
    }
}

EMB_TEST(a_blend_inside_a_context_isolates_the_context)
{
    /* The wrapper is a stacking context (z-index) without a group of its own: the blend inside makes it an isolated group. */
    ph h = ph_make("<div style=\"position:relative;z-index:1\"><div style=\"" BOX
                   "background:#f00;mix-blend-mode:difference\"></div></div>",
                   "");
    int iso = -1, blend = -1;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE) {
            if ((it->flags & EMBI_DF_ISOLATE) != 0 && iso < 0) {
                iso = (int)i;
            } else if (it->colors[0] != EMBI_BM_NORMAL && blend < 0) {
                blend = (int)i;
            }
        }
    }
    EMB_REQUIRE(iso >= 0 && blend >= 0);
    EMB_CHECK(iso < blend);
    EMB_CHECK(h.dl.items.data[iso].aux > h.dl.items.data[blend].aux);
    EMB_CHECK_EQ_INT(h.dl.items.data[h.dl.items.data[iso].aux].aux, (uint32_t)iso);
    ph_free(&h);
}

EMB_TEST(the_isolation_group_keeps_the_list_consistent)
{
    ph h = ph_make("<div style=\"position:relative;z-index:1\"><div style=\"" BOX
                   "background:#f00;mix-blend-mode:screen\"></div></div><div style=\"position:absolute;left:0;top:0;width:5px;"
                   "height:5px;background:#00f\"></div>",
                   "");
    /* Every begin and end points at its partner, and the groups nest. */
    uint32_t depth = 0;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN) {
            EMB_REQUIRE(it->aux > i && it->aux < h.dl.items.len);
            EMB_CHECK(h.dl.items.data[it->aux].kind == EMBI_DI_LAYER_END);
            EMB_CHECK_EQ_INT(h.dl.items.data[it->aux].aux, i);
            depth++;
        } else if (it->kind == EMBI_DI_LAYER_END) {
            EMB_REQUIRE(depth > 0);
            depth--;
        }
    }
    EMB_CHECK_EQ_INT(depth, 0);
    /* The contexts stay inside the list. */
    for (uint32_t i = 0; i < h.dl.ctxs.len; i++) {
        EMB_CHECK(h.dl.ctxs.data[i].first <= h.dl.ctxs.data[i].end && h.dl.ctxs.data[i].end <= h.dl.items.len);
    }
    ph_free(&h);
}

EMB_TEST(isolation_without_a_blend_inside_adds_no_group)
{
    ph h = ph_make("<div style=\"isolation:isolate\"><div style=\"" BOX "background:#f00\"></div></div>", "");
    EMB_CHECK_EQ_INT(begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0), -1);
    ph_free(&h);
}

EMB_TEST(clip_path_inset_is_a_rounded_rect_outline)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;clip-path:inset(5px 10px round 4px)\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    uint32_t ci = h.dl.items.data[b].colors[1];
    EMB_REQUIRE(ci >= 1 && ci - 1 < h.dl.cpaths.len);
    const embi_dcpath *cp = &h.dl.cpaths.data[ci - 1];
    EMB_CHECK_EQ_INT(cp->kind, EMBI_CP_RRECT);
    EMB_CHECK(near(cp->rect[0], 30) && near(cp->rect[1], 35) && near(cp->rect[2], 80) && near(cp->rect[3], 40));
    EMB_CHECK(near(cp->radii[0], 4) && near(cp->radii[2], 4));
    ph_free(&h);
}

EMB_TEST(clip_path_circle_is_a_circle_of_the_reference_box)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;clip-path:circle(20px at 50px 25px)\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_dcpath *cp = &h.dl.cpaths.data[h.dl.items.data[b].colors[1] - 1];
    /* A circle is a rounded rect of 40 x 40 with radii 20 whose corners are circular. */
    EMB_CHECK_EQ_INT(cp->kind, EMBI_CP_RRECT);
    EMB_CHECK(near(cp->rect[0], 50) && near(cp->rect[1], 35) && near(cp->rect[2], 40) && near(cp->rect[3], 40));
    EMB_CHECK(near(cp->radii[0], 20) && near(cp->radii[1], 20) && near(cp->radii[2], 20) && near(cp->radii[3], 20));
    ph_free(&h);
}

EMB_TEST(clip_path_polygon_is_a_path_with_bounds)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;clip-path:polygon(0 0, 100% 0, 50% 100%)\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_dcpath *cp = &h.dl.cpaths.data[h.dl.items.data[b].colors[1] - 1];
    EMB_CHECK_EQ_INT(cp->kind, EMBI_CP_PATH);
    EMB_CHECK_EQ_INT(cp->n, 3);
    EMB_CHECK(cp->first + cp->n <= h.dl.curves.len);
    EMB_CHECK(near(cp->bounds[0], 20) && near(cp->bounds[1], 30) && near(cp->bounds[2], 120) && near(cp->bounds[3], 80));
    ph_free(&h);
}

EMB_TEST(clip_path_path_function_parses_svg_data)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;clip-path:path('M0 0 H50 V50 Z')\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_dcpath *cp = &h.dl.cpaths.data[h.dl.items.data[b].colors[1] - 1];
    EMB_CHECK_EQ_INT(cp->kind, EMBI_CP_PATH);
    EMB_CHECK(cp->n >= 3);
    /* The path is relative to the reference box origin (20, 30). */
    EMB_CHECK(near(cp->bounds[0], 20) && near(cp->bounds[1], 30) && near(cp->bounds[2], 70) && near(cp->bounds[3], 80));
    ph_free(&h);
}

EMB_TEST(clip_path_with_no_area_is_empty)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;clip-path:inset(0 100% 0 0)\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_dcpath *cp = &h.dl.cpaths.data[h.dl.items.data[b].colors[1] - 1];
    EMB_CHECK(cp->kind == EMBI_CP_EMPTY || (cp->kind == EMBI_CP_RRECT && !(cp->rect[2] > 0.0f)));
    ph_free(&h);
}

EMB_TEST(geometry_box_selects_the_reference_box)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;padding:10px;border:2px solid #000;clip-path:content-box\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    const embi_dcpath *cp = &h.dl.cpaths.data[h.dl.items.data[b].colors[1] - 1];
    EMB_CHECK_EQ_INT(cp->kind, EMBI_CP_RRECT);
    /* The content box: the border 2 and the padding 10 inside the border box at (20, 30). */
    EMB_CHECK(near(cp->rect[0], 32) && near(cp->rect[1], 42) && near(cp->rect[2], 100) && near(cp->rect[3], 50));
    ph_free(&h);
}

EMB_TEST(mask_image_makes_mask_groups_first_in_the_composite_group)
{
    ph h = ph_make("<div style=\"" BOX "background:#f00;mask-image:linear-gradient(#000, transparent)\"></div>", "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK_EQ_INT(h.dl.items.data[b].colors[2], 1);
    const embi_ditem *m = &h.dl.items.data[b + 1];
    EMB_CHECK(m->kind == EMBI_DI_LAYER_BEGIN && m->sub == EMBI_LS_MASK);
    EMB_CHECK(m->aux > (uint32_t)b + 1 && m->aux < h.dl.items.data[b].aux);
    EMB_CHECK_EQ_INT(m->colors[0], EMBI_MM_ALPHA);
    ph_free(&h);
}

EMB_TEST(two_mask_layers_make_two_mask_groups)
{
    ph h = ph_make("<div style=\"" BOX
                   "background:#f00;mask-image:linear-gradient(#000,#000),linear-gradient(#000,transparent);"
                   "mask-composite:exclude\"></div>",
                   "");
    int b = begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(b >= 0);
    EMB_CHECK_EQ_INT(h.dl.items.data[b].colors[2], 2);
    EMB_CHECK_EQ_INT(begin_sub(&h.dl, EMBI_LS_MASK, 1) >= 0, 1);
    ph_free(&h);
}

EMB_TEST(background_blend_mode_wraps_the_blended_layer)
{
    ph h = ph_make("<div style=\"" BOX
                   "background-color:#0f0;background-image:linear-gradient(#f00,#f00);background-blend-mode:multiply\"></div>",
                   "");
    /* The element's background is an isolated group with a blend group around the image layer. */
    EMB_CHECK(begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0) >= 0);
    bool blend = false;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        blend = blend || (it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE && it->colors[0] == EMBI_BM_MULTIPLY);
    }
    EMB_CHECK(blend);
    ph_free(&h);
}

EMB_TEST(the_item_hash_sees_the_blend_mode_and_the_clip_path)
{
    ph a = ph_make("<div style=\"" BOX "background:#f00;mix-blend-mode:multiply\"></div>", "");
    ph b = ph_make("<div style=\"" BOX "background:#f00;mix-blend-mode:screen\"></div>", "");
    int ia = begin_sub(&a.dl, EMBI_LS_COMPOSITE, 0), ib = begin_sub(&b.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(ia >= 0 && ib >= 0);
    EMB_CHECK(embi_layer_item_hash(&a.dl, (uint32_t)ia, 0, 0) != embi_layer_item_hash(&b.dl, (uint32_t)ib, 0, 0));
    ph c = ph_make("<div style=\"" BOX "background:#f00;clip-path:inset(1px)\"></div>", "");
    ph d = ph_make("<div style=\"" BOX "background:#f00;clip-path:inset(2px)\"></div>", "");
    int ic = begin_sub(&c.dl, EMBI_LS_COMPOSITE, 0), id = begin_sub(&d.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(ic >= 0 && id >= 0);
    EMB_CHECK(embi_layer_item_hash(&c.dl, (uint32_t)ic, 0, 0) != embi_layer_item_hash(&d.dl, (uint32_t)id, 0, 0));
    ph_free(&a);
    ph_free(&b);
    ph_free(&c);
    ph_free(&d);
}

EMB_TEST(will_change_on_a_backdrop_root_with_a_backdrop_filter_inside_isolates)
{
    ph h = ph_make("<div style=\"will-change:opacity;position:relative\"><div style=\"" BOX
                   "background:#f00;backdrop-filter:blur(2px)\"></div></div>",
                   "");
    EMB_CHECK(begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0) >= 0);
    ph_free(&h);
}

EMB_TEST(an_empty_composite_group_is_dropped)
{
    ph h = ph_make("<div style=\"" BOX "mix-blend-mode:multiply\"></div>", "");
    EMB_CHECK_EQ_INT(begin_sub(&h.dl, EMBI_LS_COMPOSITE, 0), -1);
    ph_free(&h);
}

EMB_TEST_MAIN()
