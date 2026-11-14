/*
 * CSS Transitions (W6a): the start algorithm as a pure function. Covers the run, start and end
 * sequence, reversing shortening, replacement, cancellation when the property leaves the
 * transition list or becomes non-animatable, discrete properties with and without
 * transition-behavior: allow-discrete, visibility, display: none, delays, zero durations, and the
 * memory returned to its baseline.
 */
#include "emb_test.h"

#include "anim_fixture.h"

static bool has_event(const embi_trans_events *ev, uint8_t kind, embi_prop_id prop)
{
    for (uint32_t i = 0; i < ev->n; i++) {
        if (ev->e[i].kind == kind && ev->e[i].prop == prop) {
            return true;
        }
    }
    return false;
}

static void clear_events(embi_trans_events *ev)
{
    ev->n = 0;
}

/* One restyle that changes prop from before to after with the given spec. */
static void change(embi_arena *a, embi_transitions *tr, embi_trans_events *ev, const char *prop,
                   const char *before, const char *after, embi_trans_spec spec, double now,
                   bool in_display_none)
{
    bool ok1, ok2;
    embi_value b = anim_parse(a, prop, before, &ok1);
    embi_value f = anim_parse(a, prop, after, &ok2);
    embi_trans_change ch = {anim_prop(prop), &b, &f};
    embi_trans_input in = {&spec, 1u, &ch, 1u, in_display_none, nullptr};
    embi_trans_update(a, tr, &in, now, ev);
}

static embi_trans_spec spec_of(const char *prop, double duration, double delay)
{
    embi_trans_spec s;
    memset(&s, 0, sizeof s);
    s.prop = anim_prop(prop);
    s.duration = duration;
    s.delay = delay;
    return s;
}

