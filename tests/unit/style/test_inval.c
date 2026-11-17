/*
 * Invalidation and partial restyle (W6b): a DOM change restyles only the elements its selectors can reach, the change set
 * names what changed and how, and the result always equals a full resolve (checked by the verification of the engine,
 * which these tests run in counting mode, and by a fuzz of random mutations).
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <string.h>

typedef struct fx {
    embi_style_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
} fx;

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static fx make(const char *html, const char *css)
{
    fx f;
    memset(&f, 0, sizeof f);
    f.doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(f.doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    f.eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    f.sheet = embi_css_sheet_parse(S(css), &opts);
    embi_css_sheet *sheets[1] = {f.sheet};
    embi_document_set_author_sheets(f.eng, f.doc, sheets, 1);
    return f;
}

static void destroy(fx *f)
{
    embi_style_forget_document(f->eng, f->doc);
    embi_document_free(f->doc);
    embi_style_engine_destroy(f->eng);
    embi_css_sheet_unref(f->sheet);
}

static embi_element *by_id(fx *f, const char *id)
{
    embi_element *el = embi_get_element_by_id(f->doc, S(id));
    EMB_CHECK(el != nullptr);
    return el;
}

static void set_attr(fx *f, embi_element *el, const char *name, const char *value)
{
    EMB_REQUIRE(embi_el_set_attribute(el, S(name), embi_dstr_from_utf8(f->doc->slab, S(value))) == EMBI_DOM_OK);
}

static const char *k_page =
    "<html><body><div id=top><ul id=list>"
    "<li id=a>one</li><li id=b>two</li><li id=c class=x>three</li><li id=d>four</li><li id=e>five</li>"
    "</ul><p id=para>text <span id=sp>span</span></p></div></body></html>";

static const char *k_css =
    ".hot{color:red} .wide{width:300px} .hot + li{background-color:#eee} #list .deep{margin-left:3px}"
    "li:nth-child(odd){padding-top:2px} [data-k=v]{border-top-width:1px}";

static uint64_t total(fx *f)
{
    embi_style_restyle_stats st;
    embi_style_restyle_stats_get(f->eng, f->doc, &st);
    return st.total_restyled;
}

EMB_TEST(class_change_restyles_only_the_reachable_elements)
{
    embi_incr_set_verify(2);
    fx f = make(k_page, k_css);
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    EMB_CHECK(t0 > 5);
    set_attr(&f, by_id(&f, "b"), "class", "hot"); /* b itself, and the li after it through the sibling rule */
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t d = total(&f) - t0;
    EMB_CHECK(d >= 1 && d <= 3);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

EMB_TEST(text_and_neutral_attribute_changes_restyle_nothing)
{
    embi_incr_set_verify(2);
    fx f = make(k_page, k_css);
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    set_attr(&f, by_id(&f, "para"), "title", "hello");
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) == t0);
    set_attr(&f, by_id(&f, "para"), "data-q", "1"); /* no rule reads it */
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) == t0);
    embi_node *t = by_id(&f, "a")->base.first_child;
    EMB_REQUIRE(t != nullptr);
    embi_chardata_set(embi_as_chardata(t), embi_dstr_from_utf8(f.doc->slab, S("uno")));
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) == t0);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

