/*
 * Incremental layout (W6b): what an update does with the kept box tree and the fragments.
 *
 * Two kinds of checks. The counters of the update (the kind it was, the boxes it laid out, the boxes it built, the
 * fragments it kept) are asserted for each kind of change, with the library's own verification off, so they describe the
 * real incremental state. The result is checked against an independent reference: the document is cloned into a new DOM
 * with a new style engine and a new layout engine, resolved and laid out from nothing, and the dump of the fragment tree
 * (every box with its rect and text) and the computed style of every element must be identical to the incremental ones.
 */
#include "layout_harness.h"

#include "style/inval.h"

#include <stdio.h>
#include <SDL3/SDL_iostream.h>

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

static embi_layout_incr_stats stats(lh *h)
{
    embi_layout_incr_stats st;
    embi_layout_incr_stats_get(h->eng, h->doc, &st);
    return st;
}

static uint32_t frag_count(lh *h)
{
    uint32_t boxes, frags;
    size_t bytes;
    embi_layout_stats(h->eng, h->doc, &boxes, &frags, &bytes);
    return frags;
}

static uint32_t box_count(lh *h)
{
    uint32_t boxes, frags;
    size_t bytes;
    embi_layout_stats(h->eng, h->doc, &boxes, &frags, &bytes);
    return boxes;
}

/* ------------------------------------------------------------------ the independent reference */

typedef struct ref {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
} ref;

static float g_vw = 800.0f, g_vh = 600.0f; /* the viewport of the document under test */

static ref ref_make(lh *a, const char *html)
{
    ref r;
    memset(&r, 0, sizeof r);
    embi_element *root = embi_document_element(a->doc);
    if (root == nullptr) {
        return r;
    }
    r.text = a->text; /* the text system is shared: it holds fonts and caches, not document state */
    r.style = embi_style_engine_create(embi_tshim_fonts(r.text), nullptr, nullptr);
    r.doc = embi_document_new(true);
    embi_node *copy = embi_import_node(r.doc, &root->base, true);
    if (copy == nullptr || embi_append_child(&r.doc->base, copy) != EMBI_DOM_OK) {
        printf("  cannot clone the document\n");
    }
    char *all = lh_join_styles(html, strlen(html), "");
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    r.sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)strlen(all)}, &opts);
    SDL_free(all);
    embi_css_sheet *sheets[1] = {r.sheet};
    embi_document_set_author_sheets(r.style, r.doc, sheets, 1);
    embi_style_resolve_document(r.style, r.doc);
    r.eng = embi_layout_engine_create(r.text, r.style, nullptr);
    embi_layout_set_viewport(r.eng, g_vw, g_vh);
    (void)embi_layout_document(r.eng, r.doc);
    return r;
}

static void ref_free(ref *r)
{
    embi_layout_engine_destroy(r->eng);
    embi_style_forget_document(r->style, r->doc);
    embi_document_free(r->doc);
    embi_css_sheet_unref(r->sheet);
    embi_style_engine_destroy(r->style);
}

/* The first line where two dumps differ (for the failure message). */
static void first_diff(const char *a, const char *b)
{
    uint32_t line = 1;
    const char *pa = a, *pb = b;
    while (*pa != '\0' && *pa == *pb) {
        if (*pa == '\n') {
            line++;
        }
        pa++;
        pb++;
    }
    const char *ea = strchr(pa, '\n'), *eb = strchr(pb, '\n');
    const char *la = pa, *lb = pb;
    while (la > a && la[-1] != '\n') {
        la--;
        lb--;
    }
    printf("  first difference at line %u\n  incremental: %.*s\n  reference:   %.*s\n", line,
           (int)(ea != nullptr ? (size_t)(ea - la) : strlen(la)), la, (int)(eb != nullptr ? (size_t)(eb - lb) : strlen(lb)), lb);
}

/* The incremental document equals a document built from nothing: the same fragment tree and the same computed styles. */
static bool matches_fresh(lh *a, const char *html)
{
    ref r = ref_make(a, html);
    embi_sb da = {0}, db = {0};
    embi_layout_dump(a->eng, a->doc, &da);
    embi_layout_dump(r.eng, r.doc, &db);
    embi_sb_cstr(&da);
    embi_sb_cstr(&db);
    bool same = da.len == db.len && memcmp(da.data, db.data, da.len) == 0;
    if (!same) {
        first_diff(da.data, db.data);
        const char *dump = SDL_getenv("EMB_INCR_DUMP");
        if (dump != nullptr) {
            char path[256];
            SDL_snprintf(path, sizeof path, "%s.inc.txt", dump);
            (void)SDL_SaveFile(path, da.data, da.len);
            SDL_snprintf(path, sizeof path, "%s.ref.txt", dump);
            (void)SDL_SaveFile(path, db.data, db.len);
        }
    }
    /* computed styles, element by element in tree order */
    embi_node *ta = &a->doc->base, *tb = &r.doc->base;
    embi_node *na = embi_next_in_tree(ta, ta), *nb = embi_next_in_tree(tb, tb);
    while (same && na != nullptr && nb != nullptr) {
        if (embi_is_element(na) != embi_is_element(nb)) {
            same = false;
            break;
        }
        if (embi_is_element(na)) {
            const embi_style *sa = (const embi_style *)embi_as_element(na)->style;
            const embi_style *sb = (const embi_style *)embi_as_element(nb)->style;
            if ((sa == nullptr) != (sb == nullptr) || (sa != nullptr && embi_style_diff(sa, sb) != 0)) {
                embi_str n = embi_atom_str(na->name);
                printf("  computed style of <%.*s> differs\n", (int)n.len, n.ptr);
                same = false;
            }
        }
        na = embi_next_in_tree(ta, na);
        nb = embi_next_in_tree(tb, nb);
    }
    if (same && (na != nullptr || nb != nullptr)) {
        same = false;
    }
    embi_sb_free(&da);
    embi_sb_free(&db);
    ref_free(&r);
    return same;
}

/* ------------------------------------------------------------------ pages */

static const char *k_page =
    "<style>.hot{color:red;background-color:#def} .wide{width:300px} .big{font-size:30px} "
    ".tall{height:80px} p{margin:4px 0} #box{opacity:0.5}</style>"
    "<div id=top><div id=a><p id=p1>alpha beta gamma</p><p id=p2>delta</p></div>"
    "<div id=b><p id=p3>epsilon <span id=sp>zeta</span> eta</p></div>"
    "<div id=c class=wide><span>inline</span> text <b>bold</b></div><div id=box>boxed</div></div>";

/* The page of the fuzz: descendant, sibling, :has, :not, :empty and :nth-child rules, pseudo-elements with attr(), counters,
 * a table, a list and an attr() length. */
static const char *k_rich =
    "<style>.hot{color:red;background-color:#def} .wide{width:300px} .big{font-size:30px} "
    ".tall{height:80px} p{margin:4px 0} #box{opacity:0.5} .a p, .a span{font-size:24px} p.x::before{content:attr(data-x)} "
    ".hot + p{margin-top:12px} .hot ~ b{font-weight:bold} div:has(> .big){padding:5px} p:not(.hot){letter-spacing:1px} "
    "div:empty{height:10px} li:nth-child(2n){font-size:22px} ol{counter-reset:n} li::before{counter-increment:n;content:counter(n) '. '} "
    ".w{width:attr(data-w px)} td{padding:2px;border:1px solid #888}</style>"
    "<div id=top><div id=a><p id=p1>alpha beta gamma</p><p id=p2>delta</p></div>"
    "<div id=b><p id=p3>epsilon <span id=sp>zeta</span> eta</p></div>"
    "<div id=c class=wide><span>inline</span> text <b>bold</b></div><div id=box>boxed</div>"
    "<ol id=ol><li id=l1>one</li><li id=l2>two</li><li id=l3>three</li></ol>"
    "<table id=tb><tr><td id=t1>cell a</td><td id=t2>cell b</td></tr><tr><td id=t3>cell c</td><td id=t4>d</td></tr></table>"
    "<div id=e1></div><p id=px class=x data-x=gen>generated</p><div id=wd class=w data-w=90>attr width</div></div>";