EMB_TEST(run_start_and_end)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_RUN);
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.5, &v);
    EMB_CHECK(v.kind == EMBI_V_NUMBER && anim_near(v.num, 0.5, 1e-9));

    clear_events(&ev);
    embi_trans_advance(&tr, 0.0, &ev);
    EMB_CHECK(ev.n == 1 && ev.e[0].kind == EMBI_TEV_START);
    clear_events(&ev);
    embi_trans_advance(&tr, 0.5, &ev);
    EMB_CHECK(ev.n == 0 && tr.n == 1);
    embi_trans_advance(&tr, 1.0, &ev);
    EMB_CHECK(ev.n == 1 && ev.e[0].kind == EMBI_TEV_END && tr.n == 0);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(delay_holds_the_start_value)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.5), 0.0, false);
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.25, &v);
    EMB_CHECK(anim_near(v.num, 1.0, 1e-9));
    clear_events(&ev);
    embi_trans_advance(&tr, 0.25, &ev);
    EMB_CHECK(ev.n == 0);
    embi_trans_advance(&tr, 0.5, &ev);
    EMB_CHECK(ev.n == 1 && ev.e[0].kind == EMBI_TEV_START);
    embi_trans_value(&a, &tr.v[0], 1.0, &v);
    EMB_CHECK(anim_near(v.num, 0.5, 1e-9));
    clear_events(&ev);
    embi_trans_advance(&tr, 1.5, &ev);
    EMB_CHECK(ev.n == 1 && ev.e[0].kind == EMBI_TEV_END && tr.n == 0);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(reversing_shortens_the_transition)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    /* At 0.5 the value is 0.5. Going back to 1 (the start value) shortens the transition to half
     * its duration, starting from the current value. */
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "0.5", "1", spec_of("opacity", 1.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 1 && anim_near(tr.v[0].duration, 0.5, 1e-9));
    EMB_CHECK(has_event(&ev, EMBI_TEV_CANCEL, anim_prop("opacity")));
    EMB_CHECK(has_event(&ev, EMBI_TEV_RUN, anim_prop("opacity")));
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.5, &v);
    EMB_CHECK(anim_near(v.num, 0.5, 1e-9));
    embi_trans_value(&a, &tr.v[0], 0.75, &v);
    EMB_CHECK(anim_near(v.num, 0.75, 1e-9));
    embi_trans_value(&a, &tr.v[0], 1.0, &v);
    EMB_CHECK(anim_near(v.num, 1.0, 1e-9));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(replacement_starts_from_the_current_value)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    clear_events(&ev);
    /* A new target that is neither the end nor the start replaces the transition at 0.5. */
    change(&a, &tr, &ev, "opacity", "0.5", "0.25", spec_of("opacity", 1.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 1 && anim_near(tr.v[0].duration, 1.0, 1e-9));
    EMB_CHECK(has_event(&ev, EMBI_TEV_CANCEL, anim_prop("opacity")));
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.5, &v);
    EMB_CHECK(anim_near(v.num, 0.5, 1e-9));
    embi_trans_value(&a, &tr.v[0], 1.5, &v);
    EMB_CHECK(anim_near(v.num, 0.25, 1e-9));

    /* The same target as the running transition changes nothing. */
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "0.5", "0.25", spec_of("opacity", 1.0, 0.0), 0.7, false);
    EMB_CHECK(ev.n == 0 && tr.n == 1);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(cancellation_when_the_property_leaves_the_list)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    clear_events(&ev);
    /* transition-property: none (no specs): the running transition is cancelled. */
    embi_trans_input in = {nullptr, 0, nullptr, 0, false, nullptr};
    embi_trans_update(&a, &tr, &in, 0.2, &ev);
    EMB_CHECK(tr.n == 0 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_CANCEL);
    EMB_CHECK(anim_near(ev.e[0].elapsed, 0.2, 1e-9));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(discrete_properties_need_allow_discrete)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    /* display is discrete: without allow-discrete no transition starts and the value flips. */
    embi_trans_spec s = spec_of("display", 1.0, 0.0);
    change(&a, &tr, &ev, "display", "block", "inline", s, 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);

    s.allow_discrete = true;
    change(&a, &tr, &ev, "display", "block", "inline", s, 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_RUN);
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.25, &v);
    EMB_CHECK(v.kw == EMBI_KW_block);
    embi_trans_value(&a, &tr.v[0], 0.75, &v);
    EMB_CHECK(v.kw == EMBI_KW_inline);

    /* visibility transitions without allow-discrete, with its visible end. */
    embi_transitions_free(&tr);
    memset(&tr, 0, sizeof tr);
    clear_events(&ev);
    change(&a, &tr, &ev, "visibility", "visible", "hidden", spec_of("visibility", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 1);
    embi_trans_value(&a, &tr.v[0], 0.5, &v);
    EMB_CHECK(v.kw == EMBI_KW_visible);

    /* Non-animatable properties never transition. */
    embi_transitions_free(&tr);
    memset(&tr, 0, sizeof tr);
    clear_events(&ev);
    change(&a, &tr, &ev, "will-change", "auto", "opacity", spec_of("will-change", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);

    /* Discrete properties (pointer-events, overflow-x) transition only with allow-discrete, and
     * flip at 50%. */
    embi_trans_spec ds = spec_of("pointer-events", 1.0, 0.0);
    change(&a, &tr, &ev, "pointer-events", "auto", "none", ds, 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);
    ds.allow_discrete = true;
    change(&a, &tr, &ev, "pointer-events", "auto", "none", ds, 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 1);
    embi_trans_value(&a, &tr.v[0], 0.25, &v);
    EMB_CHECK(v.kw == EMBI_KW_auto);
    embi_trans_value(&a, &tr.v[0], 0.75, &v);
    EMB_CHECK(v.kw == EMBI_KW_none);

    /* No transition from X to X when none is running. */
    embi_transitions_free(&tr);
    memset(&tr, 0, sizeof tr);
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "0.5", "0.5", spec_of("opacity", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(display_none_cancels_everything)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "0", "1", spec_of("opacity", 1.0, 0.0), 0.5, true);
    EMB_CHECK(tr.n == 0 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_CANCEL);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(zero_combined_duration_starts_nothing)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);

    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 0.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);
    /* A positive delay alone is a transition, and it ends at its delay. */
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 0.0, 0.25), 0.0, false);
    EMB_CHECK(tr.n == 1);
    clear_events(&ev);
    embi_trans_advance(&tr, 0.25, &ev);
    EMB_CHECK(ev.n == 2 && ev.e[0].kind == EMBI_TEV_START && ev.e[1].kind == EMBI_TEV_END);
    EMB_CHECK(tr.n == 0);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

static void scenario(void)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    change(&a, &tr, &ev, "opacity", "0.5", "1", spec_of("opacity", 1.0, 0.0), 0.5, false);
    embi_trans_advance(&tr, 2.0, &ev);
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(memory_returns_to_baseline)
{
    scenario(); /* warms up the static tables */
    size_t before = embi_mem_live();
    scenario();
    EMB_CHECK(embi_mem_live() == before);
}