EMB_TEST(attribute_selector_reaches_the_element)
{
    embi_incr_set_verify(2);
    fx f = make(k_page, k_css);
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    set_attr(&f, by_id(&f, "d"), "data-k", "v");
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t d = total(&f) - t0;
    EMB_CHECK(d >= 1 && d <= 2);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

static const embi_style_change *find_change(const embi_style_delta *d, const embi_element *el)
{
    for (uint32_t i = 0; i < d->nchanges; i++) {
        if (d->changes[i].el == el) {
            return &d->changes[i];
        }
    }
    return nullptr;
}

EMB_TEST(change_set_classifies_paint_and_layout)
{
    embi_incr_set_verify(2);
    fx f = make(k_page, k_css);
    embi_style_track(f.eng, f.doc, true);
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_delta d;
    embi_style_take_delta(f.eng, f.doc, &d);
    EMB_CHECK(d.lost); /* the first take of a document starts from a full build */
    embi_style_release_grave(f.eng, f.doc);

    embi_element *para = by_id(&f, "para");
    set_attr(&f, para, "class", "hot"); /* color only: a paint change (and the span below inherits it) */
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    EMB_CHECK(!d.lost);
    const embi_style_change *c = find_change(&d, para);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->cls == EMBI_SCH_PAINT);
    c = find_change(&d, by_id(&f, "sp"));
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->cls == EMBI_SCH_PAINT);
    embi_style_release_grave(f.eng, f.doc);

    set_attr(&f, para, "class", "hot wide"); /* a width: layout */
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    c = find_change(&d, para);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->cls == EMBI_SCH_LAYOUT);
    embi_style_release_grave(f.eng, f.doc);

    embi_style_resolve_document(f.eng, f.doc); /* nothing changed */
    embi_style_take_delta(f.eng, f.doc, &d);
    EMB_CHECK(!d.lost && d.nchanges == 0 && d.norigins == 0);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

EMB_TEST(text_change_is_an_origin_not_a_style_change)
{
    fx f = make(k_page, k_css);
    embi_style_track(f.eng, f.doc, true);
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_delta d;
    embi_style_take_delta(f.eng, f.doc, &d);
    embi_style_release_grave(f.eng, f.doc);
    embi_element *a = by_id(&f, "a");
    embi_chardata_set(embi_as_chardata(a->base.first_child), embi_dstr_from_utf8(f.doc->slab, S("uno")));
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    EMB_CHECK(!d.lost && d.nchanges == 0);
    EMB_REQUIRE(d.norigins == 1);
    EMB_CHECK(d.origins[0].el == a && (d.origins[0].bits & EMBI_ORIG_TEXT) != 0);
    destroy(&f);
}

/* ---- the fuzz: random mutations, every resolve compared with a full one by the verification of the engine ---- */

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

/* html, head and body stay: the fuzz removes and fills the rest. */
static bool keep(fx *f, const embi_element *el)
{
    embi_element *root = embi_document_element(f->doc);
    return el == root || el->base.parent == &root->base;
}

/* the richer selectors the first fuzz lacks: :has, ~, :not, :empty, nth-child with an of-type and pseudo-elements, attr() */
static const char *k_page2 =
    "<html><body><div id=top><ul id=list>"
    "<li id=a>one</li><li id=b>two</li><li id=c class=x>three</li><li id=d>four</li><li id=e></li>"
    "</ul><p id=para class=x data-x=g>text <span id=sp>span</span><b id=bb>b</b></p><div id=em></div>"
    "<div id=w2 class=w data-w=40>w</div></div></body></html>";

static const char *k_css2 =
    ".hot{color:red} .wide{width:300px} .hot + li{background-color:#eee} .hot ~ b{font-weight:bold} #list .deep{margin-left:3px}"
    "li:nth-child(odd){padding-top:2px} li:nth-last-child(2){margin-top:3px} [data-k=v]{border-top-width:1px}"
    "div:has(> .x){padding:3px} ul:has(li.hot + li){margin-top:5px} li:not(.hot):not(.x){color:#333} div:empty{height:5px}"
    "li:empty{height:9px} p.x::before{content:attr(data-x)} li::after{content:'.'} .w{width:attr(data-w px)} .deep > li{color:blue}";

