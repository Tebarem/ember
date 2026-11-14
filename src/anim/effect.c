/*
 * Effect sampling (W6a): the animated values of an element at a time. CSS and Web animations are
 * composited in list order, each on the value below it (replace, add or accumulate); transitions
 * come last and override the animations, because the transition origin sits above the animation
 * origin in the cascade. An animation of a property with an !important author declaration is
 * skipped; transitions still apply.
 */
#include "anim/anim.h"

#include <string.h>

static void ov_set(embi_overlay *o, embi_prop_id p, const embi_value *v)
{
    for (uint32_t i = 0; i < o->n; i++) {
        if (o->e[i].prop == p) {
            o->e[i].value = *v;
            return;
        }
    }
    if (o->n == o->cap) {
        uint32_t nc = o->cap == 0 ? 8u : o->cap * 2u;
        o->e = embi_realloc(o->e, sizeof(embi_overlay_entry) * o->cap, sizeof(embi_overlay_entry) * nc);
        o->cap = nc;
    }
    o->e[o->n].prop = p;
    o->e[o->n].value = *v;
    o->n++;
}

const embi_value *embi_overlay_find(const embi_overlay *o, embi_prop_id prop)
{
    for (uint32_t i = 0; i < o->n; i++) {
        if (o->e[i].prop == prop) {
            return &o->e[i].value;
        }
    }
    return nullptr;
}

void embi_overlay_free(embi_overlay *o)
{
    embi_free(o->e, sizeof(embi_overlay_entry) * o->cap);
    memset(o, 0, sizeof *o);
}

/* The underlying value of a property for this effect: the value of the composition so far, or the
 * base computed value. */
static const embi_value *underlying_of(const embi_anim_env *env, const embi_overlay *o, embi_prop_id p)
{
    const embi_value *v = embi_overlay_find(o, p);
    if (v == nullptr && env->base != nullptr) {
        v = env->base(env->user, p);
    }
    return v;
}

/* The effect's current time: the timeline time through its start and rate, or the held time. */
static double current_time(const embi_anim_effect *e, double now)
{
    return e->held ? e->hold : (now - e->start) * e->timing.rate;
}

/* The iteration composite of one property (iterationComposite: accumulate): v + iteration * (last -
 * first), where first and last are the keyframe values at 0% and 100%. False when the values do not
 * add (the caller then keeps v). */
static bool accumulate(embi_arena *a, embi_prop_id p, const embi_kf_set *kf, const embi_easing *def,
                       const embi_value *under, const embi_color *cur, uint8_t comp, double iteration,
                       embi_value *v)
{
    embi_value first, last, delta, scaled;
    if (!embi_kf_sample_c(kf, p, 0.0, under, def, cur, a, comp, &first) ||
        !embi_kf_sample_c(kf, p, 1.0, under, def, cur, a, comp, &last)) {
        return false;
    }
    if (!embi_anim_add_scaled(a, p, &last, &first, -1.0, &delta) ||
        !embi_anim_add_scaled(a, p, v, &delta, iteration, &scaled)) {
        return false;
    }
    *v = scaled;
    return true;
}

void embi_anim_sample_effects(embi_arena *a, const embi_anim_env *env,
                              const embi_anim_effect *effects, uint32_t n,
                              const embi_transitions *tr, double now, embi_overlay *out)
{
    out->n = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_anim_effect *fx = &effects[i];
        if (fx->kf == nullptr) {
            continue;
        }
        embi_anim_sample s;
        embi_anim_timing_sample(&fx->timing, current_time(fx, now), &s);
        if (!s.in_effect) {
            continue;
        }
        uint32_t np = embi_kf_prop_count(fx->kf);
        for (uint32_t k = 0; k < np; k++) {
            embi_prop_id p = embi_kf_prop_at(fx->kf, k);
            if (env->important != nullptr && env->important(env->user, p)) {
                continue;
            }
            /* The underlying value: the composition so far, or the base computed value. */
            const embi_value *under = underlying_of(env, out, p);
            embi_value under_buf, v;
            if (under != nullptr && env->current != nullptr) {
                embi_anim_resolve_color(a, under, env->current, &under_buf);
                under = &under_buf;
            }
            if (!embi_kf_sample_c(fx->kf, p, s.progress, under, fx->keyframe_easing, env->current, a, fx->composite,
                                  &v)) {
                continue;
            }
            /* The iteration composite adds the iteration index times the keyframe span before the
             * value is combined with the underlying one. A value that does not add keeps v. */
            if (fx->iteration_composite == EMBI_COMP_ACCUMULATE && s.iteration != 0.0) {
                (void)accumulate(a, p, fx->kf, fx->keyframe_easing, under, env->current, fx->composite,
                                 s.iteration, &v);
            }
            /* composite: the keyframe values were composited onto the underlying value while sampling. */
            ov_set(out, p, &v);
        }
    }
    if (tr != nullptr) {
        for (uint32_t i = 0; i < tr->n; i++) {
            embi_value v;
            embi_trans_value(a, &tr->v[i], now, &v);
            ov_set(out, tr->v[i].prop, &v);
        }
    }
}
