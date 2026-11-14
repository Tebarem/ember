/*
 * Engine-level checks: style sharing through interning and the sharing cache, custom properties,
 * generated pseudo elements, lifetime (live counts return to the baseline), removal, deep trees,
 * user sheets and environment changes.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "css/selector.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <string.h>

static embi_document *parse_doc(const char *html)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return doc;
}

static embi_css_sheet *parse_css(const char *css)
{
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    return embi_css_sheet_parse((embi_str){css, (uint32_t)strlen(css)}, &opts);
}

static embi_element *by_id(embi_document *doc, const char *id)
{
    embi_element *el = embi_get_element_by_id(doc, (embi_str){id, (uint32_t)strlen(id)});
    EMB_CHECK(el != nullptr);
    return el;
}

/* The serialized computed value of a property of an element, or "" when it has none. */
static bool value_of(embi_style_engine *eng, embi_element *el, const char *prop, char *out,
                     size_t cap)
{
    out[0] = '\0';
    if (el == nullptr) {
        return false;
    }
    const embi_style *s = embi_style_get(eng, el, EMBI_PE_NONE);
    embi_prop_id pid = embi_prop_lookup((embi_str){prop, (uint32_t)strlen(prop)});
    embi_sb sb = {nullptr, 0, 0};
    bool ok = s != nullptr && pid != EMBI_PROP_NONE && embi_style_value_text(s, pid, &sb);
    size_t n = ok ? sb.len : 0;
    if (n >= cap) {
        n = cap - 1;
    }
    if (ok) {
        memcpy(out, sb.data, n);
    }
    out[n] = '\0';
    embi_sb_free(&sb);
    return ok;
}

/* One create / attach / resolve / release cycle. Returns nothing: the caller checks the counts. */
static void lifetime_cycle(void)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_document *doc = parse_doc("<div id=a class=x><p id=b>t</p></div><span id=c></span>");
    embi_css_sheet *author = parse_css(".x { color: red; } p::before { content: \"q\"; }");
    embi_css_sheet *user = parse_css("span { color: blue; }");
    embi_css_sheet *authors[2] = {author, author}; /* the same sheet twice */
    embi_css_sheet *users[1] = {user};
    embi_style_engine_set_user_sheets(eng, users, 1);
    embi_document_set_author_sheets(eng, doc, authors, 2);
    embi_style_resolve_document(eng, doc);
    embi_document_set_author_sheets(eng, doc, authors, 2);
    embi_style_resolve_document(eng, doc);
    /* Empty sheet lists allocate and release symmetrically. */
    embi_style_engine_set_user_sheets(eng, users, 0);
    embi_document_set_author_sheets(eng, doc, authors, 0);
    embi_style_resolve_document(eng, doc);
    embi_document_set_author_sheets(eng, doc, authors, 2);
    embi_style_resolve_document(eng, doc);
    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(author);
    embi_css_sheet_unref(user);
    embi_style_engine_destroy(eng);
}

