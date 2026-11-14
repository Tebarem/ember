/*
 * Property coverage and computed-value helpers (W2c): every mandated property parses with a
 * representative value, opaque grammars reject malformed tokens, var() substitution handles
 * fallback, cycles and the token limit, registered syntax matches, relative lengths resolve through
 * the W3 hook, and system colors come from the palette.
 */
#include "emb_test.h"

#include "css/props.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------------ property coverage */

typedef struct sample {
    const char *prop;
    const char *value;
} sample;

/* One representative valid value per mandated property (longhands, shorthands, logical
 * variants). Typed grammars and opaque grammars are both covered. */
static const sample samples[] = {
    {"border-top-left-radius", "10px"},
    {"border-start-start-radius", "4px"},
    {"border-block-start-width", "2px"},
    {"border-inline-end-color", "red"},
    {"border-block-start", "1px solid red"},
    {"border-block-end", "thin dotted"},
    {"border-inline-start", "2px dashed blue"},
    {"border-inline", "1px solid"},
    {"border-block", "1px solid red"},
    {"border-image", "url(a.png) 30 round"},
    {"border-image-source", "linear-gradient(red, blue)"},
    {"border-image-slice", "30 fill"},
    {"border-image-width", "1 2"},
    {"border-image-outset", "2"},
    {"border-image-repeat", "round space"},
    {"background", "url(a.png) no-repeat center / cover red"},
    {"background-image", "linear-gradient(red, blue), url(b.png)"},
    {"background-position", "left 10px top 5px"},
    {"background-position-x", "center"},
    {"background-position-y", "bottom 4px"},
    {"background-size", "cover"},
    {"background-size", "100px 50%"},
    {"background-repeat", "repeat-x"},
    {"background-attachment", "fixed"},
    {"background-clip", "text"},
    {"background-origin", "content-box"},
    {"background-blend-mode", "multiply, screen"},
    {"font", "italic bold 12px/1.5 serif"},
    {"font-variant", "small-caps"},
    {"font-variant-caps", "all-small-caps"},
    {"font-variant-ligatures", "common-ligatures no-discretionary-ligatures"},
    {"font-variant-numeric", "tabular-nums oldstyle-nums"},
    {"font-variant-east-asian", "jis78"},
    {"font-variant-position", "super"},
    {"font-variant-alternates", "historical-forms"},
    {"font-variant-emoji", "emoji"},
    {"font-synthesis", "weight style"},
    {"font-synthesis-weight", "none"},
    {"font-synthesis-style", "auto"},
    {"font-synthesis-small-caps", "none"},
    {"font-feature-settings", "\"liga\" 1"},
    {"font-variation-settings", "\"wght\" 400"},
    {"font-optical-sizing", "none"},
    {"font-palette", "dark"},
    {"text-shadow", "1px 1px 2px red, 0 0 1em blue"},
    {"text-underline-offset", "auto"},
    {"text-underline-offset", "3px"},
    {"text-underline-position", "under left"},
    {"text-decoration-skip-ink", "all"},
    {"text-overflow", "clip ellipsis"},
    {"text-overflow", "\"…\""},
    {"text-orientation", "upright"},
    {"text-wrap", "balance"},
    {"text-wrap", "nowrap pretty"},
    {"text-wrap-mode", "nowrap"},
    {"text-wrap-style", "balance"},
    {"overflow-clip-margin", "content-box 2px"},
    {"list-style", "square inside"},
    {"list-style-image", "url(bullet.png)"},
    {"list-style-image", "none"},
    {"counter-reset", "section 2 item"},
    {"counter-increment", "section"},
    {"counter-set", "page 1"},
    {"content", "counter(item) \". \""},
    {"content", "\"x\" attr(title)"},
    {"quotes", "\"«\" \"»\" \"‹\" \"›\""},
    {"quotes", "auto"},
    {"object-position", "left 20% bottom"},
    {"columns", "auto 3"},
    {"columns", "200px"},
    {"column-width", "auto"},
    {"column-count", "auto"},
    {"column-fill", "balance-all"},
    {"column-span", "all"},
    {"column-rule", "2px solid red"},
    {"column-rule-width", "thick"},
    {"column-rule-style", "double"},
    {"column-rule-color", "currentcolor"},
    {"overscroll-behavior", "contain none"},
    {"overscroll-behavior-x", "auto"},
    {"transition", "opacity 1s ease-in 200ms"},
    {"transition-property", "opacity, transform"},
    {"transition-duration", "1s, 200ms"},
    {"transition-timing-function", "cubic-bezier(0.1, 0.7, 1, 0.1)"},
    {"transition-timing-function", "steps(4, jump-end)"},
    {"transition-delay", "0s"},
    {"transition-behavior", "allow-discrete"},
    {"animation", "spin 2s linear infinite alternate"},
    {"animation-name", "spin"},
    {"animation-timing-function", "linear(0, 1)"},
    {"animation-timing-function", "ease-out"},
    {"animation-composition", "add"},
    {"animation-iteration-count", "infinite"},
    {"translate", "10px 20%"},
    {"rotate", "45deg"},
    {"rotate", "x 90deg"},
    {"scale", "2 0.5"},
    {"transform", "translate(10px, 20px) rotate(45deg) scale(2)"},
    {"transform-origin", "left top"},
    {"perspective-origin", "20% 50%"},
    {"perspective", "none"},
    {"perspective", "500px"},
    {"filter", "blur(2px) drop-shadow(0 0 2px red)"},
    {"backdrop-filter", "blur(10px)"},
    {"box-shadow", "inset 0 0 0 1px red, 2px 2px 4px blue"},
    {"clip-path", "inset(10px round 5px)"},
    {"clip-path", "polygon(0 0, 100% 0, 50% 100%)"},
    {"mask", "url(m.png) luminance no-repeat center / contain"},
    {"mask-image", "linear-gradient(black, transparent)"},
    {"mask-mode", "alpha, luminance"},
    {"mask-repeat", "no-repeat"},
    {"mask-position", "center"},
    {"mask-size", "contain"},
    {"mask-clip", "no-clip"},
    {"mask-origin", "content-box"},
    {"mask-composite", "exclude"},
    {"mask-type", "alpha"},
    {"mask-border", "linear-gradient(red, blue) 30 round"},
    {"mask-border-source", "none"},
    {"mask-border-slice", "10%"},
    {"mask-border-width", "auto"},
    {"mask-border-outset", "1"},
    {"mask-border-repeat", "space"},
    {"mask-border-mode", "luminance"},
    {"grid-template-columns", "repeat(3, 1fr) 100px"},
    {"grid-template-columns", "[a] 1fr [b]"},
    {"grid-template-rows", "subgrid"},
    {"grid-template-areas", "\"head head\" \"nav main\""},
    {"grid-template", "auto / 1fr 2fr"},
    {"grid-area", "1 / 2 / 3 / 4"},
    {"grid-area", "header"},
    {"grid-row", "1 / span 2"},
    {"grid-column", "2 / -1"},
    {"grid-row-start", "2"},
    {"grid-row-end", "span 3"},
    {"grid-column-start", "auto"},
    {"grid-column-end", "5"},
    {"grid-auto-flow", "dense column"},
    {"grid-auto-rows", "minmax(10px, auto)"},
    {"grid-auto-columns", "1fr"},
    {"grid", "auto-flow / 1fr 1fr"},
    {"gap", "10px 20px"},
    {"row-gap", "normal"},
    {"container", "sidebar / inline-size"},
    {"container-name", "sidebar card"},
    {"container-type", "inline-size"},
    {"cursor", "url(hand.cur) 4 4, pointer"},
    {"cursor", "pointer"},
    {"scroll-snap-stop", "always"},
    {"scroll-snap-type", "x mandatory"},
    {"contain-intrinsic-size", "auto 100px"},
    {"contain-intrinsic-width", "300px"},
    {"contain-intrinsic-height", "none"},
    {"contain-intrinsic-block-size", "auto 50px"},
    {"contain-intrinsic-inline-size", "10em"},
    {"caret-color", "auto"},
    {"accent-color", "auto"},
    {"color-scheme", "light dark"},
    {"will-change", "transform, opacity"},
};

