/*
 * Box tree (W3a section 1): generated content (::before, ::after, attr(), counter(), counters(),
 * quotes, url() items), list markers, replaced boxes and form-control sizes, display: contents,
 * block-in-inline splitting, flex and grid item wrapping, white space and text-transform through the
 * boxes, and the counter scope of siblings. The expected values are the CSS and Chromium values
 * for these markups; the comments give the reason for each.
 */
#include "layout_harness.h"

#include <math.h>
#include <string.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

/* The text generated for the pseudo element pe of element id, as a C string in sb (NUL-terminated
 * by embi_sb_cstr). */
static bool pseudo_is(lh *h, const char *id, uint8_t pe, const char *want)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr || el->box == nullptr) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    lh_pseudo_text((const embi_box *)el->box, el, pe, &sb);
    bool ok = sb.len == strlen(want) && (sb.len == 0 || memcmp(sb.data, want, sb.len) == 0);
    embi_sb_free(&sb);
    return ok;
}

/* The text of the box of the element id (its TEXT children, not pseudo elements): the processed
 * white space and case. */
static bool text_is(lh *h, const char *id, const char *want)
{
    embi_element *el = lh_id(h, id);
    if (el == nullptr || el->box == nullptr) {
        return false;
    }
    embi_sb sb = {nullptr, 0, 0};
    for (const embi_box *k = ((const embi_box *)el->box)->first; k != nullptr; k = k->next) {
        if (k->kind == EMBI_BK_TEXT && k->pe == 0 && k->text != nullptr) {
            embi_sb_append(&sb, (embi_str){k->text, k->text_len});
        }
    }
    bool ok = sb.len == strlen(want) && (sb.len == 0 || memcmp(sb.data, want, sb.len) == 0);
    embi_sb_free(&sb);
    return ok;
}

static bool border_size(lh *h, const char *id, float *w, float *hh)
{
    embi_rectf r;
    embi_element *el = lh_id(h, id);
    if (el == nullptr || !embi_layout_border_box(h->eng, el, &r)) {
        return false;
    }
    *w = r.w;
    *hh = r.h;
    return true;
}

EMB_TEST(box_before_and_after_content_are_generated_text)
{
    lh h = lh_make("<body><div id=d>x</div></body>",
                   "body { margin: 0 } #d::before { content: '[' } #d::after { content: ']' }");
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_BEFORE, "["));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_AFTER, "]"));
    lh_free(&h);
}

EMB_TEST(box_attr_function_reads_the_attribute)
{
    lh h = lh_make("<body><div id=d data-x='hi'></div><div id=e></div></body>",
                   "body { margin: 0 } #d::before { content: attr(data-x) } "
                   "#e::before { content: attr(data-x) '-none' }");
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_BEFORE, "hi"));
    /* A missing attribute gives the fallback string, here the literal after the name. */
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_BEFORE, "-none"));
    lh_free(&h);
}

EMB_TEST(box_counter_and_counters_follow_the_tree)
{
    /* The ol resets list-item to 0, and each li increments it: 1 and 2. counters() lists the one
     * instance in scope. */
    lh h = lh_make("<body><ol><li id=a>x</li><li id=b>y</li></ol></body>",
                   "body { margin: 0 } li::before { content: counter(list-item) '/' counters(list-item, '.') }");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_BEFORE, "1/1"));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_BEFORE, "2/2"));
    lh_free(&h);
}

EMB_TEST(box_sibling_resets_replace_the_earlier_instance)
{
    /* CSS Lists 3 section 4.4: the second list's reset replaces the first list's instance, which is
     * not in scope for it any more. Both lists start at 1. The probe that found this bug. */
    lh h = lh_make("<body><ol><li id=a>x</li></ol><ol><li id=b>y</li></ol></body>",
                   "body { margin: 0 } li::before { content: counters(list-item, '.') ':' }");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_BEFORE, "1:"));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_BEFORE, "1:"));
    lh_free(&h);
}

