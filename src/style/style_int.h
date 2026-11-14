#pragma once
/*
 * Private structures shared by the style translation units. Other modules see
 * style.h.
 */
#include "style/style.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "css/selector.h"
#include "dom/dom.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EMBI_STYLE_MAX_GROUPS 64u
#define EMBI_STYLE_NO_GROUP 0xFFFFu
#define EMBI_STYLE_CACHE_SLOTS 512u

/* Origins, as cascade ranks (see embi_cand.oset). */
enum {
    EMBI_ORIGIN_UA = 0,
    EMBI_ORIGIN_USER = 1,
    EMBI_ORIGIN_AUTHOR = 2,
};

/* Layer keys. Unlayered styles beat every layer for normal declarations. For important
 * declarations the order is reversed: the key is EMBI_LAYER_UNLAYERED - rank. */
#define EMBI_LAYER_UNLAYERED 0xFFFFu

/* ------------------------------------------------------------------ layout */

typedef struct embi_layout {
    uint32_t ngroups;
    uint16_t gid[EMBI_PROP_COUNT];   /* group of a stored property, EMBI_STYLE_NO_GROUP otherwise */
    uint16_t slot[EMBI_PROP_COUNT];  /* slot of a stored property inside its group */
    bool inherited[EMBI_STYLE_MAX_GROUPS];
    uint16_t n[EMBI_STYLE_MAX_GROUPS];
    uint16_t *props[EMBI_STYLE_MAX_GROUPS]; /* prop ids of each group, in slot order */
} embi_layout;

extern embi_layout embi_g_layout;
extern bool embi_g_layout_built;
void embi_style_engine_init_layout(void);
/* The property to group table. Inline: embi_style_value reads it for every property access of the layout. */
static inline const embi_layout *embi_layout_get(void)
{
    if (!embi_g_layout_built) {
        embi_style_engine_init_layout();
    }
    return &embi_g_layout;
}

/* ------------------------------------------------------------------ declarations */

/* One longhand of a preparsed declaration. prop is the longhand as written (it may be logical);
 * the cascade maps it to its physical property. */
typedef struct embi_plh {
    uint16_t prop;
    const embi_value *val; /* canonical */
} embi_plh;

enum {
    EMBI_PD_IMPORTANT = 1u << 0,
    EMBI_PD_HAS_VAR = 1u << 1,
    EMBI_PD_CUSTOM = 1u << 2,
    EMBI_PD_ALL = 1u << 3, /* the `all` shorthand: lh[0].val is the CSS-wide keyword for every longhand but direction and unicode-bidi */
};

/* A preparsed declaration. A declaration with var() keeps its raw value and is parsed when it is
 * computed. A custom property keeps its raw value always. */
typedef struct embi_pdecl {
    embi_atom name;      /* custom property name (CUSTOM only) */
    uint16_t prop;       /* top-level property id (0 for custom) */
    uint8_t flags;       /* EMBI_PD_* */
    uint32_t nlh;
    const embi_plh *lh;  /* parsed longhands (not HAS_VAR, not CUSTOM) */
    embi_css_slice raw;  /* HAS_VAR and CUSTOM: the raw value */
} embi_pdecl;

/* A cached declaration list (style attributes and presentational hints). */
typedef struct embi_style_declrec {
    const embi_pdecl *d;
    uint32_t n;
    embi_str text;       /* the cache key text (verified on lookup, arena copy) */
} embi_style_declrec;

/* One flattened (rule, complex selector) pair. */
typedef struct embi_sentry {
    const embi_sel_complex *sel;
    const embi_pdecl *decls;
    uint32_t ndecls;
    uint64_t order;      /* sheet sequence and source offset */
    uint16_t layer;      /* layer key for normal declarations */
    uint8_t origin;      /* EMBI_ORIGIN_* */
    bool unsafe;         /* a candidate that depends on position or state: no style sharing */
} embi_sentry;

/* @property registration. */
typedef struct embi_sreg {
    embi_atom name;
    embi_str syntax;
    bool inherits;
    bool has_initial;
    embi_css_slice initial;
} embi_sreg;

/* The flattened rules of one origin (the UA sheet, the user sheets, or a document's author
 * sheets). */
