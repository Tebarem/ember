/*
 * Robustness of the value parsers (W2c): regression inputs that once dereferenced a missing
 * component value, a deterministic random-token fuzz over every property that asserts nothing
 * crashes (run under ASan in CI), registered custom property syntax (multipliers, <image>,
 * <transform-function>, <transform-list>, <custom-ident>, case-sensitive keywords, empty values for
 * "*"), and the properties added for the scroll-snap, fragmentation, clip and text-emphasis
 * families.
 */
#include "emb_test.h"

#include "css/props.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static embi_css_slice text_of(embi_arena *a, const char *s)
{
    return embi_css_parse_cvs(a, (embi_str){s, (uint32_t)strlen(s)});
}

static embi_prop_id prop_of(const char *name)
{
    return embi_prop_lookup((embi_str){name, (uint32_t)strlen(name)});
}

static uint32_t decl(embi_arena *a, const char *prop, const char *value, embi_longhand *out)
{
    return embi_css_parse_decl(a, prop_of(prop), text_of(a, value), out, EMBI_LONGHAND_MAX);
}

static bool reg(embi_arena *a, const char *syntax, const char *value, embi_value *v)
{
    return embi_css_registered_parse(a, (embi_str){syntax, (uint32_t)strlen(syntax)},
                                     text_of(a, value), v);
}

/* ------------------------------------------------------------------ regressions */

EMB_TEST(truncated_values_do_not_crash)
{
    static const struct {
        const char *prop, *value;
        bool valid;
    } cases[] = {
        {"font-variation-settings", "\"wght\"", false},
        {"font-variation-settings", "\"M0 0\"", false},
        {"font-variation-settings", "\"wght\" 400", true},
        {"contain-intrinsic-width", "auto", false},
        {"contain-intrinsic-height", "auto", false},
        {"contain-intrinsic-inline-size", "auto", false},
        {"contain-intrinsic-block-size", "auto", false},
        {"contain-intrinsic-size", "auto", false},
        {"contain-intrinsic-width", "auto 10px", true},
        {"rotate", "0", true},
        {"rotate", "3", false},
        {"rotate", "1.5", false},
        {"rotate", "45deg 3", false},
        {"rotate", "0 0 1", false},
        {"rotate", "1 0", false},
        {"rotate", "x", false},
        {"rotate", "45deg", true},
        {"rotate", "x 45deg", true},
        {"rotate", "0 0 1 45deg", true},
        {"rotate", "45deg 0 0 1", true},
    };
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand out[EMBI_LONGHAND_MAX];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t n = decl(&a, cases[i].prop, cases[i].value, out);
        if ((n != 0) != cases[i].valid) {
            fprintf(stderr, "%s: %s -> %u (expected %s)\n", cases[i].prop, cases[i].value, n,
                    cases[i].valid ? "valid" : "invalid");
        }
        EMB_CHECK((n != 0) == cases[i].valid);
    }
    /* The one-component entry points reject a missing component value. */
    embi_value v;
    EMB_CHECK(!embi_value_parse_types(&a, EMBI_T_NUM, nullptr, &v));
    EMB_CHECK(!embi_value_parse_one(&a, prop_of("width"), nullptr, &v));
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ fuzz */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng_state >> 33) % n;
}

EMB_TEST(random_tokens_never_crash_any_property)
{
    static const char *const vocab[] = {
        "auto", "none", "0", "1", "-1", "3", "1.5", "10px", "50%", "45deg", "2s", "red", "#fff",
        "\"s\"", "\"M0 0\"", "url(a.png)", "x", "y", "z", "/", ",", "span", "inherit", "initial",
        "calc(1px + 2%)", "rgb(1 2 3)", "rgb(1, 2", "linear-gradient(red, blue)", "rotate(1turn)",
        "translate(1px, 2px)", "var(--x)", "(", ")", "[a b]", "repeat(2, 1fr)", "minmax(0, 1fr)",
        "normal", "bold", "serif", "fit-content(10px)", "rect(1px, 2px, 3px, 4px)", "filled",
        "circle", "!", "1fr", "e", "--x", "image-set(\"a.png\" 1x)", "attr(a)", "env(x)", "&",
    };
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand out[EMBI_LONGHAND_MAX];
    char buf[512];
    for (uint32_t id = 1; id < EMBI_PROP_COUNT; id++) {
        for (uint32_t round = 0; round < 64; round++) {
            uint32_t nt = rnd(7);
            size_t len = 0;
            buf[0] = '\0';
            for (uint32_t k = 0; k < nt; k++) {
                const char *t = vocab[rnd(sizeof vocab / sizeof vocab[0])];
                len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", k ? " " : "", t);
            }
            embi_css_slice s = text_of(&a, buf);
            (void)embi_css_parse_decl(&a, (embi_prop_id)id, s, out, EMBI_LONGHAND_MAX);
            if ((round & 15) == 0) {
                embi_arena_reset(&a);
            }
        }
        embi_arena_reset(&a);
    }
    embi_arena_free(&a);
    EMB_CHECK(true);
}

