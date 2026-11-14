/*
 * The play state machine of one animation (W6c): Web Animations Level 1 section 4 (the animation model) on the effect
 * sampling of anim.h. A Web animation and a CSS animation are the same record; a CSS transition is a read-only proxy.
 * The state: start time, hold time, playback rate and the pending play or pause task; the host keeps the promises and the
 * events (it is told through the notify hook).
 */
#include "anim/anim_int.h"

#include <math.h>

#define UNRES NAN

/* ------------------------------------------------------------------ small helpers */

static double ms_of(double s)
{
    return isnan(s) ? NAN : s * 1000.0;
}

static double sec_of(double ms)
{
    return isnan(ms) ? NAN : ms / 1000.0;
}

static embi_animator *an_of(const embi_an *a)
{
    return a->an;
}

void embi_an_init(embi_an *a, embi_animator *an, uint8_t kind, void *user)
{
    memset(a, 0, sizeof *a);
    a->an = an;
    a->user = user;
    a->kind = kind;
    a->seq = ++an->seq;
    a->slot = UINT32_MAX;
    a->has_timeline = true;
    a->timing = embi_anim_timing_default();
    embi_arena_init(&a->ar, 1024);
    embi_arena_init(&a->rar, 1024);
    a->start = UNRES;
    a->hold = UNRES;
    a->prev = UNRES;
    a->rate = 1.0;
    a->pending_rate = UNRES;
    a->ready_resolved = true;
    a->finished_resolved = false;
    a->last_phase = EMBI_AN_NO_PHASE;
    a->applied_phase = EMBI_AN_NO_PHASE;
    a->composite = EMBI_COMP_REPLACE;
    a->iter_composite = EMBI_COMP_REPLACE;
}

void embi_an_release_mem(embi_an *a)
{
    embi_arena_free(&a->ar);
    embi_arena_free(&a->rar);
    embi_arena_free(&a->ear);
}

void embi_an_notify(embi_an *a, int what)
{
    embi_animator *an = an_of(a);
    if (a->user != nullptr && an->host.notify != nullptr) {
        an->host.notify(an->host.user, a, what, an->time * 1000.0);
    }
}

void embi_an_queue_event(embi_an *a, int what)
{
    if (a->user == nullptr) {
        return;
    }
    an_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = -1;
    ev.a = a;
    ev.what = what;
    ev.time_ms = an_of(a)->time * 1000.0;
    embi_animator_queue(an_of(a), &ev);
}

void embi_an_touch(embi_an *a)
{
    embi_animator *an = an_of(a);
    an->want = true;
    an->replace_dirty = true;
    embi_an_sync_retain(a, false);
    if (a->target != nullptr) {
        embi_animator_touch_el(an, a->target);
    } else if (an->host.invalidate != nullptr) {
        an->host.invalidate(an->host.user);
    }
}

/* ------------------------------------------------------------------ timing */

static double active_duration(const embi_anim_timing *t)
{
    if (t->duration == 0.0 || t->iterations == 0.0) {
        return 0.0;
    }
    return t->duration * t->iterations;
}

double embi_an_effect_end(const embi_an *a)
{
    if (!a->has_effect) {
        return 0.0;
    }
    double e = a->timing.delay + active_duration(&a->timing) + a->timing.end_delay;
    return e > 0.0 ? e : 0.0;
}

double embi_an_cur_time(const embi_an *a)
{
    if (!isnan(a->hold)) {
        return a->hold;
    }
    if (isnan(a->start) || !a->has_timeline) {
        return UNRES;
    }
    return (an_of(a)->time - a->start) * a->rate;
}

int embi_an_state(const embi_an *a)
{
    double cur = embi_an_cur_time(a);
    if (isnan(cur) && a->pending_task == 0) {
        return EMBI_PLAY_IDLE;
    }
    if (a->pending_task == 2 || (isnan(a->start) && a->pending_task != 1)) {
        return EMBI_PLAY_PAUSED;
    }
    double end = embi_an_effect_end(a);
    /* a pending animation reports what it will be once it stops pending: the effective (pending) playback rate decides */
    double erate = isnan(a->pending_rate) ? a->rate : a->pending_rate;
    if ((erate > 0.0 && cur >= end) || (erate < 0.0 && cur <= 0.0)) {
        return EMBI_PLAY_FINISHED;
    }
    return EMBI_PLAY_RUNNING;
}

