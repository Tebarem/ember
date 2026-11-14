#pragma once
/*
 * The animator (W6c): the animations of a document in the frame. It sits on the engine of anim.h and on the style hooks
 * (style.h embi_style_anim_hooks): it creates the CSS animations and transitions from the computed styles, holds the Web
 * Animations (Element.animate), runs the timing and play state machine of the Web Animations spec, and overlays the
 * sampled values on the base computed style of each element. It has no JS and no DOM mutation; the bindings (impl_anim.c)
 * and the frame step (src/embed/frame.c) drive it.
 *
 * Time. The timeline is the document timeline: its time is the time of the frame in milliseconds since the document's time
 * origin (what performance.now() reads at the frame start). The embedder calls embi_animator_frame once per frame with it.
 * Between frames the time stands still, so an animation created by a script is pending until the next frame, where it
 * becomes ready with that frame's time as its start time. Inside a frame, embi_animator_frame_end (after the style update)
 * readies the animations that the scripts and the style update created during it.
 *
 * An animation (embi_an) is one of three kinds. Web animations and CSS animations (kind CSSA) have the full play state
 * machine; a CSS transition (kind CSST) is a read-only proxy of a running transition of the engine (anim.h
 * embi_transitions), with cancel and finish.
 *
 * Notifications. The host (the bindings) learns about the promises and events of an animation through the notify hook;
 * the C side keeps the state, the host keeps the promises. Events are queued and sent by embi_animator_frame.
 */
#include "anim/anim.h"

#include <math.h>

typedef struct embi_animator embi_animator;
typedef struct embi_an embi_an;

enum { EMBI_AN_WEB = 0, EMBI_AN_CSSA, EMBI_AN_CSST };
enum { EMBI_PLAY_IDLE = 0, EMBI_PLAY_RUNNING, EMBI_PLAY_PAUSED, EMBI_PLAY_FINISHED };
enum { EMBI_REPLACE_ACTIVE = 0, EMBI_REPLACE_REMOVED, EMBI_REPLACE_PERSISTED };

/* What notify reports (see embi_animator_host). */
enum {
    EMBI_ANN_READY_NEW = 0, /* the current ready promise is replaced by a new pending one */
    EMBI_ANN_READY,         /* the current ready promise resolves */
    EMBI_ANN_FINISHED_NEW,  /* the current finished promise is replaced by a new pending one */
    EMBI_ANN_FINISHED,      /* the current finished promise resolves */
    EMBI_ANN_CANCELED,      /* cancel(): the pending ready promise rejects with AbortError and becomes resolved; the pending
                             * finished promise rejects with AbortError and is replaced by a new pending one */
    EMBI_ANN_EV_FINISH,     /* dispatch the "finish" event (a queued task: sent by the frame) */
    EMBI_ANN_EV_CANCEL,     /* dispatch the "cancel" event */
    EMBI_ANN_EV_REMOVE,     /* dispatch the "remove" event */
    EMBI_ANN_RELEASE,       /* the animation is no longer relevant: the host may drop a strong reference it holds */
    EMBI_ANN_RETAIN,        /* the animation is relevant (again): the host keeps a strong reference */
    EMBI_ANN_DESTROYED,     /* the record is going away (the element or the document went): drop the wrapper */
};

enum {
    EMBI_CEV_ANIMATIONSTART = 0,
    EMBI_CEV_ANIMATIONITERATION,
    EMBI_CEV_ANIMATIONEND,
    EMBI_CEV_ANIMATIONCANCEL,
    EMBI_CEV_TRANSITIONRUN,
    EMBI_CEV_TRANSITIONSTART,
    EMBI_CEV_TRANSITIONEND,
    EMBI_CEV_TRANSITIONCANCEL,
};

typedef struct embi_animator_host {
    void *user;
    /* A restyle is needed (an animation moved or was created): the document marks its style and layout dirty. */
    void (*invalidate)(void *user);
    /* A promise or event of an animation (EMBI_ANN_*). time_ms is the timeline time of the event. */
    void (*notify)(void *user, embi_an *a, int what, double time_ms);
    /* A CSS animation or transition event (EMBI_CEV_*) for el: name is the animation name or the property name,
     * elapsed in seconds. Sent by embi_animator_frame. */
    void (*css_event)(void *user, embi_element *el, int kind, embi_str name, double elapsed);
    /* A safe point (the update loops are done, nothing is mid-update): the host runs the microtask checkpoint, when no
     * script is running. The promise reactions of the notifications above run here, never inside the animator's loops or
     * in the middle of a native call of a script. May be null. */
    void (*checkpoint)(void *user);
} embi_animator_host;

/* ------------------------------------------------------------------ lifetime and wiring */