/* Many sections, like a real page: the incremental paths must touch a few of them only. */
static char *big_page(uint32_t n)
{
    embi_sb sb = {0};
    embi_sb_append(&sb, S("<style>.sec{padding:4px;border-bottom:1px solid #ccc} h3{margin:2px 0;font-size:18px} p{margin:3px 0}"
                          ".w{width:200px} .hot{color:red} .fixed{height:40px;overflow:hidden} .fl{float:left;width:30px;height:30px}"
                          ".rel{position:relative;top:3px;left:2px}</style><div id=top>"));
    for (uint32_t i = 0; i < n; i++) {
        char buf[512];
        int len = SDL_snprintf(buf, sizeof buf,
                               "<div class=sec id=s%u><h3 id=h%u>Section %u</h3><p id=p%u>Paragraph %u with <span id=sp%u>an inline "
                               "span</span> and <b>bold words</b> to make a line that wraps around.</p><div class=fixed id=f%u><p "
                               "id=q%u>fixed box %u</p></div></div>",
                               i, i, i, i, i, i, i, i, i);
        embi_sb_append(&sb, (embi_str){buf, (uint32_t)len});
    }
    embi_sb_append(&sb, S("</div>"));
    embi_sb_cstr(&sb);
    char *out = SDL_strdup(sb.data);
    embi_sb_free(&sb);
    return out;
}

/* ------------------------------------------------------------------ counters of each kind of change */

EMB_TEST(nothing_changed_keeps_the_fragments)
{
    lh h = lh_make(k_page, "");
    embi_frag *before = (embi_frag *)embi_layout_root(h.eng, h.doc);
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_NONE);
    EMB_CHECK(embi_layout_root(h.eng, h.doc) == before);
    EMB_CHECK(embi_layout_boxes_laid_out(h.eng, h.doc) == 0);
    lh_free(&h);
}

EMB_TEST(paint_only_change_does_not_lay_out)
{
    lh h = lh_make(k_page, "");
    uint32_t builds = embi_layout_tree_builds(h.eng, h.doc);
    set_attr(&h, by_id(&h, "p2"), "class", "hot");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_PAINT);
    EMB_CHECK(embi_layout_boxes_laid_out(h.eng, h.doc) == 0);
    EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) == builds);
    uint64_t serial;
    embi_element *const *els;
    uint32_t n;
    EMB_CHECK(embi_layout_paint_dirty(h.eng, h.doc, &serial, &els, &n));
    EMB_CHECK(n >= 1);
    embi_layout_paint_clean(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, k_page));
    lh_free(&h);
}

EMB_TEST(layout_change_patches_the_box_tree)
{
    lh h = lh_make(k_page, "");
    set_attr(&h, by_id(&h, "p1"), "class", "big");
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.boxes_rebuilt > 0 && st.boxes_rebuilt < 20);
    EMB_CHECK(matches_fresh(&h, k_page));
    set_text(&h, by_id(&h, "sp"), "a much longer zeta text that goes on and on and on and on and on and on and on and on");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(matches_fresh(&h, k_page));
    embi_element *p2 = by_id(&h, "p2");
    EMB_REQUIRE(embi_remove_child(p2->base.parent, &p2->base) == EMBI_DOM_OK);
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(matches_fresh(&h, k_page));
    lh_free(&h);
}

/* A text change that keeps the paragraph's height lays out the paragraph and its parent only. */
EMB_TEST(text_change_lays_out_one_paragraph)
{
    char *page = big_page(100);
    lh h = lh_make(page, "");
    uint32_t total = frag_count(&h);
    EMB_CHECK(total > 1500);
    set_text(&h, by_id(&h, "h50"), "Section fifty");
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.partial);
    EMB_CHECK(st.boxes_laid_out > 0 && st.boxes_laid_out < 40);
    EMB_CHECK(st.boxes_laid_out * 20u < total);
    EMB_CHECK(frag_count(&h) == total);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

/* A change of a box's width in a box with a fixed size stays inside it. */
EMB_TEST(width_change_lays_out_one_subtree)
{
    char *page = big_page(100);
    lh h = lh_make(page, "");
    uint32_t total = frag_count(&h);
    set_attr(&h, by_id(&h, "q70"), "class", "w");
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.partial);
    EMB_CHECK(st.boxes_laid_out > 0 && st.boxes_laid_out * 20u < total);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

/* A change of the height reaches the parent chain: the siblings are reused, not laid out again. */
EMB_TEST(height_change_reuses_the_siblings)
{
    char *page = big_page(100);
    lh h = lh_make(page, "");
    uint32_t total = frag_count(&h);
    set_text(&h, by_id(&h, "p30"),
             "Paragraph with a much longer text that must wrap onto several more lines than before, again and again and again "
             "and again and again and again and again and again and again and again and again and again and again and again.");
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.partial);
    EMB_CHECK(st.boxes_reused > 50);
    EMB_CHECK(st.boxes_laid_out * 10u < total);
    EMB_CHECK(matches_fresh(&h, page));
    /* and back */
    set_text(&h, by_id(&h, "p30"), "short");
    (void)embi_layout_document(h.eng, h.doc);
    st = stats(&h);
    EMB_CHECK(st.partial && st.boxes_laid_out * 10u < total);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

/* Box-tree surgery for inserted and removed DOM subtrees: the boxes appear and go away, and the tree is not built again. */
EMB_TEST(insert_and_remove_patch_the_boxes)
{
    char *page = big_page(40);
    lh h = lh_make(page, "");
    uint32_t builds = embi_layout_tree_builds(h.eng, h.doc);
    uint32_t boxes0 = box_count(&h), frags0 = frag_count(&h);
    embi_element *host = by_id(&h, "s20");
    embi_element *ne = nullptr;
    EMB_REQUIRE(embi_dom_create_element(h.doc, S("div"), &ne) == EMBI_DOM_OK);
    embi_element *inner = nullptr;
    EMB_REQUIRE(embi_dom_create_element(h.doc, S("p"), &inner) == EMBI_DOM_OK);
    embi_node *t = &embi_new_text(h.doc, embi_dstr_from_utf8(h.doc->slab, S("inserted paragraph")))->base;
    EMB_REQUIRE(embi_append_child(&inner->base, t) == EMBI_DOM_OK);
    EMB_REQUIRE(embi_append_child(&ne->base, &inner->base) == EMBI_DOM_OK);
    EMB_REQUIRE(embi_pre_insert(&host->base, &ne->base, host->base.first_child) == EMBI_DOM_OK);
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.boxes_rebuilt >= 3); /* the host's boxes are built again, with the two new ones */
    EMB_CHECK(box_count(&h) == boxes0 + 3); /* div, p and the text run */
    EMB_CHECK(frag_count(&h) > frags0);
    EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) == builds + 1); /* a patch counts as one build; no second full build */
    EMB_CHECK(st.partial && st.boxes_laid_out * 10u < frags0);
    EMB_CHECK(matches_fresh(&h, page));
    EMB_REQUIRE(embi_remove_child(&host->base, &ne->base) == EMBI_DOM_OK);
    (void)embi_layout_document(h.eng, h.doc);
    st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial);
    EMB_CHECK(box_count(&h) == boxes0);
    EMB_CHECK(frag_count(&h) == frags0);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