/* The effect's sample at the animation's current time. resolved is false when the time is unresolved. */
void embi_an_effect_local(const embi_an *a, embi_anim_sample *s, bool *resolved)
{
    double cur = embi_an_cur_time(a);
    *resolved = !isnan(cur) && a->has_effect;
    memset(s, 0, sizeof *s);
    if (*resolved) {
        embi_anim_timing tm = a->timing;
        tm.rate = a->rate; /* the direction of the animation decides the phase at a boundary */
        embi_anim_timing_sample(&tm, cur, s);
    }
}

/* ------------------------------------------------------------------ the finished state */

void embi_an_update_finished(embi_an *a, bool did_seek, bool sync)
{
    (void)sync;
    double T = an_of(a)->time;
    double uct;
    if (did_seek) {
        uct = embi_an_cur_time(a);
    } else {
        uct = (isnan(a->start) || !a->has_timeline) ? UNRES : (T - a->start) * a->rate;
    }
    if (!isnan(uct) && !isnan(a->start) && a->pending_task == 0) {
        double end = embi_an_effect_end(a);
        if (a->rate > 0.0 && uct >= end) {
            if (did_seek) {
                a->hold = uct;
            } else {
                a->hold = isnan(a->prev) ? end : fmax(a->prev, end);
            }
        } else if (a->rate < 0.0 && uct <= 0.0) {
            if (did_seek) {
                a->hold = uct;
            } else {
                a->hold = isnan(a->prev) ? 0.0 : fmin(a->prev, 0.0);
            }
        } else if (a->rate != 0.0 && a->has_timeline) {
            if (did_seek && !isnan(a->hold)) {
                a->start = T - a->hold / a->rate;
            }
            a->hold = UNRES;
        }
    }
    a->prev = embi_an_cur_time(a);
    bool fin = embi_an_state(a) == EMBI_PLAY_FINISHED;
    if (fin && !a->finished_resolved) {
        a->finished_resolved = true;
        embi_an_notify(a, EMBI_ANN_FINISHED);
        embi_an_queue_event(a, EMBI_ANN_EV_FINISH);
    } else if (!fin && a->finished_resolved) {
        a->finished_resolved = false;
        embi_an_notify(a, EMBI_ANN_FINISHED_NEW);
    }
}

/* ------------------------------------------------------------------ pending tasks */

static void resolve_ready(embi_an *a)
{
    if (!a->ready_resolved) {
        a->ready_resolved = true;
        embi_an_notify(a, EMBI_ANN_READY);
    }
}

static void new_ready(embi_an *a)
{
    if (a->ready_resolved) {
        a->ready_resolved = false;
        embi_an_notify(a, EMBI_ANN_READY_NEW);
    }
}

static void apply_pending_rate(embi_an *a)
{
    if (!isnan(a->pending_rate)) {
        a->rate = a->pending_rate;
        a->pending_rate = UNRES;
    }
}

void embi_an_resolve_pending(embi_an *a, double rt)
{
    if (a->pending_task == 0 || !a->has_timeline) {
        return;
    }
    if (a->pending_task == 1) {
        a->pending_task = 0;
        if (!isnan(a->hold)) {
            apply_pending_rate(a);
            a->start = a->rate != 0.0 ? rt - a->hold / a->rate : rt;
            if (a->rate != 0.0) {
                a->hold = UNRES;
            }
        } else if (!isnan(a->start) && !isnan(a->pending_rate)) {
            double nh = (rt - a->start) * a->rate;
            apply_pending_rate(a);
            a->hold = nh;
            a->start = a->rate != 0.0 ? rt - nh / a->rate : rt;
            if (a->rate != 0.0) {
                a->hold = UNRES;
            }
        }
    } else {
        a->pending_task = 0;
        if (!isnan(a->start) && isnan(a->hold)) {
            a->hold = (rt - a->start) * a->rate;
        }
        apply_pending_rate(a);
        a->start = UNRES;
    }
    resolve_ready(a);
    embi_an_update_finished(a, false, false);
    embi_an_touch(a);
}

static void reset_pending(embi_an *a)
{
    if (a->pending_task == 0) {
        return;
    }
    a->pending_task = 0;
    apply_pending_rate(a);
}