/* Creates the animator of a document and installs its style hooks in the engine. */
embi_animator *embi_animator_new(embi_style_engine *se, const embi_animator_host *host);
/* Removes the style hooks and frees everything. The animations' records are destroyed (EMBI_ANN_DESTROYED). */
void embi_animator_free(embi_animator *an);
/* A new document: drops every animation and transition (without events) and the keyframes table. */
void embi_animator_reset(embi_animator *an);
/* The author sheets of the document (borrowed: they must outlive their use) and the media environment, for the @keyframes
 * table. Call it whenever the sheets or the environment change. */
void embi_animator_set_sheets(embi_animator *an, embi_css_sheet *const *sheets, uint32_t n, const embi_style_env *env);
/* The media environment changed (the @keyframes table follows it). */
void embi_animator_set_env(embi_animator *an, const embi_style_env *env);

/* ------------------------------------------------------------------ the frame */

/* Brings the timeline to now_ms (milliseconds since the time origin): readies the pending animations, updates the finished
 * states, sends the queued animation events, and marks for restyle every element whose animated values move. */
void embi_animator_frame(embi_animator *an, double now_ms);
/* After the frame's style update: readies the animations created since embi_animator_frame, with the frame's time. */
void embi_animator_frame_end(embi_animator *an);
/* The time of the last frame, ms. */
double embi_animator_time(const embi_animator *an);
/* True while something needs another frame: a running animation or transition that can still change a value, a pending
 * play or pause, or a queued event. An idle page returns false. */
bool embi_animator_wants_frame(const embi_animator *an);
/* CF2: true once a resolved style declared a CSS animation (animation-name) or a transition with a duration or delay: the
 * document may send CSS animation and transition events. Never goes back to false. */
bool embi_animator_declared(const embi_animator *an);
/* The number of elements the last frame marked for restyle (tests). */
uint32_t embi_animator_marked(const embi_animator *an);

/* ------------------------------------------------------------------ animations */

/* Creates a Web animation with no effect, held by the host (it gets notifications). */
embi_an *embi_an_new(embi_animator *an);
/* The record with this id, or nullptr (it went away). */
embi_an *embi_an_by_id(embi_animator *an, uint32_t id);
uint32_t embi_an_id(const embi_an *a);
/* The host takes (true) or gives up (false) a wrapper on a record: CSS animations are made without one, and adopted when a
 * script asks for them. Notifications go only to records the host holds. */
void embi_an_set_hosted(embi_an *a, bool hosted);
bool embi_an_hosted(const embi_an *a);
/* The host's wrapper was collected: the record is destroyed at the next frame, unless the animation still has a visible
 * effect on an element in the document, in which case it runs on without a wrapper. */
void embi_an_wrapper_lost(embi_animator *an, uint32_t id);
/* A timing function from its CSS text (cubic-bezier(), steps(), linear(), ease ...): nullptr when invalid. The result
 * lives as long as the animator, or in `own` once the memo (256 texts) is full. */
const embi_easing *embi_animator_easing(embi_animator *an, embi_str text, embi_arena *own);
/* As above, with the validity: false when text is not a timing function (out is then nullptr). "linear" is valid and gives
 * nullptr as well. */
bool embi_animator_easing_parse(embi_animator *an, embi_str text, embi_arena *own, const embi_easing **out);
/* The arena for a timing easing of this animation (emptied by the call: it keeps only the latest timing's easing). */
embi_arena *embi_an_easing_arena(embi_an *a);
uint8_t embi_an_kind(const embi_an *a);
embi_animator *embi_an_animator(const embi_an *a);
/* The CSS animation name, or the transition property name, for the CSS kinds ("" otherwise). */
embi_str embi_an_css_name(const embi_an *a);
/* The id for sorting by creation (composite order inside one kind). */
uint64_t embi_an_seq(const embi_an *a);

/* The effect. A Web animation's effect is a keyframe effect: a target element (may be nullptr), keyframes (specified
 * values, copied into the animation's arena) and the timing. */
