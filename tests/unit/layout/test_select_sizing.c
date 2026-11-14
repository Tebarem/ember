/*
 * A select (a replaced control sized by its options) with min-width: 0 shrinks below its longest option in a grid track
 * or a flex line, stretches to its grid area, and takes height / box-sizing like any box.
 */
#include "layout_harness.h"

#include <math.h>

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static bool border_rect(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

#define LONG_OPT "<option>Its material's (Generic (the mesh's own material has none))</option>"

EMB_TEST(select_min_width_zero_fits_a_one_fr_track)
{
    lh h = lh_make("<body><div id=g style='width:300px;display:grid;grid-template-columns:minmax(64px,30%) 1fr;gap:6px;"
                   "align-items:center'><span id=l>L</span><select id=s style='min-width:0;height:20px;padding:0 2px;"
                   "border:1px solid'>" LONG_OPT "</select></div></body>",
                   "body { margin: 0 }");
    embi_rectf g, s;
    EMB_REQUIRE(border_rect(&h, "g", &g));
    EMB_REQUIRE(border_rect(&h, "s", &s));
    EMB_CHECK(near(g.w, 300));
    EMB_CHECK(near(s.x, 96));
    EMB_CHECK(near(s.w, 204));
    EMB_CHECK(near(s.h, 20));
    lh_free(&h);
}

EMB_TEST(select_in_a_grid_item_in_an_auto_track_fits_the_container)
{
    /* The inspector: a block holding a grid with one auto column whose items are the label + select rows. The row's
     * min-content size is the sum of its tracks' bases (a 1fr track counts its minimum contribution, 0 here), so the
     * row fills the 300px column instead of growing to the long option's width. */
    lh h = lh_make("<body><div id=b style='width:300px;display:grid;gap:3px'><div id=p style='display:grid;"
                   "grid-template-columns:minmax(64px,30%) 1fr;gap:6px;align-items:center'><span id=l>L</span>"
                   "<select id=s style='min-width:0;height:20px'>" LONG_OPT "</select></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf p, s;
    EMB_REQUIRE(border_rect(&h, "p", &p));
    EMB_REQUIRE(border_rect(&h, "s", &s));
    EMB_CHECK(near(p.w, 300));
    EMB_CHECK(near(s.x, 96));
    EMB_CHECK(near(s.w, 204));
    lh_free(&h);
}

EMB_TEST(select_without_min_width_still_overflows_the_track)
{
    lh h = lh_make("<body><div id=g style='width:300px;display:grid;grid-template-columns:minmax(64px,30%) 1fr;gap:6px'>"
                   "<span>L</span><select id=s>" LONG_OPT "</select></div></body>",
                   "body { margin: 0 }");
    embi_rectf s;
    EMB_REQUIRE(border_rect(&h, "s", &s));
    EMB_CHECK(s.w > 204);
    lh_free(&h);
}

EMB_TEST(select_min_width_zero_shrinks_in_a_flex_row)
{
    lh h = lh_make("<body><div id=f style='width:300px;display:flex'><span id=l style='width:50px'>L</span>"
                   "<select id=s style='flex:1;min-width:0'>" LONG_OPT "</select></div></body>",
                   "body { margin: 0 }");
    embi_rectf s;
    EMB_REQUIRE(border_rect(&h, "s", &s));
    EMB_CHECK(near(s.x, 50));
    EMB_CHECK(near(s.w, 250));
    lh_free(&h);
}

EMB_TEST(select_height_follows_box_sizing)
{
    lh h = lh_make("<body><select id=a style='height:20px;border:1px solid;box-sizing:border-box'><option>x</option></select>"
                   "<select id=b style='height:20px;border:1px solid;padding:0;box-sizing:content-box'><option>x</option></select>"
                   "</body>",
                   "body { margin: 0 }");
    embi_rectf a, b;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(near(a.h, 20));
    EMB_CHECK(near(b.h, 22));
    lh_free(&h);
}

EMB_TEST(text_field_and_select_align_on_their_text_baseline)
{
    /* The line of a label holding an empty span and a 20px field: the field's baseline is its text's (centred in the
     * content box), not its bottom edge, so the line is as high as the field and the field sits at the top. */
    lh h = lh_make("<body><label id=l style='display:block;width:200px;font:13px Arial'><span></span>"
                   "<input id=i style='height:20px;padding:0 4px;border:1px solid;box-sizing:border-box'></label>"
                   "<label id=m style='display:block;width:200px;font:13px Arial'><span></span>"
                   "<select id=s style='height:20px;padding:0 2px;border:1px solid'><option>x</option></select></label>"
                   "</body>",
                   "body { margin: 0 }");
    embi_rectf l, i, m, s;
    EMB_REQUIRE(border_rect(&h, "l", &l));
    EMB_REQUIRE(border_rect(&h, "i", &i));
    EMB_REQUIRE(border_rect(&h, "m", &m));
    EMB_REQUIRE(border_rect(&h, "s", &s));
    EMB_CHECK(near(l.h, 20));
    EMB_CHECK(near(i.y - l.y, 0));
    EMB_CHECK(near(m.h, 20));
    EMB_CHECK(near(s.y - m.y, 0));
    lh_free(&h);
}

EMB_TEST(grid_and_flex_buttons_centre_their_item_once)
{
    /* A button that is a grid or flex container is not centred again as a block button is: a 16px item in a 24px
     * button with no border or padding sits at 4px. */
    lh h = lh_make("<body><button id=g style='width:30px;height:24px;display:grid;place-items:center;padding:0;border:0'>"
                   "<i id=gi style='display:block;width:16px;height:16px'></i></button>"
                   "<button id=f style='width:30px;height:24px;display:flex;align-items:center;padding:0;border:0'>"
                   "<i id=fi style='display:block;width:16px;height:16px'></i></button></body>",
                   "body { margin: 0 }");
    embi_rectf gi, g, fi, f;
    EMB_REQUIRE(border_rect(&h, "g", &g));
    EMB_REQUIRE(border_rect(&h, "gi", &gi));
    EMB_REQUIRE(border_rect(&h, "f", &f));
    EMB_REQUIRE(border_rect(&h, "fi", &fi));
    EMB_CHECK(near(gi.y - g.y, 4));
    EMB_CHECK(near(fi.y - f.y, 4));
    lh_free(&h);
}

EMB_TEST(grid_and_flex_buttons_keep_the_default_alignment)
{
    /* No place-items or align-items: the UA's align-content: center of a button is for block buttons only, so a grid or
     * flex button puts its content at the top (Chromium). A 40px high button with no border or padding. */
    lh h = lh_make("<body>"
                   "<button id=a style='width:40px;height:40px;display:grid;padding:0;border:0'>"
                   "<i id=ai style='display:block;width:16px;height:16px'></i></button>"
                   "<button id=b style='width:40px;height:40px;display:grid;padding:0;border:0'>"
                   "<span id=bs>t</span></button>"
                   "<button id=c style='width:40px;height:40px;display:grid;grid-template-rows:20px;padding:0;border:0'>"
                   "<i id=ci style='display:block;width:16px;height:16px'></i></button>"
                   "<button id=d style='width:40px;height:40px;display:flex;flex-wrap:wrap;padding:0;border:0'>"
                   "<i id=di style='display:block;width:16px;height:16px'></i></button>"
                   "</body>",
                   "body { margin: 0 }");
    embi_rectf r, k;
    EMB_REQUIRE(border_rect(&h, "a", &r));
    EMB_REQUIRE(border_rect(&h, "ai", &k));
    EMB_CHECK(near(k.y - r.y, 0));
    EMB_REQUIRE(border_rect(&h, "b", &r));
    EMB_REQUIRE(border_rect(&h, "bs", &k));
    EMB_CHECK(near(k.y - r.y, 0));
    EMB_CHECK(near(k.h, 40));
    EMB_REQUIRE(border_rect(&h, "c", &r));
    EMB_REQUIRE(border_rect(&h, "ci", &k));
    EMB_CHECK(near(k.y - r.y, 0));
    EMB_REQUIRE(border_rect(&h, "d", &r));
    EMB_REQUIRE(border_rect(&h, "di", &k));
    EMB_CHECK(near(k.y - r.y, 0));
    lh_free(&h);
}

EMB_TEST(select_with_auto_min_width_keeps_a_min_content_grid_wide)
{
    /* A min-content sized grid (an inline-grid in a zero-width block) with a 1fr column: a select with min-width: auto
     * contributes its whole width, so the grid stays wide; with min-width: 0 it contributes nothing. */
    lh h = lh_make("<body><div style='width:0'>"
                   "<div id=a style='display:inline-grid;grid-template-columns:minmax(64px,30%) 1fr;gap:6px'>"
                   "<span>L</span><select>" LONG_OPT "</select></div><br>"
                   "<div id=b style='display:inline-grid;grid-template-columns:minmax(64px,30%) 1fr;gap:6px'>"
                   "<span>L</span><select style='min-width:0'>" LONG_OPT "</select></div></div></body>",
                   "body { margin: 0 }");
    embi_rectf a, b;
    EMB_REQUIRE(border_rect(&h, "a", &a));
    EMB_REQUIRE(border_rect(&h, "b", &b));
    EMB_CHECK(a.w > 300);
    EMB_CHECK(b.w < a.w / 2);
    lh_free(&h);
}

EMB_TEST(control_baselines_with_padding_border_height_and_flex)
{
    /* The baseline is the text's, centred in the content box, wherever the box edges are: a field with padding and a
     * border, and a taller fixed-height one, sit at the top of their line; in a flex row with align-items: baseline a
     * 20px and a 30px field line up on the text, so the shorter one is moved down by half the difference. */
    lh h = lh_make("<body style='font:13px Arial'>"
                   "<label id=l style='display:block'><span></span><input id=i style='padding:3px 4px;border:2px solid;"
                   "box-sizing:border-box;height:28px'></label>"
                   "<label id=m style='display:block'><span></span><input id=j style='height:40px'></label>"
                   "<div id=f style='display:flex;align-items:baseline;width:400px'>"
                   "<input id=p style='height:20px;padding:0;border:1px solid;box-sizing:border-box'>"
                   "<input id=q style='height:30px;padding:0;border:1px solid;box-sizing:border-box'></div></body>",
                   "body { margin: 0 }");
    embi_rectf l, i, m, j, p, q;
    EMB_REQUIRE(border_rect(&h, "l", &l));
    EMB_REQUIRE(border_rect(&h, "i", &i));
    EMB_REQUIRE(border_rect(&h, "m", &m));
    EMB_REQUIRE(border_rect(&h, "j", &j));
    EMB_REQUIRE(border_rect(&h, "p", &p));
    EMB_REQUIRE(border_rect(&h, "q", &q));
    EMB_CHECK(near(l.h, 28));
    EMB_CHECK(near(i.y - l.y, 0));
    EMB_CHECK(near(m.h, 40));
    EMB_CHECK(near(j.y - m.y, 0));
    EMB_CHECK(near(p.y + p.h / 2, q.y + q.h / 2));
    EMB_CHECK(near(p.y - q.y, 5));
    lh_free(&h);
}

EMB_TEST_MAIN()