/* ------------------------------------------------------------------ operations */

void embi_an_play_internal(embi_an *a, bool auto_rewind, const char **err, bool *ok)
{
    *ok = true;
    bool aborted_pause = a->pending_task == 2;
    bool had_pending = a->pending_task != 0;
    double seek = UNRES;
    double cur = embi_an_cur_time(a);
    double end = embi_an_effect_end(a);
    /* the effective playback rate: the pending one when there is one */
    double erate = isnan(a->pending_rate) ? a->rate : a->pending_rate;
    if (erate > 0.0 && auto_rewind && (isnan(cur) || cur < 0.0 || cur >= end)) {
        seek = 0.0;
    } else if (erate < 0.0 && auto_rewind && (isnan(cur) || cur <= 0.0 || cur > end)) {
        if (isinf(end)) {
            *ok = false;
            *err = "InvalidStateError";
            return;
        }
        seek = end;
    } else if (erate == 0.0 && isnan(cur)) {
        seek = 0.0;
    }
    if (!isnan(seek)) {
        a->hold = seek;
    }
    if (!isnan(a->hold)) {
        a->start = UNRES;
    }
    /* "Play an animation" step: nothing to do for a running animation (no seek, no aborted pause, no pending rate). */
    if (isnan(a->hold) && isnan(seek) && !aborted_pause && isnan(a->pending_rate)) {
        return;
    }
    if (isnan(cur) && isnan(seek) && isnan(a->hold) && isnan(a->start)) {
        /* nothing to start from (rate > 0 case handled above) */
        a->hold = 0.0;
    }
    if (!had_pending) {
        new_ready(a);
    }
    a->pending_task = 1;
    /* The finished state of the seek: the animation is no longer finished when it rewound. */
    embi_an_update_finished(a, false, false);
    embi_an_touch(a);
}

bool embi_an_play(embi_an *a, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    bool ok;
    embi_an_play_internal(a, true, err, &ok);
    return ok;
}

void embi_an_pause_internal(embi_an *a, const char **err, bool *ok)
{
    *ok = true;
    if (a->pending_task == 2) {
        return;
    }
    if (embi_an_state(a) == EMBI_PLAY_PAUSED) {
        return;
    }
    double cur = embi_an_cur_time(a);
    double seek = UNRES;
    if (isnan(cur)) {
        if (a->rate >= 0.0) {
            seek = 0.0;
        } else {
            double end = embi_an_effect_end(a);
            if (isinf(end)) {
                *ok = false;
                *err = "InvalidStateError";
                return;
            }
            seek = end;
        }
    }
    if (!isnan(seek)) {
        a->hold = seek;
    }
    bool had_pending = a->pending_task != 0;
    if (!had_pending) {
        new_ready(a);
    }
    a->pending_task = 2;
    embi_an_update_finished(a, false, false);
    embi_an_touch(a);
}

bool embi_an_pause(embi_an *a, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    bool ok;
    embi_an_pause_internal(a, err, &ok);
    return ok;
}

bool embi_an_reverse(embi_an *a, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    if (!a->has_timeline) {
        *err = "InvalidStateError";
        return false;
    }
    double orig_pending = a->pending_rate;
    double orig = isnan(a->pending_rate) ? a->rate : a->pending_rate;
    a->pending_rate = orig == 0.0 ? 0.0 : -orig;
    bool ok;
    embi_an_play_internal(a, true, err, &ok);
    if (!ok) {
        a->pending_rate = orig_pending;
    }
    return ok;
}

bool embi_an_finish(embi_an *a, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        embi_animator_trans_end(a, true);
        return true;
    }
    double rate = isnan(a->pending_rate) ? a->rate : a->pending_rate;
    double end = embi_an_effect_end(a);
    if (rate == 0.0 || (rate > 0.0 && isinf(end))) {
        *err = "InvalidStateError";
        return false;
    }
    apply_pending_rate(a);
    double limit = a->rate > 0.0 ? end : 0.0;
    /* set the current time (silently), then the start time if it is unresolved */
    if (!isnan(a->hold) || isnan(a->start) || !a->has_timeline || a->rate == 0.0) {
        a->hold = limit;
    } else {
        a->start = an_of(a)->time - limit / a->rate;
    }
    if (isnan(a->start) && a->has_timeline) {
        a->start = an_of(a)->time - limit / a->rate;
    }
    if (a->pending_task == 2 && !isnan(a->start)) {
        a->hold = UNRES;
        a->pending_task = 0;
        resolve_ready(a);
    }
    if (a->pending_task == 1 && !isnan(a->start)) {
        a->pending_task = 0;
        resolve_ready(a);
    }
    embi_an_update_finished(a, true, true);
    embi_an_touch(a);
    return true;
}

