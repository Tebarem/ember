/*
 * The layout cache (src/layout/lcache.c): a box laid out again in the same run with the same constraint space gets its
 * earlier result back. Flex and grid containers lay their items out several times (measures, then the final pass), and
 * nested ones multiply that, so these pages are built from nested flex and grid containers with percentage sizes,
 * scroll containers that may grow a gutter, absolutely positioned boxes and changing children.
 *
 * Every layout here, a first build or an update after a change, is compared with an independent reference: the document
 * cloned into a new DOM, resolved by a new style engine and laid out from nothing with the cache off. The fragment trees
 * must be equal (the snapshot the library's verification uses: rects, overflow, baselines, flags, text). The library's
 * own verification runs as well, which compares cached full builds and updates with a scratch build without the cache.
 */
#include "layout_harness.h"

#include <stdio.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static embi_element *by_id(lh *h, const char *id)
{
    embi_element *el = embi_get_element_by_id(h->doc, S(id));
    EMB_CHECK(el != nullptr);
    return el;
}

static void set_attr(lh *h, embi_element *el, const char *name, const char *value)
{
    EMB_REQUIRE(embi_el_set_attribute(el, S(name), embi_dstr_from_utf8(h->doc->slab, S(value))) == EMBI_DOM_OK);
}

static void set_text(lh *h, embi_element *el, const char *text)
{
    EMB_REQUIRE(el->base.first_child != nullptr && el->base.first_child->type == EMBI_NODE_TEXT);
    embi_chardata_set(embi_as_chardata(el->base.first_child), embi_dstr_from_utf8(h->doc->slab, S(text)));
}

static embi_element *add_child(lh *h, embi_element *host, const char *tag, const char *text)
{
    embi_element *ne = nullptr;
    if (embi_dom_create_element(h->doc, S(tag), &ne) != EMBI_DOM_OK) {
        EMB_CHECK(false);
        return nullptr;
    }
    if (text != nullptr) {
        embi_node *t = &embi_new_text(h->doc, embi_dstr_from_utf8(h->doc->slab, S(text)))->base;
        EMB_CHECK(embi_append_child(&ne->base, t) == EMBI_DOM_OK);
    }
    EMB_CHECK(embi_append_child(&host->base, &ne->base) == EMBI_DOM_OK);
    return ne;
}

static float g_sb; /* the scrollbar thickness of the document under test and of its reference */

/* The boxes the cache gave back over the life of the engine. */
static uint64_t reused(lh *h)
{
    embi_layout_prof p;
    embi_layout_prof_get(h->eng, &p);
    return p.reused;
}

/* The layout of a document equals the layout of its clone, built from nothing with the cache off. */
static bool matches_uncached(lh *a, const char *html, const char *what)
{
    embi_element *root = embi_document_element(a->doc);
    if (root == nullptr) {
        return false;
    }
    embi_style_engine *style = embi_style_engine_create(embi_tshim_fonts(a->text), nullptr, nullptr);
    embi_document *doc = embi_document_new(true);
    embi_node *copy = embi_import_node(doc, &root->base, true);
    EMB_CHECK(copy != nullptr && embi_append_child(&doc->base, copy) == EMBI_DOM_OK);
    char *all = lh_join_styles(html, strlen(html), "");
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)strlen(all)}, &opts);
    SDL_free(all);
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(style, doc, sheets, 1);
    embi_style_resolve_document(style, doc);
    embi_layout_config cfg = {nullptr, nullptr, g_sb};
    embi_layout_engine *eng = embi_layout_engine_create(a->text, style, &cfg);
    bool was = embi_lay_cache_enabled();
    embi_lay_set_cache_enabled(false);
    (void)embi_layout_document(eng, doc);
    embi_lay_set_cache_enabled(was);

    embi_sb da = {0}, db = {0};
    embi_layout_dump(a->eng, a->doc, &da);
    embi_layout_dump(eng, doc, &db);
    embi_sb_cstr(&da);
    embi_sb_cstr(&db);
    bool same = da.len == db.len && memcmp(da.data, db.data, da.len) == 0;
    if (!same) {
        uint32_t line = 1;
        const char *pa = da.data, *pb = db.data;
        while (*pa != '\0' && *pa == *pb) {
            line += *pa == '\n' ? 1u : 0u;
            pa++;
            pb++;
        }
        printf("  after %s the cached layout differs from an uncached one at dump line %u\n", what, line);
    }
    embi_sb_free(&da);
    embi_sb_free(&db);
    embi_layout_engine_destroy(eng);
    embi_style_forget_document(style, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(style);
    return same;
}

