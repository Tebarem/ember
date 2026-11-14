#pragma once
/*
 * Internal structures of the animator (W6c): shared by animator.c (the document side: states, the style hook, the frame)
 * and anim_play.c (the play state machine of one animation). Times are in seconds here (the public API in frame.h uses
 * milliseconds), NaN is the unresolved time value.
 */
#include "anim/frame.h"


#include <string.h>

#define EMBI_AN_NO_PHASE 255u

typedef struct embi_css_aspec {
    embi_atom name;
    double dur, delay, iters;
    uint8_t dir, fill, comp;
    bool paused;
    const embi_easing *ease;
} embi_css_aspec;

struct embi_an {
    embi_animator *an;
    void *user;                 /* non-null while the host has a wrapper: the id as a pointer */
    uint32_t id;
    bool orphan;                /* the host's wrapper was collected while the animation still runs */
    uint8_t kind;               /* EMBI_AN_* */
    uint64_t seq;               /* creation order */
    uint32_t slot;              /* index in an->all (WEB and CSSA records), UINT32_MAX when not listed */
    bool retired;               /* removed from the element's lists, kept for the host's wrapper until destroy */
    bool has_effect;
    bool has_timeline;
    embi_element *target;
    embi_anim_timing timing;    /* the effect's timing; rate unused */
    uint8_t composite, iter_composite;
    /* keyframes: the specified entries and the set resolved for the element's base style */
    embi_arena ar;              /* the entries */
    embi_kf_input *kfin;
    uint32_t nkfin;
    embi_arena rar;             /* the resolved set */
    embi_arena ear;             /* the timing easing when the animator's memo is full (reset with each timing) */
    const embi_kf_set *kf;
    uint64_t kf_gen;            /* the base_gen of the element state the set was resolved for (0: none) */
    /* the play state, in timeline seconds */
    double start, hold, prev, rate, pending_rate;
    uint8_t pending_task;       /* 0 none, 1 play, 2 pause */
    bool ready_resolved, finished_resolved;
    uint8_t replace_state;
    bool retained;              /* the host was told EMBI_ANN_RETAIN */
    /* CSS animations */
    embi_css_aspec css;         /* the values the style last gave (to see what changed) */
    bool css_paused_applied;
    uint8_t last_phase;         /* EMBI_PH_* of the last frame (css events), EMBI_AN_NO_PHASE: idle */
    double last_iter;
    uint8_t applied_phase;      /* the phase the overlay was last sampled in */
    bool moving_hint;
    /* transition proxies */
    embi_prop_id trans_prop;
};

#define EMBI_IMP_MAX 64
#define EMBI_EASING_MEMO_MAX 256u
#define EMBI_EASING_HASH 512u
typedef struct el_state el_state;
struct el_state {
    embi_element *el;
    const embi_style *base;     /* the last base style (a reference is held) */
    uint64_t base_gen;          /* changes when the base style changes in content */
    uint32_t ver;               /* bumped when an animation of the element changes */
    EMBI_VEC(embi_an *) anims;  /* CSS animations, then Web animations, in composite order */
    EMBI_VEC(embi_an *) tproxy; /* transition proxies (made on demand) */
    embi_transitions trans;
    embi_arena tar;             /* the values of trans (deep copies, reset when no transition runs) */
    /* The properties whose base value changed in the restyle pass dpass, when the element's own inputs changed in it
     * (EMBI_OWN_SELF); dcustom: its custom properties changed too. Its children's before-change check reads them. */
    EMBI_VEC(embi_prop_id) dprops;
    uint64_t dpass;
    bool dcustom;
    /* the !important cache: the first EMBI_IMP_MAX animated properties of the element are tracked; a property past that
     * is treated as not !important (an element animating more than 64 properties at once is not a case worth the cost) */
    uint64_t imp_gen;
    uint32_t imp_ver;
    embi_prop_id imp_props[EMBI_IMP_MAX];
    bool imp_flags[EMBI_IMP_MAX];
    uint32_t nimp;
    bool animating;             /* the last sample was time-varying */
};

typedef struct an_event {
    int kind;                   /* EMBI_CEV_*, or -1 for an animation event of a (EMBI_ANN_EV_*) */
    embi_element *el;
    embi_an *a;
    int what;                   /* EMBI_ANN_EV_* */
    embi_str name;
    double elapsed;
    double time_ms;
    embi_atom name_atom;
} an_event;