void embi_an_cancel_internal(embi_an *a)
{
    if (embi_an_state(a) != EMBI_PLAY_IDLE) {
        bool had_pending = a->pending_task != 0;
        reset_pending(a);
        (void)had_pending;
        a->ready_resolved = true;
        a->finished_resolved = false;
        embi_an_notify(a, EMBI_ANN_CANCELED);
        embi_an_queue_event(a, EMBI_ANN_EV_CANCEL);
    }
    a->hold = UNRES;
    a->start = UNRES;
    a->prev = UNRES;
    embi_an_touch(a);
}

void embi_an_cancel(embi_an *a)
{
    if (a->kind == EMBI_AN_CSST) {
        embi_animator_trans_end(a, false);
        return;
    }
    embi_an_cancel_internal(a);
}

bool embi_an_set_current_time(embi_an *a, double ms, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    double seek = sec_of(ms);
    if (isnan(seek)) {
        if (!isnan(embi_an_cur_time(a))) {
            *err = "TypeError";
            return false;
        }
        return true;
    }
    /* silently set the current time */
    if (!isnan(a->hold) || isnan(a->start) || !a->has_timeline || a->rate == 0.0) {
        a->hold = seek;
    } else {
        a->start = an_of(a)->time - seek / a->rate;
    }
    if (!a->has_timeline) {
        a->start = UNRES;
    }
    a->prev = UNRES;
    if (a->pending_task == 2) {
        a->hold = seek;
        apply_pending_rate(a);
        a->start = UNRES;
        a->pending_task = 0;
        resolve_ready(a);
    }
    embi_an_update_finished(a, true, false);
    embi_an_touch(a);
    return true;
}

bool embi_an_set_start_time(embi_an *a, double ms, const char **err)
{
    (void)err;
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    double nt = sec_of(ms);
    if (isnan(nt) && isnan(a->start)) {
        return true;
    }
    double timeline_time = an_of(a)->time;
    double prev_time = embi_an_cur_time(a);
    a->start = nt;
    if (!isnan(nt)) {
        apply_pending_rate(a);
        if (a->rate != 0.0) {
            a->hold = UNRES;
        } else {
            /* keep the hold: rate 0 */
        }
    } else {
        a->hold = prev_time;
    }
    (void)timeline_time;
    if (a->pending_task != 0) {
        a->pending_task = 0;
        resolve_ready(a);
    }
    embi_an_update_finished(a, true, false);
    embi_an_touch(a);
    return true;
}

void embi_an_set_playback_rate(embi_an *a, double rate)
{
    if (a->kind == EMBI_AN_CSST) {
        return;
    }
    double prev_time = embi_an_cur_time(a);
    a->pending_rate = UNRES;
    a->rate = rate;
    if (a->has_timeline && !isnan(prev_time)) {
        const char *err = nullptr;
        (void)embi_an_set_current_time(a, ms_of(prev_time), &err);
    }
    embi_an_touch(a);
}

bool embi_an_update_playback_rate(embi_an *a, double rate, const char **err)
{
    if (a->kind == EMBI_AN_CSST) {
        return true;
    }
    int pstate = embi_an_state(a); /* the state before the new rate is pending (the pending rate takes part in it) */
    a->pending_rate = rate;
    switch (pstate) {
    case EMBI_PLAY_IDLE:
    case EMBI_PLAY_PAUSED:
        a->rate = rate;
        a->pending_rate = UNRES;
        break;
    case EMBI_PLAY_FINISHED: {
        double T = an_of(a)->time;
        double unconstrained = (isnan(a->start) ? UNRES : (T - a->start) * a->rate);
        double cur = embi_an_cur_time(a);
        (void)unconstrained;
        if (rate == 0.0) {
            a->hold = cur;
            a->start = UNRES;
        } else if (!isnan(cur)) {
            a->hold = UNRES;
            a->start = T - cur / rate;
        }
        a->rate = rate;
        a->pending_rate = UNRES;
        embi_an_update_finished(a, true, false);
        break;
    }
    default: {
        bool ok;
        embi_an_play_internal(a, false, err, &ok);
        embi_an_touch(a);
        return ok;
    }
    }
    embi_an_touch(a);
    return true;
}