static uint32_t g_fail0; /* the library's verification failures before the test */
static uint32_t g_open;  /* documents of the test not yet freed */

/* Lays page out with the library's verification on: each layout the cache gave something to (a full build or an update)
 * is compared with a scratch build without the cache, on the same document, fragment by fragment. */
static lh make(const char *page, float sb)
{
    g_sb = sb;
    if (g_open++ == 0) {
        embi_incr_set_verify(2);
        g_fail0 = embi_incr_verify_failures();
    }
    embi_layout_config cfg = {nullptr, nullptr, sb};
    return lh_make_cfg(page, strlen(page), "", &cfg);
}

static void done(lh *h)
{
    EMB_CHECK(embi_incr_verify_failures() == g_fail0);
    if (--g_open == 0) {
        embi_incr_set_verify(0);
    }
    lh_free(h);
}

static void relayout(lh *h, const char *page, const char *what)
{
    (void)embi_layout_document(h->eng, h->doc);
    EMB_CHECK(matches_uncached(h, page, what));
}

/* ------------------------------------------------------------------ pages */

/* An editor-like shell: a column flex page, a row of panels with percentage widths, nested flex rows, a grid of
 * properties whose cells hold flex rows, scroll containers and absolutely positioned badges. */
static const char *k_shell =
    "<style>html,body{height:100%} body{display:flex;flex-direction:column;font-size:13px} "
    "header{display:flex;gap:6px;padding:4px;align-items:center} header span{flex:none} header .sp{flex:1} "
    "main{flex:1;display:flex;min-height:0} "
    "#left{width:20%;display:flex;flex-direction:column;overflow:auto} "
    "#mid{flex:1;display:flex;flex-direction:column;min-width:0} "
    "#right{width:28%;overflow:auto;padding:4px} "
    ".row{display:flex;align-items:center;gap:4px;padding:2px 4px} .row .n{flex:1;min-width:0} "
    ".row .b{width:50%;max-width:60px} .row .ab{position:absolute;right:2px;top:1px} .row{position:relative} "
    ".props{display:grid;grid-template-columns:minmax(60px,35%) 1fr;gap:2px 6px} "
    ".props .v{display:flex;gap:3px} .props .v span{flex:1 1 0;min-width:0} "
    ".tall{height:400px} .wide{width:140%} .hid{display:none} .pct{height:50%} .pad{padding:9px} .big{font-size:22px} "
    ".tabs{display:flex} .tabs div{flex:1;text-align:center} .view{flex:1;display:grid;place-items:center}</style>"
    "<header><span>File</span><span>Edit</span><span id=title class=sp>Scene</span><span id=hb>Play</span></header>"
    "<main><div id=left>"
    "<div class=row id=r1><span class=n>Camera</span><span class=b>x</span><i class=ab>1</i></div>"
    "<div class=row id=r2><span class=n>Light</span><span class=b>y</span></div>"
    "<div class=row id=r3><span class=n id=r3n>Player</span><span class=b>z</span></div>"
    "<div id=lt class=pct><div class=row><span class=n>Ground</span></div></div>"
    "</div><div id=mid><div class=tabs id=tabs><div>Scene</div><div id=t2>Game</div></div>"
    "<div class=view id=view><div id=vb>viewport</div></div></div>"
    "<div id=right><div class=props id=props>"
    "<span>Name</span><div class=v><span id=nm>Player</span></div>"
    "<span>Position</span><div class=v id=pos><span>1</span><span>2</span><span>3</span></div>"
    "<span>Scale</span><div class=v><span>1</span><span>1</span><span id=sz>1</span></div>"
    "</div><div id=more class=row><span class=n>Add component</span></div></div></main>";

