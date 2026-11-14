/*
 * The animator (W6c): the document side of the animations. It owns the per-element animation state, creates the CSS
 * animations and transitions when the style hook sees a new base style, samples every animation of an element into an
 * overlay style, runs the frame step, and queues the events. The play state machine of one animation is in anim_play.c.
 *
 * The style hook. For each element the resolve computes a base style (cascade); embi_style_anim_hooks.apply gets it before it
 * is stored. If the element has animations or transitions, apply returns an overlay: the base style with the animated
 * values in place. The base style of the element is kept in its state (a reference), so the next restyle sees the
 * before-change style of the transitions even though the slot holds the overlay.
 *
 * Values. Keyframes are specified values; they are computed against the element's base style when an animation first
 * applies and whenever the base changes (the animation's resolved set). A sample is computed in a scratch arena.
 *
 * The lifetime rule of animated values (WPTASAN). Styles outlive frames in many places (a slot not restyled for a while, the
 * sharing cache, the change tracking, a layout grave, the kept base and parent styles below), and a value they point at
 * must live as long as they do. So the values of an overlay are deep-copied (embi_value_copy) into a value block of the
 * style engine, and a value block lives as long as some style group points into it: the groups pin it, whoever made them
 * (style.h, value blocks). The animator's own long-lived values never point into styles either: the transitions' values
 * are deep copies in the element state's arena (tar), and a keyframe value taken from the parent style (inherit, unset) is
 * a deep copy in the animation's resolved set. So no memory of the animator is ever reached through a style, and freeing
 * or resetting it (scratch after each sample, tar when no transition runs, a resolved set when it is resolved again) never
 * leaves a style with a dangling value.
 */
#include "anim/anim_int.h"

#include <math.h>

#define UNRES NAN
#define MAX_CHANGES 512u

/* ------------------------------------------------------------------ small helpers */

static bool kw_is(const embi_value *v, uint16_t kw)
{
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw == kw;
}

static const embi_value *item_at(const embi_value *v, uint32_t i)
{
    uint32_t n = 0;
    const embi_value *items = embi_value_items(v, &n);
    if (n == 0 || items == nullptr) {
        return nullptr;
    }
    return &items[i % n];
}

static uint32_t item_count(const embi_value *v)
{
    uint32_t n = 0;
    (void)embi_value_items(v, &n);
    return n;
}

static double time_of(const embi_value *v, double def)
{
    if (v == nullptr) {
        return def;
    }
    if (v->kind == EMBI_V_TIME) {
        return v->num;
    }
    if (v->kind == EMBI_V_CALC) {
        return def;
    }
    return def;
}

/* ------------------------------------------------------------------ states */

static el_state *st_find(const embi_animator *an, const embi_element *el)
{
    uint64_t v = 0;
    if (embi_hashmap_len(&an->states) > 0 && embi_hashmap_get(&an->states, (uint64_t)(uintptr_t)el, &v)) {
        return (el_state *)(uintptr_t)v;
    }
    return nullptr;
}

static el_state *st_get(embi_animator *an, embi_element *el)
{
    el_state *st = st_find(an, el);
    if (st == nullptr) {
        st = embi_calloc(1, sizeof *st);
        st->el = el;
        st->base_gen = ++an->gen_counter;
        embi_arena_init(&st->tar, 1024);
        embi_hashmap_put(&an->states, (uint64_t)(uintptr_t)el, (uint64_t)(uintptr_t)st);
    }
    return st;
}

static void purge_events(embi_animator *an, const embi_an *a, const embi_element *el)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < an->events.len; i++) {
        an_event *ev = &an->events.data[i];
        bool drop = (a != nullptr && ev->a == a) || (el != nullptr && ev->el == el);
        if (!drop) {
            an->events.data[w++] = *ev;
        }
    }
    an->events.len = w;
}

static void st_release_base(embi_animator *an, el_state *st)
{
    if (st->base != nullptr) {
        embi_style_release(an->se, st->base);
        st->base = nullptr;
    }
}

static void st_free(embi_animator *an, el_state *st)
{
    st_release_base(an, st);
    embi_hashmap_remove(&an->states, (uint64_t)(uintptr_t)st->el);
    embi_vec_free(&st->anims);
    embi_vec_free(&st->tproxy);
    embi_transitions_free(&st->trans);
    embi_arena_free(&st->tar);
    embi_vec_free(&st->dprops);
    embi_free(st, sizeof *st);
}

void embi_animator_queue(embi_animator *an, const an_event *ev)
{
    embi_vec_push(&an->events, *ev);
    an->want = true;
}

void embi_animator_touch_el(embi_animator *an, embi_element *el)
{
    el_state *st = st_find(an, el);
    if (st != nullptr) {
        st->ver++;
    }
    /* its animations changed (played, paused, cancelled, new keyframes): what its descendants inherit from it moves */
    if (el != nullptr) {
        embi_hashmap_put(&an->motion[0], (uint64_t)(uintptr_t)el, 0);
    }
    an->want = true;
    if (an->in_apply || an->quiet) {
        return;
    }
    embi_style_mark_element(an->se, el);
    if (an->host.invalidate != nullptr) {
        an->host.invalidate(an->host.user);
    }
}

/* Composite order: transitions, CSS animations, then Web animations; creation order inside. */
static int comp_rank(const embi_an *a)
{
    return a->kind == EMBI_AN_WEB ? 2 : 1;
}

void embi_animator_state_add(embi_an *a)
{
    if (a->target == nullptr) {
        return;
    }
    el_state *st = st_get(a->an, a->target);
    uint32_t pos = st->anims.len;
    while (pos > 0) {
        const embi_an *p = st->anims.data[pos - 1];
        if (comp_rank(p) < comp_rank(a) || (comp_rank(p) == comp_rank(a) && p->seq < a->seq)) {
            break;
        }
        pos--;
    }
    embi_vec_push(&st->anims, a);
    for (uint32_t i = st->anims.len - 1; i > pos; i--) {
        st->anims.data[i] = st->anims.data[i - 1];
    }
    st->anims.data[pos] = a;
    st->ver++;
}

void embi_animator_state_remove(embi_an *a)
{
    if (a->target == nullptr) {
        return;
    }
    el_state *st = st_find(a->an, a->target);
    if (st == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < st->anims.len; i++) {
        if (st->anims.data[i] == a) {
            memmove(&st->anims.data[i], &st->anims.data[i + 1], sizeof(embi_an *) * (st->anims.len - i - 1));
            st->anims.len--;
            break;
        }
    }
    for (uint32_t i = 0; i < st->tproxy.len; i++) {
        if (st->tproxy.data[i] == a) {
            memmove(&st->tproxy.data[i], &st->tproxy.data[i + 1], sizeof(embi_an *) * (st->tproxy.len - i - 1));
            st->tproxy.len--;
            break;
        }
    }
    st->ver++;
}

void embi_animator_list(embi_an *a)
{
    embi_animator *an = a->an;
    if (a->slot != UINT32_MAX) {
        return;
    }
    a->slot = an->all.len;
    embi_vec_push(&an->all, a);
}

void embi_animator_unlist(embi_an *a)
{
    embi_animator *an = a->an;
    if (a->slot == UINT32_MAX) {
        return;
    }
    embi_an *last = an->all.data[an->all.len - 1];
    an->all.data[a->slot] = last;
    last->slot = a->slot;
    an->all.len--;
    a->slot = UINT32_MAX;
}

/* ------------------------------------------------------------------ the records */

static void free_record(embi_an *a)
{
    embi_animator *an = a->an;
    embi_hashmap_remove(&an->ids, a->id);
    embi_animator_unlist(a);
    purge_events(an, a, nullptr);
    embi_an_release_mem(a);
    an->live--;
    embi_free(a, sizeof *a);
}

static embi_an *new_record(embi_animator *an, uint8_t kind, bool hosted)
{
    embi_an *a = embi_alloc(sizeof *a);
    embi_an_init(a, an, kind, nullptr);
    a->id = ++an->next_id;
    if (hosted) {
        a->user = (void *)(uintptr_t)a->id;
    }
    embi_hashmap_put(&an->ids, a->id, (uint64_t)(uintptr_t)a);
    an->live++;
    return a;
}

embi_an *embi_an_new(embi_animator *an)
{
    embi_an *a = new_record(an, EMBI_AN_WEB, true);
    embi_animator_list(a);
    return a;
}

embi_an *embi_an_by_id(embi_animator *an, uint32_t id)
{
    uint64_t v = 0;
    if (embi_hashmap_len(&an->ids) > 0 && embi_hashmap_get(&an->ids, id, &v)) {
        return (embi_an *)(uintptr_t)v;
    }
    return nullptr;
}

uint32_t embi_an_id(const embi_an *a)
{
    return a->id;
}

void embi_an_set_hosted(embi_an *a, bool hosted)
{
    a->user = hosted ? (void *)(uintptr_t)a->id : nullptr;
    a->orphan = false;
    a->retained = hosted && embi_an_relevant(a); /* the host takes a strong reference to a relevant animation itself */
}

void embi_an_sync_retain(embi_an *a, bool allow_release)
{
    if (a->user == nullptr || a->retired) {
        return;
    }
    bool want = a->pending_task != 0 || embi_an_relevant(a);
    if (want && !a->retained) {
        a->retained = true;
        embi_an_notify(a, EMBI_ANN_RETAIN);
    } else if (!want && a->retained && allow_release) {
        a->retained = false;
        embi_an_notify(a, EMBI_ANN_RELEASE);
    }
}

/* ---- the replace pass (Web Animations: removing replaced animations) */

static uint32_t anim_props(const embi_an *a, embi_prop_id *out, uint32_t cap)
{
    uint32_t n = 0;
    if (a->kfin != nullptr) {
        for (uint32_t i = 0; i < a->nkfin; i++) {
            embi_prop_id p = a->kfin[i].prop;
            bool seen = false;
            for (uint32_t q = 0; q < n && !seen; q++) {
                seen = out[q] == p;
            }
            if (!seen && n < cap) {
                out[n++] = p;
            }
        }
    } else if (a->kf != nullptr) {
        uint32_t np = embi_kf_prop_count(a->kf);
        for (uint32_t k = 0; k < np && n < cap; k++) {
            out[n++] = embi_kf_prop_at(a->kf, k);
        }
    }
    return n;
}

static bool bit_has(const uint64_t *set, embi_prop_id p)
{
    return (set[p >> 6] >> (p & 63u)) & 1u;
}

