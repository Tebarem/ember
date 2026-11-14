/*
 * Regression tests for the layout and style fixes found by the W4f reftests: font-relative units measured from the
 * font, anonymous inline-tables, the viewport's overflow, aspect-ratio blocks, ::first-letter generation, and the
 * grid and block bugs that the reftest run turned up under ASan.
 */
#include "emb_test.h"

#include "layout_harness.h"

#include <SDL3/SDL_iostream.h>

#include <math.h>

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

/* lh_make with a font loader that serves Ahem (1em advances) for @font-face sources. */
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

static float width_of(lh *h, const char *id)
{
    embi_rectf r = {0};
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, &r) ? r.w : -1.0f;
}

static float height_of(lh *h, const char *id)
{
    embi_rectf r = {0};
    embi_element *el = lh_id(h, id);
    return el != nullptr && embi_layout_border_box(h->eng, el, &r) ? r.h : -1.0f;
}

#define AHEM_FACE "@font-face{font-family:A;src:url(a.ttf)} .a{font:20px/1 A}"

EMB_TEST(ch_unit_is_the_advance_of_zero_in_the_font)
{
    /* Ahem is 1em wide for every glyph: 4ch at 20px is 80px (the fallback would be 0.5em, 40px). */
    lh h = make_ahem("<div class=a id=x style='width:4ch'>x</div><div class=a id=y style='width:calc(2ch + 10px)'>x</div>",
                     AHEM_FACE);
    EMB_CHECK(fabsf(width_of(&h, "x") - 80.0f) < 0.01f);
    EMB_CHECK(fabsf(width_of(&h, "y") - 50.0f) < 0.01f);
    lh_free(&h);
}

/* The number of atomic table boxes below b. */
static uint32_t count_atomic_tables(const embi_box *b)
{
    uint32_t n = 0;
    for (const embi_box *k = b != nullptr ? embi_box_first(b) : nullptr; k != nullptr; k = k->next) {
        if (k->kind == EMBI_BK_TABLE && (k->flags & EMBI_BF_ATOMIC) != 0 && k->parent != nullptr &&
            k->parent->kind == EMBI_BK_INLINE) {
            n++;
        }
        n += count_atomic_tables(k);
    }
    return n;
}

EMB_TEST(table_parts_inside_an_inline_form_an_anonymous_inline_table)
{
    /* The cells sit directly in an inline box: they get an anonymous inline-table, an atomic table box that is a child
     * of the inline box. The line stays one line: no block-in-inline split. */
    lh h = lh_make("<div id=d><span>a <span style='display:table-cell'>b</span> <span style='display:table-cell'>c</span> d</span></div>",
                   "#d{font-size:20px}");
    const embi_box *root = lh_root_box(&h);
    EMB_REQUIRE(root != nullptr);
    EMB_CHECK(count_atomic_tables(root) == 1);
    float one = height_of(&h, "d");
    lh_free(&h);
    lh g = lh_make("<div id=d>a b c d</div>", "#d{font-size:20px}");
    EMB_CHECK(fabsf(one - height_of(&g, "d")) < 1.0f);
    EMB_CHECK(count_atomic_tables(lh_root_box(&g)) == 0);
    lh_free(&g);
}

EMB_TEST(root_overflow_belongs_to_the_viewport)
{
    lh h = lh_make("<div id=a>x</div>", "html{overflow:hidden}");
    const embi_box *root = lh_root_box(&h);
    EMB_REQUIRE(root != nullptr);
    EMB_CHECK((root->flags & (EMBI_BF_SCROLL | EMBI_BF_CLIP)) == 0);
    lh_free(&h);
    /* The body's overflow propagates when the root's is visible. */
    lh g = lh_make("<div id=a>x</div>", "body{overflow:hidden}");
    const embi_box *r2 = lh_root_box(&g);
    EMB_REQUIRE(r2 != nullptr);
    bool saw_body = false;
    for (const embi_box *k = r2->first; k != nullptr; k = k->next) {
        if (!embi_box_is_text(k) && k->el != nullptr) {
            embi_str n = embi_atom_str(k->el->base.name);
            if (n.len == 4 && memcmp(n.ptr, "body", 4) == 0) {
                saw_body = true;
                EMB_CHECK((k->flags & (EMBI_BF_SCROLL | EMBI_BF_CLIP)) == 0);
            }
        }
    }
    EMB_CHECK(saw_body);
    lh_free(&g);
}

