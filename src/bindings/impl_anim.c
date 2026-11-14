/*
 * Animations in the realm (W6c): requestAnimationFrame, the Web Animations API (Element.animate, getAnimations,
 * document.timeline, the Animation classes) and the CSS animation and transition events.
 *
 * The state machine and the sampling are in C (src/anim/frame.h, the animator of the document). The objects a page sees are
 * written in JavaScript (js/animation.js, embedded here, evaluated once per realm the first time one is used): they hold the
 * promises, the events, the argument conversion (keyframes, options) and call the natives of the host object below with the
 * id of the animation record. The idle cost of a page that never animates is the install of a few accessors.
 *
 * Lifetime. A JS Animation holds a handle (a native object with a finalizer): when it is collected the record is told
 * (embi_an_wrapper_lost) and goes at the next frame, unless it still affects an element (then it runs on without a wrapper).
 * The prelude keeps a strong reference to an Animation between the RETAIN and RELEASE notifications, so one that is playing
 * or has an event to send survives with no reference from the page.
 */
#include "bindings/impl.h"
#include "bindings/impl_loader.h"
#include "bindings/gen/types.h"

#include "anim/frame.h"
#include "css/css.h"
#include "css/props.h"
#include "style/style.h"

#include <math.h>

#ifdef EMBER_LZ_ASSETS
#include "ember_lz/js/animation.js.lz.h"
#define k_animation_js embi_lz_js_animation_js
#else
static const unsigned char k_animation_js[] = {
#embed "js/animation.js"
    , 0
};
#endif

/* ------------------------------------------------------------------ the handle */

typedef struct anim_handle {
    embi_animator *an;
    uint32_t id;
} anim_handle;

static void handle_finalize(embi_js_object *o)
{
    anim_handle *h = o->internal;
    if (h != nullptr) {
        embi_an_wrapper_lost(h->an, h->id);
        embi_free(h, sizeof *h);
        o->internal = nullptr;
    }
}

static const embi_js_class k_handle_class = {
    .name = "AnimationHandle",
    .finalize = handle_finalize,
};

static embi_js_value handle_new(embi_bind_ctx *c, embi_an *a)
{
    anim_handle *h = embi_calloc(1, sizeof *h);
    h->an = c->host.animator;
    h->id = embi_an_id(a);
    embi_js_object *o = embi_js_object_new_class(c->rt, EMBI_JS_NULL, &k_handle_class);
    o->internal = h;
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ helpers */

static double num_arg(embi_bind_ctx *c, int argc, const embi_js_value *argv, int i, double def)
{
    double d = def;
    if (i < argc && embi_js_is_number(argv[i])) {
        (void)embi_js_to_number(c->rt, argv[i], &d);
    }
    return d;
}

/* A double to an unsigned integer in [0, max]: NaN and negatives are 0 (a plain cast of them is undefined). */
static uint32_t clamp_u32(double d, uint32_t max)
{
    if (!(d > 0.0)) {
        return 0;
    }
    return d >= (double)max ? max : (uint32_t)d;
}

static embi_js_value num_or_null(double d)
{
    return d != d ? EMBI_JS_NULL : embi_js_number(d);
}

static embi_an *rec_arg(embi_bind_ctx *c, int argc, const embi_js_value *argv)
{
    if (c->host.animator == nullptr || argc < 1 || !embi_js_is_number(argv[0])) {
        return nullptr;
    }
    return embi_an_by_id(c->host.animator, clamp_u32(num_arg(c, argc, argv, 0, 0), UINT32_MAX));
}

/* The element of a wrapper value, nullptr for null or anything else. */
static embi_element *el_arg(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || bo->ptr == nullptr || !embi_bind_iface_is(bo->iface, EMBI_IF_Element)) {
        return nullptr;
    }
    return bo->ptr;
}

static embi_str sb_str(const embi_sb *sb)
{
    return (embi_str){sb->data != nullptr ? sb->data : "", sb->len};
}

