/*
 * Tables (W3g): cell and column geometry through the whole layout. Expected values are derived by hand
 * from CSS Tables 3 and CSS 2.1 section 17 (separate borders unless a test says otherwise). The UA body
 * margin is zero (the harness), and every cell has zero padding so the widths are the content widths.
 */
#include "layout_harness.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

/* Every test builds and frees its own engine: the live allocation count must return to what it was before. */
static size_t g_mem_before;

static lh tb_make(const char *html, const char *css)
{
    static bool warmed;
    if (!warmed) {
        /* The first engine loads process-wide font data that stays allocated: do that first. */
        lh w = lh_make("<body><div>x</div></body>", "");
        lh_free(&w);
        warmed = true;
    }
    g_mem_before = embi_mem_live();
    return lh_make(html, css);
}

static void tb_free(lh *h)
{
    lh_free(h);
    EMB_CHECK(embi_mem_live() == g_mem_before);
}

static bool rect_of(lh *h, const char *id, embi_rectf *out)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, out);
}

EMB_TEST(table_widths_follow_cell_widths)
{
    lh h = tb_make("<body><table id=t><tr><td id=a><div style='width:30px;height:10px'></div></td>"
                   "<td id=b><div style='width:50px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.w, 80));
    EMB_CHECK(near(t.h, 10));
    EMB_CHECK(near(a.w, 30));
    EMB_CHECK(near(b.x, 30));
    EMB_CHECK(near(b.w, 50));
    tb_free(&h);
}

EMB_TEST(table_border_spacing_separates_cells_and_edges)
{
    /* Two 10px cells with 4px spacing: the table is 4 + 10 + 4 + 10 + 4 = 32 wide and 4 + 10 + 4 = 18 high. */
    lh h = tb_make("<body><table id=t><tr><td id=a><div style='width:10px;height:10px'></div></td>"
                   "<td id=b><div style='width:10px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:4px} td{padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.w, 32));
    EMB_CHECK(near(t.h, 18));
    EMB_CHECK(near(a.x, 4));
    EMB_CHECK(near(a.y, 4));
    EMB_CHECK(near(b.x, 18));
    tb_free(&h);
}

EMB_TEST(table_colspan_spans_its_columns)
{
    /* The spanning cell needs 100 and the two columns give 30 and 20: the 50px excess is shared in
     * proportion to the columns' maxima (30:20), so the columns are 60 and 40. */
    lh h = tb_make("<body><table id=t><tr><td id=a colspan=2><div style='width:100px;height:10px'></div></td>"
                   "</tr><tr><td id=b><div style='width:30px;height:10px'></div></td>"
                   "<td id=c><div style='width:20px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf a, b, c;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(a.w, 100));
    EMB_CHECK(near(b.w, 60));
    EMB_CHECK(near(c.x, 60));
    EMB_CHECK(near(c.w, 40));
    tb_free(&h);
}

EMB_TEST(table_rowspan_shares_height_among_its_rows)
{
    /* The rowspan cell needs 50, its rows give 20 and 20: each row takes 5 more (in proportion to the
     * heights), so the rows are 25 and 25. The cell in the second row goes to the second column. */
    lh h = tb_make("<body><table id=t><tr><td id=a rowspan=2><div style='width:10px;height:50px'></div></td>"
                   "<td id=b><div style='width:10px;height:20px'></div></td></tr>"
                   "<tr><td id=c><div style='width:10px;height:20px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf a, b, c;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(a.h, 50));
    EMB_CHECK(near(b.h, 25));
    EMB_CHECK(near(c.y, 25));
    EMB_CHECK(near(c.x, 10));
    tb_free(&h);
}

EMB_TEST(table_caption_sits_above_the_grid)
{
    /* A 15px caption over a 10px row: the row starts under the caption, and the table is 25 high. */
    lh h = tb_make("<body><table id=t><caption id=cap><div style='height:15px'></div></caption>"
                   "<tr><td id=a><div style='width:20px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, cap, a;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "cap", &cap));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(cap.y, 0));
    EMB_CHECK(near(cap.h, 15));
    EMB_CHECK(near(a.y, 15));
    EMB_CHECK(near(t.h, 25));
    tb_free(&h);
}

EMB_TEST(table_display_table_cell_gets_an_anonymous_row)
{
    /* A table-cell with no table-row around it sits in an anonymous row of its table. */
    lh h = tb_make("<body><div id=t style='display:table;border-spacing:0'>"
                   "<div id=d style='display:table-cell;width:40px;height:12px'></div></div></body>",
                   "");
    embi_rectf t, d;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(t.w, 40));
    EMB_CHECK(near(t.h, 12));
    EMB_CHECK(near(d.x, 0));
    EMB_CHECK(near(d.h, 12));
    tb_free(&h);
}

EMB_TEST(table_vertical_align_middle_centers_the_content)
{
    /* The short cell is centered in the 40px row (the UA sheet gives a row vertical-align: middle, which
     * its cells inherit): its 10px content starts at 15. */
    lh h = tb_make("<body><table id=t><tr><td id=a><div style='height:40px;width:10px'></div></td>"
                   "<td id=b><div id=bd style='height:10px;width:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf b, bd;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_REQUIRE(rect_of(&h, "bd", &bd));
    EMB_CHECK(near(b.h, 40));
    EMB_CHECK(near(bd.y, 15));
    tb_free(&h);
}

EMB_TEST(table_fixed_layout_takes_the_first_row_widths)
{
    /* Fixed layout: the first row sets the first column (50px); the second column takes the rest of 200px. */
    lh h = tb_make("<body><table id=t style='table-layout:fixed;width:200px'>"
                   "<tr><td id=a style='width:50px'></td><td id=b></td></tr>"
                   "<tr><td id=c><div style='width:300px;height:5px'></div></td><td></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.w, 200));
    EMB_CHECK(near(a.w, 50));
    EMB_CHECK(near(b.x, 50));
    EMB_CHECK(near(b.w, 150));
    tb_free(&h);
}

EMB_TEST(table_height_extra_is_shared_by_the_rows)
{
    /* A 100px table over two 20px rows: each row takes 30 more (in proportion to 20 and 20). */
    lh h = tb_make("<body><table id=t style='height:100px'>"
                   "<tr><td id=a><div style='height:20px;width:10px'></div></td></tr>"
                   "<tr><td id=b><div style='height:20px;width:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.h, 100));
    EMB_CHECK(near(a.h, 50));
    EMB_CHECK(near(b.y, 50));
    tb_free(&h);
}

/* The table fragment of an element (the one that carries the collapsed borders), or nullptr. */
static const embi_frag *frag_of_el(const embi_frag *f, const embi_element *el)
{
    if (f == nullptr) {
        return nullptr;
    }
    if (f->box != nullptr && f->box->el == el && (f->flags & EMBI_FF_TABLE) != 0) {
        return f;
    }
    for (const embi_frag *k = f->first; k != nullptr; k = k->next) {
        const embi_frag *r = frag_of_el(k, el);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

static const embi_cgrid *coll_of(lh *h, const char *id)
{
    const embi_frag *f = frag_of_el(embi_layout_root(h->eng, h->doc), lh_id(h, id));
    return f != nullptr ? embi_frag_coll(f) : nullptr;
}

/* A one-row, two-cell collapsed table whose first cell has the border-right a and second the border-left b.
 * The vertical edge between the cells is v[1] (row 0, column boundary 1). */
static embi_cborder middle_edge(const char *a, const char *b, bool *ok)
{
    char css[512];
    snprintf(css, sizeof css,
             "table{border-collapse:collapse} td{padding:0;width:10px;height:10px}"
             "#a{border-right:%s} #b{border-left:%s}", a, b);
    lh h = tb_make("<body><table id=t><tr><td id=a></td><td id=b></td></tr></table></body>", css);
    const embi_cgrid *g = coll_of(&h, "t");
    embi_cborder e = {0, 0, 0, 0, nullptr};
    *ok = g != nullptr && g->rows == 1 && g->cols == 2;
    if (*ok) {
        e = g->v[1];
    }
    tb_free(&h);
    return e;
}

EMB_TEST(table_collapse_hidden_beats_every_other_border)
{
    bool ok;
    embi_cborder e = middle_edge("9px solid red", "1px hidden", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(near(e.w, 0));
    EMB_CHECK(e.style == EMBI_KW_hidden);
}

EMB_TEST(table_collapse_none_loses_to_any_other_border)
{
    bool ok;
    embi_cborder e = middle_edge("none", "3px solid red", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(near(e.w, 3));
    EMB_CHECK(e.style == EMBI_KW_solid);
    EMB_CHECK(e.side == 3); /* the second cell's left border */
}

EMB_TEST(table_collapse_both_none_is_zero_wide)
{
    bool ok;
    embi_cborder e = middle_edge("none", "none", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(near(e.w, 0));
}

EMB_TEST(table_collapse_the_wider_border_wins)
{
    bool ok;
    embi_cborder e = middle_edge("2px double red", "6px dotted red", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(near(e.w, 6));
    EMB_CHECK(e.style == EMBI_KW_dotted);
}

EMB_TEST(table_collapse_equal_width_the_style_rank_decides)
{
    /* CSS 2.1 17.6.2.1: double, solid, dashed, dotted, ridge, outset, groove, inset. */
    bool ok;
    embi_cborder e = middle_edge("4px dashed red", "4px double red", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(e.style == EMBI_KW_double);
    e = middle_edge("4px dotted red", "4px solid red", &ok);
    EMB_CHECK(e.style == EMBI_KW_solid);
    e = middle_edge("4px groove red", "4px dashed red", &ok);
    EMB_CHECK(e.style == EMBI_KW_dashed);
}

EMB_TEST(table_collapse_same_border_the_left_cell_wins)
{
    /* Everything equal: the cell further to the left wins (its right border, side 1). */
    bool ok;
    embi_cborder e = middle_edge("4px solid red", "4px solid blue", &ok);
    EMB_REQUIRE(ok);
    EMB_CHECK(e.side == 1);
    EMB_CHECK(e.prio == 5);
}

EMB_TEST(table_collapse_same_border_the_upper_cell_wins)
{
    lh h = tb_make("<body><table id=t><tr><td id=a></td></tr><tr><td id=b></td></tr></table></body>",
                   "table{border-collapse:collapse} td{padding:0;width:10px;height:10px}"
                   "#a{border-bottom:4px solid red} #b{border-top:4px solid blue}");
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr && g->rows == 2 && g->cols == 1);
    EMB_CHECK(near(g->h[1].w, 4));
    EMB_CHECK(g->h[1].side == 2); /* the upper cell's bottom border */
    tb_free(&h);
}

EMB_TEST(table_collapse_element_priority_cell_row_group_column_table)
{
    /* The same 4px solid border from one element at a time on the single cell's top edge (or, for the
     * column, its left edge): the winner records the priority of its owner. */
    const char *base = "table{border-collapse:collapse} td{padding:0;width:10px;height:10px}";
    const char *tpl = "<body><table id=t><colgroup><col id=c></colgroup>"
                      "<tbody id=g><tr id=r><td id=a></td></tr></tbody></table></body>";
    const char *rules[5] = {"#a{border-top:4px solid red}", "#r{border-top:4px solid red}",
                            "#g{border-top:4px solid red}", "#c{border-left:4px solid red}",
                            "#t{border-top:4px solid red}"};
    uint8_t want[5] = {5, 4, 3, 2, 0};
    for (int i = 0; i < 5; i++) {
        char css[256];
        snprintf(css, sizeof css, "%s%s", base, rules[i]);
        lh h = tb_make(tpl, css);
        const embi_cgrid *g = coll_of(&h, "t");
        EMB_REQUIRE(g != nullptr);
        const embi_cborder *e = i == 3 ? &g->v[0] : &g->h[0];
        EMB_CHECK(near(e->w, 4));
        EMB_CHECK(e->prio == want[i]);
        tb_free(&h);
    }
}

EMB_TEST(table_collapse_cell_beats_row_on_a_shared_edge)
{
    lh h = tb_make("<body><table id=t><tr id=r><td id=a></td></tr></table></body>",
                   "table{border-collapse:collapse} td{padding:0;width:10px;height:10px}"
                   "#r{border-top:4px solid red} #a{border-top:4px solid blue}");
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr);
    EMB_CHECK(g->h[0].prio == 5);
    tb_free(&h);
}

EMB_TEST(table_collapse_odd_width_border_halves_floor_first)
{
    /* A 5px collapsed border all round a cell: the cell takes one half of each outer edge (the start side floor(5 / 2) = 2
     * at its top and left, the rest, 3, at its bottom and right) and the table the other half (3 at its top and left, 2
     * at its bottom and right), as Chromium's NGTableBorders splits an edge (W4f: the WPT border-*-applies-to reftests). */
    lh h = tb_make("<body><table id=t><tr><td id=a></td></tr></table></body>",
                   "table{border-collapse:collapse} td{padding:0;width:10px;height:10px;border:5px solid red}");
    const embi_frag *f = frag_of_el(embi_layout_root(h.eng, h.doc), lh_id(&h, "t"));
    EMB_REQUIRE(f != nullptr);
    /* left, top, right, bottom */
    EMB_CHECK(near(embi_frag_bdr(f, 0), 3));
    EMB_CHECK(near(embi_frag_bdr(f, 1), 3));
    EMB_CHECK(near(embi_frag_bdr(f, 2), 2));
    EMB_CHECK(near(embi_frag_bdr(f, 3), 2));
    tb_free(&h);
}

static const embi_frag *frag_of_box_el(const embi_frag *f, const embi_element *el)
{
    if (f == nullptr) {
        return nullptr;
    }
    if (f->kind == EMBI_FK_BOX && f->box != nullptr && f->box->pe == 0 && f->box->el == el) {
        return f;
    }
    for (const embi_frag *k = f->first; k != nullptr; k = k->next) {
        const embi_frag *r = frag_of_box_el(k, el);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

EMB_TEST(table_empty_cells_hide_marks_the_empty_cell)
{
    lh h = tb_make("<body><table id=t><tr><td id=a></td><td id=b>x</td></tr></table></body>",
                   "table{empty-cells:hide;border-spacing:2px} td{padding:0;border:1px solid red}");
    const embi_frag *root = embi_layout_root(h.eng, h.doc);
    const embi_frag *fa = frag_of_box_el(root, lh_id(&h, "a"));
    const embi_frag *fb = frag_of_box_el(root, lh_id(&h, "b"));
    EMB_REQUIRE(fa != nullptr && fb != nullptr);
    EMB_CHECK((fa->flags & EMBI_FF_HIDE_EMPTY) != 0);
    EMB_CHECK((fb->flags & EMBI_FF_HIDE_EMPTY) == 0);
    tb_free(&h);
}

EMB_TEST(table_collapsed_row_has_no_height)
{
    /* The middle row is visibility: collapse: the table is 10 + 10 high (no spacing), and the third row
     * starts at 10. */
    lh h = tb_make("<body><table id=t><tr><td><div style='height:10px;width:10px'></div></td></tr>"
                   "<tr style='visibility:collapse'><td><div style='height:30px;width:10px'></div></td></tr>"
                   "<tr><td id=c><div style='height:10px;width:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, c;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(t.h, 20));
    EMB_CHECK(near(c.y, 10));
    tb_free(&h);
}

EMB_TEST(table_collapsed_column_has_no_width)
{
    lh h = tb_make("<body><table id=t><col><col style='visibility:collapse'><col>"
                   "<tr><td><div style='height:10px;width:10px'></div></td>"
                   "<td><div style='height:10px;width:50px'></div></td>"
                   "<td id=c><div style='height:10px;width:20px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, c;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(t.w, 30));
    EMB_CHECK(near(c.x, 10));
    tb_free(&h);
}

EMB_TEST(table_rowspan_zero_spans_to_the_end_of_the_row_group)
{
    /* rowspan=0 covers the rest of the tbody (three rows): 3 x 10 high. */
    lh h = tb_make("<body><table id=t><tr><td id=a rowspan=0><div style='width:10px;height:5px'></div></td>"
                   "<td><div style='width:10px;height:10px'></div></td></tr>"
                   "<tr><td><div style='width:10px;height:10px'></div></td></tr>"
                   "<tr><td><div style='width:10px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.h, 30));
    tb_free(&h);
}

EMB_TEST(table_percentage_column_sets_the_intrinsic_maximum)
{
    /* A 50% column of a 20px cell and an auto column of 30px: the table needs 20 / 0.5 = 40 for the
     * first and 30 / (1 - 0.5) = 60 for the second, so as a float (shrink-to-fit) it is 60 wide and
     * the percentage column takes 30. */
    lh h = tb_make("<body><table id=t style='float:left'><tr>"
                   "<td id=a style='width:50%'><div style='width:20px;height:5px'></div></td>"
                   "<td id=b><div style='width:30px;height:5px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, a;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(t.w, 60));
    EMB_CHECK(near(a.w, 30));
    tb_free(&h);
}

EMB_TEST(table_percentage_columns_over_100_fill_the_available_width)
{
    /* 60% + 60% beside an auto column with content: the intrinsic maximum is unbounded, and a float
     * table fills the available width (the 300px container). */
    lh h = tb_make("<body><div style='width:300px'><table id=t style='float:left'><tr>"
                   "<td style='width:60%'><div style='width:10px;height:5px'></div></td>"
                   "<td style='width:60%'><div style='width:10px;height:5px'></div></td>"
                   "<td><div style='width:10px;height:5px'></div></td></tr></table></div></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.w, 300));
    tb_free(&h);
}

EMB_TEST(table_max_content_width_is_the_plain_sum_of_the_column_maxima)
{
    lh h = tb_make("<body><table id=t style='width:max-content'><tr>"
                   "<td style='width:50%'><div style='width:20px;height:5px'></div></td>"
                   "<td><div style='width:30px;height:5px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.w, 50));
    tb_free(&h);
}

EMB_TEST(table_fixed_layout_with_auto_width_uses_auto_layout)
{
    /* table-layout: fixed applies only to a table whose width is not auto: this one sizes to content. */
    lh h = tb_make("<body><table id=t style='table-layout:fixed'><tr>"
                   "<td style='width:10px'><div style='width:40px;height:5px'></div></td>"
                   "<td><div style='width:30px;height:5px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.w, 70));
    tb_free(&h);
}

EMB_TEST(table_height_is_a_minimum)
{
    /* The rows need 30 and the table says 10: it is 30 high. */
    lh h = tb_make("<body><table id=t style='height:10px'><tr><td>"
                   "<div style='width:10px;height:30px'></div></td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.h, 30));
    tb_free(&h);
}

EMB_TEST(table_without_columns_has_no_spacing_when_the_height_is_auto)
{
    /* A table with no cells and auto height is 0 x 0, whatever the border-spacing. */
    lh h = tb_make("<body><div id=t style='display:table;border-spacing:10px'></div></body>", "");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.w, 0));
    EMB_CHECK(near(t.h, 0));
    tb_free(&h);
}

EMB_TEST(table_cell_percent_padding_resolves_against_the_row_width)
{
    /* 10% padding on the cell of a 200px table: 20px on every side (the row width, not the cell's),
     * so the cell is 200 wide (fixed layout, border box = column) and 40 high. */
    lh h = tb_make("<body><table id=t style='width:200px;table-layout:fixed'><tr>"
                   "<td id=a style='padding:10%'></td></tr></table></body>",
                   "table{border-spacing:0}");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_CHECK(near(a.w, 200));
    EMB_CHECK(near(a.h, 40));
    tb_free(&h);
}

EMB_TEST(table_nested_table_percentages_do_not_widen_the_outer_table)
{
    /* The inner table has width:100% in an auto cell: its percentage is ignored for the outer table's
     * intrinsic size, so the outer table is as wide as the inner content (25px). */
    lh h = tb_make("<body><table id=o style='float:left'><tr><td>"
                   "<table id=i style='width:100%'><tr><td><div style='width:25px;height:5px'></div></td></tr></table>"
                   "</td></tr></table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf o;
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_CHECK(near(o.w, 25));
    tb_free(&h);
}

EMB_TEST(table_first_row_gives_the_inline_table_its_baseline)
{
    /* Two inline-tables share a line and align on their baselines. T1 has a 10px first row and a 50px
     * second one; T2 has one 30px row. A table's baseline is its first row's (the bottom of the 10px
     * block in the cell), so T1.y + 10 == T2.y + 30. A last-row baseline (T1.y + 60) or the bottom
     * edge would break the equality. */
    lh h = tb_make("<body><div><table id=t1 style='display:inline-table'>"
                   "<tr><td><div style='width:5px;height:10px'></div></td></tr>"
                   "<tr><td><div style='width:5px;height:50px'></div></td></tr></table>"
                   "<table id=t2 style='display:inline-table'>"
                   "<tr><td><div style='width:5px;height:30px'></div></td></tr></table></div></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t1, t2;
    EMB_REQUIRE(rect_of(&h, "t1", &t1));
    EMB_REQUIRE(rect_of(&h, "t2", &t2));
    EMB_CHECK(near(t1.h, 60));
    EMB_CHECK(near(t2.h, 30));
    EMB_CHECK(near(t1.y + 10, t2.y + 30));
    tb_free(&h);
}

EMB_TEST(table_rtl_places_the_first_column_at_the_right)
{
    lh h = tb_make("<body><table id=t><tr><td id=a><div style='width:30px;height:10px'></div></td>"
                   "<td id=b><div style='width:50px;height:10px'></div></td></tr></table></body>",
                   "table{border-spacing:0;direction:rtl} td{padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.w, 80));
    /* The first cell (30 wide) is at the right edge, the second to its left. */
    EMB_CHECK(near(a.x - t.x, 50));
    EMB_CHECK(near(a.w, 30));
    EMB_CHECK(near(b.x - t.x, 0));
    EMB_CHECK(near(b.w, 50));
    tb_free(&h);
}

EMB_TEST(table_rtl_spacing_and_colspan)
{
    /* Spacing 4, columns 10 / 20 / 30: the table is 4 + 10 + 4 + 20 + 4 + 30 + 4 = 76 wide. Column 2 is
     * the leftmost; the cell spanning columns 0 and 1 is at the right. */
    lh h = tb_make("<body><table id=t><tr><td id=s colspan=2><div style='width:10px;height:5px'></div></td>"
                   "<td id=c><div style='width:30px;height:5px'></div></td></tr>"
                   "<tr><td><div style='width:10px;height:5px'></div></td>"
                   "<td><div style='width:20px;height:5px'></div></td><td></td></tr></table></body>",
                   "table{border-spacing:4px;direction:rtl} td{padding:0}");
    embi_rectf t, s, c;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "s", &s));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(t.w, 76));
    EMB_CHECK(near(c.x - t.x, 4));
    EMB_CHECK(near(c.w, 30));
    EMB_CHECK(near(s.x - t.x, 4 + 30 + 4));
    EMB_CHECK(near(s.w, 34));
    tb_free(&h);
}

EMB_TEST(table_rtl_collapsed_borders_are_mirrored)
{
    /* a is column 0 (the right cell) with left 6 / right 2; b is column 1 (the left cell) with
     * left 4 / right 8. Physical edges left to right: b's left (4); the shared edge, b.right 8 against
     * a.left 6, so 8; a's right (2). */
    lh h = tb_make("<body><table id=t><tr><td id=a></td><td id=b></td></tr></table></body>",
                   "table{border-collapse:collapse;direction:rtl} td{padding:0;width:10px;height:10px}"
                   "#a{border-left:6px solid red;border-right:2px solid blue}"
                   "#b{border-left:4px solid green;border-right:8px solid black}");
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr);
    EMB_REQUIRE(g->rows == 1 && g->cols == 2);
    EMB_CHECK(near(g->v[0].w, 4));
    EMB_CHECK(near(g->v[1].w, 8));
    EMB_CHECK(near(g->v[2].w, 2));
    const embi_frag *f = frag_of_el(embi_layout_root(h.eng, h.doc), lh_id(&h, "t"));
    EMB_REQUIRE(f != nullptr);
    EMB_CHECK(near(embi_frag_bdr(f, 0), 2)); /* left (bdr is left, top, right, bottom): the start half of 4 */
    EMB_CHECK(near(embi_frag_bdr(f, 2), 1)); /* right: the end half of 2 */
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(a.x > b.x);
    EMB_CHECK(near(b.x + b.w, a.x)); /* the cells meet in the middle of the shared edge */
    tb_free(&h);
}

EMB_TEST(table_rowspan_is_cut_at_the_end_of_its_row_group)
{
    /* The rowspan=3 in the first tbody (2 rows) stops at that tbody: the cell in the second tbody is
     * not displaced to the second column. */
    lh h = tb_make("<body><table id=t>"
                   "<tbody><tr><td id=a rowspan=3><div style='width:10px;height:10px'></div></td>"
                   "<td><div style='width:10px;height:10px'></div></td></tr>"
                   "<tr><td><div style='width:10px;height:10px'></div></td></tr></tbody>"
                   "<tbody><tr><td id=c><div style='width:10px;height:10px'></div></td></tr></tbody>"
                   "</table></body>",
                   "table{border-spacing:0} td{padding:0}");
    embi_rectf t, a, c;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(a.h, 20));
    EMB_CHECK(near(c.x - t.x, 0));
    EMB_CHECK(near(c.y - t.y, 20));
    tb_free(&h);
}

EMB_TEST(table_column_count_is_capped_at_1000)
{
    lh h = tb_make("<body><table id=t><tr><td colspan=1000></td><td colspan=1000></td>"
                   "<td id=z colspan=1000></td></tr></table></body>",
                   "table{border-collapse:collapse}");
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr);
    /* The span past the 1000-column limit is ignored; the columns without a starting cell merge into
     * the track before them (CSS Tables 3 section 3.3), so the first cell's 1000 columns are one track. */
    EMB_CHECK(g->cols == 1);
    EMB_CHECK(g->rows == 1);
    tb_free(&h);
}

EMB_TEST(table_column_count_cap_keeps_exactly_1000_tracks_of_distinct_cells)
{
    /* 1001 one-column cells: each starts in its own column, so no track merges away; the cap keeps
     * exactly 1000 of them (a cap of 999 or 1001 would show here). */
    embi_sb sb = {nullptr, 0, 0};
    embi_sb_append(&sb, EMBI_STR("<body><table id=t><tr>"));
    for (int i = 0; i < 1001; i++) {
        embi_sb_append(&sb, EMBI_STR("<td></td>"));
    }
    embi_sb_append(&sb, EMBI_STR("</tr></table></body>"));
    char *html = SDL_malloc(sb.len + 1);
    memcpy(html, sb.data, sb.len);
    html[sb.len] = 0;
    embi_sb_free(&sb);
    lh h = tb_make(html, "table{border-collapse:collapse}");
    SDL_free(html);
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr);
    EMB_CHECK(g->cols == 1000);
    EMB_CHECK(g->rows == 1);
    tb_free(&h);
}

static double pct_child_height(const char *td_h)
{
    char html[512];
    SDL_snprintf(html, sizeof html,
                 "<body><table><tr><td style='height:%s;padding:0'><div id=p style='height:50%%'></div>"
                 "<div style='height:40px'></div></td></tr></table></body>",
                 td_h);
    lh h = tb_make(html, "table{border-spacing:0}");
    embi_rectf p = {0, 0, -1, -1};
    EMB_CHECK(rect_of(&h, "p", &p));
    tb_free(&h);
    return p.h;
}

EMB_TEST(table_percent_height_child_is_continuous_in_the_cell_height)
{
    /* The percentage resolves against the used height of the cell in the final layout, also when the
     * cell is exactly as tall as its measured content (W3h: the measured layout must not be reused
     * then, since it had no height to resolve against). The row is max(H, 40) tall. */
    EMB_CHECK(near(pct_child_height("39px"), 20));
    EMB_CHECK(near(pct_child_height("40px"), 20));
    EMB_CHECK(near(pct_child_height("41px"), 20.5));
}

EMB_TEST(table_collapse_outer_with_nested_percent_width_table)
{
    /* The master model of the outer collapse table is built by the first query; the nested table with
     * width:100% sizes differently under the outer's intrinsic query (percent ignored) and its layout
     * (resolved): the cell contributions must not be reused across the two. */
    lh h = tb_make("<body><table id=o style='border-collapse:collapse'><tr><td id=c style='padding:0;border:1px solid'>"
                   "<table id=i style='width:100%;border-spacing:0'><tr><td style='padding:0'>"
                   "<div style='width:25px;height:5px'></div></td></tr></table>"
                   "</td><td style='padding:0'><div style='width:100px;height:5px'></div></td></tr></table></body>",
                   "");
    embi_rectf o, c, i;
    EMB_REQUIRE(rect_of(&h, "o", &o));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_REQUIRE(rect_of(&h, "i", &i));
    /* The nested table fills its cell's content box (25px of content). The cell has its 1px left edge's floor half
     * (0) and its right edge's rest (1), so it is 26 wide; the outer table adds the other half of the outer left
     * edge (1) to the cell and the 100px cell: 1 + 26 + 100. */
    EMB_CHECK(near(i.w, 25));
    EMB_CHECK(near(c.w, 26));
    EMB_CHECK(near(o.w, 127));
    tb_free(&h);
}

EMB_TEST(table_rtl_same_border_the_right_cell_wins)
{
    /* CSS 2.1 17.6.2.1: on a tie the one further to the right wins in rtl. a is column 0, the right
     * cell: its left border (side 3) owns the shared edge. */
    lh h = tb_make("<body><table id=t><tr><td id=a></td><td id=b></td></tr></table></body>",
                   "table{border-collapse:collapse;direction:rtl} td{padding:0;width:10px;height:10px}"
                   "#a{border-left:4px solid red} #b{border-right:4px solid blue}");
    const embi_cgrid *g = coll_of(&h, "t");
    EMB_REQUIRE(g != nullptr && g->cols == 2);
    EMB_CHECK(near(g->v[1].w, 4));
    EMB_CHECK(g->v[1].side == 3);
    EMB_REQUIRE(g->v[1].owner != nullptr);
    EMB_CHECK(g->v[1].owner->el == lh_id(&h, "a"));
    tb_free(&h);
}

EMB_TEST(table_col_elements_are_capped_at_1000_columns)
{
    size_t base = embi_mem_live();
    {
        char html[512];
        snprintf(html, sizeof html,
                 "<body><table id=t><colgroup span=1000></colgroup>"
                 "<col span=1000><col span=1000><colgroup><col span=1000><col span=1000></colgroup>"
                 "<tr><td></td></tr></table></body>");
        lh h = tb_make(html, "table{border-collapse:collapse} col,colgroup{width:10px}");
        const embi_cgrid *g = coll_of(&h, "t");
        EMB_REQUIRE(g != nullptr);
        EMB_CHECK(g->cols == 1000); /* columns with a width are tracks of their own and are not merged */
        tb_free(&h);
    }
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(table_collapsed_model_is_skipped_past_the_unit_cap)
{
    /* 300 rows x 1000 columns = 300K units, over the 256K cap: no collapsed model, no crash. */
    size_t base = 0;
    for (int pass = 0; pass < 2; pass++) { /* the first pass warms the lazily built shared caches */
        if (pass == 1) {
            base = embi_mem_live();
        }
        char *html = malloc(64 * 1024);
        EMB_REQUIRE(html != nullptr);
        size_t n = (size_t)snprintf(html, 64 * 1024, "<body><table id=t><col span=1000>");
        for (int i = 0; i < 300; i++) {
            n += (size_t)snprintf(html + n, 64 * 1024 - n, "<tr><td></td></tr>");
        }
        snprintf(html + n, 64 * 1024 - n, "</table></body>");
        lh h = tb_make(html, "table{border-collapse:collapse} col,colgroup{width:10px}");
        free(html);
        const embi_cgrid *g = coll_of(&h, "t");
        EMB_CHECK(g == nullptr || g->cols == 0);
        tb_free(&h);
    }
    EMB_CHECK(embi_mem_live() == base);
}

/* Column-track merging (CSS Tables 3 section 3.3, WPT column-track-merging.html): the columns in which
 * no cell starts merge into the track before them, so a colspan=10 cell is one 50px track and the table
 * has two tracks, three border spacings and the borders: 50 + 50 + 3 * 20 + 2 * 10. */
EMB_TEST(table_empty_columns_merge_into_one_track)
{
    lh h = tb_make("<body><table id=t><tr><td id=a colspan=10></td><td id=b></td></tr>"
                   "<tr><td colspan=10></td><td></td></tr></table></body>",
                   "table{border:10px solid gray;border-spacing:20px} td{width:50px;height:50px;padding:0}");
    embi_rectf t, a, b;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(t.w, 180));
    EMB_CHECK(near(a.w, 50));
    EMB_CHECK(near(b.x - a.x, 70));
    tb_free(&h);
}

/* A column element with a width is a track of its own, merged or not by the cells: three 30px tracks and
 * four spacings of 10px. */
EMB_TEST(table_columns_with_a_width_are_not_merged)
{
    lh h = tb_make("<body><table id=t><col span=3 style='width:30px'><tr><td id=a></td></tr></table></body>",
                   "table{border-spacing:10px} td{padding:0;height:10px}");
    embi_rectf t;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_CHECK(near(t.w, 3 * 30 + 4 * 10));
    tb_free(&h);
}

/* A fixed table keeps its columns (the spanned empty columns stay tracks of zero width). */
EMB_TEST(table_fixed_layout_does_not_merge_columns)
{
    lh h = tb_make("<body><table id=t><tr><td id=a colspan=3></td><td id=b></td></tr></table></body>",
                   "table{table-layout:fixed;width:200px;border-spacing:10px} td{padding:0;height:10px}");
    embi_rectf a;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    /* Four tracks share the 200px less five 10px spacings equally (37.5px each): a spans three of them
     * and the two spacings between them. Merged, it would be two tracks of 70px each. */
    EMB_CHECK(near(a.w, 3 * 37.5 + 2 * 10));
    tb_free(&h);
}

/* The scroll box of an anonymous cell with a percentage height takes the table's height instead of
 * sizing the row by its content (WPT percent-height-overflow-auto-in-restricted-block-size-cell.html). */
EMB_TEST(table_percent_height_scroll_box_in_an_anonymous_cell)
{
    lh h = tb_make("<body><div id=t style='display:table;height:100px'>"
                   "<div id=c style='overflow:auto;width:100px;height:100%'>"
                   "<div style='width:100px;height:500px'></div></div></div></body>",
                   "");
    embi_rectf t, c;
    EMB_REQUIRE(rect_of(&h, "t", &t));
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(t.h, 100));
    EMB_CHECK(near(c.h, 100));
    tb_free(&h);
}

EMB_TEST_MAIN()