EMB_TEST(identical_siblings_share_one_style)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc(
        "<div id=a></div><div id=b></div><div id=c class=x></div>");
    embi_css_sheet *sheet = parse_css(".x { color: red; }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);

    const embi_style *a = embi_style_get(eng, by_id(doc, "a"), EMBI_PE_NONE);
    const embi_style *b = embi_style_get(eng, by_id(doc, "b"), EMBI_PE_NONE);
    const embi_style *c = embi_style_get(eng, by_id(doc, "c"), EMBI_PE_NONE);
    EMB_REQUIRE(a != nullptr && b != nullptr && c != nullptr);
    EMB_CHECK(a == b);
    EMB_CHECK(a != c);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

EMB_TEST(custom_property_text_is_kept_verbatim)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<div id=a></div>");
    embi_css_sheet *sheet = parse_css(":root { --brand: 1px solid red; }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);

    const embi_style *s = embi_style_get(eng, by_id(doc, "a"), EMBI_PE_NONE);
    EMB_REQUIRE(s != nullptr);
    embi_sb sb = {nullptr, 0, 0};
    embi_atom name = embi_atom_intern("--brand", 7);
    EMB_CHECK(embi_style_custom_text(s, name, &sb));
    EMB_CHECK_EQ_INT(sb.len, 13);
    EMB_CHECK(memcmp(sb.data, "1px solid red", 13) == 0);
    embi_sb_free(&sb);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

EMB_TEST(generated_before_pseudo_has_its_own_style)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<p id=a>x</p>");
    embi_css_sheet *sheet = parse_css("#a::before { content: \"q\"; color: rgb(0, 0, 255); }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);

    embi_element *el = by_id(doc, "a");
    const embi_style *before = embi_style_get(eng, el, EMBI_PE_BEFORE);
    const embi_style *plain = embi_style_get(eng, el, EMBI_PE_NONE);
    EMB_REQUIRE(before != nullptr && plain != nullptr);
    EMB_CHECK(before != plain);
    embi_sb sb = {nullptr, 0, 0};
    embi_prop_id color = embi_prop_lookup(EMBI_STR("color"));
    EMB_CHECK(embi_style_value_text(before, color, &sb));
    EMB_CHECK_EQ_INT(sb.len, 14); /* "rgb(0, 0, 255)" */
    embi_sb_free(&sb);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

EMB_TEST(forget_document_releases_its_styles)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_style_stats base = {0};
    embi_style_engine_stats(eng, &base);

    embi_document *doc = parse_doc("<div id=a class=x></div><div id=b></div>");
    embi_css_sheet *sheet = parse_css(".x { color: red; }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);

    embi_style_stats before = {0};
    embi_style_engine_stats(eng, &before);
    EMB_CHECK(before.styles > base.styles);
    EMB_CHECK(before.bytes > 0);
    EMB_CHECK(embi_style_group_count() > 0);

    embi_style_forget_document(eng, doc);
    embi_style_stats after = {0};
    embi_style_engine_stats(eng, &after);
    EMB_CHECK(after.styles < before.styles);
    EMB_CHECK_EQ_INT(after.styles, base.styles);
    EMB_CHECK_EQ_INT(after.groups, base.groups);

    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

/* Create, attach, resolve, release: the live allocation count returns to its baseline. The first
 * cycle warms the process-wide tables (layout, atoms, the UA sheet's interned names). */
EMB_TEST(engine_lifetime_returns_to_baseline)
{
    lifetime_cycle();
    size_t base = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        lifetime_cycle();
    }
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)base);
}

/* A document that outlives its engine is detached by the destroy: its later mutations must not
 * reach the freed engine state. */
EMB_TEST(destroy_detaches_live_documents)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<div id=a></div><div id=b></div>");
    embi_css_sheet *sheet = parse_css("#a { color: red; }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    embi_style_engine_destroy(eng);

    embi_element *a = by_id(doc, "a");
    embi_element *b = by_id(doc, "b");
    EMB_CHECK(a->style == nullptr);
    EMB_CHECK(embi_remove_child(b->base.parent, &b->base) == EMBI_DOM_OK);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
}

/* Removing a subtree releases its styles even when no resolve follows (the reviewer's repro). */
EMB_TEST(removed_elements_release_their_styles)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_style_stats base = {0};
    embi_style_engine_stats(eng, &base);

    embi_document *doc = parse_doc("<div id=a><p id=b>x</p></div>");
    embi_css_sheet *sheet = parse_css("div { margin: 1px; } p { padding: 2px; }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    embi_element *a = by_id(doc, "a");
    embi_element *b = by_id(doc, "b");
    EMB_CHECK(embi_remove_child(&a->base, &b->base) == EMBI_DOM_OK);
    embi_style_resolve_document(eng, doc);
    EMB_CHECK(b->style == nullptr);
    embi_style_forget_document(eng, doc);

    embi_style_stats after = {0};
    embi_style_engine_stats(eng, &after);
    EMB_CHECK_EQ_INT(after.styles, base.styles);
    EMB_CHECK_EQ_INT(after.groups, base.groups);

    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

/* 20,000 nested divs: the walk and the ancestor chain are heap-bounded, so this neither overflows
 * the stack nor loses ancestors beyond a fixed depth. */
EMB_TEST(deep_trees_resolve_without_stack_growth)
{
    enum { DEPTH = 20000 };
    size_t cap = DEPTH * 16 + 64;
    char *html = embi_alloc(cap);
    size_t len = 0;
    for (uint32_t i = 0; i < DEPTH; i++) {
        memcpy(html + len, "<div>", 5);
        len += 5;
    }
    memcpy(html + len, "<span id=deep></span>", 21);
    len += 21;
    for (uint32_t i = 0; i < DEPTH; i++) {
        memcpy(html + len, "</div>", 6);
        len += 6;
    }
    html[len] = '\0';
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc(html);
    embi_free(html, cap);
    embi_css_sheet *sheet = parse_css("div > span { color: rgb(1, 2, 3); }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    /* The innermost span is the last child at every level (the parser builds one chain). */
    embi_node *n = &doc->base;
    while (n->last_child != nullptr) {
        n = n->last_child;
    }
    EMB_REQUIRE(n->type == EMBI_NODE_ELEMENT);
    embi_element *deep = (embi_element *)n;
    char text[64];
    EMB_CHECK(value_of(eng, deep, "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(1, 2, 3)") == 0);
    EMB_CHECK(embi_style_get(eng, deep, EMBI_PE_BEFORE) == nullptr);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

/* User-origin sheets: a normal user declaration loses to an author declaration, and an important
 * user declaration wins over an important author declaration. */
EMB_TEST(user_origin_sheets_follow_the_cascade)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<p id=a></p><p id=b></p>");
    embi_css_sheet *user = parse_css("#a { color: rgb(1, 1, 1); } #b { color: rgb(2, 2, 2) !important; }");
    embi_css_sheet *author = parse_css("#a { color: rgb(3, 3, 3); } #b { color: rgb(4, 4, 4) !important; }");
    embi_css_sheet *users[1] = {user};
    embi_css_sheet *authors[1] = {author};
    embi_style_engine_set_user_sheets(eng, users, 1);
    embi_document_set_author_sheets(eng, doc, authors, 1);
    embi_style_resolve_document(eng, doc);

    char text[64];
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(3, 3, 3)") == 0);
    EMB_CHECK(value_of(eng, by_id(doc, "b"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(2, 2, 2)") == 0);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(user);
    embi_css_sheet_unref(author);
    embi_style_engine_destroy(eng);
}

/* revert in the author origin rolls back to the user origin, normal and important. */
EMB_TEST(revert_rolls_back_to_the_user_origin)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<p id=a></p><p id=b></p><div id=c></div>");
    embi_css_sheet *user = parse_css("#a { color: rgb(1, 1, 1); } #b { color: rgb(2, 2, 2); }");
    embi_css_sheet *author = parse_css(
        "#a { color: rgb(9, 9, 9); } #a { color: revert; }"
        "#b { color: rgb(9, 9, 9) !important; } #b { color: revert !important; }"
        "#c { display: inline; } #c { display: revert; }");
    embi_css_sheet *users[1] = {user};
    embi_css_sheet *authors[1] = {author};
    embi_style_engine_set_user_sheets(eng, users, 1);
    embi_document_set_author_sheets(eng, doc, authors, 1);
    embi_style_resolve_document(eng, doc);

    char text[64];
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(1, 1, 1)") == 0);
    EMB_CHECK(value_of(eng, by_id(doc, "b"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(2, 2, 2)") == 0);
    EMB_CHECK(value_of(eng, by_id(doc, "c"), "display", text, sizeof text));
    EMB_CHECK(strcmp(text, "block") == 0); /* no user declaration: rolls back to the UA */

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(user);
    embi_css_sheet_unref(author);
    embi_style_engine_destroy(eng);
}

/* A media environment change re-evaluates @media rules on the next resolve. */
EMB_TEST(environment_change_reevaluates_media)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<p id=a></p>");
    embi_css_sheet *sheet = parse_css(
        "#a { color: rgb(9, 9, 9); } @media (min-width: 1000px) { #a { color: rgb(1, 2, 3); } }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    char text[64];
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(9, 9, 9)") == 0); /* default width 800px */

    embi_style_env env;
    memset(&env, 0, sizeof env);
    env.width = 1200;
    env.height = 800;
    env.dppx = 1;
    env.scripting = true;
    env.hover = true;
    env.pointer = 2;
    embi_style_engine_set_env(eng, &env);
    embi_style_resolve_document(eng, doc);
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(1, 2, 3)") == 0);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

/* Presentational hints are author-level with zero specificity: an author rule wins over them. */
EMB_TEST(presentational_hints_lose_to_author_rules)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<table><tr><td id=a bgcolor=\"#ff0000\"></td><td id=b bgcolor=\"#ff0000\"></td></tr></table>");
    embi_css_sheet *sheet = parse_css("#b { background-color: rgb(0, 0, 255); }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    char text[64];
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "background-color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(255, 0, 0)") == 0);
    EMB_CHECK(value_of(eng, by_id(doc, "b"), "background-color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(0, 0, 255)") == 0);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

EMB_TEST(sharing_cache_hits_for_identical_siblings_only)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc(
        "<div id=w><p class=k>1</p><p class=k>2</p><p class=k>3</p><p class=k>4</p></div>");
    embi_css_sheet *sheet = parse_css(".k { color: rgb(1, 2, 3); }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_stats s0 = {0};
    embi_style_engine_stats(eng, &s0);
    embi_style_resolve_document(eng, doc);
    embi_style_stats s1 = {0};
    embi_style_engine_stats(eng, &s1);
    EMB_CHECK(s1.share_hits - s0.share_hits >= 3);
    embi_style_forget_document(eng, doc);
    embi_document_free(doc);

    /* A positional selector disables sharing: the identical siblings differ, and none is reused. */
    embi_document *doc2 = parse_doc(
        "<div id=w><p class=k>1</p><p class=k>2</p><p class=k>3</p><p class=k>4</p></div>");
    embi_css_sheet *sheet2 = parse_css(".k:first-child { color: rgb(1, 2, 3); } .k:nth-child(3) { width: 5px; }");
    embi_css_sheet *sheets2[1] = {sheet2};
    embi_document_set_author_sheets(eng, doc2, sheets2, 1);
    embi_style_stats s2 = {0};
    embi_style_engine_stats(eng, &s2);
    embi_style_resolve_document(eng, doc2);
    embi_style_stats s3 = {0};
    embi_style_engine_stats(eng, &s3);
    EMB_CHECK_EQ_INT((long long)(s3.share_hits - s2.share_hits), 0);
    embi_node *w = &by_id(doc2, "w")->base;
    char text[64];
    int idx = 0;
    for (embi_node *c = w->first_child; c != nullptr; c = c->next, idx++) {
        EMB_REQUIRE(c->type == EMBI_NODE_ELEMENT);
        value_of(eng, (embi_element *)c, "color", text, sizeof text);
        EMB_CHECK((strcmp(text, "rgb(1, 2, 3)") == 0) == (idx == 0));
        value_of(eng, (embi_element *)c, "width", text, sizeof text);
        EMB_CHECK((strcmp(text, "5px") == 0) == (idx == 2));
    }
    embi_style_forget_document(eng, doc2);
    embi_document_free(doc2);
    embi_css_sheet_unref(sheet);
    embi_css_sheet_unref(sheet2);
    embi_style_engine_destroy(eng);
}

/* A window-resize drag calls set_env many times: re-flattening reuses the compiled rules, so the
 * engine arena stays flat (and an unchanged environment does no work at all). */
EMB_TEST(set_env_does_not_grow_the_arena)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<div id=a class=x><p id=b>t</p></div>");
    embi_css_sheet *sheet = parse_css(
        ".x { color: rgb(9, 9, 9); margin: 1px 2px; } .x p { padding: 3px; }"
        " @media (min-width: 500px) { .x { color: rgb(1, 2, 3); } .x { & p { width: 3px; } } }"
        " @layer a.b { .x { height: 4px; } }");
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    embi_style_env env;
    memset(&env, 0, sizeof env);
    env.height = 800;
    env.dppx = 1;
    env.scripting = true;
    env.hover = true;
    env.pointer = 2;
    env.width = 400;
    embi_style_engine_set_env(eng, &env);
    env.width = 900;
    embi_style_engine_set_env(eng, &env);
    embi_style_resolve_document(eng, doc);
    embi_style_stats base = {0};
    embi_style_engine_stats(eng, &base);
    for (int i = 0; i < 100; i++) {
        env.width = (i & 1) ? 400 : 900;
        embi_style_engine_set_env(eng, &env);
        embi_style_resolve_document(eng, doc);
    }
    embi_style_stats after = {0};
    embi_style_engine_stats(eng, &after);
    EMB_CHECK_EQ_INT((long long)after.arena_bytes, (long long)base.arena_bytes);
    char text[64];
    env.width = 900;
    embi_style_engine_set_env(eng, &env);
    embi_style_resolve_document(eng, doc);
    EMB_CHECK(value_of(eng, by_id(doc, "a"), "color", text, sizeof text));
    EMB_CHECK(strcmp(text, "rgb(1, 2, 3)") == 0);
    EMB_CHECK(value_of(eng, by_id(doc, "b"), "width", text, sizeof text));
    EMB_CHECK(strcmp(text, "3px") == 0);
    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

static bool import_fetch(void *user, embi_str base, embi_str url, embi_str *resolved,
                         const unsigned char **bytes, size_t *n)
{
    (void)user;
    (void)base;
    (void)url;
    (void)resolved;
    static const char css[] = "#g { width: 7px; }";
    *bytes = (const unsigned char *)css;
    *n = sizeof css - 1;
    return true;
}

/* @import ... layer(a.b) puts the imported rules in sublayer b of layer a, below a's own rules. */
EMB_TEST(import_layer_with_dotted_name)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<div id=g></div>");
    embi_css_loader ld = {nullptr, import_fetch};
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {&ld, &hooks, EMBI_STR("")};
    const char *text = "@layer a; @import url(\"x.css\") layer(a.b); @layer a { #g { width: 22px; } }";
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){text, (uint32_t)strlen(text)}, &opts);
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    char buf[64];
    EMB_CHECK(value_of(eng, by_id(doc, "g"), "width", buf, sizeof buf));
    EMB_CHECK(strcmp(buf, "22px") == 0);
    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}


static bool import_fetch_late(void *user, embi_str base, embi_str url, embi_str *resolved,
                              const unsigned char **bytes, size_t *n)
{
    (void)user;
    (void)base;
    (void)url;
    (void)resolved;
    static const char css[] =
        "                                                                                "
        "#g { width: 7px; }";
    *bytes = (const unsigned char *)css;
    *n = sizeof css - 1;
    return true;
}

/* A rule after an @import wins over an imported rule of equal specificity, even when the imported
 * rule has the larger source offset. */
EMB_TEST(rules_after_import_beat_imported_rules)
{
    embi_style_engine *eng = embi_style_engine_create(nullptr, nullptr, nullptr);
    EMB_REQUIRE(eng != nullptr);
    embi_document *doc = parse_doc("<div id=g></div>");
    embi_css_loader ld = {nullptr, import_fetch_late};
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {&ld, &hooks, EMBI_STR("")};
    const char *text = "@import url(\"x.css\"); #g { width: 22px; }";
    embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){text, (uint32_t)strlen(text)}, &opts);
    embi_css_sheet *sheets[1] = {sheet};
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    char buf[64];
    EMB_CHECK(value_of(eng, by_id(doc, "g"), "width", buf, sizeof buf));
    EMB_CHECK(strcmp(buf, "22px") == 0);
    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
}

EMB_TEST_MAIN()