EMB_TEST(box_nested_quotes_alternate_by_depth)
{
    /* quotes pairs are used by nesting level: the outer quote is the first pair, the inner the
     * second. The close quotes come back in reverse. */
    lh h = lh_make("<body><div id=d><span id=s>x</span></div></body>",
                   "body { margin: 0 } #d { quotes: '[' ']' '<' '>' } "
                   "#d::before { content: open-quote } #d::after { content: close-quote } "
                   "#s::before { content: open-quote } #s::after { content: close-quote }");
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_BEFORE, "["));
    EMB_CHECK(pseudo_is(&h, "s", EMBI_PE_BEFORE, "<"));
    EMB_CHECK(pseudo_is(&h, "s", EMBI_PE_AFTER, ">"));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_AFTER, "]"));
    lh_free(&h);
}

EMB_TEST(box_quotes_default_pairs_are_typographic)
{
    /* quotes: auto is the typographic pair (U+201C and U+201D) at the outer level. */
    lh h = lh_make("<body><div id=d></div></body>",
                   "body { margin: 0 } #d::before { content: open-quote } #d::after { content: close-quote }");
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_BEFORE, "\xE2\x80\x9C"));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_AFTER, "\xE2\x80\x9D"));
    lh_free(&h);
}

EMB_TEST(box_ol_start_reversed_and_li_value)
{
    /* <ol start=5> numbers from 5; reversed counts down from the item count; <li value=10> sets the
     * item and the ones after it. The markers are the decimal style followed by ". ". */
    lh h = lh_make("<body><ol start=5><li id=a>x</li><li id=b>y</li></ol>"
                   "<ol reversed><li id=c>x</li><li id=d>y</li></ol>"
                   "<ol><li id=e value=10>x</li><li id=f>y</li></ol></body>",
                   "body { margin: 0 }");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "5. "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "6. "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "2. "));
    EMB_CHECK(pseudo_is(&h, "d", EMBI_PE_MARKER, "1. "));
    EMB_CHECK(pseudo_is(&h, "e", EMBI_PE_MARKER, "10. "));
    EMB_CHECK(pseudo_is(&h, "f", EMBI_PE_MARKER, "11. "));
    lh_free(&h);
}

EMB_TEST(box_list_markers_by_type)
{
    /* disc is a bullet followed by a space; none has no marker at all. */
    lh h = lh_make("<body><ul><li id=a>x</li><li id=b style='list-style-type: none'>y</li></ul>"
                   "<ul style='list-style-type: square'><li id=c>z</li></ul></body>",
                   "body { margin: 0 }");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "\xE2\x80\xA2 "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, ""));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "\xE2\x96\xAA "));
    lh_free(&h);
}

EMB_TEST(box_disclosure_markers_are_symbols_and_follow_direction)
{
    /* disclosure-open is a down-pointing triangle; disclosure-closed points right in ltr and left in
     * rtl (CSS Counter Styles 3). Symbols are followed by a space. */
    lh h = lh_make("<body><ul><li id=a style='list-style-type: disclosure-open'>x</li>"
                   "<li id=b style='list-style-type: disclosure-closed'>y</li></ul>"
                   "<ul style='direction: rtl'><li id=c style='list-style-type: disclosure-closed'>z</li></ul>"
                   "</body>",
                   "body { margin: 0 }");
    EMB_CHECK(pseudo_is(&h, "a", EMBI_PE_MARKER, "\xE2\x96\xBE "));
    EMB_CHECK(pseudo_is(&h, "b", EMBI_PE_MARKER, "\xE2\x96\xB8 "));
    EMB_CHECK(pseudo_is(&h, "c", EMBI_PE_MARKER, "\xE2\x97\x82 "));
    lh_free(&h);
}

static bool one_image(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    (void)user;
    (void)url;
    *w = 40;
    *h = 20;
    *has_ratio = true;
    return true;
}