/* ------------------------------------------------------------------ registered syntax */

EMB_TEST(registered_multipliers)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v;
    EMB_CHECK(reg(&a, "<length>+", "1px 2px 3px", &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_LIST);
    EMB_CHECK_EQ_INT(v.list.n, 3);
    EMB_CHECK_EQ_INT(v.sep, EMBI_SEP_SPACE);
    EMB_CHECK(reg(&a, "<length>+", "1px", &v));
    EMB_CHECK(!reg(&a, "<length>+", "1px, 2px", &v));
    EMB_CHECK(!reg(&a, "<length>+", "1px red", &v));
    EMB_CHECK(!reg(&a, "<length>", "1px 2px", &v));
    EMB_CHECK(reg(&a, "<number>#", "1, 2.5,3", &v));
    EMB_CHECK_EQ_INT(v.list.n, 3);
    EMB_CHECK_EQ_INT(v.sep, EMBI_SEP_COMMA);
    EMB_CHECK(!reg(&a, "<number>#", "1 2", &v));
    EMB_CHECK(!reg(&a, "<number>#", "1,", &v));
    EMB_CHECK(!reg(&a, "<number>#", ",1", &v));
    EMB_CHECK(reg(&a, "<length>+ | auto", "auto", &v));
    EMB_CHECK(reg(&a, "<length>+ | auto", "1px 2px", &v));
    EMB_CHECK(!reg(&a, "<length>+ | auto", "auto 1px", &v));
    EMB_CHECK(!reg(&a, "<length>+", "", &v));
    embi_arena_free(&a);
}