/* Floats, relative boxes and fixed-size clipped boxes in the changed path fall back or patch, never differ. */
EMB_TEST(floats_and_relative_boxes_match_a_fresh_layout)
{
    char *page = big_page(30);
    lh h = lh_make(page, "");
    EMB_CHECK(matches_fresh(&h, page));
    set_attr(&h, by_id(&h, "h10"), "class", "fl");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_attr(&h, by_id(&h, "p11"), "class", "rel");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_text(&h, by_id(&h, "p11"), "relative paragraph with a different text that is a little longer than it was");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_text(&h, by_id(&h, "p12"), "after the float: a text that wraps around the float and is longer than before to wrap more");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

EMB_TEST(viewport_change_rebuilds)
{
    lh h = lh_make(k_page, "");
    embi_layout_set_viewport(h.eng, 500.0f, 400.0f);
    g_vw = 500.0f;
    g_vh = 400.0f;
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_FULL);
    EMB_CHECK(matches_fresh(&h, k_page));
    g_vw = 800.0f;
    g_vh = 600.0f;
    lh_free(&h);
}

/* The library's own verification (EMBER_VERIFY_INCREMENTAL) compares against a throwaway build and leaves the real state
 * alone: the counters and the kinds of the update are the same with it on. */
EMB_TEST(verification_does_not_change_the_update)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    char *page = big_page(30);
    lh h = lh_make(page, "");
    set_attr(&h, by_id(&h, "p5"), "class", "hot");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_PAINT);
    EMB_CHECK(embi_layout_boxes_laid_out(h.eng, h.doc) == 0);
    uint64_t serial;
    embi_element *const *els;
    uint32_t n;
    EMB_CHECK(embi_layout_paint_dirty(h.eng, h.doc, &serial, &els, &n) && n >= 1);
    embi_layout_paint_clean(h.eng, h.doc);
    set_text(&h, by_id(&h, "h6"), "A new title");
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.boxes_laid_out < 40);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    SDL_free(page);
    lh_free(&h);
    embi_incr_set_verify(0);
}

/* ------------------------------------------------------------------ the fuzz */

static uint32_t rng_state;

static uint32_t rnd(uint32_t n)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) % n;
}

static void collect(embi_node *top, embi_element **out, uint32_t *n, uint32_t cap)
{
    for (embi_node *x = top; x != nullptr; x = embi_next_in_tree(top, x)) {
        if (embi_is_element(x) && *n < cap) {
            out[(*n)++] = embi_as_element(x);
        }
    }
}

static bool skip_fuzz(const char *name)
{
    const char *only = SDL_getenv("EMB_ONLY"); /* debugging: run one fuzz */
    return only != nullptr && strcmp(only, name) != 0;
}

static void fuzz_run(const char *page, uint32_t steps, uint32_t seed, uint32_t min_partial)
{
    lh h = lh_make(page, "");
    static const char *classes[] = {"", "hot", "wide", "big", "tall", "hot big", "wide tall", "w", "fixed", "fl", "rel", "sec", "a", "x", "a hot"};
    static const char *styles[] = {"",
                                   "color:blue",
                                   "width:120px",
                                   "display:none",
                                   "margin-top:9px",
                                   "float:left;width:50px",
                                   "position:absolute;top:3px",
                                   "opacity:0.3",
                                   "overflow:hidden;height:20px",
                                   "padding:7px 3px",
                                   "display:flex",
                                   "position:relative;left:4px;top:1px",
                                   "height:30px",
                                   "display:inline-block",
                                   "clear:both;margin-bottom:5px",
                                   "overflow:auto;height:25px"};
    static const char *texts[] = {"q", "a longer piece of text to wrap around the box a few times, a longer piece of text to wrap",
                                  "x y", "", "one two three four five six seven eight nine ten eleven twelve"};
    rng_state = seed;
    char history[6][200];
    uint32_t seen[5] = {0, 0, 0, 0, 0};
    uint32_t partial = 0;
    for (uint32_t it = 0; it < steps; it++) {
        embi_element *els[256];
        uint32_t n = 0;
        embi_element *top = by_id(&h, "top");
        collect(&top->base, els, &n, 256);
        embi_element *el = els[rnd(n)];
        const char *what = "?";
        const char *last_val = "";
        char vbuf[200];
        switch (rnd(10)) {
        case 0:
            what = "class";
            last_val = classes[rnd(15)];
            set_attr(&h, el, "class", last_val);
            break;
        case 1:
            what = "style";
            last_val = styles[rnd(16)];
            set_attr(&h, el, "style", last_val);
            break;
        case 2:
            what = "remove";
            if (el != top && n > 12) {
                (void)embi_remove_child(el->base.parent, &el->base);
            }
            break;
        case 3: {
            what = "append";
            embi_element *ne = nullptr;
            if (n < 140 && embi_dom_create_element(h.doc, S(rnd(2) == 0 ? "div" : "span"), &ne) == EMBI_DOM_OK) {
                embi_node *t = &embi_new_text(h.doc, embi_dstr_from_utf8(h.doc->slab, S(texts[rnd(5)])))->base;
                (void)embi_append_child(&ne->base, t);
                (void)embi_append_child(&el->base, &ne->base);
            }
            break;
        }
        case 4: {
            what = "insert-before";
            embi_element *ne = nullptr;
            if (n < 140 && el != top && embi_dom_create_element(h.doc, S(rnd(2) == 0 ? "p" : "div"), &ne) == EMBI_DOM_OK) {
                embi_node *t = &embi_new_text(h.doc, embi_dstr_from_utf8(h.doc->slab, S(texts[rnd(5)])))->base;
                (void)embi_append_child(&ne->base, t);
                (void)embi_pre_insert(el->base.parent, &ne->base, &el->base);
            }
            break;
        }
        case 5:
        case 6:
            what = "text";
            if (el->base.first_child != nullptr && el->base.first_child->type == EMBI_NODE_TEXT) {
                last_val = texts[rnd(5)];
                embi_chardata_set(embi_as_chardata(el->base.first_child),
                                  embi_dstr_from_utf8(h.doc->slab, S(last_val)));
            }
            break;
        case 7:
            what = "title";
            set_attr(&h, el, rnd(3) == 0 ? "title" : (rnd(2) == 0 ? "data-x" : "data-w"), rnd(2) == 0 ? "t" : "70");
            break;
        case 8:
            what = "class+style";
            {
                const char *cl = classes[rnd(15)], *stl = styles[rnd(16)];
                SDL_snprintf(vbuf, sizeof vbuf, "class=[%s] style=[%s]", cl, stl);
                last_val = vbuf;
                set_attr(&h, el, "class", cl);
                set_attr(&h, el, "style", stl);
            }
            break;
        default:
            what = "hidden";
            set_attr(&h, el, "hidden", "");
            if (rnd(2) == 0) {
                embi_el_remove_attribute(el, S("hidden"));
            }
            break;
        }
        {
            embi_str idv = {nullptr, 0};
            (void)idv;
            SDL_snprintf(history[it % 6], sizeof history[0], "step %u: %s [%s] on <%.*s> id=%s", it, what, last_val,
                         (int)embi_atom_str(el->base.name).len, embi_atom_str(el->base.name).ptr,
                         embi_el_id(el) != 0 ? embi_atom_str(embi_el_id(el)).ptr : "");
        }
        (void)embi_layout_document(h.eng, h.doc);
        embi_layout_incr_stats st = stats(&h);
        if (st.last_kind < 5) {
            seen[st.last_kind]++;
        }
        partial += st.partial ? 1u : 0u;
        if (!matches_fresh(&h, page)) {
            printf("  mismatch after step %u (%s on <%.*s>), the update was kind %u partial %d\n", it, what,
                   (int)embi_atom_str(el->base.name).len, embi_atom_str(el->base.name).ptr, st.last_kind, (int)st.partial);
            for (uint32_t k = it > 5 ? it - 5 : 0; k <= it; k++) {
                printf("    %s%c", history[k % 6], 10);
            }
            EMB_REQUIRE(false);
        }
    }
    printf("  kinds none=%u paint=%u surgery=%u full=%u partial=%u\n", seen[0], seen[1], seen[2], seen[3], partial);
    EMB_CHECK(seen[EMBI_LU_SURGERY] > steps / 10u);
    EMB_CHECK(partial >= min_partial);
    lh_free(&h);
}

