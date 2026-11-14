/*
 * Easing curves and the Web Animations timing model (W6a): cubic-bezier, steps with all four jump
 * terms, linear() with stops, and the phase, iteration and direction table of an effect.
 */
#include "emb_test.h"

#include "anim_fixture.h"

static embi_easing make_steps(uint32_t n, uint8_t jump)
{
    embi_easing e;
    memset(&e, 0, sizeof e);
    e.kind = EMBI_EASE_STEPS;
    e.steps = n;
    e.jump = jump;
    return e;
}

EMB_TEST(cubic_bezier_curves)
{
    embi_arena a;
    anim_arena_open(&a);
    const embi_easing *ease = anim_easing(&a, "cubic-bezier(0.42, 0, 0.58, 1)");
    EMB_CHECK(ease != nullptr);
    EMB_CHECK(anim_near(embi_easing_eval(ease, 0.0), 0.0, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(ease, 1.0), 1.0, 1e-9));
    /* The ease-in-out curve is symmetric about (0.5, 0.5). */
    EMB_CHECK(anim_near(embi_easing_eval(ease, 0.5), 0.5, 1e-4));
    EMB_CHECK(embi_easing_eval(ease, 0.25) < 0.5 && embi_easing_eval(ease, 0.75) > 0.5);
    /* A linear cubic-bezier is the identity. */
    const embi_easing *lin = anim_easing(&a, "cubic-bezier(0, 0, 1, 1)");
    EMB_CHECK(lin != nullptr);
    EMB_CHECK(anim_near(embi_easing_eval(lin, 0.3), 0.3, 1e-6));
    /* A null easing is linear. */
    EMB_CHECK(anim_near(embi_easing_eval(nullptr, 0.7), 0.7, 1e-12));
    embi_arena_free(&a);
}

EMB_TEST(steps_with_every_jump_term)
{
    embi_easing e = make_steps(4, EMBI_JUMP_END);
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.24), 0.0, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.25), 0.25, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.5), 0.5, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.99), 0.75, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 1.0), 1.0, 1e-9));

    e = make_steps(4, EMBI_JUMP_START);
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.0), 0.25, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.5), 0.75, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 1.0), 1.0, 1e-9));

    e = make_steps(4, EMBI_JUMP_NONE);
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.0), 0.0, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.5), 2.0 / 3.0, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 1.0), 1.0, 1e-9));

    e = make_steps(4, EMBI_JUMP_BOTH);
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.0), 0.2, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.5), 0.6, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 1.0), 1.0, 1e-9));
}

EMB_TEST(linear_with_stops)
{
    embi_easing_pt pts[3] = {{0.0, 0.0}, {0.5, 1.0}, {1.0, 1.0}};
    embi_easing e;
    memset(&e, 0, sizeof e);
    e.kind = EMBI_EASE_LINEAR_FN;
    e.npts = 3;
    e.pts = pts;
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.25), 0.5, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.75), 1.0, 1e-9));
    EMB_CHECK(anim_near(embi_easing_eval(&e, 1.0), 1.0, 1e-9));
    /* A vertical step: the later point wins at the shared input. */
    embi_easing_pt jump[4] = {{0.0, 0.0}, {0.5, 1.0}, {0.5, 0.0}, {1.0, 1.0}};
    e.npts = 4;
    e.pts = jump;
    EMB_CHECK(anim_near(embi_easing_eval(&e, 0.5), 0.0, 1e-9));
}

EMB_TEST(timing_phases_and_iterations)
{
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 2.0;
    t.delay = 0.5;
    embi_anim_sample s;

    embi_anim_timing_sample(&t, 0.2, &s);
    EMB_CHECK(s.phase == EMBI_PH_BEFORE && !s.in_effect);

    embi_anim_timing_sample(&t, 1.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && s.in_effect && s.iteration == 0.0);
    EMB_CHECK(anim_near(s.progress, 0.5, 1e-9));

    embi_anim_timing_sample(&t, 2.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && s.iteration == 1.0 && anim_near(s.progress, 0.5, 1e-9));

    embi_anim_timing_sample(&t, 3.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && !s.in_effect && s.iteration == 1.0);

    /* The fills cover the phases they name. */
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_timing_sample(&t, 3.0, &s);
    EMB_CHECK(s.in_effect && anim_near(s.progress, 1.0, 1e-9));
    t.fill = EMBI_FILL_BACKWARDS;
    embi_anim_timing_sample(&t, 0.2, &s);
    EMB_CHECK(s.in_effect && anim_near(s.progress, 0.0, 1e-9));
    t.fill = 0;
}

