/*
 * Damage rects of W4d (damage.c): single and several item changes, merging, the half-view bounding box, the 60%
 * full redraw, scale, forced and first frames, changes that are clipped away, and a real page where one box changes.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/damage.h"
#include "paint/layer.h"
#include "paint/paint_int.h"

#include <math.h>

/* A list of the given fills (x, y, w, h, color), in order. */
typedef struct fillspec {
    float r[4];
    uint32_t rgba;
} fillspec;

static void build(embi_dl *d, const fillspec *f, uint32_t n)
{
    memset(d, 0, sizeof *d);
    embi_dl_init(d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = d;
    float radii[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(embi_pb_fill(&p, f[i].r, radii, f[i].rgba, 0, 0));
    }
}

static bool rect_is(const embi_drect *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return r->x0 == x0 && r->y0 == y0 && r->x1 == x1 && r->y1 == y1;
}

EMB_TEST(identical_lists_damage_nothing)
{
    fillspec f[3] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_BLUE}, {{50, 50, 10, 10}, PH_GREEN}};
    embi_dl a, b;
    build(&a, f, 3);
    build(&b, f, 3);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(!dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 0);
    EMB_CHECK_EQ_INT((int)dmg.area, 0);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(one_changed_item_damages_its_bounds)
{
    fillspec f[3] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_BLUE}, {{50, 50, 10, 10}, PH_GREEN}};
    fillspec g[3] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_GREEN}, {{50, 50, 10, 10}, PH_GREEN}};
    embi_dl a, b;
    build(&a, f, 3);
    build(&b, g, 3);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 19, 19, 31, 31));
    EMB_CHECK_EQ_INT((int)dmg.area, 144);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(distant_changes_stay_separate_rects)
{
    fillspec f[3] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_BLUE}, {{50, 50, 10, 10}, PH_GREEN}};
    fillspec g[3] = {{{0, 0, 10, 10}, PH_BLUE}, {{20, 20, 10, 10}, PH_BLUE}, {{50, 50, 10, 10}, PH_RED}};
    embi_dl a, b;
    build(&a, f, 3);
    build(&b, g, 3);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_REQUIRE(dmg.rects.len == 2);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 0, 11, 11));
    EMB_CHECK(rect_is(&dmg.rects.data[1], 49, 49, 61, 61));
    EMB_CHECK(!dmg.full);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(overlapping_changes_merge)
{
    fillspec f[2] = {{{10, 10, 20, 20}, PH_RED}, {{20, 20, 20, 20}, PH_RED}};
    fillspec g[2] = {{{10, 10, 20, 20}, PH_BLUE}, {{20, 20, 20, 20}, PH_BLUE}};
    embi_dl a, b;
    build(&a, f, 2);
    build(&b, g, 2);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 9, 9, 41, 41));
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(damage_is_in_device_px_at_scale)
{
    fillspec f[1] = {{{10, 10, 10, 10}, PH_RED}};
    fillspec g[1] = {{{10, 10, 10, 10}, PH_BLUE}};
    embi_dl a, b;
    build(&a, f, 1);
    build(&b, g, 1);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 2.0f, 200, 200, false);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 19, 19, 41, 41));
    EMB_CHECK_EQ_INT((int)dmg.area, 484);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(first_frame_and_forced_frames_redraw_everything)
{
    fillspec f[1] = {{{10, 10, 10, 10}, PH_RED}};
    embi_dl a;
    build(&a, f, 1);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, nullptr, &a, 1.0f, 100, 60, false);
    EMB_CHECK(dmg.full);
    EMB_CHECK(dmg.any);
    EMB_CHECK_EQ_INT((int)dmg.area, 6000);
    /* A resize or a scale change forces a full redraw even when nothing differs. */
    embi_damage_compute(&dmg, &a, &a, 1.0f, 100, 60, true);
    EMB_CHECK(dmg.full);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 0);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
}

