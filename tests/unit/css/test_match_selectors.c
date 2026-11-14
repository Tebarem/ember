/*
 * Selector matching (W2b) against the fixture tree: type, id, class, attribute, combinator,
 * structural, :is/:where/:not, :has, :lang, :dir, :scope, namespaces and pseudo-elements.
 */
#include "emb_test.h"

#include "match_fixture.h"

#include <stdio.h>

EMB_TEST(match_types_ids_classes)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.div, "div"));
    EMB_CHECK(mf_matches(t.div, "#d1"));
    EMB_CHECK(mf_matches(t.div, ".x"));
    EMB_CHECK(!mf_matches(t.div, ".y"));
    EMB_CHECK(mf_matches(t.p, "div p"));
    EMB_CHECK(mf_matches(t.p, "html > body > div > p"));
    EMB_CHECK(!mf_matches(t.p, "body > p"));
    EMB_CHECK(mf_matches(t.li2, "li.sel"));
    EMB_CHECK(mf_matches(t.li2, "ul > .sel"));
    EMB_CHECK(mf_matches(t.body, "#main.a.b"));
    EMB_CHECK(!mf_matches(t.body, "#main.a.c"));
    /* HTML element type names are ASCII case-insensitive. */
    EMB_CHECK(mf_matches(t.div, "DIV"));
    EMB_CHECK(mf_matches(t.div, "*"));
    EMB_CHECK(mf_matches(t.ul, "div + ul"));
    EMB_CHECK(mf_matches(t.ul, "div ~ ul"));
    EMB_CHECK(!mf_matches(t.div, "ul + div"));
    EMB_CHECK(mf_matches(t.span, "p + span"));
    EMB_CHECK(mf_matches(t.span, "p ~ span"));
    EMB_CHECK(!mf_matches(t.span, "span ~ p"));
    mf_free(&t);
}

EMB_TEST(match_attributes)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.p, "[title]"));
    EMB_CHECK(mf_matches(t.p, "[ title ]"));
    EMB_CHECK(mf_matches(t.p, "[title=\"hello world\"]"));
    EMB_CHECK(mf_matches(t.p, "[title*=world]"));
    EMB_CHECK(mf_matches(t.p, "[title^=hell]"));
    EMB_CHECK(mf_matches(t.p, "[title$=world]"));
    EMB_CHECK(mf_matches(t.p, "[title~=world]"));
    EMB_CHECK(!mf_matches(t.p, "[title~=wor]"));
    EMB_CHECK(!mf_matches(t.p, "[title|=hello]"));
    EMB_CHECK(!mf_matches(t.p, "[title=hello]"));
    EMB_CHECK(!mf_matches(t.p, "[data-missing]"));
    /* The i flag makes the comparison ASCII case-insensitive, and s makes it sensitive. */
    EMB_CHECK(mf_matches(t.p, "[title=\"HELLO WORLD\" i]"));
    EMB_CHECK(!mf_matches(t.p, "[title=\"HELLO WORLD\"]"));
    EMB_CHECK(!mf_matches(t.p, "[title=\"HELLO WORLD\" s]"));
    /* The attribute name and the type value are case-insensitive on HTML elements. */
    EMB_CHECK(mf_matches(t.p, "[TITLE]"));
    EMB_CHECK(mf_matches(t.input, "[TYPE=CHECKBOX]"));
    /* Empty values never match the substring-style operators. */
    EMB_CHECK(!mf_matches(t.p, "[title^=\"\"]"));
    EMB_CHECK(!mf_matches(t.p, "[title*=\"\"]"));
    EMB_CHECK(!mf_matches(t.p, "[title~=\"\"]"));
    EMB_CHECK(mf_matches(t.input, "[checked]"));
    mf_free(&t);
}

