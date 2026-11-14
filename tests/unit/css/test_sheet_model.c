/*
 * Stylesheet model tests: rule kinds, context rules, declarations, nesting, @supports through the
 * property hooks, layers, registrations, keyframes, containers, scopes, and refcounting.
 */
#include "emb_test.h"

#include "css/css.h"

#include <string.h>

static bool is_str(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool atom_is(embi_atom a, const char *lit)
{
    return is_str(embi_atom_str(a), lit);
}

/* Accepts every property except one named "bogus". */
static bool hook_decl_ok(void *user, embi_str name, embi_css_slice value, bool important)
{
    (void)user;
    (void)value;
    (void)important;
    return !is_str(name, "bogus");
}

static bool hook_selector_ok(void *user, embi_css_slice selector)
{
    (void)user;
    (void)selector;
    return true;
}

static bool hook_reject_all(void *user, embi_str name, embi_css_slice value, bool important)
{
    (void)user;
    (void)name;
    (void)value;
    (void)important;
    return false;
}

static const embi_css_hooks hooks_accept = {nullptr, hook_decl_ok, hook_selector_ok};
static const embi_css_hooks hooks_reject = {nullptr, hook_reject_all, nullptr};

static embi_css_sheet *parse_with(const char *css, const embi_css_hooks *hooks)
{
    embi_css_parse_opts opts = {nullptr, hooks, {nullptr, 0}};
    return embi_css_sheet_parse((embi_str){css, (uint32_t)strlen(css)}, &opts);
}

EMB_TEST(css_sheet_style_rule_and_decls)
{
    embi_css_sheet *s = parse_with("a { color: red !important; --X: 1 2; COLOR: blue }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *r = &s->rules[0];
    EMB_CHECK(r->kind == EMBI_CSS_RULE_STYLE);
    EMB_REQUIRE(r->ndecls == 3);
    EMB_CHECK(atom_is(r->decls[0].name, "color"));
    EMB_CHECK(r->decls[0].important);
    EMB_CHECK(atom_is(r->decls[1].name, "--X"));
    EMB_CHECK(!r->decls[1].important);
    EMB_CHECK(atom_is(r->decls[2].name, "color"));
    EMB_CHECK(!r->decls[2].important);
    EMB_CHECK(r->decls[0].prop == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_nested_rules)
{
    embi_css_sheet *s = parse_with("a { b { c: d } & e { f: g } color: red; }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *r = &s->rules[0];
    EMB_CHECK(r->kind == EMBI_CSS_RULE_STYLE);
    EMB_CHECK(r->nkids == 2);
    EMB_CHECK(r->ndecls == 1);
    EMB_REQUIRE(r->nkids == 2);
    EMB_CHECK(r->kids[0].kind == EMBI_CSS_RULE_STYLE && r->kids[0].ndecls == 1);
    EMB_CHECK(r->kids[1].kind == EMBI_CSS_RULE_STYLE && r->kids[1].ndecls == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_media_and_nested_media)
{
    embi_css_sheet *s =
        parse_with("@media screen { a { b: c } } p { @media print { color: red } }", nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_MEDIA);
    EMB_CHECK(s->rules[0].nkids == 1);
    EMB_CHECK(s->rules[1].kind == EMBI_CSS_RULE_STYLE);
    EMB_REQUIRE(s->rules[1].nkids == 1);
    const embi_css_rule *m = &s->rules[1].kids[0];
    EMB_CHECK(m->kind == EMBI_CSS_RULE_MEDIA);
    EMB_CHECK(m->ndecls == 1);
    EMB_CHECK(m->nkids == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_context_rules)
{
    /* @import and @namespace are dropped inside a block, and @import is dropped after a rule. */
    embi_css_sheet *s = parse_with(
        "@media print { @import 'x.css'; @namespace 'y'; a{} } a {} @import 'late.css';", nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_MEDIA);
    EMB_CHECK(s->rules[0].nkids == 1);
    EMB_CHECK(s->rules[1].kind == EMBI_CSS_RULE_STYLE);
    embi_css_sheet_unref(s);

    /* Font-face and keyframes are dropped when nested in a style rule. */
    s = parse_with("a { @font-face { font-family: x } @keyframes k { } }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].nkids == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_namespace)
{
    embi_css_sheet *s =
        parse_with("@namespace svg url(http://x); @namespace foo 'bar'; @namespace a b c;", nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_NAMESPACE);
    EMB_CHECK(atom_is(s->rules[0].name, "svg"));
    EMB_CHECK(is_str(s->rules[0].url, "http://x"));
    EMB_CHECK(atom_is(s->rules[1].name, "foo"));
    EMB_CHECK(is_str(s->rules[1].url, "bar"));
    embi_css_sheet_unref(s);

    /* A namespace after a style rule is dropped. */
    s = parse_with("a {} @namespace 'z';", nullptr);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_supports_with_hooks)
{
    embi_css_sheet *s = parse_with(
        "@supports (display: grid) { a{} }"
        "@supports not (bogus: 1) { a{} }"
        "@supports (display: grid) and (bogus: 1) { }"
        "@supports (display: grid) or (bogus: 1) { }"
        "@supports selector(a > b) { }"
        "@supports foo { }"
        "@supports (not (bogus: 1)) and selector(x) { }",
        &hooks_accept);
    /* "@supports foo" is not a valid condition, so its rule is dropped rather than kept as false. */
    EMB_REQUIRE(s->nrules == 6);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_SUPPORTS && s->rules[0].cond);
    EMB_CHECK(s->rules[1].cond);
    EMB_CHECK(!s->rules[2].cond);
    EMB_CHECK(s->rules[3].cond);
    EMB_CHECK(s->rules[4].cond);
    EMB_CHECK(s->rules[5].cond);
    embi_css_sheet_unref(s);

    /* Without hooks, declarations and selectors are unsupported. */
    s = parse_with("@supports (display: grid) { a{} } @supports selector(a) { }", nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(!s->rules[0].cond);
    EMB_CHECK(!s->rules[1].cond);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_layers)
{
    embi_css_sheet *s = parse_with(
        "@layer a.b, c; @layer d { x{} } @layer { y{} } @layer e.f.g {} @layer ; @layer p, q { }",
        nullptr);
    EMB_REQUIRE(s->nrules == 4);
    const embi_css_rule *st = &s->rules[0];
    EMB_CHECK(st->kind == EMBI_CSS_RULE_LAYER && !st->block);
    EMB_REQUIRE(st->nlayers == 2);
    EMB_CHECK(atom_is(st->layers[0], "a.b"));
    EMB_CHECK(atom_is(st->layers[1], "c"));
    EMB_CHECK(s->rules[1].block && atom_is(s->rules[1].name, "d") && s->rules[1].nkids == 1);
    EMB_CHECK(s->rules[2].block && s->rules[2].name == 0 && s->rules[2].nkids == 1);
    EMB_CHECK(s->rules[3].block && atom_is(s->rules[3].name, "e.f.g") && s->rules[3].nkids == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_font_face_property_and_validation)
{
    /* Descriptors are kept even when the property hook rejects everything. */
    embi_css_sheet *s = parse_with(
        "@font-face { font-family: X; src: url(x.woff) }"
        "@font-face foo { }"
        "@property --p { syntax: '<length>'; inherits: false; initial-value: 0px }"
        "a { color: red }",
        &hooks_reject);
    EMB_REQUIRE(s->nrules == 3);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_FONT_FACE && s->rules[0].ndecls == 2);
    EMB_CHECK(atom_is(s->rules[0].decls[0].name, "font-family"));
    EMB_CHECK(s->rules[1].kind == EMBI_CSS_RULE_PROPERTY);
    EMB_CHECK(atom_is(s->rules[1].name, "--p"));
    EMB_CHECK(s->rules[1].ndecls == 3);
    EMB_CHECK(s->rules[2].kind == EMBI_CSS_RULE_STYLE && s->rules[2].ndecls == 0);
    embi_css_sheet_unref(s);

    /* A @property name must be a custom property name. */
    s = parse_with("@property x { } @property --q { }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(atom_is(s->rules[0].name, "--q"));
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_keyframes)
{
    embi_css_sheet *s = parse_with(
        "@keyframes spin { from { transform: none } 50% { x: y } to {} } @keyframes none { }",
        nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *kf = &s->rules[0];
    EMB_CHECK(kf->kind == EMBI_CSS_RULE_KEYFRAMES);
    EMB_CHECK(atom_is(kf->name, "spin"));
    EMB_REQUIRE(kf->nkids == 3);
    EMB_CHECK(kf->kids[0].ndecls == 1);
    EMB_CHECK(kf->kids[1].ndecls == 1);
    EMB_CHECK(kf->kids[2].ndecls == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_container_and_scope)
{
    embi_css_sheet *s = parse_with(
        "@container sidebar (min-width: 400px) { a {} }"
        "@container (width > 1px) { }"
        "@scope (.a) to (.b) { p {} }",
        nullptr);
    EMB_REQUIRE(s->nrules == 3);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_CONTAINER && atom_is(s->rules[0].name, "sidebar"));
    EMB_CHECK(s->rules[0].nkids == 1);
    EMB_CHECK(s->rules[1].kind == EMBI_CSS_RULE_CONTAINER && s->rules[1].name == 0);
    EMB_CHECK(s->rules[2].kind == EMBI_CSS_RULE_SCOPE && s->rules[2].nkids == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_unknown_and_charset_dropped)
{
    embi_css_sheet *s = parse_with("@charset 'utf-8'; @foo bar { } @unknown; a {}", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_STYLE);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_bad_declarations_are_dropped)
{
    embi_css_sheet *s = parse_with("a { color red; x: y; ; z }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_REQUIRE(s->rules[0].ndecls == 1);
    EMB_CHECK(atom_is(s->rules[0].decls[0].name, "x"));
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_deep_nesting_is_bounded)
{
    /* Deeper than the builder limit: the excess is dropped and nothing crashes. */
    enum { DEPTH = 300 };
    static char css[DEPTH * 3 + 8];
    size_t n = 0;
    for (int k = 0; k < DEPTH; k++) {
        css[n++] = 'a';
        css[n++] = '{';
    }
    for (int k = 0; k < DEPTH; k++) {
        css[n++] = '}';
    }
    css[n] = '\0';
    embi_css_sheet *s = parse_with(css, nullptr);
    EMB_CHECK(s->nrules == 1);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_refcount_releases_memory)
{
    const char *css = "a { b: c } @media print { p { q: r } } @layer x { }";
    embi_css_sheet *warm = parse_with(css, nullptr);
    embi_css_sheet_unref(warm);
    size_t base = embi_mem_live();

    for (int k = 0; k < 20; k++) {
        embi_css_sheet *s = parse_with(css, nullptr);
        embi_css_sheet_ref(s);
        embi_css_sheet_unref(s);
        EMB_CHECK(s->refs == 1);
        embi_css_sheet_unref(s);
    }
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(css_sheet_keyframe_names)
{
    /* A string is a valid name even when it reads "none". Idents may not be none, the CSS-wide
     * keywords, or default. */
    embi_css_sheet *s = parse_with(
        "@keyframes \"none\" { to {} } @keyframes initial {} @keyframes default {}"
        "@keyframes inherit {} @keyframes revert-layer {} @keyframes ok {}",
        nullptr);
    EMB_REQUIRE(s->nrules == 2);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_KEYFRAMES && atom_is(s->rules[0].name, "none"));
    EMB_CHECK(atom_is(s->rules[1].name, "ok"));
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_decl_and_nested_order)
{
    /* Declarations and nested rules keep their source order, so CSS Nesting's interleaving can be
     * recovered from the order fields. */
    const char *css = "a { color: red; & b { x: y } color: blue }";
    embi_css_sheet *s = parse_with(css, nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *r = &s->rules[0];
    EMB_REQUIRE(r->ndecls == 2);
    EMB_REQUIRE(r->nkids == 1);
    uint32_t red = (uint32_t)(strstr(css, "color: red") - css);
    uint32_t nested = (uint32_t)(strstr(css, "& b") - css);
    uint32_t blue = (uint32_t)(strstr(css, "color: blue") - css);
    EMB_CHECK(r->decls[0].order == red);
    EMB_CHECK(r->kids[0].order == nested);
    EMB_CHECK(r->decls[1].order == blue);
    EMB_CHECK(r->decls[0].order < r->kids[0].order && r->kids[0].order < r->decls[1].order);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_custom_property_with_braces)
{
    /* A custom property may hold a {} block as its value, alone or with other values. */
    embi_css_sheet *s = parse_with("a { --x:{p:q}; color: red; --y:{a:b} y; --z: { } ; }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *r = &s->rules[0];
    EMB_REQUIRE(r->ndecls == 4);
    EMB_CHECK(atom_is(r->decls[0].name, "--x"));
    EMB_CHECK(atom_is(r->decls[1].name, "color"));
    EMB_CHECK(atom_is(r->decls[2].name, "--y"));
    EMB_CHECK(atom_is(r->decls[3].name, "--z"));
    EMB_CHECK(r->nkids == 0);
    embi_css_sheet_unref(s);

    /* The same block next to a nested rule: the custom property is a declaration, and the
     * hover rule is a nested rule. */
    s = parse_with("a { & b { c: d } --x:{p:q}; a:hover { color: red } }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].ndecls == 1);
    EMB_CHECK(atom_is(s->rules[0].decls[0].name, "--x"));
    EMB_REQUIRE(s->rules[0].nkids == 2);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_text_with_cdo_cdc)
{
    /* At top level CDO and CDC are ignored, both before and after the style rule. */
    embi_css_sheet *s = parse_with("<!-- --> a { b: c } -->", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_STYLE);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_layer_statement_in_style_rule_rejected)
{
    embi_css_sheet *s = parse_with("a { @layer x, y; color: red; @layer z { b { c: d } } }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].kind == EMBI_CSS_RULE_STYLE);
    EMB_CHECK(s->rules[0].ndecls == 1);
    /* The statement form is dropped. The block form nests. */
    for (uint32_t k = 0; k < s->rules[0].nkids; k++) {
        EMB_CHECK(s->rules[0].kids[k].kind == EMBI_CSS_RULE_LAYER && s->rules[0].kids[k].block);
    }
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_keyframes_invalid_selectors_skipped)
{
    embi_css_sheet *s = parse_with(
        "@keyframes k { foo { a: b } 50% { a: b } -10% { a: b } 101% { a: b } 100.5% { a: b }"
        " 0% { a: b } 100% { a: b } from, bogus { a: b } 10px { a: b } to, 30% { a: b } , { a: b } }",
        nullptr);
    EMB_REQUIRE(s->nrules == 1);
    const embi_css_rule *kf = &s->rules[0];
    EMB_CHECK(kf->kind == EMBI_CSS_RULE_KEYFRAMES);
    /* Kept: 50%, 0%, 100%, and "to, 30%". */
    EMB_CHECK(kf->nkids == 4);
    embi_css_sheet_unref(s);

    s = parse_with("@keyframes k { 0% { a: b } -0% { a: b } 1e2% { a: b } 100.0001% { a: b } }",
                   nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_CHECK(s->rules[0].nkids == 3);
    embi_css_sheet_unref(s);
}

EMB_TEST(css_sheet_keyframes_important_ignored)
{
    embi_css_sheet *s = parse_with(
        "@keyframes k { from { a: b !important; c: d } to { e: f ! important } }", nullptr);
    EMB_REQUIRE(s->nrules == 1);
    EMB_REQUIRE(s->rules[0].nkids == 2);
    EMB_CHECK(s->rules[0].kids[0].ndecls == 1);
    EMB_CHECK(atom_is(s->rules[0].kids[0].decls[0].name, "c"));
    EMB_CHECK(!s->rules[0].kids[0].decls[0].important);
    EMB_CHECK(s->rules[0].kids[1].ndecls == 0);
    embi_css_sheet_unref(s);
}

EMB_TEST_MAIN()