EMB_TEST(damage_over_sixty_percent_of_the_view_is_a_full_redraw)
{
    fillspec f[1] = {{{0, 0, 80, 80}, PH_RED}};
    fillspec g[1] = {{{0, 0, 80, 80}, PH_BLUE}};
    embi_dl a, b;
    build(&a, f, 1);
    build(&b, g, 1);
    embi_damage dmg;
    embi_damage_init(&dmg);
    /* 6400 of 10000 px: 64% of the view. */
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(dmg.full);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 0);

    /* A 40 px box that becomes 70 px: the damage is the 70 px box (both boxes overlap) and the 1 px margin every item
     * gets, 51% of the view. */
    fillspec s[1] = {{{0, 0, 40, 40}, PH_RED}};
    fillspec h[1] = {{{0, 0, 70, 70}, PH_BLUE}};
    embi_dl c, sm;
    build(&sm, s, 1);
    build(&c, h, 1);
    embi_damage_compute(&dmg, &sm, &c, 1.0f, 100, 100, false);
    EMB_CHECK(!dmg.full);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 0, 71, 71));
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
    embi_dl_free(&c);
    embi_dl_free(&sm);
}

EMB_TEST(rects_over_half_the_view_collapse_to_their_bounds)
{
    /* Two rects of 4500 px each, apart: 90% of the view in total, so they become one bounding box, which is the whole
     * view, and the frame is a full redraw. */
    fillspec f[2] = {{{0, 0, 45, 100}, PH_RED}, {{55, 0, 45, 100}, PH_RED}};
    fillspec g[2] = {{{0, 0, 45, 100}, PH_BLUE}, {{55, 0, 45, 100}, PH_BLUE}};
    embi_dl a, b;
    build(&a, f, 2);
    build(&b, g, 2);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(dmg.full);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(a_changed_item_clipped_away_damages_nothing)
{
    embi_dl a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    embi_dl_init(&a);
    embi_dl_init(&b);
    embi_pst pa, pb;
    memset(&pa, 0, sizeof pa);
    memset(&pb, 0, sizeof pb);
    pa.d = &a;
    pb.d = &b;
    float cr[4] = {50, 50, 10, 10};
    float zero[4] = {0, 0, 0, 0};
    float r[4] = {0, 0, 10, 10};
    float radii[4] = {0, 0, 0, 0};
    uint32_t clip_a = embi_pb_clip(&pa, cr, zero, 0, 0);
    uint32_t clip_b = embi_pb_clip(&pb, cr, zero, 0, 0);
    EMB_CHECK(embi_pb_fill(&pa, r, radii, PH_RED, 0, clip_a));
    EMB_CHECK(embi_pb_fill(&pb, r, radii, PH_BLUE, 0, clip_b));
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(!dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 0);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(page_box_color_change_damages_only_that_box)
{
    ph h1 = ph_make("<div style=\"height:10px;background:#f00\"></div>"
                    "<div style=\"height:10px;background:#00f\"></div>",
                    "");
    ph h2 = ph_make("<div style=\"height:10px;background:#f00\"></div>"
                    "<div style=\"height:10px;background:#0f0\"></div>",
                    "");
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &h1.dl, &h2.dl, 1.0f, 800, 600, false);
    EMB_CHECK(dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 9, 800, 21));
    embi_damage_free(&dmg);
    ph_free(&h1);
    ph_free(&h2);
}

EMB_TEST(damage_leaves_no_live_memory)
{
    size_t base = embi_mem_live();
    fillspec f[2] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_BLUE}};
    fillspec g[2] = {{{0, 0, 10, 10}, PH_GREEN}, {{20, 20, 10, 10}, PH_BLUE}};
    embi_dl a, b;
    build(&a, f, 2);
    build(&b, g, 2);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 1);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
    EMB_CHECK(embi_mem_live() == base);
}

/* The text item of a list (the first), and whether a damage rect covers its device px box. */
static bool text_item_covered(const embi_dl *d, const embi_damage *dmg, float scale)
{
    int t = ph_find(d, 0, EMBI_DI_TEXT);
    if (t < 0) {
        return false;
    }
    const float *r = d->items.data[t].rect;
    int32_t x0 = (int32_t)floorf(r[0] * scale), y0 = (int32_t)floorf(r[1] * scale);
    int32_t x1 = (int32_t)ceilf((r[0] + r[2]) * scale), y1 = (int32_t)ceilf((r[1] + r[3]) * scale);
    for (uint32_t i = 0; i < dmg->rects.len; i++) {
        const embi_drect *q = &dmg->rects.data[i];
        if (q->x0 <= x0 && q->y0 <= y0 && q->x1 >= x1 && q->y1 >= y1) {
            return true;
        }
    }
    return false;
}