EMB_TEST(fuzz_small_page_equals_a_fresh_layout)
{
    if (skip_fuzz("small")) {
        return;
    }
    fuzz_run(k_rich, 1000, 777, 60);
}

EMB_TEST(fuzz_big_page_equals_a_fresh_layout)
{
    if (skip_fuzz("big")) {
        return;
    }
    char *page = big_page(12);
    fuzz_run(page, 1000, 4242, 50);
    SDL_free(page);
}

/* The same fuzz with the library's verification on: every update is also compared with a throwaway build inside the
 * library, and none may differ. */
EMB_TEST(fuzz_with_the_library_verification)
{
    if (skip_fuzz("lib")) {
        return;
    }
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    char *page = big_page(8);
    fuzz_run(page, 300, 99, 20);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    SDL_free(page);
    embi_incr_set_verify(0);
}

/* CF1: list items with and without markers, flex and grid containers as the boxes a partial relayout starts at, and
 * ::before styles that change by class (a paint change of the pseudo-element is made in place). */
static const char *k_lists =
    "<style>.hot{color:red} .wide{width:300px} .big{font-size:30px} .tall{height:80px} .a li{list-style:square} "
    ".x::before{content:'*';color:green} .x.hot::before{color:blue} .x.big::before{content:'**'} "
    "ul{list-style:none;padding:0} "
    ".fl{display:flex;gap:4px;height:30px;align-items:center} .w{display:grid;grid-template-columns:30px 1fr} "
    ".rel{position:relative} .fixed{height:40px} .sec li{list-style:disc} .ab{position:absolute;left:0}</style>"
    "<div id=top><ul id=u1><li id=l1><div id=d1 class=w><span class=ab>a</span><label id=lb1 class=x>one</label>"
    "</div></li>"
    "<li id=l2><div class=w><span>b</span><label class=x>two</label></div></li><li id=l3>three</li></ul>"
    "<footer id=ft class=fl><span id=c1><b>3</b> left</span><ul id=f1 class=fl><li>All</li><li>Active</li></ul>"
    "<span id=tl></span>"
    "</footer><div id=m class=sec><ul><li id=m1>m one</li><li id=m2><p>m two</p></li></ul></div><p id=pp class=x>para</p></div>";

EMB_TEST(fuzz_lists_flex_and_pseudo_elements)
{
    if (skip_fuzz("lists")) {
        return;
    }
    fuzz_run(k_lists, 1000, 1313, 20);
}

EMB_TEST(fuzz_lists_with_the_library_verification)
{
    if (skip_fuzz("listslib")) {
        return;
    }
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fuzz_run(k_lists, 400, 2024, 5);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    embi_incr_set_verify(0);
}

/* A list item's color changes: its marker (a text-kind box, here inside the box of the item's first paragraph) takes the
 * new marker style. The verification compares the boxes' styles with a throwaway build. */
EMB_TEST(list_marker_color_change_refreshes_the_marker)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    const char *html = "<style>.hot{color:red} li.m::marker{color:green} li.m.on::marker{color:blue}</style>"
                       "<ul id=u><li id=a><p>alpha</p></li><li id=b class=m><div><p>beta</p></div></li><li id=c>gamma</li></ul>";
    lh h = lh_make(html, "");
    const char *ops[][2] = {{"a", "hot"}, {"b", "m on"}, {"b", "m hot"}, {"c", "hot"}, {"a", ""}, {"b", "m"}};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        set_attr(&h, by_id(&h, ops[i][0]), "class", ops[i][1]);
        (void)embi_layout_document(h.eng, h.doc);
        EMB_CHECK(embi_incr_verify_failures() == base_fail);
        EMB_CHECK(matches_fresh(&h, html));
    }
    lh_free(&h);
    embi_incr_set_verify(0);
}

/* ------------------------------------------------------------------ directed scenarios */

/* A scenario is a page and a list of DOM operations, each checked against a fresh layout of the result:
 *   c:ID=v class   s:ID=v style   t:ID=v text of the first child   a:ID:name=v attribute   d:ID:name remove attribute
 *   r:ID remove    A:ID:tag:text append a child   B:ID:tag:text insert before ID */
typedef struct scenario {
    const char *name;
    const char *page;
    const char *ops[8];
} scenario;

static const char *k_sc_css =
    "<style>.a p{font-size:30px} .b{width:120px} .hot{color:red} .big{font-size:28px} .tall{height:90px} "
    ".fl{float:left;width:40px;height:30px} p{margin:3px 0} p.x::before{content:attr(data-x)} p.x::after{content:' !'} "
    ".w{width:attr(data-w px)} .hot + p{margin-top:20px} .hot ~ i{font-weight:bold} div:has(> .big){padding:6px} "
    "p:not(.hot){letter-spacing:1px} div:empty{height:12px;background:#ccc} li:nth-child(2n){font-size:24px} "
    "li:first-child{margin-left:9px} ol{counter-reset:n} li::before{counter-increment:n;content:counter(n) '. '} "
    "td{padding:3px;border:1px solid #888} .grid{display:grid;grid-template-columns:1fr 1fr} .flex{display:flex} "
    ".sh{width:max-content}</style>";