static void replace_pass(embi_animator *an)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        el_state *st = (el_state *)(uintptr_t)v;
        if (st->anims.len < 2) {
            continue;
        }
        uint64_t over[(EMBI_PROP_COUNT + 63) / 64];
        memset(over, 0, sizeof over);
        for (uint32_t i = st->anims.len; i > 0; i--) {
            embi_an *a = st->anims.data[i - 1];
            if (!a->has_effect || a->retired || a->replace_state == EMBI_REPLACE_REMOVED) {
                continue;
            }
            embi_prop_id props[64];
            uint32_t np = anim_props(a, props, 64);
            if (np == 0) {
                continue;
            }
            embi_anim_sample s;
            bool res;
            embi_an_effect_local(a, &s, &res);
            bool in_effect = res && s.in_effect;
            if (a->kind == EMBI_AN_WEB && a->replace_state == EMBI_REPLACE_ACTIVE && a->composite == EMBI_COMP_REPLACE &&
                a->has_timeline && in_effect && s.phase == EMBI_PH_AFTER && embi_an_state(a) == EMBI_PLAY_FINISHED) {
                bool all = np < 64; /* a full property list may have dropped some: not fully covered */
                for (uint32_t q = 0; q < np && all; q++) {
                    all = bit_has(over, props[q]);
                }
                if (all) {
                    a->replace_state = EMBI_REPLACE_REMOVED;
                    embi_an_queue_event(a, EMBI_ANN_EV_REMOVE);
                    embi_style_mark_element(an->se, a->target);
                    an->marked++;
                    continue;
                }
            }
            if (in_effect && a->composite == EMBI_COMP_REPLACE) {
                for (uint32_t q = 0; q < np; q++) {
                    over[props[q] >> 6] |= (uint64_t)1 << (props[q] & 63u);
                }
            }
        }
    }
}

bool embi_an_hosted(const embi_an *a)
{
    return a->user != nullptr;
}

void embi_an_wrapper_lost(embi_animator *an, uint32_t id)
{
    embi_an *a = embi_an_by_id(an, id);
    if (a == nullptr) {
        return;
    }
    a->user = nullptr; /* nobody is listening any more */
    embi_vec_push(&an->zombies, id);
    an->want = true;
}

/* Takes a record out of its element and the animator's lists. An animation the host holds stays (idle) until the host
 * destroys it. */
static void retire(embi_an *a, bool events)
{
    embi_animator *an = a->an;
    embi_element *el = a->target;
    if (events && a->kind == EMBI_AN_CSSA && el != nullptr &&
        (a->last_phase == EMBI_PH_BEFORE || a->last_phase == EMBI_PH_ACTIVE)) {
        an_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = EMBI_CEV_ANIMATIONCANCEL;
        ev.el = el;
        ev.name_atom = a->css.name;
        double cur = embi_an_cur_time(a);
        double act = a->timing.duration * a->timing.iterations;
        if (a->timing.duration == 0.0 || a->timing.iterations == 0.0) {
            act = 0.0;
        }
        ev.elapsed = isnan(cur) ? 0.0 : fmax(fmin(cur - a->timing.delay, act), 0.0);
        embi_animator_queue(an, &ev);
    }
    if (events) {
        embi_an_cancel_internal(a);
    }
    embi_animator_state_remove(a);
    a->target = nullptr;
    a->retired = true;
    if (a->retained) {
        a->retained = false;
        embi_an_notify(a, EMBI_ANN_RELEASE);
    }
    embi_animator_unlist(a);
    if (a->user == nullptr) {
        free_record(a);
    }
}

/* Frees a record the host no longer wants (its element is not touched: it may be gone). */
static void destroy_record(embi_an *a)
{
    embi_animator *an = a->an;
    el_state *st = a->target != nullptr ? st_find(an, a->target) : nullptr;
    a->user = nullptr;
    embi_animator_state_remove(a);
    a->target = nullptr;
    free_record(a);
    if (st != nullptr && st->anims.len == 0 && st->tproxy.len == 0 && st->trans.n == 0) {
        st_free(an, st);
    }
}

static void sweep(embi_animator *an)
{
    for (uint32_t i = 0; i < an->zombies.len; i++) {
        embi_an *a = embi_an_by_id(an, an->zombies.data[i]);
        if (a == nullptr || a->user != nullptr || a->orphan) { /* adopted again, or already orphaned */
            continue;
        }
        el_state *st = a->target != nullptr ? st_find(an, a->target) : nullptr;
        bool visible = st != nullptr && st->base != nullptr && a->has_effect && embi_an_relevant(a);
        if (visible) {
            a->user = nullptr; /* it keeps running, with no wrapper */
            a->orphan = true;
        } else {
            destroy_record(a);
        }
    }
    an->zombies.len = 0;
}

uint8_t embi_an_kind(const embi_an *a)
{
    return a->kind;
}

embi_animator *embi_an_animator(const embi_an *a)
{
    return a->an;
}

embi_str embi_an_css_name(const embi_an *a)
{
    if (a->kind == EMBI_AN_CSSA) {
        return embi_atom_str(a->css.name);
    }
    if (a->kind == EMBI_AN_CSST) {
        const embi_prop_info *pi = embi_prop_info_of(a->trans_prop);
        if (pi != nullptr) {
            return (embi_str){pi->name, (uint32_t)strlen(pi->name)};
        }
    }
    return (embi_str){"", 0};
}

uint64_t embi_an_seq(const embi_an *a)
{
    return a->seq;
}

/* ------------------------------------------------------------------ the effect */

static void adopt_keyframes(embi_an *a, embi_arena *src, const embi_kf_input *kf, uint32_t n)
{
    embi_arena_free(&a->ar);
    if (src != nullptr) {
        a->ar = *src;
        memset(src, 0, sizeof *src);
    } else {
        embi_arena_init(&a->ar, 1024);
    }
    a->kfin = (embi_kf_input *)(uintptr_t)kf;
    a->nkfin = n;
    a->kf = nullptr;
    a->kf_gen = 0;
}

void embi_an_set_effect(embi_an *a, const embi_an_effect_desc *d)
{
    embi_element *old = a->target;
    if (old != d->target) {
        embi_animator_state_remove(a);
    }
    adopt_keyframes(a, d->arena, d->kf, d->nkf);
    a->has_effect = d->has_effect;
    a->timing = d->timing;
    a->timing.rate = 1.0;
    a->composite = d->composite;
    a->iter_composite = d->iteration_composite;
    if (old != d->target) {
        a->target = d->target;
        embi_animator_state_add(a);
    }
    embi_an_update_finished(a, false, false);
    if (old != nullptr && old != d->target) {
        embi_animator_touch_el(a->an, old);
    }
    embi_an_touch(a);
}

embi_arena *embi_an_easing_arena(embi_an *a)
{
    embi_arena_free(&a->ear); /* the previous timing easing is replaced by the one the caller parses next */
    embi_arena_init(&a->ear, 256);
    return &a->ear;
}

void embi_an_set_timing(embi_an *a, const embi_anim_timing *t)
{
    double r = a->timing.rate;
    a->timing = *t;
    a->timing.rate = r;
    embi_an_update_finished(a, false, false);
    embi_an_touch(a);
}

void embi_an_set_target(embi_an *a, embi_element *el)
{
    if (a->target == el) {
        return;
    }
    embi_element *old = a->target;
    embi_animator_state_remove(a);
    a->target = el;
    embi_animator_state_add(a);
    if (old != nullptr) {
        embi_animator_touch_el(a->an, old);
    }
    embi_an_touch(a);
}

void embi_an_set_composite(embi_an *a, uint8_t composite, uint8_t iteration_composite)
{
    a->composite = composite;
    a->iter_composite = iteration_composite;
    embi_an_touch(a);
}

void embi_an_set_keyframes(embi_an *a, embi_arena *src, const embi_kf_input *kf, uint32_t n)
{
    adopt_keyframes(a, src, kf, n);
    embi_an_touch(a);
}

embi_element *embi_an_target(const embi_an *a)
{
    return a->target;
}

const embi_anim_timing *embi_an_timing(const embi_an *a)
{
    return &a->timing;
}

bool embi_an_has_effect(const embi_an *a)
{
    return a->has_effect;
}

void embi_an_clear_effect(embi_an *a)
{
    embi_element *old = a->target;
    embi_animator_state_remove(a);
    a->target = nullptr;
    a->has_effect = false;
    a->kf = nullptr;
    embi_an_update_finished(a, false, false);
    if (old != nullptr) {
        embi_animator_touch_el(a->an, old);
    }
    a->an->want = true;
}

void embi_an_set_timeline(embi_an *a, bool has_timeline)
{
    a->has_timeline = has_timeline;
    if (!has_timeline) {
        a->start = UNRES;
    }
    embi_an_touch(a);
}

bool embi_an_has_timeline(const embi_an *a)
{
    return a->has_timeline;
}

uint32_t embi_an_keyframe_count(const embi_an *a)
{
    return a->nkfin;
}

bool embi_an_keyframe_at(const embi_an *a, uint32_t i, double *offset, embi_prop_id *prop, embi_sb *value,
                         const embi_easing **easing)
{
    if (i >= a->nkfin) {
        return false;
    }
    const embi_kf_input *k = &a->kfin[i];
    *offset = k->offset;
    *prop = k->prop;
    *easing = k->easing;
    if (value != nullptr && k->value.kind != EMBI_V_NONE) {
        embi_value_serialize(value, &k->value);
    }
    return true;
}

/* ------------------------------------------------------------------ lifetime */

static void hook_gone(void *user, embi_element *el);
static const embi_style *hook_apply(void *user, embi_style_engine *e, embi_element *el, const embi_style *old_slot,
                                    const embi_style *base, const embi_style *parent, bool is_root);

embi_animator *embi_animator_new(embi_style_engine *se, const embi_animator_host *host)
{
    embi_animator *an = embi_calloc(1, sizeof *an);
    an->se = se;
    if (host != nullptr) {
        an->host = *host;
    }
    embi_hashmap_init(&an->states);
    embi_hashmap_init(&an->ids);
    embi_hashmap_init(&an->motion[0]);
    embi_hashmap_init(&an->motion[1]);
    embi_hashmap_init(&an->decl);
    embi_hashmap_init(&an->pstyle);
    embi_arena_init(&an->scratch, 4096);
    embi_arena_init(&an->kfar, 4096);
    embi_style_anim_hooks h = {.user = an, .apply = hook_apply, .gone = hook_gone};
    embi_style_set_anim_hooks(se, &h);
    return an;
}