EMB_TEST(text_change_with_equal_advances_damages_the_text_box)
{
    /* "10" and "11" in the same font, size and place: the item's rect, baseline and color are the same, only the glyphs
     * differ. The damage must still cover the text box (the item hash covers the shaped run's content). */
    ph h1 = ph_make("<div style=\"font-size:24px;font-family:monospace\">10</div>", "");
    ph h2 = ph_make("<div style=\"font-size:24px;font-family:monospace\">11</div>", "");
    int a = ph_find(&h1.dl, 0, EMBI_DI_TEXT), b = ph_find(&h2.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(a >= 0 && b >= 0);
    EMB_CHECK(h1.dl.items.data[a].rect[0] == h2.dl.items.data[b].rect[0]);
    EMB_CHECK(h1.dl.items.data[a].rect[2] == h2.dl.items.data[b].rect[2]);
    EMB_CHECK(embi_layer_item_hash(&h1.dl, (uint32_t)a, 0, 0) != embi_layer_item_hash(&h2.dl, (uint32_t)b, 0, 0));
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &h1.dl, &h2.dl, 1.0f, 800, 600, false);
    EMB_CHECK(dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_CHECK(text_item_covered(&h2.dl, &dmg, 1.0f));
    embi_damage_free(&dmg);

    /* The same text again is no change at all. */
    ph h3 = ph_make("<div style=\"font-size:24px;font-family:monospace\">11</div>", "");
    embi_damage_compute(&dmg, &h2.dl, &h3.dl, 1.0f, 800, 600, false);
    EMB_CHECK(!dmg.any);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 0);
    embi_damage_free(&dmg);
    ph_free(&h1);
    ph_free(&h2);
    ph_free(&h3);
}

EMB_TEST(a_kind_change_damages_the_item_and_counts_every_changed_item)
{
    /* prev: red, blue, green fills. cur: a triangle where red was (a kind change), then two different fills. Three items
     * differ, so changed is 3 (it used to stop at the first). */
    fillspec f[3] = {{{0, 0, 10, 10}, PH_RED}, {{20, 20, 10, 10}, PH_BLUE}, {{50, 50, 10, 10}, PH_GREEN}};
    embi_dl a, b;
    build(&a, f, 3);
    embi_dl_init(&b);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &b;
    float r0[4] = {0, 0, 10, 10}, r1[4] = {20, 20, 10, 10}, r2[4] = {50, 50, 10, 10};
    float radii[4] = {0, 0, 0, 0};
    embi_pb_triangle(&p, r0, EMBI_TRI_DOWN, PH_RED, 0, 0);
    EMB_CHECK(embi_pb_fill(&p, r1, radii, PH_RED, 0, 0));
    EMB_CHECK(embi_pb_fill(&p, r2, radii, PH_RED, 0, 0));
    EMB_REQUIRE(b.items.len == 3);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 100, 100, false);
    EMB_CHECK(dmg.any);
    EMB_CHECK_EQ_INT((int)dmg.changed, 3);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

EMB_TEST(an_item_inserted_or_removed_early_damages_only_that_item)
{
    fillspec f[4] = {{{0, 0, 10, 10}, PH_RED},
                     {{20, 0, 10, 10}, PH_BLUE},
                     {{40, 0, 10, 10}, PH_GREEN},
                     {{60, 0, 10, 10}, PH_RED}};
    fillspec g[5] = {{{0, 50, 10, 10}, PH_GREEN}, f[0], f[1], f[2], f[3]};
    embi_dl a, b;
    build(&a, f, 4);
    build(&b, g, 5);
    embi_damage dmg;
    embi_damage_init(&dmg);
    /* Inserted first: later items shift by one index in the group but are unchanged. */
    embi_damage_compute(&dmg, &a, &b, 1.0f, 200, 100, false);
    EMB_CHECK(dmg.any && !dmg.full);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 49, 11, 61));
    EMB_CHECK_EQ_INT((int)dmg.changed, 1);
    /* Removed first: the same rect. */
    embi_damage_compute(&dmg, &b, &a, 1.0f, 200, 100, false);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 49, 11, 61));
    EMB_CHECK_EQ_INT((int)dmg.changed, 1);
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
}