static void fuzz_partial(const char *page, const char *css, uint32_t seed)
{
    embi_incr_set_verify(2);
    fx f = make(page, css);
    embi_style_track(f.eng, f.doc, true);
    embi_style_resolve_document(f.eng, f.doc);
    static const char *classes[] = {"", "hot", "wide", "x", "deep", "hot deep", "wide x"};
    static const char *styles[] = {"", "color:blue", "width:12px", "display:none", "margin-top:4px;color:green"};
    rng_state = seed;
    for (int it = 0; it < 1000; it++) {
        embi_element *els[256];
        uint32_t n = 0;
        collect(&embi_document_element(f.doc)->base, els, &n, 256);
        embi_element *el = els[rnd(n)];
        switch (rnd(7)) {
        case 0:
            set_attr(&f, el, "class", classes[rnd(7)]);
            break;
        case 1:
            set_attr(&f, el, "style", styles[rnd(5)]);
            break;
        case 2:
            if (!keep(&f, el)) {
                (void)embi_remove_child(el->base.parent, &el->base);
            }
            break;
        case 3: {
            embi_element *ne = nullptr;
            if (n < 200 && embi_dom_create_element(f.doc, S("li"), &ne) == EMBI_DOM_OK) {
                if (rnd(2) == 0) {
                    set_attr(&f, ne, "class", classes[rnd(7)]);
                }
                (void)embi_append_child(&el->base, &ne->base);
            }
            break;
        }
        case 4:
            set_attr(&f, el, rnd(3) == 0 ? "data-w" : (rnd(2) == 0 ? "data-x" : "data-k"), rnd(2) == 0 ? "v" : "w");
            break;
        case 5:
            if (el->base.first_child != nullptr && el->base.first_child->type == EMBI_NODE_TEXT) {
                embi_chardata_set(embi_as_chardata(el->base.first_child), embi_dstr_from_utf8(f.doc->slab, S("zz")));
            }
            break;
        default:
            set_attr(&f, el, "title", rnd(2) == 0 ? "t" : "u");
            break;
        }
        embi_style_resolve_document(f.eng, f.doc);
        embi_style_delta d;
        embi_style_take_delta(f.eng, f.doc, &d);
        embi_style_release_grave(f.eng, f.doc);
    }
    EMB_CHECK(embi_incr_verify_failures() == 0);
    embi_style_restyle_stats st;
    embi_style_restyle_stats_get(f.eng, f.doc, &st);
    EMB_CHECK(st.partial_passes > 500); /* the partial path ran, not a full pass every time */
    destroy(&f);
    embi_incr_set_verify(0);
}

EMB_TEST(fuzz_partial_restyle_equals_full)
{
    fuzz_partial(k_page, k_css, 12345);
}

EMB_TEST(fuzz_partial_restyle_with_rich_selectors)
{
    fuzz_partial(k_page2, k_css2, 987);
}

/* attr() in a rule makes every attribute matter: a change must restyle the element even when no selector names it. */
EMB_TEST(attr_function_makes_any_attribute_matter)
{
    embi_incr_set_verify(2);
    fx f = make(k_page2, k_css2);
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    set_attr(&f, by_id(&f, "w2"), "data-w", "90");
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) > t0);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

/* A restyle that recomputes declarations with var() and gets the same substituted text must not grow the engine arena: the
 * value is looked up, not parsed again (an animation restyles every frame; parsing each time grew the viewer ~5 MB/s). */
static const char *k_vpage =
    "<html><body><div id=box><p id=v1>one</p><p id=v2>two</p><p id=v3>three</p></div></body></html>";
static const char *k_vcss =
    ":root{--c:#3366ff;--w:10px} p{color:var(--c);width:calc(var(--w) * 2);margin:var(--w) 0;"
    "background-image:linear-gradient(var(--c), blue);box-shadow:0 0 var(--w) var(--c)} .hot{padding-top:1px}";

EMB_TEST(var_restyle_does_not_grow_the_engine_arena)
{
    fx f = make(k_vpage, k_vcss);
    embi_style_resolve_document(f.eng, f.doc);
    const char *ids[3] = {"v1", "v2", "v3"};
    for (int i = 0; i < 4; i++) { /* warm up: both class states seen once */
        set_attr(&f, by_id(&f, ids[i % 3]), "class", (i & 1) ? "" : "hot");
        embi_style_resolve_document(f.eng, f.doc);
    }
    embi_style_stats a;
    embi_style_engine_stats(f.eng, &a);
    uint64_t t0 = total(&f);
    for (int i = 0; i < 3000; i++) {
        set_attr(&f, by_id(&f, ids[i % 3]), "class", ((i / 3) & 1) ? "" : "hot");
        embi_style_resolve_document(f.eng, f.doc);
    }
    embi_style_stats b;
    embi_style_engine_stats(f.eng, &b);
    EMB_CHECK(total(&f) - t0 >= 2900); /* the changes really restyled their elements */
    EMB_CHECK(b.arena_bytes == a.arena_bytes);
    destroy(&f);
}

