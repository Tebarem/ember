/*
 * Timing functions and the Web Animations timing model (W6a): cubic-bezier with Newton-Raphson
 * and bisection, steps() with all four jump terms, linear() with stops, and the phases, iteration
 * index and directed progress of an effect at a local time.
 */
#include "anim/anim.h"

#include <math.h>

static double bezier_x(double x1, double x2, double t)
{
    double u = 1.0 - t;
    return 3.0 * u * u * t * x1 + 3.0 * u * t * t * x2 + t * t * t;
}

static double bezier_dx(double x1, double x2, double t)
{
    double u = 1.0 - t;
    return 3.0 * u * u * x1 + 6.0 * u * t * (x2 - x1) + 3.0 * t * t * (1.0 - x2);
}

/* The curve parameter t with bezier_x(t) == x: Newton-Raphson from x, bisection when the slope
 * is too small or Newton does not converge. */
static double bezier_solve(double x1, double x2, double x)
{
    double t = x;
    for (int i = 0; i < 8; i++) {
        double err = bezier_x(x1, x2, t) - x;
        if (fabs(err) < 1e-7) {
            return t;
        }
        double d = bezier_dx(x1, x2, t);
        if (fabs(d) < 1e-6) {
            break;
        }
        t -= err / d;
        if (t < 0.0 || t > 1.0) {
            break;
        }
    }
    double lo = 0.0, hi = 1.0;
    t = x;
    for (int i = 0; i < 100; i++) {
        double err = bezier_x(x1, x2, t) - x;
        if (fabs(err) < 1e-7) {
            break;
        }
        if (err < 0.0) {
            lo = t;
        } else {
            hi = t;
        }
        t = 0.5 * (lo + hi);
    }
    return t;
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* CSS Easing, the steps() output progress. before is the "before flag": at an input that lies on a jump the value before the
 * jump is returned (the Web Animations before phase going forwards, the after phase going backwards). */
static double steps_eval(const embi_easing *e, double x, bool before)
{
    double n = (double)e->steps;
    double cur = floor(x * n);
    double jumps = n;
    switch (e->jump) {
    case EMBI_JUMP_START:
        cur += 1.0;
        break;
    case EMBI_JUMP_NONE:
        jumps = n - 1.0;
        break;
    case EMBI_JUMP_BOTH:
        cur += 1.0;
        jumps = n + 1.0;
        break;
    default: /* EMBI_JUMP_END */
        break;
    }
    if (before && x * n == floor(x * n)) {
        cur -= 1.0;
    }
    if (x >= 0.0 && cur < 0.0) {
        cur = 0.0;
    }
    if (jumps <= 0.0) {
        return clamp01(cur > 0.0 ? 1.0 : 0.0);
    }
    if (x <= 1.0 && cur > jumps) {
        cur = jumps;
    }
    return clamp01(cur / jumps);
}

static double linear_eval(const embi_easing *e, double x)
{
    if (e->npts == 0) {
        return x;
    }
    const embi_easing_pt *p = e->pts;
    if (x < p[0].in) {
        return p[0].out;
    }
    for (uint32_t i = 0; i + 1 < e->npts; i++) {
        if (x < p[i + 1].in) {
            double w = p[i + 1].in - p[i].in;
            if (w <= 0.0) {
                return p[i + 1].out;
            }
            return p[i].out + (p[i + 1].out - p[i].out) * (x - p[i].in) / w;
        }
    }
    return p[e->npts - 1].out;
}

double embi_easing_eval(const embi_easing *e, double x)
{
    return embi_easing_eval_flag(e, x, false);
}

double embi_easing_eval_flag(const embi_easing *e, double x, bool before)
{
    if (e == nullptr) {
        return x;
    }
    switch (e->kind) {
    case EMBI_EASE_CUBIC:
        if (x <= 0.0) {
            return 0.0;
        }
        if (x >= 1.0) {
            return 1.0;
        }
        if (e->c[0] == e->c[1] && e->c[2] == e->c[3]) {
            return x; /* a linear-equivalent curve is exact */
        }
        return bezier_x(e->c[1], e->c[3], bezier_solve(e->c[0], e->c[2], x));
    case EMBI_EASE_STEPS:
        return steps_eval(e, x, before);
    case EMBI_EASE_LINEAR_FN:
        return linear_eval(e, x);
    default:
        return x;
    }
}

embi_anim_timing embi_anim_timing_default(void)
{
    embi_anim_timing t = {0};
    t.iterations = 1.0;
    t.rate = 1.0;
    t.direction = EMBI_AD_NORMAL;
    t.fill = 0;
    return t;
}

void embi_anim_timing_sample(const embi_anim_timing *t, double local, embi_anim_sample *out)
{
    double d = t->duration;
    double n = t->iterations;
    double active = (d == 0.0 || n == 0.0) ? 0.0 : d * n;
    /* The end time includes the end delay, which does not move the phases except through the
     * boundaries: before-active ends at min(delay, end time), after-active at
     * max(min(delay + active, end time), 0). A negative end delay therefore ends the active
     * interval early. */
    double end_time = fmax(t->delay + active + t->end_delay, 0.0);
    double before_b = fmax(fmin(t->delay, end_time), 0.0);
    double after_b = fmax(fmin(t->delay + active, end_time), 0.0);

    uint8_t phase;
    double active_time;
    /* Web Animations, "animation direction": a negative playback rate runs backwards. At a boundary the phase is the one the
     * animation comes from (before at the start going backwards, after at the end going forwards). The times are seconds
     * made of milliseconds, so values closer than a nanosecond are the same boundary. */
    const double eps = 1e-9;
    bool backwards = t->rate < 0.0;
    if (local < before_b - eps || (backwards && local <= before_b + eps)) {
        phase = EMBI_PH_BEFORE;
        active_time = 0.0;
    } else if (local > after_b + eps || (!backwards && local >= after_b - eps)) {
        phase = EMBI_PH_AFTER;
        /* Web Animations, calculating the active time: max(min(local - start delay, active
         * duration), 0). */
        active_time = fmin(local - t->delay, active);
        if (active_time >= active - eps) {
            active_time = active;
        }
        active_time = fmax(active_time, 0.0);
    } else {
        phase = EMBI_PH_ACTIVE;
        active_time = local - t->delay;
        if (active_time > active) {
            active_time = active;
        }
        if (active_time < 0.0) {
            active_time = 0.0;
        }
    }

    out->phase = phase;
    out->iteration = 0.0;
    out->progress = 0.0;
    out->in_effect = phase == EMBI_PH_ACTIVE ||
                     (phase == EMBI_PH_BEFORE && (t->fill & EMBI_FILL_BACKWARDS) != 0) ||
                     (phase == EMBI_PH_AFTER && (t->fill & EMBI_FILL_FORWARDS) != 0);
    /* Web Animations, "calculating the overall progress", the simple iteration progress and the current iteration. An effect
     * whose active duration is zero is complete at its end (its overall progress is its iteration count after the before
     * phase); the iteration start is added to the progress. */
    bool active_zero = d == 0.0 || n == 0.0;
    double ov0;
    if (active_zero) {
        ov0 = phase == EMBI_PH_BEFORE ? 0.0 : n;
    } else {
        /* at the end of the active interval the quotient is exactly the iteration count (no rounding error of the division) */
        ov0 = active_time >= active ? n : active_time / d;
    }
    double ov = ov0 + t->iteration_start;
    double frac;
    if (isinf(ov)) {
        frac = t->iteration_start - floor(t->iteration_start);
    } else {
        frac = ov - floor(ov);
    }
    bool at_end = false;
    if (frac == 0.0 && phase != EMBI_PH_BEFORE && active_time == active && n != 0.0) {
        frac = 1.0;
        at_end = true;
    }
    double idx;
    if (phase == EMBI_PH_AFTER && isinf(n)) {
        idx = INFINITY;
    } else if (isinf(ov)) {
        idx = INFINITY;
    } else {
        idx = floor(ov) - (at_end ? 1.0 : 0.0);
    }
    if (idx < 0.0) {
        idx = 0.0;
    }

    bool odd = isfinite(idx) && fmod(idx, 2.0) != 0.0;
    double p;
    switch (t->direction) {
    case EMBI_AD_REVERSE:
        p = 1.0 - frac;
        break;
    case EMBI_AD_ALTERNATE:
        p = odd ? 1.0 - frac : frac;
        break;
    case EMBI_AD_ALTERNATE_REVERSE:
        p = odd ? frac : 1.0 - frac;
        break;
    default:
        p = frac;
        break;
    }
    out->iteration = idx;
    /* Web Animations, "calculating the transformed progress": the before flag is set in the before phase going forwards and
     * in the after phase going backwards, so that a step function at a jump gives the value before it. */
    bool fwd = t->direction == EMBI_AD_NORMAL || (t->direction == EMBI_AD_ALTERNATE && !odd) ||
               (t->direction == EMBI_AD_ALTERNATE_REVERSE && odd);
    bool before_flag = (fwd && phase == EMBI_PH_BEFORE) || (!fwd && phase == EMBI_PH_AFTER);
    out->progress = embi_easing_eval_flag(t->easing, clamp01(p), before_flag);
}

/* ------------------------------------------------------------------ easing keywords */

static const embi_easing k_ease = {.kind = EMBI_EASE_CUBIC, .c = {0.25, 0.1, 0.25, 1.0}};
static const embi_easing k_ease_in = {.kind = EMBI_EASE_CUBIC, .c = {0.42, 0.0, 1.0, 1.0}};
static const embi_easing k_ease_out = {.kind = EMBI_EASE_CUBIC, .c = {0.0, 0.0, 0.58, 1.0}};
static const embi_easing k_ease_in_out = {.kind = EMBI_EASE_CUBIC, .c = {0.42, 0.0, 0.58, 1.0}};
static const embi_easing k_step_start = {.kind = EMBI_EASE_STEPS, .jump = EMBI_JUMP_START, .steps = 1};
static const embi_easing k_step_end = {.kind = EMBI_EASE_STEPS, .jump = EMBI_JUMP_END, .steps = 1};

bool embi_easing_of_value(const embi_value *v, const embi_easing **out)
{
    *out = nullptr;
    if (v == nullptr) { /* no value: the initial value of transition-timing-function and animation-timing-function */
        *out = &k_ease;
        return true;
    }
    if (v->kind == EMBI_V_EASING) {
        *out = v->easing;
        return true;
    }
    if (v->kind != EMBI_V_KEYWORD) {
        return false;
    }
    switch (v->kw) {
    case EMBI_KW_linear:
        return true;
    case EMBI_KW_ease:
        *out = &k_ease;
        return true;
    case EMBI_KW_ease_in:
        *out = &k_ease_in;
        return true;
    case EMBI_KW_ease_out:
        *out = &k_ease_out;
        return true;
    case EMBI_KW_ease_in_out:
        *out = &k_ease_in_out;
        return true;
    case EMBI_KW_step_start:
        *out = &k_step_start;
        return true;
    case EMBI_KW_step_end:
        *out = &k_step_end;
        return true;
    default:
        return false;
    }
}