static void add_ctx(embi_dl *d, uint32_t first, uint32_t end)
{
    embi_dctx c = {first, end, 0};
    embi_vec_push(&d->ctxs, c);
}

EMB_TEST(a_context_inserted_early_does_not_damage_the_later_ones)
{
    fillspec f[4] = {{{0, 0, 10, 10}, PH_RED},
                     {{20, 0, 10, 10}, PH_BLUE},
                     {{40, 0, 10, 10}, PH_GREEN},
                     {{60, 0, 10, 10}, PH_RED}};
    fillspec g[5] = {{{0, 50, 10, 10}, PH_GREEN}, f[0], f[1], f[2], f[3]};
    embi_dl a, b;
    build(&a, f, 4);
    build(&b, g, 5);
    add_ctx(&a, 0, 4);
    add_ctx(&a, 0, 2);
    add_ctx(&a, 2, 4);
    add_ctx(&b, 0, 5);
    add_ctx(&b, 0, 1);
    add_ctx(&b, 1, 3);
    add_ctx(&b, 3, 5);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &a, &b, 1.0f, 200, 100, false);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 0, 49, 11, 61));
    /* A change inside the second of two kept contexts, after an inserted one, damages that item only. */
    fillspec h[5] = {g[0], f[0], f[1], f[2], {{60, 0, 10, 10}, PH_BLUE}};
    embi_dl c;
    build(&c, h, 5);
    add_ctx(&c, 0, 5);
    add_ctx(&c, 0, 1);
    add_ctx(&c, 1, 3);
    add_ctx(&c, 3, 5);
    embi_damage_compute(&dmg, &b, &c, 1.0f, 200, 100, false);
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(rect_is(&dmg.rects.data[0], 59, 0, 71, 11));
    embi_damage_free(&dmg);
    embi_dl_free(&a);
    embi_dl_free(&b);
    embi_dl_free(&c);
}

EMB_TEST(text_damage_covers_overhang_beyond_the_fragment_box)
{
    ph h1 = ph_make("<div style=\"font-size:24px;font-family:monospace;font-style:italic\">fj</div>", "");
    ph h2 = ph_make("<div style=\"font-size:24px;font-family:monospace;font-style:italic\">fl</div>", "");
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &h1.dl, &h2.dl, 1.0f, 800, 600, false);
    int t = ph_find(&h1.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(t >= 0 && dmg.rects.len >= 1);
    const float *r = h1.dl.items.data[t].rect;
    float pad = 0.25f * r[3];
    /* The rect covers the text box plus a quarter of its height on every side (clamped to the view). */
    const embi_drect *q = &dmg.rects.data[0];
    EMB_CHECK((float)q->x0 <= fmaxf(0.0f, r[0] - pad) && (float)q->y0 <= fmaxf(0.0f, r[1] - pad));
    EMB_CHECK((float)q->x1 >= r[0] + r[2] + pad && (float)q->y1 >= r[1] + r[3] + pad);
    embi_damage_free(&dmg);
    ph_free(&h1);
    ph_free(&h2);
}

static bool match_gif(void *user, embi_str url)
{
    (void)user;
    return url.len == 5 && memcmp(url.ptr, "a.gif", 5) == 0;
}

/* W6c: an animated image that changed its frame has the same item hash, so the compare finds nothing; its rects are added
 * by URL, and only for the items that draw that URL. */
