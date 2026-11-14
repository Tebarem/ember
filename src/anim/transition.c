/*
 * CSS Transitions (W6a): the start algorithm as a pure function of the changes of one restyle and
 * the running transitions (CSS Transitions 2 section 3). Events (run, start, end, cancel) are
 * returned to the caller, which dispatches them (W6c).
 */
#include "anim/anim.h"

#include <math.h>
#include <string.h>

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* A transition-property entry names a property when they are equal, or when the entry is a
 * shorthand and the property is one of its longhands (recursively). */
static bool names_prop(embi_prop_id name, embi_prop_id p, int depth)
{
    if (name == p) {
        return true;
    }
    if (depth > 8 || !embi_prop_is_shorthand(name)) {
        return false;
    }
    const embi_prop_info *pi = embi_prop_info_of(name);
    const uint16_t *lh = embi_prop_longhands(name);
    for (uint16_t i = 0; lh != nullptr && i < pi->sub_n; i++) {
        if (names_prop(lh[i], p, depth + 1)) {
            return true;
        }
    }
    return false;
}

/* The last matching entry in the list gives the property its timing. all matches every animatable
 * longhand. */
static const embi_trans_spec *spec_for(const embi_trans_input *in, embi_prop_id p)
{
    const embi_trans_spec *found = nullptr;
    for (uint32_t i = 0; i < in->nspecs; i++) {
        const embi_trans_spec *s = &in->specs[i];
        bool match = s->all ? embi_anim_animatable(p) : names_prop(s->prop, p, 0);
        if (match) {
            found = s;
        }
    }
    return found;
}

/* Discrete properties (visibility excepted: it transitions with its visible end) only transition
 * with transition-behavior: allow-discrete. */
static bool needs_allow_discrete(embi_prop_id p)
{
    const embi_prop_info *pi = embi_prop_info_of(p);
    return pi != nullptr && pi->anim == 'D' && pi->interp != EMBI_IP_VIS;
}

/* Whether a change from one value to another starts a transition. Visibility always does. Other
 * discrete properties do only with allow-discrete, and every other property needs its two values to
 * interpolate (a change from auto to a length does not transition). */
static bool starts_for(embi_arena *a, embi_prop_id p, const embi_trans_spec *s,
                       const embi_value *from, const embi_value *to)
{
    const embi_prop_info *pi = embi_prop_info_of(p);
    if (pi == nullptr) {
        return false;
    }
    if (pi->interp == EMBI_IP_VIS) {
        return true;
    }
    if (pi->anim == 'D') {
        return s->allow_discrete;
    }
    return embi_anim_can_mix(a, p, from, to);
}

/* Structural equality of two computed values. The serializer prints every calc() as "calc()",
 * so calc trees, lists, positions, shadows, filters, transforms and shapes compare field by
 * field; only kinds that are never animatable fall back to their serialization. */
static bool value_eq(const embi_value *x, const embi_value *y);

static bool calc_eq(const embi_calc *p, const embi_calc *q)
{
    if (p == q) {
        return true;
    }
    if (p == nullptr || q == nullptr || p->op != q->op || p->type != q->type ||
        p->unit != q->unit || p->mode != q->mode || p->num != q->num || p->n != q->n) {
        return false;
    }
    for (uint32_t i = 0; i < p->n; i++) {
        if (!calc_eq(p->kids[i], q->kids[i])) {
            return false;
        }
    }
    return true;
}

static bool vals_eq(const embi_value *x, const embi_value *y, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (!value_eq(&x[i], &y[i])) {
            return false;
        }
    }
    return true;
}

static bool axis_eq(const embi_pos_axis *x, const embi_pos_axis *y)
{
    return x->anchor == y->anchor && value_eq(&x->off, &y->off);
}

static bool pos_eq(const embi_position *x, const embi_position *y)
{
    return axis_eq(&x->x, &y->x) && axis_eq(&x->y, &y->y) && value_eq(&x->z, &y->z);
}

static bool shadow_eq(const embi_shadow *x, const embi_shadow *y)
{
    return x->inset == y->inset && value_eq(&x->color, &y->color) && value_eq(&x->x, &y->x) &&
           value_eq(&x->y, &y->y) && value_eq(&x->blur, &y->blur) &&
           value_eq(&x->spread, &y->spread);
}