/* The timing array [delay, endDelay, fill, iterationStart, iterations, duration, direction, easing] (times in ms). */
static bool timing_from(embi_bind_ctx *c, embi_js_value arr, embi_anim_timing *t, embi_an *owner)
{
    memset(t, 0, sizeof *t);
    t->iterations = 1.0;
    t->rate = 1.0;
    if (!embi_js_is_object(arr)) {
        return false;
    }
    double v[7];
    for (uint32_t i = 0; i < 7; i++) {
        v[i] = 0.0;
        embi_js_value e = embi_bind_array_get(c, arr, i);
        if (embi_js_is_number(e)) {
            (void)embi_js_to_number(c->rt, e, &v[i]);
        }
    }
    t->delay = v[0] / 1000.0;
    t->end_delay = v[1] / 1000.0;
    t->fill = (uint8_t)(clamp_u32(v[2], 3u) & 3u);
    t->iteration_start = v[3];
    t->iterations = v[4];
    t->duration = v[5] / 1000.0;
    t->direction = (uint8_t)clamp_u32(v[6], 3u);
    embi_sb es = {0};
    embi_js_value ev = embi_bind_array_get(c, arr, 7);
    if (embi_js_is_string(ev) && ldr_to_utf8(c, ev, &es) && es.len > 0) {
        t->easing = embi_animator_easing(c->host.animator, sb_str(&es), owner != nullptr ? embi_an_easing_arena(owner) : nullptr);
    }
    embi_sb_free(&es);
    return true;
}

/* The flat keyframe list [key, offset, easing, property, value]* into an arena and an input array. Returns the count. */
static uint32_t keyframes_from(embi_bind_ctx *c, embi_js_value flat, embi_arena *ar, embi_kf_input **out)
{
    *out = nullptr;
    if (!embi_js_is_object(flat)) {
        return 0;
    }
    uint32_t len = embi_bind_array_len(c, flat);
    uint32_t n = 0, cap = 0;
    embi_kf_input *in = nullptr;
    for (uint32_t i = 0; i + 5 < len; i += 6) {
        double key = 0, offset = 0;
        (void)embi_js_to_number(c->rt, embi_bind_array_get(c, flat, i), &key);
        (void)embi_js_to_number(c->rt, embi_bind_array_get(c, flat, i + 1), &offset);
        embi_sb easing = {0}, name = {0}, value = {0};
        (void)ldr_to_utf8(c, embi_bind_array_get(c, flat, i + 2), &easing);
        (void)ldr_to_utf8(c, embi_bind_array_get(c, flat, i + 3), &name);
        (void)ldr_to_utf8(c, embi_bind_array_get(c, flat, i + 4), &value);
        double kcomp = 0;
        (void)embi_js_to_number(c->rt, embi_bind_array_get(c, flat, i + 5), &kcomp);
        embi_prop_id prop = embi_prop_lookup(sb_str(&name));
        if (prop != EMBI_PROP_NONE && value.len > 0) {
            char *copy = embi_arena_alloc(ar, value.len + 1, 1);
            memcpy(copy, value.data, value.len);
            copy[value.len] = '\0';
            embi_css_slice sl = embi_css_parse_cvs(ar, (embi_str){copy, value.len});
            embi_longhand lh[EMBI_LONGHAND_MAX];
            uint32_t cnt = embi_css_parse_decl(ar, prop, sl, lh, EMBI_LONGHAND_MAX);
            const embi_easing *ease = easing.len > 0 ? embi_animator_easing(c->host.animator, sb_str(&easing), ar) : nullptr;
            for (uint32_t j = 0; j < cnt; j++) {
                if (!embi_anim_animatable(lh[j].prop)) {
                    continue;
                }
                if (n == cap) {
                    uint32_t nc = cap == 0 ? 8u : cap * 2u;
                    in = embi_realloc(in, sizeof *in * cap, sizeof *in * nc);
                    cap = nc;
                }
                in[n++] = (embi_kf_input){.offset = offset,
                                          .prop = lh[j].prop,
                                          .value = lh[j].value,
                                          .easing = ease,
                                          .key = clamp_u32(key, UINT32_MAX),
                                          .comp = (uint8_t)clamp_u32(kcomp, 3u)};
            }
        }
        embi_sb_free(&easing);
        embi_sb_free(&name);
        embi_sb_free(&value);
    }
    if (n > 0) {
        embi_kf_input *res = embi_arena_alloc(ar, sizeof *res * n, 8);
        memcpy(res, in, sizeof *res * n);
        *out = res;
    }
    embi_free(in, sizeof *in * cap);
    return n;
}