/* ------------------------------------------------------------------ CF1 */

/* A page with a list, rows with a marked span, and filler paragraphs: the restyles of the tests below are counted against
 * its size. */
static char *row_page(uint32_t fill)
{
    embi_sb sb = {0};
    embi_sb_append(&sb, S("<html><body><div id=top><ul id=list><li id=a><span class=x id=ax>a</span><span id=ay>y</span></li>"
                          "<li id=b><span class=x id=bx>b</span><span id=by>y</span></li></ul><div id=box>"));
    for (uint32_t i = 0; i < fill; i++) {
        char buf[96];
        int len = SDL_snprintf(buf, sizeof buf, "<p class=%s>filler %u</p>", (i & 1) ? "item" : "other", i);
        embi_sb_append(&sb, (embi_str){buf, (uint32_t)len});
    }
    embi_sb_append(&sb, S("</div></div></body></html>"));
    embi_sb_cstr(&sb);
    char *out = SDL_strdup(sb.data);
    embi_sb_free(&sb);
    return out;
}

/* The selector state hook of the tests: the hovered element and its ancestors are :hover. */
static embi_element *g_hovered;

static bool hover_state(void *user, embi_element *el, embi_sel_pc pc)
{
    (void)user;
    if (pc != EMBI_PC_HOVER) {
        return false;
    }
    for (const embi_node *n = g_hovered != nullptr ? &g_hovered->base : nullptr; n != nullptr; n = n->parent) {
        if (n == &el->base) {
            return true;
        }
    }
    return false;
}

/* Marks the :hover flips between the old and the new hovered element, as the input layer does. */
static void flip_hover(fx *f, embi_element *was, embi_element *now)
{
    embi_element *chain[2] = {was, now};
    for (int k = 0; k < 2; k++) {
        for (embi_node *n = chain[k] != nullptr ? &chain[k]->base : nullptr; n != nullptr; n = n->parent) {
            if (!embi_is_element(n)) {
                continue;
            }
            bool in_other = false;
            for (embi_node *m = chain[1 - k] != nullptr ? &chain[1 - k]->base : nullptr; m != nullptr; m = m->parent) {
                in_other = in_other || m == n;
            }
            if (!in_other) {
                EMB_CHECK(embi_style_mark_user_state(f->eng, embi_as_element(n), EMBI_PC_HOVER));
            }
        }
    }
    g_hovered = now;
}

/* A :hover flip restyles the elements whose state flipped and what the rules reading :hover reach from them, not the
 * document; the result equals a full restyle (the engine's verification, counting mode). */
EMB_TEST(hover_flip_restyles_what_the_rules_reach)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    char *page = row_page(40);
    fx f = make(page, "li:hover .x{color:red} li:hover{padding-top:3px} #box p:hover{color:blue}");
    embi_sel_hooks hooks = {nullptr, hover_state};
    embi_style_set_sel_hooks(f.eng, &hooks);
    g_hovered = nullptr;
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    flip_hover(&f, nullptr, by_id(&f, "by"));
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t d = total(&f) - t0;
    EMB_CHECK(d >= 2 && d <= 12); /* by and its ancestors, and the .x spans; not the 40 paragraphs */
    EMB_CHECK(embi_style_get(f.eng, by_id(&f, "bx"), EMBI_PE_NONE) != embi_style_get(f.eng, by_id(&f, "ax"), EMBI_PE_NONE));
    t0 = total(&f);
    flip_hover(&f, by_id(&f, "by"), by_id(&f, "ay"));
    embi_style_resolve_document(f.eng, f.doc);
    d = total(&f) - t0;
    EMB_CHECK(d >= 2 && d <= 12);
    flip_hover(&f, by_id(&f, "ay"), nullptr);
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(!embi_style_mark_user_state(f.eng, by_id(&f, "ay"), EMBI_PC_CHECKED)); /* not a user-action state */
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    destroy(&f);
    SDL_free(page);
    g_hovered = nullptr;
    embi_incr_set_verify(0);
}