EMB_TEST(block_with_aspect_ratio_and_definite_height_gets_its_width_from_the_ratio)
{
    lh h = lh_make("<div id=a style='height:100px;aspect-ratio:1/1'></div>", "");
    EMB_CHECK(fabsf(width_of(&h, "a") - 100.0f) < 0.01f);
    lh_free(&h);
}

/* Appends the text of the text boxes below b in tree order: the first-letter ones to fl, the others to rest. */
static void collect_text(const embi_box *b, embi_sb *fl, embi_sb *rest)
{
    for (const embi_box *k = b != nullptr ? embi_box_first(b) : nullptr; k != nullptr; k = k->next) {
        if (k->kind == EMBI_BK_TEXT && k->text != nullptr) {
            embi_sb_append(k->pe == EMBI_PE_FIRST_LETTER ? fl : rest, (embi_str){k->text, k->text_len});
        }
        collect_text(k, fl, rest);
    }
}

/* The text wrapped by element id's ::first-letter boxes, and the text of the other boxes inside it. */
static void fl_texts(lh *h, const char *id, char *fl, size_t flcap, char *rest, size_t restcap)
{
    embi_element *el = lh_id(h, id);
    embi_sb a = {0}, b = {0};
    collect_text(el != nullptr ? el->box : nullptr, &a, &b);
    SDL_snprintf(fl, flcap, "%.*s", (int)a.len, a.data != nullptr ? a.data : "");
    SDL_snprintf(rest, restcap, "%.*s", (int)b.len, b.data != nullptr ? b.data : "");
    embi_sb_free(&a);
    embi_sb_free(&b);
}

EMB_TEST(first_letter_wraps_the_leading_quote_and_the_letter)
{
    lh h = lh_make("<p id=p>\"Hello world</p>", "p::first-letter{color:red;font-size:200%}");
    const embi_box *root = lh_root_box(&h);
    EMB_REQUIRE(root != nullptr);
    const embi_box *w = lh_find_box(root, EMBI_BK_INLINE, EMBI_PE_FIRST_LETTER);
    EMB_REQUIRE(w != nullptr);
    EMB_REQUIRE(w->first != nullptr && w->first->kind == EMBI_BK_TEXT);
    EMB_CHECK(w->first->text_len == 2);
    char fl[64], rest[64];
    fl_texts(&h, "p", fl, sizeof fl, rest, sizeof rest);
    EMB_CHECK(strcmp(fl, "\"H") == 0);
    EMB_CHECK(strcmp(rest, "ello world") == 0);
    lh_free(&h);
}

EMB_TEST(first_letter_takes_trailing_punctuation_and_unicode_punctuation)
{
    char fl[64], rest[64];
    lh h = lh_make("<p id=p>A!? rest</p>", "p::first-letter{color:red}");
    fl_texts(&h, "p", fl, sizeof fl, rest, sizeof rest);
    EMB_CHECK(strcmp(fl, "A!?") == 0);
    EMB_CHECK(strcmp(rest, " rest") == 0);
    lh_free(&h);
    /* U+00AB (Pi), U+00DC, U+00BB (Pf): all three belong to the first letter. */
    lh g = lh_make("<p id=p>&#171;&#220;&#187; x</p>", "p::first-letter{color:red}");
    fl_texts(&g, "p", fl, sizeof fl, rest, sizeof rest);
    EMB_CHECK(strcmp(fl, "\xC2\xAB" "\xC3\x9C" "\xC2\xBB") == 0);
    EMB_CHECK(strcmp(rest, " x") == 0);
    lh_free(&g);
}

EMB_TEST(first_letter_keeps_combining_marks_with_the_letter)
{
    char fl[64], rest[64];
    /* e + U+0301 (2 bytes) is one letter; the following letters are not part of it. */
    lh h = lh_make("<p id=p>e&#769;llo</p>", "p::first-letter{color:red}");
    fl_texts(&h, "p", fl, sizeof fl, rest, sizeof rest);
    EMB_CHECK(strcmp(fl, "e\xCC\x81") == 0);
    EMB_CHECK(strcmp(rest, "llo") == 0);
    lh_free(&h);
    /* Text that starts with a space has no first letter wrap on that run. */
    lh g = lh_make("<p id=p><b>  </b>xyz</p>", "p::first-letter{color:red}");
    fl_texts(&g, "p", fl, sizeof fl, rest, sizeof rest);
    EMB_CHECK(strcmp(fl, "x") == 0);
    lh_free(&g);
}