static bool cmd_eq(const embi_shape_cmd *x, const embi_shape_cmd *y)
{
    if (x->op != y->op || x->rel != y->rel || x->ncp != y->ncp || x->large != y->large || x->cw != y->cw ||
        x->nr != y->nr || x->rot != y->rot) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (!axis_eq(&x->p[i], &y->p[i]) || !value_eq(&x->r[i], &y->r[i])) {
            return false;
        }
        for (int k = 0; k < 2; k++) {
            if (!axis_eq(&x->c[i][k], &y->c[i][k])) {
                return false;
            }
        }
    }
    return true;
}

static bool shape_eq(const embi_shape *x, const embi_shape *y)
{
    if (x->kind == EMBI_SHAPE_SHAPE) {
        if (y->kind != x->kind || x->evenodd != y->evenodd || x->ncmds != y->ncmds || !pos_eq(&x->from, &y->from)) {
            return false;
        }
        for (uint32_t i = 0; i < x->ncmds; i++) {
            if (!cmd_eq(&x->cmds[i], &y->cmds[i])) {
                return false;
            }
        }
        return true;
    }
    if (x->kind != y->kind || x->evenodd != y->evenodd || x->has_round != y->has_round ||
        x->has_at != y->has_at || x->npts != y->npts || x->path.len != y->path.len) {
        return false;
    }
    if (x->path.len != 0 && memcmp(x->path.ptr, y->path.ptr, x->path.len) != 0) {
        return false;
    }
    return vals_eq(x->box, y->box, 4) && vals_eq(x->radii, y->radii, 8) &&
           vals_eq(x->r, y->r, 2) && pos_eq(&x->at, &y->at) &&
           (x->npts == 0 || vals_eq(x->pts, y->pts, x->npts * 2u));
}

static bool value_eq(const embi_value *x, const embi_value *y)
{
    if (x->kind != y->kind) {
        return false;
    }
    switch (x->kind) {
    case EMBI_V_NONE:
        return true;
    case EMBI_V_NUMBER:
    case EMBI_V_INTEGER:
    case EMBI_V_PERCENT:
    case EMBI_V_LENGTH:
    case EMBI_V_ANGLE:
    case EMBI_V_TIME:
    case EMBI_V_RESOLUTION:
        return x->num == y->num && x->unit == y->unit;
    case EMBI_V_KEYWORD:
        return x->kw == y->kw;
    case EMBI_V_CALC:
        return calc_eq(x->calc, y->calc);
    case EMBI_V_COLOR:
        return x->color.space == y->color.space && x->color.flags == y->color.flags &&
               x->color.sys == y->color.sys && x->color.c[0] == y->color.c[0] &&
               x->color.c[1] == y->color.c[1] && x->color.c[2] == y->color.c[2] &&
               x->color.alpha == y->color.alpha && x->color.alpha_k == y->color.alpha_k;
    case EMBI_V_LIST:
        return x->sep == y->sep && x->list.n == y->list.n &&
               vals_eq(x->list.items, y->list.items, x->list.n);
    case EMBI_V_POSITION:
        return pos_eq(x->pos, y->pos);
    case EMBI_V_POS_AXIS:
        return axis_eq(x->axis, y->axis);
    case EMBI_V_RATIO:
        return x->ratio.a == y->ratio.a && x->ratio.b == y->ratio.b;
    case EMBI_V_SHADOWS:
        if (x->shadows.n != y->shadows.n) {
            return false;
        }
        for (uint32_t i = 0; i < x->shadows.n; i++) {
            if (!shadow_eq(&x->shadows.p[i], &y->shadows.p[i])) {
                return false;
            }
        }
        return true;
    case EMBI_V_FILTERS:
        if (x->filters.n != y->filters.n) {
            return false;
        }
        for (uint32_t i = 0; i < x->filters.n; i++) {
            const embi_filter *fx = &x->filters.p[i], *fy = &y->filters.p[i];
            if (fx->fn != fy->fn || !value_eq(&fx->arg, &fy->arg) ||
                (fx->shadow == nullptr) != (fy->shadow == nullptr) ||
                (fx->shadow != nullptr && !shadow_eq(fx->shadow, fy->shadow))) {
                return false;
            }
        }
        return true;
    case EMBI_V_TRANSFORM:
        if (x->xf.n != y->xf.n) {
            return false;
        }
        for (uint32_t i = 0; i < x->xf.n; i++) {
            const embi_xform *tx = &x->xf.p[i], *ty = &y->xf.p[i];
            if (tx->fn != ty->fn || tx->n != ty->n || !vals_eq(tx->args, ty->args, tx->n)) {
                return false;
            }
        }
        return true;
    case EMBI_V_SHAPE:
        return x->kw == y->kw && ((x->shape == nullptr) == (y->shape == nullptr)) &&
               (x->shape == nullptr || shape_eq(x->shape, y->shape));
    default: {
        embi_sb sx = {0}, sy = {0};
        embi_value_serialize(&sx, x);
        embi_value_serialize(&sy, y);
        bool eq = strcmp(embi_sb_cstr(&sx), embi_sb_cstr(&sy)) == 0;
        embi_sb_free(&sx);
        embi_sb_free(&sy);
        return eq;
    }
    }
}

