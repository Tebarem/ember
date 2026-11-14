/*
 * Inline layout (W3c, inline.c and line.c): forced breaks, preserved newlines, bidi placement, box edges
 * across lines, atomic inline sizes, text-overflow, soft hyphens, and the memory the layout frees. Each
 * case lays a small document out and looks for the expected lines in the fragment dump (layout_harness.h),
 * with the values worked out by hand from the specs (Ahem: 20px per glyph, 20px line-height). Both these
 * expectations and the fixtures in tests/layout/inline were derived by hand from the specs; none has
 * been compared with Chromium yet (re-verify them there before relying on them as ground truth).
 */
#include "layout_harness.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>

/* The dump of the document, as a NUL-terminated string (free with embi_sb_free). */
static embi_sb dump_of(lh *h)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_layout_dump(h->eng, h->doc, &sb);
    embi_sb_append(&sb, (embi_str){"", 1});
    return sb;
}

/* Whether the dump contains needle. */
static bool has(const embi_sb *sb, const char *needle)
{
    return sb->data != nullptr && strstr(sb->data, needle) != nullptr;
}

/* The number of LINE fragments in the dump. */
static uint32_t line_count(const embi_sb *sb)
{
    uint32_t n = 0;
    const char *p = sb->data;
    while (p != nullptr && (p = strstr(p, "line <")) != nullptr) {
        n++;
        p += 6;
    }
    return n;
}

static const char *ahem_css =
    "@font-face { font-family: Ahem; src: url(Ahem.ttf); } .a { font: 20px/20px Ahem }";

/* The font loader: every @font-face source is the Ahem test font (1em advances, 0.8em ascent). */
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

/* The harness's lh_make with the Ahem loader, so the test font is registered. */
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

/* A full line break in the middle of a line (CSS Text 3 5, HTML br): width 80, the words break at the space,
 * and br ends the line before ZZ: XX, YY, ZZ, three lines of 20px. */