static const scenario k_scenarios[] = {
    {"two rebuilt boxes at different depths",
     "<div id=top><p id=p1>aa</p><div id=d1><p id=p2>x</p></div></div>",
     {"c:top=a", "c:top=", "c:top=a", nullptr}},
    {"class on a container with descendant rules and a deep tree",
     "<div id=top><div id=m><p id=p1>aa</p><div id=d1><div id=d2><p id=p2>x y z</p></div></div></div><p id=p3>tail</p></div>",
     {"c:top=a", "c:m=a", "c:top=", "c:d1=a", nullptr}},
    {"attr() in content", "<div id=top><p id=p1 class=x data-x=short>text</p><p id=p2>after</p></div>",
     {"a:p1:data-x=a much longer attribute value for the pseudo", "a:p1:data-x=s", "d:p1:data-x", "c:p1=", "c:p1=x", nullptr}},
    {"attr() in a length", "<div id=top><div id=d class=w data-w=100>box</div><p>below</p></div>",
     {"a:d:data-w=50", "a:d:data-w=200", "d:d:data-w", nullptr}},
    {"title and data without attr() use", "<div id=top><p id=p1 title=a data-q=1>text</p></div>",
     {"a:p1:title=b", "a:p1:data-q=2", nullptr}},
    {"adjacent sibling", "<div id=top><p id=p1>a</p><p id=p2>b</p><p id=p3>c</p></div>",
     {"c:p1=hot", "c:p2=hot", "c:p1=", "r:p2", nullptr}},
    {"general sibling", "<div id=top><i id=i1>a</i><span id=s1>b</span><i id=i2>c</i></div>",
     {"c:s1=hot", "c:i1=hot", "r:s1", "A:top:i:new", nullptr}},
    {":has child", "<div id=top><div id=d1><p id=p1>a</p></div><div id=d2><p id=p2>b</p></div></div>",
     {"c:p1=big", "c:p2=big", "c:p1=", "r:p2", nullptr}},
    {":not", "<div id=top><p id=p1 class=hot>a</p><p id=p2>b</p></div>",
     {"c:p1=", "c:p2=hot", "c:p1=hot", nullptr}},
    {":empty", "<div id=top><div id=e></div><div id=f>x</div></div>",
     {"A:e:span:hello", "t:f=", "r:f", "A:top:div:", nullptr}},
    {"nth-child on a list", "<ul id=top><li id=l1>one</li><li id=l2>two</li><li id=l3>three</li><li id=l4>four</li></ul>",
     {"r:l1", "B:l2:li:inserted", "A:top:li:last", "c:l3=big", nullptr}},
    {"counters", "<ol id=top><li id=l1>one</li><li id=l2>two</li><li id=l3>three</li></ol>",
     {"r:l2", "A:top:li:four", "B:l1:li:zero", "s:l1=display:none", nullptr}},
    {"table",
     "<table id=top><tr id=r1><td id=c1>a</td><td id=c2>b</td></tr><tr id=r2><td id=c3>cc</td><td id=c4>d</td></tr></table>",
     {"t:c1=a much longer cell text that wraps", "c:c4=big", "r:r1", "s:c3=width:90px", nullptr}},
    {"table rows added", "<table id=top><tr id=r1><td id=c1>a</td></tr></table>",
     {"A:r1:td:b", "A:top:tr:", "c:c1=tall", nullptr}},
    {"floats",
     "<div id=top><div id=f class=fl></div><p id=p1>text next to a float that wraps around it a few times, text</p><p id=p2>x</p></div>",
     {"c:f=", "c:f=fl", "c:p1=big", "s:p2=clear:both", "r:f", nullptr}},
    {"absolute box",
     "<div id=top style=position:relative><p id=p1>a</p><div id=ab style=position:absolute;top:5px;left:5px>abs</div><p id=p2>b</p></div>",
     {"t:p1=a longer first paragraph that is now two lines long maybe if the width allows", "s:ab=position:absolute;top:50px", "r:ab",
      nullptr}},
    {"flex container", "<div id=top class=flex><p id=p1>a</p><p id=p2>bb</p></div>",
     {"c:p1=big", "t:p2=bb cc dd", "A:top:p:new", "c:top=", nullptr}},
    {"grid container", "<div id=top class=grid><p id=p1>a</p><p id=p2>bb</p><p id=p3>c</p></div>",
     {"c:p1=big", "r:p2", "B:p3:p:ins", nullptr}},
    {"shrink to fit", "<div id=top><div id=sh class=sh><p id=p1>a</p><div id=d1><p id=p2>b</p></div></div></div>",
     {"c:sh=a sh", "t:p2=a longer text", "c:sh=sh", nullptr}},
    {"display none and back", "<div id=top><div id=d1><p id=p1>a</p></div><div id=d2><p id=p2>b</p></div></div>",
     {"s:d1=display:none", "c:top=a", "s:d1=", "c:top=", nullptr}},
    {"insert and remove at depth", "<div id=top><div id=d1><div id=d2><p id=p1>a</p></div></div><p id=p2>z</p></div>",
     {"A:d2:p:new", "r:p1", "B:d2:div:ins", "c:top=a", nullptr}},
    {"margins collapse through a changing block", "<div id=top><div id=a><p id=p1>a</p></div><div id=b><p id=p2>b</p></div></div>",
     {"s:p1=margin:30px 0", "s:a=padding:5px", "s:p1=display:none", "s:b=border-top:1px solid", nullptr}},
    {"inline content", "<div id=top><p id=p1>one <b id=b1>bold</b> two <i id=i1>it</i> three</p></div>",
     {"c:b1=big", "t:b1=a lot more bold words here", "r:i1", "A:p1:span:added", nullptr}},
    {"hidden attribute", "<div id=top><div id=d1><p id=p1>a</p></div><p id=p2>b</p></div>",
     {"a:d1:hidden=", "d:d1:hidden", "a:p2:hidden=", nullptr}},
    {"height change reaches ancestors",
     "<div id=top><div id=a><div id=b><p id=p1>a</p></div></div><div id=c><p id=p2>c</p></div></div>",
     {"c:b=tall", "c:a=tall", "c:b=", "c:top=a", nullptr}},
    {"two siblings change at once",
     "<div id=top><div id=a><p id=p1>a</p></div><div id=b><p id=p2>b</p></div><div id=c><p id=p3>c</p></div></div>",
     {"c:top=a", "c:top=", "c:a=a", "c:c=a", nullptr}},
    {"text then structure", "<div id=top><p id=p1>abc</p><p id=p2>def</p></div>",
     {"t:p1=a much longer text to wrap many many times around this narrow box in the page", "A:top:p:x", "t:p2=", "r:p1",
      nullptr}},
    {"overflow and fixed height", "<div id=top><div id=f style='height:40px;overflow:hidden'><p id=p1>a</p></div><p id=p2>b</p></div>",
     {"t:p1=a very long text that overflows the fixed box many many times over and over and over again", "s:f=height:80px",
      nullptr}},
    {"relative offsets", "<div id=top><p id=p1 style='position:relative;left:5px;top:3px'>a</p><p id=p2>b</p></div>",
     {"c:p1=big", "s:p1=position:relative;left:9px", "t:p2=more text", nullptr}},
    {"pseudo element toggle", "<div id=top><p id=p1 class=x data-x=pre>a</p><p id=p2 data-x=q>b</p></div>",
     {"c:p2=x", "a:p2:data-x=longer", "c:p1=", "c:p2=", nullptr}},
};

static void apply_op(lh *h, const char *op)
{
    char buf[400];
    SDL_strlcpy(buf, op, sizeof buf);
    char kind = buf[0];
    char *id = buf + 2, *rest = strchr(id, ':'), *eq = strchr(id, '=');
    if (kind == 'c' || kind == 's' || kind == 't') {
        *eq = '\0';
        embi_element *el = by_id(h, id);
        if (kind == 't') {
            if (el->base.first_child == nullptr) {
                embi_node *t = &embi_new_text(h->doc, embi_dstr_from_utf8(h->doc->slab, S(eq + 1)))->base;
                (void)embi_append_child(&el->base, t);
            } else if (el->base.first_child->type == EMBI_NODE_TEXT) {
                set_text(h, el, eq + 1);
            }
        } else {
            set_attr(h, el, kind == 'c' ? "class" : "style", eq + 1);
        }
        return;
    }
    if (kind == 'a' || kind == 'd') {
        *rest++ = '\0';
        embi_element *el = by_id(h, id);
        if (kind == 'a') {
            char *e2 = strchr(rest, '=');
            *e2 = '\0';
            set_attr(h, el, rest, e2 + 1);
        } else {
            embi_el_remove_attribute(el, S(rest));
        }
        return;
    }
    if (kind == 'r') {
        embi_element *el = by_id(h, id);
        (void)embi_remove_child(el->base.parent, &el->base);
        return;
    }
    /* A / B: ID:tag:text */
    *rest++ = '\0';
    char *text = strchr(rest, ':');
    *text++ = '\0';
    embi_element *at = by_id(h, id), *ne = nullptr;
    EMB_REQUIRE(embi_dom_create_element(h->doc, S(rest), &ne) == EMBI_DOM_OK);
    if (text[0] != '\0') {
        (void)embi_append_child(&ne->base, &embi_new_text(h->doc, embi_dstr_from_utf8(h->doc->slab, S(text)))->base);
    }
    if (kind == 'A') {
        (void)embi_append_child(&at->base, &ne->base);
    } else {
        (void)embi_pre_insert(at->base.parent, &ne->base, &at->base);
    }
}

