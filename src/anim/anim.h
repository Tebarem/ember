#pragma once
/*
 * Animation engine (W6a). Internal to Ember: the style engine, the frame loop (W6c) and the Web
 * Animations bindings use these types. Everything here works on computed values (embi_value) and
 * on time in seconds. There is no DOM and no JS in this module.
 *
 * Pieces:
 *   - Animatability and interpolation: embi_anim_animatable, embi_anim_mix (discrete, numbers,
 *     lengths and calc() mixes, colors in premultiplied Oklab, transform lists, shadows, filters,
 *     basic shapes, positions, lists) and embi_anim_add_scaled (the additive and accumulate
 *     composition of Web Animations).
 *   - Timing: embi_easing_eval (cubic-bezier, steps, linear()) and embi_anim_timing_sample (the
 *     Web Animations timing model: phases, iterations, direction, fill).
 *   - Keyframes: embi_kf_build (from a flat list of keyframes, used for Web Animations and for the
 *     CSS @keyframes rules), embi_kf_table_collect (@keyframes of a sheet list, with cascade-layer
 *     aware name resolution) and embi_kf_sample.
 *   - Transitions: embi_trans_update (the CSS Transitions start algorithm as a function of the
 *     changes and the running transitions), embi_trans_advance (run, start and end events) and
 *     embi_trans_value.
 *   - Effect sampling: embi_anim_sample_effects, which overlays the animated values of the CSS and
 *     Web animations (with their composite operations) and then the transitions, skipping the
 *     animations of properties that have an !important author declaration.
 *
 * Memory. Values are allocated in the arena the caller passes. Tables and running lists are plain
 * heap vectors (embi_vec); the caller frees them with the matching *_free function.
 */
#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "style/style.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ animatability */

/* True when the property has an animation type other than none (props.txt anim column, not N). */
bool embi_anim_animatable(embi_prop_id prop);
/* The interpolation kind of a property (props.txt interp column). EMBI_IP_NONE for unknown ids. */
embi_interp embi_anim_interp_of(embi_prop_id prop);

/* Mixes two computed values of prop at progress p. p = 0 gives from and p = 1 gives to exactly.
 * Other values of p mix (extrapolate when p is outside 0..1) where the property interpolates.
 * Where it does not (discrete), the result is from for p < 0.5 and to for p >= 0.5. Visibility
 * is visible for 0 < p < 1 when either end is visible. The result is arena allocated, or
 * shares the arena pointers of its inputs. from and to must be non-null and of the property's
 * type. Range clamping (opacity 0..1, non-negative lengths) is not done here: an overshooting
 * easing gives out-of-range values, and the style engine clamps them when it uses them. Colors
 * are the exception: their sRGB result is clamped into the gamut. A currentcolor value does not
 * mix (it is discrete): resolve it with embi_anim_resolve_color first. */
void embi_anim_mix(embi_arena *a, embi_prop_id prop, const embi_value *from, const embi_value *to,
                   double p, embi_value *out);
/* True when embi_anim_mix of the two values interpolates (is not discrete). The transition start
 * algorithm uses it: a pair that cannot interpolate does not transition. */
bool embi_anim_can_mix(embi_arena *a, embi_prop_id prop, const embi_value *from, const embi_value *to);
/* out is v with each currentcolor replaced by cur (colors, shadow and drop-shadow colors). With
 * cur nullptr, out is a copy of v. */
void embi_anim_resolve_color(embi_arena *a, const embi_value *v, const embi_color *cur,
                             embi_value *out);

/* The additive composition of Web Animations: out = base + k * v for numbers, percentages,
 * lengths and calc() mixes, colors (component-wise in sRGB), shadow lists (pairwise, padded with
 * transparent zero shadows), basic shapes of one kind, and transform and filter lists (by
 * concatenation, where k must be 1). Returns false when the values do not compose (the caller
 * then replaces the value). Used with k = iteration for the iteration composite. */
bool embi_anim_add_scaled(embi_arena *a, embi_prop_id prop, const embi_value *base,
                          const embi_value *v, double k, embi_value *out);

/* ------------------------------------------------------------------ timing */

typedef enum embi_anim_direction {
    EMBI_AD_NORMAL = 0,
    EMBI_AD_REVERSE,
    EMBI_AD_ALTERNATE,
    EMBI_AD_ALTERNATE_REVERSE,
} embi_anim_direction;

#define EMBI_FILL_BACKWARDS (1u << 0)
#define EMBI_FILL_FORWARDS (1u << 1)
#define EMBI_FILL_BOTH (EMBI_FILL_BACKWARDS | EMBI_FILL_FORWARDS)