void embi_animator_reset(embi_animator *an)
{
    /* The states are rebuilt from scratch; the count of the table is read before the states go. */
    uint32_t cnt = (uint32_t)embi_hashmap_len(&an->states);
    (void)cnt;
    an->events.len = 0;
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    EMBI_VEC(el_state *) list = {0};
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_vec_push(&list, (el_state *)(uintptr_t)v);
    }
    for (uint32_t i = 0; i < list.len; i++) {
        el_state *st = list.data[i];
        while (st->anims.len > 0) {
            embi_an *a = st->anims.data[st->anims.len - 1];
            a->user = nullptr;
            retire(a, false);
        }
        for (uint32_t j = 0; j < st->tproxy.len; j++) {
            embi_an *p = st->tproxy.data[j];
            p->target = nullptr;
            p->retired = true;
            p->user = nullptr;
            free_record(p);
        }
        st->tproxy.len = 0;
        st_free(an, st);
    }
    embi_vec_free(&list);
    while (an->all.len > 0) {
        embi_an *a = an->all.data[an->all.len - 1];
        a->target = nullptr;
        a->user = nullptr; /* the host's realm is gone: nobody holds the record */
        retire(a, false);  /* unlists it, and frees it */
    }
    /* What is left is in neither list: a hosted transition proxy whose element went (hook_gone takes it off the element and
     * leaves it to the host's wrapper). The wrapper's finalizer may never run before the realm goes, so free them here. */
    if (embi_hashmap_len(&an->ids) > 0) {
        EMBI_VEC(embi_an *) rest = {0};
        embi_hashmap_iter rit = embi_hashmap_iter_begin(&an->ids);
        uint64_t rk = 0, rv = 0;
        while (embi_hashmap_iter_next(&rit, &rk, &rv)) {
            embi_vec_push(&rest, (embi_an *)(uintptr_t)rv);
        }
        for (uint32_t i = 0; i < rest.len; i++) {
            rest.data[i]->target = nullptr;
            rest.data[i]->user = nullptr;
            free_record(rest.data[i]);
        }
        embi_vec_free(&rest);
    }
    an->zombies.len = 0;
    if (an->kft_valid) {
        embi_kf_table_free(&an->kft);
        an->kft_valid = false;
    }
    embi_arena_free(&an->kfar);
    embi_arena_init(&an->kfar, 4096);
    an->neasings = 0; /* the memo's text and easings lived in kfar */
    memset(an->easing_hash, 0, sizeof an->easing_hash);
    embi_arena_reset(&an->scratch);
    /* the old document's elements: the parent styles kept for them go, and nothing they did counts any more */
    embi_hashmap_iter pit = embi_hashmap_iter_begin(&an->pstyle);
    uint64_t pk = 0, pv = 0;
    while (embi_hashmap_iter_next(&pit, &pk, &pv)) {
        embi_style_release(an->se, (const embi_style *)(uintptr_t)pv);
    }
    embi_hashmap_clear(&an->pstyle);
    embi_hashmap_clear(&an->motion[0]);
    embi_hashmap_clear(&an->motion[1]);
    embi_hashmap_clear(&an->decl);
    an->nsheets = 0;
    an->want = false;
    an->time = 0.0;
}

void embi_animator_free(embi_animator *an)
{
    if (an == nullptr) {
        return;
    }
    embi_style_set_anim_hooks(an->se, nullptr);
    embi_animator_reset(an);
    embi_vec_free(&an->all);
    embi_vec_free(&an->events);
    embi_vec_free(&an->effs);
    embi_vec_free(&an->tspec);
    embi_overlay_free(&an->ov);
    embi_hashmap_free(&an->states);
    embi_hashmap_free(&an->pstyle); /* emptied by embi_animator_reset */
    embi_hashmap_free(&an->motion[0]);
    embi_hashmap_free(&an->motion[1]);
    embi_hashmap_free(&an->decl);
    embi_hashmap_free(&an->ids);
    embi_vec_free(&an->zombies);
    embi_free(an->easings, sizeof(an->easings[0]) * an->capeasings);
    embi_arena_free(&an->scratch);
    embi_arena_free(&an->kfar);
    if (an->sheets != nullptr) {
        embi_free(an->sheets, sizeof(embi_css_sheet *) * an->capsheets);
    }
    embi_free(an, sizeof *an);
}

/* The poisoned parts of the values the transitions of st keep. */
static uint32_t trans_poisoned(const el_state *st)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < st->trans.n; i++) {
        const embi_transition *t = &st->trans.v[i];
        n += embi_value_debug_poisoned(&t->from) + embi_value_debug_poisoned(&t->to) +
             embi_value_debug_poisoned(&t->reversing_start);
    }
    return n;
}

uint32_t embi_animator_debug_poisoned(const embi_animator *an)
{
    uint32_t n = an->debug_poison_reads;
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        const el_state *st = (const el_state *)(uintptr_t)v;
        n += embi_style_debug_poisoned(st->base) + trans_poisoned(st);
    }
    it = embi_hashmap_iter_begin(&an->pstyle);
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        n += embi_style_debug_poisoned((const embi_style *)(uintptr_t)v);
    }
    return n;
}

void embi_animator_set_sheets(embi_animator *an, embi_css_sheet *const *sheets, uint32_t n, const embi_style_env *env)
{
    if (n > an->capsheets) {
        an->sheets = embi_realloc(an->sheets, sizeof(embi_css_sheet *) * an->capsheets, sizeof(embi_css_sheet *) * n);
        an->capsheets = n;
    }
    for (uint32_t i = 0; i < n; i++) {
        an->sheets[i] = sheets[i];
    }
    an->nsheets = n;
    if (env != nullptr) {
        an->env = *env;
    }
    if (an->kft_valid) {
        embi_kf_table_free(&an->kft);
        an->kft_valid = false;
    }
}

void embi_animator_set_env(embi_animator *an, const embi_style_env *env)
{
    embi_animator_set_sheets(an, an->sheets, an->nsheets, env);
}

static const embi_kf_table *kf_table(embi_animator *an)
{
    if (!an->kft_valid) {
        memset(&an->kft, 0, sizeof an->kft);
        embi_kf_table_collect(&an->kfar, &an->kft, an->sheets, an->nsheets, &an->env);
        an->kft_valid = true;
    }
    return &an->kft;
}

double embi_animator_time(const embi_animator *an)
{
    return an->time * 1000.0;
}

uint32_t embi_animator_marked(const embi_animator *an)
{
    return an->marked;
}

embi_style_engine *embi_animator_style(const embi_animator *an)
{
    return an->se;
}

uint32_t embi_animator_count(const embi_animator *an)
{
    return an->live;
}

size_t embi_animator_arena_bytes(const embi_animator *an)
{
    size_t total = embi_style_vblock_bytes(an->se);
    for (const embi_arena_block *b = an->scratch.first; b != nullptr; b = b->next) {
        total += b->cap + sizeof *b;
    }
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        for (const embi_arena_block *b = ((const el_state *)(uintptr_t)v)->tar.first; b != nullptr; b = b->next) {
            total += b->cap + sizeof *b;
        }
    }
    return total;
}

/* ------------------------------------------------------------------ reading the style */

/* The lists the CSS animations are made from, as one spec per animation-name item. */
static uint32_t read_css_specs(const embi_style *base, embi_css_aspec *out, uint32_t cap)
{
    const embi_value *names = embi_style_value(base, EMBI_PROP_animation_name);
    if (names == nullptr) {
        return 0;
    }
    uint32_t n = item_count(names);
    uint32_t k = 0;
    const embi_value *dur = embi_style_value(base, EMBI_PROP_animation_duration);
    const embi_value *delay = embi_style_value(base, EMBI_PROP_animation_delay);
    const embi_value *iters = embi_style_value(base, EMBI_PROP_animation_iteration_count);
    const embi_value *dir = embi_style_value(base, EMBI_PROP_animation_direction);
    const embi_value *fill = embi_style_value(base, EMBI_PROP_animation_fill_mode);
    const embi_value *play = embi_style_value(base, EMBI_PROP_animation_play_state);
    const embi_value *ease = embi_style_value(base, EMBI_PROP_animation_timing_function);
    const embi_value *comp = embi_style_value(base, EMBI_PROP_animation_composition);
    for (uint32_t i = 0; i < n && k < cap; i++) {
        const embi_value *nm = item_at(names, i);
        embi_atom name = 0;
        if (nm != nullptr && nm->kind == EMBI_V_IDENT) {
            name = nm->ident;
        } else if (nm != nullptr && nm->kind == EMBI_V_STRING) {
            name = embi_atom_intern(nm->str.ptr, nm->str.len);
        }
        if (name == 0) {
            continue;
        }
        embi_css_aspec s;
        memset(&s, 0, sizeof s);
        s.name = name;
        const embi_value *d = item_at(dur, i);
        s.dur = (d != nullptr && d->kind == EMBI_V_TIME) ? d->num : 0.0;
        s.delay = time_of(item_at(delay, i), 0.0);
        const embi_value *it = item_at(iters, i);
        s.iters = (it != nullptr && (it->kind == EMBI_V_NUMBER || it->kind == EMBI_V_INTEGER)) ? it->num : (kw_is(it, EMBI_KW_infinite) ? INFINITY : 1.0);
        const embi_value *dv = item_at(dir, i);
        s.dir = EMBI_AD_NORMAL;
        if (dv != nullptr && dv->kind == EMBI_V_KEYWORD) {
            s.dir = dv->kw == EMBI_KW_reverse ? EMBI_AD_REVERSE
                    : dv->kw == EMBI_KW_alternate ? EMBI_AD_ALTERNATE
                    : dv->kw == EMBI_KW_alternate_reverse ? EMBI_AD_ALTERNATE_REVERSE : EMBI_AD_NORMAL;
        }
        const embi_value *fv = item_at(fill, i);
        s.fill = 0;
        if (fv != nullptr && fv->kind == EMBI_V_KEYWORD) {
            s.fill = fv->kw == EMBI_KW_forwards ? EMBI_FILL_FORWARDS
                     : fv->kw == EMBI_KW_backwards ? EMBI_FILL_BACKWARDS
                     : fv->kw == EMBI_KW_both ? EMBI_FILL_BOTH : 0;
        }
        s.paused = kw_is(item_at(play, i), EMBI_KW_paused);
        const embi_value *ev = item_at(ease, i);
        (void)embi_easing_of_value(ev, &s.ease);
        const embi_value *cv = item_at(comp, i);
        s.comp = kw_is(cv, EMBI_KW_add) ? EMBI_COMP_ADD : kw_is(cv, EMBI_KW_accumulate) ? EMBI_COMP_ACCUMULATE : EMBI_COMP_REPLACE;
        out[k++] = s;
    }
    return k;
}

static bool has_css_animation(const embi_style *base)
{
    const embi_value *names = embi_style_value(base, EMBI_PROP_animation_name);
    if (names == nullptr) {
        return false;
    }
    if (names->kind == EMBI_V_KEYWORD) {
        return false;
    }
    return true;
}