EMB_TEST(match_structural_and_nth)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.li1, "li:first-child"));
    EMB_CHECK(mf_matches(t.li3, "li:last-child"));
    EMB_CHECK(!mf_matches(t.li2, "li:first-child"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-child(2)"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-child(even)"));
    EMB_CHECK(mf_matches(t.li1, "li:nth-child(odd)"));
    EMB_CHECK(mf_matches(t.li3, "li:nth-child(odd)"));
    EMB_CHECK(mf_matches(t.li3, "li:nth-last-child(1)"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-last-child(2)"));
    EMB_CHECK(mf_matches(t.li2, "ul > li:nth-child(2n)"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-child(-n+2)"));
    EMB_CHECK(!mf_matches(t.li3, "li:nth-child(-n+2)"));
    /* li2 is the only .sel sibling, so it is first among them. */
    EMB_CHECK(mf_matches(t.li2, "li:nth-child(1 of .sel)"));
    EMB_CHECK(!mf_matches(t.li2, "li:nth-child(2 of .sel)"));
    EMB_CHECK(!mf_matches(t.li1, "li:nth-child(1 of .sel)"));
    EMB_CHECK(mf_matches(t.li2, "li:not(:first-child):not(:last-child)"));
    EMB_CHECK(!mf_matches(t.li1, "li:not(:first-child):not(:last-child)"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-of-type(2)"));
    EMB_CHECK(!mf_matches(t.p, "p:only-child"));
    EMB_CHECK(mf_matches(t.span, "span:first-of-type"));
    EMB_CHECK(mf_matches(t.span, "span:last-of-type"));
    EMB_CHECK(mf_matches(t.span, "span:only-of-type"));
    /* p is the only p in its div, so it is both first and last of its type. */
    EMB_CHECK(mf_matches(t.p, "p:last-of-type"));
    EMB_CHECK(mf_matches(t.p, "p:only-of-type"));
    EMB_CHECK(mf_matches(t.i, ":empty"));
    EMB_CHECK(!mf_matches(t.p, ":empty"));
    EMB_CHECK(!mf_matches(t.div, ":empty"));
    EMB_CHECK(mf_matches(t.html, ":root"));
    EMB_CHECK(mf_matches(t.html, "html:root"));
    EMB_CHECK(!mf_matches(t.body, ":root"));
    mf_free(&t);
}

EMB_TEST(match_logical)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.p, "p:is(li, p)"));
    EMB_CHECK(!mf_matches(t.p, "p:is(li, ul)"));
    EMB_CHECK(mf_matches(t.li2, "li:where(.sel)"));
    EMB_CHECK(!mf_matches(t.li1, "li:where(.sel)"));
    EMB_CHECK(mf_matches(t.li1, "li:not(.sel)"));
    EMB_CHECK(!mf_matches(t.li2, "li:not(.sel)"));
    EMB_CHECK(mf_matches(t.li2, "li:is(:not(.x), .y)"));
    /* A forgiving :is() with no valid item matches nothing and keeps the rule valid. */
    EMB_CHECK(!mf_matches(t.li2, ":is(::before)"));
    mf_free(&t);
}

EMB_TEST(match_has)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.div, "div:has(> p)"));
    EMB_CHECK(!mf_matches(t.div, "div:has(> ul)"));
    EMB_CHECK(mf_matches(t.div, "div:has(p)"));
    EMB_CHECK(mf_matches(t.body, "body:has(li.sel)"));
    EMB_CHECK(mf_matches(t.ul, "ul:has(+ i)"));
    EMB_CHECK(mf_matches(t.ul, "ul:has(~ input)"));
    EMB_CHECK(!mf_matches(t.ul, "ul:has(+ input)"));
    EMB_CHECK(mf_matches(t.div, "div:has(+ ul li.sel)"));
    EMB_CHECK(mf_matches(t.div, "div:has(~ a)"));
    EMB_CHECK(!mf_matches(t.p, "p:has(span)"));
    EMB_CHECK(mf_matches(t.li1, "li:has(+ li.sel)"));
    EMB_CHECK(!mf_matches(t.a, "a:has(> *)"));
    EMB_CHECK(mf_matches(t.html, "html:has(rect)"));
    EMB_CHECK(mf_matches(t.body, "body:has(> .y)") == false);
    EMB_CHECK(mf_matches(t.div, "div:has(.y, .sel)"));
    mf_free(&t);
}

EMB_TEST(match_lang_and_dir)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.body, "body:lang(en)"));
    EMB_CHECK(mf_matches(t.p, "p:lang(en-us)"));
    EMB_CHECK(mf_matches(t.p, "p:lang(en-US)"));
    EMB_CHECK(mf_matches(t.p, "p:lang(\"*\")"));
    /* RFC 4647 extended filtering: wildcard subtags. */
    EMB_CHECK(mf_matches(t.p, "p:lang(\"*-US\")"));
    EMB_CHECK(mf_matches(t.body, "body:lang(\"en-*\")"));
    EMB_CHECK(!mf_matches(t.span, "span:lang(\"*-US\")"));
    EMB_CHECK(mf_matches(t.span, "span:lang(\"*-CA\")"));
    EMB_CHECK(!mf_matches(t.span, "span:lang(\"en-*\")"));
    EMB_CHECK(mf_matches(t.span, "span:lang(fr)"));
    EMB_CHECK(!mf_matches(t.span, "span:lang(en)"));
    EMB_CHECK(!mf_matches(t.ul, "ul:lang(fr)"));
    EMB_CHECK(mf_matches(t.li1, "li:lang(en)"));
    EMB_CHECK(mf_matches(t.body, "body:dir(rtl)"));
    EMB_CHECK(mf_matches(t.p, "p:dir(rtl)"));
    EMB_CHECK(mf_matches(t.span, "span:dir(rtl)"));
    EMB_CHECK(mf_matches(t.html, "html:dir(ltr)"));
    EMB_CHECK(!mf_matches(t.html, "html:dir(rtl)"));

    /* dir=auto and bdi take the direction of the first strong character. The elements are left
     * detached, which the direction walk does not need. */
    embi_element *bdi = mf_el(t.doc, "bdi");
    mf_text(&bdi->base, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
    EMB_CHECK(mf_matches(bdi, "bdi:dir(rtl)"));
    embi_element *auto_ltr = mf_el(t.doc, "div");
    mf_attr(auto_ltr, "dir", "auto");
    mf_text(&auto_ltr->base, "1 abc");
    EMB_CHECK(mf_matches(auto_ltr, "div:dir(ltr)"));
    embi_element *auto_rtl = mf_el(t.doc, "div");
    mf_attr(auto_rtl, "dir", "AUTO");
    mf_text(&auto_rtl->base, "123 \xD7\xA9");
    EMB_CHECK(mf_matches(auto_rtl, "div:dir(rtl)"));
    mf_free(&t);
}

EMB_TEST(match_scope)
{
    mf_tree t = mf_build();
    /* With no scope element, :scope is the document element. */
    EMB_CHECK(mf_matches(t.html, ":scope"));
    EMB_CHECK(!mf_matches(t.body, ":scope"));
    /* With a scope element, :scope is that element. */
    EMB_CHECK(mf_matches_in(t.div, ":scope", nullptr, t.div, nullptr));
    EMB_CHECK(mf_matches_in(t.p, ":scope > p", nullptr, t.div, nullptr));
    EMB_CHECK(!mf_matches_in(t.body, ":scope", nullptr, t.div, nullptr));
    mf_free(&t);
}

typedef struct hover_state {
    embi_element *target;
} hover_state;

static bool hover_hook(void *user, embi_element *el, embi_sel_pc pc)
{
    hover_state *h = user;
    return pc == EMBI_PC_HOVER && el == h->target;
}