typedef struct embi_anim_timing {
    double delay;              /* start delay, seconds (may be negative) */
    double end_delay;          /* end delay, seconds (does not change the phases) */
    double duration;           /* iteration duration, seconds, >= 0 (+inf allowed) */
    double iterations;         /* iteration count, >= 0 (+inf allowed) */
    double iteration_start;    /* >= 0: the fractional iteration the effect starts at */
    double rate;               /* playback rate: effect time = timeline time * rate (effect use) */
    const embi_easing *easing; /* the timing function; nullptr is linear */
    uint8_t direction;         /* embi_anim_direction */
    uint8_t fill;              /* EMBI_FILL_* */
} embi_anim_timing;

typedef enum embi_anim_phase {
    EMBI_PH_BEFORE = 0,
    EMBI_PH_ACTIVE,
    EMBI_PH_AFTER,
} embi_anim_phase;

typedef struct embi_anim_sample {
    uint8_t phase;     /* embi_anim_phase */
    bool in_effect;    /* the effect applies at this time (active, or the fill covers it) */
    double iteration;  /* the current iteration index, from 0 (the last one after the end) */
    double progress;   /* the directed iteration progress after the timing function. The
                        * effect uses it only when in_effect is true. */
} embi_anim_sample;

/* The timing function of a unit input x in 0..1 (cubic-bezier, steps with all jump terms, or
 * linear() with stops). A nullptr easing is linear. Input outside 0..1 clamps for cubic-bezier,
 * steps and linear() (the output of the first and last stops); no CSS Easing extrapolation. */
double embi_easing_eval(const embi_easing *e, double x);
/* The same with the CSS Easing "before flag" (steps() at a jump gives the value before the jump). */
double embi_easing_eval_flag(const embi_easing *e, double x, bool before);
/* The timing function a parsed value stands for: an easing value, or one of the keywords linear, ease, ease-in, ease-out,
 * ease-in-out, step-start and step-end (the parser keeps those as keywords). Returns false for any other value (out is then
 * nullptr); linear gives true and nullptr; a null v is the initial value, ease. */
bool embi_easing_of_value(const embi_value *v, const embi_easing **out);

/* The default timing: no delays, one iteration of zero duration, normal direction, rate 1, no
 * fill, linear. */
embi_anim_timing embi_anim_timing_default(void);

/* Samples the Web Animations timing model at local time (seconds, in the effect's time frame,
 * not scaled by the rate). */
void embi_anim_timing_sample(const embi_anim_timing *t, double local, embi_anim_sample *out);

/* ------------------------------------------------------------------ keyframes */

typedef struct embi_kf_set embi_kf_set;

/* One keyframe entry: a property at an offset (0..1), with the value. A value of kind
 * EMBI_V_NONE stands for the underlying value (the implicit 0% and 100% keyframes, or a keyframe
 * that names the property without a value). easing is the timing function from this keyframe to
 * the next one (nullptr: linear). offset NaN means unspecified (Web Animations spacing). */
typedef struct embi_kf_input {
    double offset;
    embi_prop_id prop;
    embi_value value;
    const embi_easing *easing;
    /* The keyframe the entry belongs to: entries with the same key are one keyframe (its
     * properties). A keyframe with no explicit offset is spaced with the others. */
    uint32_t key;
    /* 0: the composite of the effect; else 1 + embi_composite (a keyframe's own composite, Web Animations) */
    uint8_t comp;
} embi_kf_input;

/* Builds a keyframe set from entries in any order, sorted by offset per property (stable). Offsets
 * are computed over the whole list first: a keyframe (a key) takes its first explicit offset, the
 * unspecified ones are spaced evenly between the known ones, the first unspecified one is 0 and the
 * last is 1 (Web Animations). The implicit 0% and 100% keyframes are added as underlying-value
 * entries where a property has none. The result lives in a. Returns a set with no tracks for
 * n == 0. */
embi_kf_set *embi_kf_build(embi_arena *a, const embi_kf_input *in, uint32_t n);
/* True when the set has a track for prop. */
bool embi_kf_has_prop(const embi_kf_set *s, embi_prop_id prop);
/* The number of animated properties of the set, and the i-th one (in track order). */
uint32_t embi_kf_prop_count(const embi_kf_set *s);
embi_prop_id embi_kf_prop_at(const embi_kf_set *s, uint32_t i);
/* The value of prop at progress t in 0..1. current (nullptr: none) resolves currentcolor in the
 * keyframe values and in base before they mix. t is the progress the keyframes are sampled at: the
 * iteration progress after the effect's timing function (Web Animations), or the directed
 * progress itself when the effect has no timing function (CSS animations, whose timing function is
 * per keyframe). Between two keyframes the easing of the earlier one applies, or def when that
 * keyframe has none (the CSS animation-timing-function, or linear for Web Animations). base is the
 * underlying value for implicit keyframes (may be nullptr). Returns false when the set has no
 * track for prop, or no value can be resolved. */