EMB_TEST(animated_image_frame_damages_only_its_image_rects)
{
    embi_dl b;
    memset(&b, 0, sizeof b);
    embi_dl_init(&b);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &b;
    float radii[4] = {0, 0, 0, 0};
    float fill[4] = {0, 0, 100, 10};
    float r1[4] = {10, 20, 30, 40}, r2[4] = {60, 70, 20, 20}, r3[4] = {5, 5, 4, 4};
    float uv[4] = {0, 0, 1, 1};
    EMB_CHECK(embi_pb_fill(&p, fill, radii, PH_RED, 0, 0));
    embi_pb_image(&p, (embi_str){"a.gif", 5}, r1, uv, 0xFFFFFFFFu, false, 0, 0);
    embi_pb_image(&p, (embi_str){"b.png", 5}, r3, uv, 0xFFFFFFFFu, false, 0, 0);
    embi_pb_image(&p, (embi_str){"a.gif", 5}, r2, uv, 0xFFFFFFFFu, false, 0, 0);
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &b, &b, 1.0f, 200, 200, false);
    EMB_CHECK(!dmg.any);
    embi_damage_add_images(&dmg, &b, 1.0f, 200, 200, match_gif, nullptr);
    EMB_CHECK(dmg.any);
    EMB_CHECK(!dmg.full);
    EMB_CHECK_EQ_INT((int)dmg.rects.len, 2);
    bool one = false, two = false;
    for (uint32_t i = 0; i < dmg.rects.len; i++) {
        const embi_drect *r = &dmg.rects.data[i];
        one = one || (r->x0 <= 10 && r->y0 <= 20 && r->x1 >= 40 && r->y1 >= 60 && r->x1 <= 42 && r->y1 <= 62);
        two = two || (r->x0 <= 60 && r->y0 <= 70 && r->x1 >= 80 && r->y1 >= 90 && r->x1 <= 82 && r->y1 <= 92);
    }
    EMB_CHECK(one && two); /* b.png at (5, 5) is not damaged */
    EMB_CHECK(dmg.area < 3000);
    embi_damage_free(&dmg);
    embi_dl_free(&b);
}

/* TSHIMUAF: the list on screen is compared after the next layout, whose text-cache sweep frees the shaped runs the old
 * list points at (the text of a rebuilt tree that the new layout does not use). The old items' hashes must not read them:
 * under ASan this was a heap-use-after-free in embi_tshim_content_hash. */
EMB_TEST(previous_list_hashes_survive_the_text_cache_sweep)
{
    ph h = ph_make("<style>.a{font-size:20px}.b{font-size:31px}</style><div id=d class=a>sweep me <b>and me</b></div>"
                   "<div id=e>stays</div>", "");

    embi_dl_free(&h.dl);
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    embi_dl old, cur;
    embi_dl_init(&old);
    embi_dl_init(&cur);
    embi_paint_document(&old, h.eng, h.doc, &cfg);
    uint32_t n = old.items.len;
    EMB_REQUIRE(n > 0 && ph_find(&old, 0, EMBI_DI_TEXT) >= 0);
    uint64_t *before = SDL_malloc(n * sizeof *before);
    for (uint32_t i = 0; i < n; i++) {
        before[i] = embi_layer_item_hash(&old, i, 0.0f, 0.0f);
    }
    /* A new size: the tree is built again and the runs of the old size are swept at the end of the layout. */
    embi_element *el = ph_id(&h, "d");
    EMB_REQUIRE(el != nullptr);
    EMB_REQUIRE(embi_el_set_attribute(el, (embi_str){"class", 5}, embi_dstr_from_utf8(h.doc->slab, (embi_str){"b", 1})) ==
                EMBI_DOM_OK);
    embi_layout_invalidate_all(h.eng, h.doc); /* a full build, the only kind that sweeps (a kept tree keeps its runs) */
    (void)embi_layout_document(h.eng, h.doc);
    embi_paint_document(&cur, h.eng, h.doc, &cfg);
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(embi_layer_item_hash(&old, i, 0.0f, 0.0f) == before[i]);
    }
    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &old, &cur, 1.0f, 800, 600, false);
    EMB_CHECK(dmg.any);
    embi_damage_free(&dmg);
    SDL_free(before);
    embi_dl_free(&old);
    embi_dl_free(&cur);
    memset(&h.dl, 0, sizeof h.dl);
    embi_dl_init(&h.dl);
    ph_free(&h);
}

EMB_TEST_MAIN()