/* ------------------------------------------------------------------ the host object of the prelude */

enum {
    N_CREATE,
    N_ADOPT,
    N_SET_EFFECT,
    N_SET_TIMING,
    N_SET_TARGET,
    N_SET_KEYFRAMES,
    N_CLEAR_EFFECT,
    N_GET,
    N_OP,
    N_LIST,
    N_COMPUTED,
    N_TARGET,
    N_TIME,
    N_CSSINFO,
    N_RAF_COUNT,
    N_REPORT,
    N_EASING_OK,
    N_ANIMATABLE,
    N_VALUE_OK,
    N_ID,
};

/* Fields of N_GET. */
enum {
    F_CURRENT_TIME,
    F_START_TIME,
    F_PLAY_STATE,
    F_PENDING,
    F_REPLACE_STATE,
    F_PLAYBACK_RATE,
    F_RELEVANT,
    F_READY_RESOLVED,
    F_FINISHED_RESOLVED,
    F_HAS_EFFECT,
    F_PENDING_RATE,
    F_KIND,
    F_HAS_TIMELINE,
};

/* Operations of N_OP. */
enum {
    O_PLAY,
    O_PAUSE,
    O_REVERSE,
    O_FINISH,
    O_CANCEL,
    O_SET_CURRENT_TIME,
    O_SET_START_TIME,
    O_SET_PLAYBACK_RATE,
    O_UPDATE_PLAYBACK_RATE,
    O_PERSIST,
    O_SET_TIMELINE,
};