EMB_TEST(match_state_pseudo_classes)
{
    mf_tree t = mf_build();
    /* Without a hook, the dynamic states never match. */
    EMB_CHECK(!mf_matches(t.p, "p:hover"));
    EMB_CHECK(!mf_matches(t.p, "p:focus"));
    EMB_CHECK(!mf_matches(t.p, "p:target"));

    hover_state hs = {t.p};
    embi_sel_hooks hooks = {&hs, hover_hook};
    EMB_CHECK(mf_matches_in(t.p, "p:hover", nullptr, nullptr, &hooks));
    EMB_CHECK(!mf_matches_in(t.span, "span:hover", nullptr, nullptr, &hooks));

    /* Attribute-based form states without a hook. */
    EMB_CHECK(mf_matches(t.input, "input:checked"));
    EMB_CHECK(mf_matches(t.input, "input:disabled"));
    EMB_CHECK(!mf_matches(t.input, "input:enabled"));
    EMB_CHECK(!mf_matches(t.input, "input:required"));
    EMB_CHECK(mf_matches(t.input, "input:optional"));
    EMB_CHECK(mf_matches(t.input, "input:default"));
    EMB_CHECK(mf_matches(t.input, "input:read-only"));
    EMB_CHECK(!mf_matches(t.input, "input:read-write"));
    EMB_CHECK(mf_matches(t.p, "p:read-only"));
    EMB_CHECK(!mf_matches(t.p, "p:read-write"));
    EMB_CHECK(mf_matches(t.a, "a:link"));
    EMB_CHECK(mf_matches(t.a, "a:any-link"));
    EMB_CHECK(!mf_matches(t.a, "a:visited"));
    EMB_CHECK(!mf_matches(t.p, "p:link"));
    EMB_CHECK(mf_matches(t.div, ":defined"));
    EMB_CHECK(!mf_matches(t.custom, ":defined"));
    EMB_CHECK(!mf_matches(t.p, ":invalid"));
    EMB_CHECK(!mf_matches(t.p, ":valid"));
    mf_free(&t);
}

EMB_TEST(match_pseudo_elements)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches_pe(t.p, "p::before", EMBI_PE_BEFORE));
    EMB_CHECK(!mf_matches_pe(t.p, "p::before", EMBI_PE_NONE));
    EMB_CHECK(!mf_matches_pe(t.p, "p", EMBI_PE_BEFORE));
    EMB_CHECK(mf_matches_pe(t.p, "p:after", EMBI_PE_AFTER));
    EMB_CHECK(mf_matches_pe(t.p, "::before", EMBI_PE_BEFORE));
    EMB_CHECK(!mf_matches_pe(t.p, "p::after", EMBI_PE_BEFORE));
    EMB_CHECK(mf_matches_pe(t.input, "input::placeholder", EMBI_PE_PLACEHOLDER));
    EMB_CHECK(!mf_matches_pe(t.span, "p::before", EMBI_PE_BEFORE));
    mf_free(&t);
}

EMB_TEST(match_webkit_range_and_scrollbar_parts)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches_pe(t.input, "input::-webkit-slider-runnable-track", EMBI_PE_SLIDER_TRACK));
    EMB_CHECK(mf_matches_pe(t.input, "input::-webkit-slider-thumb", EMBI_PE_SLIDER_THUMB));
    EMB_CHECK(!mf_matches_pe(t.input, "input::-webkit-slider-thumb", EMBI_PE_SLIDER_TRACK));
    EMB_CHECK(!mf_matches_pe(t.input, "input::-webkit-slider-thumb", EMBI_PE_NONE));
    EMB_CHECK(!mf_matches_pe(t.p, "input::-webkit-slider-thumb", EMBI_PE_SLIDER_THUMB));
    /* the universal scrollbar rule matches every element */
    EMB_CHECK(mf_matches_pe(t.p, "::-webkit-scrollbar", EMBI_PE_SCROLLBAR));
    EMB_CHECK(mf_matches_pe(t.div, "*::-webkit-scrollbar", EMBI_PE_SCROLLBAR));
    EMB_CHECK(mf_matches_pe(t.div, "div::-webkit-scrollbar-thumb", EMBI_PE_SCROLLBAR_THUMB));
    EMB_CHECK(!mf_matches_pe(t.div, "span::-webkit-scrollbar-thumb", EMBI_PE_SCROLLBAR_THUMB));
    EMB_CHECK(mf_matches_pe(t.div, "::-webkit-scrollbar-track, ::-webkit-scrollbar-corner", EMBI_PE_SCROLLBAR_CORNER));
    EMB_CHECK(!mf_matches_pe(t.div, "::-webkit-scrollbar-track", EMBI_PE_SCROLLBAR_CORNER));
    mf_free(&t);
}

EMB_TEST(match_namespaces)
{
    mf_tree t = mf_build();
    embi_atom svg_uri = embi_atom_intern(MF_SVG, (uint32_t)strlen(MF_SVG));
    embi_atom html_uri = embi_atom_intern(MF_XHTML, (uint32_t)strlen(MF_XHTML));
    embi_atom svg_prefix = embi_atom_intern("svg", 3);
    embi_sel_nsmap maps[] = {{svg_prefix, svg_uri}, {0, html_uri}};
    embi_sel_env env = {maps, 2, nullptr};

    EMB_CHECK(mf_matches_in(t.rect, "svg|rect", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.rect, "*|rect", &env, nullptr, nullptr));
    /* An unprefixed type takes the default namespace (XHTML here), so the SVG rect does not match. */
    EMB_CHECK(!mf_matches_in(t.rect, "rect", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.div, "div", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.div, "svg|div", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.rect, "|rect", &env, nullptr, nullptr));
    mf_free(&t);
}

