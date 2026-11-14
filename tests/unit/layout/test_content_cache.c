/*
 * The content-keyed caches of the layout (W3q): the shaped runs and the paragraph analyses survive a rebuild of
 * the box tree because they are keyed by what they depend on (text, shaping style, language, break rules), not by
 * box identity. These tests check that an entry is used again when nothing it depends on changed, and is not used
 * when something did: every layout of the warm engine must equal the layout of a fresh engine on the same document.
 */
#include "layout_harness.h"

#include <SDL3/SDL_iostream.h>
#include <stdatomic.h>

/* The dump of the layout, as a NUL-terminated string (free with embi_sb_free). */
static embi_sb dump_of(lh *h)
{
    embi_sb sb = {nullptr, 0, 0};
    embi_layout_dump(h->eng, h->doc, &sb);
    embi_sb_append(&sb, (embi_str){"", 1});
    return sb;
}

static bool same(const embi_sb *a, const embi_sb *b)
{
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static embi_element *by_id(lh *h, const char *id)
{
    return embi_get_element_by_id(h->doc, (embi_str){id, (uint32_t)strlen(id)});
}

static void set_attr(lh *h, const char *id, const char *name, const char *value)
{
    embi_element *el = by_id(h, id);
    EMB_REQUIRE(el != nullptr);
    embi_str v = {value, (uint32_t)strlen(value)};
    EMB_REQUIRE(embi_el_set_attribute(el, (embi_str){name, (uint32_t)strlen(name)},
                                      embi_dstr_from_utf8(h->doc->slab, v)) == EMBI_DOM_OK);
}

static void set_text_of(lh *h, const char *id, const char *text)
{
    embi_element *el = by_id(h, id);
    EMB_REQUIRE(el != nullptr);
    embi_node *t = el->base.first_child;
    while (t != nullptr && t->type != EMBI_NODE_TEXT) {
        t = t->next;
    }
    EMB_REQUIRE(t != nullptr);
    embi_chardata_set(embi_as_chardata(t), embi_dstr_from_utf8(h->doc->slab, (embi_str){text, (uint32_t)strlen(text)}));
}

/* Resolves the styles after a DOM change and lays out again (the box tree is rebuilt, the caches are kept). */
static void relayout(lh *h)
{
    /* W6b: a small DOM change patches the tree now; these tests are about the build that follows a rebuild */
    embi_layout_invalidate_all(h->eng, h->doc);
    embi_style_resolve_document(h->style, h->doc);
    (void)embi_layout_document(h->eng, h->doc);
}

#define CSS "body { margin: 0; font: 16px/1.3 sans-serif } #m { width: 220px } .w { white-space: nowrap }"
#define HTML(P1, ATTR) \
    "<body id=b><div id=m " ATTR "><p id=p1>" P1 "</p><p id=p2>second <i>italic</i> and <b>bold</b> words that wrap " \
    "across a few lines of this narrow box</p><p class=w id=p3>no wrap here at all</p></div></body>"

EMB_TEST(a_rebuild_that_reuses_the_cached_runs_equals_a_fresh_layout)
{
    lh warm = lh_make("<p>warm</p>", "");
    lh_free(&warm);
    size_t live0 = embi_mem_live();
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    embi_sb a = dump_of(&h);
    uint32_t builds = embi_layout_tree_builds(h.eng, h.doc);
    size_t cache0 = embi_layout_cache_bytes(h.eng, h.doc);
    EMB_CHECK(cache0 > 0);
    for (int i = 0; i < 3; i++) {
        set_attr(&h, "m", "data-x", i % 2 == 0 ? "odd" : "even");
        relayout(&h);
        EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) > builds); /* the tree was built again */
        builds = embi_layout_tree_builds(h.eng, h.doc);
        embi_sb b = dump_of(&h);
        EMB_CHECK(same(&a, &b));
        embi_sb_free(&b);
    }
    lh fresh = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    embi_sb f = dump_of(&fresh);
    EMB_CHECK(same(&a, &f));
    EMB_CHECK(embi_layout_cache_bytes(h.eng, h.doc) <= cache0 + cache0 / 4); /* the caches do not pile up */
    embi_sb_free(&f);
    embi_sb_free(&a);
    lh_free(&fresh);
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)live0);
}