EMB_TEST(direction_alternate_and_reverse)
{
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 2.0;
    embi_anim_sample s;

    t.direction = EMBI_AD_REVERSE;
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(anim_near(s.progress, 0.75, 1e-9));

    t.direction = EMBI_AD_ALTERNATE;
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(anim_near(s.progress, 0.25, 1e-9));
    embi_anim_timing_sample(&t, 1.25, &s);
    EMB_CHECK(s.iteration == 1.0 && anim_near(s.progress, 0.75, 1e-9));

    t.direction = EMBI_AD_ALTERNATE_REVERSE;
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(anim_near(s.progress, 0.75, 1e-9));
    embi_anim_timing_sample(&t, 1.25, &s);
    EMB_CHECK(anim_near(s.progress, 0.25, 1e-9));
}

EMB_TEST(fractional_and_infinite_iterations)
{
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.5;
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_sample s;
    embi_anim_timing_sample(&t, 1.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.iteration == 1.0 && anim_near(s.progress, 0.5, 1e-9));

    t.iterations = INFINITY;
    t.fill = 0;
    embi_anim_timing_sample(&t, 1e6 + 0.25, &s);
    EMB_CHECK(s.in_effect && s.iteration == 1000000.0 && anim_near(s.progress, 0.25, 1e-6));
}

EMB_TEST(iteration_start_delay_and_zero_duration)
{
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.0;
    t.iteration_start = 0.5;
    embi_anim_sample s;
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(anim_near(s.progress, 0.5, 1e-9));
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(anim_near(s.progress, 0.75, 1e-9));
    /* The single iteration spans overall 0.5 to 1.5: the second half is iteration 1. */
    embi_anim_timing_sample(&t, 0.5, &s);
    EMB_CHECK(s.iteration == 1.0 && anim_near(s.progress, 0.0, 1e-9));

    /* A negative delay starts the effect partway through. */
    t = embi_anim_timing_default();
    t.duration = 2.0;
    t.iterations = 1.0;
    t.delay = -1.0;
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && anim_near(s.progress, 0.5, 1e-9));

    /* Zero duration completes at its end, which is the delay. */
    t = embi_anim_timing_default();
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && anim_near(s.progress, 1.0, 1e-9));
}

EMB_TEST(easing_applies_to_the_iteration)
{
    embi_arena a;
    anim_arena_open(&a);
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.0;
    t.easing = anim_easing(&a, "steps(2, jump-none)");
    embi_anim_sample s;
    embi_anim_timing_sample(&t, 0.25, &s);
    /* steps(2, jump-none) on 0.25: floor(0.5) = 0 over 1 jump. */
    EMB_CHECK(anim_near(s.progress, 0.0, 1e-9));
    embi_anim_timing_sample(&t, 0.75, &s);
    EMB_CHECK(anim_near(s.progress, 1.0, 1e-9));
    embi_arena_free(&a);
}

EMB_TEST(end_delay_moves_the_after_boundary)
{
    embi_anim_sample s;
    /* A positive end delay keeps the active interval: the after phase starts at 1s. */
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.0;
    t.end_delay = 0.5;
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_timing_sample(&t, 0.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && anim_near(s.progress, 0.5, 1e-9));
    embi_anim_timing_sample(&t, 1.2, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && anim_near(s.progress, 1.0, 1e-9));

    /* A negative end delay shortens the active interval: the after phase starts at 0.5s, and the
     * active time there is min(local - delay, active duration) (Web Animations). */
    t.end_delay = -0.5;
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && anim_near(s.progress, 0.25, 1e-9));
    embi_anim_timing_sample(&t, 0.49, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && anim_near(s.progress, 0.49, 1e-9));
    embi_anim_timing_sample(&t, 0.6, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && anim_near(s.progress, 0.6, 1e-9));
    t.fill = 0;
    embi_anim_timing_sample(&t, 0.6, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && !s.in_effect);

    /* With a delay: before-active ends at the delay, after-active at delay + active + end delay. */
    t.delay = 0.25;
    t.end_delay = -0.5;
    t.fill = EMBI_FILL_BOTH;
    embi_anim_timing_sample(&t, 0.2, &s);
    EMB_CHECK(s.phase == EMBI_PH_BEFORE && s.in_effect && anim_near(s.progress, 0.0, 1e-9));
    embi_anim_timing_sample(&t, 0.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && anim_near(s.progress, 0.25, 1e-9));
    embi_anim_timing_sample(&t, 0.8, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && anim_near(s.progress, 0.55, 1e-9));

    /* An end delay that makes the end time negative clamps it at zero: the effect is over at once. */
    t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.0;
    t.end_delay = -2.0;
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect);
}