EMB_TEST(box_url_content_and_list_image_become_replaced_boxes)
{
    /* A url() item of generated content is a replaced box of the image's natural size (here 40x20),
     * and so is a list-style-image marker. Without an image size no box is made. */
    embi_layout_config cfg = {nullptr, one_image, 0};
    lh h = lh_make_cfg("<body><div id=d></div><ul><li id=a style='list-style-image: url(x.png)'>x</li></ul>"
                       "</body>",
                       strlen("<body><div id=d></div><ul><li id=a style='list-style-image: url(x.png)'>x</li></ul></body>"),
                       "body { margin: 0 } #d::before { content: url(x.png) }", &cfg);
    const embi_box *d = (const embi_box *)lh_id(&h, "d")->box;
    const embi_box *img = lh_find_box(d, EMBI_BK_REPLACED, EMBI_PE_BEFORE);
    EMB_REQUIRE(img != nullptr);
    EMB_CHECK(near(img->rw, 40));
    EMB_CHECK(near(img->rh, 20));
    const embi_box *a = (const embi_box *)lh_id(&h, "a")->box;
    const embi_box *mk = lh_find_box(a, EMBI_BK_REPLACED, EMBI_PE_MARKER);
    EMB_CHECK(mk != nullptr);
    lh_free(&h);

    lh none = lh_make("<body><div id=d></div></body>", "body { margin: 0 } #d::before { content: url(x.png) }");
    EMB_CHECK(lh_find_box((const embi_box *)lh_id(&none, "d")->box, EMBI_BK_REPLACED, EMBI_PE_BEFORE) == nullptr);
    lh_free(&none);
}

