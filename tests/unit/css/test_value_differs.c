/*
 * embi_value_clearly_differs (ANIMSCALE): the compare that lets the style engine tell an animated value that moved from one
 * that did not without serializing both. It may only say "differs" when the serializations differ: every pair below is
 * checked against the serialization, and the pairs whose serializations are equal (a difference below the six decimals the
 * serialization keeps) must be left undecided.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"

#include <string.h>

static bool parse(embi_arena *a, const char *prop, const char *text, embi_value *out)
{
    embi_prop_id p = embi_prop_lookup((embi_str){prop, (uint32_t)strlen(prop)});
    embi_longhand lh[EMBI_LONGHAND_MAX];
    embi_css_slice s = embi_css_parse_cvs(a, (embi_str){text, (uint32_t)strlen(text)});
    if (embi_css_parse_decl(a, p, s, lh, EMBI_LONGHAND_MAX) < 1) {
        return false;
    }
    *out = lh[0].value;
    return true;
}

static bool ser_differs(const embi_value *x, const embi_value *y)
{
    embi_sb p = {0}, q = {0};
    embi_value_serialize(&p, x);
    embi_value_serialize(&q, y);
    bool d = !(p.len == q.len && (p.len == 0 || memcmp(p.data, q.data, p.len) == 0));
    embi_sb_free(&p);
    embi_sb_free(&q);
    return d;
}

typedef struct pair {
    const char *prop, *a, *b;
    bool clear; /* expected: decided as different */
} pair;

static const pair k_pairs[] = {
    {"opacity", "0.5", "0.6", true},
    {"opacity", "0.5", "0.5000004", false}, /* the same six decimals */
    {"opacity", "0.5", "0.50002", true},
    {"width", "10px", "10.1px", true},
    {"width", "10px", "10.0000001px", false},
    {"width", "10px", "10em", false}, /* another unit: left to the serialization */
    {"width", "10%", "10.5%", true},
    {"width", "calc(10px + 5%)", "calc(10px + 6%)", false},
    {"background-color", "#ff0000", "#0000ff", true},
    {"background-color", "#ff0000", "#ff0000", false},
    {"background-color", "rgb(255, 0, 0)", "rgb(254, 0, 0)", true},
    {"background-color", "rgba(255, 0, 0, 0.5)", "rgba(255, 0, 0, 0.6)", true},
    {"background-color", "oklch(0.5 0.1 30)", "oklch(0.5 0.1 31)", true},
    {"background-color", "color(display-p3 1 0 0)", "color(display-p3 1 0.1 0)", true},
    {"background-color", "currentcolor", "red", false},
    {"transform", "translateX(10px)", "translateX(20px)", true},
    {"transform", "translateX(10px)", "translateX(10px)", false},
    {"transform", "translateX(10px)", "translateY(10px)", true},
    {"transform", "translateX(10px)", "translateX(10px) rotate(1deg)", true},
    {"transform", "rotate(10deg) scale(1.5)", "rotate(10deg) scale(1.6)", true},
    {"transform", "translate(1px, calc(1em + 2px))", "translate(1px, calc(1em + 3px))", false},
    {"transform", "matrix(1, 0, 0, 1, 5, 5)", "matrix(1, 0, 0, 1, 5, 6)", true},
};

EMB_TEST(it_decides_only_what_the_serializations_decide)
{
    for (size_t i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++) {
        const pair *p = &k_pairs[i];
        embi_arena a;
        embi_arena_init(&a, 4096);
        embi_value x, y;
        EMB_REQUIRE(parse(&a, p->prop, p->a, &x));
        EMB_REQUIRE(parse(&a, p->prop, p->b, &y));
        bool clear = embi_value_clearly_differs(&x, &y);
        EMB_CHECK_EQ_INT(clear, p->clear);
        EMB_CHECK_EQ_INT(embi_value_clearly_differs(&y, &x), clear);
        if (clear) {
            EMB_CHECK(ser_differs(&x, &y)); /* never "differs" for a pair the serialization calls the same */
        }
        embi_arena_free(&a);
    }
}

/* Numbers a hair apart: wherever the compare decides, the serialization agrees. */
EMB_TEST(near_numbers_never_disagree_with_the_serialization)
{
    uint64_t seed = 0x1234567u;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        double base = (double)(int64_t)(seed >> 40) / 4096.0;
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        double step = (double)(seed >> 44) * 1e-7; /* 0 to ~1.7 in steps of 1e-7 */
        embi_value x, y;
        memset(&x, 0, sizeof x);
        memset(&y, 0, sizeof y);
        x.kind = y.kind = EMBI_V_NUMBER;
        x.num = base;
        y.num = base + step;
        if (embi_value_clearly_differs(&x, &y)) {
            EMB_CHECK(ser_differs(&x, &y));
        }
        if (step > 2e-5) {
            EMB_CHECK(embi_value_clearly_differs(&x, &y));
        }
    }
}

EMB_TEST_MAIN()