EMB_TEST(a_text_change_is_not_served_from_the_old_entries)
{
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    embi_sb a = dump_of(&h);
    const char *t2 = "a much longer replacement text for the first paragraph so that it needs more lines than before";
    set_text_of(&h, "p1", t2);
    relayout(&h);
    embi_sb b = dump_of(&h);
    EMB_CHECK(!same(&a, &b));
    lh fresh = lh_make(HTML("a much longer replacement text for the first paragraph so that it needs more lines than before", ""), CSS);
    embi_sb f = dump_of(&fresh);
    EMB_CHECK(same(&b, &f));
    /* Back to the first text: the old entries may be served again, the result is the first layout. */
    set_text_of(&h, "p1", "first paragraph of plain text that wraps too");
    relayout(&h);
    embi_sb c = dump_of(&h);
    EMB_CHECK(same(&a, &c));
    embi_sb_free(&a);
    embi_sb_free(&b);
    embi_sb_free(&c);
    embi_sb_free(&f);
    lh_free(&fresh);
    lh_free(&h);
}

EMB_TEST(a_change_of_the_shaping_style_is_not_served_from_the_old_entries)
{
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    embi_sb a = dump_of(&h);
    static const char *const styles[] = {"font-size: 24px", "letter-spacing: 3px", "font-size: 11px",
                                         "word-break: break-all", "white-space: nowrap", "direction: rtl"};
    for (size_t i = 0; i < sizeof styles / sizeof styles[0]; i++) {
        set_attr(&h, "m", "style", styles[i]);
        relayout(&h);
        embi_sb b = dump_of(&h);
        EMB_CHECK(!same(&a, &b));
        char attr[96];
        SDL_snprintf(attr, sizeof attr, "style='%s'", styles[i]);
        char html[1024];
        SDL_snprintf(html, sizeof html, HTML("first paragraph of plain text that wraps too", "%s"), attr);
        lh fresh = lh_make(html, CSS);
        embi_sb f = dump_of(&fresh);
        EMB_CHECK(same(&b, &f));
        embi_sb_free(&b);
        embi_sb_free(&f);
        lh_free(&fresh);
    }
    /* The style removed again: the first layout. */
    set_attr(&h, "m", "style", "");
    relayout(&h);
    embi_sb c = dump_of(&h);
    EMB_CHECK(same(&a, &c));
    embi_sb_free(&a);
    embi_sb_free(&c);
    lh_free(&h);
}

EMB_TEST(a_language_change_is_not_served_from_the_old_entries)
{
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    set_attr(&h, "m", "lang", "tr");
    relayout(&h);
    embi_sb b = dump_of(&h);
    lh fresh = lh_make(HTML("first paragraph of plain text that wraps too", "lang=tr"), CSS);
    embi_sb f = dump_of(&fresh);
    EMB_CHECK(same(&b, &f));
    embi_sb_free(&b);
    embi_sb_free(&f);
    lh_free(&fresh);
    lh_free(&h);
}

/* The harness's lh_make with a font loader that serves Ahem (1em advances) for every @font-face source, so
 * that a font-family change really changes the widths. */
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

static uint64_t shaped_now(void)
{
    return atomic_load(&embi_tshim_shaped_runs);
}

static uint32_t analyses_of(lh *h)
{
    embi_layout_cold_stats cs;
    embi_layout_cold_stats_get(h->eng, h->doc, &cs);
    return cs.analyses;
}

