/*
 * Overlay scrollbars and scroll input of W4d (scroll.c): the scrollbar style from scrollbar-width and scrollbar-color, the
 * thumb and track items of a scroll container at its offsets and alpha, and non-finite scroll deltas.
 */
#include "paint_harness.h"

#include "paint/dl.h"
#include "paint/paint_int.h"
#include "paint/scroll.h"

#include <math.h>

static embi_frag *frag_of(ph *h, const char *id)
{
    embi_element *el = ph_id(h, id);
    return el != nullptr && el->box != nullptr ? ((embi_box *)el->box)->frag : nullptr;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* The alpha byte of an RGBA8 color. */
static uint32_t alpha_of(uint32_t rgba)
{
    return (rgba >> 24) & 0xFFu;
}

EMB_TEST(scrollbar_style_resolves_width_and_colors)
{
    embi_sb_style d = embi_sb_style_default();
    embi_sb_style a = embi_sb_style_resolve(EMBI_SBW_AUTO, false, 0, 0);
    EMB_CHECK(near(a.width, d.width));
    EMB_CHECK(a.thumb == d.thumb && a.track == d.track);
    EMB_CHECK(near(embi_sb_style_resolve(EMBI_SBW_THIN, false, 0, 0).width, 4.0f));
    EMB_CHECK(near(embi_sb_style_resolve(EMBI_SBW_NONE, true, 0xFF00FF00u, 0x80000000u).width, 0.0f));
    /* scrollbar-color: <thumb> <track> replaces the defaults; auto keeps them. */
    embi_sb_style c = embi_sb_style_resolve(EMBI_SBW_AUTO, true, 0xFF00FF00u, 0x80000000u);
    EMB_CHECK(c.thumb == 0xFF00FF00u);
    EMB_CHECK(c.track == 0x80000000u);
    EMB_CHECK(c.thumb != d.thumb);
}

EMB_TEST(thumb_and_track_items_of_a_vertical_scroller)
{
    /* A first page loads the process-wide caches (the default font); the baseline is taken after it. */
    ph warm = ph_make("<div>warm</div>", "");
    ph_free(&warm);
    size_t base = embi_mem_live();
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\">"
                   "<div style=\"height:200px\"></div></div>",
                   "");
    embi_frag *s = frag_of(&h, "s");
    EMB_REQUIRE(s != nullptr);
    embi_sb_style st = embi_sb_style_default();
    embi_dl d;
    embi_dl_init(&d);
    /* Track alpha 0 (the default): only the thumb. The thumb is 50 * 50 / 200 = 12.5 px, raised to the 16 px minimum,
     * at the top of the track. */
    uint32_t n = embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 1.0f);
    EMB_CHECK_EQ_INT((int)n, 1);
    EMB_REQUIRE(d.items.len == 1);
    const embi_ditem *it = &d.items.data[0];
    EMB_CHECK(it->kind == EMBI_DI_FILL);
    EMB_CHECK(near(it->rect[0], 92.0f) && near(it->rect[1], 0.0f));
    EMB_CHECK(near(it->rect[2], 8.0f) && near(it->rect[3], 16.0f));
    EMB_CHECK_EQ_INT((int)alpha_of(it->colors[0]), 0x66);

    /* At the end of the range the thumb ends at the bottom of the track: 50 - 16 = 34 px down. */
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 0.0f, 1000.0f, false, 800.0f, 600.0f));
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 1.0f), 1);
    EMB_REQUIRE(d.items.len == 1);
    EMB_CHECK(near(d.items.data[0].rect[1], 34.0f));

    /* The alpha from embi_sb_alpha scales the color: half of 0x66 is 0x33. Zero alpha draws nothing. */
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 0.5f), 1);
    EMB_CHECK_EQ_INT((int)alpha_of(d.items.data[0].colors[0]), 0x33);
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 0.0f), 0);
    EMB_CHECK_EQ_INT((int)d.items.len, 0);

    /* A track color with alpha adds the track item below the thumb. */
    embi_sb_style tr = embi_sb_style_resolve(EMBI_SBW_AUTO, true, 0xFF0000FFu, 0x40FFFFFFu);
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &tr, 1.0f), 2);
    EMB_REQUIRE(d.items.len == 2);
    EMB_CHECK(near(d.items.data[0].rect[1], 0.0f) && near(d.items.data[0].rect[3], 50.0f));
    EMB_CHECK(d.items.data[1].colors[0] == 0xFF0000FFu);

    /* scrollbar-width: none draws nothing. */
    embi_sb_style none = embi_sb_style_resolve(EMBI_SBW_NONE, false, 0, 0);
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &none, 1.0f), 0);
    embi_dl_free(&d);
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(horizontal_bar_on_a_wide_scroller_and_none_without_overflow)
{
    ph h = ph_make("<div id=w style=\"overflow:auto;width:50px;height:100px\">"
                   "<div style=\"width:200px;height:10px\"></div></div>"
                   "<div id=n style=\"overflow:auto;width:50px;height:50px\">"
                   "<div style=\"width:10px;height:10px\"></div></div>",
                   "");
    embi_sb_style st = embi_sb_style_default();
    embi_dl d;
    embi_dl_init(&d);
    embi_frag *w = frag_of(&h, "w");
    EMB_REQUIRE(w != nullptr);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, w, 0.0f, 0.0f, &st, 1.0f), 1);
    EMB_REQUIRE(d.items.len == 1);
    /* Horizontal: the bar runs along the block end (y = 100 - 8) and its thumb is 50 * 50 / 200 = 12.5 raised to 16 px. */
    EMB_CHECK(near(d.items.data[0].rect[0], 0.0f));
    EMB_CHECK(near(d.items.data[0].rect[1], 92.0f));
    EMB_CHECK(near(d.items.data[0].rect[2], 16.0f));
    EMB_CHECK(near(d.items.data[0].rect[3], 8.0f));

    /* A scroller that does not overflow gets no bar at all. */
    embi_frag *n = frag_of(&h, "n");
    EMB_REQUIRE(n != nullptr);
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, n, 0.0f, 0.0f, &st, 1.0f), 0);
    embi_dl_free(&d);
    ph_free(&h);
}

