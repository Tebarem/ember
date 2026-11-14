/*
 * Regression tests for the layout changes of W4g5: negative margins of atomic inlines count in the line, text-align:
 * justify-all, white-space-trim on inline boxes, a float after a line whose content overflows, calc() in vertical-align
 * and in the intrinsic text-indent. Expected values are worked out by hand with Ahem (every glyph 1em wide).
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

EMB_TEST(a_negative_margin_on_an_inline_block_lets_the_next_item_share_the_line)
{
    lh h = make_ahem("<div style=\"width:30px\"><span id=a style=\"display:inline-block;width:10px;height:10px;margin-right:-10px\"></span>"
                     "<span id=b style=\"display:inline-block;width:30px;height:10px\"></span></div>", AHEM);
    embi_rectf a, b;
    EMB_REQUIRE(rect_of(&h, "a", &a) && rect_of(&h, "b", &b));
    EMB_CHECK(near(b.y, a.y));        /* 10 - 10 + 30 fits in 30: one line */
    EMB_CHECK(near(b.x, a.x));        /* b starts where a started: a took no room */
    lh_free(&h);
}

EMB_TEST(justify_all_stretches_the_last_line_and_justify_does_not)
{
    const char *body = "<div style=\"width:100px;text-align:%s\">aa bb <span id=c>cc</span></div>";
    char buf[256];
    snprintf(buf, sizeof buf, body, "justify-all");
    lh a = make_ahem(buf, AHEM);
    snprintf(buf, sizeof buf, body, "justify");
    lh b = make_ahem(buf, AHEM);
    embi_rectf ca, cb;
    EMB_REQUIRE(rect_of(&a, "c", &ca) && rect_of(&b, "c", &cb));
    EMB_CHECK(near(ca.x + ca.w, 100.0f)); /* the 20px of free space went to the two spaces: cc ends at the edge */
    EMB_CHECK(near(cb.x + cb.w, 80.0f));  /* plain justify leaves the last line alone */
    lh_free(&a);
    lh_free(&b);
}

EMB_TEST(justify_all_yields_to_an_explicit_text_align_last)
{
    lh h = make_ahem("<div style=\"width:100px;text-align:justify-all;text-align-last:left\">aa bb <span id=c>cc</span></div>", AHEM);
    embi_rectf c;
    EMB_REQUIRE(rect_of(&h, "c", &c));
    EMB_CHECK(near(c.x + c.w, 80.0f));
    lh_free(&h);
}

EMB_TEST(white_space_trim_discard_before_removes_the_space_before_the_box)
{
    lh a = make_ahem("<div style=\"width:200px\">a <span id=s>b</span></div>", AHEM);
    lh b = make_ahem("<div style=\"width:200px\">a <span id=s style=\"white-space-trim:discard-before\">b</span></div>", AHEM);
    embi_rectf sa, sb;
    EMB_REQUIRE(rect_of(&a, "s", &sa) && rect_of(&b, "s", &sb));
    EMB_CHECK(near(sa.x, 20.0f)); /* "a", a space, then b */
    EMB_CHECK(near(sb.x, 10.0f)); /* the space is gone */
    lh_free(&a);
    lh_free(&b);
}

EMB_TEST(white_space_trim_discard_after_removes_the_spaces_after_the_box)
{
    lh a = make_ahem("<div style=\"width:200px\"><span>a</span>   <i id=t>b</i></div>", AHEM);
    lh b = make_ahem("<div style=\"width:200px\"><span style=\"white-space-trim:discard-after\">a</span>   <i id=t>b</i></div>", AHEM);
    embi_rectf ta, tb;
    EMB_REQUIRE(rect_of(&a, "t", &ta) && rect_of(&b, "t", &tb));
    EMB_CHECK(near(ta.x, 20.0f));
    EMB_CHECK(near(tb.x, 10.0f));
    lh_free(&a);
    lh_free(&b);
}

EMB_TEST(white_space_trim_discard_inner_removes_the_edge_spaces_inside_the_box)
{
    lh h = make_ahem("<div style=\"width:200px\">x<span id=s style=\"white-space-trim:discard-inner\">  a  </span><i id=t>b</i></div>", AHEM);
    embi_rectf s, t;
    EMB_REQUIRE(rect_of(&h, "s", &s) && rect_of(&h, "t", &t));
    EMB_CHECK(near(s.w, 10.0f)); /* only the a */
    EMB_CHECK(near(t.x, 20.0f)); /* x, a, then b with no space in between */
    lh_free(&h);
}

EMB_TEST(white_space_trim_keeps_the_interior_spaces)
{
    lh h = make_ahem("<div style=\"width:200px\"><span id=s style=\"white-space-trim:discard-inner discard-before discard-after\">a  b</span></div>", AHEM);
    embi_rectf s;
    EMB_REQUIRE(rect_of(&h, "s", &s));
    EMB_CHECK(near(s.w, 30.0f)); /* a, one collapsed space, b */
    lh_free(&h);
}

EMB_TEST(a_float_after_a_line_that_overflows_goes_below_the_line)
{
    lh h = make_ahem("<div style=\"width:30px\"><span style=\"white-space:nowrap\">aaaaaaaa</span>"
                     "<div id=f style=\"float:left;width:10px;height:10px\"></div></div>", AHEM);
    embi_rectf f;
    EMB_REQUIRE(rect_of(&h, "f", &f));
    EMB_CHECK(f.y >= 10.0f - 0.01f); /* not beside the overflowing line */
    lh_free(&h);
}

EMB_TEST(a_percentage_in_calc_vertical_align_refers_to_the_line_height)
{
    lh a = make_ahem("<div style=\"font:10px/20px A;width:200px\">x<span id=s style=\"vertical-align:calc(50%)\">y</span></div>", AHEM);
    lh b = make_ahem("<div style=\"font:10px/20px A;width:200px\">x<span id=s style=\"vertical-align:50%\">y</span></div>", AHEM);
    embi_rectf sa, sb;
    EMB_REQUIRE(rect_of(&a, "s", &sa) && rect_of(&b, "s", &sb));
    EMB_CHECK(near(sa.y, sb.y)); /* calc(50%) is the same shift as 50% */
    lh_free(&a);
    lh_free(&b);
}

EMB_TEST(a_percentage_text_indent_counts_zero_in_the_shrink_to_fit_width)
{
    lh h = make_ahem("<div id=d style=\"float:left;text-indent:50%\">aaaa</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.w, 40.0f)); /* the 50% indent adds nothing to the intrinsic width */
    lh_free(&h);
}

EMB_TEST_MAIN()
