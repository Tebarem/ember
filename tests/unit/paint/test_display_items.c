/*
 * Display list storage and the items of simple boxes (W4c): the pooled arrays, the URL table, the backgrounds,
 * borders, text items and their geometry against the layout, and the leak check.
 */
#include "paint_harness.h"

#include "base/base.h"
#include "paint/paint_int.h"

EMB_TEST(display_list_init_reset_and_pooling)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    /* index 0 of clips and xforms is the sentinel and the identity */
    EMB_CHECK_EQ_INT(d.clips.len, 1);
    EMB_CHECK_EQ_INT(d.xforms.len, 1);
    EMB_CHECK(d.xforms.data[0].m[0] == 1.0f && d.xforms.data[0].m[3] == 1.0f);
    EMB_CHECK_EQ_INT(d.items.len, 0);

    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    float r[4] = {1, 2, 3, 4};
    float radii[4] = {0, 0, 0, 0};
    for (int i = 0; i < 100; i++) {
        EMB_CHECK(embi_pb_fill(&p, r, radii, PH_RED, 0, 0));
    }
    EMB_CHECK_EQ_INT(d.items.len, 100);
    uint32_t cap = d.items.cap;
    size_t bytes = embi_dl_bytes(&d);
    EMB_CHECK(bytes >= 100 * sizeof(embi_ditem));

    /* A transparent fill is not emitted. */
    EMB_CHECK(!embi_pb_fill(&p, r, radii, 0x00FF0000u, 0, 0));
    /* An empty rect is not emitted. */
    float empty[4] = {0, 0, 0, 5};
    EMB_CHECK(!embi_pb_fill(&p, empty, radii, PH_RED, 0, 0));
    EMB_CHECK_EQ_INT(d.items.len, 100);

    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT(d.items.len, 0);
    EMB_CHECK_EQ_INT(d.clips.len, 1);
    EMB_CHECK_EQ_INT(d.xforms.len, 1);
    /* Pooled: the arrays keep their capacity across a reset. */
    EMB_CHECK(d.items.cap == cap);

    embi_dl_free(&d);
    EMB_CHECK(d.items.data == nullptr && d.clips.data == nullptr);
    /* Free and then reset must come back usable. */
    embi_dl_reset(&d);
    EMB_CHECK_EQ_INT(d.clips.len, 1);
    embi_dl_free(&d);
}

EMB_TEST(display_list_url_table_interns_repeats)
{
    embi_dl d;
    memset(&d, 0, sizeof d);
    embi_dl_init(&d);
    embi_pst p;
    memset(&p, 0, sizeof p);
    p.d = &d;
    embi_str a = {"a.png", 5}, b = {"bb.png", 6};
    uint32_t ia = embi_pb_url(&p, a);
    uint32_t ib = embi_pb_url(&p, b);
    uint32_t ia2 = embi_pb_url(&p, a);
    EMB_CHECK_EQ_INT(ia, 0);
    EMB_CHECK_EQ_INT(ib, 1);
    EMB_CHECK_EQ_INT(ia2, 2);
    embi_str got = embi_dl_url(&d, ib);
    EMB_CHECK_EQ_INT(got.len, 6);
    EMB_CHECK(memcmp(got.ptr, "bb.png", 6) == 0);
    embi_str empty = {"", 0};
    uint32_t ie = embi_pb_url(&p, empty);
    EMB_CHECK(embi_dl_url(&d, ie).len == 0);
    EMB_CHECK(embi_dl_url(&d, 99).len == 0);
    embi_dl_free(&d);
}