typedef struct embi_sset {
    embi_sentry *e;
    uint32_t n, cap;
    embi_sel_index index;
    uint32_t pe_mask;    /* bit (1 << pseudo) for each pseudo-element that some rule uses */
    embi_sreg *regs;
    uint32_t nregs, capregs;
    const embi_css_rule **cstyles; /* the @counter-style rules, in order (the sheets outlive the set) */
    uint32_t ncstyles, capcstyles;
    bool has_attrfn;     /* W6b: some declaration of the set uses attr() */
} embi_sset;

/* ------------------------------------------------------------------ interned values */

typedef struct embi_cust_entry {
    embi_atom name;
    embi_css_slice val;
} embi_cust_entry;

/* Computed custom properties, sorted by name. Allocated in the engine arena. */
typedef struct embi_custom_map {
    uint32_t n;
    embi_cust_entry e[];
} embi_custom_map;

typedef struct embi_style_group embi_style_group;
struct embi_style_group {
    uint32_t refs;
    uint32_t hash;
    uint64_t xh;         /* XOR of embi_style_slot_mix over the slots */
    uint32_t gid;
    uint32_t n;
    const embi_value *vals[];
};

struct embi_style {
    uint32_t refs;
    uint32_t hash;
    const embi_custom_map *custom;
    const embi_style_group *groups[];
};

/* ------------------------------------------------------------------ cascade scratch */

typedef struct embi_cand {
    uint8_t oset;        /* importance and origin rank: normal UA 1, USER 2, AUTHOR 3; important
                          * AUTHOR 4, USER 5, UA 6 */
    bool important;
    uint8_t origin;      /* EMBI_ORIGIN_* */
    uint16_t layer;      /* layer key */
    bool attr;           /* element-attached (style attribute): beats every layer of its oset */
    embi_sel_spec spec;
    uint64_t order;
    const embi_pdecl *d;
} embi_cand;

typedef struct embi_win {
    int32_t cand;        /* index into the candidate list */
    uint16_t orig;       /* the longhand (as written) that the candidate sets */
} embi_win;

typedef struct embi_cust_win {
    embi_atom name;
    const embi_pdecl *d;
    uint32_t order;      /* candidate index of the winning declaration */
} embi_cust_win;

typedef struct embi_sctx {
    embi_style_engine *eng;
    const embi_style *parent;       /* nullptr while the initial style is built */
    const embi_style *init;         /* nullptr while the initial style is built */
    const embi_custom_map *pcustom; /* the parent's custom properties */
    const embi_custom_map *ccustom; /* the element's custom properties (computed) */
    bool custom_ready;              /* ccustom is final: var() reads it */
    double root_px;                 /* computed font size of the root element */
    bool is_root;
    uint32_t gen;
    uint64_t dirty;                 /* groups that differ from their source */
    const embi_value *ovr[EMBI_PROP_COUNT];
    uint32_t ovr_gen[EMBI_PROP_COUNT];
    embi_win win[EMBI_PROP_COUNT];
    uint32_t win_gen[EMBI_PROP_COUNT];
    uint16_t targets[EMBI_PROP_COUNT];
    uint32_t ntargets;
    embi_cand *cands;
    uint32_t ncands, capcands;
    embi_cust_win *cw;
    uint32_t ncw, capcw;
    bool unsafe;
    embi_element *el;               /* the element (the originating one for a pseudo-element), for attr() */
    bool dark;                      /* used color scheme of the element (light-dark) */
    uint16_t wm;                    /* writing-mode keyword of the element */
    uint16_t dir;                   /* direction keyword of the element */
    const embi_sset *sets[3];       /* the origins that apply: UA, user, author */
    uint32_t nsets;
} embi_sctx;

/* ------------------------------------------------------------------ documents */

/* W6b: what the selectors of a set of rules can see of a change (inval.c). names maps (kind << 32 | lowercase atom)
 * of an id, class or attribute name that occurs outside the subject compound of some selector to the EMBI_IVM_* bits
 * of the elements its change reaches. */
/* CF1: the user-action pseudo-classes the selector state hooks answer (embi_style_mark_user_state). */
enum { EMBI_USTATE_HOVER, EMBI_USTATE_ACTIVE, EMBI_USTATE_FOCUS, EMBI_USTATE_FOCUS_VISIBLE, EMBI_USTATE_FOCUS_WITHIN,
       EMBI_USTATE_COUNT };