/* A class change of an ancestor restyles the descendants that a rule reaching them can select (here the .item
 * paragraphs), not every descendant. */
EMB_TEST(descendant_rules_restyle_only_the_elements_they_can_select)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    char *page = row_page(60);
    fx f = make(page, ".on .item{color:red} .on > ul li{padding-top:1px}");
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t t0 = total(&f);
    set_attr(&f, by_id(&f, "top"), "class", "on");
    embi_style_resolve_document(f.eng, f.doc);
    uint64_t d = total(&f) - t0;
    EMB_CHECK(d >= 30 && d <= 40); /* top, the 30 .item paragraphs, the two li (and what their change reaches) */
    set_attr(&f, by_id(&f, "top"), "class", "");
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    destroy(&f);
    SDL_free(page);
    embi_incr_set_verify(0);
}

/* A splitter drag sets custom properties on the root element. An element that reads none of the changed names takes its
 * parent's new custom properties without a cascade; the ones that read them, directly or through a custom property defined
 * from them (--x below), are restyled; a full resolve gives the same styles (the verification of the engine). */
static const char *k_cpage =
    "<html><body><div id=shell class=shell><div id=left class=panel><p id=p1>a</p><p id=p2 class=uses>b</p></div>"
    "<div id=right class=panel><div id=inner class=derive><span id=s1 class=usesx>c</span><span id=s2>d</span></div>"
    "<ul id=list><li>one</li><li class=uses>two</li><li>three</li></ul></div></div></body></html>";
static const char *k_ccss =
    ":root{--lw:100px;--rw:50px;--c:red} .shell{display:grid;grid-template-columns:var(--lw) 1fr var(--rw)} "
    ".panel{color:var(--c)} .uses{width:var(--lw)} .derive{--x:calc(var(--rw) + 1px)} .usesx{margin-left:var(--x)} "
    "p{padding:1px} li + li{margin-top:2px} .hot{--c:blue} li::before{content:'-';margin-left:var(--rw)}";

static uint32_t drag_resolve(fx *f)
{
    embi_style_resolve_document(f->eng, f->doc);
    embi_style_restyle_stats st;
    embi_style_restyle_stats_get(f->eng, f->doc, &st);
    embi_style_delta d;
    embi_style_take_delta(f->eng, f->doc, &d);
    embi_style_release_grave(f->eng, f->doc);
    return st.last_custom_only;
}

EMB_TEST(custom_property_drag_restyles_only_the_readers)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fx f = make(k_cpage, k_ccss);
    embi_style_track(f.eng, f.doc, true);
    (void)drag_resolve(&f);
    embi_element *root = embi_document_element(f.doc);
    char st[64];
    uint32_t custom_only = 0;
    for (int i = 0; i < 12; i++) {
        SDL_snprintf(st, sizeof st, "--lw:%dpx;--rw:%dpx", 100 + i, i % 2 == 0 ? 50 : 60);
        set_attr(&f, root, "style", st);
        custom_only += drag_resolve(&f);
    }
    EMB_CHECK(custom_only >= 12u * 5u); /* body, the panels, the paragraph and the items that read nothing, every step */
    set_attr(&f, by_id(&f, "right"), "class", "panel hot"); /* a name the panel's children read: they are restyled too */
    (void)drag_resolve(&f);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    destroy(&f);
    embi_incr_set_verify(0);
}

