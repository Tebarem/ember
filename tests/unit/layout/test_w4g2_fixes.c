/*
 * Regression tests for the text fixes of the css-text burn-down (W4g2): preserved tabs advance to tab stops, a line of only
 * preserved spaces is not empty, word-break: break-all breaks between letters, and the space separators (U+3000 ...) hang at
 * the end of a line. Expected values are worked out by hand with Ahem (every glyph 1em wide).
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

EMB_TEST(preserved_tab_advances_to_the_next_tab_stop)
{
    /* tab-size: 4 is four space advances (40 px): "a" is 10 px, the tab goes to 40, "b" to 50 */
    lh h = make_ahem("<div id=d style='display:inline-block;white-space:pre;tab-size:4'>a&#9;b</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.w, 50));
    lh_free(&h);
}

EMB_TEST(tab_size_length_is_the_stop_distance)
{
    lh h = make_ahem("<div id=d style='display:inline-block;white-space:pre;tab-size:25px'>a&#9;b&#9;c</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    /* a to 10, tab to 25, b to 35, tab to 50, c to 60 */
    EMB_CHECK(near(d.w, 60));
    lh_free(&h);
}

EMB_TEST(collapsed_tab_is_a_space)
{
    lh h = make_ahem("<div id=d style='display:inline-block'>a&#9;b</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.w, 30));
    lh_free(&h);
}

EMB_TEST(pre_wrap_line_of_only_spaces_is_not_empty)
{
    lh h = make_ahem("<div id=d style='white-space:pre-wrap'>  &#10; x</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.h, 20));
    lh_free(&h);
}

EMB_TEST(break_all_breaks_between_letters)
{
    lh h = make_ahem("<div id=d style='width:30px;word-break:break-all'>abcdefg</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.h, 30));
    lh_free(&h);
}

EMB_TEST(ideographic_space_hangs_at_the_end_of_the_line)
{
    lh h = make_ahem("<div id=d style='width:max-content'>ab&#x3000;<br>ab</div>", AHEM);
    embi_rectf d;
    EMB_REQUIRE(rect_of(&h, "d", &d));
    EMB_CHECK(near(d.w, 20) && near(d.h, 20));
    lh_free(&h);
}

EMB_TEST_MAIN()