void embi_an_persist(embi_an *a)
{
    if (a->replace_state != EMBI_REPLACE_PERSISTED) {
        a->replace_state = EMBI_REPLACE_PERSISTED;
    }
}

/* ------------------------------------------------------------------ the public getters */

double embi_an_current_time(const embi_an *a)
{
    if (a->kind == EMBI_AN_CSST) {
        return embi_an_trans_time(a);
    }
    return ms_of(embi_an_cur_time(a));
}

double embi_an_start_time(const embi_an *a)
{
    return ms_of(a->start);
}

double embi_an_hold_time(const embi_an *a)
{
    return ms_of(a->hold);
}

double embi_an_playback_rate(const embi_an *a)
{
    return a->rate;
}

double embi_an_pending_rate(const embi_an *a)
{
    return a->pending_rate;
}

int embi_an_play_state(const embi_an *a)
{
    if (a->kind == EMBI_AN_CSST) {
        double dl, du;
        if (!embi_an_trans_info(a, &dl, &du)) {
            return EMBI_PLAY_IDLE;
        }
        return embi_an_trans_time(a) >= dl + du ? EMBI_PLAY_FINISHED : EMBI_PLAY_RUNNING;
    }
    return embi_an_state(a);
}

bool embi_an_pending(const embi_an *a)
{
    return a->pending_task != 0;
}

bool embi_an_ready_resolved(const embi_an *a)
{
    return a->ready_resolved;
}

bool embi_an_finished_resolved(const embi_an *a)
{
    return a->finished_resolved;
}

int embi_an_replace_state(const embi_an *a)
{
    return a->replace_state;
}

bool embi_an_relevant(const embi_an *a)
{
    if (a->kind == EMBI_AN_CSST) {
        double dl, du;
        return embi_an_trans_info(a, &dl, &du);
    }
    if (!a->has_effect || a->retired || a->replace_state == EMBI_REPLACE_REMOVED) {
        return false;
    }
    embi_anim_sample s;
    bool res;
    embi_an_effect_local(a, &s, &res);
    if (!res) {
        return false;
    }
    if (s.in_effect) {
        return true;
    }
    /* current: before the active interval and heading into it, or after it and heading back */
    if (s.phase == EMBI_PH_BEFORE && a->rate > 0.0) {
        return true;
    }
    if (s.phase == EMBI_PH_AFTER && a->rate < 0.0) {
        return true;
    }
    return false;
}

void embi_an_computed_timing(const embi_an *a, embi_an_computed *out)
{
    memset(out, 0, sizeof *out);
    if (a->kind == EMBI_AN_CSST) {
        double dl = 0, du = 0;
        out->phase = -1;
        out->local_time = NAN;
        out->progress = NAN;
        out->current_iteration = NAN;
        if (embi_an_trans_info(a, &dl, &du)) {
            out->end_time = dl + du;
            out->active_duration = du;
            out->local_time = embi_an_trans_time(a);
            out->phase = out->local_time < dl ? EMBI_PH_BEFORE : (out->local_time < dl + du ? EMBI_PH_ACTIVE : EMBI_PH_AFTER);
        }
        return;
    }
    out->end_time = a->has_effect ? embi_an_effect_end(a) * 1000.0 : 0.0;
    out->active_duration = a->has_effect ? active_duration(&a->timing) * 1000.0 : 0.0;
    out->local_time = NAN;
    out->progress = NAN;
    out->current_iteration = NAN;
    out->phase = -1;
    if (!a->has_effect) {
        return;
    }
    embi_anim_sample s;
    bool res;
    embi_an_effect_local(a, &s, &res);
    if (!res) {
        return;
    }
    out->local_time = ms_of(embi_an_cur_time(a));
    out->phase = s.phase;
    if (s.in_effect) {
        out->progress = s.progress;
        out->current_iteration = s.iteration;
    }
}