/* Random custom property and class changes on random elements, each resolve checked against a full one. */
EMB_TEST(fuzz_custom_property_changes_equal_full)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fx f = make(k_cpage, k_ccss);
    embi_style_track(f.eng, f.doc, true);
    (void)drag_resolve(&f);
    static const char *styles[] = {"", "--lw:3px", "--rw:7px", "--c:blue", "--x:2px", "--lw:9px;--c:green",
                                   "--rw:var(--lw)", "--x:var(--c)", "width:var(--rw)", "--lw:4px;--rw:5px"};
    static const char *classes[] = {"", "uses", "usesx", "derive", "panel", "shell", "hot", "panel hot", "derive usesx"};
    rng_state = 4242;
    uint32_t custom_only = 0;
    for (int it = 0; it < 800; it++) {
        embi_element *els[64];
        uint32_t n = 0;
        collect(&embi_document_element(f.doc)->base, els, &n, 64);
        embi_element *el = rnd(3) == 0 ? embi_document_element(f.doc) : els[rnd(n)];
        if (rnd(3) == 0) {
            set_attr(&f, el, "class", classes[rnd(sizeof classes / sizeof classes[0])]);
        } else {
            set_attr(&f, el, "style", styles[rnd(sizeof styles / sizeof styles[0])]);
        }
        custom_only += drag_resolve(&f);
    }
    EMB_CHECK(custom_only > 0);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    destroy(&f);
    embi_incr_set_verify(0);
}

/* A restyle that computes the same custom properties again gives the same custom property map, so a child that only
 * inherits keeps its style object. */
EMB_TEST(equal_custom_properties_keep_the_style_objects)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fx f = make("<html><body><div id=c class=c><p id=t class=t>x</p></div></body></html>",
                ".c{--k:red;--pad:2px} .c .t{color:var(--k)} .flag{border-top:2px solid}");
    embi_style_resolve_document(f.eng, f.doc);
    const void *ts = by_id(&f, "t")->style;
    set_attr(&f, by_id(&f, "c"), "class", "c flag"); /* the parent's own style changes, its custom properties do not */
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(by_id(&f, "c")->style != nullptr);
    EMB_CHECK(by_id(&f, "t")->style == ts);
    EMB_CHECK(embi_incr_verify_failures() == base_fail);
    destroy(&f);
    embi_incr_set_verify(0);
}

/* Setting an attribute to the value it has changes nothing: no restyle, no origin for layout. */
EMB_TEST(same_value_attribute_set_restyles_nothing)
{
    fx f = make(k_page, k_css);
    embi_style_track(f.eng, f.doc, true);
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_delta d;
    embi_style_take_delta(f.eng, f.doc, &d);
    embi_style_release_grave(f.eng, f.doc);
    uint64_t t0 = total(&f);
    set_attr(&f, by_id(&f, "c"), "class", "x");
    set_attr(&f, by_id(&f, "c"), "id", "c");
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) == t0);
    embi_style_take_delta(f.eng, f.doc, &d);
    EMB_CHECK(!d.lost && d.nchanges == 0 && d.norigins == 0);
    embi_style_release_grave(f.eng, f.doc);
    set_attr(&f, by_id(&f, "c"), "class", "hot"); /* a real change still restyles */
    embi_style_resolve_document(f.eng, f.doc);
    EMB_CHECK(total(&f) > t0);
    destroy(&f);
}

/* A ::before whose color changes is a paint change of its element (its boxes keep their place); one whose content
 * changes is a layout change. */
EMB_TEST(pseudo_element_changes_are_classified)
{
    embi_incr_set_verify(2);
    fx f = make("<html><body><p id=p class=t>x</p></body></html>",
                ".t::before{content:'>';color:red} .t.on::before{color:blue} .t.wide::before{content:'>>'}");
    embi_style_track(f.eng, f.doc, true);
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_delta d;
    embi_style_take_delta(f.eng, f.doc, &d);
    embi_style_release_grave(f.eng, f.doc);
    embi_element *p = by_id(&f, "p");
    set_attr(&f, p, "class", "t on");
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    const embi_style_change *c = find_change(&d, p);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->pseudo && c->cls == EMBI_SCH_PAINT);
    embi_style_release_grave(f.eng, f.doc);
    set_attr(&f, p, "class", "t wide");
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    c = find_change(&d, p);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->pseudo && c->cls == EMBI_SCH_LAYOUT);
    embi_style_release_grave(f.eng, f.doc);
    set_attr(&f, p, "class", "");
    embi_style_resolve_document(f.eng, f.doc);
    embi_style_take_delta(f.eng, f.doc, &d);
    c = find_change(&d, p);
    EMB_REQUIRE(c != nullptr);
    EMB_CHECK(c->pseudo && c->cls == EMBI_SCH_LAYOUT); /* the ::before goes away */
    embi_style_release_grave(f.eng, f.doc);
    EMB_CHECK(embi_incr_verify_failures() == 0);
    destroy(&f);
    embi_incr_set_verify(0);
}