/* Grids inside flex inside grids, with auto and percentage tracks and items that size to their content. */
static const char *k_nest =
    "<style>#top{display:grid;grid-template-columns:auto 1fr 25%;gap:4px;width:700px} "
    ".f{display:flex;flex-wrap:wrap;gap:3px} .f>*{flex:1 1 auto} .g{display:grid;grid-template-columns:repeat(2,auto)} "
    ".c{display:flex;flex-direction:column} .p{width:60%} .h{height:30%} .w{width:200px} .big{font-size:20px} "
    ".hid{display:none} .ib{display:inline-block} .sc{overflow:auto;max-height:60px}</style>"
    "<div id=top><div class=c id=a><div class=f id=a1><span>alpha</span><span id=a1b>beta gamma</span></div>"
    "<div class=g id=a2><b>one</b><b id=a2b>two</b><b>three</b></div></div>"
    "<div class=f id=b><div class=g id=b1><div class=c><i>x</i><i id=b1i>y</i></div><span>z</span></div>"
    "<div class='c p' id=b2><span id=b2s>percent</span><div class=h id=b2h>h</div></div></div>"
    "<div class='c sc' id=c><span>c1</span><span id=c2>c2</span><div class=f><span>c3</span><span>c4</span></div></div>"
    "</div>";

/* ------------------------------------------------------------------ tests */

EMB_TEST(full_build_of_nested_flex_and_grid_equals_an_uncached_one)
{
    lh h = make(k_shell, 0.0f);
    EMB_CHECK(matches_uncached(&h, k_shell, "the first build"));
    EMB_CHECK(reused(&h) > 0); /* the nested containers did lay boxes out more than once */
    done(&h);
    lh g = make(k_nest, 0.0f);
    EMB_CHECK(matches_uncached(&g, k_nest, "the first build"));
    EMB_CHECK(reused(&g) > 0);
    done(&g);
}

EMB_TEST(the_cache_can_be_turned_off)
{
    embi_lay_set_cache_enabled(false);
    lh h = make(k_shell, 0.0f);
    EMB_CHECK(reused(&h) == 0);
    embi_lay_set_cache_enabled(true);
    set_text(&h, by_id(&h, "title"), "Scene 2");
    relayout(&h, k_shell, "a title change");
    EMB_CHECK(reused(&h) > 0);
    done(&h);
}

/* A child of a flex row changes in ways that keep its size and in ways that change it: the row's other items and the
 * containers around it, which the cache gives back where their constraints did not change, must land where a build from
 * nothing puts them. */
EMB_TEST(flex_parent_with_a_changed_child)
{
    lh h = make(k_shell, 0.0f);
    set_text(&h, by_id(&h, "r3n"), "Enemy"); /* about the same width */
    relayout(&h, k_shell, "a same-size text change in a flex row");
    set_text(&h, by_id(&h, "r3n"), "A much longer name that wraps the row of the outline panel onto more lines");
    relayout(&h, k_shell, "a text change that grows a flex item");
    set_attr(&h, by_id(&h, "hb"), "class", "big");
    relayout(&h, k_shell, "a header item that grows taller");
    set_attr(&h, by_id(&h, "hb"), "class", "hid");
    relayout(&h, k_shell, "a header item taken out of the flow");
    set_attr(&h, by_id(&h, "t2"), "class", "pad");
    relayout(&h, k_shell, "padding on a tab");
    (void)add_child(&h, by_id(&h, "left"), "div", "a new row");
    relayout(&h, k_shell, "a row appended to a column flex");
    done(&h);
}

EMB_TEST(grid_parent_with_a_changed_child)
{
    lh h = make(k_shell, 0.0f);
    set_text(&h, by_id(&h, "nm"), "Player two");
    relayout(&h, k_shell, "a text change in a grid cell");
    set_text(&h, by_id(&h, "nm"), "A long name that is wider than the property column and wraps inside the grid");
    relayout(&h, k_shell, "a grid cell that grows");
    set_attr(&h, by_id(&h, "sz"), "class", "big");
    relayout(&h, k_shell, "a bigger font in a nested flex inside a grid");
    lh g = make(k_nest, 0.0f);
    set_text(&g, by_id(&g, "a2b"), "a much wider auto column item");
    relayout(&g, k_nest, "an auto track that grows");
    set_attr(&g, by_id(&g, "b1i"), "class", "big");
    relayout(&g, k_nest, "a column flex inside a grid inside a flex");
    set_attr(&g, by_id(&g, "a1b"), "class", "hid");
    relayout(&g, k_nest, "a flex item hidden inside a grid item");
    done(&g);
    done(&h);
}