static bool g_sc_forbid_partial;

static void run_scenarios(void)
{
    for (size_t i = 0; i < sizeof k_scenarios / sizeof k_scenarios[0]; i++) {
        const scenario *sc = &k_scenarios[i];
        size_t css_len = strlen(k_sc_css);
        size_t page_len = strlen(sc->page);
        char *html = SDL_malloc(css_len + page_len + 1);
        SDL_memcpy(html, k_sc_css, css_len);
        SDL_memcpy(html + css_len, sc->page, page_len + 1);
        lh h = lh_make(html, "");
        for (uint32_t k = 0; sc->ops[k] != nullptr; k++) {
            apply_op(&h, sc->ops[k]);
            (void)embi_layout_document(h.eng, h.doc);
            if (g_sc_forbid_partial) {
                embi_layout_incr_stats st = stats(&h);
                EMB_CHECK(!st.partial && st.n_partial == 0);
            }
            if (!matches_fresh(&h, html)) {
                printf("  scenario '%s', step %u (%s) differs from a fresh layout\n", sc->name, k, sc->ops[k]);
                EMB_CHECK(false);
                break;
            }
        }
        lh_free(&h);
        SDL_free(html);
    }
}

EMB_TEST(scenario_table_equals_a_fresh_layout)
{
    run_scenarios();
}

EMB_TEST(scenario_table_with_no_partial_relayout)
{
    /* the surgery path without the partial relayout must agree too */
    embi_lay_set_partial_enabled(false);
    g_sc_forbid_partial = true;
    run_scenarios();
    g_sc_forbid_partial = false;
    embi_lay_set_partial_enabled(true);
}

EMB_TEST(two_rebuilt_boxes_at_different_depths)
{
    const char *html = "<style>.a p{font-size:30px}</style>"
                       "<div id=top><p id=p1>aa</p><div id=d1><p id=p2>x</p></div></div>";
    lh h = lh_make(html, "");
    set_attr(&h, by_id(&h, "top"), "class", "a");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    lh_free(&h);
}

EMB_TEST(attr_function_in_content_and_in_a_length)
{
    const char *html = "<style>p.x::before{content:attr(data-x)} .w{width:attr(data-w px)}</style>"
                       "<div id=top><p id=p class=x data-x=a>t</p><div id=d class=w data-w=100>box</div></div>";
    lh h = lh_make(html, "");
    set_attr(&h, by_id(&h, "p"), "data-x", "a long value for the generated content of the paragraph");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    set_attr(&h, by_id(&h, "d"), "data-w", "50");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    lh_free(&h);
}

/* ------------------------------------------------------------------ CF1: the TodoMVC toggle-all click */

/* The shape of the TodoMVC page: a list of rows with out-of-flow boxes, list items without markers, and a flex footer of a
 * fixed height whose last item changes (an empty span becomes a button with text, which moves the footer's last
 * baseline). */
static char *todo_page(uint32_t n)
{
    embi_sb sb = {0};
    embi_sb_append(&sb, S("<style>ul{list-style:none;margin:0;padding:0} .app{position:relative;margin:40px 0} "
                          ".ab{position:absolute;top:0;left:0} h1{position:absolute;top:-30px;margin:0} "
                          ".row{display:grid;grid-template-columns:40px 1fr 40px;position:relative} "
                          ".ft{display:flex;height:20px;padding:10px;justify-content:space-between;align-items:center} "
                          ".filters{display:flex;gap:6px} button{line-height:20px;border:0;padding:0}</style>"
                          "<div class=app id=app><h1>todos</h1><ul id=list>"));
    for (uint32_t i = 0; i < n; i++) {
        char buf[256];
        int len = SDL_snprintf(buf, sizeof buf,
                               "<li id=r%u><div class=row><input class=ab type=checkbox><label>Item %u</label></div></li>",
                               i, i);
        embi_sb_append(&sb, (embi_str){buf, (uint32_t)len});
    }
    embi_sb_append(&sb, S("</ul><footer class=ft id=ft><span id=cnt><b id=num>3</b> items left</span><ul class=filters>"
                          "<li>All</li><li>Active</li><li>Done</li></ul><span id=tail></span></footer></div>"));
    embi_sb_cstr(&sb);
    char *out = SDL_strdup(sb.data);
    embi_sb_free(&sb);
    return out;
}

/* Replaces the last child of the footer with a new element: a button with text, or an empty span. */
static void swap_tail(lh *h, bool button)
{
    embi_element *ft = by_id(h, "ft");
    embi_node *last = ft->base.last_child;
    EMB_REQUIRE(last != nullptr);
    embi_element *nx = nullptr;
    EMB_REQUIRE(embi_dom_create_element(h->doc, S(button ? "button" : "span"), &nx) == EMBI_DOM_OK);
    if (button) {
        EMB_REQUIRE(embi_append_child(&nx->base, &embi_new_text_utf8(h->doc, S("Clear completed"))->base) == EMBI_DOM_OK);
    }
    EMB_REQUIRE(embi_replace_child(&ft->base, &nx->base, last) == EMBI_DOM_OK);
}

/* The footer's content changes: the footer is built again on its own (its list items take part in no counter that
 * anything reads), laid out again on its own (a flex container is recorded where a block lays it out), and the last
 * baseline it gives its ancestors moves up the plain blocks (bl_patch). Nothing else is laid out. */
EMB_TEST(flex_footer_change_lays_out_only_the_footer)
{
    char *page = todo_page(60);
    lh h = lh_make(page, "");
    uint32_t total = frag_count(&h);
    uint32_t builds = stats(&h).tree_builds;
    swap_tail(&h, true);
    (void)embi_layout_document(h.eng, h.doc);
    embi_layout_incr_stats st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(st.partial);
    EMB_CHECK(st.boxes_laid_out > 0 && st.boxes_laid_out < 30);
    EMB_CHECK(st.boxes_rebuilt < 30);
    EMB_CHECK(st.tree_builds == builds);
    EMB_CHECK(matches_fresh(&h, page));
    /* and back: the button goes, an empty span comes */
    swap_tail(&h, false);
    (void)embi_layout_document(h.eng, h.doc);
    st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.boxes_laid_out < 30);
    EMB_CHECK(frag_count(&h) == total);
    EMB_CHECK(matches_fresh(&h, page));
    /* the count changes (text in an inline of a flex item) */
    set_text(&h, by_id(&h, "num"), "0");
    (void)embi_layout_document(h.eng, h.doc);
    st = stats(&h);
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.boxes_laid_out < 30);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
}

/* The same changes with a row's class at the same time, under the library's verification (box styles, flags and fragments
 * against a throwaway build). */
EMB_TEST(flex_footer_change_passes_the_verification)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    char *page = todo_page(20);
    lh h = lh_make(page, "");
    for (int i = 0; i < 4; i++) {
        swap_tail(&h, i % 2 == 0);
        set_attr(&h, by_id(&h, "r3"), "class", i % 2 == 0 ? "done" : "");
        (void)embi_layout_document(h.eng, h.doc);
        EMB_CHECK(stats(&h).last_kind == EMBI_LU_SURGERY);
    }
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    EMB_CHECK(matches_fresh(&h, page));
    SDL_free(page);
    lh_free(&h);
    embi_incr_set_verify(0);
}

/* A list item's content changes in a list whose markers are symbols (they read no counter value): no full build. A
 * numbered list reads the counters: changes there keep the numbers right. */
