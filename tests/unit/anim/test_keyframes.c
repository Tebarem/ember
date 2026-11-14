/*
 * Keyframes (W6a): sets built from flat entries (unsorted offsets, duplicates, implicit 0% and
 * 100% keyframes), @keyframes rules collected from parsed sheets (keyframe selector lists, from
 * and to, !important ignored, non-animatable properties ignored, cascade-layer aware names), and
 * the per-keyframe timing functions.
 */
#include "emb_test.h"

#include "anim_fixture.h"

static embi_css_sheet *parse_sheet(const char *text)
{
    embi_css_parse_opts opts;
    memset(&opts, 0, sizeof opts);
    return embi_css_sheet_parse((embi_str){text, (uint32_t)strlen(text)}, &opts);
}

static double sample_num(const embi_kf_set *s, const char *prop, double t, const embi_value *base,
                         bool *ok)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_value v;
    memset(&v, 0, sizeof v);
    *ok = embi_kf_sample(s, anim_prop(prop), t, base, nullptr, nullptr, &a, &v);
    double r = *ok ? v.num : NAN;
    embi_arena_free(&a);
    return r;
}

EMB_TEST(build_sorts_and_fills_the_implicit_keyframes)
{
    embi_arena a;
    anim_arena_open(&a);
    /* Entries in arbitrary order; opacity has no 0% and no 100%. */
    embi_kf_input in[3];
    memset(in, 0, sizeof in);
    in[0].offset = 1.0;
    in[0].prop = anim_prop("opacity");
    in[0].value.kind = EMBI_V_NUMBER;
    in[0].value.num = 1.0;
    in[1].offset = 0.0 + 0.5;
    in[1].prop = anim_prop("opacity");
    in[1].value.kind = EMBI_V_NUMBER;
    in[1].value.num = 0.5;
    in[2].offset = 0.5;
    in[2].prop = anim_prop("opacity");
    in[2].value.kind = EMBI_V_NUMBER;
    in[2].value.num = 0.75;
    const embi_kf_set *s = embi_kf_build(&a, in, 3);
    EMB_CHECK(embi_kf_prop_count(s) == 1 && embi_kf_has_prop(s, anim_prop("opacity")));

    /* The implicit 0% keyframe is the underlying value (0.2 here). */
    embi_value base;
    memset(&base, 0, sizeof base);
    base.kind = EMBI_V_NUMBER;
    base.num = 0.2;
    embi_value v;
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 0.0, &base, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 0.2, 1e-9));
    /* Duplicate offsets: the later one is the value at and after that offset. */
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 0.5, &base, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 0.75, 1e-9));
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 0.25, &base, nullptr, nullptr, &a, &v));
    /* 0.25 lies between the underlying 0% and the 50% keyframe (0.5). */
    EMB_CHECK(anim_near(v.num, 0.2 + (0.5 - 0.2) * 0.5, 1e-9));
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 1.0, &base, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 1.0, 1e-9));
    /* A property with no track samples to false. */
    EMB_CHECK(!embi_kf_sample(s, anim_prop("width"), 0.5, &base, nullptr, nullptr, &a, &v));
    embi_arena_free(&a);
}