static bool has_transition(const embi_style *base)
{
    const embi_value *dur = embi_style_value(base, EMBI_PROP_transition_duration);
    const embi_value *delay = embi_style_value(base, EMBI_PROP_transition_delay);
    uint32_t n = item_count(dur), m = item_count(delay);
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *d = item_at(dur, i);
        if (d != nullptr && d->kind == EMBI_V_TIME && d->num > 0.0) {
            return true;
        }
    }
    for (uint32_t i = 0; i < m; i++) {
        const embi_value *d = item_at(delay, i);
        if (d != nullptr && d->kind == EMBI_V_TIME && d->num > 0.0) {
            return true;
        }
    }
    return false;
}

static bool same_content(const embi_style *a, const embi_style *b)
{
    return a == b || embi_style_same_content(a, b);
}

/* ------------------------------------------------------------------ transitions */

static void add_spec(embi_animator *an, embi_prop_id prop, bool all, double dur, double delay, const embi_easing *ease,
                     bool discrete)
{
    embi_trans_spec s;
    memset(&s, 0, sizeof s);
    s.prop = prop;
    s.all = all;
    s.duration = dur;
    s.delay = delay;
    s.easing = ease;
    s.allow_discrete = discrete;
    embi_vec_push(&an->tspec, s);
}

static void build_trans_specs(embi_animator *an, const embi_style *base)
{
    an->tspec.len = 0;
    const embi_value *props = embi_style_value(base, EMBI_PROP_transition_property);
    const embi_value *dur = embi_style_value(base, EMBI_PROP_transition_duration);
    const embi_value *delay = embi_style_value(base, EMBI_PROP_transition_delay);
    const embi_value *ease = embi_style_value(base, EMBI_PROP_transition_timing_function);
    const embi_value *beh = embi_style_value(base, EMBI_PROP_transition_behavior);
    uint32_t n = item_count(props);
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *p = item_at(props, i);
        if (p == nullptr) {
            continue;
        }
        const embi_value *dv = item_at(dur, i);
        double d = (dv != nullptr && dv->kind == EMBI_V_TIME) ? dv->num : 0.0;
        double dl = time_of(item_at(delay, i), 0.0);
        const embi_value *ev = item_at(ease, i);
        const embi_easing *e = nullptr;
        (void)embi_easing_of_value(ev, &e);
        bool disc = kw_is(item_at(beh, i), EMBI_KW_allow_discrete);
        if (p->kind == EMBI_V_KEYWORD && (p->kw == EMBI_KW_all || p->kw == EMBI_KW_none)) {
            if (p->kw == EMBI_KW_all) {
                add_spec(an, 0, true, d, dl, e, disc);
            }
            continue;
        }
        embi_str nm;
        if (p->kind == EMBI_V_KEYWORD) {
            /* a property that is also a keyword of the grammar (top, left, ...) is stored as a keyword */
            const char *kn = embi_kw_name(p->kw);
            if (kn == nullptr) {
                continue;
            }
            nm = (embi_str){kn, (uint32_t)strlen(kn)};
        } else if (p->kind == EMBI_V_IDENT) {
            nm = embi_atom_str(p->ident);
        } else {
            continue;
        }
        if (nm.len >= 2 && nm.ptr[0] == '-' && nm.ptr[1] == '-') {
            continue;
        }
        embi_prop_id id = embi_prop_lookup(nm);
        if (id == EMBI_PROP_NONE) {
            continue;
        }
        if (embi_prop_is_shorthand(id)) {
            const embi_prop_info *pi = embi_prop_info_of(id);
            const uint16_t *subs = embi_prop_longhands(id);
            for (uint32_t k = 0; pi != nullptr && subs != nullptr && k < pi->sub_n; k++) {
                add_spec(an, subs[k], false, d, dl, e, disc);
            }
        } else {
            if (embi_prop_is_logical(id)) {
                /* mapped to the physical property below with the style's writing mode */
                uint16_t wm = EMBI_KW_horizontal_tb, dr = EMBI_KW_ltr;
                const embi_value *wv = embi_style_value(base, EMBI_PROP_writing_mode);
                const embi_value *dv2 = embi_style_value(base, EMBI_PROP_direction);
                if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
                    wm = wv->kw;
                }
                if (dv2 != nullptr && dv2->kind == EMBI_V_KEYWORD) {
                    dr = dv2->kw;
                }
                id = embi_prop_to_physical(id, wm, dr);
            }
            add_spec(an, id, false, d, dl, e, disc);
        }
    }
}

static void queue_trans_events(embi_animator *an, embi_element *el, const embi_trans_events *tev)
{
    for (uint32_t i = 0; i < tev->n; i++) {
        const embi_trans_event *e = &tev->e[i];
        an_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = e->kind == EMBI_TEV_RUN      ? EMBI_CEV_TRANSITIONRUN
                  : e->kind == EMBI_TEV_START  ? EMBI_CEV_TRANSITIONSTART
                  : e->kind == EMBI_TEV_END    ? EMBI_CEV_TRANSITIONEND
                                               : EMBI_CEV_TRANSITIONCANCEL;
        ev.el = el;
        const embi_prop_info *pi = embi_prop_info_of(e->prop);
        ev.name = pi != nullptr ? (embi_str){pi->name, (uint32_t)strlen(pi->name)} : (embi_str){"", 0};
        ev.elapsed = e->elapsed;
        embi_animator_queue(an, &ev);
    }
}

/* ---- changes inherited from an ancestor's animation
 *
 * CSS Transitions 1 section 3 ("Starting of transitions"): the before-change style is the computed values of the previous style
 * change event, with declarative animations updated to the current time; a transition starts only for a property whose
 * before-change and after-change values differ. An ancestor's animation updates the inherited input of the element in its
 * before-change style as well (an em length follows an animated font-size), so that change starts nothing.
 *
 * Done per property. A changed property of the element is its own change when the element's cascade for it changed: the
 * rules as they are now, under the parent style of the element's last restyle (pstyle), no longer give the old value. Its
 * own changes always transition. The others only changed through what the element inherits, and inherited_mode decides:
 *   - KEEP: they transition. No ancestor animated, transitioned or had its animations changed in this frame or the one before
 *     (motion); or the restyle pass is a full resolve (a sheet, media, viewport or root font change: every element's inputs
 *     may have changed); or an ancestor other than the case below had its own inputs changed in this pass (decl: its rules,
 *     attributes or state changed; the style engine says why each element is restyled, embi_style_anim_own_change). So a
 *     declared change anywhere on the chain (#app.dark{color} above or below an animating element, :hover on an ancestor with
 *     a descendant rule) transitions, however long ago an ancestor's animation or transition ended.
 *   - DROP: an ancestor moved and no ancestor had its own inputs changed in this pass: the inherited changes are the
 *     animations' doing, and none transitions.
 *   - PARENT: the parent moved and had its own inputs changed in this pass (a class that changes a declaration and starts an
 *     animation in the same pass), and nothing above it moved or changed. The parent's before-change style is built: its
 *     style now, with the properties whose base value changed in this pass set back to their values in the old parent style,
 *     except those its animations set now (updated to the current time). A change that the element's cascade under it also
 *     gives is the animations' doing; one it does not give transitions.
 * Limits: a declared change of an ancestor other than a moving parent keeps every inherited change of that pass
 * transitioning, also those the animations caused alone (per pass, not per property along the chain); and when the old
 * parent style is not known (the element's first restyle with a transition declared) every change transitions. */
static void motion_note(embi_animator *an, const embi_element *el)
{
    embi_hashmap_put(&an->motion[0], (uint64_t)(uintptr_t)el, 0);
}

static bool moved(const embi_animator *an, const embi_node *n)
{
    uint64_t v = 0, k = (uint64_t)(uintptr_t)n;
    return (embi_hashmap_len(&an->motion[0]) > 0 && embi_hashmap_get(&an->motion[0], k, &v)) ||
           (embi_hashmap_len(&an->motion[1]) > 0 && embi_hashmap_get(&an->motion[1], k, &v));
}

static bool declared_in_pass(const embi_animator *an, const embi_node *n, uint64_t pass)
{
    uint64_t v = 0;
    return an->decl_pass == pass && embi_hashmap_len(&an->decl) > 0 &&
           embi_hashmap_get(&an->decl, (uint64_t)(uintptr_t)n, &v);
}

/* Records why el is restyled in the running pass (inside apply). */
static void decl_note(embi_animator *an, embi_style_engine *e, const embi_element *el)
{
    uint8_t own = embi_style_anim_own_change(e);
    if (own == EMBI_OWN_NONE) {
        return;
    }
    uint64_t pass = embi_style_anim_pass(e);
    if (own == EMBI_OWN_ALL) {
        an->decl_all_pass = pass;
        return;
    }
    if (an->decl_pass != pass) {
        embi_hashmap_clear(&an->decl);
        an->decl_pass = pass;
    }
    embi_hashmap_put(&an->decl, (uint64_t)(uintptr_t)el, 0);
}

/* The base properties of an animated element that changed with its own inputs in this pass (its children's PARENT check). */
static void dprops_note(embi_animator *an, embi_style_engine *e, el_state *st, const embi_style *old_base, const embi_style *base)
{
    if (!an->declared || embi_style_anim_own_change(e) != EMBI_OWN_SELF) {
        return;
    }
    embi_prop_id diff[MAX_CHANGES];
    uint32_t n = old_base != nullptr ? embi_style_diff_props(old_base, base, diff, MAX_CHANGES) : 0; /* nullptr: no change */
    st->dprops.len = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&st->dprops, diff[i]);
    }
    st->dcustom = old_base != nullptr && embi_style_custom_key(old_base) != embi_style_custom_key(base);
    st->dpass = embi_style_anim_pass(e);
}

typedef enum inh_mode { INH_KEEP = 0, INH_DROP, INH_PARENT } inh_mode;

/* See above. For INH_PARENT, *pst is the parent's state. The DOM tree is the flat tree: the engine has no shadow roots or
 * slots. */