EMB_TEST(an_unchanged_rebuild_shapes_and_analyses_nothing_a_changed_paragraph_does)
{
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    uint64_t shaped0 = shaped_now();
    uint32_t an0 = analyses_of(&h);
    EMB_CHECK(an0 >= 3);
    uint32_t builds = embi_layout_tree_builds(h.eng, h.doc);
    for (int i = 0; i < 3; i++) {
        set_attr(&h, "m", "data-x", i % 2 == 0 ? "odd" : "even");
        relayout(&h);
        EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) > builds);
        builds = embi_layout_tree_builds(h.eng, h.doc);
        EMB_CHECK_EQ_INT((long long)shaped_now(), (long long)shaped0);
        EMB_CHECK_EQ_INT(analyses_of(&h), an0);
    }
    /* One paragraph's text changes: only it is analysed again, the other two come from the cache. */
    set_text_of(&h, "p1", "a different first paragraph, still plain text that wraps");
    relayout(&h);
    EMB_CHECK(shaped_now() > shaped0);
    EMB_CHECK_EQ_INT(analyses_of(&h), an0 + 1);
    lh_free(&h);
}

EMB_TEST(an_abspos_child_is_cached_across_a_rebuild)
{
    const char *html = "<body><div id=m style='position:relative;width:220px;height:100px'>"
                       "<p id=ab style='position:absolute;left:10px;top:5px;margin:0'>absolutely positioned text "
                       "that wraps over a few lines</p></div></body>";
    const char *css = "body { margin: 0; font: 16px/1.3 sans-serif }";
    lh h = lh_make(html, css);
    embi_sb a = dump_of(&h);
    uint64_t shaped0 = shaped_now();
    uint32_t an0 = analyses_of(&h);
    set_attr(&h, "m", "data-x", "1");
    relayout(&h);
    embi_sb b = dump_of(&h);
    EMB_CHECK(same(&a, &b));
    EMB_CHECK_EQ_INT((long long)shaped_now(), (long long)shaped0);
    EMB_CHECK_EQ_INT(analyses_of(&h), an0);
    lh fresh = lh_make(html, css);
    embi_sb f = dump_of(&fresh);
    EMB_CHECK(same(&b, &f));
    embi_sb_free(&a);
    embi_sb_free(&b);
    embi_sb_free(&f);
    lh_free(&fresh);
    lh_free(&h);
}

EMB_TEST(inline_blocks_and_floats_are_not_served_stale)
{
    const char *html = "<body><div id=m style='width:240px'><p id=p1>before <span style='display:inline-block;"
                       "width:40px;height:10px;background:red'></span> and a <b>bold</b> word <span "
                       "style='float:right;width:30px;height:12px'></span> after the float, long enough to "
                       "wrap onto several lines of this box</p></div></body>";
    const char *css = "body { margin: 0; font: 16px/1.3 sans-serif }";
    lh h = lh_make(html, css);
    lh fresh = lh_make(html, css);
    embi_sb f = dump_of(&fresh);
    for (int i = 0; i < 3; i++) {
        set_attr(&h, "m", "data-x", i % 2 == 0 ? "odd" : "even");
        relayout(&h);
        embi_sb b = dump_of(&h);
        EMB_CHECK(same(&b, &f));
        embi_sb_free(&b);
    }
    /* The paragraph holds an atomic inline and a float: it is analysed again on each layout (uncacheable). */
    uint32_t an0 = analyses_of(&h);
    set_attr(&h, "m", "data-x", "again");
    relayout(&h);
    EMB_CHECK(analyses_of(&h) > an0);
    embi_sb_free(&f);
    lh_free(&fresh);
    lh_free(&h);
}