static embi_js_value err_value(embi_bind_ctx *c, const char *err)
{
    if (err == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_bind_utf8_value(c, (embi_str){err, (uint32_t)strlen(err)});
}

static embi_js_value host_native(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_animator *an = c != nullptr ? c->host.animator : nullptr;
    if (an == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    switch (magic) {
    case N_CREATE: {
        embi_an *a = embi_an_new(an);
        return handle_new(c, a);
    }
    case N_ADOPT: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr) {
            return EMBI_JS_NULL;
        }
        embi_an_set_hosted(a, true);
        return handle_new(c, a);
    }
    case N_ID: {
        if (argc < 1 || !embi_js_is_object(argv[0]) || embi_js_as_object(argv[0])->klass != &k_handle_class) {
            return EMBI_JS_UNDEFINED;
        }
        anim_handle *h = embi_js_as_object(argv[0])->internal;
        return h != nullptr ? embi_js_number((double)h->id) : EMBI_JS_UNDEFINED;
    }
    case N_SET_EFFECT: {
        /* (id, element|null, flatKeyframes, timing, composite, iterationComposite) */
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr || argc < 4) {
            return EMBI_JS_UNDEFINED;
        }
        embi_an_effect_desc d;
        memset(&d, 0, sizeof d);
        d.target = el_arg(argv[1]);
        embi_arena ar;
        embi_arena_init(&ar, 1024);
        embi_kf_input *kf = nullptr;
        d.nkf = keyframes_from(c, argv[2], &ar, &kf);
        d.kf = kf;
        d.arena = &ar;
        (void)timing_from(c, argv[3], &d.timing, a);
        d.composite = (uint8_t)clamp_u32(num_arg(c, argc, argv, 4, 0), 2u);
        d.iteration_composite = (uint8_t)clamp_u32(num_arg(c, argc, argv, 5, 0), 2u);
        d.has_effect = true;
        embi_an_set_effect(a, &d);
        embi_arena_free(&ar); /* the animation took the contents */
        return EMBI_JS_UNDEFINED;
    }
    case N_SET_TIMING: {
        embi_an *a = rec_arg(c, argc, argv);
        embi_anim_timing t;
        if (a != nullptr && argc > 1 && timing_from(c, argv[1], &t, a)) {
            embi_an_set_timing(a, &t);
        }
        return EMBI_JS_UNDEFINED;
    }
    case N_SET_TARGET: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a != nullptr && argc > 1) {
            embi_an_set_target(a, el_arg(argv[1]));
        }
        return EMBI_JS_UNDEFINED;
    }
    case N_SET_KEYFRAMES: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr || argc < 2) {
            return EMBI_JS_UNDEFINED;
        }
        embi_arena ar;
        embi_arena_init(&ar, 1024);
        embi_kf_input *kf = nullptr;
        uint32_t n = keyframes_from(c, argv[1], &ar, &kf);
        embi_an_set_keyframes(a, &ar, kf, n);
        embi_arena_free(&ar);
        return EMBI_JS_UNDEFINED;
    }
    case N_CLEAR_EFFECT: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a != nullptr) {
            embi_an_clear_effect(a);
        }
        return EMBI_JS_UNDEFINED;
    }
    case N_GET: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr || argc < 2) {
            return EMBI_JS_UNDEFINED;
        }
        switch ((int)clamp_u32(num_arg(c, argc, argv, 1, 0), 255)) {
        case F_CURRENT_TIME:
            return num_or_null(embi_an_current_time(a));
        case F_START_TIME:
            return num_or_null(embi_an_start_time(a));
        case F_PLAY_STATE:
            return embi_js_number(embi_an_play_state(a));
        case F_PENDING:
            return embi_js_from_bool(embi_an_pending(a));
        case F_REPLACE_STATE:
            return embi_js_number(embi_an_replace_state(a));
        case F_PLAYBACK_RATE:
            return embi_js_number(embi_an_playback_rate(a));
        case F_RELEVANT:
            return embi_js_from_bool(embi_an_relevant(a));
        case F_READY_RESOLVED:
            return embi_js_from_bool(embi_an_ready_resolved(a));
        case F_FINISHED_RESOLVED:
            return embi_js_from_bool(embi_an_finished_resolved(a));
        case F_HAS_EFFECT:
            return embi_js_from_bool(embi_an_has_effect(a));
        case F_PENDING_RATE:
            return num_or_null(embi_an_pending_rate(a));
        case F_KIND:
            return embi_js_number(embi_an_kind(a));
        case F_HAS_TIMELINE:
            return embi_js_from_bool(embi_an_has_timeline(a));
        default:
            return EMBI_JS_UNDEFINED;
        }
    }
    case N_OP: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr || argc < 2) {
            return EMBI_JS_UNDEFINED;
        }
        const char *err = nullptr;
        double arg = num_arg(c, argc, argv, 2, NAN);
        switch ((int)clamp_u32(num_arg(c, argc, argv, 1, 0), 255)) {
        case O_PLAY:
            (void)embi_an_play(a, &err);
            break;
        case O_PAUSE:
            (void)embi_an_pause(a, &err);
            break;
        case O_REVERSE:
            (void)embi_an_reverse(a, &err);
            break;
        case O_FINISH:
            (void)embi_an_finish(a, &err);
            break;
        case O_CANCEL:
            embi_an_cancel(a);
            break;
        case O_SET_CURRENT_TIME:
            (void)embi_an_set_current_time(a, arg, &err);
            break;
        case O_SET_START_TIME:
            (void)embi_an_set_start_time(a, arg, &err);
            break;
        case O_SET_PLAYBACK_RATE:
            embi_an_set_playback_rate(a, arg);
            break;
        case O_UPDATE_PLAYBACK_RATE:
            (void)embi_an_update_playback_rate(a, arg, &err);
            break;
        case O_PERSIST:
            embi_an_persist(a);
            break;
        case O_SET_TIMELINE:
            embi_an_set_timeline(a, arg != 0.0);
            break;
        default:
            break;
        }
        return err_value(c, err);
    }
    case N_LIST: {
        /* (element|null, subtree) -> ids */
        embi_an_list l = {0};
        embi_element *el = argc > 0 ? el_arg(argv[0]) : nullptr;
        bool subtree = argc > 1 && embi_js_to_boolean(argv[1]);
        /* getAnimations updates the style first: a CSS animation or transition the last change started is in the list */
        embi_style_engine *se = embi_bind_style(c);
        if (se != nullptr) {
            embi_style_resolve_document(se, el != nullptr ? el->base.doc : embi_bind_document(c));
        }
        embi_animator_collect(an, el, subtree, &l);
        embi_js_value arr = embi_bind_array_new(c, l.n);
        embi_bind_pin(c, arr);
        for (uint32_t i = 0; i < l.n; i++) {
            embi_bind_array_set(c, arr, i, embi_js_number((double)embi_an_id(l.v[i])));
        }
        embi_an_list_free(&l);
        return arr;
    }
    case N_COMPUTED: {
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr) {
            return EMBI_JS_UNDEFINED;
        }
        embi_an_computed cv;
        embi_an_computed_timing(a, &cv);
        embi_js_value arr = embi_bind_array_new(c, 6);
        embi_bind_pin(c, arr);
        embi_bind_array_set(c, arr, 0, embi_js_number(cv.end_time));
        embi_bind_array_set(c, arr, 1, embi_js_number(cv.active_duration));
        embi_bind_array_set(c, arr, 2, num_or_null(cv.local_time));
        embi_bind_array_set(c, arr, 3, num_or_null(cv.progress));
        embi_bind_array_set(c, arr, 4, num_or_null(cv.current_iteration));
        embi_bind_array_set(c, arr, 5, embi_js_number(cv.phase));
        return arr;
    }
    case N_TARGET: {
        embi_an *a = rec_arg(c, argc, argv);
        embi_element *el = a != nullptr ? embi_an_target(a) : nullptr;
        return el != nullptr ? embi_bind_node_value(c, &el->base) : EMBI_JS_NULL;
    }
    case N_TIME:
        return embi_js_number(embi_animator_time(an));
    case N_CSSINFO: {
        /* (id) -> [kind, name] for a CSS animation or transition */
        embi_an *a = rec_arg(c, argc, argv);
        if (a == nullptr) {
            return EMBI_JS_UNDEFINED;
        }
        embi_js_value arr = embi_bind_array_new(c, 2);
        embi_bind_pin(c, arr);
        embi_bind_array_set(c, arr, 0, embi_js_number(embi_an_kind(a)));
        embi_bind_array_set(c, arr, 1, embi_bind_utf8_value(c, embi_an_css_name(a)));
        return arr;
    }
    case N_RAF_COUNT:
        c->raf_count = clamp_u32(num_arg(c, argc, argv, 0, 0), UINT32_MAX);
        return EMBI_JS_UNDEFINED;
    case N_REPORT:
        embi_bind_report_error(c, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, nullptr, 0, 0);
        return EMBI_JS_UNDEFINED;
    case N_EASING_OK: {
        embi_sb es = {0};
        bool ok = argc > 0 && ldr_to_utf8(c, argv[0], &es) && embi_animator_easing_parse(an, sb_str(&es), nullptr, nullptr);
        embi_sb_free(&es);
        return embi_js_from_bool(ok);
    }
    case N_VALUE_OK: {
        /* (css property name, text) -> whether the text is a value of the property (a keyframe drops the others) */
        embi_sb ns = {0}, vs = {0};
        bool ok = true;
        if (argc > 1 && ldr_to_utf8(c, argv[0], &ns) && ldr_to_utf8(c, argv[1], &vs)) {
            embi_str nm = sb_str(&ns), vt = sb_str(&vs);
            bool custom = nm.len >= 2 && nm.ptr[0] == '-' && nm.ptr[1] == '-';
            bool has_var = false;
            for (uint32_t i = 0; i + 3 < vt.len && !has_var; i++) {
                has_var = (vt.ptr[i] == 'v' || vt.ptr[i] == 'V') && (vt.ptr[i + 1] == 'a' || vt.ptr[i + 1] == 'A') &&
                          (vt.ptr[i + 2] == 'r' || vt.ptr[i + 2] == 'R') && vt.ptr[i + 3] == '(';
            }
            if (!custom && !has_var) {
                embi_bind_scope sc = embi_bind_enter(c);
                embi_css_slice sl = embi_css_parse_cvs(&c->arena, vt);
                ok = embi_prop_decl_ok(nullptr, nm, sl, false);
                (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
            }
        }
        embi_sb_free(&ns);
        embi_sb_free(&vs);
        return embi_js_from_bool(ok);
    }
    case N_ANIMATABLE: {
        /* (css property name) -> whether it is an animatable property name: a custom property, an animatable longhand, or a
         * shorthand of which some longhand is. The keyframes argument does not read the others. */
        embi_sb es = {0};
        bool ok = false;
        if (argc > 0 && ldr_to_utf8(c, argv[0], &es)) {
            embi_str nm = sb_str(&es);
            if (nm.len >= 2 && nm.ptr[0] == '-' && nm.ptr[1] == '-') {
                ok = true;
            } else {
                embi_prop_id id = embi_prop_lookup(nm);
                if (id != EMBI_PROP_NONE && embi_prop_is_shorthand(id)) {
                    const embi_prop_info *pi = embi_prop_info_of(id);
                    const uint16_t *subs = embi_prop_longhands(id);
                    for (uint32_t k = 0; pi != nullptr && subs != nullptr && k < pi->sub_n && !ok; k++) {
                        ok = embi_anim_animatable(subs[k]);
                    }
                } else if (id != EMBI_PROP_NONE) {
                    ok = embi_anim_animatable(id);
                }
            }
        }
        embi_sb_free(&es);
        return embi_js_from_bool(ok);
    }
    default:
        return EMBI_JS_UNDEFINED;
    }
}