EMB_TEST(box_form_controls_and_svg_have_their_sizes)
{
    /* Sizes of appearance: auto controls. An svg
     * with a viewBox and no size has the ratio only: it takes the available width (CSS 2.1 10.3.2). */
    lh h = lh_make("<body style='margin:0'><input id=c type=checkbox><select id=s></select>"
                   "<textarea id=t></textarea><progress id=p></progress><meter id=m></meter>"
                   "<input id=i1><input id=i2 type=number><input id=i3 type=color>"
                   "<input id=i4 type=date><input id=i5 type=file>"
                   "<input id=b1 type=submit><input id=b2 type=button><input id=b3 type=reset>"
                   "<button id=b4></button>"
#if EMBER_SVG
                   "<svg id=v viewBox='0 0 200 100'></svg><svg id=w width=50 viewBox='0 0 100 50'></svg>"
#endif
                   "</body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(border_size(&h, "c", &w, &hh));
    EMB_CHECK(near(w, 13) && near(hh, 13));
    /* The constants are border boxes (box-sizing: border-box from the UA sheet): the select's
     * padding and border are inside its 153 x 21; the UA min-inline-size and min-block-size (24px, html.css) lift an empty one to 24 x 24. */
    EMB_CHECK(border_size(&h, "s", &w, &hh));
    EMB_CHECK(near(w, 24) && near(hh, 24));
    EMB_CHECK(border_size(&h, "t", &w, &hh));
    /* W5e: a textarea is cols (20) times the advance of "0" in its own font, plus 8, wide (168 for a 0.6 em monospace
     * face at 13.333 px; the test font is narrower), and rows (2) lines of 15 px plus 6 high. */
    EMB_CHECK(w >= 120.0f && w <= 176.0f && near(hh, 36));
    EMB_CHECK(border_size(&h, "p", &w, &hh));
    EMB_CHECK(near(w, 160) && near(hh, 16));
    EMB_CHECK(border_size(&h, "m", &w, &hh));
    EMB_CHECK(near(w, 80) && near(hh, 16));
    /* Chromium: text-like inputs 177 x 21, color 50 x 27, date 116 x 21, file 253 x 21. */
    EMB_CHECK(border_size(&h, "i1", &w, &hh));
    EMB_CHECK(near(w, 177) && near(hh, 21));
    EMB_CHECK(border_size(&h, "i2", &w, &hh));
    EMB_CHECK(near(w, 177) && near(hh, 21));
    EMB_CHECK(border_size(&h, "i3", &w, &hh));
    EMB_CHECK(near(w, 50) && near(hh, 27));
    EMB_CHECK(border_size(&h, "i4", &w, &hh));
    EMB_CHECK(near(w, 116) && near(hh, 21));
    EMB_CHECK(border_size(&h, "i5", &w, &hh));
    EMB_CHECK(near(w, 253) && near(hh, 21));
    
    /* Buttons are sized by their label plus the native chrome (6px + 2px each side, 1px + 2px
     * above and below): Chromium gives "Submit" 58 x 21 and an empty button 16 x 21 with Arial
     * 13.333px; the test font is another, so the label width and line height are relative here. */
    float w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    EMB_CHECK(border_size(&h, "b1", &w, &hh));
    EMB_CHECK(border_size(&h, "b2", &w2, &h2));
    EMB_CHECK(border_size(&h, "b3", &w3, &h3));
    EMB_CHECK(near(w2, 16));
    EMB_CHECK(h2 > 6 && near(h2, hh) && near(h2, h3));
    EMB_CHECK(w > 16 + 10 && w3 > 16 + 10 && w != w3);
    EMB_CHECK(border_size(&h, "b4", &w, &hh));
    EMB_CHECK(near(w, 16) && near(hh, 6));
#if EMBER_SVG
    EMB_CHECK(border_size(&h, "v", &w, &hh));
    EMB_CHECK(near(w, 800) && near(hh, 400)); /* the available width (the 800 px viewport), the height from the ratio */
    EMB_CHECK(border_size(&h, "w", &w, &hh));
    EMB_CHECK(near(w, 50) && near(hh, 25));
#endif
    lh_free(&h);
}

/* The number of non-text boxes below b (b included) of element el with pseudo kind pe. */
static uint32_t count_pseudo_boxes(const embi_box *b, const embi_element *el, uint8_t pe)
{
    if (b == nullptr) {
        return 0;
    }
    uint32_t n = (b->el == el && b->pe == pe && b->kind != EMBI_BK_TEXT) ? 1 : 0;
    for (const embi_box *k = embi_box_first(b); k != nullptr; k = k->next) {
        n += count_pseudo_boxes(k, el, pe);
    }
    return n;
}

EMB_TEST(box_display_contents_has_no_box_and_one_pseudo)
{
    /* display: contents generates no box for the element; its ::before is generated once, not twice. */
    lh h = lh_make("<body><div id=d style='display: contents'><span id=s>x</span></div></body>",
                   "body { margin: 0 } #d::before { content: 'A' } #d::after { content: 'B' }");
    embi_element *d = lh_id(&h, "d");
    EMB_REQUIRE(d != nullptr);
    EMB_CHECK(d->box == nullptr);
    /* Count the pseudo boxes themselves: their text children carry the same element and pseudo kind. */
    EMB_CHECK_EQ_INT(count_pseudo_boxes(lh_root_box(&h), d, EMBI_PE_BEFORE), 1);
    EMB_CHECK_EQ_INT(count_pseudo_boxes(lh_root_box(&h), d, EMBI_PE_AFTER), 1);
    EMB_CHECK(lh_id(&h, "s") != nullptr && lh_id(&h, "s")->box != nullptr);
    lh_free(&h);
}

EMB_TEST(box_inline_split_by_a_block_keeps_every_piece)
{
    /* CSS 2.1 9.2.1.1: the span becomes two inline pieces around the block, and layout reports the
     * union of the pieces (CSSOM getBoundingClientRect), with one client rect per piece. */
    lh h = lh_make("<body style='margin:0'><span id=sp><canvas width=10 height=10></canvas>"
                   "<div style='height: 30px'></div><canvas width=10 height=10></canvas></span></body>",
                   "");
    embi_element *sp = lh_id(&h, "sp");
    EMB_REQUIRE(sp != nullptr);
    embi_rectf rects[8];
    uint32_t n = embi_layout_client_rects(h.eng, sp, rects, 8);
    EMB_CHECK_EQ_INT(n, 2);
    embi_rectf u;
    EMB_REQUIRE(embi_layout_border_box(h.eng, sp, &u));
    /* The union covers the block between the two pieces. */
    EMB_CHECK(u.h >= 30);
    lh_free(&h);
}

EMB_TEST(box_flex_and_grid_wrap_text_and_drop_blank_runs)
{
    /* CSS Flexbox 1 section 4 and CSS Grid 2 section 6.1: a text run becomes an anonymous item; a
     * whitespace-only run is dropped. */
    lh h = lh_make("<body><div id=f style='display:flex'>x<span id=s></span></div>"
                   "<div id=g style='display:flex'>\n  <span id=t></span>\n</div>"
                   "<div id=k style='display:grid'>x<span id=u></span></div></body>",
                   "body { margin: 0 }");
    const embi_box *f = (const embi_box *)lh_id(&h, "f")->box;
    EMB_REQUIRE(f->first != nullptr);
    EMB_CHECK((f->first->flags & EMBI_BF_ANON) != 0);
    EMB_CHECK(f->first->next == (const embi_box *)lh_id(&h, "s")->box);
    /* The style engine blockifies display for flex items, so the span is block-level already. */
    EMB_CHECK(f->first->next->kind == EMBI_BK_BLOCK);
    EMB_CHECK((f->first->next->flags & EMBI_BF_INLINE_LEVEL) == 0);
    const embi_box *g = (const embi_box *)lh_id(&h, "g")->box;
    EMB_REQUIRE(g->first != nullptr);
    EMB_CHECK(g->first == (const embi_box *)lh_id(&h, "t")->box);
    EMB_CHECK(g->first->next == nullptr);
    const embi_box *k = (const embi_box *)lh_id(&h, "k")->box;
    EMB_CHECK((k->first->flags & EMBI_BF_ANON) != 0);
    lh_free(&h);
}

EMB_TEST(box_out_of_flow_boxes_are_not_wrapped)
{
    /* CSS 2.1 9.2.1.1: a float beside a block stays a child of the block, not in an anonymous one. */
    lh h = lh_make("<body><div id=d><div id=fl style='float: left; width: 10px'></div><div></div></div></body>",
                   "body { margin: 0 }");
    const embi_box *d = (const embi_box *)lh_id(&h, "d")->box;
    EMB_REQUIRE(d->first != nullptr);
    EMB_CHECK(d->first == (const embi_box *)lh_id(&h, "fl")->box);
    EMB_CHECK((d->first->flags & EMBI_BF_ANON) == 0);
    lh_free(&h);
}

EMB_TEST(box_text_transform_maps_non_ascii_through_the_case_tables)
{
    /* text-transform: uppercase uses the Unicode default mapping: e-acute and the others map, not only
     * ASCII. "été" becomes "ÉTÉ". */
    lh h = lh_make("<body><div id=d style='text-transform: uppercase'>\xC3\xA9t\xC3\xA9</div>"
                   "<div id=e style='text-transform: lowercase'>\xC3\x89T\xC3\x89</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&h, "d", "\xC3\x89T\xC3\x89"));
    EMB_CHECK(text_is(&h, "e", "\xC3\xA9t\xC3\xA9"));
    lh_free(&h);
}

EMB_TEST(box_capitalize_continues_a_word_across_inline_boxes)
{
    /* CSS Text 3 section 2.1: "hel<b>lo</b>" is one word, capitalised once. */
    lh h = lh_make("<body><div id=d style='text-transform:capitalize'>hel<b id=b>lo</b> wor</div>"
                   "<div id=e style='text-transform:capitalize'>a<br>b <i id=i>c</i>d</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&h, "d", "Hel Wor"));
    EMB_CHECK(text_is(&h, "b", "lo"));
    /* A break ends the word, and so does a space: "c" starts one and "d" continues it. */
    EMB_CHECK(text_is(&h, "e", "AB d"));
    EMB_CHECK(text_is(&h, "i", "C"));
    lh_free(&h);
    /* An atomic inline ends the word too. */
    lh a = lh_make("<body><div id=d style='text-transform:capitalize'>x<span style='display:inline-block'>y</span>z</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&a, "d", "XZ"));
    lh_free(&a);
}

EMB_TEST(box_out_of_flow_boxes_do_not_end_a_capitalized_word)
{
    lh h = lh_make("<body><div id=d style='text-transform:capitalize'>hel<span style='float:left'></span>lo "
                   "wor<span style='position:absolute'></span>ld</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&h, "d", "Hello World"));
    lh_free(&h);
}

EMB_TEST(box_button_chrome_comes_from_the_ua_sheet_so_resets_win)
{
    lh h = lh_make("<body style='margin:0'><button id=a>x</button>"
                   "<button id=b style='border:0;padding:0'>x</button>"
                   "<span id=s style='display:inline-block'>x</span></body>",
                   "");
    float wa = 0, ha = 0, wb = 0, hb = 0, ws = 0, hs = 0;
    EMB_CHECK(border_size(&h, "a", &wa, &ha));
    EMB_CHECK(border_size(&h, "b", &wb, &hb));
    EMB_CHECK(border_size(&h, "s", &ws, &hs));
    /* The reset button is the bare label; the plain one adds 2px + 6px and 2px + 1px per side. */
    EMB_CHECK(near(wb, ws) && near(hb, hs));
    EMB_CHECK(near(wa, ws + 16) && near(ha, hs + 6));
    float bdr[4], pad[4], mg[4];
    EMB_CHECK(embi_layout_box_edges(h.eng, lh_id(&h, "b"), bdr, pad, mg));
    EMB_CHECK(near(bdr[0] + bdr[1] + bdr[2] + bdr[3] + pad[0] + pad[1] + pad[2] + pad[3], 0));
    lh_free(&h);
}

EMB_TEST(box_white_space_discard_removes_collapsible_characters)
{
    /* white-space-collapse: discard removes the spaces and newlines altogether. */
    lh h = lh_make("<body><div id=d style='white-space-collapse: discard'>a  b\n c</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&h, "d", "abc"));
    lh_free(&h);
}

EMB_TEST(box_newline_between_wide_characters_is_removed)
{
    /* CSS Text 3 section 4.1.2: a segment break between two East Asian wide characters disappears;
     * between Latin letters it is a space. */
    lh h = lh_make("<body><div id=d>\xE4\xB8\xAD\n\xE6\x96\x87</div><div id=e>a\nb</div></body>",
                   "body { margin: 0 }");
    EMB_CHECK(text_is(&h, "d", "\xE4\xB8\xAD\xE6\x96\x87"));
    EMB_CHECK(text_is(&h, "e", "a b"));
    lh_free(&h);
}

EMB_TEST(box_margin_collapse_does_not_change_the_fragment_margins)
{
    /* The fragment keeps the element's own margins (getComputedStyle), while the collapsed margins are
     * used for positions only: a 5px margin on the parent with a 40px child reports 5 for the parent. */
    lh h = lh_make("<body><div id=p style='margin-top: 5px'><div id=c style='margin-top: 40px; height: 10px'></div></div></body>",
                   "body { margin: 0 }");
    float bdr[4], pad[4], mg[4];
    EMB_REQUIRE(embi_layout_box_edges(h.eng, lh_id(&h, "p"), bdr, pad, mg));
    EMB_CHECK(near(mg[1], 5));
    EMB_CHECK(embi_layout_box_edges(h.eng, lh_id(&h, "c"), bdr, pad, mg));
    EMB_CHECK(near(mg[1], 40));
    embi_rectf p;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "p"), &p));
    EMB_CHECK(near(p.y, 40));
    lh_free(&h);
}

EMB_TEST(box_form_control_with_a_specified_size_and_content_box_sizing)
{
    /* With auto sizes the border box is the constant. A specified width is the border box too
     * (border-box), and the other axis keeps its constant: controls have no aspect ratio. With
     * box-sizing: content-box the constant is the content size, so padding and border add to it. */
    lh h = lh_make("<body style='margin:0'>"
                   "<select id=a style='width: 100px'></select>"
                   "<textarea id=b style='box-sizing: content-box; padding: 5px; border: 2px solid'></textarea>"
                   "</body>",
                   "");
    float w = 0, hh = 0;
    EMB_CHECK(border_size(&h, "a", &w, &hh));
    EMB_CHECK(near(w, 100) && near(hh, 24));
    EMB_CHECK(border_size(&h, "b", &w, &hh));
    EMB_CHECK(w >= 120.0f + 14 && w <= 176.0f + 14 && near(hh, 36 + 14)); /* the width as above, plus padding and border */
    lh_free(&h);
}

EMB_TEST_MAIN()