/* Percentage sizes resolve against the containing block: when it changes, the cached results keyed by the old one must
 * not come back. */
EMB_TEST(percentage_sized_children_follow_their_container)
{
    lh h = make(k_shell, 0.0f);
    set_attr(&h, by_id(&h, "left"), "style", "width:35%");
    relayout(&h, k_shell, "a wider panel with percentage children");
    set_attr(&h, by_id(&h, "r1"), "style", "width:70%");
    relayout(&h, k_shell, "a percentage-width row");
    set_attr(&h, by_id(&h, "lt"), "style", "height:120px");
    relayout(&h, k_shell, "a percentage height that becomes fixed");
    set_attr(&h, by_id(&h, "lt"), "style", "");
    relayout(&h, k_shell, "a percentage height again");
    lh g = make(k_nest, 0.0f);
    set_attr(&g, by_id(&g, "top"), "style", "width:500px");
    relayout(&g, k_nest, "a narrower grid with a percentage track");
    set_attr(&g, by_id(&g, "b2h"), "style", "height:50%");
    relayout(&g, k_nest, "a percentage height in a column flex");
    set_attr(&g, by_id(&g, "b2"), "class", "c");
    relayout(&g, k_nest, "a percentage width removed");
    done(&g);
    done(&h);
}

/* Scroll containers with classic scrollbars: content that starts or stops overflowing takes a gutter, which changes the
 * width every box inside sees. A cached result laid out without the gutter must not come back with it. */
EMB_TEST(scrollbar_gutters_appear_and_go)
{
    lh h = make(k_shell, 12.0f);
    EMB_CHECK(matches_uncached(&h, k_shell, "the first build with scrollbars"));
    set_attr(&h, by_id(&h, "props"), "class", "props tall");
    relayout(&h, k_shell, "a tall property grid that overflows");
    set_attr(&h, by_id(&h, "r2"), "class", "row wide");
    relayout(&h, k_shell, "a wide row that overflows sideways");
    set_attr(&h, by_id(&h, "props"), "class", "props");
    relayout(&h, k_shell, "the property grid short again");
    set_attr(&h, by_id(&h, "r2"), "class", "row");
    relayout(&h, k_shell, "the row narrow again");
    lh g = make(k_nest, 12.0f);
    set_attr(&g, by_id(&g, "c2"), "class", "tall");
    relayout(&g, k_nest, "a scroll container in a grid that overflows");
    done(&g);
    done(&h);
}

/* An editor shell whose panel widths are custom properties on the root element, which a splitter drag sets: the grid's
 * template and the right panel's width change, and the boxes of the grid items are kept (a box rebuilt in a grid takes the
 * rebuild up to the nearest block, the body). The cache's entries outlive the layout run while the box tree is kept: the
 * header, and the right panel when its width comes back to an earlier one, are given back from an earlier run. The rows
 * that are relatively positioned or hold absolutely positioned boxes are moved by the final pass of each run, so an entry
 * above them serves only its own run. */
static const char *k_split =
    "<style>:root{--lw:180px;--rw:220px} html,body{height:100%;margin:0} body{display:flex;flex-direction:column;font-size:13px} "
    "header{display:flex;gap:6px;padding:4px} header .sp{flex:1} "
    "main{flex:1;min-height:0;display:grid;grid-template-columns:var(--lw) 4px 1fr var(--rw)} "
    "#left,#right{display:flex;flex-direction:column;overflow:auto;min-width:0} #right{width:var(--rw)} "
    ".row{display:flex;gap:4px;padding:2px} .row .n{flex:1;min-width:0} .rel{position:relative;left:2px} "
    ".ab{position:absolute;right:0;top:0} #mid{display:grid;place-items:center;position:relative} "
    ".props{display:grid;grid-template-columns:minmax(40px,35%) 1fr;gap:2px}</style>"
    "<header><span>File</span><span class=sp id=title>Scene</span><span>Play</span></header>"
    "<main><div id=left><div class=row id=r1><span class=n>Camera</span><span>eye</span></div>"
    "<div class='row rel' id=r2><span class=n>Light with a long name that wraps in a narrow panel</span><i class=ab>1</i></div>"
    "<div class=row><span class=n>Player</span></div></div><div id=split></div>"
    "<div id=mid><div id=vb>viewport</div><b class=ab>fps</b></div>"
    "<div id=right><div class=props><span>Name</span><span id=nm>Player</span><span>Position</span><span>1 2 3</span></div>"
    "<div class=row><span class=n>Add component</span></div></div></main>";