/* CF1: the subjects that the rules reaching descendants (EMBI_IVM_DESC) select, as features an element must have to be one:
 * (1 << 32 | lowercase tag atom), (2 << 32 | lowercase class atom) or (3 << 32 | lowercase id atom), one per rule. all: some
 * rule's subject has none of them (every descendant can be one), or more than EMBI_IVFS_MAX different features were added
 * (fs_add then degrades the set to all, and a change restyles every descendant as before CF1). Lowercase comparison makes
 * the match a superset (quirks-mode classes, non-HTML type names). */
#define EMBI_IVFS_MAX 6
typedef struct embi_ivfs {
    bool all;
    uint8_t n;
    uint64_t f[EMBI_IVFS_MAX];
} embi_ivfs;

typedef struct embi_ivset {
    embi_hashmap names;
    embi_hashmap dfeat;              /* CF1: names key -> 1 + index in fs of the subjects of its EMBI_IVM_DESC rules */
    EMBI_VEC(embi_ivfs) fs;          /* CF1 */
    uint32_t ufs[EMBI_USTATE_COUNT]; /* CF1: 1 + index in fs for the EMBI_IVM_DESC rules of ustate (0: none) */
    uint32_t flags;   /* EMBI_IVF_* */
    uint8_t anyattr;  /* EMBI_IVM_* bits that any attribute change reaches (state pseudo-classes outside the subject) */
    uint8_t ustate[EMBI_USTATE_COUNT]; /* CF1: EMBI_IVM_* bits that a flip of a user-action state of an element reaches */
    bool valid;
} embi_ivset;

/* One tracked style change of an element (see embi_style_delta): old holds a reference. */
typedef struct embi_style_chg {
    embi_element *el;
    const embi_style *old;
    uint8_t pseudo; /* a pseudo-element style of the element changed: the worst class (EMBI_SCH_*) of those changes */
} embi_style_chg;

typedef struct embi_docstate {
    embi_style_engine *eng;
    embi_document *doc;
    embi_css_sheet **sheets;
    uint32_t nsheets;
    embi_sset author;
    bool dirty;
    bool resolved;
    bool observed;
    uint64_t epoch;       /* counts the resolves that did work (embi_style_document_epoch) */
    double root_px;
    embi_hashmap pseudo;  /* (element << 4 | pseudo) -> embi_style* (or nullptr: not generated) */
    /* ---- W6b (inval.c, inval_walk.c): incremental restyle and change tracking ---- */
    bool full;                  /* the next resolve restyles every element */
    bool track;                 /* a layout engine consumes the change set (embi_style_track) */
    bool verifying;             /* the full pass of EMBER_VERIFY_INCREMENTAL: a changed style is a mismatch */
    bool pseudo_changed;        /* scratch of enter_element: a pseudo-element style of the element changed */
    uint8_t pseudo_cls;         /* CF1: and the worst class (EMBI_SCH_*) of those changes */
    embi_ivset iv;              /* the invalidation info of the author sheets */
    embi_hashmap dmap;          /* element -> EMBI_IVD_* bits: restyle needed, or something below needs it */
    embi_hashmap cmap;          /* element -> index + 1 in chg (the tracked changes) */
    EMBI_VEC(embi_style_chg) chg;
    embi_hashmap omap;          /* element -> EMBI_ORIG_* bits: the DOM changes layout must look at */
    EMBI_VEC(const embi_style *) grave;  /* old styles layout caches may be keyed by (references held) */
    EMBI_VEC(embi_style_change) outc;  /* the last delta handed out */
    EMBI_VEC(embi_style_origin) outo;
    bool lost;                  /* tracking was dropped (too many changes, or a full restyle without a consumer) */
    uint64_t from_epoch;        /* the epoch at the last take */
    uint32_t restyled;          /* elements restyled by the last resolve */
    uint64_t total_restyled, partial_passes, full_passes, skipped_resolves;
    uint32_t cls_layout, cls_paint, cls_refresh;  /* classes of the last delta */
} embi_docstate;

/* The flattened form of one rule: its compiled selectors and preparsed declarations. Sheets are
 * immutable and retained by the engine, so a rule (and the nesting parent it was compiled against)
 * is parsed once per engine however often the sets are re-flattened. */
typedef struct embi_rcache_rec {
    const embi_css_rule *rule;
    const embi_sel_list *nest;
    const embi_sel_list *list;   /* nullptr: the selector list is invalid */
    const embi_pdecl *pds;
    uint32_t nd;
} embi_rcache_rec;