static void host_set(embi_bind_ctx *c, embi_js_value host, const char *name, int magic, int length)
{
    embi_js_value f = ldr_native(c, name, length, host_native, magic);
    if (f != EMBI_JS_EXCEPTION) {
        embi_bind_object_set_c(c, host, name, f);
    }
}

/* ------------------------------------------------------------------ the prelude */

/* The names the prelude defines as globals, in the order of the lazy accessors' magic numbers. */
static const char *const k_names[] = {"Animation",         "KeyframeEffect",   "AnimationEffect",
                                      "AnimationTimeline", "DocumentTimeline", "AnimationPlaybackEvent",
                                      "AnimationEvent",    "TransitionEvent",  "CSSAnimation",
                                      "CSSTransition"};
#define N_NAMES ((int)(sizeof k_names / sizeof k_names[0]))

/* The exports object, evaluating the source first when needed. EMBI_JS_EXCEPTION with the exception pending. */
static embi_js_value exports_of(embi_bind_ctx *c)
{
    if (c->anim_exports != 0) {
        return embi_js_persistent_get(c->rt, c->anim_exports - 1u);
    }
    embi_js_value host = embi_bind_pin(c, embi_bind_object_new(c));
    static const struct {
        const char *name;
        int magic, length;
    } fns[] = {{"create", N_CREATE, 0},
               {"adopt", N_ADOPT, 1},
               {"id", N_ID, 1},
               {"setEffect", N_SET_EFFECT, 6},
               {"setTiming", N_SET_TIMING, 2},
               {"setTarget", N_SET_TARGET, 2},
               {"setKeyframes", N_SET_KEYFRAMES, 2},
               {"clearEffect", N_CLEAR_EFFECT, 1},
               {"get", N_GET, 2},
               {"op", N_OP, 3},
               {"list", N_LIST, 2},
               {"computed", N_COMPUTED, 1},
               {"target", N_TARGET, 1},
               {"time", N_TIME, 0},
               {"cssInfo", N_CSSINFO, 1},
               {"rafCount", N_RAF_COUNT, 1},
               {"report", N_REPORT, 1},
               {"easingOk", N_EASING_OK, 1},
               {"animatable", N_ANIMATABLE, 1},
               {"valueOk", N_VALUE_OK, 2}};
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        host_set(c, host, fns[i].name, fns[i].magic, fns[i].length);
    }
    embi_js_value global = embi_js_from_object(c->realm->global);
    static const char *const imports[] = {"Event", "EventTarget", "DOMException"};
    for (size_t i = 0; i < sizeof imports / sizeof imports[0]; i++) {
        embi_js_value v = ldr_get(c, global, imports[i]);
        if (v == EMBI_JS_EXCEPTION) {
            return v;
        }
        embi_bind_object_set_c(c, host, imports[i], v);
    }
    embi_js_value fn = embi_bind_eval_asset(c, k_animation_js, sizeof k_animation_js, "animation.js");
    if (fn == EMBI_JS_EXCEPTION) {
        return fn;
    }
    embi_js_value ex = embi_js_call(c->rt, fn, EMBI_JS_UNDEFINED, 1, &host);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    c->anim_exports = embi_js_persistent_new(c->rt, ex) + 1u;
    return ex;
}