static uint64_t reused_xrun(lh *h)
{
    embi_layout_prof p;
    embi_layout_prof_get(h->eng, &p);
    return p.reused_xrun;
}

EMB_TEST(a_splitter_drag_keeps_the_boxes_and_reuses_earlier_runs)
{
    lh h = make(k_split, 0.0f);
    EMB_CHECK(matches_uncached(&h, k_split, "the first build"));
    embi_element *root = embi_document_element(h.doc);
    const void *r1 = by_id(&h, "r1")->box, *nm = by_id(&h, "nm")->box, *title = by_id(&h, "title")->box;
    uint64_t x0 = reused_xrun(&h);
    char st[64];
    for (int i = 0; i < 24; i++) {
        SDL_snprintf(st, sizeof st, "--lw:%dpx;--rw:%dpx", 180 + i * 7, i % 3 == 0 ? 220 : 260);
        set_attr(&h, root, "style", st);
        relayout(&h, k_split, st);
        EMB_CHECK(by_id(&h, "r1")->box == r1); /* no box was built again */
        EMB_CHECK(by_id(&h, "nm")->box == nm);
        EMB_CHECK(by_id(&h, "title")->box == title);
    }
    EMB_CHECK(reused_xrun(&h) > x0);
    /* other changes between the drags: the entries of the boxes they touch, and of the boxes above, are not given back */
    set_text(&h, by_id(&h, "nm"), "A much longer name that wraps in the property grid of the right panel");
    relayout(&h, k_split, "a text change in the right panel");
    set_attr(&h, root, "style", "--lw:200px;--rw:260px");
    relayout(&h, k_split, "a drag after the text change");
    set_attr(&h, by_id(&h, "r1"), "class", "row rel");
    relayout(&h, k_split, "a row that becomes relatively positioned");
    set_attr(&h, root, "style", "--lw:210px;--rw:220px");
    relayout(&h, k_split, "a drag after the row moved");
    done(&h);
}

/* Random changes on both pages, each layout compared with an uncached build, and the library's verification on. */
static uint32_t rng;

static uint32_t rnd(uint32_t n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) % n;
}

/* The elements below <body> (the head and its style element are left alone). */
static void collect(lh *h, embi_element **out, uint32_t *n, uint32_t cap)
{
    embi_node *top = nullptr;
    for (embi_node *k = embi_document_element(h->doc)->base.first_child; k != nullptr; k = k->next) {
        embi_str name = embi_atom_str(k->name);
        if (embi_is_element(k) && name.len == 4 && memcmp(name.ptr, "body", 4) == 0) {
            top = k;
        }
    }
    EMB_REQUIRE(top != nullptr);
    for (embi_node *k = embi_next_in_tree(top, top); k != nullptr && *n < cap; k = embi_next_in_tree(top, k)) {
        if (embi_is_element(k)) {
            out[(*n)++] = embi_as_element(k);
        }
    }
}

