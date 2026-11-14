/*
 * Scroll clamping, sticky offsets and the damage of a scroll, on real pages (W4d, scroll.c). The scroll offsets live in
 * the layout; the display list of the scrolled page moves the children, and the damage between the two lists covers
 * the visible part of the scroll container.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/damage.h"
#include "paint/paint_int.h"
#include "paint/scroll.h"

#include <math.h>

static embi_paint_cfg cfg800(void)
{
    embi_paint_cfg c = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    return c;
}

static embi_frag *frag_of(ph *h, const char *id)
{
    embi_element *el = ph_id(h, id);
    return el != nullptr && el->box != nullptr ? ((embi_box *)el->box)->frag : nullptr;
}

/* The first FILL with color rgba: its rect's y, or NaN. */
static float fill_y(const embi_dl *d, uint32_t rgba)
{
    int i = ph_fill_of(d, rgba);
    /* the scroll offset is a translation in the item's transform (scroll60) */
    return i < 0 ? NAN : d->items.data[i].rect[1] + (d->items.data[i].xform != 0 ? d->xforms.data[d->items.data[i].xform].m[5] : 0.0f);
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

EMB_TEST(scroll_range_is_the_overflow_beyond_the_container)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\">"
                   "<div style=\"height:200px\"></div></div>",
                   "");
    float mx = -1, my = -1;
    embi_scroll_max(frag_of(&h, "s"), &mx, &my);
    EMB_CHECK(near(mx, 0.0f));
    EMB_CHECK(near(my, 150.0f));
    ph_free(&h);
}

EMB_TEST(scroll_by_clamps_to_the_range_and_reports_changes)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\">"
                   "<div style=\"height:200px;background:#f00\"></div></div>",
                   "");
    embi_frag *s = frag_of(&h, "s");
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 1000.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sy(s), 150.0f));
    /* Already at the end: nothing changes. */
    EMB_CHECK(!embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 10.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sy(s), 150.0f));
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, -40.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sy(s), 110.0f));
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, -1000.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sy(s), 0.0f));
    /* Horizontal range is zero: a horizontal scroll does nothing. */
    EMB_CHECK(!embi_paint_scroll_by(h.eng, h.doc, s, 50.0f, 0.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sx(s), 0.0f));
    ph_free(&h);
}

EMB_TEST(scrolling_a_plain_box_does_nothing)
{
    ph h = ph_make("<div id=p style=\"height:10px\"><div style=\"height:200px\"></div></div>", "");
    EMB_CHECK(!embi_paint_scroll_by(h.eng, h.doc, frag_of(&h, "p"), 0.0f, 50.0f, false, 800.0f, 600.0f));
    ph_free(&h);
}

EMB_TEST(scroll_moves_content_and_damages_only_the_container)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\">"
                   "<div style=\"height:200px;background:#f00\"></div></div>",
                   "");
    embi_dl before;
    memset(&before, 0, sizeof before);
    embi_dl_init(&before);
    embi_paint_cfg cfg = cfg800();
    embi_paint_document(&before, h.eng, h.doc, &cfg);
    EMB_CHECK(near(fill_y(&before, PH_RED), 0.0f));

    embi_frag *s = frag_of(&h, "s");
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 30.0f, false, 800.0f, 600.0f));
    embi_dl after;
    memset(&after, 0, sizeof after);
    embi_dl_init(&after);
    embi_paint_document(&after, h.eng, h.doc, &cfg);
    EMB_CHECK(near(fill_y(&after, PH_RED), -30.0f));

    embi_damage dmg;
    embi_damage_init(&dmg);
    embi_damage_compute(&dmg, &before, &after, 1.0f, 800, 600, false);
    EMB_CHECK(dmg.any);
    EMB_CHECK(!dmg.full);
    /* The moved child is clipped to the container's 100 x 50 padding box, both before and after. */
    EMB_REQUIRE(dmg.rects.len == 1);
    EMB_CHECK(dmg.rects.data[0].x0 == 0 && dmg.rects.data[0].y0 == 0);
    EMB_CHECK(dmg.rects.data[0].x1 == 100 && dmg.rects.data[0].y1 == 50);

    embi_damage_free(&dmg);
    embi_dl_free(&before);
    embi_dl_free(&after);
    ph_free(&h);
}

EMB_TEST(sticky_child_follows_its_container_scroll)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;height:50px\">"
                   "<div id=k style=\"position:sticky;top:0;height:10px;background:#f00\"></div>"
                   "<div style=\"height:200px\"></div></div>",
                   "");
    embi_frag *s = frag_of(&h, "s");
    embi_frag *k = frag_of(&h, "k");
    float dx = -1, dy = -1;
    embi_scroll_sticky_offset(k, &dx, &dy);
    EMB_CHECK(near(dy, 0.0f));
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 30.0f, false, 800.0f, 600.0f));
    embi_scroll_sticky_offset(k, &dx, &dy);
    EMB_CHECK(near(dy, 30.0f));

    /* The painted position: the content moved up by the scroll (30) and the sticky box took the offset back, so it
     * sits at the top of the scrollport (page y 0), as in a browser. */
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_paint_cfg cfg = cfg800();
    embi_paint_document(&d, h.eng, h.doc, &cfg);
    EMB_CHECK(near(fill_y(&d, PH_RED), 0.0f));
    embi_dl_free(&d);
    ph_free(&h);
}

EMB_TEST(root_scroller_scrolls_the_viewport_range)
{
    ph h = ph_make("<div style=\"height:2000px\"></div>", "");
    embi_frag *root = (embi_frag *)embi_layout_root(h.eng, h.doc);
    EMB_REQUIRE(root != nullptr);
    float mx = -1, my = -1;
    embi_scroll_max_root(root, 800.0f, 600.0f, &mx, &my);
    EMB_CHECK(near(mx, 0.0f));
    EMB_CHECK(near(my, 1400.0f));
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, 5000.0f, true, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sy(root), 1400.0f));
    EMB_CHECK(!embi_paint_scroll_by(h.eng, h.doc, root, 0.0f, 5000.0f, true, 800.0f, 600.0f));
    ph_free(&h);
}

EMB_TEST_MAIN()