/* :nth-child(An+B of S) and :nth-last-child with a user-action pseudo-class in S, at any depth of S: a hover flip on one
 * li changes which sibling matches, also a sibling whose own state did not flip and the descendants of the siblings. */
EMB_TEST(nth_of_a_user_state_restyles_the_siblings)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fx f = make("<html><body><ul id=u><li id=a><b id=ab>a</b></li><li id=b><b id=bb>b</b></li><li id=c><b id=cb>c</b></li>"
                "</ul></body></html>",
                "li:nth-child(1 of :hover){color:red} li:nth-child(2 of :not(:hover)){font-weight:bold}"
                " li:nth-last-child(1 of :is(.z, :not(:is(:hover)))) b{text-decoration:underline}");
    embi_sel_hooks hooks = {nullptr, hover_state};
    embi_style_set_sel_hooks(f.eng, &hooks);
    g_hovered = nullptr;
    embi_style_resolve_document(f.eng, f.doc);
    const char *ids[] = {"a", "b", "a", "c", nullptr};
    embi_element *was = nullptr;
    for (int i = 0; i < 5; i++) {
        embi_element *now = ids[i] != nullptr ? by_id(&f, ids[i]) : nullptr;
        flip_hover(&f, was, now);
        was = now;
        embi_style_resolve_document(f.eng, f.doc);
        EMB_CHECK(embi_incr_verify_failures() == base_fail);
    }
    destroy(&f);
    g_hovered = nullptr;
    embi_incr_set_verify(0);
}

/* ---- CF1 descendant feature filters: an ancestor's class, id or attribute change restyles only the descendants whose
 * id, class or tag a rule reaching them names; each case below would leave a stale style (a verification failure)
 * if the filter skipped an element it must restyle ---- */

typedef struct mut {
    const char *id, *attr, *value;
} mut;

static void filter_case(const char *page, const char *css, const mut *muts)
{
    embi_incr_set_verify(2);
    uint32_t base_fail = embi_incr_verify_failures();
    fx f = make(page, css);
    embi_style_resolve_document(f.eng, f.doc);
    for (const mut *m = muts; m->id != nullptr; m++) {
        set_attr(&f, by_id(&f, m->id), m->attr, m->value);
        embi_style_resolve_document(f.eng, f.doc);
        if (embi_incr_verify_failures() != base_fail) {
            SDL_Log("stale style after %s.%s=\"%s\" under \"%s\"", m->id, m->attr, m->value, css);
            EMB_CHECK(embi_incr_verify_failures() == base_fail);
            break;
        }
    }
    destroy(&f);
    embi_incr_set_verify(0);
}

static const char *k_fpage =
    "<!doctype html><html><body><div id=s1></div><div id=r><p id=pb class=b>b<span id=sb class=c>c</span></p>"
    "<p id=pc class=c>c</p><p id=pn>n<span id=sn>s</span><i id=in class=y>i</i></p>"
    "<ul id=u><li id=l1>1</li><li id=l2 class=c>2</li><li id=l3 class=c>3</li><li id=l4>4</li></ul></div>"
    "<div id=s2 class=b><p id=q><span id=qc class=c>c</span></p></div></body></html>";

static const mut k_toggle_r[] = {{"r", "class", "a"}, {"r", "class", "a b"}, {"r", "class", "b"}, {"r", "class", "a"},
                                 {"r", "class", ""}, {nullptr, nullptr, nullptr}};