bool embi_kf_sample(const embi_kf_set *s, embi_prop_id prop, double t, const embi_value *base,
                    const embi_easing *def, const embi_color *current, embi_arena *a,
                    embi_value *out);

/* embi_kf_sample with the composite operation of the effect (embi_composite): a keyframe value composited with add or
 * accumulate (its own, or the effect's) is added to the underlying value base before the keyframes interpolate. */
bool embi_kf_sample_c(const embi_kf_set *s, embi_prop_id prop, double t, const embi_value *base,
                      const embi_easing *def, const embi_color *current, embi_arena *a, uint8_t ecomp,
                      embi_value *out);

/* The @keyframes rules of a sheet list, by name. A name resolves to the rule that wins the
 * cascade: a rule in a later layer (unlayered rules are last), then the later rule in source
 * order. */
typedef struct embi_kf_named {
    embi_atom name;
    const embi_kf_set *set;
    uint32_t rank;  /* layer rank: the larger wins; unlayered rules have UINT32_MAX */
    uint64_t seq;   /* source order */
    const embi_css_rule *rule; /* the winning raw @keyframes rule (for W6c to re-walk) */
} embi_kf_named;
typedef struct embi_kf_table {
    embi_kf_named *e;
    uint32_t n, cap;
} embi_kf_table;

/* Collects the @keyframes of the sheets (in cascade order) that match env. The sets live in a.
 * The table is heap allocated; free it with embi_kf_table_free. */
void embi_kf_table_collect(embi_arena *a, embi_kf_table *t, embi_css_sheet *const *sheets,
                           uint32_t n, const embi_style_env *env);
/* The winning set for a name, or nullptr. */
const embi_kf_set *embi_kf_table_find(const embi_kf_table *t, embi_atom name);
/* The raw winning @keyframes rule for a name, or nullptr. */
const embi_css_rule *embi_kf_table_find_rule(const embi_kf_table *t, embi_atom name);
/* How many declarations of a collected rule were left out of the set because they contain var()
 * or a CSS-wide keyword (initial, inherit, unset, revert) and need the element's cascade. 0 for
 * sets built by embi_kf_build. When non-zero W6c must re-walk the rule from
 * embi_kf_table_find_rule and resolve those declarations itself. */
uint32_t embi_kf_dropped_count(const embi_kf_set *s);
void embi_kf_table_free(embi_kf_table *t);
/* The entries of a raw @keyframes rule (embi_kf_table_find_rule) with their specified values, as embi_kf_build takes them:
 * the animation layer (W6c) computes the values against the element, then builds the set. Declarations with var() are
 * left out; CSS-wide keywords (initial, inherit, unset, revert, revert-layer) are kept as values of that kind. The array
 * lives in a. Returns the number of entries. */
uint32_t embi_kf_rule_inputs(embi_arena *a, const embi_css_rule *r, embi_kf_input **out);

/* ------------------------------------------------------------------ transitions */

/* One transition-property entry of the after-change style, with its timing. all: the entry is
 * `all` (every animatable longhand; prop is ignored). allow_discrete: transition-behavior. */
typedef struct embi_trans_spec {
    embi_prop_id prop;
    bool all;
    bool allow_discrete;
    double duration;   /* seconds */
    double delay;      /* seconds */
    const embi_easing *easing;
} embi_trans_spec;

/* A property whose computed value changed in the restyle. before is the value before the change
 * (the running transition's current value is used instead when there is one). */
typedef struct embi_trans_change {
    embi_prop_id prop;
    const embi_value *before;
    const embi_value *after;
} embi_trans_change;

typedef struct embi_trans_input {
    const embi_trans_spec *specs; /* the transition-property list, in order; none: nspecs 0 */
    uint32_t nspecs;
    const embi_trans_change *changes;
    uint32_t nchanges;
    bool display_none;            /* the BEFORE-change style has display: none (the element had no box):
                                   * every transition is cancelled and none starts. A change from
                                   * display: block to none with allow-discrete leaves it false. */
    const embi_color *current;    /* the element's color, resolving currentcolor (nullptr: none) */
} embi_trans_input;

typedef enum embi_trans_event_kind {
    EMBI_TEV_RUN = 0,  /* transitionrun: the transition was created */
    EMBI_TEV_START,    /* transitionstart: the delay ended */
    EMBI_TEV_END,      /* transitionend */
    EMBI_TEV_CANCEL,   /* transitioncancel */
} embi_trans_event_kind;

