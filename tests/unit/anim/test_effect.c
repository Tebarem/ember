/*
 * Effect sampling (W6a): the animated values of an element at a time. Covers the overlay of an
 * animation on the base value, the held time, the fill, the add and accumulate composites, the
 * !important skip, and transitions overriding animations.
 */
#include "emb_test.h"

#include "anim_fixture.h"

static embi_value g_base;

static const embi_value *base_cb(void *user, embi_prop_id prop)
{
    (void)user;
    (void)prop;
    return &g_base;
}

static bool important_opacity(void *user, embi_prop_id prop)
{
    (void)user;
    return prop == anim_prop("opacity");
}

static void set_base(double v)
{
    memset(&g_base, 0, sizeof g_base);
    g_base.kind = EMBI_V_NUMBER;
    g_base.num = v;
}

/* A two-keyframe opacity set: from at 0% to at 100%. */
static const embi_kf_set *opacity_kf(embi_arena *a, double from, double to)
{
    embi_kf_input in[2];
    memset(in, 0, sizeof in);
    in[0].offset = 0.0;
    in[0].prop = anim_prop("opacity");
    in[0].value.kind = EMBI_V_NUMBER;
    in[0].value.num = from;
    in[1].offset = 1.0;
    in[1].prop = anim_prop("opacity");
    in[1].value.kind = EMBI_V_NUMBER;
    in[1].value.num = to;
    return embi_kf_build(a, in, 2);
}

static embi_anim_effect effect_of(const embi_kf_set *kf)
{
    embi_anim_effect fx;
    memset(&fx, 0, sizeof fx);
    fx.kf = kf;
    fx.timing = embi_anim_timing_default();
    fx.timing.duration = 1.0;
    fx.timing.iterations = 1.0;
    fx.composite = EMBI_COMP_REPLACE;
    return fx;
}

static double opacity_of(const embi_overlay *o)
{
    const embi_value *v = embi_overlay_find(o, anim_prop("opacity"));
    return v != nullptr ? v->num : NAN;
}

