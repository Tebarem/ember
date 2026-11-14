/*
 * embi_value_copy (WPTASAN): a deep copy lives in its own arena. Each value is parsed into an arena, copied into another,
 * and the first arena is freed in the poison mode of the arenas (its memory is filled with a pattern and never reused): the
 * copy still serializes as the original did, and none of its parts is in poisoned memory.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"

#include <string.h>

/* The value's text, with what embi_value_serialize leaves as a placeholder spelled out: calc trees and var() component
 * values, at any depth of a list. */
static void text_of_value(embi_sb *sb, const embi_value *v)
{
    embi_value_serialize(sb, v);
    embi_sb_append(sb, EMBI_STR(" |"));
    if (v->kind == EMBI_V_CALC) {
        embi_sb_append(sb, EMBI_STR(" "));
        embi_calc_serialize(sb, v->calc, false);
    } else if (v->kind == EMBI_V_VAR || v->kind == EMBI_V_RAW) {
        for (uint32_t i = 0; i < v->raw.n; i++) {
            embi_sb_append(sb, EMBI_STR(" "));
            embi_css_serialize_cv(sb, &v->raw.p[i]);
        }
    } else if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            text_of_value(sb, &v->list.items[i]);
        }
    }
}

typedef struct copy_case {
    const char *prop;
    const char *text;
} copy_case;

static const copy_case k_cases[] = {
    {"background-image", "linear-gradient(45deg, red 10%, blue calc(20% + 5px)), radial-gradient(circle at 10px 20px, red, "
                         "blue), url(a.png)"},
    {"background-image", "image-set(url(a.png) 1x, url(b.png) 2x)"},
    {"background-image", "conic-gradient(from 10deg at 20% 30%, red, blue 50%, green)"},
    {"transform", "translate(10px, calc(1em + 5%)) rotate(30deg) matrix(1, 0, 0, 1, 5, 5)"},
    {"box-shadow", "1px 2px 3px red, inset 0 0 5px calc(1px + 1em) blue"},
    {"text-shadow", "0 0 5px rgb(255, 0, 0), 1px 1px currentcolor"},
    {"filter", "blur(2px) drop-shadow(1px 1px 2px red) url(#f) saturate(30%)"},
    {"clip-path", "polygon(10% 20%, calc(100% - 5px) 0, 50% 100%)"},
    {"clip-path", "path('M0 0 L10 10 Z')"},
    {"clip-path", "circle(30% at 10px 20px)"},
    {"clip-path", "inset(5px round 3px 4px)"},
    {"transition-timing-function", "cubic-bezier(0.1, 0.2, 0.3, 0.4), steps(3, end), linear(0, 0.5 30%, 1)"},
    {"grid-template-columns", "[a] repeat(2, minmax(10px, 1fr) [b]) fit-content(calc(50% + 2px))"},
    {"grid-row-start", "span 2 foo"},
    {"font-family", "\"My Font\", serif"},
    {"content", "\"a\" counter(x, upper-roman) attr(data-x) \"b\""},
    {"width", "calc(100% - 2 * (3em + 4px))"},
    {"width", "calc(var(--a) + 1px)"},
    {"transform-origin", "left 10px 3px"},
    {"background-position-x", "right 10px"},
    {"color", "light-dark(red, blue)"},
    {"aspect-ratio", "16 / 9"},
};

EMB_TEST(a_deep_copy_does_not_depend_on_the_arena_of_the_original)
{
    for (size_t k = 0; k < sizeof k_cases / sizeof k_cases[0]; k++) {
        const copy_case *c = &k_cases[k];
        embi_arena src;
        embi_arena dst;
        embi_arena_init(&src, 4096);
        embi_arena_init(&dst, 4096);
        embi_prop_id prop = embi_prop_lookup((embi_str){c->prop, (uint32_t)strlen(c->prop)});
        EMB_CHECK(prop != EMBI_PROP_NONE);
        embi_longhand lh[EMBI_LONGHAND_MAX];
        embi_css_slice s = embi_css_parse_cvs(&src, (embi_str){c->text, (uint32_t)strlen(c->text)});
        uint32_t n = embi_css_parse_decl(&src, prop, s, lh, EMBI_LONGHAND_MAX);
        EMB_CHECK(n == 1);
        if (n != 1) {
            SDL_Log("does not parse: %s: %s", c->prop, c->text);
            embi_arena_free(&src);
            embi_arena_free(&dst);
            continue;
        }
        embi_sb before = {nullptr, 0, 0};
        text_of_value(&before, &lh[0].value);
        embi_sb_append_char(&before, '\0');

        embi_value copy;
        embi_value_copy(&dst, &lh[0].value, &copy);
        embi_value again = copy;
        embi_value_copy(&dst, &again, &again); /* in place */

        embi_arena_debug_poison(true);
        embi_arena_free(&src); /* poisoned, kept aside */
        embi_sb after = {nullptr, 0, 0};
        embi_sb in_place = {nullptr, 0, 0};
        text_of_value(&after, &copy);
        text_of_value(&in_place, &again);
        embi_sb_append_char(&after, '\0');
        embi_sb_append_char(&in_place, '\0');
        EMB_CHECK_EQ_STR(after.data, before.data);
        EMB_CHECK_EQ_STR(in_place.data, before.data);
        EMB_CHECK_EQ_INT((int)embi_value_debug_poisoned(&copy), 0);
        embi_arena_debug_poison(false);

        embi_sb_free(&before);
        embi_sb_free(&after);
        embi_sb_free(&in_place);
        embi_arena_free(&dst);
    }
}

/* Without the deep copy (a struct copy), the same check sees the poison: the check above is able to fail. */
EMB_TEST(a_shallow_copy_reads_the_poisoned_arena)
{
    embi_arena src;
    embi_arena_init(&src, 4096);
    const char *text = "translate(10px, 5px) rotate(3deg)";
    embi_prop_id prop = embi_prop_lookup(EMBI_STR("transform"));
    embi_longhand lh[EMBI_LONGHAND_MAX];
    embi_css_slice s = embi_css_parse_cvs(&src, (embi_str){text, (uint32_t)strlen(text)});
    EMB_CHECK_EQ_INT((int)embi_css_parse_decl(&src, prop, s, lh, EMBI_LONGHAND_MAX), 1);
    embi_value shallow = lh[0].value;
    embi_arena_debug_poison(true);
    embi_arena_free(&src);
    EMB_CHECK(shallow.kind == EMBI_V_TRANSFORM);
    EMB_CHECK(embi_arena_debug_poisoned(shallow.xf.p, 8));
    EMB_CHECK(embi_value_debug_poisoned(&shallow) > 0); /* the deep check of the styles (embi_style_debug_poisoned) sees it */
    embi_arena_debug_poison(false);
}

EMB_TEST_MAIN()
