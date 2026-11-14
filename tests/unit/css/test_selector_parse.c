/*
 * Selector parsing (W2b): valid and invalid lists, forgiving :is()/:where(), pseudo-element
 * placement, namespaces, nesting, relative lists, and the structure of the compiled output.
 */
#include "emb_test.h"

#include "css/selector.h"

#include <stdio.h>
#include <string.h>

static embi_str S(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

/* Parses with a fresh arena; returns the list (nullptr when invalid). The arena stays alive in *a. */
static const embi_sel_list *parse(embi_arena *a, const char *sel, const embi_sel_env *env)
{
    return embi_sel_parse_text(a, S(sel), env);
}

static bool valid(const char *sel)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    bool ok = parse(&a, sel, nullptr) != nullptr;
    embi_arena_free(&a);
    return ok;
}

EMB_TEST(sel_valid_simple_forms)
{
    static const char *const good[] = {
        "div",          "*",          "#a",         ".b",          "[x]",
        "[x=y]",        "[x~=y i]",   "a > b",      "a + b",       "a ~ b",
        "a b",          "a  \n b",    ":not(.a, .b)", ":is(a, b) c", ":where()",
        ":is()",        ":has(> a)",  ":nth-child(2n+1 of .x)", "::before",
        ":before",      "a:hover",    "input:checked", "::selection", "*|*",
        "|a",           "li:nth-last-of-type(odd)", "[|y=\"z\" s]", "p:lang(en, \"fr\")",
        "div:dir(rtl)", "a:not(:first-child)", ":root", ":scope > p", "html:has(+ body)",
    };
    for (size_t k = 0; k < sizeof good / sizeof good[0]; k++) {
        if (!valid(good[k])) {
            SDL_Log("    should be valid: \"%s\"", good[k]);
            EMB_CHECK(false);
        }
    }
}

EMB_TEST(sel_invalid_forms)
{
    static const char *const bad[] = {
        "",           " ",         "a >",          "> a",         "a,",       ",a",
        "a,,b",       "a b >",     ".",            "#1",          "[x=]",     "[x y]",
        "[x=1]z",     "a::before b", "::before a",  "::unknown",   ":unknown", ":nth-child(foo)",
        "div.",       ":has(:has(a))", ":not(::before)", ":not()", "x|a",     "a|b",
        ":lang()",    ":dir(up)",  "[x|=]",        "a::before::after",   "p:lang(*)",
        ":hover(a)",  "*a",        "a*",           "[*]",         "p:first-child(1)",
        "[|=a]",
    };
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        if (valid(bad[k])) {
            SDL_Log("    should be invalid: \"%s\"", bad[k]);
            EMB_CHECK(false);
        }
    }
}

EMB_TEST(sel_forgiving_is_where)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = parse(&a, ":is(a, ::before, b)", nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_REQUIRE(l->n == 1);
    /* The argument list drops the pseudo-element item; the outer compound keeps the rest. */
    const embi_sel_simple *s = &l->items[0].comp[0].ss[0];
    EMB_CHECK(s->kind == EMBI_SS_IS);
    EMB_CHECK(s->list->n == 2);

    const embi_sel_list *none = parse(&a, ":where(::before)", nullptr);
    EMB_REQUIRE(none != nullptr);
    EMB_CHECK(none->items[0].comp[0].ss[0].list->n == 0);

    /* Unforgiving: the same argument breaks the whole :not() and the rule. */
    EMB_CHECK(parse(&a, ":not(a, ::before)", nullptr) == nullptr);
    embi_arena_free(&a);
}

EMB_TEST(sel_pseudo_element_placement)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = parse(&a, "p::before", nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_CHECK(l->items[0].pseudo == EMBI_PE_BEFORE);
    EMB_CHECK(l->items[0].nc == 1);

    const embi_sel_list *legacy = parse(&a, "p:after", nullptr);
    EMB_REQUIRE(legacy != nullptr);
    EMB_CHECK(legacy->items[0].pseudo == EMBI_PE_AFTER);

    /* Single-colon forms exist only for the four legacy names. */
    EMB_CHECK(parse(&a, "p:marker", nullptr) == nullptr);
    EMB_CHECK(parse(&a, "p::marker", nullptr) != nullptr);
    EMB_CHECK(parse(&a, "p::file-selector-button", nullptr) != nullptr);
    embi_arena_free(&a);
}