EMB_TEST(intrinsic_sizes_after_a_rebuild_equal_a_fresh_engine)
{
    static const char *const widths[] = {"width:200px", "width:120px", "width:340px", "width:200px"};
    const char *css = "body { margin: 0; font: 16px/1.3 sans-serif }";
    char html[1400];
    lh h = {0};
    for (size_t i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        SDL_snprintf(html, sizeof html,
                     "<body><div id=m style='%s'><div id=f style='float:left'>shrink wrapped float text "
                     "that is fairly long</div><div style='display:flex'><div>flex item with some words"
                     "</div><div>and another</div></div><div style='display:inline-block' id=ib>inline block "
                     "text, also long enough to wrap</div><table><tr><td>cell one text</td><td>cell two "
                     "has more text in it</td></tr></table></div></body>",
                     widths[i]);
        lh fresh = lh_make(html, css);
        embi_sb f = dump_of(&fresh);
        if (i == 0) {
            h = lh_make(html, css);
        } else {
            set_attr(&h, "m", "style", widths[i]);
            relayout(&h);
        }
        embi_sb b = dump_of(&h);
        EMB_CHECK(same(&b, &f));
        embi_sb_free(&b);
        embi_sb_free(&f);
        lh_free(&fresh);
    }
    lh_free(&h);
}

EMB_TEST(a_font_family_change_between_different_fonts_is_not_served_stale)
{
    static const char *const fams[] = {"font-family: sans-serif", "font-family: Ahem", "font-family: Ahem, serif",
                                       "font-family: monospace", "font-family: sans-serif", "font-family: Ahem"};
    const char *css = "@font-face { font-family: Ahem; src: url(Ahem.ttf); } body { margin: 0; font-size: 16px }";
    const char *base = "<body><div id=m style='width:200px;%s'><p id=p1>%s</p></div></body>";
    const char *text = "first paragraph of plain text that wraps too";
    char html[512];
    SDL_snprintf(html, sizeof html, base, fams[0], text);
    lh h = make_ahem(html, css);
    embi_sb first = dump_of(&h);
    embi_sb ahem = {nullptr, 0, 0};
    for (size_t i = 1; i < sizeof fams / sizeof fams[0]; i++) {
        char val[96];
        SDL_snprintf(val, sizeof val, "width:200px;%s", fams[i]);
        set_attr(&h, "m", "style", val);
        relayout(&h);
        embi_sb b = dump_of(&h);
        SDL_snprintf(html, sizeof html, base, fams[i], text);
        lh fresh = make_ahem(html, css);
        embi_sb f = dump_of(&fresh);
        EMB_CHECK(same(&b, &f));
        if (i == 1) {
            EMB_CHECK(!same(&first, &b)); /* Ahem and sans-serif really differ */
            ahem = b;
        } else {
            if (i == 4) {
                EMB_CHECK(same(&first, &b)); /* back to the first family: the first layout */
            }
            if (i == 5) {
                EMB_CHECK(same(&ahem, &b));
            }
            embi_sb_free(&b);
        }
        embi_sb_free(&f);
        lh_free(&fresh);
    }
    embi_sb_free(&ahem);
    embi_sb_free(&first);
    lh_free(&h);
}

EMB_TEST(many_rebuilds_with_text_variants_stay_equal_and_hold_memory)
{
    lh warm = lh_make("<p>warm</p>", "");
    lh_free(&warm);
    size_t live0 = embi_mem_live();
    lh h = lh_make(HTML("first paragraph of plain text that wraps too", ""), CSS);
    embi_sb a = dump_of(&h);
    for (int i = 0; i < 40; i++) {
        /* Alternate the text of p1 so that entries die and are reclaimed, and rebuild each time. */
        char t[96];
        SDL_snprintf(t, sizeof t, "paragraph variant number %d with words that wrap", i % 7);
        set_text_of(&h, "p1", t);
        relayout(&h);
    }
    set_text_of(&h, "p1", "first paragraph of plain text that wraps too");
    relayout(&h);
    embi_sb b = dump_of(&h);
    EMB_CHECK(same(&a, &b));
    embi_sb_free(&a);
    embi_sb_free(&b);
    lh_free(&h);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)live0);
}

EMB_TEST_MAIN()