EMB_TEST(ex_and_cap_units_come_from_the_font_metrics)
{
    /* Ahem: x-height and cap height are 0.8em, so at 20px 3ex and 2cap are 48px and 32px. */
    lh h = make_ahem("<div class=a id=x style='width:3ex'>x</div><div class=a id=y style='width:2cap'>x</div>"
                     "<div class=a id=z style='width:calc(1ex + 1cap)'>x</div>",
                     AHEM_FACE);
    EMB_CHECK(fabsf(width_of(&h, "x") - 48.0f) < 0.01f);
    EMB_CHECK(fabsf(width_of(&h, "y") - 32.0f) < 0.01f);
    EMB_CHECK(fabsf(width_of(&h, "z") - 32.0f) < 0.01f);
    lh_free(&h);
}

EMB_TEST(ic_unit_falls_back_to_one_em_when_the_font_has_no_water_ideograph)
{
    /* Ahem has no U+6C34: 2ic is 2em (CSS Values 4: the fallback is 1em). */
    lh h = make_ahem("<div class=a id=x style='width:2ic'>x</div>", AHEM_FACE);
    EMB_CHECK(fabsf(width_of(&h, "x") - 40.0f) < 0.01f);
    lh_free(&h);
}

EMB_TEST(unmatched_control_characters_get_the_box_glyph_not_a_blank)
{
    /* The page lays out with a control character in it: it takes room (the box glyph's advance) instead of vanishing. */
    lh h = lh_make("<div id=a>a&#1;b</div><div id=b>ab</div>", "div{font-size:20px;width:max-content}");
    EMB_CHECK(width_of(&h, "a") > width_of(&h, "b") + 1.0f);
    lh_free(&h);
}

EMB_TEST(form_feed_and_carriage_return_do_not_get_the_box_glyph)
{
    /* U+000C and U+000D are not shaped as unmapped controls (CSS Text handles them): no box-glyph advance, unlike U+0001. */
    lh h = lh_make("<div id=a style='white-space:pre'>a&#12;b</div><div id=b style='white-space:pre'>a&#1;b</div>",
                   "div{font-size:20px;width:max-content}");
    EMB_CHECK(width_of(&h, "a") < width_of(&h, "b") - 1.0f);
    lh_free(&h);
}

EMB_TEST(display_contents_root_does_not_treat_a_text_box_as_the_root_element)
{
    /* The first box built for a display: contents root can be a text run, whose bytes alias a block's child pointer. The
     * root generates no box of its own; the body's block fills the 800px viewport (body margin 8px each side). */
    lh h = lh_make("<div id=a>x</div>", "html{display:contents} body{margin:0}");
    EMB_CHECK(fabsf(width_of(&h, "a") - 800.0f) < 0.01f);
    lh_free(&h);
    lh g = lh_make("x<div id=a>x</div>", "html,body{display:contents}");
    EMB_CHECK(fabsf(width_of(&g, "a") - 800.0f) < 0.01f);
    lh_free(&g);
}

EMB_TEST(subgrid_items_in_implicit_rows_beyond_the_root_tracks_keep_the_geometry)
{
    /* The subgrid spans the root's 2 columns and its first row. Its five items auto-place into rows 1 to 3 of the subgrid
     * (implicit rows of the subgrid, which the root never created), so grid.c's mark_rows and area_len see a span
     * [2, 3) against a root that has 2 row tracks (checked by instrumenting mark_rows). The clamps keep those reads
     * inside the track list; the array has slack capacity, so a missing clamp is not observable as an ASan report, only
     * as undefined track sizes. The geometry must be the same as without the extra items. */
    lh h = lh_make("<div id=g><div id=s><i id=a>a</i><i>b</i><i>c</i><i>d</i><i style='grid-column:1/span 9'>e</i></div></div>",
                   "#g{display:grid;width:200px;grid-template-columns:100px 100px;grid-template-rows:30px 30px}"
                   "#s{display:grid;grid-column:1/3;grid-row:1/2;grid-template-columns:subgrid;grid-template-rows:subgrid}"
                   "#s>i{display:block;height:10px}");
    EMB_CHECK(fabsf(width_of(&h, "g") - 200.0f) < 0.01f);
    EMB_CHECK(fabsf(height_of(&h, "g") - 60.0f) < 0.01f);
    EMB_CHECK(fabsf(width_of(&h, "s") - 200.0f) < 0.01f);
    EMB_CHECK(fabsf(height_of(&h, "s") - 30.0f) < 0.01f);
    EMB_CHECK(fabsf(width_of(&h, "a") - 100.0f) < 0.01f);
    EMB_CHECK(fabsf(height_of(&h, "a") - 10.0f) < 0.01f);
    lh_free(&h);
}

EMB_TEST_MAIN()