/* The WebKit parts of a range input and of a scroll container parse as pseudo-elements, with a user-action class after them. */
EMB_TEST(sel_webkit_range_and_scrollbar_parts)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    static const struct {
        const char *sel;
        embi_sel_pseudo pe;
    } ok[] = {
        {"input::-webkit-slider-runnable-track", EMBI_PE_SLIDER_TRACK},
        {"input[type=range]:hover::-webkit-slider-thumb", EMBI_PE_SLIDER_THUMB},
        {"input[type=range]:active::-webkit-slider-thumb", EMBI_PE_SLIDER_THUMB},
        {"::-webkit-scrollbar", EMBI_PE_SCROLLBAR},
        {"*::-webkit-scrollbar", EMBI_PE_SCROLLBAR},
        {".foo::-webkit-scrollbar", EMBI_PE_SCROLLBAR},
        {"::-webkit-scrollbar-thumb", EMBI_PE_SCROLLBAR_THUMB},
        {"::-webkit-scrollbar-thumb:hover", EMBI_PE_SCROLLBAR_THUMB},
        {"::-webkit-scrollbar-track", EMBI_PE_SCROLLBAR_TRACK},
        {"::-webkit-scrollbar-corner", EMBI_PE_SCROLLBAR_CORNER},
        {"::-WEBKIT-Scrollbar", EMBI_PE_SCROLLBAR},
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        const embi_sel_list *l = parse(&a, ok[i].sel, nullptr);
        EMB_REQUIRE(l != nullptr);
        EMB_CHECK_EQ_INT(l->items[0].pseudo, ok[i].pe);
    }
    /* a list takes each item's own pseudo-element, and the compound before it keeps its classes */
    const embi_sel_list *l = parse(&a, "::-webkit-scrollbar-track, ::-webkit-scrollbar-corner", nullptr);
    EMB_REQUIRE(l != nullptr && l->n == 2);
    EMB_CHECK_EQ_INT(l->items[0].pseudo, EMBI_PE_SCROLLBAR_TRACK);
    EMB_CHECK_EQ_INT(l->items[1].pseudo, EMBI_PE_SCROLLBAR_CORNER);
    l = parse(&a, "input[type=range]:hover::-webkit-slider-thumb", nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_CHECK_EQ_INT(l->items[0].comp[0].n, 3); /* input, [type=range], :hover */
    /* single-colon forms are not accepted, nor a pseudo-element inside :is() or a structural class after one */
    EMB_CHECK(parse(&a, "input:-webkit-slider-thumb", nullptr) == nullptr);
    EMB_CHECK(parse(&a, "::-webkit-scrollbar:first-child", nullptr) == nullptr);
    EMB_CHECK(parse(&a, "::-webkit-scrollbar::before", nullptr) == nullptr);
    embi_arena_free(&a);
}

EMB_TEST(sel_compound_structure)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = parse(&a, "div#x.y > p[z]:hover", nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_REQUIRE(l->n == 1);
    const embi_sel_complex *c = &l->items[0];
    EMB_REQUIRE(c->nc == 2);
    EMB_CHECK_EQ_INT(c->comb[0], EMBI_SC_NONE);
    EMB_CHECK_EQ_INT(c->comb[1], EMBI_SC_CHILD);
    EMB_CHECK_EQ_INT(c->comp[0].n, 3);
    EMB_CHECK_EQ_INT(c->comp[1].n, 3);
    EMB_CHECK_EQ_INT(c->comp[0].ss[0].kind, EMBI_SS_TYPE);
    EMB_CHECK(embi_str_eq(embi_atom_str(c->comp[0].ss[0].name), S("div")));
    EMB_CHECK_EQ_INT(c->comp[0].ss[1].kind, EMBI_SS_ID);
    EMB_CHECK_EQ_INT(c->comp[0].ss[2].kind, EMBI_SS_CLASS);
    EMB_CHECK_EQ_INT(c->comp[1].ss[0].kind, EMBI_SS_TYPE);
    EMB_CHECK_EQ_INT(c->comp[1].ss[1].kind, EMBI_SS_ATTR);
    EMB_CHECK_EQ_INT(c->comp[1].ss[1].sub, EMBI_AO_EXISTS);
    EMB_CHECK_EQ_INT(c->comp[1].ss[2].kind, EMBI_SS_PC);
    embi_arena_free(&a);
}