/* drag: some steps set the root's custom properties, as a splitter drag does (k_split reads them). */
static void fuzz_ex(const char *page, float sb, uint32_t steps, uint32_t seed, bool drag)
{
    static const char *classes[] = {"", "row", "tall", "wide", "hid", "pct", "pad", "big", "f", "g", "c", "p", "h", "sc",
                                    "row big", "props", "ib"};
    static const char *styles[] = {"", "width:40%", "height:25px", "flex:2", "display:grid", "display:flex",
                                   "position:absolute;left:3px", "min-width:0", "padding:5px", "margin:3px auto",
                                   "overflow:auto;height:40px", "font-size:18px", "width:max-content", "flex-basis:30%"};
    static const char *texts[] = {"", "x", "two words", "a longer run of words that wraps inside narrow panels", "999"};
    lh h = make(page, sb);
    rng = seed;
    char what[160];
    for (uint32_t it = 0; it < steps; it++) {
        embi_element *els[256];
        uint32_t n = 0;
        collect(&h, els, &n, 256);
        EMB_REQUIRE(n > 0);
        embi_element *el = els[rnd(n)];
        switch (rnd(drag ? 7 : 5)) {
        case 0: {
            const char *v = classes[rnd(sizeof classes / sizeof classes[0])];
            set_attr(&h, el, "class", v);
            SDL_snprintf(what, sizeof what, "step %u: class [%s]", it, v);
            break;
        }
        case 1: {
            const char *v = styles[rnd(sizeof styles / sizeof styles[0])];
            set_attr(&h, el, "style", v);
            SDL_snprintf(what, sizeof what, "step %u: style [%s]", it, v);
            break;
        }
        case 2:
            if (n < 120) {
                (void)add_child(&h, el, rnd(2) == 0 ? "span" : "div", texts[rnd(5)]);
            }
            SDL_snprintf(what, sizeof what, "step %u: append", it);
            break;
        case 5:
        case 6:
            if (drag) {
                SDL_snprintf(what, sizeof what, "--lw:%upx;--rw:%upx", 60u + rnd(200), 80u + 40u * rnd(3));
                set_attr(&h, embi_document_element(h.doc), "style", what);
                SDL_snprintf(what, sizeof what, "step %u: drag", it);
                break;
            }
            [[fallthrough]];
        default:
            if (el->base.first_child != nullptr && el->base.first_child->type == EMBI_NODE_TEXT) {
                set_text(&h, el, texts[rnd(5)]);
            }
            SDL_snprintf(what, sizeof what, "step %u: text", it);
            break;
        }
        (void)embi_layout_document(h.eng, h.doc);
        if (!matches_uncached(&h, page, what)) {
            EMB_REQUIRE(false);
        }
    }
    EMB_CHECK(reused(&h) > 0);
    done(&h);
}

static void fuzz(const char *page, float sb, uint32_t steps, uint32_t seed)
{
    fuzz_ex(page, sb, steps, seed, false);
}

EMB_TEST(fuzz_shell_equals_an_uncached_layout)
{
    fuzz(k_shell, 0.0f, 300, 31);
}

EMB_TEST(fuzz_nested_with_scrollbars_equals_an_uncached_layout)
{
    fuzz(k_nest, 12.0f, 300, 77);
}

EMB_TEST(fuzz_splitter_drags_between_changes_equal_an_uncached_layout)
{
    fuzz_ex(k_split, 0.0f, 300, 2024, true);
    fuzz_ex(k_split, 12.0f, 300, 99, true);
}

EMB_TEST(fuzz_shell_with_scrollbars_equals_an_uncached_layout)
{
    fuzz(k_shell, 12.0f, 300, 5150);
}

/* Found by the fuzz: an inline box with auto horizontal margins (margin:3px auto) took them as NaN, which put its
 * fragment at an infinite x and its text at NaN. Auto margins on an inline box are 0 (CSS 2.1 10.3.1). */
EMB_TEST(auto_margins_on_an_inline_box_are_zero)
{
    const char *pages[] = {
        "<div><span id=s style='margin:3px auto'>a longer run of words</span></div>",
        ("<div style='display:flex;flex-direction:column;width:200px'><div><span>999<span id=s style='margin:3px auto'>"
         "a longer run of words</span></span></div></div>"),
        "<div style='width:max-content'><span id=s style='margin:0 auto;direction:rtl'>words here</span></div>",
    };
    for (uint32_t i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        lh h = make(pages[i], 0.0f);
        embi_sb d = {0};
        embi_layout_dump(h.eng, h.doc, &d);
        embi_sb_cstr(&d);
        EMB_CHECK(strstr(d.data, "nan") == nullptr && strstr(d.data, "inf") == nullptr);
        EMB_CHECK(i == 1 || strstr(d.data, "box <span> id=s at (0, 0)") != nullptr); /* the second follows "999" */
        embi_sb_free(&d);
        EMB_CHECK(matches_uncached(&h, pages[i], "a build with auto margins on an inline box"));
        done(&h);
    }
}

EMB_TEST_MAIN()