EMB_TEST(every_mandated_property_parses)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        embi_arena_reset(&a);
        embi_longhand lh[EMBI_LONGHAND_MAX];
        uint32_t n = decl(&a, samples[i].prop, samples[i].value, lh);
        if (n == 0) {
            fprintf(stderr, "  %s: \"%s\" did not parse\n", samples[i].prop, samples[i].value);
        }
        EMB_CHECK(n > 0);
    }
    embi_arena_free(&a);
}

/* Every property name in the mandated set exists in the table. */
EMB_TEST(mandated_names_are_known)
{
    static const char *const names[] = {
        "background-attachment", "background-blend-mode", "background-clip", "background-origin",
        "background-position", "background-position-x", "background-position-y", "background-repeat",
        "background-size", "background-image", "border-image", "border-image-source",
        "border-image-slice", "border-image-width", "border-image-outset", "border-image-repeat",
        "text-shadow", "font", "font-variant", "font-variant-ligatures", "font-variant-numeric",
        "font-synthesis", "transition", "animation", "transition-timing-function",
        "animation-timing-function", "translate", "rotate", "scale", "transform-origin",
        "perspective-origin", "grid-area", "grid-template-areas", "overflow-clip-margin",
        "text-overflow", "text-underline-offset", "text-decoration-skip-ink", "counter-reset",
        "counter-increment", "list-style-image", "object-position", "columns", "container",
        "scroll-snap-stop", "mask", "mask-image", "mask-mode", "mask-repeat", "mask-position",
        "mask-clip", "mask-origin", "mask-size", "mask-composite", "mask-type", "mask-border",
        "border-inline-start-width", "border-block-end-color", "border-start-end-radius",
        "border-end-start-radius", "border-end-end-radius", "border-inline-end-style",
        "border-block-start", "border-inline", "border-block-width", "grid-row", "grid-column",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        bool known = prop_of(names[i]) != EMBI_PROP_NONE;
        if (!known) {
            fprintf(stderr, "  missing property %s\n", names[i]);
        }
        EMB_CHECK(known);
    }
}