typedef struct embi_an_effect_desc {
    embi_element *target;
    const embi_kf_input *kf;  /* entries as embi_kf_build takes them; values are specified (parsed) values */
    uint32_t nkf;
    embi_arena *arena;        /* the arena that holds kf and its values: the animation takes its contents (the struct is
                               * left empty); nullptr when there are no keyframes */
    embi_anim_timing timing;  /* rate is ignored */
    uint8_t composite;        /* embi_composite */
    uint8_t iteration_composite;
    bool has_effect;          /* false: the animation has no effect (new Animation()) */
} embi_an_effect_desc;
/* Sets or replaces the whole effect. The keyframes are not copied: the animation takes the arena. */
void embi_an_set_effect(embi_an *a, const embi_an_effect_desc *d);
/* Replaces only the timing (updateTiming). */
void embi_an_set_timing(embi_an *a, const embi_anim_timing *t);
void embi_an_set_target(embi_an *a, embi_element *el);
void embi_an_set_composite(embi_an *a, uint8_t composite, uint8_t iteration_composite);
/* Replaces the keyframes (setKeyframes). */
void embi_an_set_keyframes(embi_an *a, embi_arena *arena, const embi_kf_input *kf, uint32_t n);
embi_element *embi_an_target(const embi_an *a);
const embi_anim_timing *embi_an_timing(const embi_an *a);
bool embi_an_has_effect(const embi_an *a);
/* Detaches the effect from the animation (animation.effect = null) or reattaches. */
void embi_an_clear_effect(embi_an *a);
/* The resolved keyframes of the animation as property/offset/value text, for getKeyframes: the number of entries and the
 * i-th one's offset, property id, serialized value (in sb) and easing (nullptr for linear). */
uint32_t embi_an_keyframe_count(const embi_an *a);
bool embi_an_keyframe_at(const embi_an *a, uint32_t i, double *offset, embi_prop_id *prop, embi_sb *value,
                         const embi_easing **easing);

/* Timeline association: false for an animation without a timeline (new Animation(effect, null)). */
void embi_an_set_timeline(embi_an *a, bool has_timeline);
bool embi_an_has_timeline(const embi_an *a);

/* Times are in milliseconds in this group (the API units); NaN is the unresolved time value. */
double embi_an_current_time(const embi_an *a);
double embi_an_start_time(const embi_an *a);
double embi_an_hold_time(const embi_an *a);
double embi_an_playback_rate(const embi_an *a);
double embi_an_pending_rate(const embi_an *a); /* NaN: none */
int embi_an_play_state(const embi_an *a);      /* EMBI_PLAY_* */
bool embi_an_pending(const embi_an *a);
bool embi_an_ready_resolved(const embi_an *a);
bool embi_an_finished_resolved(const embi_an *a);
int embi_an_replace_state(const embi_an *a);
/* True when the animation is relevant (current or in effect). */
bool embi_an_relevant(const embi_an *a);

/* The operations; false with *err set to a DOMException name ("InvalidStateError", "TypeError") when it throws. */
bool embi_an_play(embi_an *a, const char **err);
bool embi_an_pause(embi_an *a, const char **err);
bool embi_an_reverse(embi_an *a, const char **err);
bool embi_an_finish(embi_an *a, const char **err);
void embi_an_cancel(embi_an *a);
bool embi_an_set_current_time(embi_an *a, double ms, const char **err);
bool embi_an_set_start_time(embi_an *a, double ms, const char **err);
void embi_an_set_playback_rate(embi_an *a, double rate);
bool embi_an_update_playback_rate(embi_an *a, double rate, const char **err);
void embi_an_persist(embi_an *a);

/* getComputedTiming: the values of the effect at the animation's current time (ms where times). */
typedef struct embi_an_computed {
    double end_time, active_duration, local_time;
    double progress, current_iteration; /* NaN when the effect is not in effect */
    int phase;                          /* EMBI_PH_*, or -1 (idle: local time unresolved) */
} embi_an_computed;
void embi_an_computed_timing(const embi_an *a, embi_an_computed *out);

/* ------------------------------------------------------------------ queries */

/* The relevant animations of el (and, with subtree, of its descendants), in composite order, appended to out (a plain
 * vector grown with embi_realloc; free it with embi_free(out->v, sizeof(embi_an *) * out->cap)). With el nullptr, those of
 * the whole document. */
typedef struct embi_an_list {
    embi_an **v;
    uint32_t n, cap;
} embi_an_list;
void embi_animator_collect(embi_animator *an, embi_element *el, bool subtree, embi_an_list *out);
void embi_an_list_free(embi_an_list *l);

/* Style engine helpers for tests and the bindings. */
embi_style_engine *embi_animator_style(const embi_animator *an);
/* Total number of animation records alive (tests, leak checks). */
uint32_t embi_animator_count(const embi_animator *an);

/* The bytes of the animated values: the value blocks of the animator's style engine, its scratch and the transitions'
 * arenas (tests: a steady animation must not grow them without bound). */
size_t embi_animator_arena_bytes(const embi_animator *an);
/* WPTASAN, with embi_arena_debug_poison on (tests): the number of values, or parts of values, in poisoned memory that the
 * animator keeps (in the elements' base styles, their transitions and the parent styles of their last restyle), plus those
 * its restyles have read so far (the same, and the new base of each restyle). */
uint32_t embi_animator_debug_poisoned(const embi_animator *an);