struct embi_animator {
    embi_style_engine *se;
    embi_animator_host host;
    embi_hashmap states;        /* embi_element* -> el_state* */
    EMBI_VEC(embi_an *) all;    /* WEB and CSSA records that are listed */
    EMBI_VEC(an_event) events;
    EMBI_VEC(embi_anim_effect) effs;   /* scratch of the style hook */
    EMBI_VEC(embi_trans_spec) tspec;
    embi_overlay ov;
    uint32_t capsheets;
    bool in_apply;              /* the style hook is running: changes do not mark the element */
    bool quiet;                 /* no marking (the frame end, element removal) */
    bool declared;              /* CF2: a resolved style declared a CSS animation or transition (embi_animator_declared) */
    const void *plain_anim;     /* CF2: the last animation property group seen without either (groups are interned) */
    uint64_t seq;
    bool replace_dirty;         /* something finished or changed: the replace pass runs at the next frame */
    uint32_t next_id;
    embi_hashmap ids;           /* id -> embi_an* */
    EMBI_VEC(uint32_t) zombies; /* ids of records whose wrapper was collected (swept at the frame) */
    struct {
        embi_str text;
        const embi_easing *easing;
        bool ok;
    } *easings;                 /* memo of parsed easing texts (at most EMBI_EASING_MEMO_MAX) */
    uint32_t neasings, capeasings;
    uint16_t easing_hash[EMBI_EASING_HASH]; /* open-addressing index into easings: index + 1, 0 = empty */
    uint64_t gen_counter;
    uint64_t frame_no;          /* counts the frame steps */
    /* The elements whose animations or transitions moved (or were changed) in this frame step or the one before: two
     * generations, the older one cleared at each frame step (embi_element* -> 0). */
    embi_hashmap motion[2];
    /* The elements whose own inputs may have changed in the restyle pass decl_pass (EMBI_OWN_SELF; embi_element* -> 0), and
     * the last pass that was a full resolve (EMBI_OWN_ALL: every element's inputs may have changed). */
    embi_hashmap decl;
    uint64_t decl_pass;
    uint64_t decl_all_pass;
    embi_hashmap pstyle;        /* embi_element* -> the parent style (a reference) of its last restyle; elements that declare transitions */
    uint32_t debug_poison_reads; /* tests, poison mode: values in poisoned memory that restyles read (hook_apply) */
    double time;                /* seconds */
    double frame_ms;
    bool want;
    uint32_t marked;
    bool in_frame;
    /* The scratch of one sample (reset once the overlay is built). The values an overlay puts in a style are deep copies in a
     * value block of the style engine, which lives as long as a style group points into it (style.h, value blocks). */
    embi_arena scratch;
    /* @keyframes */
    embi_css_sheet **sheets;
    uint32_t nsheets;
    embi_style_env env;
    bool kft_valid;
    embi_kf_table kft;
    embi_arena kfar;
    uint32_t live;              /* records alive */
};

/* anim_play.c */
void embi_an_init(embi_an *a, embi_animator *an, uint8_t kind, void *user);
void embi_an_release_mem(embi_an *a);
double embi_an_effect_end(const embi_an *a);
double embi_an_cur_time(const embi_an *a);            /* seconds, NaN unresolved */
int embi_an_state(const embi_an *a);
void embi_an_resolve_pending(embi_an *a, double ready_time);
void embi_an_update_finished(embi_an *a, bool did_seek, bool sync);
void embi_an_notify(embi_an *a, int what);
void embi_an_queue_event(embi_an *a, int what);
void embi_an_touch(embi_an *a);                       /* the element must restyle, the document needs a frame */
void embi_an_play_internal(embi_an *a, bool auto_rewind, const char **err, bool *ok);
void embi_an_pause_internal(embi_an *a, const char **err, bool *ok);
void embi_an_cancel_internal(embi_an *a);
void embi_an_effect_local(const embi_an *a, embi_anim_sample *s, bool *resolved);

/* animator.c */
void embi_animator_touch_el(embi_animator *an, embi_element *el);
void embi_animator_unlist(embi_an *a);
void embi_animator_list(embi_an *a);
void embi_animator_state_add(embi_an *a);
void embi_animator_state_remove(embi_an *a);
void embi_animator_queue(embi_animator *an, const an_event *ev);
void embi_an_sync_retain(embi_an *a, bool allow_release);
void embi_animator_trans_end(embi_an *p, bool finish);
double embi_an_trans_time(const embi_an *p);
bool embi_an_trans_info(const embi_an *p, double *delay_ms, double *duration_ms);