/* Compound grammars reject malformed tokens and stray closers. */
EMB_TEST(compound_values_reject_malformed_tokens)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_CHECK(decl(&a, "transform", "rotate(45deg)", lh) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_TRANSFORM);
    EMB_CHECK(decl(&a, "transform", "rotate(45deg))", lh) == 0);
    EMB_CHECK(decl(&a, "transform", "url(a b)", lh) == 0);
    EMB_CHECK(decl(&a, "background-image", "url(a b) red", lh) == 0);
    EMB_CHECK(decl(&a, "grid-template-columns", "1fr ]", lh) == 0);
    embi_arena_free(&a);
}

/* Keyword-only typed longhands stay strict. */
EMB_TEST(typed_keywords_stay_strict)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_CHECK(decl(&a, "background-attachment", "sideways", lh) == 0);
    EMB_CHECK(decl(&a, "column-span", "2", lh) == 0);
    EMB_CHECK(decl(&a, "text-decoration-skip-ink", "some", lh) == 0);
    EMB_CHECK(decl(&a, "scroll-snap-stop", "sometimes", lh) == 0);
    EMB_CHECK(decl(&a, "list-style-image", "red", lh) == 0);
    embi_arena_free(&a);
}

/* Serialization of typed compound values. */
EMB_TEST(compound_values_serialize)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    EMB_REQUIRE(decl(&a, "box-shadow", "inset 0 0 0 1px red", lh) == 1);
    embi_sb sb = {0};
    embi_value_serialize(&sb, &lh[0].value);
    EMB_CHECK(sb.len >= 16);
    EMB_CHECK(sb.data && strstr(sb.data, "inset") != nullptr);
    embi_sb_free(&sb);
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ var() */

typedef struct var_table {
    const char *name;
    const char *value;
} var_table;

static const var_table *cur_vars;
static uint32_t cur_nvars;

static embi_arena var_arena;

/* Declares the variables in a table, parsed into the variable arena. */
static bool lookup_var_text(void *user, embi_str name, embi_css_slice *value)
{
    (void)user;
    for (uint32_t i = 0; i < cur_nvars; i++) {
        if (strlen(cur_vars[i].name) == name.len && memcmp(cur_vars[i].name, name.ptr, name.len) == 0) {
            *value = text_of(&var_arena, cur_vars[i].value);
            return true;
        }
    }
    return false;
}