static inh_mode inherited_mode(const embi_animator *an, embi_style_engine *e, const embi_element *el, el_state **pst)
{
    if (embi_hashmap_len(&an->motion[0]) + embi_hashmap_len(&an->motion[1]) == 0) {
        return INH_KEEP;
    }
    uint64_t pass = embi_style_anim_pass(e);
    if (an->decl_all_pass == pass) {
        return INH_KEEP;
    }
    const embi_node *par = el->base.parent;
    if (par == nullptr || par->type != EMBI_NODE_ELEMENT) {
        return INH_KEEP;
    }
    bool par_decl = declared_in_pass(an, par, pass);
    bool par_moved = moved(an, par);
    bool above_moved = false;
    for (const embi_node *n = par->parent; n != nullptr && n->type == EMBI_NODE_ELEMENT; n = n->parent) {
        if (declared_in_pass(an, n, pass)) {
            return INH_KEEP;
        }
        above_moved = above_moved || moved(an, n);
    }
    if (!par_decl) {
        return par_moved || above_moved ? INH_DROP : INH_KEEP;
    }
    el_state *ps = st_find(an, (const embi_element *)par);
    if (!par_moved || above_moved || ps == nullptr || ps->dpass != pass || ps->dcustom || ps->base == nullptr) {
        return INH_KEEP;
    }
    *pst = ps;
    return INH_PARENT;
}

/* INH_PARENT: the parent's before-change style (see above). parent is its style now, old_parent the one of the element's last
 * restyle. Owned by the caller. */
static const embi_style *parent_before_change(embi_style_engine *e, const el_state *ps, const embi_style *parent,
                                              const embi_style *old_parent)
{
    embi_prop_id props[MAX_CHANGES];
    const embi_value *vals[MAX_CHANGES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < ps->dprops.len && n < MAX_CHANGES; i++) {
        embi_prop_id q = ps->dprops.data[i];
        const embi_value *now = embi_style_value(parent, q);
        if (now != embi_style_value(ps->base, q)) {
            continue; /* its animations set it now: the before-change style has the value updated to now */
        }
        const embi_value *old = embi_style_value(old_parent, q);
        if (old != nullptr && old != now) {
            props[n] = q;
            vals[n] = old;
            n++;
        }
    }
    return embi_style_overlay(e, parent, props, vals, n);
}

/* The transitions' values live in st->tar. A transition replaced by a new one leaves its values there until no transition
 * runs (the arena is reset then); an element whose transitions keep being replaced while others run gets its arena compacted
 * (the running transitions' values copied into a new one). */
#define TAR_COMPACT_BYTES (16u * 1024u)

static void tar_prepare(el_state *st)
{
    if (st->trans.n == 0) {
        embi_arena_reset(&st->tar);
        return;
    }
    size_t used = 0;
    for (const embi_arena_block *b = st->tar.first; b != nullptr; b = b->next) {
        used += b->cap;
    }
    if (used < TAR_COMPACT_BYTES) {
        return;
    }
    embi_arena fresh;
    embi_arena_init(&fresh, 1024);
    for (uint32_t i = 0; i < st->trans.n; i++) {
        embi_transition *t = &st->trans.v[i];
        embi_value_copy(&fresh, &t->from, &t->from);
        embi_value_copy(&fresh, &t->to, &t->to);
        embi_value_copy(&fresh, &t->reversing_start, &t->reversing_start);
    }
    embi_arena_free(&st->tar);
    st->tar = fresh;
}

static bool prop_in(const embi_prop_id *list, uint32_t n, embi_prop_id p)
{
    for (uint32_t j = 0; j < n; j++) {
        if (list[j] == p) {
            return true;
        }
    }
    return false;
}

static void do_transitions(embi_animator *an, embi_style_engine *e, el_state *st, embi_element *el, const embi_style *old_base,
                           const embi_style *base, const embi_style *parent, const embi_style *old_parent)
{
    build_trans_specs(an, base);
    tar_prepare(st);
    embi_trans_change changes[MAX_CHANGES];
    uint32_t nch = 0;
    if (old_base != nullptr && old_base != base) {
        embi_prop_id diff[MAX_CHANGES];
        uint32_t nd = embi_style_diff_props(old_base, base, diff, MAX_CHANGES);
        embi_prop_id own[MAX_CHANGES];
        uint32_t nown = 0;
        embi_prop_id keep[MAX_CHANGES]; /* INH_PARENT: the inherited changes that the parent's declared change made */
        uint32_t nkeep = 0;
        inh_mode mode = INH_KEEP;
        el_state *ps = nullptr;
        if (nd > 0 && old_parent != nullptr) {
            mode = inherited_mode(an, e, el, &ps);
        }
        const embi_style *alt = mode != INH_KEEP ? embi_style_anim_cascade_under(e, el, old_parent) : nullptr;
        if (alt == nullptr) {
            mode = INH_KEEP;
        } else {
            nown = embi_style_diff_props(old_base, alt, own, MAX_CHANGES);
            embi_style_release(e, alt);
            if (mode == INH_PARENT) {
                const embi_style *bcp = parent_before_change(e, ps, parent, old_parent);
                const embi_style *bc = embi_style_anim_cascade_under(e, el, bcp);
                nkeep = embi_style_diff_props(bc, base, keep, MAX_CHANGES);
                embi_style_release(e, bc);
                embi_style_release(e, bcp);
            }
        }
        for (uint32_t i = 0; i < nd; i++) {
            if (!embi_anim_animatable(diff[i])) {
                continue;
            }
            if (mode != INH_KEEP && !prop_in(own, nown, diff[i]) && !(mode == INH_PARENT && prop_in(keep, nkeep, diff[i]))) {
                continue; /* only the inherited input changed, by the animations: the before-change style has it too */
            }
            const embi_value *bv = embi_style_value(old_base, diff[i]);
            const embi_value *av = embi_style_value(base, diff[i]);
            if (bv == nullptr || av == nullptr) {
                continue;
            }
            /* a transition keeps these values (by value, with their parts): copies in tar, not the styles' memory */
            embi_value *cb = embi_arena_alloc(&st->tar, sizeof *cb, 8);
            embi_value *ca = embi_arena_alloc(&st->tar, sizeof *ca, 8);
            embi_value_copy(&st->tar, bv, cb);
            embi_value_copy(&st->tar, av, ca);
            changes[nch].prop = diff[i];
            changes[nch].before = cb;
            changes[nch].after = ca;
            nch++;
        }
    }
    const embi_value *disp = old_base != nullptr ? embi_style_value(old_base, EMBI_PROP_display) : nullptr;
    const embi_value *col = embi_style_value(base, EMBI_PROP_color);
    embi_trans_input in;
    memset(&in, 0, sizeof in);
    in.specs = an->tspec.data;
    in.nspecs = an->tspec.len;
    in.changes = changes;
    in.nchanges = nch;
    in.display_none = kw_is(disp, EMBI_KW_none);
    in.current = (col != nullptr && col->kind == EMBI_V_COLOR && col->color.space != EMBI_CS_CURRENT) ? &col->color : nullptr;
    embi_trans_events tev = {0};
    uint32_t before = st->trans.n;
    embi_trans_update(&st->tar, &st->trans, &in, an->time, &tev);
    queue_trans_events(an, st->el, &tev);
    embi_trans_events_free(&tev);
    if (st->trans.n != before || nch > 0) {
        an->want = true;
    }
}

/* ------------------------------------------------------------------ CSS animations */

static void css_set_timing(embi_an *a, const embi_css_aspec *s)
{
    a->timing.delay = s->delay;
    a->timing.end_delay = 0.0;
    a->timing.duration = s->dur;
    a->timing.iterations = s->iters;
    a->timing.iteration_start = 0.0;
    a->timing.easing = nullptr;
    a->timing.direction = s->dir;
    a->timing.fill = s->fill;
    a->composite = s->comp;
}

static bool css_spec_same(const embi_css_aspec *x, const embi_css_aspec *y)
{
    return x->name == y->name && x->dur == y->dur && x->delay == y->delay && x->iters == y->iters && x->dir == y->dir &&
           x->fill == y->fill && x->comp == y->comp && x->paused == y->paused && x->ease == y->ease;
}

static void css_reconcile(embi_animator *an, el_state *st, embi_element *el, const embi_style *base)
{
    embi_css_aspec specs[64];
    uint32_t ns = has_css_animation(base) ? read_css_specs(base, specs, 64) : 0;
    bool matched_spec[64] = {false};
    /* The existing CSS animations, in order. */
    embi_an *cur[64];
    uint32_t nc = 0;
    for (uint32_t i = 0; i < st->anims.len && nc < 64; i++) {
        if (st->anims.data[i]->kind == EMBI_AN_CSSA) {
            cur[nc++] = st->anims.data[i];
        }
    }
    bool matched_cur[64] = {false};
    for (uint32_t i = 0; i < nc; i++) {
        for (uint32_t j = 0; j < ns; j++) {
            if (!matched_spec[j] && specs[j].name == cur[i]->css.name) {
                matched_spec[j] = true;
                matched_cur[i] = true;
                embi_an *a = cur[i];
                if (!css_spec_same(&a->css, &specs[j])) {
                    bool was_paused = a->css.paused;
                    a->css = specs[j];
                    css_set_timing(a, &specs[j]);
                    if (specs[j].paused != was_paused) {
                        const char *err = nullptr;
                        bool ok;
                        if (specs[j].paused) {
                            embi_an_pause_internal(a, &err, &ok);
                        } else {
                            embi_an_play_internal(a, false, &err, &ok);
                        }
                    }
                    embi_an_update_finished(a, false, false);
                    an->want = true;
                }
                break;
            }
        }
    }
    for (uint32_t i = 0; i < nc; i++) {
        if (!matched_cur[i]) {
            retire(cur[i], true);
        }
    }
    const embi_kf_table *kt = ns > 0 ? kf_table(an) : nullptr;
    for (uint32_t j = 0; j < ns; j++) {
        if (matched_spec[j]) {
            continue;
        }
        const embi_css_rule *rule = embi_kf_table_find_rule(kt, specs[j].name);
        if (rule == nullptr) {
            continue;
        }
        embi_an *a = new_record(an, EMBI_AN_CSSA, false);
        a->target = el;
        a->has_effect = true;
        a->css = specs[j];
        css_set_timing(a, &specs[j]);
        a->nkfin = embi_kf_rule_inputs(&a->ar, rule, &a->kfin);
        embi_animator_list(a);
        embi_animator_state_add(a);
        const char *err = nullptr;
        bool ok;
        embi_an_play_internal(a, true, &err, &ok);
        if (specs[j].paused) {
            embi_an_pause_internal(a, &err, &ok);
        }
        an->want = true;
    }
}

/* ------------------------------------------------------------------ keyframe resolution and sampling */