/* One entry of the style sharing cache. The entry holds one reference on its style. */
typedef struct embi_share_slot {
    uint64_t key;
    const embi_style *s;
} embi_share_slot;

/* ------------------------------------------------------------------ engine */

/* A value of a declaration with var() (compute.c compute_var), for one longhand and one substituted text. */
typedef struct embi_varrec {
    const void *d;          /* the declaration (embi_pdecl) */
    const char *text;       /* the text after substitution (engine arena copy) */
    uint32_t len;
    uint16_t orig;          /* the longhand */
    uint16_t prop;          /* the declared property (a shorthand gives other longhands from the same text) */
    const embi_value *v;    /* the canonical value, nullptr when the text is invalid for the longhand */
} embi_varrec;

#define EMBI_STYLE_VCACHE_MAX 65536u

struct embi_style_engine {
    embi_fontdb *fonts;
    embi_style_env env;
    embi_style_callbacks cb;
    embi_arena arena;
    embi_css_sheet *ua_sheet;     /* nullptr until the UA set is first flattened (embi_style_ua_ensure) */
    bool ua_ready;                /* e->ua is flattened for the current environment */
    bool ua_used;                 /* this engine counts as a user of the shared UA sheet */
    embi_sset ua;
    embi_sset user;
    embi_css_sheet **user_sheets;
    uint32_t nuser_sheets;
    embi_css_sheet **retained;   /* every sheet ever attached (one reference each): kept alive for slices */
    uint32_t nretained, capretained;
    embi_hashmap docs;           /* embi_document* -> embi_docstate* */
    embi_hashmap vtab;           /* text hash -> canonical value record */
    embi_hashmap gtab;           /* group hash -> group */
    embi_hashmap stab;           /* style hash -> style */
    embi_hashmap ctab;           /* CF1: content hash -> custom property map (embi_style_custom_intern) */
    embi_hashmap hints;          /* declaration cache: hash -> embi_style_declrec */
    embi_hashmap rcache;         /* flatten cache: rule/nest hash -> embi_rcache_rec (sheets are immutable) */
    const embi_value *w_med;     /* the used initial border/outline width (medium, 3px) */
    uint64_t cache_hits, cache_misses;
    const embi_style *init;
    const embi_value *initial_raw[EMBI_PROP_COUNT];
    embi_sctx sc;
    embi_bloom bloom;
    embi_share_slot cache[EMBI_STYLE_CACHE_SLOTS];
    embi_css_sheet **font_sheets; /* sheets whose @font-face rules are registered */
    uint32_t nfont_sheets, capfont_sheets;
    embi_hashmap face_done;       /* @font-face rules (by address) whose face is registered or failed for good */
    bool font_pending;            /* the load_font callback said a source is not there yet (embi_style_font_defer) */
    embi_str font_base;           /* base URL passed to load_font for sheets registered next */
    embi_sel_ctx selctx;
    embi_sel_hooks hooks;         /* the selector state hook (embi_style_set_sel_hooks); hooks.state nullptr when none */
    bool (*state_idle)(void *user); /* optional: true when no element is hovered, pressed or focused (embi_style_set_state_idle) */
    bool uses_state;              /* an author or user sheet has a :hover, :active or :focus* selector (never reset) */
    embi_sb sb;
    uint32_t nstyles, ngroups, ncustom;
    size_t style_bytes, group_bytes, custom_bytes;
    bool init_built;
    embi_ivset iv;                /* W6b: invalidation info of the UA and user sheets */
    bool attrfn_inline;           /* W6b: a style attribute or hint declaration used attr() (sticky) */
    embi_style_anim_hooks anim;   /* W6c: the animation layer (anim_hook.c) */
    /* The values of declarations with var() (compute.c compute_var), keyed by the declaration, the longhand and the text after
     * substitution: a restyle that substitutes the same text (every frame of an animation) parses nothing into the engine
     * arena. The substitution itself goes to vscratch, which is rewound after each use. */
    embi_arena vscratch;
    embi_hashmap vcache;          /* key hash -> embi_varrec */
    embi_sb vsb;
    uint32_t nvcache;
    /* WPTASAN (vblock.c): the value blocks of the animation layer. vpages maps a page of memory to the chain of the pieces of
     * sealed blocks on it (nvpieces of them in all); vpins maps a group that points into some block to the block (or, tagged with bit 0, to an embi_vpin_list
     * when it points into several). */
    embi_hashmap vpages;
    uint8_t *vfilter; /* a counting filter in front of vpages (vblock.c) */
    uint32_t nvpieces;
    embi_hashmap vpins;
    uint32_t nvblocks;
    size_t vblock_bytes;
    embi_style_vblock *vfree; /* freed blocks kept for reuse (their arena reset to its first block) */
    uint32_t nvfree;
    /* WPTASAN: the restyle that apply runs in (embi_style_anim_pass, embi_style_anim_own_change) */
    uint64_t pass_no;             /* counts the resolve passes */
    uint8_t own_change;           /* EMBI_OWN_*: why the element being styled is restyled */
    uint32_t debug_poison_reads;  /* WPTASAN test hook (embi_style_debug_poison_reads) */
};