static embi_sb_style style_of_id(ph *h, const char *id)
{
    embi_element *el = ph_id(h, id);
    const embi_style *st = el != nullptr && el->box != nullptr ? ((const embi_box *)el->box)->style : nullptr;
    return embi_sb_style_from(st);
}

EMB_TEST(scrollbar_style_from_computed_css)
{
    embi_sb_style d = embi_sb_style_default();
    ph h = ph_make("<div id=a></div><div id=b></div><div id=c></div><div id=e></div><div id=f></div>"
                   "<div id=g dir=rtl></div>",
                   "#b{scrollbar-width:thin}"
                   "#c{scrollbar-width:none}"
                   "#e{scrollbar-color:#ff0000 rgba(0,0,255,0.5);scrollbar-width:auto}"
                   "#f{scrollbar-color:auto}"
                   "#g{direction:rtl}");
    embi_sb_style a = style_of_id(&h, "a");
    EMB_CHECK(near(a.width, d.width) && a.thumb == d.thumb && a.track == d.track && !a.vbar_left);
    EMB_CHECK(near(style_of_id(&h, "b").width, 4.0f));
    EMB_CHECK(near(style_of_id(&h, "c").width, 0.0f));
    embi_sb_style e = style_of_id(&h, "e");
    EMB_CHECK(near(e.width, 8.0f));
    EMB_CHECK(e.thumb == 0xFF0000FFu); /* r in the low byte */
    EMB_CHECK_EQ_INT((int)alpha_of(e.track), 128);
    EMB_CHECK((e.track & 0x00FFFFFFu) == 0x00FF0000u);
    embi_sb_style f = style_of_id(&h, "f");
    EMB_CHECK(f.thumb == d.thumb && f.track == d.track);
    EMB_CHECK(style_of_id(&h, "g").vbar_left);
    /* A null style gives the defaults. */
    EMB_CHECK(embi_sb_style_from(nullptr).thumb == d.thumb);
    ph_free(&h);
}