static void resolve_kf(embi_animator *an, embi_style_engine *e, el_state *st, embi_an *a, const embi_style *base,
                       const embi_style *parent, bool *ctx_ready, embi_element *el, bool is_root)
{
    if (a->kf != nullptr && a->kf_gen == st->base_gen) {
        return;
    }
    embi_arena_reset(&a->rar);
    a->kf = nullptr;
    if (a->nkfin == 0) {
        a->kf = embi_kf_build(&a->rar, nullptr, 0);
        a->kf_gen = st->base_gen;
        return;
    }
    if (!*ctx_ready) {
        embi_style_anim_ctx(e, el, base, parent, is_root);
        *ctx_ready = true;
    }
    uint16_t wm = EMBI_KW_horizontal_tb, dr = EMBI_KW_ltr;
    const embi_value *wv = embi_style_value(base, EMBI_PROP_writing_mode);
    const embi_value *dv = embi_style_value(base, EMBI_PROP_direction);
    if (wv != nullptr && wv->kind == EMBI_V_KEYWORD) {
        wm = wv->kw;
    }
    if (dv != nullptr && dv->kind == EMBI_V_KEYWORD) {
        dr = dv->kw;
    }
    embi_kf_input *in = embi_arena_alloc(&a->rar, sizeof(embi_kf_input) * a->nkfin, 8);
    uint32_t m = 0;
    for (uint32_t i = 0; i < a->nkfin; i++) {
        embi_kf_input x = a->kfin[i];
        if (x.prop == EMBI_PROP_NONE || x.prop >= EMBI_PROP_COUNT || embi_prop_is_shorthand(x.prop)) {
            continue;
        }
        embi_prop_id p = x.prop;
        if (embi_prop_is_logical(p)) {
            p = embi_prop_to_physical(p, wm, dr);
        }
        x.prop = p;
        if (x.value.kind != EMBI_V_NONE) {
            const embi_value *res = nullptr;
            switch (x.value.kind) {
            case EMBI_V_INITIAL:
                res = embi_style_value(embi_style_initial(e), p);
                break;
            case EMBI_V_INHERIT:
                res = embi_style_value(parent, p);
                break;
            case EMBI_V_UNSET:
                res = embi_prop_is_inherited(p) ? embi_style_value(parent, p) : embi_style_value(embi_style_initial(e), p);
                break;
            case EMBI_V_REVERT:
            case EMBI_V_REVERT_LAYER:
            case EMBI_V_VAR:
            case EMBI_V_RAW:
                res = nullptr;
                break;
            default:
                res = embi_style_anim_compute(e, p, &x.value);
                break;
            }
            if (res == nullptr) {
                continue;
            }
            if (x.value.kind == EMBI_V_INHERIT || x.value.kind == EMBI_V_UNSET) {
                /* the parent's value may be an animated one (a value block that goes with the parent's overlay): the set
                 * keeps a copy */
                embi_value_copy(&a->rar, res, &x.value);
            } else {
                x.value = *res;
            }
        }
        in[m++] = x;
    }
    a->kf = embi_kf_build(&a->rar, in, m);
    a->kf_gen = st->base_gen;
    (void)an;
}

typedef struct sample_user {
    const embi_style *base;
    el_state *st;
} sample_user;

static const embi_value *env_base(void *user, embi_prop_id prop)
{
    const sample_user *su = user;
    return embi_style_value(su->base, prop);
}

static bool env_important(void *user, embi_prop_id prop)
{
    const sample_user *su = user;
    for (uint32_t i = 0; i < su->st->nimp; i++) {
        if (su->st->imp_props[i] == prop) {
            return su->st->imp_flags[i];
        }
    }
    return false;
}

/* The properties whose used value cannot be negative: const-initialized, no lazy global state. */
static const embi_prop_id g_nonneg[] = {
    EMBI_PROP_width,         EMBI_PROP_height,         EMBI_PROP_min_width,         EMBI_PROP_min_height,
    EMBI_PROP_max_width,     EMBI_PROP_max_height,     EMBI_PROP_padding_top,       EMBI_PROP_padding_right,
    EMBI_PROP_padding_bottom, EMBI_PROP_padding_left,  EMBI_PROP_border_top_width,  EMBI_PROP_border_right_width,
    EMBI_PROP_border_bottom_width, EMBI_PROP_border_left_width, EMBI_PROP_font_size, EMBI_PROP_line_height,
    EMBI_PROP_flex_grow,     EMBI_PROP_flex_shrink,    EMBI_PROP_row_gap,           EMBI_PROP_column_gap,
    EMBI_PROP_outline_width, EMBI_PROP_column_width,   EMBI_PROP_perspective,
};

/* An overshooting easing can take a value out of its range; the style engine does not clamp animated values. */
static void clamp_value(embi_prop_id p, embi_value *v)
{
    if (p == EMBI_PROP_opacity) {
        if (v->kind == EMBI_V_NUMBER) {
            v->num = v->num < 0.0 ? 0.0 : (v->num > 1.0 ? 1.0 : v->num);
        }
        return;
    }
    for (uint32_t i = 0; i < sizeof g_nonneg / sizeof g_nonneg[0]; i++) {
        if (g_nonneg[i] == p) {
            if ((v->kind == EMBI_V_LENGTH || v->kind == EMBI_V_PERCENT || v->kind == EMBI_V_NUMBER) && v->num < 0.0) {
                v->num = 0.0;
            }
            return;
        }
    }
}

/* Whether the animation changes its value with time right now. */
static bool anim_moving(const embi_an *a)
{
    if (!a->has_effect || a->pending_task != 0 || !isnan(a->hold) || isnan(a->start) || a->rate == 0.0 || !a->has_timeline) {
        return false;
    }
    embi_anim_sample s;
    bool res;
    embi_an_effect_local(a, &s, &res);
    return res && s.phase == EMBI_PH_ACTIVE;
}

static uint32_t trans_poisoned(const el_state *st);

/* Tests, in the poison mode of the arenas: counts the values in poisoned memory of what this restyle reads besides the slot
 * and the parent (the engine counts those): the new base, the kept base, the transitions' values and the kept parent style. */
static void debug_poison_check(embi_animator *an, const el_state *st, const embi_element *el, const embi_style *base)
{
    uint64_t v = 0;
    uint32_t k = embi_style_debug_poisoned(base);
    if (st != nullptr) {
        k += embi_style_debug_poisoned(st->base) + trans_poisoned(st);
    }
    if (embi_hashmap_len(&an->pstyle) > 0 && embi_hashmap_get(&an->pstyle, (uint64_t)(uintptr_t)el, &v)) {
        k += embi_style_debug_poisoned((const embi_style *)(uintptr_t)v);
    }
    an->debug_poison_reads += k;
    embi_arena_debug_poison_note(k);
}

static const embi_style *hook_apply(void *user, embi_style_engine *e, embi_element *el, const embi_style *old_slot,
                                    const embi_style *base, const embi_style *parent, bool is_root)
{
    embi_animator *an = user;
    el_state *st = st_find(an, el);
    if (embi_arena_debug_poison_on()) {
        debug_poison_check(an, st, el, base);
    }
    const embi_style *pending_release = nullptr; /* the old parent style, released once do_transitions has used it */
    bool css_decl = has_css_animation(base);
    if (!an->declared) {
        /* The animation properties live in one interned group: a group seen without a declaration is not looked at again. */
        const void *g = embi_style_group_key(base, EMBI_PROP_transition_duration);
        if (g == nullptr || g != an->plain_anim) {
            if (css_decl || has_transition(base)) {
                an->declared = true;
            } else {
                an->plain_anim = g;
            }
        }
    }
    if (an->declared) {
        decl_note(an, e, el);
    }
    /* the parent style the element was last restyled under (kept for the elements that declare transitions) */
    const embi_style *old_parent = nullptr;
    {
        uint64_t pv = 0;
        uint64_t k = (uint64_t)(uintptr_t)el;
        bool had = embi_hashmap_len(&an->pstyle) > 0 && embi_hashmap_get(&an->pstyle, k, &pv);
        old_parent = had ? (const embi_style *)(uintptr_t)pv : nullptr;
        if (has_transition(base) && !is_root) {
            if (parent != old_parent) {
                embi_style_retain(parent);
                embi_hashmap_put(&an->pstyle, k, (uint64_t)(uintptr_t)parent);
                if (had) {
                    pending_release = old_parent;
                }
            }
        } else if (had) {
            embi_hashmap_remove(&an->pstyle, k);
            pending_release = old_parent;
        }
    }
    if (st == nullptr) {
        bool trans = old_slot != nullptr && has_transition(base) && !same_content(old_slot, base);
        if (!css_decl && !trans) {
            if (pending_release != nullptr) {
                embi_style_release(an->se, pending_release);
            }
            return base;
        }
        st = st_get(an, el);
    }
    an->in_apply = true;
    const embi_style *old_base = st->base != nullptr ? st->base : old_slot;
    bool base_changed = st->base == nullptr || !same_content(st->base, base);
    if (base_changed) {
        st->base_gen = ++an->gen_counter;
    }
    bool ctx_ready = false;

    if (base_changed) {
        if (old_base != nullptr && (has_transition(base) || st->trans.n > 0)) {
            do_transitions(an, e, st, el, old_base, base, parent, old_parent);
        }
        css_reconcile(an, st, el, base);
    } else if (css_decl && st->anims.len == 0) {
        css_reconcile(an, st, el, base);
    }
    if (pending_release != nullptr) {
        embi_style_release(an->se, pending_release);
    }
    dprops_note(an, e, st, base_changed ? old_base : nullptr, base);

    /* the effects */
    an->effs.len = 0;
    bool moving = st->trans.n > 0;
    uint32_t nprops = 0;
    embi_prop_id props[EMBI_IMP_MAX];
    for (uint32_t i = 0; i < st->anims.len; i++) {
        embi_an *a = st->anims.data[i];
        if (!a->has_effect || a->replace_state == EMBI_REPLACE_REMOVED) {
            continue;
        }
        resolve_kf(an, e, st, a, base, parent, &ctx_ready, el, is_root);
        if (a->kf == nullptr) {
            continue;
        }
        double cur = embi_an_cur_time(a);
        if (isnan(cur)) {
            continue;
        }
        embi_anim_effect fx;
        memset(&fx, 0, sizeof fx);
        fx.kf = a->kf;
        fx.timing = a->timing;
        fx.timing.rate = a->rate;
        fx.keyframe_easing = a->kind == EMBI_AN_CSSA ? a->css.ease : nullptr;
        fx.start = a->start;
        fx.hold = a->hold;
        fx.held = !isnan(a->hold);
        fx.composite = a->composite;
        fx.iteration_composite = a->iter_composite;
        embi_vec_push(&an->effs, fx);
        if (anim_moving(a)) {
            moving = true;
        }
        embi_anim_sample s;
        bool res;
        embi_an_effect_local(a, &s, &res);
        a->applied_phase = res ? s.phase : EMBI_AN_NO_PHASE;
        uint32_t np = embi_kf_prop_count(a->kf);
        for (uint32_t k = 0; k < np && nprops < EMBI_IMP_MAX; k++) {
            embi_prop_id p = embi_kf_prop_at(a->kf, k);
            bool seen = false;
            for (uint32_t q = 0; q < nprops && !seen; q++) {
                seen = props[q] == p;
            }
            if (!seen) {
                props[nprops++] = p;
            }
        }
    }
    /* the !important declarations of the element beat the animations */
    if (nprops > 0 && (st->imp_gen != st->base_gen || st->imp_ver != st->ver)) {
        if (!ctx_ready) {
            embi_style_anim_ctx(e, el, base, parent, is_root);
            ctx_ready = true;
        }
        st->nimp = nprops;
        memcpy(st->imp_props, props, sizeof(embi_prop_id) * nprops);
        embi_style_anim_important(e, props, nprops, st->imp_flags);
        st->imp_gen = st->base_gen;
        st->imp_ver = st->ver;
    } else if (nprops == 0) {
        st->nimp = 0;
    }

    const embi_style *result = base;
    if (an->effs.len > 0 || st->trans.n > 0) {
        embi_arena *ar = &an->scratch;
        sample_user su = {base, st};
        const embi_value *col = embi_style_value(base, EMBI_PROP_color);
        embi_anim_env env;
        memset(&env, 0, sizeof env);
        env.user = &su;
        env.base = env_base;
        env.important = env_important;
        env.current = (col != nullptr && col->kind == EMBI_V_COLOR && col->color.space != EMBI_CS_CURRENT) ? &col->color : nullptr;
        embi_anim_sample_effects(ar, &env, an->effs.data, an->effs.len, &st->trans, an->time, &an->ov);
        if (an->ov.n > 0) {
            /* The lifetime rule (top of the file): the values go into a value block as deep copies; the groups of the
             * overlay, and of every style that later inherits one of them, pin the block. */
            embi_prop_id oprops[128];
            const embi_value *ovals[128];
            uint32_t n = 0;
            embi_style_vblock *vb = embi_style_vblock_open(e, 512);
            embi_arena *va = embi_style_vblock_arena(vb);
            for (uint32_t i = 0; i < an->ov.n && n < 128; i++) {
                embi_value *pv = embi_arena_alloc(va, sizeof *pv, 8);
                embi_value_copy(va, &an->ov.e[i].value, pv);
                clamp_value(an->ov.e[i].prop, pv);
                oprops[n] = an->ov.e[i].prop;
                ovals[n] = pv;
                n++;
            }
            embi_style_vblock_seal(e, vb);
            result = embi_style_overlay(e, base, oprops, ovals, n);
            embi_style_vblock_drop(e, vb);
        }
        an->ov.n = 0; /* its entries point into the scratch */
        embi_arena_reset(ar);
    }
    st->animating = moving;
    if (moving || st->trans.n > 0) {
        motion_note(an, el);
    }

    /* The state keeps its own reference on the base style (the before-change style of the next restyle). A base that only equals
     * the kept one in content replaces it too, so the kept style does not hold on to the groups (and the value blocks they pin,
     * the animated values the element inherited) of an older restyle. */
    if (st->base != base) {
        const embi_style *old = st->base;
        embi_style_retain(base);
        st->base = base;
        if (old != nullptr) {
            embi_style_release(e, old);
        }
    }
    if (result != base) {
        embi_style_release(e, base); /* the overlay does not hold it */
    }
    an->in_apply = false;
    if (st->anims.len == 0 && st->tproxy.len == 0 && st->trans.n == 0) {
        st_free(an, st);
    }
    return result;
}