static bool subst_text(embi_arena *a, const char *self, const char *value, embi_css_slice *out,
                       const var_table *vars, uint32_t nvars, uint32_t limit)
{
    cur_vars = vars;
    cur_nvars = nvars;
    embi_var_env env = {.user = nullptr, .lookup = lookup_var_text, .max_tokens = limit};
    embi_str s = self ? (embi_str){self, (uint32_t)strlen(self)} : (embi_str){nullptr, 0};
    return embi_css_substitute_vars(a, &env, s, text_of(a, value), out);
}

EMB_TEST(var_substitution_and_fallback)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_arena_init(&var_arena, 4096);
    static const var_table vars[] = {
        {"--gap", "10px"},
        {"--pad", "var(--gap) 2px"},
    };
    embi_css_slice out;
    EMB_CHECK(subst_text(&a, nullptr, "var(--pad)", &out, vars, 2, 0));
    /* "10px" then " 2px" (the substituted text keeps its whitespace). */
    EMB_CHECK(out.n >= 3);
    EMB_CHECK(subst_text(&a, nullptr, "var(--missing, 7px)", &out, vars, 2, 0));
    EMB_CHECK_EQ_INT(out.n, 1);
    EMB_CHECK(!subst_text(&a, nullptr, "var(--missing)", &out, vars, 2, 0));
    /* An empty fallback is valid; a malformed name is not. */
    EMB_CHECK(subst_text(&a, nullptr, "var(--missing,)", &out, vars, 2, 0));
    EMB_CHECK_EQ_INT(out.n, 0);
    EMB_CHECK(!subst_text(&a, nullptr, "var(gap)", &out, vars, 2, 0));
    /* Substitution through a longhand: the parsed value is a length. */
    embi_longhand lh[EMBI_LONGHAND_MAX];
    cur_vars = vars;
    cur_nvars = 2;
    embi_var_env env = {.user = nullptr, .lookup = lookup_var_text, .max_tokens = 0};
    EMB_CHECK(embi_css_parse_decl_vars(&a, prop_of("margin-top"), text_of(&a, "var(--gap)"),
                                       &env, lh, EMBI_LONGHAND_MAX) == 1);
    EMB_CHECK_EQ_INT(lh[0].value.kind, EMBI_V_LENGTH);
    EMB_CHECK(lh[0].value.num == 10);
    embi_arena_free(&var_arena);
    embi_arena_free(&a);
}

EMB_TEST(var_cycles_are_guaranteed_invalid)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_arena_init(&var_arena, 4096);
    static const var_table vars[] = {
        {"--a", "var(--b)"},
        {"--b", "var(--a)"},
        {"--self", "1px var(--self)"},
        {"--ok", "3px"},
    };
    embi_css_slice out;
    EMB_CHECK(!subst_text(&a, nullptr, "var(--a)", &out, vars, 4, 0));
    /* A self reference is a cycle even with a fallback. */
    EMB_CHECK(!subst_text(&a, "--self", "var(--self, 2px)", &out, vars, 4, 0));
    EMB_CHECK(subst_text(&a, nullptr, "var(--ok)", &out, vars, 4, 0));
    embi_arena_free(&var_arena);
    embi_arena_free(&a);
}