EMB_TEST(unspecified_offsets_are_spaced_evenly)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_input in[4];
    memset(in, 0, sizeof in);
    for (int i = 0; i < 4; i++) {
        in[i].prop = anim_prop("opacity");
        in[i].value.kind = EMBI_V_NUMBER;
        in[i].value.num = (double)i;
        in[i].offset = NAN;
        in[i].key = (uint32_t)i;
    }
    in[0].offset = 0.0;
    in[3].offset = 1.0;
    const embi_kf_set *s = embi_kf_build(&a, in, 4);
    embi_value v;
    /* The middle two keyframes sit at 1/3 and 2/3. */
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 1.0 / 3.0, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 1.0, 1e-9));
    EMB_CHECK(embi_kf_sample(s, anim_prop("opacity"), 2.0 / 3.0, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 2.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(collects_keyframes_with_selectors_and_defaults)
{
    embi_css_sheet *sh = parse_sheet(
        "@keyframes fade { from { opacity: 0 } 50%, 75% { opacity: 0.5 } to { opacity: 1 } }"
        "@keyframes late { 50% { opacity: 0.5 } to { opacity: 1 } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    env.width = 800;
    env.height = 600;
    env.dppx = 1;
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);

    const embi_kf_set *fade = embi_kf_table_find(&t, embi_atom_intern("fade", 4));
    EMB_CHECK(fade != nullptr);
    bool ok;
    EMB_CHECK(anim_near(sample_num(fade, "opacity", 0.25, nullptr, &ok), 0.25, 1e-9) && ok);
    EMB_CHECK(anim_near(sample_num(fade, "opacity", 0.6, nullptr, &ok), 0.5, 1e-9) && ok);
    EMB_CHECK(anim_near(sample_num(fade, "opacity", 0.875, nullptr, &ok), 0.75, 1e-9) && ok);

    /* No 0% keyframe: the underlying opacity (0.2) is the start. */
    const embi_kf_set *late = embi_kf_table_find(&t, embi_atom_intern("late", 4));
    EMB_CHECK(late != nullptr);
    embi_value base;
    memset(&base, 0, sizeof base);
    base.kind = EMBI_V_NUMBER;
    base.num = 0.2;
    EMB_CHECK(anim_near(sample_num(late, "opacity", 0.25, &base, &ok), 0.35, 1e-9) && ok);

    /* An unknown name is not found. */
    EMB_CHECK(embi_kf_table_find(&t, embi_atom_intern("nope", 4)) == nullptr);

    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

EMB_TEST(important_and_non_animatable_declarations_are_ignored)
{
    embi_css_sheet *sh = parse_sheet(
        "@keyframes imp { to { opacity: 1 !important } }"
        "@keyframes pos { to { will-change: opacity; opacity: 0.5 } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);

    const embi_kf_set *imp = embi_kf_table_find(&t, embi_atom_intern("imp", 3));
    EMB_CHECK(imp != nullptr && embi_kf_prop_count(imp) == 0);
    const embi_kf_set *pos = embi_kf_table_find(&t, embi_atom_intern("pos", 3));
    EMB_CHECK(pos != nullptr && !embi_kf_has_prop(pos, anim_prop("will-change")));
    EMB_CHECK(embi_kf_has_prop(pos, anim_prop("opacity")));

    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

EMB_TEST(discrete_properties_are_kept_and_flip_at_half_and_extrapolation)
{
    embi_css_sheet *sh = parse_sheet(
        "@keyframes d { from { pointer-events: auto; overflow-x: visible; opacity: 0 }"
        " to { pointer-events: none; overflow-x: hidden; opacity: 1 } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);
    const embi_kf_set *d = embi_kf_table_find(&t, embi_atom_intern("d", 1));
    EMB_CHECK(d != nullptr);
    EMB_CHECK(embi_kf_has_prop(d, anim_prop("pointer-events")));
    EMB_CHECK(embi_kf_has_prop(d, anim_prop("overflow-x")));
    embi_value v;
    memset(&v, 0, sizeof v);
    EMB_CHECK(embi_kf_sample(d, anim_prop("pointer-events"), 0.25, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(v.kw == EMBI_KW_auto);
    EMB_CHECK(embi_kf_sample(d, anim_prop("pointer-events"), 0.75, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(v.kw == EMBI_KW_none);
    EMB_CHECK(embi_kf_sample(d, anim_prop("overflow-x"), 0.75, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(v.kw == EMBI_KW_hidden);
    /* Outside 0..1 the last interval is extended. */
    EMB_CHECK(embi_kf_sample(d, anim_prop("opacity"), 1.5, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, 1.5, 1e-9));
    EMB_CHECK(embi_kf_sample(d, anim_prop("opacity"), -0.5, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(anim_near(v.num, -0.5, 1e-9));
    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

EMB_TEST(dropped_declarations_are_counted_and_the_raw_rule_is_exposed)
{
    embi_css_sheet *sh = parse_sheet(
        "@keyframes d { from { opacity: var(--a); width: inherit; height: 4px } to { opacity: 1 } }"
        "@keyframes ok { to { opacity: 1 } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);
    embi_atom d = embi_atom_intern("d", 1);
    EMB_CHECK(embi_kf_dropped_count(embi_kf_table_find(&t, d)) == 2);
    EMB_CHECK(embi_kf_dropped_count(embi_kf_table_find(&t, embi_atom_intern("ok", 2))) == 0);
    const embi_css_rule *r = embi_kf_table_find_rule(&t, d);
    EMB_CHECK(r != nullptr && r->kind == EMBI_CSS_RULE_KEYFRAMES && r->nkids == 2);
    EMB_CHECK(embi_kf_table_find_rule(&t, embi_atom_intern("nope", 4)) == nullptr);
    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

EMB_TEST(layers_resolve_names_by_cascade_order)
{
    /* The unlayered rule beats the layered one whatever the order; among layers the later one wins. */
    embi_css_sheet *sh = parse_sheet(
        "@layer a { @keyframes x { from { opacity: 0 } to { opacity: 0.1 } } }"
        "@keyframes x { from { opacity: 0 } to { opacity: 0.9 } }"
        "@keyframes y { from { opacity: 0 } to { opacity: 0.2 } }"
        "@layer b { @keyframes y { from { opacity: 0 } to { opacity: 0.8 } } }"
        "@layer c { @keyframes z { from { opacity: 0 } to { opacity: 0.1 } } }"
        "@layer d { @keyframes z { from { opacity: 0 } to { opacity: 0.7 } } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);
    bool ok;
    const embi_kf_set *x = embi_kf_table_find(&t, embi_atom_intern("x", 1));
    EMB_CHECK(anim_near(sample_num(x, "opacity", 1.0, nullptr, &ok), 0.9, 1e-9) && ok);
    const embi_kf_set *y = embi_kf_table_find(&t, embi_atom_intern("y", 1));
    /* The unlayered y beats the layered one. */
    EMB_CHECK(anim_near(sample_num(y, "opacity", 1.0, nullptr, &ok), 0.2, 1e-9) && ok);
    /* Between two layers, the later layer wins. */
    const embi_kf_set *z = embi_kf_table_find(&t, embi_atom_intern("z", 1));
    EMB_CHECK(anim_near(sample_num(z, "opacity", 1.0, nullptr, &ok), 0.7, 1e-9) && ok);

    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

EMB_TEST(per_keyframe_timing_functions_apply_between_keyframes)
{
    embi_css_sheet *sh = parse_sheet(
        "@keyframes step { from { opacity: 0; animation-timing-function: steps(1, jump-end) }"
        " to { opacity: 1 } }");
    EMB_CHECK(sh != nullptr);
    embi_css_sheet *sheets[1] = {sh};
    embi_style_env env;
    memset(&env, 0, sizeof env);
    embi_arena a;
    anim_arena_open(&a);
    embi_kf_table t;
    memset(&t, 0, sizeof t);
    embi_kf_table_collect(&a, &t, sheets, 1, &env);
    const embi_kf_set *s = embi_kf_table_find(&t, embi_atom_intern("step", 4));
    EMB_CHECK(s != nullptr);
    bool ok;
    /* steps(1, jump-end) holds 0 until the end of the segment. */
    EMB_CHECK(anim_near(sample_num(s, "opacity", 0.5, nullptr, &ok), 0.0, 1e-9) && ok);
    EMB_CHECK(anim_near(sample_num(s, "opacity", 0.999, nullptr, &ok), 0.0, 1e-9) && ok);

    /* The default easing (for CSS animations, the animation-timing-function) applies to the
     * segments that have none. */
    embi_css_sheet *sh2 = parse_sheet("@keyframes lin { from { opacity: 0 } to { opacity: 1 } }");
    embi_css_sheet *sheets2[1] = {sh2};
    embi_kf_table t2;
    memset(&t2, 0, sizeof t2);
    embi_kf_table_collect(&a, &t2, sheets2, 1, &env);
    const embi_kf_set *lin = embi_kf_table_find(&t2, embi_atom_intern("lin", 3));
    embi_arena b;
    anim_arena_open(&b);
    const embi_easing *stepped = anim_easing(&b, "steps(2, jump-none)");
    embi_value v;
    EMB_CHECK(embi_kf_sample(lin, anim_prop("opacity"), 0.25, nullptr, stepped, nullptr, &b, &v));
    EMB_CHECK(anim_near(v.num, 0.0, 1e-9));
    embi_arena_free(&b);

    embi_kf_table_free(&t2);
    embi_css_sheet_unref(sh2);
    embi_kf_table_free(&t);
    embi_arena_free(&a);
    embi_css_sheet_unref(sh);
}

static void kf_in(embi_kf_input *x, double off, const char *prop, double num, uint32_t key)
{
    memset(x, 0, sizeof *x);
    x->offset = off;
    x->prop = anim_prop(prop);
    x->value.kind = EMBI_V_NUMBER;
    x->value.num = num;
    x->key = key;
}

static void three_keyframes_without_offsets_are_at_0_half_and_1_body(void)
{
    {
        embi_arena a;
        anim_arena_open(&a);
        /* The `[{opacity:0},{opacity:.5},{opacity:1}]` form: every offset unspecified. */
        embi_kf_input in[3];
        kf_in(&in[0], NAN, "opacity", 0.0, 0);
        kf_in(&in[1], NAN, "opacity", 0.5, 1);
        kf_in(&in[2], NAN, "opacity", 1.0, 2);
        const embi_kf_set *s = embi_kf_build(&a, in, 3);
        bool ok;
        embi_value v;
        ok = embi_kf_sample(s, anim_prop("opacity"), 0.0, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 0.0, 1e-9));
        ok = embi_kf_sample(s, anim_prop("opacity"), 0.25, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 0.25, 1e-9));
        ok = embi_kf_sample(s, anim_prop("opacity"), 0.5, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 0.5, 1e-9));
        /* t = 1 is the last keyframe: there is no underlying value needed. */
        ok = embi_kf_sample(s, anim_prop("opacity"), 1.0, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 1.0, 1e-9));
        embi_arena_free(&a);
    }
}

EMB_TEST(three_keyframes_without_offsets_are_at_0_half_and_1)
{
    /* A first run warms up the interned names; the second run must return to the baseline. */
    three_keyframes_without_offsets_are_at_0_half_and_1_body();
    size_t before = embi_mem_live();
    three_keyframes_without_offsets_are_at_0_half_and_1_body();
    EMB_CHECK(embi_mem_live() == before);
}

static void unspecified_offsets_span_all_properties_together_body(void)
{
    {
        embi_arena a;
        anim_arena_open(&a);
        /* Keyframe 0 names opacity, keyframe 1 width, keyframe 2 both. Spacing is over keyframes,
         * not per property: width's only explicit entries sit at 0.5 and 1. */
        embi_kf_input in[4];
        kf_in(&in[0], NAN, "opacity", 0.0, 0);
        kf_in(&in[1], NAN, "width", 10.0, 1);
        kf_in(&in[2], NAN, "opacity", 1.0, 2);
        kf_in(&in[3], NAN, "width", 20.0, 2);
        in[1].value.unit = EMBI_U_PX;
        in[1].value.kind = EMBI_V_LENGTH;
        in[3].value.unit = EMBI_U_PX;
        in[3].value.kind = EMBI_V_LENGTH;
        const embi_kf_set *s = embi_kf_build(&a, in, 4);
        EMB_CHECK(embi_kf_prop_count(s) == 2);
        bool ok;
        embi_value v;
        ok = embi_kf_sample(s, anim_prop("opacity"), 0.5, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 0.5, 1e-9));
        ok = embi_kf_sample(s, anim_prop("width"), 0.5, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && v.kind == EMBI_V_LENGTH && anim_near(v.num, 10.0, 1e-9));
        ok = embi_kf_sample(s, anim_prop("width"), 0.75, nullptr, nullptr, nullptr, &a, &v);
        EMB_CHECK(ok && anim_near(v.num, 15.0, 1e-9));
        embi_arena_free(&a);
    }
}

EMB_TEST(unspecified_offsets_span_all_properties_together)
{
    /* A first run warms up the interned names; the second run must return to the baseline. */
    unspecified_offsets_span_all_properties_together_body();
    size_t before = embi_mem_live();
    unspecified_offsets_span_all_properties_together_body();
    EMB_CHECK(embi_mem_live() == before);
}

static void duplicate_selectors_and_properties_take_the_last_value_body(void)
{
    {
        embi_css_sheet *sh = parse_sheet(
            "@keyframes dupk { 0% { opacity: 0 } 50% { opacity: 1 } 50% { opacity: .2 } 100% { opacity: 0 } }"
            "@keyframes dupp { from { opacity: 0 } to { opacity: 1; opacity: .5 } }");
        EMB_CHECK(sh != nullptr);
        embi_css_sheet *sheets[1] = {sh};
        embi_style_env env;
        memset(&env, 0, sizeof env);
        embi_arena a;
        anim_arena_open(&a);
        embi_kf_table t;
        memset(&t, 0, sizeof t);
        embi_kf_table_collect(&a, &t, sheets, 1, &env);
        bool ok;
        /* 0 -> 0.2 over the first half: at 0.25 that is 0.1, not the overridden 1. */
        const embi_kf_set *dupk = embi_kf_table_find(&t, embi_atom_intern("dupk", 4));
        EMB_CHECK(dupk != nullptr);
        EMB_CHECK(anim_near(sample_num(dupk, "opacity", 0.25, nullptr, &ok), 0.1, 1e-9) && ok);
        const embi_kf_set *dupp = embi_kf_table_find(&t, embi_atom_intern("dupp", 4));
        EMB_CHECK(dupp != nullptr);
        EMB_CHECK(anim_near(sample_num(dupp, "opacity", 1.0, nullptr, &ok), 0.5, 1e-9) && ok);
        EMB_CHECK(anim_near(sample_num(dupp, "opacity", 0.5, nullptr, &ok), 0.25, 1e-9) && ok);
        embi_kf_table_free(&t);
        embi_arena_free(&a);
        embi_css_sheet_unref(sh);
    }
}

EMB_TEST(duplicate_selectors_and_properties_take_the_last_value)
{
    /* A first run warms up the interned names; the second run must return to the baseline. */
    duplicate_selectors_and_properties_take_the_last_value_body();
    size_t before = embi_mem_live();
    duplicate_selectors_and_properties_take_the_last_value_body();
    EMB_CHECK(embi_mem_live() == before);
}

static void out_of_range_percentages_are_ignored_body(void)
{
    {
        /* 150% and -20% are not keyframe selectors: those keyframes are ignored. */
        embi_css_sheet *sh = parse_sheet(
            "@keyframes big { 0% { opacity: 0 } 150% { opacity: 1 } }"
            "@keyframes neg { -20% { opacity: 1 } 0% { opacity: 0 } 100% { opacity: 0.5 } }");
        EMB_CHECK(sh != nullptr);
        embi_css_sheet *sheets[1] = {sh};
        embi_style_env env;
        memset(&env, 0, sizeof env);
        embi_arena a;
        anim_arena_open(&a);
        embi_kf_table t;
        memset(&t, 0, sizeof t);
        embi_kf_table_collect(&a, &t, sheets, 1, &env);
        bool ok;
        const embi_kf_set *big = embi_kf_table_find(&t, embi_atom_intern("big", 3));
        EMB_CHECK(big != nullptr);
        /* Only the 0% keyframe is left: at 1 the underlying value is needed and there is none. */
        (void)sample_num(big, "opacity", 1.0, nullptr, &ok);
        EMB_CHECK(!ok);
        const embi_kf_set *neg = embi_kf_table_find(&t, embi_atom_intern("neg", 3));
        EMB_CHECK(neg != nullptr);
        EMB_CHECK(anim_near(sample_num(neg, "opacity", 1.0, nullptr, &ok), 0.5, 1e-9) && ok);
        EMB_CHECK(anim_near(sample_num(neg, "opacity", 0.5, nullptr, &ok), 0.25, 1e-9) && ok);
        embi_kf_table_free(&t);
        embi_arena_free(&a);
        embi_css_sheet_unref(sh);
    }
}

EMB_TEST(out_of_range_percentages_are_ignored)
{
    /* A first run warms up the interned names; the second run must return to the baseline. */
    out_of_range_percentages_are_ignored_body();
    size_t before = embi_mem_live();
    out_of_range_percentages_are_ignored_body();
    EMB_CHECK(embi_mem_live() == before);
}

static void keyframe_collection_returns_memory_to_baseline_body(void)
{
    for (int round = 0; round < 2; round++) {
        embi_css_sheet *sh = parse_sheet("@keyframes fade { from { opacity: 0 } to { opacity: 1 } }");
        embi_css_sheet *sheets[1] = {sh};
        embi_style_env env;
        memset(&env, 0, sizeof env);
        embi_arena a;
        anim_arena_open(&a);
        embi_kf_table t;
        memset(&t, 0, sizeof t);
        embi_kf_table_collect(&a, &t, sheets, 1, &env);
        embi_kf_table_free(&t);
        embi_arena_free(&a);
        embi_css_sheet_unref(sh);
    }
}

EMB_TEST(keyframe_collection_returns_memory_to_baseline)
{
    /* A first run warms up the interned names; the second run must return to the baseline. */
    keyframe_collection_returns_memory_to_baseline_body();
    size_t before = embi_mem_live();
    keyframe_collection_returns_memory_to_baseline_body();
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST_MAIN()