/* ------------------------------------------------------------------ element removal */

static void hook_gone(void *user, embi_element *el)
{
    embi_animator *an = user;
    el_state *st = st_find(an, el);
    {
        uint64_t k = (uint64_t)(uintptr_t)el, pv = 0;
        embi_hashmap_remove(&an->motion[0], k);
        embi_hashmap_remove(&an->motion[1], k);
        embi_hashmap_remove(&an->decl, k);
        if (embi_hashmap_get(&an->pstyle, k, &pv)) {
            embi_hashmap_remove(&an->pstyle, k);
            embi_style_release(an->se, (const embi_style *)(uintptr_t)pv);
        }
    }
    purge_events(an, nullptr, el);
    if (st == nullptr) {
        return;
    }
    bool q = an->quiet;
    an->quiet = true;
    /* CSS animations and transitions end with the element's style; Web animations wait for it to come back. */
    for (uint32_t i = st->anims.len; i > 0; i--) {
        embi_an *a = st->anims.data[i - 1];
        if (a->kind == EMBI_AN_CSSA || a->orphan) {
            retire(a, false);
        }
    }
    for (uint32_t j = 0; j < st->tproxy.len; j++) {
        embi_an *p = st->tproxy.data[j];
        p->target = nullptr;
        p->retired = true;
        if (p->user == nullptr) {
            free_record(p);
        }
    }
    st->tproxy.len = 0;
    embi_transitions_free(&st->trans);
    memset(&st->trans, 0, sizeof st->trans);
    st_release_base(an, st);
    if (st->anims.len == 0) {
        st_free(an, st);
    } else {
        st->base_gen = ++an->gen_counter;
    }
    an->quiet = q;
}

/* ------------------------------------------------------------------ CSS animation events */

static void css_events(embi_animator *an, embi_an *a)
{
    embi_anim_sample s;
    bool res;
    embi_an_effect_local(a, &s, &res);
    uint8_t phase = res ? s.phase : EMBI_AN_NO_PHASE;
    uint8_t prev = a->last_phase;
    double act = (a->timing.duration == 0.0 || a->timing.iterations == 0.0) ? 0.0 : a->timing.duration * a->timing.iterations;
    an_event ev;
    memset(&ev, 0, sizeof ev);
    ev.el = a->target;
    ev.name_atom = a->css.name;
    if (a->target == nullptr || phase == prev) {
        if (phase == EMBI_PH_ACTIVE && a->target != nullptr && s.iteration != a->last_iter) {
            ev.kind = EMBI_CEV_ANIMATIONITERATION;
            ev.elapsed = a->timing.duration * (s.iteration - a->timing.iteration_start);
            embi_animator_queue(an, &ev);
        }
    } else if (phase != EMBI_AN_NO_PHASE) {
        double start_elapsed = fmax(fmin(-a->timing.delay, act), 0.0);
        bool from_before = prev == EMBI_AN_NO_PHASE || prev == EMBI_PH_BEFORE;
        if (from_before && (phase == EMBI_PH_ACTIVE || phase == EMBI_PH_AFTER)) {
            ev.kind = EMBI_CEV_ANIMATIONSTART;
            ev.elapsed = start_elapsed;
            embi_animator_queue(an, &ev);
            if (phase == EMBI_PH_AFTER) {
                ev.kind = EMBI_CEV_ANIMATIONEND;
                ev.elapsed = act;
                embi_animator_queue(an, &ev);
            }
        } else if (prev == EMBI_PH_ACTIVE && (phase == EMBI_PH_AFTER || phase == EMBI_PH_BEFORE)) {
            ev.kind = EMBI_CEV_ANIMATIONEND;
            ev.elapsed = phase == EMBI_PH_AFTER ? act : 0.0;
            embi_animator_queue(an, &ev);
        } else if (prev == EMBI_PH_AFTER && (phase == EMBI_PH_ACTIVE || phase == EMBI_PH_BEFORE)) {
            ev.kind = EMBI_CEV_ANIMATIONSTART;
            ev.elapsed = act;
            embi_animator_queue(an, &ev);
            if (phase == EMBI_PH_BEFORE) {
                ev.kind = EMBI_CEV_ANIMATIONEND;
                ev.elapsed = 0.0;
                embi_animator_queue(an, &ev);
            }
        }
    }
    a->last_phase = phase;
    if (res) {
        a->last_iter = s.iteration;
    }
}

/* ------------------------------------------------------------------ the frame */

static void flush_events(embi_animator *an)
{
    /* Handlers may queue more events (a listener that starts an animation); those wait for the next frame. */
    uint32_t n = an->events.len;
    EMBI_VEC(an_event) batch = {0};
    for (uint32_t i = 0; i < n; i++) {
        embi_vec_push(&batch, an->events.data[i]);
    }
    an->events.len = 0;
    for (uint32_t i = 0; i < batch.len; i++) {
        an_event ev = batch.data[i];
        if (ev.kind >= 0) {
            if (an->host.css_event != nullptr && ev.el != nullptr) {
                embi_str nm = ev.name.len > 0 ? ev.name : embi_atom_str(ev.name_atom);
                an->host.css_event(an->host.user, ev.el, ev.kind, nm, ev.elapsed);
            }
        } else if (ev.a != nullptr && ev.a->user != nullptr && an->host.notify != nullptr) {
            an->host.notify(an->host.user, ev.a, ev.what, ev.time_ms);
        }
    }
    embi_vec_free(&batch);
}

static void compute_want(embi_animator *an)
{
    bool want = an->events.len > 0;
    for (uint32_t i = 0; i < an->all.len && !want; i++) {
        const embi_an *a = an->all.data[i];
        if (a->pending_task != 0 && a->has_timeline) {
            want = true;
        } else if (a->has_effect && a->has_timeline && embi_an_state(a) == EMBI_PLAY_RUNNING && !isnan(a->start) &&
                   a->rate != 0.0) {
            want = true;
        }
    }
    if (!want) {
        embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
        uint64_t k = 0, v = 0;
        while (!want && embi_hashmap_iter_next(&it, &k, &v)) {
            if (((el_state *)(uintptr_t)v)->trans.n > 0) {
                want = true;
            }
        }
    }
    an->want = want;
}

