/*
 * Regression tests for the layout fixes of the CSS2 reftest burn-down (W4g1): box-decoration-break slice for split inline
 * boxes, floats that do not fit beside the line content, table min-height and column and cell min/max widths,
 * text-indent in the intrinsic widths, a BFC box beside a float with an over-constrained margin, and floats inside a table
 * (anonymous cell). Expected values are worked out by hand from CSS 2.1 (sections 9.2.1.1, 9.5, 10.3.3, 17) and Ahem
 * (every glyph 1em wide).
 */
#include "layout_harness.h"

#include <SDL3/SDL_iostream.h>

#include <math.h>
#include <stdio.h>

static bool load_ahem(void *user, embi_str base, embi_str url, uint8_t **data, size_t *len)
{
    (void)user;
    (void)base;
    (void)url;
    size_t n = 0;
    void *buf = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/wpt/fonts/Ahem.ttf", &n);
    if (buf == nullptr) {
        return false;
    }
    *data = buf;
    *len = n;
    return true;
}

/* lh_make with a font loader that serves Ahem for @font-face sources. */
static lh make_ahem(const char *html, const char *css)
{
    lh h;
    memset(&h, 0, sizeof h);
    h.text = embi_tshim_create();
    embi_style_callbacks cb = {nullptr, load_ahem};
    h.style = embi_style_engine_create(embi_tshim_fonts(h.text), nullptr, &cb);
    h.doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(h.doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    char *all = lh_join_styles(html, strlen(html), css);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    h.sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)strlen(all)}, &opts);
    SDL_free(all);
    embi_css_sheet *sheets[1] = {h.sheet};
    embi_document_set_author_sheets(h.style, h.doc, sheets, 1);
    embi_style_resolve_document(h.style, h.doc);
    h.eng = embi_layout_engine_create(h.text, h.style, nullptr);
    (void)embi_layout_document(h.eng, h.doc);
    return h;
}

#define AHEM "@font-face{font-family:A;src:url(a.ttf)} body{margin:0;font:10px/1 A}"

static bool rect_of(lh *h, const char *id, embi_rectf *r)
{
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, r);
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

EMB_TEST(float_that_does_not_fit_beside_the_line_content_goes_below_the_line)
{
    /* 80 px of text, then a 40 px float in a 100 px container: 80 + 40 > 100, so the float goes below the line (y = 10)
     * instead of shortening the line it follows. A 20 px float fits beside the text and stays on the first line. */
    lh h = make_ahem("<div id=c style='width:100px;overflow:hidden'>xxxxxxxx<div id=f style='float:left;width:40px;"
                     "height:10px'></div></div><div id=d style='width:100px;overflow:hidden'>xxxxxxxx<div id=g "
                     "style='float:left;width:20px;height:10px'></div></div>",
                     AHEM);
    embi_rectf f, g, d;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_REQUIRE(rect_of(&h, "g", &g));
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(f.x, 0) && near(f.y, 10));
    EMB_CHECK(near(g.y, d.y)); /* the small float stays at the top of its container, beside the text */
    lh_free(&h);
}

EMB_TEST(inline_box_split_by_a_block_has_start_edge_on_first_piece_and_end_edge_on_last)
{
    /* box-decoration-break: slice (CSS 2.1 section 9.2.1.1): the left border and padding only before the first piece, the
     * right ones only after the last. The first piece is the left edge 15 plus "a" (10) = 25; the last is "b" plus the right edge 5 = 15. The
     * border box of the span is the union, so its width is 25 (cloning the edges onto every piece would give 30). */
    lh h = make_ahem("<div style='width:300px'><span id=s style='border-left:15px solid;border-right:5px solid'>a<div style='height:10px'></div>b</span></div>",
                     AHEM);
    embi_rectf s;
    EMB_REQUIRE(rect_of(&h, "s", &s));
    EMB_CHECK(near(s.w, 25));
    lh_free(&h);
}

EMB_TEST(table_row_stretches_to_the_min_height_of_a_cell)
{
    lh h = make_ahem("<table style='border-spacing:0'><tr id=r><td id=a style='padding:0;min-height:40px'></td></tr>"
                     "</table>",
                     AHEM);
    embi_rectf r;
    EMB_REQUIRE(rect_of(&h, "r", &r));
    EMB_CHECK(near(r.h, 40));
    lh_free(&h);
}

EMB_TEST(table_column_width_is_clamped_by_the_col_max_width_and_min_width)
{
    lh h = make_ahem("<table style='border-spacing:0'>"
                     "<col style='width:150px;max-width:60px'><col style='width:10px;min-width:40px'>"
                     "<tr><td id=a style='padding:0'></td><td id=b style='padding:0'></td></tr></table>",
                     AHEM);
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a));
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(a.w, 60)); /* 150 clamped by max-width */
    EMB_CHECK(near(b.w, 40)); /* 10 raised by min-width */
    lh_free(&h);
}

EMB_TEST(text_indent_widens_only_the_first_run_of_the_min_content_width)
{
    /* A float in a 1 px body shrinks to its min-content width. "x" (10) after a 30 px indent is 40, the second run "yyy"
     * (30, no indent) is narrower: the width is 40. Adding the indent to every run would give 60. */
    lh h = make_ahem("<body style='width:1px'><div id=f style='float:left;text-indent:30px'>x&#8203;yyy</div></body>",
                     AHEM);
    embi_rectf f;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_CHECK(near(f.w, 40));
    lh_free(&h);
}

EMB_TEST(bfc_box_beside_a_float_keeps_its_width_when_the_margin_at_the_inline_end_is_over_constrained)
{
    /* A 100 px container, a 50 px float, and an overflow:hidden box of width 50 with margin-right 1: the right margin is
     * over-constrained (CSS 2.1 section 10.3.3), so the box fits beside the float instead of moving below it. */
    lh h = make_ahem("<div style='width:100px'><div id=f style='float:left;width:50px;height:100px'></div>"
                     "<div id=b style='overflow:hidden;margin-right:1px;width:50px;height:100px'></div></div>",
                     AHEM);
    embi_rectf b;
    EMB_REQUIRE(rect_of(&h, "b", &b));
    EMB_CHECK(near(b.x, 50) && near(b.y, 0));
    lh_free(&h);
}

EMB_TEST(float_child_of_a_table_goes_into_an_anonymous_cell)
{
    /* A float directly in a table is in flow for table fixup: it is wrapped in an anonymous row and cell, so the table has
     * its width (the float's 30 px) and the float is inside it. */
    lh h = make_ahem("<div style='display:table;border-spacing:0'><div id=f style='float:left;width:30px;height:10px'>"
                     "</div></div>",
                     AHEM);
    embi_rectf f;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_CHECK(near(f.w, 30) && near(f.x, 0));
    lh_free(&h);
}

EMB_TEST_MAIN()