/* ------------------------------------------------------------------ intern.c */

/* Canonical copy of a value: simple values are deduplicated by their serialization. The result
 * lives as long as the engine. */
const embi_value *embi_style_canon(embi_style_engine *e, const embi_value *v);
/* A group with the given values (one per slot, canonical), owned by the caller (one reference). */
const embi_style_group *embi_style_group_intern(embi_style_engine *e, uint32_t gid,
                                                const embi_value *const *vals);
/* A style over the groups and the custom map. Each group carries one reference for this call,
 * which the style takes over (or releases when an equal style already exists). The result is owned
 * by the caller. */
/* CF1: the custom property map with these n entries (sorted by name): an earlier map with the same names and the same
 * tokens, else a new one in the engine arena. Equal maps are one object, so a restyle that computes the same custom
 * properties again gives the same style (and the descendants that inherit them are not restyled). */
const embi_custom_map *embi_style_custom_intern(embi_style_engine *e, const embi_cust_entry *ents, uint32_t n);
embi_style *embi_style_intern(embi_style_engine *e, const embi_style_group *const *groups,
                              const embi_custom_map *custom);
void embi_style_ref(const embi_style *s);
void embi_style_unref(embi_style_engine *e, const embi_style *s);
embi_custom_map *embi_style_custom_alloc(embi_style_engine *e, uint32_t n);

/* Contribution of one slot to a group hash. A group hash is the XOR over its slots, so a group that
 * differs from a base group in a few slots is hashed from the base hash in O(changed slots). */
uint64_t embi_style_slot_mix(uint32_t slot, const embi_value *v);
/* Interns a group whose slot hash is xh (computed with embi_style_slot_mix). Takes a reference. */
const embi_style_group *embi_style_group_intern_xh(embi_style_engine *e, uint32_t gid,
                                                   const embi_value *const *vals, uint64_t xh);

/* ------------------------------------------------------------------ vblock.c (WPTASAN) */

/* A new group pins the value blocks its slots point into; a freed group unpins them (intern.c calls these). */
void embi_style_vpin_group(embi_style_engine *e, const embi_style_group *g);
void embi_style_vunpin_group(embi_style_engine *e, const embi_style_group *g);
/* Engine teardown: frees the blocks that are left. */
void embi_style_vblocks_free(embi_style_engine *e);

/* ------------------------------------------------------------------ flatten.c */

/* Flattens the rules of a sheet set. Clears the set first. */
void embi_style_flatten_sheets(embi_style_engine *e, embi_sset *set, embi_css_sheet *const *sheets,
                               uint32_t n, uint8_t origin, const embi_style_env *env);
void embi_style_sset_free(embi_sset *set);
/* Font faces of a sheet set (run when the set is attached). */
void embi_style_register_fonts(embi_style_engine *e, embi_css_sheet *const *sheets, uint32_t n,
                               const embi_style_env *env);
/* Whether a compiled selector depends on something style sharing cannot key on. */
bool embi_style_sel_unsafe(const embi_sel_complex *c);
/* Whether the selector reads a user-action state (:hover, :active, :focus, :focus-visible, :focus-within). */
bool embi_style_sel_state(const embi_sel_complex *c);
/* Preparses a declaration list into the engine arena. */
const embi_pdecl *embi_style_preparse(embi_style_engine *e, const embi_css_decl *decls, uint32_t n,
                                      uint32_t *count);

/* ------------------------------------------------------------------ cascade.c */

/* Appends one candidate per declaration. layer is the normal layer key. */
void embi_style_push_decls(embi_sctx *sc, const embi_pdecl *decls, uint32_t n, uint8_t origin,
                           uint16_t layer, embi_sel_spec spec, uint64_t order, bool attr);