static void ev_push(embi_trans_events *ev, uint8_t kind, embi_prop_id p, double elapsed)
{
    if (ev->n == ev->cap) {
        uint32_t nc = ev->cap == 0 ? 8u : ev->cap * 2u;
        ev->e = embi_realloc(ev->e, sizeof(embi_trans_event) * ev->cap, sizeof(embi_trans_event) * nc);
        ev->cap = nc;
    }
    ev->e[ev->n++] = (embi_trans_event){kind, p, elapsed};
}

static void tr_push(embi_transitions *tr, const embi_transition *t)
{
    if (tr->n == tr->cap) {
        uint32_t nc = tr->cap == 0 ? 8u : tr->cap * 2u;
        tr->v = embi_realloc(tr->v, sizeof(embi_transition) * tr->cap, sizeof(embi_transition) * nc);
        tr->cap = nc;
    }
    tr->v[tr->n++] = *t;
}

static void tr_remove(embi_transitions *tr, uint32_t i)
{
    memmove(&tr->v[i], &tr->v[i + 1], sizeof(embi_transition) * (tr->n - i - 1));
    tr->n--;
}

/* The time a running transition has spent past its delay, clamped to its duration. */
static double elapsed_of(const embi_transition *t, double now)
{
    double el = now - t->start - t->delay;
    if (el < 0.0) {
        el = 0.0;
    }
    return el > t->duration ? t->duration : el;
}

/* The linear progress of a transition at now: 0 during its delay, clamped to 0..1. */
static double progress_at(const embi_transition *t, double now)
{
    double el = now - t->start - t->delay;
    if (el <= 0.0) {
        return 0.0;
    }
    return t->duration > 0.0 ? clamp01(el / t->duration) : 1.0;
}

/* Cancels the transition at index i (transitioncancel) and removes it. */
static void tr_cancel(embi_transitions *tr, uint32_t i, double now, embi_trans_events *ev)
{
    ev_push(ev, EMBI_TEV_CANCEL, tr->v[i].prop, elapsed_of(&tr->v[i], now));
    tr_remove(tr, i);
}

void embi_trans_value(embi_arena *a, const embi_transition *t, double now, embi_value *out)
{
    double el = now - t->start - t->delay;
    if (el <= 0.0) {
        /* The delay is in progress: the backwards fill holds the start value. */
        *out = t->from;
        return;
    }
    double p = t->duration > 0.0 ? clamp01(el / t->duration) : 1.0;
    embi_anim_mix(a, t->prop, &t->from, &t->to, embi_easing_eval(t->easing, p), out);
}