EMB_TEST(match_quirks_case_folding)
{
    /* Standards mode (a doctype): class and id selectors are case-sensitive. */
    mf_tree t = mf_build();
    EMB_CHECK_EQ_INT(t.doc->quirks, EMBI_QUIRKS_NONE);
    EMB_CHECK(!mf_matches(t.div, ".X"));
    EMB_CHECK(!mf_matches(t.div, "#D1"));
    EMB_CHECK(mf_matches(t.div, ".x"));
    mf_free(&t);

    /* No doctype: the parser decides quirks mode, and the selectors turn ASCII case-insensitive. */
    mf_tree q = mf_build_quirks();
    EMB_CHECK_EQ_INT(q.doc->quirks, EMBI_QUIRKS_FULL);
    EMB_CHECK(mf_matches(q.div, ".X"));
    EMB_CHECK(mf_matches(q.div, "#D1"));
    EMB_CHECK(!mf_matches(q.div, ".z"));
    EMB_CHECK(mf_matches(q.body, "#MAIN.A.B"));
    EMB_CHECK(mf_matches(q.p, "DIV .Y"));
    mf_free(&q);
}

/* The parser folds tag and attribute-name case. The selectors still see the right tree. */
EMB_TEST(match_parser_case_folding)
{
    embi_document *d = mf_parse("<!DOCTYPE html><DIV CLASS=A ID=B><INPUT TYPE=Checkbox></DIV>");
    embi_element *div = mf_find(d, "div");
    embi_element *in = mf_find(d, "input");
    EMB_REQUIRE(div != nullptr && in != nullptr);
    EMB_CHECK(mf_matches(div, "div"));
    EMB_CHECK(mf_matches(div, "DIV"));
    EMB_CHECK(mf_matches(div, "[class]"));
    EMB_CHECK(mf_matches(div, "[CLASS]"));
    EMB_CHECK(mf_matches(div, ".A#B"));
    EMB_CHECK(!mf_matches(div, ".a"));
    EMB_CHECK(!mf_matches(div, "#b"));
    EMB_CHECK(mf_matches(div, "[id=B]"));
    EMB_CHECK(!mf_matches(div, "[id=b]"));
    EMB_CHECK(mf_matches(in, "[TYPE=CHECKBOX]"));
    EMB_CHECK(mf_matches(in, "[type=checkbox]"));
    EMB_CHECK(!mf_matches(in, "[type=checkbox s]"));
    EMB_CHECK(mf_matches(in, "div > INPUT"));
    embi_document_free(d);

    /* The same markup without a doctype is quirks: class and id compare ASCII-insensitively. */
    embi_document *q = mf_parse("<DIV CLASS=A ID=B></DIV>");
    embi_element *qd = mf_find(q, "div");
    EMB_REQUIRE(qd != nullptr);
    EMB_CHECK_EQ_INT(q->quirks, EMBI_QUIRKS_FULL);
    EMB_CHECK(mf_matches(qd, ".a#b"));
    EMB_CHECK(mf_matches(qd, ".A#B"));
    embi_document_free(q);
}

EMB_TEST(match_has_memo_reset)
{
    mf_tree t = mf_build();
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = embi_sel_parse_text(&a, ms("body:has(li.sel)"), nullptr);
    EMB_REQUIRE(l != nullptr);
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, nullptr, nullptr);
    EMB_CHECK(embi_sel_match_list(l, t.body, &ctx));
    /* The second call answers from the memo and must agree. */
    EMB_CHECK(embi_sel_match_list(l, t.body, &ctx));
    embi_sel_ctx_reset(&ctx);
    EMB_CHECK(embi_sel_match_list(l, t.body, &ctx));

    /* After the DOM changes and the memo is reset, the new answer shows. */
    embi_element *lone = mf_el(t.doc, "li");
    EMB_CHECK(embi_remove_child(&t.ul->base, &t.li2->base) == EMBI_DOM_OK);
    embi_sel_ctx_reset(&ctx);
    EMB_CHECK(!embi_sel_match_list(l, t.body, &ctx));
    mf_app(&t.ul->base, &lone->base);
    embi_sel_ctx_free(&ctx);
    embi_arena_free(&a);
    mf_free(&t);
}

/* :disabled without a hook: fieldset inheritance reaches form controls only, the first legend is
 * exempt, option takes its own attribute or its optgroup's, and optgroup takes its own. */
EMB_TEST(match_disabled_fieldset_rules)
{
    mf_tree t = mf_build();
    embi_element *fs = mf_el(t.doc, "fieldset");
    mf_attr(fs, "disabled", "");
    mf_app(&t.body->base, &fs->base);

    embi_element *sel = mf_el(t.doc, "select");
    mf_app(&fs->base, &sel->base);
    embi_element *opt = mf_el(t.doc, "option");
    mf_app(&sel->base, &opt->base);

    embi_element *og = mf_el(t.doc, "optgroup");
    mf_attr(og, "disabled", "");
    mf_app(&sel->base, &og->base);
    embi_element *opt2 = mf_el(t.doc, "option");
    mf_app(&og->base, &opt2->base);

    embi_element *legend = mf_el(t.doc, "legend");
    mf_app(&fs->base, &legend->base);
    embi_element *in_legend = mf_el(t.doc, "input");
    mf_app(&legend->base, &in_legend->base);
    embi_element *in_body = mf_el(t.doc, "input");
    mf_app(&fs->base, &in_body->base);

    /* A form control inherits from the disabled fieldset. */
    EMB_CHECK(mf_matches(sel, "select:disabled"));
    EMB_CHECK(mf_matches(in_body, "input:disabled"));
    /* An option does not inherit from the fieldset through its select. */
    EMB_CHECK(!mf_matches(opt, "option:disabled"));
    EMB_CHECK(mf_matches(opt, "option:enabled"));
    /* An optgroup's own attribute disables its options. */
    EMB_CHECK(mf_matches(og, "optgroup:disabled"));
    EMB_CHECK(mf_matches(opt2, "option:disabled"));
    /* The first legend's contents are not disabled by the fieldset. */
    EMB_CHECK(!mf_matches(in_legend, "input:disabled"));
    EMB_CHECK(mf_matches(in_legend, "input:enabled"));
    mf_free(&t);
}