EMB_TEST(sel_attribute_operators_and_flags)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = parse(&a, "[a~=\"x y\" i][b|=c s][d^=e][f$=g][h*=i][j=k]", nullptr);
    /* "x y" with ~= is valid as a string; it just never matches. */
    EMB_REQUIRE(l != nullptr);
    const embi_sel_simple *ss = l->items[0].comp[0].ss;
    EMB_CHECK_EQ_INT(ss[0].sub, EMBI_AO_WORD);
    EMB_CHECK((ss[0].flags & EMBI_SAF_CI) != 0);
    EMB_CHECK_EQ_INT(ss[1].sub, EMBI_AO_DASH);
    EMB_CHECK((ss[1].flags & EMBI_SAF_CS) != 0);
    EMB_CHECK_EQ_INT(ss[2].sub, EMBI_AO_PREFIX);
    EMB_CHECK_EQ_INT(ss[3].sub, EMBI_AO_SUFFIX);
    EMB_CHECK_EQ_INT(ss[4].sub, EMBI_AO_SUBSTR);
    EMB_CHECK_EQ_INT(ss[5].sub, EMBI_AO_EQ);
    EMB_CHECK(embi_str_eq(ss[5].value, S("k")));
    /* "j" is not in the HTML case-insensitive attribute list. */
    EMB_CHECK((ss[5].flags & EMBI_SAF_HTML_CI) == 0);
    const embi_sel_list *t = parse(&a, "[TYPE=a]", nullptr);
    EMB_REQUIRE(t != nullptr);
    EMB_CHECK((t->items[0].comp[0].ss[0].flags & EMBI_SAF_HTML_CI) != 0);
    embi_arena_free(&a);
}

EMB_TEST(sel_namespaces)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    embi_atom svg_uri = embi_atom_intern("http://www.w3.org/2000/svg", 26);
    embi_atom html_uri = embi_atom_intern("http://www.w3.org/1999/xhtml", 28);
    embi_atom svg_prefix = embi_atom_intern("svg", 3);
    embi_sel_nsmap maps[] = {{svg_prefix, svg_uri}, {0, html_uri}};
    embi_sel_env env = {maps, 2, nullptr};

    const embi_sel_list *l = parse(&a, "svg|rect", &env);
    EMB_REQUIRE(l != nullptr);
    EMB_CHECK_EQ_INT(l->items[0].comp[0].ss[0].nsk, EMBI_NSK_URI);
    EMB_CHECK(l->items[0].comp[0].ss[0].ns_uri == svg_uri);

    /* No prefix takes the default namespace. */
    const embi_sel_list *d = parse(&a, "div", &env);
    EMB_REQUIRE(d != nullptr);
    EMB_CHECK_EQ_INT(d->items[0].comp[0].ss[0].nsk, EMBI_NSK_URI);
    EMB_CHECK(d->items[0].comp[0].ss[0].ns_uri == html_uri);

    /* Attributes never take the default namespace. */
    const embi_sel_list *at = parse(&a, "[x]", &env);
    EMB_REQUIRE(at != nullptr);
    EMB_CHECK_EQ_INT(at->items[0].comp[0].ss[1].nsk, EMBI_NSK_NONE);

    /* A compound without a type selector gets the implicit default-namespace universal. */
    const embi_sel_list *cl = parse(&a, ".a", &env);
    EMB_REQUIRE(cl != nullptr);
    EMB_CHECK_EQ_INT(cl->items[0].comp[0].n, 2);
    EMB_CHECK_EQ_INT(cl->items[0].comp[0].ss[0].kind, EMBI_SS_UNIVERSAL);
    EMB_CHECK_EQ_INT(cl->items[0].comp[0].ss[0].nsk, EMBI_NSK_URI);
    EMB_CHECK(cl->items[0].comp[0].ss[0].ns_uri == html_uri);
    EMB_CHECK_EQ_INT(parse(&a, ".a", nullptr)->items[0].comp[0].n, 1);

    EMB_CHECK_EQ_INT(parse(&a, "foo|rect", &env) == nullptr, 1);
    EMB_CHECK_EQ_INT(parse(&a, "*|rect", &env) != nullptr, 1);
    EMB_CHECK_EQ_INT(parse(&a, "|rect", &env) != nullptr, 1);
    EMB_CHECK_EQ_INT(parse(&a, "[*|x]", &env) != nullptr, 1);
    embi_arena_free(&a);
}