EMB_TEST(list_items_without_numbers_can_be_rebuilt)
{
    const char *html = "<style>.big{font-size:30px} li{margin:2px}</style>"
                       "<div id=top><ul id=u><li id=a><div id=ad>alpha</div></li><li id=b><div id=bd>beta</div></li>"
                       "<li id=c><div>gamma</div></li></ul>"
                       "<ol id=o><li id=x><div id=xd>one</div></li><li id=y>two</li><li id=z>three</li></ol></div>";
    lh h = lh_make(html, "");
    uint32_t builds = stats(&h).tree_builds;
    set_attr(&h, by_id(&h, "bd"), "class", "big");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    set_attr(&h, by_id(&h, "xd"), "class", "big");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    embi_element *y = by_id(&h, "y");
    EMB_REQUIRE(embi_remove_child(y->base.parent, &y->base) == EMBI_DOM_OK);
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, html));
    (void)builds;
    lh_free(&h);

    /* without numbered markers anywhere, a rebuilt list item is a surgery, not a build */
    const char *plain = "<style>.big{font-size:30px} ul{list-style:square}</style>"
                        "<div id=top><ul><li><div id=a1>alpha</div></li><li><div id=b1>beta</div></li></ul></div>";
    lh g = lh_make(plain, "");
    builds = stats(&g).tree_builds;
    set_attr(&g, by_id(&g, "b1"), "class", "big");
    (void)embi_layout_document(g.eng, g.doc);
    EMB_CHECK(stats(&g).last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(stats(&g).tree_builds == builds);
    EMB_CHECK(matches_fresh(&g, plain));
    lh_free(&g);
}

/* A ::before whose color changes is a paint change: its boxes take the new style and nothing is laid out; one whose
 * content changes is built again. The verification compares the boxes' styles with a throwaway build. */
EMB_TEST(pseudo_element_paint_change_keeps_the_boxes)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    const char *html = "<style>.t::before{content:'>';color:red} .t.on::before{color:blue} .t.wide::before{content:'>>>>'}"
                       "</style><div id=top><p id=p class=t>text</p><p id=q>other</p></div>";
    lh h = lh_make(html, "");
    set_attr(&h, by_id(&h, "p"), "class", "t on");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_PAINT);
    EMB_CHECK(embi_layout_boxes_laid_out(h.eng, h.doc) == 0);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    set_attr(&h, by_id(&h, "p"), "class", "t wide");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_SURGERY);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    EMB_CHECK(matches_fresh(&h, html));
    lh_free(&h);
    embi_incr_set_verify(0);
}

/* ---- CF1 review: the counter read flag, the baseline patch and the takeover of a rebuilt box ---- */

static embi_layout_incr_stats relayout(lh *h, const char *html, const char *what)
{
    (void)embi_layout_document(h->eng, h->doc);
    embi_layout_incr_stats st = stats(h);
    if (SDL_getenv("EMB_INCR_TRACE") != nullptr) {
        SDL_Log("  %s: kind %u partial %d laid out %u rebuilt %u reused %u builds %u", what, st.last_kind, st.partial,
                st.boxes_laid_out, st.boxes_rebuilt, st.boxes_reused, st.tree_builds);
    }
    if (!matches_fresh(h, html)) {
        SDL_Log("  differs from a fresh layout after: %s", what);
        EMB_CHECK(false);
    }
    return st;
}

static embi_element *add_child(lh *h, embi_element *host, const char *tag, const char *text)
{
    embi_element *ne = nullptr;
    EMB_CHECK(embi_dom_create_element(h->doc, S(tag), &ne) == EMBI_DOM_OK);
    if (text != nullptr) {
        embi_node *t = &embi_new_text(h->doc, embi_dstr_from_utf8(h->doc->slab, S(text)))->base;
        EMB_CHECK(embi_append_child(&ne->base, t) == EMBI_DOM_OK);
    }
    EMB_CHECK(embi_append_child(&host->base, &ne->base) == EMBI_DOM_OK);
    return ne;
}

/* A tree that reads no counter value may rebuild list items and boxes with counter properties in place (a symbol marker
 * and counter-reset do not read the counters). A surgery whose new boxes would read one (a counter() in content, a
 * numbered marker, a quote) cannot run without the counter state: it falls back to a full build. Once the tree reads
 * counters, the old rule holds (a subtree that takes part in the counters is not rebuilt in place). */
EMB_TEST(counter_reads_introduced_by_a_surgery_fall_back_to_a_full_build)
{
    const char *html = "<style>ul{list-style:square} .big{font-size:30px} .c::before{content:counter(list-item) '. '}"
                       " .d{list-style-type:decimal} .q::before{content:open-quote} .r{counter-reset:k 3}</style>"
                       "<div id=top><ul id=u><li id=a><div id=ad>alpha</div></li><li id=b>beta</li><li id=c>gamma</li>"
                       "</ul><p id=p>para</p><p id=p2>more</p></div>";
    lh h = lh_make(html, "");
    uint32_t builds = stats(&h).tree_builds;
    set_attr(&h, by_id(&h, "a"), "class", "big"); /* the list item itself, with its symbol marker */
    embi_layout_incr_stats st = relayout(&h, html, "symbol-marker item rebuilt");
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.tree_builds == builds);
    set_attr(&h, by_id(&h, "b"), "class", "r");
    st = relayout(&h, html, "counter-reset on an item");
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.tree_builds == builds);
    set_attr(&h, by_id(&h, "b"), "class", "d");
    st = relayout(&h, html, "a decimal marker comes");
    EMB_CHECK(st.tree_builds == builds + 1);
    lh_free(&h);

    static const char *const reads[] = {"c", "q"};
    for (int i = 0; i < 2; i++) {
        lh g = lh_make(html, "");
        builds = stats(&g).tree_builds;
        set_attr(&g, by_id(&g, "p"), "class", reads[i]);
        st = relayout(&g, html, i == 0 ? "a counter() comes" : "a quote comes");
        EMB_CHECK(st.tree_builds == builds + 1);
        /* now the tree reads counters: a rebuilt list item is a full build, a counter-free box is still a surgery */
        builds = st.tree_builds;
        set_attr(&g, by_id(&g, "c"), "class", "big");
        st = relayout(&g, html, "item rebuilt in a counter-reading tree");
        EMB_CHECK(st.tree_builds == builds + 1);
        builds = st.tree_builds;
        set_attr(&g, by_id(&g, "p2"), "class", "big");
        st = relayout(&g, html, "counter-free box rebuilt in a counter-reading tree");
        EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.tree_builds == builds);
        lh_free(&g);
    }
}

/* A box laid out again with the same size whose baselines moved (a bigger font in a fixed line height): its ancestors'
 * baselines follow when they took theirs from it, stay when another in-flow child gives them (an earlier or a later
 * one; an empty block, an absolutely positioned box and a float do not), and an ancestor that reads its children's
 * baselines (an inline formatting context around an inline-block, a flex container aligning by baseline) is laid out
 * again. Each step is compared with a fresh layout and with the library's verification, which compares the baselines
 * of every block fragment. */