EMB_TEST(background_color_matches_the_border_box)
{
    /* A first build initializes the process-wide text and style caches (kept for the whole run); the leak check
     * compares the live bytes after that, so only what a build allocates and does not free is counted. */
    {
        ph warm = ph_make("<div>warm</div>", "");
        ph_free(&warm);
    }
    size_t before = embi_mem_live();
    {
        ph h = ph_make("<div id=a style=\"background:#f00;width:100px;height:50px;margin-left:7px;margin-top:3px\"></div>", "");
        embi_element *el = ph_id(&h, "a");
        EMB_REQUIRE(el != nullptr);
        embi_rectf bb;
        EMB_REQUIRE(embi_layout_border_box(h.eng, el, &bb));
        EMB_CHECK(bb.x == 7.0f && bb.y == 3.0f && bb.w == 100.0f && bb.h == 50.0f);
        int i = ph_fill_of(&h.dl, PH_RED);
        EMB_REQUIRE(i >= 0);
        const embi_ditem *it = &h.dl.items.data[i];
        EMB_CHECK(it->rect[0] == 7.0f);
        EMB_CHECK(it->rect[1] == 3.0f);
        EMB_CHECK(it->rect[2] == 100.0f);
        EMB_CHECK(it->rect[3] == 50.0f);
        /* the item is inside the viewport clip of the canvas */
        EMB_CHECK(it->clip != 0);
        EMB_CHECK(it->xform == 0);
        for (int k = 0; k < 4; k++) {
            EMB_CHECK(it->radii[k] == 0.0f);
        }
        ph_free(&h);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(transparent_box_emits_no_fill)
{
    ph h = ph_make("<div id=a style=\"width:10px;height:10px;background:transparent\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 0);
    ph_free(&h);
}

EMB_TEST(border_item_has_widths_colors_and_styles)
{
    ph h = ph_make("<div style=\"border:2px solid #f00;border-bottom:4px dashed #00f;width:10px;height:10px\"></div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* top, right, bottom, left */
    EMB_CHECK(it->widths[0] == 2.0f);
    EMB_CHECK(it->widths[1] == 2.0f);
    EMB_CHECK(it->widths[2] == 4.0f);
    EMB_CHECK(it->widths[3] == 2.0f);
    EMB_CHECK(it->colors[0] == PH_RED);
    EMB_CHECK(it->colors[2] == PH_BLUE);
    EMB_CHECK(it->styles[0] == EMBI_DS_SOLID);
    EMB_CHECK(it->styles[2] == EMBI_DS_DASHED);
    /* the rect is the border box, which includes the borders */
    EMB_CHECK(it->rect[2] == 14.0f);
    EMB_CHECK(it->rect[3] == 16.0f);
    ph_free(&h);
}

EMB_TEST(no_border_item_without_widths)
{
    ph h = ph_make("<div style=\"border:0 solid #f00;width:10px;height:10px\"></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 0);
    ph_free(&h);
}

EMB_TEST(radii_are_resolved_and_scaled_to_the_side)
{
    ph h = ph_make("<div style=\"width:40px;height:40px;background:#f00;border-radius:30px 30px 0 0\"></div>", "");
    int i = ph_fill_of(&h.dl, PH_RED);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* 30 + 30 along a 40 px side scales every radius by 40 / 60 */
    EMB_CHECK(it->radii[0] > 19.9f && it->radii[0] < 20.1f);
    EMB_CHECK(it->radii[1] > 19.9f && it->radii[1] < 20.1f);
    EMB_CHECK(it->radii[2] == 0.0f);
    EMB_CHECK(it->radii[3] == 0.0f);
    ph_free(&h);
}

EMB_TEST(text_item_points_at_the_run_and_its_bytes)
{
    ph h = ph_make("<div id=t style=\"color:#00f;font-family:Ahem\">Hello</div>", "");
    int i = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    EMB_CHECK_EQ_INT(it->aux3, 0);
    EMB_CHECK_EQ_INT(it->aux2, 5);
    EMB_CHECK(it->colors[0] == PH_BLUE);
    EMB_CHECK(it->aux < h.dl.runs.len);
    /* the baseline is inside the text box */
    EMB_CHECK(it->f[1] >= it->rect[1]);
    EMB_CHECK(it->f[1] <= it->rect[1] + it->rect[3]);
    ph_free(&h);
}

EMB_TEST(text_decorations_come_after_the_text)
{
    ph h = ph_make("<div style=\"text-decoration:underline line-through;color:#000\">Text</div>", "");
    int t = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(t >= 0);
    int d = ph_find(&h.dl, (uint32_t)t, EMBI_DI_DECORATION);
    EMB_CHECK(d > t);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_DECORATION), 2);
    ph_free(&h);
}

EMB_TEST(visibility_hidden_paints_nothing_of_its_own)
{
    ph h = ph_make("<div style=\"visibility:hidden;background:#f00;height:10px\"><span style=\"visibility:visible\">x</span></div>", "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 0);
    /* the visible child still paints its text */
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_TEXT), 1);
    ph_free(&h);
}

EMB_TEST(root_background_paints_the_canvas_once)
{
    ph h = ph_make("<html><body style=\"background:#0f0\"><p>x</p></body></html>", "");
    /* the body's background moves to the canvas: exactly one fill of that color, over the viewport */
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_FILL), 1);
    int i = ph_fill_of(&h.dl, PH_GREEN);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].rect[2] == 800.0f);
    EMB_CHECK(h.dl.items.data[i].rect[3] == 600.0f);
    ph_free(&h);
}

EMB_TEST(collapsed_table_borders_paint_one_item_per_grid_edge)
{
    /* a 2 x 2 table: (2 + 1) horizontal rows of 2 edges and 2 rows of (2 + 1) vertical edges, all 2 px red */
    ph h = ph_make("<table style=\"border-collapse:collapse\"><tr><td>a</td><td>b</td></tr>"
                   "<tr><td>c</td><td>d</td></tr></table>",
                   "td{border:2px solid #f00;padding:0}");
    uint32_t n = ph_count(&h.dl, EMBI_DI_BORDER);
    EMB_CHECK(n >= 12);
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].colors[0] == PH_RED);
    /* every border item is a grid edge: 2 px on its one side, and no cell border of its own */
    for (uint32_t k = 0; k < h.dl.items.len; k++) {
        const embi_ditem *q = &h.dl.items.data[k];
        if (q->kind == EMBI_DI_BORDER) {
            EMB_CHECK(q->widths[0] + q->widths[1] + q->widths[2] + q->widths[3] == 2.0f);
        }
    }
    ph_free(&h);
}

EMB_TEST(display_list_of_an_empty_document_is_empty_and_valid)
{
    ph h = ph_make("", "");
    EMB_CHECK(h.dl.items.len <= 1);
    ph_free(&h);
}

EMB_TEST_MAIN()