EMB_TEST(reversing_uses_the_eased_progress)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    /* An ease-in curve: at the midpoint the eased progress is well below one half. */
    embi_trans_spec s = spec_of("opacity", 1.0, 0.0);
    s.easing = anim_easing(&a, "cubic-bezier(0.42, 0, 1, 1)");
    EMB_CHECK(s.easing != nullptr);
    change(&a, &tr, &ev, "opacity", "1", "0", s, 0.0, false);
    /* Back to 1 at 0.5: the shortening factor is the eased output at 0.5, not 0.5 itself. */
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "0.5", "1", s, 0.5, false);
    double eased = embi_easing_eval(s.easing, 0.5);
    EMB_CHECK(tr.n == 1 && anim_near(tr.v[0].duration, eased, 1e-9));
    EMB_CHECK(anim_near(tr.v[0].reversing_factor, eased, 1e-9));
    EMB_CHECK(!anim_near(eased, 0.5, 1e-3));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(double_reversal_is_shortened_again)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    /* 1 -> 0 over 1s, back to 1 at 0.5s (shortened to 0.5s), then back to 0 at 0.75s. That change
     * reverses the reversal: 0 is the reversing-adjusted start of the shortened transition. Its
     * shortening factor is 0.5 * 0.5 + (1 - 0.5) = 0.75, so it lasts 0.75s, not the full 1s. */
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    change(&a, &tr, &ev, "opacity", "0.5", "1", spec_of("opacity", 1.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 1 && anim_near(tr.v[0].duration, 0.5, 1e-9));
    change(&a, &tr, &ev, "opacity", "0.75", "0", spec_of("opacity", 1.0, 0.0), 0.75, false);
    EMB_CHECK(tr.n == 1 && anim_near(tr.v[0].duration, 0.75, 1e-9));
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.75, &v);
    EMB_CHECK(anim_near(v.num, 0.75, 1e-9));
    embi_trans_value(&a, &tr.v[0], 1.5, &v);
    EMB_CHECK(anim_near(v.num, 0.0, 1e-9));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(a_change_to_the_current_value_cancels)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    clear_events(&ev);
    /* The property is already at 0.5 when it changes to 0.5: nothing runs. */
    change(&a, &tr, &ev, "opacity", "0.5", "0.5", spec_of("opacity", 1.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 0 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_CANCEL);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(shorthand_transition_property_matches_its_longhands)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "margin-top", "0px", "10px", spec_of("margin", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1 && tr.v[0].prop == anim_prop("margin-top"));
    /* A longhand the shorthand does not name does not transition. */
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("margin", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 0);
    /* border-radius names its corner longhands. */
    embi_transitions_free(&tr);
    memset(&tr, 0, sizeof tr);
    change(&a, &tr, &ev, "border-top-left-radius", "0px", "4px", spec_of("border-radius", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(values_that_do_not_interpolate_do_not_transition)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    /* auto has no interpolation with a length: no transition, and no run event. */
    change(&a, &tr, &ev, "width", "auto", "100px", spec_of("width", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 0 && ev.n == 0);
    /* A length does transition. */
    change(&a, &tr, &ev, "width", "10px", "100px", spec_of("width", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1 && ev.n == 1 && ev.e[0].kind == EMBI_TEV_RUN);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(structurally_equal_calc_values_do_not_restart)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "width", "0px", "calc(10px + 50%)", spec_of("width", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1);
    clear_events(&ev);
    /* A restyle that recomputes the same calc() in a new node is the same target. */
    change(&a, &tr, &ev, "width", "calc(10px + 50%)", "calc(10px + 50%)", spec_of("width", 1.0, 0.0), 0.5, false);
    EMB_CHECK(ev.n == 0 && tr.n == 1);
    EMB_CHECK(anim_near(tr.v[0].duration, 1.0, 1e-9));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(different_calc_end_values_cancel_and_start_a_new_run)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "width", "0px", "calc(10px + 50%)", spec_of("width", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1);
    clear_events(&ev);
    /* The end value is replaced by a different calc(): cancel, then run again. */
    change(&a, &tr, &ev, "width", "calc(10px + 50%)", "calc(20px + 50%)", spec_of("width", 1.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 1);
    EMB_CHECK(ev.n >= 2 && ev.e[0].kind == EMBI_TEV_CANCEL && ev.e[ev.n - 1].kind == EMBI_TEV_RUN);
    EMB_CHECK(tr.v[0].to.kind == EMBI_V_CALC && anim_near(anim_px(&tr.v[0].to), 120.0, 1e-9));
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(different_calc_target_replaces_a_running_calc_current_value)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "width", "0px", "calc(10px + 50%)", spec_of("width", 1.0, 0.0), 0.0, false);
    EMB_CHECK(tr.n == 1);
    embi_value cur;
    embi_trans_value(&a, &tr.v[0], 0.5, &cur);
    EMB_CHECK(cur.kind == EMBI_V_CALC);
    clear_events(&ev);
    /* The before-change value is the running (calc) current value. */
    embi_transition old = tr.v[0];
    embi_value after = anim_parse(&a, "width", "calc(20px + 50%)", &(bool){false});
    embi_trans_spec spec = spec_of("width", 1.0, 0.0);
    embi_trans_change ch = {anim_prop("width"), &cur, &after};
    embi_trans_input in = {&spec, 1u, &ch, 1u, false, nullptr};
    embi_trans_update(&a, &tr, &in, 0.5, &ev);
    EMB_CHECK(tr.n == 1);
    EMB_CHECK(ev.n >= 2 && ev.e[0].kind == EMBI_TEV_CANCEL && ev.e[ev.n - 1].kind == EMBI_TEV_RUN);
    EMB_CHECK(anim_near(anim_px(&tr.v[0].to), 120.0, 1e-9));
    EMB_CHECK(anim_px(&old.to) != anim_px(&tr.v[0].to));
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(negative_delay_start_elapsed_time)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    /* A -0.25s delay on a 1s transition: it has already run 0.25s when it starts. */
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, -0.25), 0.0, false);
    clear_events(&ev);
    embi_trans_advance(&tr, 0.0, &ev);
    EMB_CHECK(ev.n == 1 && ev.e[0].kind == EMBI_TEV_START && anim_near(ev.e[0].elapsed, 0.25, 1e-9));

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(currentcolor_transitions_mix_with_the_element_color)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    bool ok1, ok2;
    embi_value before = anim_parse(&a, "border-top-color", "currentcolor", &ok1);
    embi_value after = anim_parse(&a, "border-top-color", "red", &ok2);
    embi_value blue = anim_parse(&a, "color", "blue", &ok1);
    EMB_CHECK(ok1 && ok2);
    embi_trans_change ch = {anim_prop("border-top-color"), &before, &after};
    embi_trans_spec spec = spec_of("border-top-color", 1.0, 0.0);
    embi_trans_input in = {&spec, 1, &ch, 1, false, &blue.color};
    embi_trans_update(&a, &tr, &in, 0.0, &ev);
    EMB_CHECK(tr.n == 1);
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.5, &v);
    /* A mix of blue and red keeps some blue; a discrete flip would be red. */
    EMB_CHECK(v.kind == EMBI_V_COLOR && v.color.c[2] > 0.0 && v.color.c[0] < 1.0);

    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(display_block_to_none_fades_out_with_allow_discrete)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    embi_trans_spec s = spec_of("display", 1.0, 0.0);
    s.allow_discrete = true;
    change(&a, &tr, &ev, "display", "block", "none", s, 0.0, false);
    EMB_CHECK(tr.n == 1);
    embi_value v;
    embi_trans_value(&a, &tr.v[0], 0.75, &v);
    EMB_CHECK(v.kw == EMBI_KW_block);
    embi_trans_value(&a, &tr.v[0], 1.0, &v);
    EMB_CHECK(v.kw == EMBI_KW_none);
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST(same_end_value_with_zero_duration_leaves_the_running_transition)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_transitions tr;
    memset(&tr, 0, sizeof tr);
    embi_trans_events ev;
    memset(&ev, 0, sizeof ev);
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 1.0, 0.0), 0.0, false);
    clear_events(&ev);
    change(&a, &tr, &ev, "opacity", "1", "0", spec_of("opacity", 0.0, 0.0), 0.5, false);
    EMB_CHECK(tr.n == 1 && ev.n == 0);
    embi_transitions_free(&tr);
    embi_trans_events_free(&ev);
    embi_arena_free(&a);
}

EMB_TEST_MAIN()