EMB_TEST(animation_overlays_the_base_value)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.9);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    const embi_kf_set *kf = opacity_kf(&a, 0.0, 1.0);
    embi_anim_effect fx = effect_of(kf);
    embi_overlay out;
    memset(&out, 0, sizeof out);

    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.5, 1e-9));

    /* Outside the active interval, with no fill, nothing is overlaid. */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 2.0, &out);
    EMB_CHECK(embi_overlay_find(&out, anim_prop("opacity")) == nullptr);

    /* A held effect samples its held time whatever the clock. */
    fx.held = true;
    fx.hold = 0.25;
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 99.0, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.25, 1e-9));

    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST(important_declarations_beat_animations)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.9);
    embi_anim_env env = {nullptr, base_cb, important_opacity, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));
    embi_overlay out;
    memset(&out, 0, sizeof out);
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
    EMB_CHECK(embi_overlay_find(&out, anim_prop("opacity")) == nullptr);
    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST(transitions_override_animations)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.9);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));

    /* A transition from 0.2 to 0.4 started at 0: at 0.5 it is 0.3, and it beats the animation's
     * 0.5 there. */
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    bool ok1, ok2;
    embi_value before = anim_parse(&a, "opacity", "0.2", &ok1);
    embi_value after = anim_parse(&a, "opacity", "0.4", &ok2);
    embi_trans_change ch = {anim_prop("opacity"), &before, &after};
    embi_trans_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.prop = anim_prop("opacity");
    spec.duration = 1.0;
    embi_trans_input in = {&spec, 1, &ch, 1, false, nullptr};
    embi_trans_update(&a, &tr, &in, 0.0, &ev);
    EMB_CHECK(tr.n == 1);

    embi_overlay out;
    memset(&out, 0, sizeof out);
    embi_anim_sample_effects(&a, &env, &fx, 1, &tr, 0.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.3, 1e-9));

    embi_overlay_free(&out);
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(add_composes_on_the_underlying_value)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.5);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 0.5));
    fx.composite = EMBI_COMP_ADD;
    embi_overlay out;
    memset(&out, 0, sizeof out);
    /* The animated value 0.25 at 0.5 adds to the underlying 0.5. */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.75, 1e-9));
    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST(iteration_composite_accumulates_the_iterations)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.0);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));
    fx.timing.iterations = 3.0;
    /* iterationComposite accumulate, composite replace: the value is the keyframe value plus the
     * iteration index times the keyframe span. */
    fx.iteration_composite = EMBI_COMP_ACCUMULATE;
    fx.composite = EMBI_COMP_REPLACE;
    embi_overlay out;
    memset(&out, 0, sizeof out);
    /* Local time 1.5 is iteration 1 at 0.5: 0.5 plus one iteration of (1 - 0). */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 1.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 1.5, 1e-9));
    /* The first iteration does not accumulate. */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.5, 1e-9));
    /* The third iteration at its middle: 0.5 + 2 * 1 = 2.5. */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 2.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 2.5, 1e-9));
    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST(composite_accumulate_adds_to_the_underlying_value_once)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.5);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 0.5));
    fx.timing.iterations = 3.0;
    /* composite accumulate is an add with the underlying value (the same as add), and the
     * iterations do not add unless iterationComposite says so: iteration 1 at 0.5 is 0.25 + 0.5. */
    fx.composite = EMBI_COMP_ACCUMULATE;
    fx.iteration_composite = EMBI_COMP_REPLACE;
    embi_overlay out;
    memset(&out, 0, sizeof out);
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 1.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.75, 1e-9));

    /* With both fields set the two compose independently: 0.25 + 1 * 0.5 + 0.5 = 1.25. */
    fx.iteration_composite = EMBI_COMP_ACCUMULATE;
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 1.5, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 1.25, 1e-9));
    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST(overlay_sampling_returns_memory_to_baseline)
{
    embi_overlay out;
    memset(&out, 0, sizeof out);
    for (int round = 0; round < 2; round++) {
        embi_arena a;
        anim_arena_open(&a);
        set_base(0.9);
        embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
        embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));
        embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
        embi_arena_free(&a);
    }
    embi_overlay_free(&out);
    size_t before = embi_mem_live();
    {
        embi_arena a;
        anim_arena_open(&a);
        set_base(0.9);
        embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
        embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));
        embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 0.5, &out);
        embi_overlay_free(&out);
        embi_arena_free(&a);
    }
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(currentcolor_in_the_underlying_value_resolves_for_the_animation)
{
    embi_arena a;
    anim_arena_open(&a);
    bool ok;
    /* The base border color is currentcolor, the element's color is blue, and the keyframes go to
     * red: at half way the animated color is a mix (blue is in it), not the discrete red. */
    embi_value base_cc = anim_parse(&a, "border-top-color", "currentcolor", &ok);
    embi_value blue = anim_parse(&a, "color", "blue", &ok);
    EMB_CHECK(ok && blue.kind == EMBI_V_COLOR);
    embi_kf_input in[2];
    memset(in, 0, sizeof in);
    in[0].offset = 0.0;
    in[0].prop = anim_prop("border-top-color");
    in[0].value = base_cc;
    in[1].offset = 1.0;
    in[1].prop = anim_prop("border-top-color");
    in[1].value = anim_parse(&a, "border-top-color", "red", &ok);
    const embi_kf_set *kf = embi_kf_build(&a, in, 2);
    embi_value v;
    EMB_CHECK(embi_kf_sample(kf, anim_prop("border-top-color"), 0.5, nullptr, nullptr, &blue.color, &a, &v));
    EMB_CHECK(v.kind == EMBI_V_COLOR && v.color.c[2] > 0.0 && v.color.c[0] < 1.0);
    /* Without the element color the keyframe stays unresolved and flips. */
    EMB_CHECK(embi_kf_sample(kf, anim_prop("border-top-color"), 0.25, nullptr, nullptr, nullptr, &a, &v));
    EMB_CHECK(v.kind == EMBI_V_COLOR && v.color.space == EMBI_CS_CURRENT);
    embi_arena_free(&a);
}

EMB_TEST(effect_time_uses_the_playback_rate)
{
    embi_arena a;
    anim_arena_open(&a);
    set_base(0.0);
    embi_anim_env env = {nullptr, base_cb, nullptr, nullptr};
    embi_anim_effect fx = effect_of(opacity_kf(&a, 0.0, 1.0));
    fx.start = 1.0;
    fx.timing.rate = 2.0;
    embi_overlay out;
    memset(&out, 0, sizeof out);
    /* At timeline time 1.25 the effect has run 0.25 s; at rate 2 its effect time is 0.5 s, which
     * is half of the duration. */
    embi_anim_sample_effects(&a, &env, &fx, 1, nullptr, 1.25, &out);
    EMB_CHECK(anim_near(opacity_of(&out), 0.5, 1e-9));
    embi_overlay_free(&out);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