void embi_trans_update(embi_arena *a, embi_transitions *tr, const embi_trans_input *in, double now,
                       embi_trans_events *ev)
{
    if (in->display_none) {
        while (tr->n > 0) {
            tr_cancel(tr, 0, now, ev);
        }
        return;
    }

    /* A running transition whose property is no longer transitioned is cancelled. */
    for (uint32_t i = 0; i < tr->n;) {
        embi_prop_id p = tr->v[i].prop;
        const embi_trans_spec *s = spec_for(in, p);
        if (s == nullptr || !embi_anim_animatable(p) || (needs_allow_discrete(p) && !s->allow_discrete)) {
            tr_cancel(tr, i, now, ev);
        } else {
            i++;
        }
    }

    for (uint32_t c = 0; c < in->nchanges; c++) {
        const embi_trans_change *ch = &in->changes[c];
        embi_prop_id p = ch->prop;
        if (!embi_anim_animatable(p) || ch->before == nullptr || ch->after == nullptr) {
            continue;
        }
        /* currentcolor in either value is the element's color, when the caller gave one. */
        embi_value before_r, after_r;
        embi_anim_resolve_color(a, ch->before, in->current, &before_r);
        embi_anim_resolve_color(a, ch->after, in->current, &after_r);

        int32_t run = -1;
        for (uint32_t i = 0; i < tr->n && run < 0; i++) {
            if (tr->v[i].prop == p) {
                run = (int32_t)i;
            }
        }
        const embi_trans_spec *s = spec_for(in, p);
        if (run >= 0 && value_eq(&tr->v[run].to, &after_r)) {
            continue; /* the running transition already ends at this value: leave it alone */
        }
        bool combined = s != nullptr && s->duration + s->delay > 0.0;
        if (!combined) {
            if (run >= 0) {
                tr_cancel(tr, (uint32_t)run, now, ev);
            }
            continue;
        }

        if (run < 0) {
            if (value_eq(&before_r, &after_r) || !starts_for(a, p, s, &before_r, &after_r)) {
                continue;
            }
            embi_transition nt;
            memset(&nt, 0, sizeof nt);
            nt.prop = p;
            nt.from = before_r;
            nt.to = after_r;
            nt.reversing_start = before_r;
            nt.reversing_factor = 1.0;
            nt.start = now;
            nt.delay = s->delay;
            nt.duration = s->duration;
            nt.easing = s->easing;
            tr_push(tr, &nt);
            ev_push(ev, EMBI_TEV_RUN, p, 0.0);
            continue;
        }

        embi_transition *t = &tr->v[run];
        embi_value cur;
        embi_trans_value(a, t, now, &cur);
        if (value_eq(&cur, &after_r) || !starts_for(a, p, s, &cur, &after_r)) {
            /* The new value is where the property already is, or cannot be interpolated to: the
             * running transition is cancelled and nothing starts. */
            tr_cancel(tr, (uint32_t)run, now, ev);
            continue;
        }

        /* Reversing (CSS Transitions 2): the after-change value is the reversing-adjusted start
         * value of the running transition. The new duration is the spec duration times the
         * reversing shortening factor, which is the eased progress of the old transition scaled
         * by its own factor, plus the rest. */
        bool reversing = value_eq(&after_r, &t->reversing_start);
        double factor = 1.0;
        if (reversing) {
            double eased = embi_easing_eval(t->easing, progress_at(t, now));
            factor = fabs(clamp01(eased * t->reversing_factor + (1.0 - t->reversing_factor)));
        }
        double dur = s->duration * factor;
        double dly = s->delay >= 0.0 ? s->delay : s->delay * factor;
        ev_push(ev, EMBI_TEV_CANCEL, p, elapsed_of(t, now));
        /* A reversal whose combined duration is not positive still runs: it is created and ends at
         * once (embi_trans_advance sends its start and end events). */
        embi_value old_to = t->to;
        t->reversing_start = reversing ? old_to : cur;
        t->reversing_factor = factor;
        t->from = cur;
        t->to = after_r;
        t->start = now;
        t->delay = dly;
        t->duration = dur;
        t->easing = s->easing;
        t->started = false;
        ev_push(ev, EMBI_TEV_RUN, p, 0.0);
    }
}

void embi_trans_advance(embi_transitions *tr, double now, embi_trans_events *ev)
{
    for (uint32_t i = 0; i < tr->n;) {
        embi_transition *t = &tr->v[i];
        double begin = t->start + t->delay;
        if (!t->started && now >= begin) {
            t->started = true;
            /* A negative delay has started before the transition was created: its elapsed time is
             * the part of the delay that has passed, clamped to the duration. */
            double el = t->delay < 0.0 ? fmin(-t->delay, t->duration) : 0.0;
            ev_push(ev, EMBI_TEV_START, t->prop, el);
        }
        if (now >= begin + t->duration) {
            ev_push(ev, EMBI_TEV_END, t->prop, t->duration);
            tr_remove(tr, i);
        } else {
            i++;
        }
    }
}

void embi_transitions_free(embi_transitions *tr)
{
    embi_free(tr->v, sizeof(embi_transition) * tr->cap);
    memset(tr, 0, sizeof *tr);
}

void embi_trans_events_free(embi_trans_events *ev)
{
    embi_free(ev->e, sizeof(embi_trans_event) * ev->cap);
    memset(ev, 0, sizeof *ev);
}