EMB_TEST(baseline_moves_follow_the_blocks_that_take_them)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    static const char *const bodies[] = {
        "<div id=n0><div id=n1><div id=r class=rr>plain chain up to the root</div></div></div>",
        "<div id=n0><div id=n1><div id=r class=rr>plain chain up to the root</div></div></div><p>x</p><p>y</p>",
        "<div id=n0><div id=n1><div id=r class=rr>plain chain up to the root</div><p>z</p></div></div><p>x</p>",
        "<div>x <span class=ib><div id=m><div id=r class=rr>nested in an inline-block</div></div></span> y</div>",
        "<div>x <div class=ib><div></div><div>first</div><div id=r class=rr>between</div><div>last</div></div> y</div>",
        ("<div>x <div class=ib><div></div><div class=ab>abs</div><div id=r class=rr>only source</div>"
         "<div class=fl>fl</div></div> y</div>"),
        "<div class=fx><div>a</div><div><div id=r class=rr>flex item child</div></div></div>",
    };
    for (int i = 0; i < 7; i++) {
        char html[1024];
        SDL_snprintf(html, sizeof html,
                     "<style>.rr{line-height:60px;font-size:12px} .rr.big{font-size:20px} .ib{display:inline-block}"
                     " .fx{display:flex;align-items:baseline} .ab{position:absolute} .fl{float:left}</style>"
                     "<div id=top>%s<div id=tail>after</div></div>", bodies[i]);
        lh h = lh_make(html, "");
        for (int k = 0; k < 2; k++) {
            set_attr(&h, by_id(&h, "r"), "class", k == 0 ? "rr big" : "rr");
            embi_layout_incr_stats st = relayout(&h, html, bodies[i]);
            if (i < 3) { /* plain blocks up to the root: the baselines are patched, nothing above r is laid out */
                EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.boxes_laid_out <= 4);
            }
        }
        lh_free(&h);
    }
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    embi_incr_set_verify(0);
}

/* A box built again whose style stayed (a child came or went) is laid out on its own where its old fragment was; when
 * its size and the extent of its content stay that is all, when they change its parent is laid out next. Plain blocks
 * and flex containers alike. boxes_reused counts the fragments a parent's relayout kept: 0 when no parent was laid out. */
EMB_TEST(rebuilt_box_with_the_same_style_is_laid_out_where_it_was)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    embi_sb sb = {0};
    embi_sb_append(&sb, EMBI_STR("<style>.fx{display:flex;height:30px} #fixed{height:200px}</style><div id=top>"
                                 "<div id=fixed><span>one</span></div><div id=grow><p>two</p></div>"
                                 "<div id=fx class=fx><span>a</span></div>"));
    for (int i = 0; i < 40; i++) {
        embi_sb_append(&sb, EMBI_STR("<p>row of text</p>"));
    }
    embi_sb_append(&sb, EMBI_STR("</div>"));
    embi_sb_cstr(&sb);
    const char *html = sb.data;
    lh h = lh_make(html, "");
    uint32_t builds = stats(&h).tree_builds;
    embi_element *fixed = by_id(&h, "fixed");
    EMB_REQUIRE(embi_remove_child(&fixed->base, fixed->base.first_child) == EMBI_DOM_OK);
    (void)add_child(&h, fixed, "span", "one"); /* a new child laid out as the old one was: the content keeps its extent */
    embi_layout_incr_stats st = relayout(&h, html, "child of a block swapped");
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.tree_builds == builds);
    EMB_CHECK(st.boxes_laid_out <= 12); /* the block and its children (with their lines and text), nothing above */
    EMB_CHECK(st.boxes_reused == 0);    /* the parent was not laid out (it would keep its other children's fragments) */
    (void)add_child(&h, by_id(&h, "grow"), "div", "added");
    st = relayout(&h, html, "child into an auto-height block");
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.tree_builds == builds);
    EMB_CHECK(st.boxes_laid_out < 30); /* the block, then the parent (the siblings below move, their fragments stay) */
    EMB_CHECK(st.boxes_reused > 0);
    (void)add_child(&h, by_id(&h, "fx"), "span", "b");
    st = relayout(&h, html, "item into a fixed-height flex container");
    EMB_CHECK(st.last_kind == EMBI_LU_SURGERY && st.partial && st.tree_builds == builds);
    EMB_CHECK(st.boxes_laid_out <= 8 && st.boxes_reused == 0);
    embi_element *fx = by_id(&h, "fx");
    EMB_REQUIRE(embi_remove_child(&fx->base, fx->base.last_child) == EMBI_DOM_OK);
    (void)relayout(&h, html, "item out of the flex container");
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    lh_free(&h);
    embi_sb_free(&sb);
    embi_incr_set_verify(0);
}

/* ANIMSCALE: an update that restyles more elements than the grave's fixed bound (2048 old styles) is still the update its
 * change calls for: a color change on thousands of elements stays a paint-only update (no tree build), and a width change on
 * as many gives the layout a fresh build would. */
EMB_TEST(restyling_thousands_of_elements_stays_paint_only)
{
    enum { SECTIONS = 1200 };
    char *page = big_page(SECTIONS);
    lh h = lh_make(page, "");
    uint32_t builds = embi_layout_tree_builds(h.eng, h.doc);
    for (uint32_t i = 0; i < SECTIONS; i++) {
        char id[24];
        SDL_snprintf(id, sizeof id, "s%u", i);
        set_attr(&h, by_id(&h, id), "class", "sec hot");
    }
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_PAINT);
    EMB_CHECK(embi_layout_boxes_laid_out(h.eng, h.doc) == 0);
    EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) == builds);
    EMB_CHECK(embi_style_grave_count(h.style, h.doc) == 0); /* the old styles went with the update */
    EMB_CHECK(matches_fresh(&h, page));
    /* the same again, back: the boxes' styles were refreshed, so nothing held an old one */
    for (uint32_t i = 0; i < SECTIONS; i++) {
        char id[24];
        SDL_snprintf(id, sizeof id, "s%u", i);
        set_attr(&h, by_id(&h, id), "class", "sec");
    }
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind == EMBI_LU_PAINT);
    EMB_CHECK(embi_layout_tree_builds(h.eng, h.doc) == builds);
    EMB_CHECK(matches_fresh(&h, page));
    for (uint32_t i = 0; i < SECTIONS; i += 2) {
        char id[24];
        SDL_snprintf(id, sizeof id, "s%u", i);
        set_attr(&h, by_id(&h, id), "class", "sec w");
    }
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(stats(&h).last_kind != EMBI_LU_PAINT && stats(&h).last_kind != EMBI_LU_NONE);
    EMB_CHECK(matches_fresh(&h, page));
    lh_free(&h);
    SDL_free(page);
}


/* A multi-column container is never part of a partial relayout's chain: an edit inside its columns, or of the container, gives
 * what a fresh layout gives (the cut fragments and the column sets included). */
EMB_TEST(edits_inside_columns_match_a_fresh_layout)
{
    static const char *page =
        "<style>#mc{width:300px;column-count:3;column-gap:10px;column-rule:1px solid #888;font:16px/20px sans-serif} "
        "p{margin:6px 0;border:1px solid #aaa} #sp{column-span:all}</style>"
        "<div id=top><div id=mc><p id=p1>one<br>two<br>three<br>four</p><p id=p2>five<br>six<br>seven</p>"
        "<div id=sp>span</div><p id=p3>eight<br>nine<br>ten<br>eleven<br>twelve</p></div><div id=after>after</div></div>";
    lh h = lh_make(page, "");
    EMB_CHECK(matches_fresh(&h, page));
    {   /* the three columns exist: the box of the first paragraph is cut or the next one stands in another column */
        embi_rectf r[4];
        EMB_CHECK(embi_layout_client_rects(h.eng, by_id(&h, "p2"), r, 4) >= 1 && r[0].x > 50.0f);
    }
    set_text(&h, by_id(&h, "after"), "after the columns, changed");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_attr(&h, by_id(&h, "p2"), "style", "break-before:column;height:70px");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_attr(&h, by_id(&h, "mc"), "style", "column-count:2;column-fill:auto;height:90px");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    set_attr(&h, by_id(&h, "p1"), "style", "margin-top:30px");
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(matches_fresh(&h, page));
    lh_free(&h);
}

EMB_TEST_MAIN()