/* The number of compounds in a complex selector is capped at 64. */
EMB_TEST(sel_compound_cap)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    static char buf[1024];
    for (int n = 63; n <= 66; n++) {
        size_t len = 0;
        for (int k = 0; k < n; k++) {
            len += (size_t)snprintf(buf + len, sizeof buf - len, k ? " + i" : "i");
        }
        const embi_sel_list *l = embi_sel_parse_text(&a, (embi_str){buf, (uint32_t)len}, nullptr);
        EMB_CHECK_EQ_INT(l != nullptr, n <= 64);
    }
    embi_arena_free(&a);
}

EMB_TEST(sel_nesting_ampersand)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *parent = parse(&a, "section.card", nullptr);
    EMB_REQUIRE(parent != nullptr);

    embi_sel_env env = {nullptr, 0, parent};
    const embi_sel_list *l = parse(&a, "& > h2", &env);
    EMB_REQUIRE(l != nullptr);
    EMB_CHECK_EQ_INT(l->items[0].comp[0].ss[0].kind, EMBI_SS_IS);
    EMB_CHECK(l->items[0].comp[0].ss[0].list == parent);

    /* Without a parent, & is :scope. */
    const embi_sel_list *top = parse(&a, "&", nullptr);
    EMB_REQUIRE(top != nullptr);
    EMB_CHECK_EQ_INT(top->items[0].comp[0].ss[0].kind, EMBI_SS_PC);
    EMB_CHECK_EQ_INT(top->items[0].comp[0].ss[0].sub, EMBI_PC_SCOPE);
    embi_arena_free(&a);
}

EMB_TEST(sel_relative_lists)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    embi_css_slice in = embi_css_parse_cvs(&a, S("> a b, + c, d"));
    const embi_sel_list *l = embi_sel_parse_relative(&a, in, nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_REQUIRE(l->n == 3);
    EMB_CHECK(l->relative);
    EMB_CHECK_EQ_INT(l->items[0].comb[0], EMBI_SC_CHILD);
    EMB_CHECK_EQ_INT(l->items[1].comb[0], EMBI_SC_NEXT);
    EMB_CHECK_EQ_INT(l->items[2].comb[0], EMBI_SC_DESC);
    EMB_CHECK_EQ_INT(l->items[0].nc, 2);

    /* A bare combinator, or one not followed by a compound, is invalid. */
    EMB_CHECK(embi_sel_parse_relative(&a, embi_css_parse_cvs(&a, S(">")), nullptr) == nullptr);
    embi_arena_free(&a);
}

EMB_TEST(sel_bloom_requirements)
{
    embi_arena a;
    embi_arena_init(&a, 1024);
    /* div > p#x .y: div, p and #x are all strict ancestors of the subject .y. */
    const embi_sel_list *l = parse(&a, "div > p#x .y", nullptr);
    EMB_REQUIRE(l != nullptr);
    const embi_sel_complex *c = &l->items[0];
    EMB_CHECK_EQ_INT(c->nreq, 3);
    /* A sibling combinator breaks the chain: nothing to the left of it is required. */
    const embi_sel_list *s = parse(&a, "p#x + q .y", nullptr);
    EMB_REQUIRE(s != nullptr);
    EMB_CHECK_EQ_INT(s->items[0].nreq, 1);
    embi_arena_free(&a);
}