static embi_js_value call_export(embi_bind_ctx *c, const char *name, int argc, const embi_js_value *argv)
{
    embi_js_value ex = exports_of(c);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    embi_js_value fn = ldr_get(c, ex, name);
    if (fn == EMBI_JS_EXCEPTION) {
        return fn;
    }
    return embi_js_call(c->rt, fn, EMBI_JS_UNDEFINED, argc, argv);
}

/* ------------------------------------------------------------------ lazy globals and methods */

static void define_data(embi_bind_ctx *c, const char *name, embi_js_value v)
{
    embi_js_key k = embi_js_key_c(c->rt, name);
    (void)embi_js_obj_delete(c->rt, c->realm->global, k);
    embi_js_define_raw(c->rt, c->realm->global, k, v, EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

static embi_js_value lazy_get(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    embi_js_value ex = exports_of(c);
    if (ex == EMBI_JS_EXCEPTION) {
        return ex;
    }
    embi_js_value v = ldr_get(c, ex, k_names[magic]);
    if (v == EMBI_JS_EXCEPTION) {
        return v;
    }
    define_data(c, k_names[magic], v);
    return v;
}

static embi_js_value lazy_set(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    define_data(c, k_names[magic], argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    return EMBI_JS_UNDEFINED;
}

enum { M_RAF, M_CAF, M_ANIMATE, M_EL_GET, M_DOC_GET, M_TIMELINE };

static embi_js_value method_native(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (c == nullptr) {
        return embi_js_throw_type_error(r->rt, "Illegal invocation");
    }
    embi_js_value a0 = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_js_value a1 = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    switch (magic) {
    case M_RAF:
        if (!embi_js_is_callable(a0)) {
            return embi_js_throw_type_error(r->rt, "Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function.");
        }
        return call_export(c, "raf", 1, &a0);
    case M_CAF:
        return call_export(c, "caf", 1, &a0);
    case M_ANIMATE: {
        embi_bind_obj *bo = nullptr;
        if (!embi_bind_this(c, this_v, EMBI_IF_Element, &bo)) {
            return EMBI_JS_EXCEPTION;
        }
        embi_js_value args[3] = {this_v, a0, a1};
        return call_export(c, "elementAnimate", 3, args);
    }
    case M_EL_GET: {
        embi_bind_obj *bo = nullptr;
        if (!embi_bind_this(c, this_v, EMBI_IF_Element, &bo)) {
            return EMBI_JS_EXCEPTION;
        }
        embi_js_value args[2] = {this_v, a0};
        return call_export(c, "elementGetAnimations", 2, args);
    }
    case M_DOC_GET:
        return call_export(c, "documentGetAnimations", 0, nullptr);
    case M_TIMELINE:
        return call_export(c, "documentTimeline", 0, nullptr);
    default:
        return EMBI_JS_UNDEFINED;
    }
}

void embi_impl_anim_install(embi_bind_ctx *c)
{
    for (int i = 0; i < N_NAMES; i++) {
        embi_js_define_getset(c->realm, c->realm->global, k_names[i], lazy_get, lazy_set, i, EMBI_JS_ATTR_CONFIGURABLE);
    }
    embi_js_define_method(c->realm, c->realm->global, "requestAnimationFrame", method_native, 1, M_RAF);
    embi_js_define_method(c->realm, c->realm->global, "cancelAnimationFrame", method_native, 1, M_CAF);
    if (embi_js_is_object(c->protos[EMBI_IF_Element])) {
        embi_js_object *ep = embi_js_as_object(c->protos[EMBI_IF_Element]);
        embi_js_define_method(c->realm, ep, "animate", method_native, 1, M_ANIMATE);
        embi_js_define_method(c->realm, ep, "getAnimations", method_native, 0, M_EL_GET);
    }
    if (embi_js_is_object(c->protos[EMBI_IF_Document])) {
        embi_js_object *dp = embi_js_as_object(c->protos[EMBI_IF_Document]);
        embi_js_define_method(c->realm, dp, "getAnimations", method_native, 0, M_DOC_GET);
        embi_js_define_getset(c->realm, dp, "timeline", method_native, nullptr, M_TIMELINE,
                              EMBI_JS_ATTR_CONFIGURABLE | EMBI_JS_ATTR_ENUMERABLE);
    }
}

/* ------------------------------------------------------------------ from the frame */

bool embi_bind_raf_pending(const embi_bind_ctx *c)
{
    return c->raf_count > 0;
}

void embi_bind_run_animation_frame(embi_bind_ctx *c, double timestamp_ms)
{
    if (c->raf_count == 0 || c->anim_exports == 0 || c->freed) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    if (call_export(c, "beginFrame", 0, nullptr) == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return;
    }
    embi_js_value ts = embi_js_number(timestamp_ms);
    for (;;) {
        embi_js_value more = call_export(c, "runNext", 1, &ts);
        if (more == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            break;
        }
        embi_js_run_jobs(c->rt); /* the microtask checkpoint after each callback */
        if (!embi_js_to_boolean(more)) {
            break;
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_bind_anim_notify(embi_bind_ctx *c, uint32_t id, int what, double time_ms)
{
    if (c->anim_exports == 0 || c->freed) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value args[3] = {embi_js_number((double)id), embi_js_number(what), embi_js_number(time_ms)};
    if (call_export(c, "notify", 3, args) == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    }
    /* The promise reactions wait for the checkpoint (embi_bind_anim_checkpoint): notify runs inside native calls of a
     * script and inside the animator's loops. The events are dispatched by the frame, a host-started dispatch. */
    if (what >= EMBI_ANN_EV_FINISH && what <= EMBI_ANN_EV_REMOVE && c->rt->nest == 0) {
        embi_js_run_jobs(c->rt);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_bind_anim_checkpoint(embi_bind_ctx *c)
{
    if (c->freed || c->rt->nest != 0) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_run_jobs(c->rt);
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

bool embi_bind_idle(embi_bind_ctx *c, bool anim_soon)
{
    if (c->freed || c->rt->nest != 0) {
        return false;
    }
    if (anim_soon && c->anim_exports == 0 && !c->anim_idle_tried) {
        /* The prelude the first CSS animation or transition event needs (its compile is about 1 ms), evaluated now rather
         * than inside the frame that sends that event. Tried once: a failure leaves it to the first use, as before. */
        c->anim_idle_tried = true;
        embi_bind_scope sc = embi_bind_enter(c);
        if (exports_of(c) == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
        }
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return true;
    }
    return embi_js_gc_idle(c->rt) != 0;
}

void embi_bind_anim_css_event(embi_bind_ctx *c, struct embi_element *el, int kind, embi_str name, double elapsed)
{
    if (c->freed) {
        return;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value args[4] = {embi_bind_node_value(c, &el->base), embi_js_number(kind), embi_bind_utf8_value(c, name),
                             embi_js_number(elapsed)};
    for (int i = 0; i < 4; i++) {
        embi_bind_pin(c, args[i]);
    }
    if (call_export(c, "cssEvent", 4, args) == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
    }
    if (c->rt->nest == 0) {
        embi_js_run_jobs(c->rt);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}