/* dir=auto takes the first strong character. Neutral punctuation and symbols do not count. */
EMB_TEST(match_auto_dir_skips_neutral_text)
{
    mf_tree t = mf_build();
    embi_element *d1 = mf_el(t.doc, "div");
    mf_attr(d1, "dir", "auto");
    mf_text(&d1->base, "\xE2\x80\x94 \xD7\xA9"); /* em dash, space, shin */
    EMB_CHECK(mf_matches(d1, "div:dir(rtl)"));

    embi_element *d2 = mf_el(t.doc, "div");
    mf_attr(d2, "dir", "auto");
    mf_text(&d2->base, "\xE2\x80\x9C\xD7\xA9"); /* left double quote, shin */
    EMB_CHECK(mf_matches(d2, "div:dir(rtl)"));

    embi_element *d3 = mf_el(t.doc, "div");
    mf_attr(d3, "dir", "auto");
    mf_text(&d3->base, "\xE2\x82\xAC \xE4\xB8\x80\xD7\xA9"); /* euro sign, space, CJK, shin */
    EMB_CHECK(mf_matches(d3, "div:dir(ltr)"));

    embi_element *d4 = mf_el(t.doc, "div");
    mf_attr(d4, "dir", "auto");
    mf_text(&d4->base, "\xE2\x80\x94"); /* only neutral text: the default is ltr */
    EMB_CHECK(mf_matches(d4, "div:dir(ltr)"));

    embi_element *d5 = mf_el(t.doc, "div");
    mf_attr(d5, "dir", "auto");
    mf_text(&d5->base, "\xCE\xB1\xD7\xA9"); /* Greek alpha, shin */
    EMB_CHECK(mf_matches(d5, "div:dir(ltr)"));
    mf_free(&t);
}


/* CSS Nesting: & stands for the parent list (as :is()), or :scope without one. */
EMB_TEST(match_nesting_ampersand)
{
    mf_tree t = mf_build();
    embi_arena a;
    embi_arena_init(&a, 2048);
    const embi_sel_list *parent = embi_sel_parse_text(&a, ms("div.x, ul"), nullptr);
    EMB_REQUIRE(parent != nullptr);
    embi_sel_env env = {nullptr, 0, parent};
    EMB_CHECK(mf_matches_in(t.p, "& > p", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.li1, "& > li", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.li1, "& li", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.i, "& > i", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.div, "&", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.ul, "&:not(.q)", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.body, "&", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.div, ".x&", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.div, ".y&", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.ul, "& + ul", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.ul, "div.x + &", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.i, "& + i", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.i, "& ~ input", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.input, "& ~ input", &env, nullptr, nullptr));
    /* Without a parent list, & is :scope. */
    EMB_CHECK(mf_matches_in(t.p, "& > p", nullptr, t.div, nullptr));
    EMB_CHECK(!mf_matches_in(t.p, "& > p", nullptr, t.ul, nullptr));
    embi_arena_free(&a);
    mf_free(&t);
}

EMB_TEST(match_nth_last_and_of_s)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.li2, "li:nth-last-of-type(2)"));
    EMB_CHECK(mf_matches(t.li1, "li:nth-last-of-type(3)"));
    EMB_CHECK(!mf_matches(t.li3, "li:nth-last-of-type(2)"));
    EMB_CHECK(mf_matches(t.li3, "li:nth-last-of-type(1)"));
    EMB_CHECK(mf_matches(t.li2, "li:nth-last-child(1 of .sel)"));
    EMB_CHECK(!mf_matches(t.li3, "li:nth-last-child(1 of .sel)"));
    EMB_CHECK(mf_matches(t.li3, "li:nth-last-child(1 of :not(.sel))"));
    EMB_CHECK(mf_matches(t.li1, "li:nth-last-child(2 of :not(.sel))"));
    EMB_CHECK(!mf_matches(t.li1, "li:nth-last-child(1 of :not(.sel))"));
    EMB_CHECK(mf_matches(t.li1, "li:nth-child(1 of :not(.sel))"));
    EMB_CHECK(mf_matches(t.li3, "li:nth-child(2 of li:not(.sel))"));
    EMB_CHECK(mf_matches(t.li1, "li:nth-last-child(n+2 of li)"));
    EMB_CHECK(!mf_matches(t.li3, "li:nth-last-child(n+2 of li)"));
    mf_free(&t);
}