void embi_animator_frame(embi_animator *an, double now_ms)
{
    sweep(an);
    an->frame_no++;
    embi_hashmap_clear(&an->motion[1]);
    {
        embi_hashmap t = an->motion[0];
        an->motion[0] = an->motion[1];
        an->motion[1] = t;
    }
    an->time = now_ms / 1000.0;
    an->frame_ms = now_ms;
    an->marked = 0;
    an->in_frame = true;
    an->quiet = true;
    /* update animations and send events: pending tasks, finished states, CSS animation phases */
    for (uint32_t i = 0; i < an->all.len; i++) {
        embi_an *a = an->all.data[i];
        if (a->pending_task != 0) {
            embi_an_resolve_pending(a, an->time);
        } else if (a->has_timeline && !isnan(a->start) && a->rate != 0.0) {
            embi_an_update_finished(a, false, false);
        }
        if (a->kind == EMBI_AN_WEB && a->replace_state == EMBI_REPLACE_ACTIVE && embi_an_state(a) == EMBI_PLAY_FINISHED) {
            an->replace_dirty = true;
        }
        if (a->kind == EMBI_AN_CSSA) {
            css_events(an, a);
        }
        if (a->orphan && a->pending_task == 0 && !embi_an_relevant(a)) {
            embi_element *te = a->target;
            destroy_record(a);
            if (te != nullptr) {
                embi_style_mark_element(an->se, te);
            }
            i--; /* the last record moved into this slot */
        }
    }
    if (an->replace_dirty) {
        an->replace_dirty = false;
        replace_pass(an);
    }
    /* transitions */
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    bool any = false;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        el_state *st = (el_state *)(uintptr_t)v;
        if (st->trans.n > 0) {
            motion_note(an, st->el);
            embi_trans_events tev = {0};
            embi_trans_advance(&st->trans, an->time, &tev);
            queue_trans_events(an, st->el, &tev);
            embi_trans_events_free(&tev);
            embi_style_mark_element(an->se, st->el);
            an->marked++;
            any = true;
        }
    }
    /* mark the elements whose animated values move or change phase */
    for (uint32_t i = 0; i < an->all.len; i++) {
        embi_an *a = an->all.data[i];
        if (a->target == nullptr || !a->has_effect) {
            continue;
        }
        bool mark = anim_moving(a);
        if (!mark && a->pending_task == 0) {
            embi_anim_sample s;
            bool res;
            embi_an_effect_local(a, &s, &res);
            uint8_t ph = res ? s.phase : EMBI_AN_NO_PHASE;
            mark = ph != a->applied_phase;
        }
        if (mark) {
            motion_note(an, a->target);
            embi_style_mark_element(an->se, a->target);
            an->marked++;
            any = true;
        }
    }
    an->quiet = false;
    an->in_frame = false;
    if (any && an->host.invalidate != nullptr) {
        an->host.invalidate(an->host.user);
    }
    /* The promise reactions of this frame's resolutions come before the events (finished before the finish event). */
    if (an->host.checkpoint != nullptr) {
        an->host.checkpoint(an->host.user);
    }
    flush_events(an);
    if (an->host.checkpoint != nullptr) {
        an->host.checkpoint(an->host.user);
    }
    compute_want(an);
}

void embi_animator_frame_end(embi_animator *an)
{
    an->quiet = true;
    for (uint32_t i = 0; i < an->all.len; i++) {
        embi_an *a = an->all.data[i];
        if (a->pending_task != 0) {
            embi_an_resolve_pending(a, an->time);
        }
        embi_an_sync_retain(a, true);
    }
    an->quiet = false;
    if (an->host.checkpoint != nullptr) {
        an->host.checkpoint(an->host.user);
    }
    compute_want(an);
}

bool embi_animator_wants_frame(const embi_animator *an)
{
    return an->want;
}

bool embi_animator_declared(const embi_animator *an)
{
    return an != nullptr && an->declared;
}

/* ------------------------------------------------------------------ transition proxies */

static embi_an *proxy_for(embi_animator *an, el_state *st, embi_prop_id prop)
{
    for (uint32_t i = 0; i < st->tproxy.len; i++) {
        if (st->tproxy.data[i]->trans_prop == prop) {
            return st->tproxy.data[i];
        }
    }
    embi_an *p = new_record(an, EMBI_AN_CSST, false);
    p->target = st->el;
    p->trans_prop = prop;
    embi_vec_push(&st->tproxy, p);
    return p;
}

static const embi_transition *proxy_transition(const embi_an *p)
{
    el_state *st = p->target != nullptr ? st_find(p->an, p->target) : nullptr;
    if (st == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < st->trans.n; i++) {
        if (st->trans.v[i].prop == p->trans_prop) {
            return &st->trans.v[i];
        }
    }
    return nullptr;
}

/* Ends or cancels the transition a proxy stands for. */
void embi_animator_trans_end(embi_an *p, bool finish)
{
    embi_animator *an = p->an;
    el_state *st = p->target != nullptr ? st_find(an, p->target) : nullptr;
    if (st == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < st->trans.n; i++) {
        if (st->trans.v[i].prop == p->trans_prop) {
            embi_transition t = st->trans.v[i];
            memmove(&st->trans.v[i], &st->trans.v[i + 1], sizeof(embi_transition) * (st->trans.n - i - 1));
            st->trans.n--;
            an_event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = finish ? EMBI_CEV_TRANSITIONEND : EMBI_CEV_TRANSITIONCANCEL;
            ev.el = st->el;
            const embi_prop_info *pi = embi_prop_info_of(t.prop);
            ev.name = pi != nullptr ? (embi_str){pi->name, (uint32_t)strlen(pi->name)} : (embi_str){"", 0};
            ev.elapsed = finish ? t.duration : fmax(an->time - t.start - t.delay, 0.0);
            embi_animator_queue(an, &ev);
            break;
        }
    }
    embi_animator_touch_el(an, p->target);
}

double embi_an_trans_time(const embi_an *p)
{
    const embi_transition *t = proxy_transition(p);
    if (t == nullptr) {
        return NAN;
    }
    return (p->an->time - t->start) * 1000.0;
}

bool embi_an_trans_info(const embi_an *p, double *delay_ms, double *duration_ms)
{
    const embi_transition *t = proxy_transition(p);
    if (t == nullptr) {
        return false;
    }
    *delay_ms = t->delay * 1000.0;
    *duration_ms = t->duration * 1000.0;
    return true;
}

/* ------------------------------------------------------------------ queries */

static int cmp_order(const void *x, const void *y)
{
    const embi_an *a = *(embi_an *const *)x, *b = *(embi_an *const *)y;
    int ra = a->kind == EMBI_AN_CSST ? 0 : comp_rank(a);
    int rb = b->kind == EMBI_AN_CSST ? 0 : comp_rank(b);
    if (ra != rb) {
        return ra < rb ? -1 : 1;
    }
    if (a->seq != b->seq) {
        return a->seq < b->seq ? -1 : 1;
    }
    return 0;
}

static bool under(const embi_element *el, const embi_element *root)
{
    for (const embi_node *n = &el->base; n != nullptr; n = n->parent) {
        if (n == &root->base) {
            return true;
        }
    }
    return false;
}

static void list_push(embi_an_list *out, embi_an *a)
{
    if (out->n == out->cap) {
        uint32_t nc = out->cap == 0 ? 8u : out->cap * 2u;
        out->v = embi_realloc(out->v, sizeof(embi_an *) * out->cap, sizeof(embi_an *) * nc);
        out->cap = nc;
    }
    out->v[out->n++] = a;
}

void embi_animator_collect(embi_animator *an, embi_element *el, bool subtree, embi_an_list *out)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&an->states);
    uint64_t k = 0, v = 0;
    uint32_t start = out->n;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        el_state *st = (el_state *)(uintptr_t)v;
        if (el != nullptr && st->el != el && !(subtree && under(st->el, el))) {
            continue;
        }
        for (uint32_t i = 0; i < st->anims.len; i++) {
            embi_an *a = st->anims.data[i];
            if (embi_an_relevant(a)) {
                list_push(out, a);
            }
        }
        for (uint32_t i = 0; i < st->trans.n; i++) {
            list_push(out, proxy_for(an, st, st->trans.v[i].prop));
        }
    }
    if (out->n > start + 1) {
        qsort(out->v + start, out->n - start, sizeof(embi_an *), cmp_order);
    }
}

void embi_an_list_free(embi_an_list *l)
{
    embi_free(l->v, sizeof(embi_an *) * l->cap);
    memset(l, 0, sizeof *l);
}

/* ------------------------------------------------------------------ timing functions */

static uint32_t easing_hash_of(embi_str t)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < t.len; i++) {
        h = (h ^ (uint8_t)t.ptr[i]) * 16777619u;
    }
    return h;
}

/* The easing memo holds at most EMBI_EASING_MEMO_MAX texts (hash lookup). Past that a text is parsed into `own`, an arena
 * of the animation that keeps the result (own == nullptr: a throwaway arena, only the validity is returned). */
bool embi_animator_easing_parse(embi_animator *an, embi_str text, embi_arena *own, const embi_easing **out)
{
    uint32_t h = easing_hash_of(text);
    uint32_t slot = h & (EMBI_EASING_HASH - 1u);
    while (an->easing_hash[slot] != 0) {
        uint32_t i = (uint32_t)an->easing_hash[slot] - 1u;
        if (an->easings[i].text.len == text.len && memcmp(an->easings[i].text.ptr, text.ptr, text.len) == 0) {
            if (out != nullptr) {
                *out = an->easings[i].easing;
            }
            return an->easings[i].ok;
        }
        slot = (slot + 1u) & (EMBI_EASING_HASH - 1u);
    }
    bool memo = an->neasings < EMBI_EASING_MEMO_MAX;
    embi_arena tmp;
    embi_arena *ar = memo ? &an->kfar : own;
    if (ar == nullptr) {
        embi_arena_init(&tmp, 512);
        ar = &tmp;
    }
    embi_css_slice sl = embi_css_parse_cvs(ar, text);
    embi_longhand lh[EMBI_LONGHAND_MAX];
    uint32_t n = embi_css_parse_decl(ar, EMBI_PROP_transition_timing_function, sl, lh, EMBI_LONGHAND_MAX);
    const embi_easing *e = nullptr;
    bool ok = false;
    if (n > 0) {
        uint32_t cnt = 0;
        const embi_value *items = embi_value_items(&lh[0].value, &cnt);
        ok = cnt == 1 && embi_easing_of_value(&items[0], &e);
    }
    if (memo) {
        if (an->neasings == an->capeasings) {
            uint32_t nc = an->capeasings == 0 ? 16u : an->capeasings * 2u;
            an->easings = embi_realloc(an->easings, sizeof(an->easings[0]) * an->capeasings, sizeof(an->easings[0]) * nc);
            an->capeasings = nc;
        }
        char *copy = embi_arena_alloc(&an->kfar, text.len + 1, 1);
        memcpy(copy, text.ptr, text.len);
        copy[text.len] = '\0';
        an->easings[an->neasings].text = (embi_str){copy, text.len};
        an->easings[an->neasings].easing = e;
        an->easings[an->neasings].ok = ok;
        an->neasings++;
        an->easing_hash[slot] = (uint16_t)an->neasings; /* slot is the empty one the probe ended on */
    }
    if (ar == &tmp) {
        e = nullptr; /* lived in the throwaway arena */
    }
    if (out != nullptr) {
        *out = e;
    }
    if (ar == &tmp) {
        embi_arena_free(&tmp);
    }
    return ok;
}

const embi_easing *embi_animator_easing(embi_animator *an, embi_str text, embi_arena *own)
{
    const embi_easing *e = nullptr;
    (void)embi_animator_easing_parse(an, text, own, &e);
    return e;
}