typedef struct embi_trans_event {
    uint8_t kind;      /* embi_trans_event_kind */
    embi_prop_id prop;
    double elapsed;    /* the elapsedTime of the event: 0 for run and start, the duration for end,
                        * and the time past the delay for cancel */
} embi_trans_event;
typedef struct embi_trans_events {
    embi_trans_event *e;
    uint32_t n, cap;
} embi_trans_events;

typedef struct embi_transition {
    embi_prop_id prop;
    bool started;      /* the delay ended and transitionstart was sent */
    embi_value from, to;
    /* The value that a change back to must reverse (the before-change value of a new transition,
     * the end value of the transition it reversed), and the factor the duration was shortened by
     * (1 for a transition that did not reverse). */
    embi_value reversing_start;
    double reversing_factor;
    double start;      /* timeline time the transition was created */
    double delay;
    double duration;
    const embi_easing *easing;
} embi_transition;

typedef struct embi_transitions {
    embi_transition *v;
    uint32_t n, cap;
} embi_transitions;

/* Applies the changes of one restyle at time now (seconds). Running transitions that no longer
 * match, that became non-animatable, or that are discrete without allow-discrete are cancelled.
 * A change starts a transition when a matching entry has a positive combined duration (duration
 * + delay) and its values interpolate (visibility always does; other discrete properties need
 * allow-discrete). A change whose after value equals the running transition's end value leaves it
 * alone, even when the combined duration is zero. A change to the value the property already has
 * at now cancels it. A
 * change back to the running transition's reversing-adjusted start value reverses it: the duration
 * and a negative delay are scaled by the reversing shortening factor. Any other change replaces it.
 * Events go to ev. Allocations go to a. */
void embi_trans_update(embi_arena *a, embi_transitions *tr, const embi_trans_input *in, double now,
                       embi_trans_events *ev);
/* Sends the start and end events of transitions whose times have come, and removes the ended
 * ones. Call after embi_trans_update with the same now. */
void embi_trans_advance(embi_transitions *tr, double now, embi_trans_events *ev);
/* The current value of a running transition at now. */
void embi_trans_value(embi_arena *a, const embi_transition *t, double now, embi_value *out);
void embi_transitions_free(embi_transitions *tr);
void embi_trans_events_free(embi_trans_events *ev);

/* ------------------------------------------------------------------ effect sampling */

typedef enum embi_composite {
    EMBI_COMP_REPLACE = 0,
    EMBI_COMP_ADD,
    EMBI_COMP_ACCUMULATE,
} embi_composite;

/* One animation of an element: a CSS animation or a Web Animation. The current time is
 * (now - start) * timing.rate, or hold while the animation is held (paused, or pending its start).
 * kf may be nullptr, which has no effect. */
typedef struct embi_anim_effect {
    const embi_kf_set *kf;
    embi_anim_timing timing;      /* timing.easing applies to the whole iteration (Web Animations) */
    const embi_easing *keyframe_easing; /* CSS: the animation-timing-function, used between
                                         * keyframes that have no timing function of their own */
    double start;
    double hold;
    bool held;
    uint8_t composite;           /* embi_composite: replace takes the value; add and accumulate add
                                  * it to the underlying value */
    uint8_t iteration_composite; /* embi_composite, REPLACE or ACCUMULATE: accumulate adds the
                                  * iteration index times the keyframe span (iterationComposite) */
} embi_anim_effect;

typedef struct embi_anim_env {
    void *user;
    /* The underlying computed value of a property (no animations), or nullptr. */
    const embi_value *(*base)(void *user, embi_prop_id prop);
    /* True when the element has an !important author declaration for the property. Animations
     * of such properties are skipped. Transitions still apply. */
    bool (*important)(void *user, embi_prop_id prop);
    /* The element's color, which resolves currentcolor before values mix (nullptr: none). */
    const embi_color *current;
} embi_anim_env;

typedef struct embi_overlay_entry {
    embi_prop_id prop;
    embi_value value;
} embi_overlay_entry;
typedef struct embi_overlay {
    embi_overlay_entry *e;
    uint32_t n, cap;
} embi_overlay;

/* Overlays the animated values at now on the base computed values: the animations in list order
 * (each composited on the value below it), then the running transitions, which override the
 * animations. out is cleared first. Its entries are arena values or pointers into the inputs. */
void embi_anim_sample_effects(embi_arena *a, const embi_anim_env *env,
                              const embi_anim_effect *effects, uint32_t n,
                              const embi_transitions *tr, double now, embi_overlay *out);
/* The animated value of prop in the overlay, or nullptr. */
const embi_value *embi_overlay_find(const embi_overlay *o, embi_prop_id prop);
void embi_overlay_free(embi_overlay *o);