EMB_TEST(inline_br_forces_break_after_wrapped_words)
{
    lh h = make_ahem("<div id=d class=a style='width:80px'>XX YY<br>ZZ</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 3);
    EMB_CHECK(has(&sb, "text <> at (0, 40) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A br with no wrapping before it: two lines, the second at y 20. */
EMB_TEST(inline_bare_br_makes_two_lines)
{
    lh h = make_ahem("<div id=d class=a>XX<br>YY</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* Each br gives a line; two in a row give an empty line of the strut's height (in the middle). */
EMB_TEST(inline_two_brs_make_an_empty_line)
{
    lh h = make_ahem("<div id=d class=a><span>XX</span><br><br><span>YY</span></div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 3);
    EMB_CHECK(has(&sb, "text <> at (0, 40) 40x20 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A preserved newline in pre is a forced break (the text module's paragraph has several paragraphs, and the
 * shaping of such a run must still succeed). */
EMB_TEST(inline_pre_newline_breaks_the_line)
{
    lh h = make_ahem("<div id=d class=a style='white-space:pre'>XX\nYY</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* Shaping a run with a line feed works (it has two paragraphs; its breaks and advances still come out). */
EMB_TEST(text_shim_shapes_a_run_with_a_line_feed)
{
    embi_text *t = embi_tshim_create();
    embi_str fams[1] = {{"Ahem", 4}};
    embi_lay_tstyle ts;
    memset(&ts, 0, sizeof ts);
    ts.families = fams;
    ts.family_count = 1;
    ts.size = 20.0f;
    ts.weight = 400.0f;
    ts.stretch = 100.0f;
    ts.optical_sizing = true;
    embi_lay_shaped *s = embi_tshim_shape(t, "XX\nYY", 5, &ts);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK_EQ_INT((int)embi_tshim_len(s), 5);
    EMB_CHECK(embi_tshim_measure(s, 0, 2) > 0.0f);
    EMB_CHECK(embi_tshim_measure(s, 3, 5) > 0.0f);
    embi_tshim_free(s);
    embi_tshim_destroy(t);
}

/* A wide line before a br: width 60, XX XX XX takes three lines, then YY after the br: four lines. */
EMB_TEST(inline_wide_line_before_br)
{
    lh h = make_ahem("<div id=d class=a style='width:60px'>XX XX XX<br>YY</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 4);
    EMB_CHECK(has(&sb, "text <> at (0, 60) 40x20 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* pre-wrap: the spaces before a preserved newline hang, so XX fits on its line. */
EMB_TEST(inline_pre_wrap_hangs_spaces_before_newline)
{
    lh h = make_ahem("<div id=d class=a style='width:100px;white-space:pre-wrap'>XX  \nZZ</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* An atomic inline with margin-bottom sits on its margin edge: the line is 34px, the box at y 0. */
EMB_TEST(inline_atomic_baseline_is_bottom_margin_edge)
{
    lh h = make_ahem("<div id=d class=a style='width:100px'><span style='display:inline-block;width:20px;"
                   "height:20px;margin-bottom:10px'></span></div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "box <div> id=d class=a at (0, 0) 100x34"));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* text-align right with overflow starts the content at the start edge (CSS Text 3 7.1). */
EMB_TEST(inline_right_aligned_overflow_starts_at_start_edge)
{
    lh h = make_ahem("<div id=d class=a style='width:40px;text-align:right;white-space:nowrap'>"
                   "<span>XXXX</span></div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 80x20 \"XXXX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* An rtl inline box: its padding-left and border-left are its end edge and are drawn on the left. */
EMB_TEST(inline_rtl_box_keeps_physical_left_padding)
{
    lh h = make_ahem("<div id=d class=a style='width:100px;direction:rtl'>"
                   "<span style='padding-left:10px;border-left:4px solid'>XX</span></div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "box <span> at (46, 0) 54x20"));
    EMB_CHECK(has(&sb, "text <> at (60, 0) 40x20 \"XX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* An empty inline with padding makes a box of its padding's width. */
EMB_TEST(inline_empty_inline_with_padding_makes_a_box)
{
    lh h = make_ahem("<div id=d class=a style='width:100px'><span style='padding-left:10px'></span>XX</div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "box <span> at (0, 0) 10x20"));
    EMB_CHECK(has(&sb, "text <> at (10, 0) 40x20 \"XX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* text-overflow: ellipsis on a clipped nowrap line: one line, XX then the ellipsis. */
EMB_TEST(inline_ellipsis_truncates_one_line)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis;"
                   "white-space:nowrap'>XXXXXXXX</div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 1);
    /* 60px: XX (40px) then the ellipsis character (20px). */
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (40, 0) 20x20 \"\xE2\x80\xA6\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* text-overflow-009: an atomic inline wider than the box that starts the line stays (clipped); the ellipsis is not all
 * the line shows. A literal U+FFFC character in the text (written as an escape) is no atomic and is ellipsised. */
EMB_TEST(inline_ellipsis_keeps_an_atomic_first_item)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap'>"
                     "<span id=b style='display:inline-block;width:80px;height:20px'></span>XX</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "at (0, 0) 80x20"));
    embi_sb_free(&sb);
    lh_free(&h);
}

EMB_TEST(inline_ellipsis_does_not_keep_a_literal_object_replacement_character)
{
    lh h = make_ahem("<div id=d class=a style='width:25px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap'>"
                     "\xEF\xBF\xBCXXXX</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "\"\xE2\x80\xA6\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A soft hyphen that a line breaks at shows its hyphen at the end of the line. */
EMB_TEST(inline_soft_hyphen_shows_hyphen_at_break)
{
    lh h = make_ahem("<div id=d class=a style='width:100px'>XXXX&shy;XXXX</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (80, 0) 20x20 \"-\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* text-wrap: pretty leaves no one-word last line (XX / XX XX rather than XX XX / XX). */
EMB_TEST(inline_pretty_avoids_one_word_last_line)
{
    lh h = make_ahem("<div id=d class=a style='width:100px;text-wrap:pretty'>XX XX XX</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 100x20 \"XX XX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* Inline layout frees what it allocates: the live bytes after a layout and its teardown equal those before
 * (after a warm-up pass, which fills the process-wide caches once). */
EMB_TEST(inline_layout_teardown_returns_memory)
{
    const char *html = "<div id=d class=a style='width:80px'>XX YY<br>ZZ <span style='padding:2px'>QQ"
                       " RR</span> <span style='display:inline-block;width:10px'></span></div>";
    lh warm = make_ahem(html, ahem_css);
    lh_free(&warm);
    size_t before = embi_mem_live();
    lh h = make_ahem(html, ahem_css);
    lh_free(&h);
    size_t after = embi_mem_live();
    EMB_CHECK_EQ_INT((int)after, (int)before);
}

/* How many times needle occurs in the dump. */
static uint32_t count_of(const embi_sb *sb, const char *needle)
{
    uint32_t n = 0;
    const char *p = sb->data;
    while (p != nullptr && (p = strstr(p, needle)) != nullptr) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* An inline box that opens in the clipped tail of an ellipsised word and closes on a later line: the
 * box is open on the next line, so its text there gets the box's fragment. Width 60: line 1 is XX and
 * the ellipsis (the word XXXXXXXXYY overflows, the next break is after the space), line 2 is ZZ. */
EMB_TEST(inline_ellipsis_box_opened_in_clipped_tail_continues)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis'>"
                   "XXXXXXXX<b>YY ZZ</b></div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (40, 0) 20x20 \"\xE2\x80\xA6\""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"ZZ\""));
    /* The b has one fragment, on line 2 (a skipped OPEN would leave it without one). */
    EMB_CHECK_EQ_INT(count_of(&sb, "box <b>"), 1);
    EMB_CHECK(has(&sb, "box <b> at (0, 20) 40x20"));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A box that opens before the cut and closes inside the clipped tail must not stay open (a phantom
 * continuation on the next line). Line 1: X, then the b holds the second X and the ellipsis follows;
 * the rest of the b, with its close, is clipped. Line 2 is ZZ, outside the b. */
EMB_TEST(inline_ellipsis_box_closed_in_clipped_tail_does_not_continue)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis'>"
                   "X<b>XXXXXXXX</b> ZZ</div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"ZZ\""));
    EMB_CHECK_EQ_INT(count_of(&sb, "box <b>"), 1); /* line 1 only */
    EMB_CHECK(has(&sb, "box <b> at (20, 0)"));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A float in the clipped tail of an ellipsised word is still placed. It follows a word (80px) that already fills the
 * 60px line, so it does not fit beside the line content and goes below the line (CSS 2.1 section 9.5.1, as a float after
 * an overflowing word does in browsers): the line stays at the top with XX and the ellipsis, and the float is at y 20. */
EMB_TEST(inline_ellipsis_float_in_clipped_tail_is_placed)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis'>"
                   "XXXXXXXX<i style='float:left;width:10px;height:10px'></i>YY</div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "box <i> at (0, 20) 10x10"));
    EMB_CHECK(has(&sb, "line <div> id=d class=a at (0, 0) 60x20"));
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (40, 0) 20x20 \"…\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* word-break: keep-all removes the opportunities between CJK characters (CSS Text 3 section 5.2); the
 * normal mode breaks between them. Six ideographs in 50px: several lines, then one. */
EMB_TEST(inline_keep_all_keeps_cjk_together)
{
    const char *cjk = "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94\xE5\x85\xAD";
    char html[400];
    SDL_snprintf(html, sizeof html, "<div id=d class=a style='width:50px'>%s</div>", cjk);
    lh h = make_ahem(html, ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(line_count(&sb) > 1);
    embi_sb_free(&sb);
    lh_free(&h);
    SDL_snprintf(html, sizeof html, "<div id=d class=a style='width:50px;word-break:keep-all'>%s</div>", cjk);
    h = make_ahem(html, ahem_css);
    sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 1);
    embi_sb_free(&sb);
    lh_free(&h);
    /* A space is still an opportunity under keep-all. */
    SDL_snprintf(html, sizeof html, "<div id=d class=a style='width:50px;word-break:keep-all'>%s %s</div>",
                 cjk, cjk);
    h = make_ahem(html, ahem_css);
    sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    embi_sb_free(&sb);
    lh_free(&h);
}

/* overflow-wrap: anywhere breaks a long word where it overflows: XXXXX in 60px gives XXX / XX. */
EMB_TEST(inline_overflow_wrap_anywhere_breaks_the_word)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;overflow-wrap:anywhere'>XXXXX</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 60x20 \"XXX\""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"XX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* white-space: break-spaces: both spaces of "XX  YY" are kept and a line may break after each: "XX "
 * then " YY" (CSS Text 3 section 4.1.3). With pre-wrap the spaces hang and YY starts line 2 at 0. */
EMB_TEST(inline_break_spaces_breaks_between_spaces)
{
    lh h = make_ahem("<div id=d class=a style='width:60px;white-space:break-spaces'>XX  YY</div>", ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 60x20 \"XX \""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 60x20 \" YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
    h = make_ahem("<div id=d class=a style='width:60px;white-space:pre-wrap'>XX  YY</div>", ahem_css);
    sb = dump_of(&h);
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* A line box with mixed font sizes: the strut (20px, line-height 1.5 = 30px) and a 40px span (60px
 * tall) make a 60px line (CSS 2.1 10.8). The span's box is its font's 40px content area, not its 60px
 * inline box, and the first span's is 20px. */
EMB_TEST(inline_line_box_with_mixed_font_sizes)
{
    lh h = make_ahem("<div id=d style='font:20px/1.5 Ahem;width:300px'><span>XX</span>"
                   "<span style='font-size:40px'>YY</span></div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "line <div> id=d at (0, 0) 300x60"));
    EMB_CHECK(has(&sb, "box <span> at (0, 26) 40x20"));
    EMB_CHECK(has(&sb, "box <span> at (40, 10) 80x40"));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* vertical-align keywords on a 10px child of a 20px Ahem parent (ascent 16, descent 4), with the
 * positions of the child's box and the height of the line (CSS 2.1 10.8.1). The half-leading of a
 * fixed line-height is part of the inline box that text-top and text-bottom align (WPT reftests
 * vertical-align-117a, -118a, -negative-leading-001); the derivations are in the layout fixtures
 * vertical-align-text-top-fixed-lh and -text-bottom-fixed-lh. With line-height normal there is no leading
 * and the child's box sits on the parent's content area edge. */
EMB_TEST(inline_vertical_align_keywords)
{
    static const struct {
        const char *kw;
        const char *lh;
        const char *box;   /* the dump line of the child's box */
        const char *line;  /* the dump line of the line box */
    } cases[] = {
        {"text-top", "normal", "box <span> at (40, 0) 20x10", "line <div> id=d at (0, 0) 300x20"},
        {"text-bottom", "normal", "box <span> at (40, 10) 20x10", "line <div> id=d at (0, 0) 300x20"},
        {"text-top", "40px", "box <span> at (40, 25) 20x10", "line <div> id=d at (0, 0) 300x50"},
        {"text-bottom", "40px", "box <span> at (40, 15) 20x10", "line <div> id=d at (0, 0) 300x50"},
        {"middle", "20px", "box <span> at (40, 5) 20x10", "line <div> id=d at (0, 0) 300x22"},
        {"sub", "20px", "box <span> at (40, 13) 20x10", "line <div> id=d at (0, 0) 300x28"},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char html[400];
        SDL_snprintf(html, sizeof html,
                     "<div id=d style='font:20px/%s Ahem;width:300px'>XX<span style='vertical-align:%s;"
                     "font-size:10px'>YY</span></div>",
                     cases[i].lh, cases[i].kw);
        lh h = make_ahem(html, ahem_css);
        embi_sb sb = dump_of(&h);
        if (!has(&sb, cases[i].box) || !has(&sb, cases[i].line)) {
            SDL_Log("case %s/%s\n%s", cases[i].kw, cases[i].lh, sb.data);
        }
        EMB_CHECK(has(&sb, cases[i].box));
        EMB_CHECK(has(&sb, cases[i].line));
        embi_sb_free(&sb);
        lh_free(&h);
    }
}

/* text-wrap: balance with a text-indent: the first line has the indent taken off, so the balanced width
 * must leave room for it. Four words in 200px with a 40px indent: greedy gives XX XX XX / XX, balance
 * gives two lines of two words (the narrowest width is 140 = 40 + 100), the first after the indent. */
EMB_TEST(inline_balance_counts_the_text_indent)
{
    lh h = make_ahem("<div id=d class=a style='width:200px;text-wrap:balance;text-indent:40px'>"
                   "XX XX XX XX</div>",
                   ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (40, 0) 100x20 \"XX XX\""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 100x20 \"XX XX\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* The paths that allocate boxes and shaped runs of their own (the ellipsis, the hyphen, floats, balance
 * and pretty, break-spaces, justify) free them all too. */
EMB_TEST(inline_special_paths_return_memory)
{
    static const char *docs[] = {
        "<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis'>XXXXXXXX<b>YY ZZ</b></div>",
        "<div id=d class=a style='width:60px;overflow:hidden;text-overflow:ellipsis'>XXXXXXXX"
        "<i style='float:left;width:10px;height:10px'></i>YY</div>",
        "<div id=d class=a style='width:100px'>XXXX&shy;XXXX&shy;XXXX</div>",
        "<div id=d class=a style='width:100px'><span style='float:right;width:30px;height:10px'></span>"
        "XX XX XX XX</div>",
        "<div id=d class=a style='width:200px;text-wrap:balance;text-indent:20px'>XX XX XX XX XX</div>",
        "<div id=d class=a style='width:100px;text-wrap:pretty'>XX XX XX</div>",
        "<div id=d class=a style='width:60px;white-space:break-spaces'>XX  YY</div>",
        "<div id=d class=a style='width:100px;text-align:justify'>XX <b>XX</b> XX XX XX</div>",
    };
    lh warm = make_ahem(docs[0], ahem_css);
    lh_free(&warm);
    for (uint32_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        lh w = make_ahem(docs[i], ahem_css);
        lh_free(&w);
        size_t before = embi_mem_live();
        lh h = make_ahem(docs[i], ahem_css);
        lh_free(&h);
        size_t after = embi_mem_live();
        if (after != before) {
            SDL_Log("document %u leaks %d bytes", (unsigned)i, (int)after - (int)before);
        }
        EMB_CHECK_EQ_INT((int)after, (int)before);
    }
}

/* text-align: justify spreads the extra room over the spaces of every line but the last (CSS Text 3 7.3):
 * "XX YY ZZ XX YY" in 200px breaks after ZZ; the first line is 160px of text with two spaces, so each space
 * takes 20px more (40px), and XX YY ZZ land at 0, 80, 160. The last line stays start-aligned. */
EMB_TEST(inline_justify_spreads_the_spaces)
{
    lh h = make_ahem("<div id=d class=a style='width:200px;text-align:justify'>XX YY ZZ XX YY</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (80, 0) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "text <> at (160, 0) 40x20 \"ZZ\""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 100x20 \"XX YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* text-align center and right with a text-indent: the indent is part of the first line, so the line
 * of 100px sits in the 160px left after the 40px indent: centered at 40 + 30, right at 200 - 100. */
EMB_TEST(inline_center_and_right_count_the_indent)
{
    lh c = make_ahem("<div id=d class=a style='width:200px;text-align:center;text-indent:40px'>XX YY</div>",
                     ahem_css);
    embi_sb sb = dump_of(&c);
    EMB_CHECK(has(&sb, "text <> at (70, 0) 100x20 \"XX YY\""));
    embi_sb_free(&sb);
    lh_free(&c);
    lh r = make_ahem("<div id=d class=a style='width:200px;text-align:right;text-indent:40px'>XX YY</div>",
                     ahem_css);
    sb = dump_of(&r);
    EMB_CHECK(has(&sb, "text <> at (100, 0) 100x20 \"XX YY\""));
    embi_sb_free(&sb);
    lh_free(&r);
}

/* W3q: text-indent is read once per paragraph (line_indent), not per line: the first line only, every line but the first
 * (hanging), and the line after a forced break too (each-line) must still come out right on a many-line paragraph. Ahem
 * 20px glyphs, 70px container: every word is a line of its own. */
EMB_TEST(inline_text_indent_applies_to_the_right_lines_of_a_paragraph)
{
    lh a = make_ahem("<div id=d class=a style='width:70px;text-indent:20px'>XX YY ZZ</div>", ahem_css);
    embi_sb sb = dump_of(&a);
    EMB_CHECK(has(&sb, "text <> at (20, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (0, 20) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "text <> at (0, 40) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&a);
    lh b = make_ahem("<div id=d class=a style='width:70px;text-indent:20px hanging'>XX YY ZZ</div>", ahem_css);
    sb = dump_of(&b);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (20, 20) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "text <> at (20, 40) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&b);
    lh c = make_ahem("<div id=d class=a style='width:70px;text-indent:20px each-line'>XX<br>YY ZZ</div>", ahem_css);
    sb = dump_of(&c);
    EMB_CHECK(has(&sb, "text <> at (20, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (20, 20) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "text <> at (0, 40) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&c);
}

/* Bidi: a bidi-override rtl span inside an ltr paragraph shows its two runs in reverse order
 * (XX, then BB before AA, then YY), and an ltr override inside an rtl paragraph is one left-to-right
 * run placed at the right edge (160px of text in 200px starts at 40). */
EMB_TEST(inline_bidi_reorders_runs)
{
    lh a = make_ahem("<div id=d class=a style='width:300px'>XX<span style='direction:rtl;"
                     "unicode-bidi:bidi-override'>AA<i>BB</i></span>YY</div>",
                     ahem_css);
    embi_sb sb = dump_of(&a);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (40, 0) 40x20 \"BB\""));
    EMB_CHECK(has(&sb, "text <> at (80, 0) 40x20 \"AA\""));
    EMB_CHECK(has(&sb, "text <> at (120, 0) 40x20 \"YY\""));
    /* The inline box covers exactly its own text, whatever the visual order. */
    EMB_CHECK(has(&sb, "box <i> at (40, 0) 40x20"));
    embi_sb_free(&sb);
    lh_free(&a);
    lh a2 = make_ahem("<div id=d class=a style='width:300px'>XX<span style='direction:rtl;"
                      "unicode-bidi:bidi-override'><i>BB</i>AA</span>YY</div>",
                      ahem_css);
    sb = dump_of(&a2);
    EMB_CHECK(has(&sb, "text <> at (40, 0) 40x20 \"AA\""));
    EMB_CHECK(has(&sb, "text <> at (80, 0) 40x20 \"BB\""));
    EMB_CHECK(has(&sb, "box <i> at (80, 0) 40x20"));
    embi_sb_free(&sb);
    lh_free(&a2);
    /* An inline box whose text falls in two visual runs gets one box fragment per run. In the rtl
     * paragraph the Hebrew letters are level 1 and the Latin XXYY is one level-2 run to their left,
     * so the box (Hebrew B, then XX) is two runs: XX, and B after the YY that is not inside it. */
    lh a3 = make_ahem("<div id=d class=a style='width:300px;direction:rtl'>\xD7\x90<i>\xD7\x91" "XX</i>YY</div>",
                      ahem_css);
    sb = dump_of(&a3);
    EMB_CHECK(has(&sb, "text <> at (180, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (220, 0) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "box <i> at (180, 0) 40x20"));
    EMB_CHECK(has(&sb, "box <i> at (260, 0) 20x20"));
    EMB_CHECK(!has(&sb, "box <i> at (180, 0) 120x20"));
    embi_sb_free(&sb);
    lh_free(&a3);
    lh b = make_ahem("<div id=d class=a style='width:200px;direction:rtl'>XX <span style='direction:ltr;"
                     "unicode-bidi:bidi-override'>AB</span> YY</div>",
                     ahem_css);
    sb = dump_of(&b);
    EMB_CHECK(has(&sb, "text <> at (40, 0) 60x20 \"XX \""));
    EMB_CHECK(has(&sb, "text <> at (100, 0) 40x20 \"AB\""));
    EMB_CHECK(has(&sb, "text <> at (140, 0) 60x20 \" YY\""));
    embi_sb_free(&sb);
    lh_free(&b);
}

/* An inline box's BOX fragment is its border box: a trailing margin is outside it, in logical order
 * and on a reordered line. */
EMB_TEST(inline_box_fragment_excludes_end_margin)
{
    lh a = make_ahem("<div id=d class=a style='width:300px'>XX<i style='margin-right:10px;padding:5px'>BB"
                     "</i>YY</div>",
                     ahem_css);
    embi_sb sb = dump_of(&a);
    EMB_CHECK(has(&sb, "text <> at (45, 0) 40x20 \"BB\""));
    EMB_CHECK(has(&sb, "box <i> at (40, -5) 50x30"));
    embi_sb_free(&sb);
    lh_free(&a);
    lh b = make_ahem("<div id=d class=a style='width:300px'>XX<span style='direction:rtl;"
                     "unicode-bidi:bidi-override'>AA<i style='margin:0 10px;padding:0 5px'>BB</i></span>"
                     "YY</div>",
                     ahem_css);
    sb = dump_of(&b);
    EMB_CHECK(has(&sb, "box <i> at (50, 0) 50x20"));
    embi_sb_free(&sb);
    lh_free(&b);
}

/* A fixed line-height with a font whose ascent is not whole: 16px Ahem has ascent 12.8 and descent 3.2,
 * rounded to 13 and 3 (a content area of 16), so the 30px line has a half-leading of 7 and the baseline
 * falls on y 20. The text fragment spans the content area (y 7, height 16), the 10px span its own (8 + 2,
 * at y 12), and the span's 30px line-height makes the line 32px tall. */
EMB_TEST(inline_fixed_line_height_baseline_is_whole)
{
    lh h = make_ahem("<div id=d style='font:16px/30px Ahem;width:200px'>XX<span style='font-size:10px'>YY"
                     "</span></div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "line <div> id=d at (0, 0) 200x32"));
    EMB_CHECK(has(&sb, "text <> at (0, 7) 32x16 \"XX\""));
    EMB_CHECK(has(&sb, "box <span> at (32, 12) 20x10"));
    EMB_CHECK(has(&sb, "text <> at (32, 12) 20x10 \"YY\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* Four-byte characters outside the CJK extensions (emoji) are not ideographic: justified text of them
 * keeps each line a single run, with no space opened between the characters. */
EMB_TEST(inline_justify_does_not_split_emoji)
{
    lh h = make_ahem("<div id=d class=a style='width:100px;text-align:justify'>"
                     "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
                     "\xF0\x9F\x98\x80\xF0\x9F\x98\x80</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK_EQ_INT(line_count(&sb), 2);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 100x20 \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
                       "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\""));
    embi_sb_free(&sb);
    lh_free(&h);
}

/* unicode-bidi: plaintext takes the direction of its first strong character (an FSI), not the box's
 * direction: the Hebrew start makes the span rtl inside the ltr paragraph, so the Latin YY after it
 * is drawn before the Hebrew. */
EMB_TEST(inline_bidi_plaintext_takes_the_first_strong_direction)
{
    lh h = make_ahem("<div id=d class=a style='width:300px'>XX<span style='unicode-bidi:plaintext'>"
                     "\xD7\x90\xD7\x91 YY</span>ZZ</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "text <> at (0, 0) 40x20 \"XX\""));
    EMB_CHECK(has(&sb, "text <> at (40, 0) 40x20 \"YY\""));
    EMB_CHECK(has(&sb, "text <> at (140, 0) 40x20 \"ZZ\""));
    embi_sb_free(&sb);
    lh_free(&h);
    /* A Latin start keeps the span ltr. */
    lh l = make_ahem("<div id=d class=a style='width:300px'>XX<span style='unicode-bidi:plaintext'>"
                     "YY \xD7\x90\xD7\x91</span>ZZ</div>",
                     ahem_css);
    sb = dump_of(&l);
    EMB_CHECK(has(&sb, "text <> at (40, 0) 60x20 \"YY \""));
    embi_sb_free(&sb);
    lh_free(&l);
}

/* line-height as calc() or in a relative unit sets the line box height like a length does. */
EMB_TEST(inline_line_height_calc_and_em)
{
    lh h = make_ahem("<div id=d class=a style='width:300px;line-height:calc(10px + 20px)'>XX</div>",
                     ahem_css);
    embi_sb sb = dump_of(&h);
    EMB_CHECK(has(&sb, "line <div> id=d class=a at (0, 0) 300x30"));
    embi_sb_free(&sb);
    lh_free(&h);
    lh e = make_ahem("<div id=d class=a style='width:300px;line-height:calc(1em + 50%)'>XX</div>",
                     ahem_css);
    sb = dump_of(&e);
    EMB_CHECK(has(&sb, "line <div> id=d class=a at (0, 0) 300x30"));
    embi_sb_free(&sb);
    lh_free(&e);
}

/* A paragraph of thousands of words in one text node lays out in time linear in its length: every
 * break candidate's prefix width comes from a table, not from measuring the rest of the text node.
 * "XX " words in 300px make lines of five words. The test counts the bytes the layout asks the shaper
 * to measure (shaping the paragraph itself is the text module's cost, so a clock would measure that
 * too): a linear layout measures a small multiple of the text length, and doubling the paragraph
 * doubles the count, where measuring the tail of the node for every candidate would quadruple it. */
static uint64_t long_paragraph_measured(int words, uint32_t *lines, uint64_t *ms)
{
    embi_sb html = {nullptr, 0, 0};
    const char *head = "<div id=d class=a style='width:300px'>";
    embi_sb_append(&html, (embi_str){head, (uint32_t)strlen(head)});
    for (int i = 0; i < words; i++) {
        embi_sb_append(&html, EMBI_STR("XX "));
    }
    embi_sb_append(&html, EMBI_STR("</div>"));
    embi_sb_append(&html, (embi_str){"", 1});
    uint64_t t0 = SDL_GetTicks();
    uint64_t m0 = embi_tshim_measured_bytes;
    lh h = make_ahem(html.data, ahem_css);
    uint64_t measured = embi_tshim_measured_bytes - m0;
    embi_sb sb = dump_of(&h);
    *ms = SDL_GetTicks() - t0;
    *lines = line_count(&sb);
    embi_sb_free(&sb);
    lh_free(&h);
    embi_sb_free(&html);
    return measured;
}

EMB_TEST(inline_long_paragraph_is_linear)
{
    uint32_t n1 = 0, n2 = 0;
    uint64_t ms1 = 0, ms2 = 0;
    uint64_t m1 = long_paragraph_measured(5000, &n1, &ms1);
    uint64_t m2 = long_paragraph_measured(10000, &n2, &ms2);
    EMB_CHECK_EQ_INT(n1, 1000);
    EMB_CHECK_EQ_INT(n2, 2000);
    EMB_CHECK(m1 > 0);
    EMB_CHECK(m1 <= 8u * 15000u);
    EMB_CHECK(m2 <= 8u * 30000u);
    EMB_CHECK(m2 * 10 < m1 * 25); /* doubling the text: under 2.5x (linear is 2x, quadratic 4x) */
    uint64_t limit = EMB_TEST_ASAN ? 60000 : 15000;
    EMB_CHECK(ms1 < limit);
    EMB_CHECK(ms2 < limit);
}

EMB_TEST(inline_space_between_atomic_inlines_keeps_its_width)
{
    /* A collapsible space after an atomic inline is not at the start of the line and stays: the second
     * inline-block begins one space (10px, font-size 10px Ahem) after the first one ends. W3h: the space
     * used to be dropped. */
    lh h = lh_make("<body><div style='font:10px/10px Ahem'>"
                   "<div id=a style='display:inline-block;width:30px;height:10px'></div> "
                   "<div id=b style='display:inline-block;width:30px;height:10px'></div>"
                   "<div id=c style='display:inline-block;width:30px;height:10px'></div></div></body>",
                   "@font-face { font-family: Ahem; src: url(Ahem.ttf); }");
    embi_rectf a, b, c;
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "a"), &a));
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "b"), &b));
    EMB_REQUIRE(embi_layout_border_box(h.eng, lh_id(&h, "c"), &c));
    EMB_CHECK(b.x > a.x + a.w + 1.0f); /* the space between a and b */
    EMB_CHECK(c.x < b.x + b.w + 0.01f && c.x > b.x + b.w - 0.01f); /* no space between b and c */
    lh_free(&h);
}

EMB_TEST_MAIN()