EMB_TEST(match_attribute_namespaces)
{
    mf_tree t = mf_build();
    embi_element *use = mf_el_ns(t.doc, MF_SVG, "use");
    EMB_CHECK(embi_el_set_attribute_ns(use, true, ms("http://www.w3.org/1999/xlink"), ms("xlink:href"),
                                       embi_dstr_from_utf8(t.doc->slab, ms("#q"))) == EMBI_DOM_OK);
    mf_attr(use, "id", "u");
    mf_app(&t.body->base, &use->base);
    embi_atom xl = embi_atom_intern("http://www.w3.org/1999/xlink", 28);
    embi_atom pf = embi_atom_intern("xl", 2);
    embi_sel_nsmap maps[] = {{pf, xl}};
    embi_sel_env env = {maps, 1, nullptr};
    EMB_CHECK(mf_matches_in(use, "[xl|href]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(use, "[xl|href=\"#q\"]", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(use, "[xl|href=\"#z\"]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(use, "[*|href]", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(use, "[|href]", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(use, "[href]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(use, "[|id]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(use, "[*|id]", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(use, "[xl|id]", &env, nullptr, nullptr));
    EMB_CHECK(!mf_matches_in(t.a, "[xl|href]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.a, "[*|href]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.a, "[|href]", &env, nullptr, nullptr));
    EMB_CHECK(mf_matches_in(t.a, "[href]", &env, nullptr, nullptr));
    /* An undeclared prefix invalidates the selector. */
    embi_arena ar;
    embi_arena_init(&ar, 512);
    EMB_CHECK(embi_sel_parse_text(&ar, ms("[zz|href]"), &env) == nullptr);
    embi_arena_free(&ar);
    mf_free(&t);
}

static bool state_hook_modal(void *user, embi_element *el, embi_sel_pc pc)
{
    (void)user;
    (void)el;
    return pc == EMBI_PC_MODAL || pc == EMBI_PC_POPOVER_OPEN;
}

EMB_TEST(match_open_modal_popover)
{
    embi_document *d = mf_parse("<!DOCTYPE html><body><details open><summary>s</summary></details>"
                                "<details></details><dialog open></dialog><dialog></dialog>"
                                "<div popover></div>");
    embi_element *d1 = mf_find(d, "details");
    embi_element *d2 = (embi_element *)d1->base.next;
    embi_element *dl1 = mf_find(d, "dialog");
    embi_element *dl2 = (embi_element *)dl1->base.next;
    embi_element *pop = mf_find(d, "div");
    EMB_REQUIRE(d1 && d2 && dl1 && dl2 && pop);
    EMB_CHECK(mf_matches(d1, "details:open"));
    EMB_CHECK(!mf_matches(d2, "details:open"));
    EMB_CHECK(mf_matches(dl1, "dialog:open"));
    EMB_CHECK(!mf_matches(dl2, "dialog:open"));
    EMB_CHECK(!mf_matches(pop, ":open"));
    /* Without a hook the modal and popover states never match. */
    EMB_CHECK(!mf_matches(dl1, ":modal"));
    EMB_CHECK(!mf_matches(pop, ":popover-open"));
    embi_sel_hooks hooks = {nullptr, state_hook_modal};
    EMB_CHECK(mf_matches_in(dl1, ":modal", nullptr, nullptr, &hooks));
    EMB_CHECK(mf_matches_in(pop, ":popover-open", nullptr, nullptr, &hooks));
    EMB_CHECK(!mf_matches_in(pop, ":hover", nullptr, nullptr, &hooks));
    /* With a hook, the hook answers :open too (it says no here). */
    EMB_CHECK(!mf_matches_in(d1, "details:open", nullptr, nullptr, &hooks));
    embi_document_free(d);
}

EMB_TEST(match_word_operator_with_whitespace_value)
{
    mf_tree t = mf_build();
    /* A ~= value with whitespace (or empty) never matches, even if the whole attribute equals it. */
    EMB_CHECK(!mf_matches(t.p, "[title~=\"hello world\"]"));
    EMB_CHECK(!mf_matches(t.p, "[title~=\" hello\"]"));
    EMB_CHECK(!mf_matches(t.p, "[title~=\"\"]"));
    EMB_CHECK(mf_matches(t.p, "[title~=\"hello\"]"));
    EMB_CHECK(mf_matches(t.p, "[title~=hello][title~=world]"));
    EMB_CHECK(mf_matches(t.p, "[title=\"hello world\"]"));
    mf_free(&t);
}

EMB_TEST(match_has_not_with_siblings_at_depth)
{
    mf_tree t = mf_build();
    EMB_CHECK(mf_matches(t.ul, "ul:has(> li:first-child + li.sel)"));
    EMB_CHECK(mf_matches(t.ul, "ul:has(li:not(.sel) ~ li.sel)"));
    EMB_CHECK(mf_matches(t.ul, "ul:has(> li + li + li)"));
    EMB_CHECK(!mf_matches(t.ul, "ul:has(> li + li + li + li)"));
    EMB_CHECK(mf_matches(t.ul, "ul:not(:has(li + li + li + li))"));
    EMB_CHECK(mf_matches(t.body, "body:has(div + ul + i)"));
    EMB_CHECK(mf_matches(t.body, "body:has(div ~ a)"));
    EMB_CHECK(!mf_matches(t.body, "body:has(i + ul)"));
    EMB_CHECK(mf_matches(t.body, "body:has(ul + i ~ input + a)"));
    EMB_CHECK(mf_matches(t.div, ":is(ul, div):has(+ ul > li + li)"));
    EMB_CHECK(mf_matches(t.ul, ":is(ul, div):has(+ i, ~ a)"));
    EMB_CHECK(mf_matches(t.div, "div:not(:has(+ i))"));
    EMB_CHECK(!mf_matches(t.ul, "ul:not(:has(+ i))"));
    EMB_CHECK(mf_matches(t.html, "html:has(body ul li + li.sel)"));
    EMB_CHECK(!mf_matches(t.li2, "li:not(:first-child, :last-child) + li:not(.sel)"));
    EMB_CHECK(mf_matches(t.li3, "li:not(:first-child, :last-child) + li:not(.sel)"));
    EMB_CHECK(mf_matches(t.li3, "ul:has(> li.sel) > li:not(.sel):not(:first-child)"));
    mf_free(&t);
}

/* Long descendant chains that cannot match must stay fast (no combinatorial backtracking). */
EMB_TEST(match_long_descendant_chains)
{
    enum { DEPTH = 40 };
    embi_document *d = embi_document_new(true);
    embi_element *html = mf_el(d, "html");
    mf_app(&d->base, &html->base);
    embi_element *cur = html;
    for (int k = 0; k < DEPTH; k++) {
        embi_element *e = mf_el(d, "div");
        mf_attr(e, "class", "a");
        mf_app(&cur->base, &e->base);
        cur = e;
    }
    embi_element *b = mf_el(d, "span");
    mf_attr(b, "class", "b");
    mf_app(&cur->base, &b->base);
    EMB_CHECK(!mf_matches(b, ".z .a .a .a .a .a .a .a .a .b"));
    EMB_CHECK(!mf_matches(b, ".z .a .a .a .a > .a .a .a .a .a .b"));
    EMB_CHECK(!mf_matches(b, ".a .a .a .a .a .a .a .a .a .a .a .a .a .a .a .a .z .b"));
    EMB_CHECK(!mf_matches(b, ".a ~ .a ~ .a ~ .a ~ .a ~ .a ~ .a ~ .b"));
    EMB_CHECK(mf_matches(b, "html .a .a .a .a .a .a .a .a .b"));
    EMB_CHECK(mf_matches(b, ".a .a > .a .a .a > .a .b"));
    EMB_CHECK(!mf_matches(b, ".a .a > .a .a .a > .z .a .b"));
    EMB_CHECK(mf_matches(html, "html:has(.a .a .a .a .a .a .a .a .b)"));
    EMB_CHECK(!mf_matches(html, "html:has(.a .a .a .a .a .a .a .a .z)"));
    EMB_CHECK(!mf_matches(html, "html:has(> .a .a .a .a .a > .a .a .a .a .a .z)"));
    embi_document_free(d);
}

/* A mixed combinator chain at the compound cap on a deep tree stays fast; one over the cap is
 * rejected at parse time instead of exhausting the stack or backtracking exponentially. */
EMB_TEST(match_chain_at_cap)
{
    enum { DEPTH = 100 };
    embi_document *d = embi_document_new(true);
    embi_element *html = mf_el(d, "html");
    mf_app(&d->base, &html->base);
    embi_element *cur = html;
    for (int k = 0; k < DEPTH; k++) {
        embi_element *e = mf_el(d, "div");
        mf_attr(e, "class", "a");
        mf_app(&cur->base, &e->base);
        cur = e;
    }
    static char sel[1024];
    for (int reps = 59; reps <= 69; reps += 3) {
        size_t len = (size_t)snprintf(sel, sizeof sel, ".z > .a");
        for (int k = 0; k < reps; k++) {
            len += (size_t)snprintf(sel + len, sizeof sel - len, " .a");
        }
        embi_arena a;
        embi_arena_init(&a, 1024);
        const embi_sel_list *l = embi_sel_parse_text(&a, (embi_str){sel, (uint32_t)len}, nullptr);
        EMB_CHECK_EQ_INT(l != nullptr, reps + 2 <= 64);
        embi_arena_free(&a);
        if (reps + 2 <= 64) {
            EMB_CHECK(!mf_matches(cur, sel));
        }
    }
    embi_document_free(d);
}

/* Combinator chains that need real backtracking give the right answer. */
EMB_TEST(match_backtracking_correctness)
{
    embi_document *d = mf_parse("<!DOCTYPE html><body><div class=a><div class=b><p class=c></p></div>"
                                "<div class=b><p class=d></p></div></div>");
    embi_element *c = mf_find(d, "p");
    embi_element *dd = (embi_element *)c->base.parent->next->first_child;
    EMB_REQUIRE(c && dd && dd->base.type == EMBI_NODE_ELEMENT);
    EMB_CHECK(mf_matches(dd, ".b ~ .b > .d"));
    EMB_CHECK(!mf_matches(c, ".b ~ .b > .c"));
    EMB_CHECK(mf_matches(dd, ".a .b ~ .b .d"));
    EMB_CHECK(mf_matches(dd, "body > .a > .b + .b > .d"));
    EMB_CHECK(!mf_matches(dd, "body > .a > .b + .b + .b > .d"));
    EMB_CHECK(mf_matches(c, ".a .b .c"));
    EMB_CHECK(mf_matches(c, "div div p"));
    EMB_CHECK(mf_matches(c, "body div > div p"));
    embi_document_free(d);
}

EMB_TEST(match_dir_edge_cases)
{
    embi_document *d = mf_parse(
        "<!DOCTYPE html><body>"
        "<div id=i1 dir=auto>\xE0\xA4\x85 \xD7\xA9</div>"   /* Devanagari then Hebrew */
        "<div id=i2 dir=auto>\xE0\xA6\x85\xD7\xA9</div>"    /* Bengali then Hebrew */
        "<div id=i3 dir=auto>\xE0\xAE\x85\xD7\xA9</div>"    /* Tamil then Hebrew */
        "<div id=i4 dir=auto>\xE0\xB0\x85\xD7\xA9</div>"    /* Telugu then Hebrew */
        "<div id=i5 dir=auto>\xE1\x88\x80\xD7\xA9</div>"    /* Ethiopic then Hebrew */
        "<div id=r1 dir=auto>\xD6\xBE</div>"                /* maqaf: strong R */
        "<div id=r2 dir=auto>\xD7\x83" "a</div>"            /* sof pasuq: strong R */
        "<div id=l1 dir=auto>\xD9\xA1 a\xD7\xA9</div>"      /* Arabic-Indic digit is weak */
        "<div id=l2 dir=auto>\xD8\x80 a\xD7\xA9</div>"      /* U+0600 is AN, weak */
        "<div id=m1 dir=auto>\xD6\x91\xD7\xA9</div>"        /* Hebrew point is NSM, then shin */
        "<div id=a1 dir=auto>\xD8\xA7</div>"                /* Arabic alef */
        "<bdi id=b1 dir=bogus>\xD7\xA9</bdi>"
        "<bdi id=b2>\xD7\xA9</bdi>"
        "<bdi id=b3 dir=ltr>\xD7\xA9</bdi>"
        "<div id=c1 dir=auto><span dir=bogus>\xD7\xA9</span></div>"
        "<div id=c2 dir=auto><span dir=ltr>\xD7\xA9</span>a</div>"
        "<div id=c3 dir=auto><span dir=rtl>a</span>\xD7\xA9</div>"
        "<div id=e1 dir=auto>\xF0\x9F\x98\x80 \xE2\x80\x9C\xD7\xA9</div>" /* emoji, quote, shin */
        "</body>");
    EMB_REQUIRE(d != nullptr);
    static const struct {
        const char *id;
        const char *sel;
    } cases[] = {
        {"i1", "#i1:dir(ltr)"}, {"i2", "#i2:dir(ltr)"}, {"i3", "#i3:dir(ltr)"},
        {"i4", "#i4:dir(ltr)"}, {"i5", "#i5:dir(ltr)"}, {"r1", "#r1:dir(rtl)"},
        {"r2", "#r2:dir(rtl)"}, {"l1", "#l1:dir(ltr)"}, {"l2", "#l2:dir(ltr)"},
        {"m1", "#m1:dir(rtl)"}, {"a1", "#a1:dir(rtl)"}, {"b1", "#b1:dir(rtl)"},
        {"b2", "#b2:dir(rtl)"}, {"b3", "#b3:dir(ltr)"}, {"c1", "#c1:dir(rtl)"},
        {"c2", "#c2:dir(ltr)"}, {"c3", "#c3:dir(rtl)"}, {"e1", "#e1:dir(rtl)"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        embi_element *e = mf_by_id(d, cases[k].id);
        EMB_REQUIRE(e != nullptr);
        if (!mf_matches(e, cases[k].sel)) {
            emb_test_report_fail(__FILE__, __LINE__, cases[k].id);
        }
    }
    embi_document_free(d);
}

EMB_TEST(match_form_and_link_fallbacks)
{
    embi_document *d = mf_parse(
        "<!DOCTYPE html><head><link id=lk href=x></head><body>"
        "<div id=h contenteditable><span id=s1>x</span><p id=p1 contenteditable=false>y"
        "<b id=b1>z</b></p><i id=i1 contenteditable=bogus>w</i></div>"
        "<span id=s2>q</span><area id=ar href=y><a id=an href=z>k</a><a id=nh>k</a>"
        "<textarea id=ta></textarea><textarea id=tr readonly></textarea>"
        "<font-face id=ff></font-face><annotation-xml id=ax></annotation-xml><my-el id=me></my-el>"
        "</body>");
    EMB_REQUIRE(d != nullptr);
    embi_element *lk = mf_by_id(d, "lk");
    embi_element *ar = mf_by_id(d, "ar");
    embi_element *an = mf_by_id(d, "an");
    embi_element *nh = mf_by_id(d, "nh");
    embi_element *h = mf_by_id(d, "h");
    embi_element *s1 = mf_by_id(d, "s1");
    embi_element *p1 = mf_by_id(d, "p1");
    embi_element *b1 = mf_by_id(d, "b1");
    embi_element *i1 = mf_by_id(d, "i1");
    embi_element *s2 = mf_by_id(d, "s2");
    embi_element *ta = mf_by_id(d, "ta");
    embi_element *tr = mf_by_id(d, "tr");
    embi_element *ff = mf_by_id(d, "ff");
    embi_element *ax = mf_by_id(d, "ax");
    embi_element *me = mf_by_id(d, "me");
    EMB_REQUIRE(lk && ar && an && nh && h && s1 && p1 && b1 && i1 && s2 && ta && tr && ff && ax && me);
    /* :link is a and area only (not link). */
    EMB_CHECK(!mf_matches(lk, ":link"));
    EMB_CHECK(mf_matches(ar, ":any-link"));
    EMB_CHECK(mf_matches(an, ":link"));
    EMB_CHECK(!mf_matches(nh, ":link"));
    /* The editing host and its descendants are :read-write. contenteditable=false cuts it off,
     * and an invalid value inherits. */
    EMB_CHECK(mf_matches(h, ":read-write"));
    EMB_CHECK(mf_matches(s1, ":read-write"));
    EMB_CHECK(!mf_matches(p1, ":read-write"));
    EMB_CHECK(!mf_matches(b1, ":read-write"));
    EMB_CHECK(mf_matches(b1, ":read-only"));
    EMB_CHECK(mf_matches(i1, ":read-write"));
    EMB_CHECK(!mf_matches(s2, ":read-write"));
    EMB_CHECK(mf_matches(ta, ":read-write"));
    EMB_CHECK(!mf_matches(tr, ":read-write"));
    /* Reserved hyphenated names are defined. */
    EMB_CHECK(mf_matches(ff, ":defined"));
    EMB_CHECK(mf_matches(ax, ":defined"));
    EMB_CHECK(!mf_matches(me, ":defined"));
    embi_document_free(d);
}

EMB_TEST(match_lang_edge_cases)
{
    embi_document *d = mf_parse(
        "<!DOCTYPE html><body lang=\"en-a-b-us\"><p id=p>x</p>"
        "<span lang=\"a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-q-r-s-t-u-v-w-x-y-z-a-b-c-d-e-f-g-h\">y</span>"
        "</body>");
    embi_element *body = mf_find(d, "body");
    embi_element *sp = mf_find(d, "span");
    EMB_REQUIRE(body && sp);
    EMB_CHECK(mf_matches(body, ":lang(en)"));
    EMB_CHECK(mf_matches(body, ":lang(\"en-a\")"));
    EMB_CHECK(mf_matches(body, ":lang(\"en-a-b-us\")"));
    /* A singleton subtag that does not match ends the search (RFC 4647 2.3.2). */
    EMB_CHECK(!mf_matches(body, ":lang(\"en-us\")"));
    EMB_CHECK(!mf_matches(body, ":lang(\"en-b\")"));
    /* A range longer than the cap matches nothing instead of a truncated prefix. */
    EMB_CHECK(!mf_matches(body, ":lang(\"en-a-b-us-*-*-*-*-*-*-*-*-*-*-*-*-zz\")"));
    /* A tag longer than the cap matches nothing. */
    EMB_CHECK(!mf_matches(sp, ":lang(a)"));
    embi_document_free(d);
}

EMB_TEST_MAIN()