EMB_TEST(feature_filter_is_and_not_subjects)
{
    filter_case(k_fpage, ".a :is(.b,.c){color:red}", k_toggle_r);
    filter_case(k_fpage, ".a :is(.b,#sn){color:red}", k_toggle_r);
    filter_case(k_fpage, ".a :not(.b){color:red}", k_toggle_r);
    filter_case(k_fpage, ".a span:not(.c){color:red}", k_toggle_r);
    filter_case(k_fpage, ".a:not(.b) .c{color:red} .a:not(.b) i{color:blue}", k_toggle_r);
    filter_case(k_fpage, ":not(.a) .c{color:red}", k_toggle_r);
}

EMB_TEST(feature_filter_sibling_then_descendant)
{
    static const mut m[] = {{"s1", "class", "a"}, {"r", "class", "b"}, {"s1", "class", ""}, {"s1", "class", "a"},
                            {"r", "class", ""}, {"s1", "class", ""}, {nullptr, nullptr, nullptr}};
    filter_case(k_fpage, ".a + .b .c{color:red} .a ~ .b span{color:blue}", m);
    filter_case(k_fpage, ".a + .b .c{color:red}", k_toggle_r);
}

EMB_TEST(feature_filter_attribute_and_id_ancestors)
{
    static const mut m[] = {{"r", "data-x", ""}, {"r", "data-x", "1"}, {"u", "data-x", "1"}, {"r", "id", "r2"},
                            {nullptr, nullptr, nullptr}};
    filter_case(k_fpage, "[data-x] .y{color:red} [data-x=\"1\"] li{color:blue} #r2 .c{color:green}", m);
    static const mut m2[] = {{"r", "id", "zz"}, {"zz", "id", "r"}, {nullptr, nullptr, nullptr}};
    filter_case(k_fpage, "#zz span{color:red} #r .c{color:blue}", m2);
}

EMB_TEST(feature_filter_nested_rules)
{
    filter_case(k_fpage, ".a { & .c{color:red} & > p{color:blue} }", k_toggle_r);
    filter_case(k_fpage, ".c { .a & {color:red} }", k_toggle_r);
}

EMB_TEST(feature_filter_nth_of_and_type_only_subjects)
{
    static const mut m[] = {{"r", "class", "a"}, {"l1", "class", "c"}, {"l2", "class", ""}, {"l4", "class", "c"},
                            {"r", "class", ""}, {"l3", "class", ""}, {"r", "class", "a"}, {nullptr, nullptr, nullptr}};
    filter_case(k_fpage, ".a li:nth-child(2 of .c){color:red} .a :nth-last-child(1 of .c){color:blue}", m);
    filter_case(k_fpage, ".a span{color:red} .a > *{padding-top:1px} .a li + li{color:blue}", k_toggle_r);
}

/* More rules than the filter holds: past its cap it restyles every descendant, which must still be right. */
EMB_TEST(feature_filter_past_its_cap)
{
    filter_case(k_fpage,
                ".a .f1{color:red} .a .f2{color:red} .a .f3{color:red} .a .f4{color:red} .a .f5{color:red}"
                " .a .f6{color:red} .a .f7{color:red} .a .c{color:blue}",
                k_toggle_r);
}

/* Quirks mode matches class selectors ASCII case-insensitively, so a filter that compared classes exactly would skip
 * an element the rule selects; in no-quirks mode the same rules must not match, and the result is still compared. */
EMB_TEST(feature_filter_quirks_mode_class_case)
{
    static const char *quirks =
        "<html><body><div id=r><p id=p class=Item>x<span id=s class=iTEM>y</span></p><b id=b class=item>b</b></div>"
        "</body></html>";
    static const mut m[] = {{"r", "class", "On"}, {"r", "class", ""}, {"r", "class", "ON"}, {"r", "class", "on"},
                            {nullptr, nullptr, nullptr}};
    filter_case(quirks, ".on .ITEM{color:red} .ON .item{padding-top:1px}", m);
    filter_case(quirks, ".On .Item{color:red}", m);
    static const char *noquirks =
        "<!doctype html><html><body><div id=r><p id=p class=Item>x<span id=s class=iTEM>y</span></p>"
        "<b id=b class=item>b</b></div></body></html>";
    filter_case(noquirks, ".on .ITEM{color:red} .ON .item{padding-top:1px}", m);
}

EMB_TEST_MAIN()