EMB_TEST(scrollbar_properties_reject_bad_values)
{
    ph h = ph_make("<div id=a></div><div id=b></div>",
                   "#a{scrollbar-width:thin;scrollbar-width:wide;scrollbar-color:red}"
                   "#b{scrollbar-color:red blue green}");
    /* Invalid declarations are dropped: the earlier valid width stays, the color stays auto. */
    embi_sb_style d = embi_sb_style_default();
    embi_sb_style a = style_of_id(&h, "a");
    EMB_CHECK(near(a.width, 4.0f));
    EMB_CHECK(a.thumb == d.thumb);
    EMB_CHECK(style_of_id(&h, "b").thumb == d.thumb);
    ph_free(&h);
}

EMB_TEST(both_bars_do_not_overlap_and_rtl_moves_the_vertical_one)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:60px\">"
                   "<div style=\"width:300px;height:300px\"></div></div>",
                   "");
    embi_frag *s = frag_of(&h, "s");
    EMB_REQUIRE(s != nullptr);
    embi_sb_style st = embi_sb_style_resolve(EMBI_SBW_AUTO, true, 0xFF0000FFu, 0x40FFFFFFu);
    embi_dl d;
    embi_dl_init(&d);
    /* ltr: track + thumb for each bar (4 items). The vertical track ends 8 px above the bottom, the horizontal one 8 px
     * before the right edge. */
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 1.0f), 4);
    EMB_REQUIRE(d.items.len == 4);
    EMB_CHECK(near(d.items.data[0].rect[0], 92.0f) && near(d.items.data[0].rect[1], 0.0f));
    EMB_CHECK(near(d.items.data[0].rect[3], 52.0f));
    EMB_CHECK(near(d.items.data[2].rect[0], 0.0f) && near(d.items.data[2].rect[1], 52.0f));
    EMB_CHECK(near(d.items.data[2].rect[2], 92.0f));
    /* No item of one bar intersects an item of the other. */
    for (int i = 0; i < 2; i++) {
        for (int j = 2; j < 4; j++) {
            const float *a = d.items.data[i].rect, *b = d.items.data[j].rect;
            bool hit = a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
            EMB_CHECK(!hit);
        }
    }
    /* The thumb of the vertical bar at the end of its range reaches the end of its (shortened) track. */
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, 1000.0f, 1000.0f, false, 800.0f, 600.0f));
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 1.0f), 4);
    EMB_CHECK(near(d.items.data[1].rect[1] + d.items.data[1].rect[3], 52.0f));
    EMB_CHECK(near(d.items.data[3].rect[0] + d.items.data[3].rect[2], 92.0f));

    /* rtl: the vertical bar is on the left edge and the horizontal track starts after it. */
    st.vbar_left = true;
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT((int)embi_sb_emit(&d, s, 0.0f, 0.0f, &st, 1.0f), 4);
    EMB_CHECK(near(d.items.data[0].rect[0], 0.0f));
    EMB_CHECK(near(d.items.data[2].rect[0], 8.0f) && near(d.items.data[2].rect[2], 92.0f));
    embi_dl_free(&d);
    ph_free(&h);
}

EMB_TEST(non_finite_scroll_deltas_do_not_store_nan)
{
    ph h = ph_make("<div id=s style=\"overflow:auto;width:100px;height:50px\">"
                   "<div style=\"height:200px\"></div></div>",
                   "");
    embi_frag *s = frag_of(&h, "s");
    EMB_REQUIRE(s != nullptr);
    /* NaN and infinite horizontal deltas scroll nothing horizontally; the vertical delta still applies. */
    EMB_CHECK(embi_paint_scroll_by(h.eng, h.doc, s, NAN, 10.0f, false, 800.0f, 600.0f));
    EMB_CHECK(near(embi_frag_sx(s), 0.0f));
    EMB_CHECK(near(embi_frag_sy(s), 10.0f));
    EMB_CHECK(!embi_paint_scroll_by(h.eng, h.doc, s, INFINITY, NAN, false, 800.0f, 600.0f));
    EMB_CHECK(!isnan(embi_frag_sx(s)) && !isnan(embi_frag_sy(s)));
    EMB_CHECK(near(embi_frag_sy(s), 10.0f));
    ph_free(&h);
}

EMB_TEST_MAIN()