/* Matches el (or its pseudo-element pe) against the entries of a set. */
void embi_style_gather(embi_sctx *sc, const embi_sset *set, embi_element *el, embi_sel_pseudo pe,
                       const embi_bloom *anc, embi_sel_ctx *selctx);
void embi_style_cands_sort(embi_sctx *sc);
/* The style attribute, appended to the candidates. */
void embi_style_gather_attr(embi_style_engine *e, embi_sctx *sc, embi_element *el);
/* Copies an attribute value out as UTF-8 (free the buffer with embi_free). */
embi_str embi_style_attr_utf8(const embi_dstr *d, size_t *cap_out, char **buf_out);
/* Computes the element's values from the gathered candidates and returns its style (owned). */
embi_style *embi_style_cascade_finish(embi_style_engine *e, embi_sctx *sc);
/* Builds a style from the computed values (dirty groups interned, clean groups shared). */
embi_style *embi_style_finish_groups(embi_style_engine *e, embi_sctx *sc);

/* ------------------------------------------------------------------ compute.c */

const embi_value *embi_style_initial_value(const embi_sctx *sc, uint16_t p);
const embi_value *embi_style_parent_value(const embi_sctx *sc, uint16_t p);
void embi_style_set_ovr(embi_sctx *sc, uint16_t p, const embi_value *v);
bool embi_style_touched(const embi_sctx *sc, uint16_t p);
const embi_value *embi_style_elem_value(const embi_sctx *sc, uint16_t p);
void embi_style_compute_target(embi_sctx *sc, uint16_t t);
uint32_t embi_style_origs(const embi_pdecl *d, uint16_t *out, uint32_t cap);
uint16_t embi_style_target(const embi_sctx *sc, uint16_t o);
bool embi_style_var_lookup(void *user, embi_str name, embi_css_slice *value);
void embi_style_fix_display(embi_sctx *sc);
void embi_style_fix_widths(embi_sctx *sc);
void embi_style_fix_text_align(embi_sctx *sc);
bool embi_style_scheme_dark(const embi_sctx *sc, const embi_value *scheme);
/* The element's used color scheme is dark (Color Adjust 1, section 2.2). */
const embi_value *embi_style_finalize_value(embi_sctx *sc, uint16_t p, const embi_value *v);

/* ------------------------------------------------------------------ resolve.c */

void embi_style_sc_setup(embi_style_engine *e, const embi_docstate *ds, const embi_style *parent, bool is_root);
void embi_style_gather_all(embi_style_engine *e, embi_sctx *sc, embi_element *el, embi_sel_pseudo pe, const embi_bloom *anc);
void embi_style_forget_document_walk(embi_style_engine *e, embi_document *doc);
/* Releases the styles of a detached subtree (its elements and their pseudo-elements). */
void embi_style_release_subtree(embi_style_engine *e, embi_docstate *ds, embi_node *root);
/* Drops every reference held by the sharing cache. */
void embi_style_cache_clear(embi_style_engine *e);
void embi_style_value_serialize(embi_sb *sb, const embi_value *v);

/* ------------------------------------------------------------------ fontface.c */

/* Registers one @font-face rule with the engine's font registry. base is the sheet's base URL
 * (passed to the embedder's load_font callback). */
void embi_style_font_face_rule(embi_style_engine *e, embi_str base, const embi_css_rule *r);

/* ------------------------------------------------------------------ hints.c */

/* Presentational hints of an element, appended to the candidates. */
void embi_style_gather_hints(embi_style_engine *e, embi_sctx *sc, embi_element *el);

/* ------------------------------------------------------------------ ua.c */

/* The shared UA sheet: see style.h (embi_style_ua_sheet and friends). The engine flattens it into e->ua on first
 * use: every reader of e->ua calls embi_style_ua_ensure first. */

/* ------------------------------------------------------------------ helpers */

static inline bool embi_str_is_custom(embi_str s)
{
    return s.len >= 2 && s.ptr[0] == '-' && s.ptr[1] == '-';
}

static inline embi_str embi_style_copy_str(embi_arena *a, embi_str s)
{
    char *p = embi_arena_alloc(a, s.len + 1, 1);
    if (s.len > 0) {
        memcpy(p, s.ptr, s.len);
    }
    p[s.len] = '\0';
    return (embi_str){p, s.len};
}