EMB_TEST(timing_phase_table_with_end_delay_and_fills)
{
    /* Each row: delay, end delay, local time, expected phase, expected progress with fill both. */
    struct row {
        double delay, end_delay, local;
        uint8_t phase;
        double progress;
    } rows[] = {
        {0.0, 0.0, -0.1, EMBI_PH_BEFORE, 0.0},
        {0.0, 0.0, 0.0, EMBI_PH_ACTIVE, 0.0},
        {0.0, 0.5, 0.75, EMBI_PH_ACTIVE, 0.75},
        {0.0, 0.5, 1.25, EMBI_PH_AFTER, 1.0},
        {0.0, -0.5, 0.75, EMBI_PH_AFTER, 0.75},
        {0.5, -0.25, 0.25, EMBI_PH_BEFORE, 0.0},
        {0.5, -0.25, 0.7, EMBI_PH_ACTIVE, 0.2},
        {0.5, -0.25, 1.3, EMBI_PH_AFTER, 0.8},
    };
    for (uint32_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        embi_anim_timing t = embi_anim_timing_default();
        t.duration = 1.0;
        t.iterations = 1.0;
        t.delay = rows[i].delay;
        t.end_delay = rows[i].end_delay;
        t.fill = EMBI_FILL_BOTH;
        embi_anim_sample s;
        embi_anim_timing_sample(&t, rows[i].local, &s);
        EMB_CHECK(s.phase == rows[i].phase);
        EMB_CHECK(anim_near(s.progress, rows[i].progress, 1e-9));
    }
}

EMB_TEST(iteration_start_phase_arithmetic)
{
    embi_anim_sample s;
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 1.0;
    t.fill = EMBI_FILL_FORWARDS;

    /* The iteration start 2 with one iteration ends at overall progress 3: the simple progress is 1.0 (not 0) and the
     * current iteration the one that just ended, 2. */
    t.iteration_start = 2.0;
    embi_anim_timing_sample(&t, 5.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.iteration == 2.0 && anim_near(s.progress, 1.0, 1e-12));
    /* Before the end: progress 0 of iteration 2 at the start of the active interval. */
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && s.iteration == 2.0 && anim_near(s.progress, 0.0, 1e-12));

    /* A fractional start ends mid iteration: overall 2.5, iteration 2, progress 0.5. */
    t.iteration_start = 1.5;
    embi_anim_timing_sample(&t, 5.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.iteration == 2.0 && anim_near(s.progress, 0.5, 1e-12));

    /* The before phase with a backwards fill shows the iteration start, whatever the iteration count. */
    t.iteration_start = 0.25;
    t.delay = 1.0;
    t.fill = EMBI_FILL_BOTH;
    embi_anim_timing_sample(&t, 0.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_BEFORE && s.in_effect && s.iteration == 0.0 && anim_near(s.progress, 0.25, 1e-12));

    /* Infinite iterations: the progress is iteration start modulo 1 at the unreachable end, and mid-run it counts on. */
    t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = INFINITY;
    t.iteration_start = 0.25;
    embi_anim_timing_sample(&t, 10.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE && s.iteration == 10.0 && anim_near(s.progress, 0.75, 1e-9));
}

EMB_TEST(zero_iterations_and_zero_active_duration)
{
    embi_anim_sample s;
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.iterations = 0.0;
    t.fill = EMBI_FILL_FORWARDS;
    /* Zero iterations: the active duration is 0, so the effect is complete at once at progress iteration start % 1. */
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && s.iteration == 0.0 && anim_near(s.progress, 0.0, 1e-12));
    t.iteration_start = 0.5;
    embi_anim_timing_sample(&t, 3.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && s.iteration == 0.0 && anim_near(s.progress, 0.5, 1e-12));
    /* Without a fill nothing applies. */
    t.fill = 0;
    embi_anim_timing_sample(&t, 3.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && !s.in_effect);

    /* A zero duration with three iterations ends on the last iteration at progress 1. */
    t = embi_anim_timing_default();
    t.duration = 0.0;
    t.iterations = 3.0;
    t.fill = EMBI_FILL_FORWARDS;
    embi_anim_timing_sample(&t, 0.0, &s);
    EMB_CHECK(s.phase == EMBI_PH_AFTER && s.in_effect && s.iteration == 2.0 && anim_near(s.progress, 1.0, 1e-12));
    /* Backwards, before the delay, the zero duration effect shows its start. */
    t.delay = 1.0;
    t.fill = EMBI_FILL_BACKWARDS;
    embi_anim_timing_sample(&t, 0.5, &s);
    EMB_CHECK(s.phase == EMBI_PH_BEFORE && s.in_effect && s.iteration == 0.0 && anim_near(s.progress, 0.0, 1e-12));
}

EMB_TEST(negative_delay_before_phase_boundary)
{
    embi_anim_timing t = embi_anim_timing_default();
    t.duration = 1.0;
    t.delay = -0.5;
    embi_anim_sample s;
    embi_anim_timing_sample(&t, -0.25, &s);
    EMB_CHECK(s.phase == EMBI_PH_BEFORE && !s.in_effect);
    embi_anim_timing_sample(&t, 0.25, &s);
    EMB_CHECK(s.phase == EMBI_PH_ACTIVE);
}

EMB_TEST_MAIN()