/* After a pseudo-element, only user-action pseudo-classes may follow in the same compound. */
EMB_TEST(sel_pseudo_element_only_user_actions_follow)
{
    static const char *const bad[] = {
        "a::before.b",     "a::before#x",     "a::before[x]",    "a::before:first-child",
        "::before:not(.x)", "a::before:is(.x)", "a::before:hover.b", "a::before&",
        "::before:nth-child(1)", "a::before:first-of-type",
    };
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        if (valid(bad[k])) {
            SDL_Log("    should be invalid: \"%s\"", bad[k]);
            EMB_CHECK(false);
        }
    }

    static const char *const good[] = {
        "::before:hover",       "a::before:focus-visible", "a::before:active",
        "p::after:focus-within", "p:hover::before",         "input::placeholder:focus",
    };
    for (size_t k = 0; k < sizeof good / sizeof good[0]; k++) {
        if (!valid(good[k])) {
            SDL_Log("    should be valid: \"%s\"", good[k]);
            EMB_CHECK(false);
        }
    }

    embi_arena a;
    embi_arena_init(&a, 1024);
    const embi_sel_list *l = parse(&a, "::before:hover", nullptr);
    EMB_REQUIRE(l != nullptr);
    EMB_CHECK(l->items[0].pseudo == EMBI_PE_BEFORE);
    /* The pseudo-element adds no simple selector; the user-action class is the only one. */
    EMB_CHECK(l->items[0].comp[0].n == 1);
    EMB_CHECK(l->items[0].comp[0].ss[0].kind == EMBI_SS_PC);
    EMB_CHECK(l->items[0].comp[0].ss[0].sub == EMBI_PC_HOVER);
    embi_arena_free(&a);
}

/* Untrusted, deeply nested input must not blow the stack: past the depth cap the rule is invalid. */
EMB_TEST(sel_nesting_depth_is_capped)
{
    static const char *const fns[] = {":is(", ":where(", ":not(", ":has(", ":nth-child(2 of "};
    for (size_t f = 0; f < sizeof fns / sizeof fns[0]; f++) {
        for (int closed = 0; closed < 2; closed++) {
            enum { DEPTH = 10000 };
            size_t fl = strlen(fns[f]);
            size_t cap = (size_t)DEPTH * (fl + 1) + 8;
            char *buf = embi_alloc(cap);
            size_t n = 0;
            for (int k = 0; k < DEPTH; k++) {
                memcpy(buf + n, fns[f], fl);
                n += fl;
            }
            buf[n++] = 'a';
            for (int k = 0; closed && k < DEPTH; k++) {
                buf[n++] = ')';
            }
            embi_arena a;
            embi_arena_init(&a, 4096);
            const embi_sel_list *l = embi_sel_parse_text(&a, (embi_str){buf, (uint32_t)n}, nullptr);
            EMB_CHECK(l == nullptr);
            embi_arena_free(&a);
            embi_free(buf, cap);
        }
    }
}

/* A moderate depth is accepted, one past the cap is not, also inside a forgiving :is(). */
EMB_TEST(sel_nesting_depth_boundary)
{
    char buf[512];
    for (int depth = 1; depth <= 40; depth++) {
        size_t n = 0;
        for (int k = 0; k < depth; k++) {
            memcpy(buf + n, ":is(", 4);
            n += 4;
        }
        buf[n++] = 'a';
        for (int k = 0; k < depth; k++) {
            buf[n++] = ')';
        }
        buf[n] = 0;
        embi_arena a;
        embi_arena_init(&a, 1024);
        const embi_sel_list *l = parse(&a, buf, nullptr);
        EMB_CHECK((l != nullptr) == (depth <= 32));
        embi_arena_free(&a);
        char wrap[600];
        memcpy(wrap, "x:is(", 5);
        memcpy(wrap + 5, buf, n);
        wrap[5 + n] = ')';
        wrap[6 + n] = 0;
        EMB_CHECK(valid(wrap) == (depth + 1 <= 32));
    }
}

EMB_TEST_MAIN()