EMB_TEST(var_expansion_is_limited)
{
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    embi_arena_init(&var_arena, 4096);
    /* Each level references the next one twice: 2^k leaves without a limit. */
    static const var_table vars[] = {
        {"--e0", "x"},
        {"--e1", "var(--e0) var(--e0)"},
        {"--e2", "var(--e1) var(--e1)"},
        {"--e3", "var(--e2) var(--e2)"},
        {"--e4", "var(--e3) var(--e3)"},
        {"--e5", "var(--e4) var(--e4)"},
    };
    embi_css_slice out;
    EMB_CHECK(subst_text(&a, nullptr, "var(--e5)", &out, vars, 6, 0));
    /* L(e0) = 1 and L(ek) = 2 * L(ek-1) + 1 (the space between the two references), so L(e5) = 63. */
    EMB_CHECK_EQ_INT(out.n, 63);
    /* With a small limit the same expansion is rejected. */
    EMB_CHECK(!subst_text(&a, nullptr, "var(--e5)", &out, vars, 6, 16));
    embi_arena_free(&var_arena);
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ registered syntax */

EMB_TEST(registered_syntax_matches)
{
    embi_arena a = {0};
    embi_arena_init(&a, 1024);
    embi_value v;
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<length>"), text_of(&a, "10px"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_LENGTH);
    EMB_CHECK(!embi_css_registered_parse(&a, EMBI_STR("<length>"), text_of(&a, "red"), &v));
    EMB_CHECK(!embi_css_registered_parse(&a, EMBI_STR("<integer>"), text_of(&a, "1.5"), &v));
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<integer>"), text_of(&a, "7"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_INTEGER);
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<length-percentage>"), text_of(&a, "50%"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_PERCENT);
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<color> | auto"), text_of(&a, "auto"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_IDENT);
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<color> | auto"), text_of(&a, "red"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_COLOR);
    EMB_CHECK(!embi_css_registered_parse(&a, EMBI_STR("<color> | auto"), text_of(&a, "1px"), &v));
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("*"), text_of(&a, "a b (c)"), &v));
    EMB_CHECK_EQ_INT(v.kind, EMBI_V_RAW);
    EMB_CHECK(!embi_css_registered_parse(&a, EMBI_STR("*"), text_of(&a, "url(a b)"), &v));
    EMB_CHECK(embi_css_registered_parse(&a, EMBI_STR("<angle>"), text_of(&a, "90deg"), &v));
    EMB_CHECK(v.num == 90);
    embi_arena_free(&a);
}

/* ------------------------------------------------------------------ relative lengths */

EMB_TEST(relative_lengths_resolve_through_the_hook)
{
    embi_len_ctx c = {.font_px = 16.0, .root_px = 10.0, .vw = 1000.0, .vh = 500.0, .cq_w = NAN, .cq_h = NAN};
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_EM, 2) - 32.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_REM, 2) - 20.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_VW, 10) - 100.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_VMIN, 10) - 50.0) < 1e-9);
    /* No container: container units use the viewport. */
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CQW, 10) - 100.0) < 1e-9);
    /* With a container 200 x 80 wide: cqw, cqh and cqmin use it. */
    c.cq_w = 200.0;
    c.cq_h = 80.0;
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CQW, 10) - 20.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CQH, 50) - 40.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CQMIN, 100) - 80.0) < 1e-9);
    EMB_CHECK(fabs(embi_len_px_cb(&c, EMBI_U_CQMAX, 100) - 200.0) < 1e-9);
    /* Angles are not lengths: the hook has no answer for them. */
    EMB_CHECK(isnan(embi_len_px_cb(&c, EMBI_U_DEG, 1)));
}

/* ------------------------------------------------------------------ system colors */

EMB_TEST(system_colors_follow_the_palette)
{
    embi_color c;
    uint16_t canvas = 0;
    EMB_REQUIRE(embi_color_system((embi_str){"Canvas", 6}, &canvas));
    EMB_CHECK(embi_color_system_value(canvas, false, nullptr, &c));
    EMB_CHECK(c.c[0] == 1.0 && c.c[1] == 1.0 && c.c[2] == 1.0);
    EMB_CHECK(embi_color_system_value(canvas, true, nullptr, &c));
    EMB_CHECK(fabs(c.c[0] - 18.0 / 255.0) < 1e-12);
    /* An embedder palette overrides the defaults. */
    embi_color_rgb8 pal[19];
    for (uint32_t i = 0; i < 19; i++) {
        pal[i] = (embi_color_rgb8){1, 2, 3};
    }
    EMB_CHECK(embi_color_system_value(canvas, false, pal, &c));
    EMB_CHECK(fabs(c.c[2] - 3.0 / 255.0) < 1e-12);
    EMB_CHECK(!embi_color_system_value((uint16_t)embi_system_color_count, false, nullptr, &c));
}

EMB_TEST_MAIN()