EMB_TEST(registered_types)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v;
    EMB_CHECK(reg(&a, "<image>", "url(a.png)", &v));
    EMB_CHECK(reg(&a, "<image>", "linear-gradient(red, blue)", &v));
    EMB_CHECK(!reg(&a, "<image>", "red", &v));
    EMB_CHECK(reg(&a, "<image>#", "url(a.png), linear-gradient(red, blue)", &v));
    EMB_CHECK_EQ_INT(v.list.n, 2);
    EMB_CHECK(reg(&a, "<transform-function>", "rotate(45deg)", &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_TRANSFORM);
    EMB_CHECK(!reg(&a, "<transform-function>", "rotate(45deg) scale(2)", &v));
    EMB_CHECK(!reg(&a, "<transform-function>", "bogus(1)", &v));
    EMB_CHECK(!reg(&a, "<transform-function>", "10px", &v));
    EMB_CHECK(reg(&a, "<transform-list>", "rotate(45deg) scale(2)", &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_TRANSFORM);
    EMB_CHECK_EQ_INT(v.xf.n, 2);
    EMB_CHECK(!reg(&a, "<transform-list>", "rotate(45deg) 3", &v));
    EMB_CHECK(!reg(&a, "<transform-list>+", "rotate(45deg)", &v));
    EMB_CHECK(reg(&a, "<transform-function>+", "rotate(45deg) scale(2)", &v));
    EMB_CHECK_EQ_INT(v.list.n, 2);
    EMB_CHECK(reg(&a, "<custom-ident>", "foo", &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_IDENT);
    EMB_CHECK(!reg(&a, "<custom-ident>", "inherit", &v));
    EMB_CHECK(!reg(&a, "<custom-ident>", "foo bar", &v));
    EMB_CHECK(!reg(&a, "<custom-ident>", "1px", &v));
    EMB_CHECK(reg(&a, "<custom-ident>+", "foo bar baz", &v));
    EMB_CHECK_EQ_INT(v.list.n, 3);
    EMB_CHECK(reg(&a, "<custom-ident>#", "a, b", &v));
    EMB_CHECK(!reg(&a, "<custom-ident>+", "foo initial", &v));
    embi_arena_free(&a);
}

EMB_TEST(registered_keywords_are_case_sensitive)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v;
    EMB_CHECK(reg(&a, "Auto | foo", "Auto", &v));
    EMB_CHECK(!reg(&a, "Auto | foo", "auto", &v));
    EMB_CHECK(!reg(&a, "Auto | foo", "AUTO", &v));
    EMB_CHECK(reg(&a, "Auto | foo", "foo", &v));
    EMB_CHECK(!reg(&a, "foo", "foo bar", &v));
    /* Data type names stay case-insensitive. */
    EMB_CHECK(reg(&a, "<Length>", "1px", &v));
    embi_arena_free(&a);
}

EMB_TEST(registered_universal_accepts_empty)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v;
    EMB_CHECK(reg(&a, "*", "", &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_RAW);
    EMB_CHECK_EQ_INT(v.raw.n, 0);
    EMB_CHECK(reg(&a, "*", "   ", &v));
    EMB_CHECK(reg(&a, " * ", "a b", &v));
    EMB_CHECK(!reg(&a, "<length>", "", &v));
    EMB_CHECK(!reg(&a, "<length> | auto", "  ", &v));
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ added properties */

EMB_TEST(added_properties_parse)
{
    static const struct {
        const char *prop, *value;
        bool valid;
    } cases[] = {
        {"clip", "auto", true},
        {"clip", "rect(1px, 2px, 3px, 4px)", true},
        {"clip", "rect(1px 2px 3px 4px)", true},
        {"clip", "rect(auto, 2px, auto, 4px)", true},
        {"clip", "rect(1px, 2px, 3px)", false},
        {"clip", "rect(1px 2px, 3px 4px)", false},
        {"clip", "rect(1px, , 3px, 4px)", false},
        {"clip", "rect(1px, 2px, 3px, 4px, 5px)", false},
        {"clip", "rect(1, 2px, 3px, 4px)", false},
        {"scroll-margin", "1px 2px", true},
        {"scroll-margin", "10%", false},
        {"scroll-margin-top", "4px", true},
        {"scroll-margin-block", "1px 2px", true},
        {"scroll-margin-inline-start", "1px", true},
        {"scroll-padding", "auto", true},
        {"scroll-padding", "10% 2px 3px 4px", true},
        {"scroll-padding-top", "-1px", false},
        {"scroll-padding-inline", "auto 5px", true},
        {"orphans", "3", true},
        {"widows", "2", true},
        {"orphans", "1.5", false},
        {"orphans", "0", false},
        {"orphans", "1", true},
        {"widows", "0", false},
        {"widows", "-2", false},
        {"widows", "1", true},
        {"column-count", "0", false},
        {"column-count", "1", true},
        {"break-before", "page", true},
        {"break-after", "avoid-column", true},
        {"break-inside", "avoid", true},
        {"break-inside", "page", false},
        {"animation-timeline", "auto", true},
        {"animation-timeline", "none, foo", true},
        {"text-emphasis-style", "filled circle", true},
        {"text-emphasis-style", "sesame", true},
        {"text-emphasis-style", "\"x\"", true},
        {"text-emphasis-style", "filled open", false},
        {"text-emphasis-style", "none", true},
        {"text-emphasis-color", "red", true},
        {"text-emphasis-position", "over right", true},
        {"text-emphasis-position", "under", true},
        {"text-emphasis", "filled circle red", true},
        {"text-emphasis", "red", true},
        {"text-emphasis", "none", true},
        {"text-emphasis", "\"x\" blue", true},
        {"text-emphasis", "filled filled", false},
    };
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand out[EMBI_LONGHAND_MAX];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t n = decl(&a, cases[i].prop, cases[i].value, out);
        if ((n != 0) != cases[i].valid) {
            fprintf(stderr, "%s: %s -> %u (expected %s)\n", cases[i].prop, cases[i].value, n,
                    cases[i].valid ? "valid" : "invalid");
        }
        EMB_CHECK((n != 0) == cases[i].valid);
    }
    EMB_CHECK_EQ_INT(decl(&a, "scroll-margin", "1px 2px", out), 4);
    EMB_CHECK_EQ_INT(decl(&a, "text-emphasis", "filled circle red", out), 2);
    /* Logical/physical mapping covers the new families. */
    EMB_CHECK(embi_prop_is_logical(prop_of("scroll-margin-inline-start")));
    EMB_CHECK(!embi_prop_is_logical(prop_of("scroll-padding-left")));
    EMB_CHECK_EQ_INT(embi_prop_to_physical(prop_of("scroll-padding-block-start"), EMBI_KW_horizontal_tb, EMBI_KW_ltr),
                     prop_of("scroll-padding-top"));
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ var() work bound and fuzz */

#define VT_MAX 40

typedef struct vtab {
    char name[VT_MAX][16];
    embi_css_slice val[VT_MAX];
    uint32_t n;
} vtab;

static bool vtab_lookup(void *user, embi_str name, embi_css_slice *value)
{
    const vtab *t = user;
    for (uint32_t i = 0; i < t->n; i++) {
        if (strlen(t->name[i]) == name.len && memcmp(t->name[i], name.ptr, name.len) == 0) {
            *value = t->val[i];
            return true;
        }
    }
    return false;
}

static void vtab_add(embi_arena *a, vtab *t, const char *name, const char *value)
{
    snprintf(t->name[t->n], sizeof t->name[0], "%s", name);
    t->val[t->n] = text_of(a, value);
    t->n++;
}

/* Wall-clock bound for one substitution; generous so that only unbounded work fails. */
static double time_limit_s(void)
{
    return EMB_TEST_ASAN ? 10.0 : 2.0;
}

static void doubling_graph(bool spaced)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    vtab t = {0};
    vtab_add(&a, &t, "--a0", "");
    for (int k = 1; k <= 25; k++) {
        char name[16], prev[16], val[64];
        snprintf(name, sizeof name, "--a%d", k);
        snprintf(prev, sizeof prev, "--a%d", k - 1);
        snprintf(val, sizeof val, "var(%s)%svar(%s)", prev, spaced ? " " : "", prev);
        vtab_add(&a, &t, name, val);
    }
    embi_var_env env = {.user = &t, .lookup = vtab_lookup, .max_tokens = 0};
    embi_css_slice out;
    clock_t t0 = clock();
    /* 2^25 expansions of nothing: guaranteed-invalid, quickly. */
    EMB_CHECK(!embi_css_substitute_vars(&a, &env, (embi_str){nullptr, 0}, text_of(&a, "var(--a25)"),
                                        &out));
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (dt >= time_limit_s()) {
        fprintf(stderr, "doubling expansion took %.3f s\n", dt);
    }
    EMB_CHECK(dt < time_limit_s());
    /* A small level of the same graph is within budget and expands to (whitespace and) nothing. */
    EMB_CHECK(embi_css_substitute_vars(&a, &env, (embi_str){nullptr, 0}, text_of(&a, "var(--a8)"),
                                       &out));
    if (!spaced) {
        EMB_CHECK_EQ_INT(out.n, 0);
    }
    embi_arena_free(&a);
}

EMB_TEST(var_empty_doubling_is_bounded)
{
    doubling_graph(false);
}

EMB_TEST(var_empty_doubling_spaced_is_bounded)
{
    doubling_graph(true);
}

EMB_TEST(var_fuzz_random_graphs_are_bounded)
{
    static const char *const pieces[] = {
        "", "x", "1px", "var(--v%d)", "var(--v%d)var(--v%d)", "var(--v%d) var(--v%d)",
        "var(--v%d, fb)", "var(--v%d,)", "f(var(--v%d))", "(var(--v%d) var(--v%d))",
        "var(--missing)", "var(--missing, var(--v%d))", "var(--v%d)var(--v%d)var(--v%d)",
        "var(", "var(--v%d", "var()", "var(1)", "[var(--v%d)]",
    };
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    clock_t t0 = clock();
    for (uint32_t round = 0; round < 300; round++) {
        vtab t = {0};
        uint32_t nv = 1 + rnd(12);
        for (uint32_t i = 0; i < nv; i++) {
            char name[16], val[160];
            snprintf(name, sizeof name, "--v%u", i);
            const char *pc = pieces[rnd(sizeof pieces / sizeof pieces[0])];
            /* Mostly doubling shapes so that deep graphs get exercised. */
            if (rnd(3) == 0 && i > 0) {
                pc = rnd(2) ? "var(--v%d)var(--v%d)" : "var(--v%d) var(--v%d)";
            }
            uint32_t j = i > 0 && rnd(4) != 0 ? i - 1 : rnd(nv);
            snprintf(val, sizeof val, pc, j, j, j);
            vtab_add(&a, &t, name, val);
        }
        char top[64], self[16];
        snprintf(top, sizeof top, "var(--v%u)", rnd(nv));
        snprintf(self, sizeof self, "--v%u", rnd(nv));
        static const uint32_t limits[] = {0, 8, 100, 4096};
        embi_var_env env = {.user = &t, .lookup = vtab_lookup, .max_tokens = limits[rnd(4)]};
        embi_css_slice out;
        bool use_self = rnd(2) != 0;
        clock_t r0 = clock();
        bool ok = embi_css_substitute_vars(&a, &env,
                                           use_self ? (embi_str){self, (uint32_t)strlen(self)}
                                                    : (embi_str){nullptr, 0},
                                           text_of(&a, top), &out);
        double dt = (double)(clock() - r0) / CLOCKS_PER_SEC;
        if (dt >= time_limit_s()) {
            fprintf(stderr, "round %u took %.3f s (top %s)\n", round, dt, top);
        }
        EMB_CHECK(dt < time_limit_s());
        if (ok && env.max_tokens) {
            EMB_CHECK(out.n <= env.max_tokens);
        }
        embi_arena_reset(&a);
    }
    double total = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EMB_CHECK(total < 4 * time_limit_s());
    embi_arena_free(&a);
}

EMB_TEST(registered_fuzz_random_syntax_is_bounded)
{
    static const char *const syn[] = {
        "*", "<length>", "<number>", "<integer>", "<percentage>", "<length-percentage>",
        "<color>", "<angle>", "<time>", "<resolution>", "<url>", "<string>", "<image>",
        "<custom-ident>", "<transform-function>", "<transform-list>", "<length>+", "<color>#",
        "<number>#", "<transform-list>+", "<image>#", "|", " | ", "foo", "bar", "auto", "<", ">",
        "<>", "<<length>>", "+", "#", "<length", "length>", "<foo>", "<length>++", "<length>#+",
        "<length>|", "|<length>", "||", "<LENGTH>", " ", "", "\t", "<custom-ident>+", "\n",
    };
    static const char *const val[] = {
        "", "1px", "2", "50%", "red", "#fff", "foo", "bar", "auto", "\"s\"", "url(a.png)", ",",
        "1px, 2px", "1px 2px", "rotate(1turn)", "translate(1px, 2px) scale(2)", "calc(1px + 2%)",
        "linear-gradient(red, blue)", "45deg", "2s", "1dppx", "inherit", "var(--x)", "(", ")",
        "{", "[a]", "!", "1.5", "-3", "x y z", "a, b, c",
    };
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    clock_t t0 = clock();
    for (uint32_t round = 0; round < 6000; round++) {
        char sbuf[256], vbuf[256];
        size_t sl = 0, vl = 0;
        sbuf[0] = vbuf[0] = '\0';
        uint32_t ns = 1 + rnd(5), nv = rnd(6);
        for (uint32_t k = 0; k < ns; k++) {
            sl += (size_t)snprintf(sbuf + sl, sizeof sbuf - sl, "%s%s", k && rnd(2) ? " | " : "",
                                   syn[rnd(sizeof syn / sizeof syn[0])]);
        }
        for (uint32_t k = 0; k < nv; k++) {
            vl += (size_t)snprintf(vbuf + vl, sizeof vbuf - vl, "%s%s", k ? " " : "",
                                   val[rnd(sizeof val / sizeof val[0])]);
        }
        embi_value v;
        (void)reg(&a, sbuf, vbuf, &v);
        if ((round & 63) == 0) {
            embi_arena_reset(&a);
        }
    }
    double total = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EMB_CHECK(total < 4 * time_limit_s());
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
