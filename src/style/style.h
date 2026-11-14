#pragma once
/*
 * Cascade, computed style and @font-face registration (W2d). Internal to Ember: the layout (W3),
 * paint and CSSOM (W5) read computed values through this header.
 *
 * Model. An embi_style_engine owns the UA sheet, the user sheets, the media environment and the
 * computed-style intern tables. Each document has a set of author sheets. Computed styles are
 * immutable and interned: an embi_style holds one embi_style_group per group (a property group of
 * props.txt, split into its inherited and reset halves), plus the custom-property map. Elements hold
 * a counted reference to their embi_style in embi_element.style. Two elements whose computed values
 * agree share the same structs, so the per-element cost is one pointer.
 *
 * Lifetime. Styles and groups are freed when their last reference goes. Parsed values, declared
 * values and every arena allocation live as long as the engine: the arena also holds the cached
 * style-attribute and presentational-hint declarations, which grow with the distinct values a page
 * sets (no eviction yet; W6 may add one). Sheets are retained by the engine (one reference per
 * distinct sheet, released by embi_style_engine_destroy), so raw token slices stay valid. Call
 * embi_style_forget_document before freeing a document that the engine resolved; a document still
 * attached when the engine is destroyed is forgotten by the destroy.
 *
 * Dirty tracking. Every DOM mutation of a document marks it dirty through a W1 observer, and
 * embi_style_resolve_document then runs a full resolve. Incremental restyle is W6.
 */
#include "css/css.h"
#include "css/props.h"
#include "css/selector.h"
#include "dom/dom.h"
typedef struct embi_fontdb embi_fontdb;

typedef struct embi_style embi_style;
typedef struct embi_style_engine embi_style_engine;

/* The media environment (Media Queries 4/5). Lengths are CSS px. */
typedef struct embi_style_env {
    double width, height;   /* viewport size in CSS px */
    double dppx;            /* device pixel ratio */
    bool dark;              /* prefers-color-scheme: dark */
    bool reduced_motion;    /* prefers-reduced-motion: reduce */
    uint8_t contrast;       /* 0 no-preference, 1 more, 2 less */
    bool hover;             /* hover and any-hover: hover */
    uint8_t pointer;        /* 0 none, 1 coarse, 2 fine (pointer and any-pointer) */
    bool scripting;         /* scripting: enabled */
    bool forced_colors;     /* forced-colors: active */
    uint8_t gamut;          /* 0 srgb, 1 p3, 2 rec2020 (color-gamut) */
    bool hdr;               /* dynamic-range: high */
    uint8_t update;         /* 0 none, 1 slow, 2 fast */
    /* Optional system color palettes (embi_system_color_names order), nullptr for the built-in
     * defaults. Each must have embi_system_color_count entries. */
    const embi_color_rgb8 *palette_light;
    const embi_color_rgb8 *palette_dark;
} embi_style_env;

/* Embedder callbacks. load_font fetches one @font-face source. url is as written, base is the
 * sheet's base URL (possibly empty). On success it sets *data to bytes from SDL_malloc (the font
 * registry takes ownership) and *len to their length, and returns true. Returning false tries the
 * next source. */
typedef struct embi_style_callbacks {
    void *user;
    bool (*load_font)(void *user, embi_str base, embi_str url, uint8_t **data, size_t *len);
} embi_style_callbacks;

/* ------------------------------------------------------------------ engine */

/* fonts receives @font-face faces and must outlive the engine. env and cb may be nullptr (a
 * default environment of 800x600 and no font loader). */
embi_style_engine *embi_style_engine_create(embi_fontdb *fonts, const embi_style_env *env,
                                            const embi_style_callbacks *cb);
void embi_style_engine_destroy(embi_style_engine *e);

/* The UA sheet, parsed once per cache lifetime (while an engine or runtime uses it) and shared by all engines (thread
 * safe; the first caller parses, others wait for it). embi_style_ua_sheet returns a new reference and counts a user; the cache holds its own reference while
 * the user count is above zero. embi_style_ua_release drops a reference taken that way and its user;
 * embi_style_ua_unuse drops only the user (the caller has released the reference itself). The runtime prefetches.
 * embi_style_ua_sheet returns nullptr, and counts no user, when the sheet cannot be made (a damaged compressed asset);
 * embi_style_ua_release(nullptr) does nothing. */
embi_css_sheet *embi_style_ua_sheet(void);
void embi_style_ua_unuse(void);
void embi_style_ua_release(embi_css_sheet *s);
uint32_t embi_style_ua_users(void);
/* Flattens the engine's UA set now if it is not yet (it is otherwise done by the first style resolve). Waits for the
 * shared sheet if another thread is still parsing it. */
void embi_style_ua_ensure(embi_style_engine *e);
/* Replaces the media environment. Every document is re-flattened and marked dirty. */
void embi_style_engine_set_env(embi_style_engine *e, const embi_style_env *env);
/* Sets the selector state hook (css/selector.h) that every match of the engine uses for the state pseudo-classes
 * (:hover, :focus, :checked, ...). The hook is copied; nullptr restores the DOM-only answers. Every document is marked
 * dirty (W5d2). */
void embi_style_set_sel_hooks(embi_style_engine *e, const embi_sel_hooks *hooks);
/* An optional fast check on the hook's user pointer: when idle(user) is true, no element is hovered, pressed or focused,
 * so the resolver skips the per-element state queries of the style-sharing path. nullptr (the default) never skips. */
void embi_style_set_state_idle(embi_style_engine *e, bool (*idle)(void *user));
/* Whether an author or user sheet of the engine has a rule that reads :hover, :active, :focus, :focus-visible or
 * :focus-within. Sticky: once a sheet has one, the answer stays true. The input layer restyles on pointer and focus
 * changes only when this holds (W5d2). */
bool embi_style_uses_state(const embi_style_engine *e);
/* Replaces the user-origin sheets (refcounted by the engine). */
void embi_style_engine_set_user_sheets(embi_style_engine *e, embi_css_sheet *const *sheets,
                                       uint32_t n);
/* Sets the base URL (the parse's base_url) that load_font receives for the @font-face rules of
 * the author sheets registered by the next embi_document_set_author_sheets calls. Copied. */
void embi_style_engine_set_font_base(embi_style_engine *e, embi_str base);
/* Called by the load_font callback when a source failed only because it has not arrived yet (a network font): the sheet
 * being registered is not marked as registered, so the next embi_document_set_author_sheets registers its faces again. */
void embi_style_font_defer(embi_style_engine *e);
/* Replaces the author sheets of a document (refcounted by the engine) and registers their
 * @font-face rules. The document is marked dirty. */
void embi_document_set_author_sheets(embi_style_engine *e, embi_document *doc,
                                     embi_css_sheet *const *sheets, uint32_t n);
/* Marks the document dirty: the next resolve restyles it fully. For a change the engine cannot see (a selector state
 * moved, W5d2); DOM and sheet changes mark it themselves. */
void embi_style_mark_dirty(embi_style_engine *e, embi_document *doc);
/* Full resolve of the document when it is dirty (or was never resolved). Every element's style
 * slot gets its embi_style. */
void embi_style_resolve_document(embi_style_engine *e, embi_document *doc);
/* A counter that changes every time embi_style_resolve_document recomputed the styles of the document
 * (that is, after any DOM mutation, sheet change or environment change). Zero for a document the engine
 * does not know. Layout (W3p) keeps its box tree while the epoch is unchanged. */
uint64_t embi_style_document_epoch(embi_style_engine *e, embi_document *doc);
/* Releases the document's element and pseudo-element styles, and detaches the engine from it. The
 * document must still be alive. */
void embi_style_forget_document(embi_style_engine *e, embi_document *doc);

/* The style of an element (pseudo == EMBI_PE_NONE), or of one of its generated pseudo-elements.
 * Pseudo-elements that are not generated return nullptr. Non-generated styles are computed on
 * demand. The result is valid until the next resolve of the document. */
const embi_style *embi_style_get(embi_style_engine *e, embi_element *el, embi_sel_pseudo pseudo);
/* As embi_style_get for ::before, ::after and ::marker, but only returns the style the last resolve of the
 * document generated, never computing one on demand: nullptr when the element has none. Layout asks for
 * these on every element of every layout, and the on-demand path rebuilds the ancestor chain and the
 * selector context for the elements that have no such pseudo-element (W3h). */
const embi_style *embi_style_generated(embi_style_engine *e, embi_element *el, embi_sel_pseudo pseudo);
/* False when no element of the document has a stored generated (::before, ::after, ::marker) style, so
 * embi_style_generated would find none: the box build skips its per-element lookups then (W3q). True when the
 * document is not attached. */
bool embi_style_has_generated(embi_style_engine *e, const embi_document *doc);
/* The @counter-style rule named name that applies to the document (CSS Counter Styles 3 section 3: the last one
 * in the author sheets, then the user and UA sheets), or nullptr. Valid until the document's sheets change. */
const embi_css_rule *embi_style_counter_style(embi_style_engine *e, const embi_document *doc, embi_atom name);
/* The computed value of a property, serialized (embi_value_serialize). Logical properties read
 * the physical property they map to in the style's writing mode and direction. Returns false for
 * shorthands and unknown ids. */
bool embi_style_value_text(const embi_style *s, embi_prop_id prop, embi_sb *sb);
/* The computed value of a property, or nullptr for shorthands and unknown ids. Logical properties
 * read their physical counterpart. */
const embi_value *embi_style_value(const embi_style *s, embi_prop_id prop);
/* W4g4c: the aux of a computed display keyword of an absolutely positioned box that blockification changed from an
 * inline-level display (its static position is the one of an inline-level box). */
#define EMBI_DISP_FROM_INLINE 1
/* The computed value of a custom property (--name). Returns false when it is not set. */
bool embi_style_custom_text(const embi_style *s, embi_atom name, embi_sb *sb);
/* R4c: the style an element would have with `parent` as the style it inherits from (the DOM parent's by default): the cascade
 * matches the element in its own place (its selectors see its real ancestors) but the inherited properties come from
 * parent. This is how the instance of a `use` element gets its style (SVG 2 section 5.6: the referenced element inherits from
 * the use element). The result is owned by the caller (release it with embi_style_release) and is not stored in the element.
 * nullptr when the document is not attached to the engine. */
const embi_style *embi_style_compute_under(embi_style_engine *e, embi_element *el, const embi_style *parent);
void embi_style_release(embi_style_engine *e, const embi_style *s);
/* The initial style (the computed values of every property with no declarations). */
const embi_style *embi_style_initial(const embi_style_engine *e);

/* Counts for benches and tests: the live interned styles and groups, and their bytes. */
typedef struct embi_style_stats {
    uint32_t styles, groups, custom_maps;
    size_t bytes;        /* interned styles, groups and custom maps (not the element slots) */
    size_t arena_bytes;  /* the engine arena: declared and canonical values, cached declarations */
    uint64_t share_hits, share_misses; /* style sharing cache lookups of non-root elements */
} embi_style_stats;
void embi_style_engine_stats(const embi_style_engine *e, embi_style_stats *out);

/* ------------------------------------------------------------------ media queries */

/* True when the media query list (component values of an @media prelude) matches env. */
bool embi_style_media_match(embi_css_slice prelude, const embi_style_env *env);

/* ------------------------------------------------------------------ property group layout */

/* The number of style groups (each group is one property group, inherited or reset). */
uint32_t embi_style_group_count(void);
/* The group index of a stored property, or UINT32_MAX for shorthands and logical properties. */
uint32_t embi_style_group_of(embi_prop_id prop);

/* ------------------------------------------------------------------ incremental restyle (W6b) */

/* Invalidation and partial restyle. A DOM change marks the elements its selectors can reach (inval.c: invalidation sets
 * built from the selectors of the UA, user and author sheets), and embi_style_resolve_document restyles only the marked
 * paths, reusing the parents' styles. A change that the engine cannot see (a sheet, the environment, the selector state
 * hooks, embi_style_mark_dirty) restyles every element. Either way the result is exactly what a full resolve gives: with
 * EMBER_VERIFY_INCREMENTAL=1 (or embi_incr_set_verify) every partial resolve is followed by a full one and compared.
 *
 * Change tracking. A layout engine opts in with embi_style_track. From then on the engine records, for every element whose
 * style (or pseudo-element style) changed, the old style and what kind of change it was, and the elements whose children,
 * text or attributes changed. embi_style_take_delta hands the set out once. The old styles stay alive (the "grave") until
 * embi_style_release_grave, because the layout's caches are keyed by style address. */
typedef enum embi_style_change_class {
    EMBI_SCH_NONE = 0,    /* the style object differs but no visible property does (never reported in a delta) */
    EMBI_SCH_REFRESH = 1, /* only custom properties or properties that layout and paint do not read */
    EMBI_SCH_PAINT = 2,   /* only paint properties: color, background, opacity, transform ... (no relayout) */
    EMBI_SCH_LAYOUT = 3,  /* anything else, or a pseudo-element style changed */
} embi_style_change_class;

typedef struct embi_style_change {
    embi_element *el;
    const embi_style *old_style; /* what the element's boxes hold; valid until embi_style_release_grave */
    uint8_t cls;                 /* embi_style_change_class */
    bool pseudo;                 /* CF1: a pseudo-element style of the element changed (cls counts that change: a ::before,
                                  * ::after or ::marker that stays generated with the same layout properties is a paint
                                  * change, the boxes of the pseudo-element then take its new style) */
} embi_style_change;

/* DOM changes layout has to look at, per element (the parent for child and text changes). */
#define EMBI_ORIG_STRUCT 0x01u /* children were inserted or removed */
#define EMBI_ORIG_TEXT 0x02u   /* the data of a text child changed */
#define EMBI_ORIG_ATTR 0x04u   /* an attribute the box build may read changed */
#define EMBI_ORIG_PAINT 0x08u  /* CF1: an attribute only the paint reads changed (checked): the element is repainted */
typedef struct embi_style_origin {
    embi_element *el;
    uint8_t bits;
} embi_style_origin;

typedef struct embi_style_delta {
    bool lost;           /* tracking overflowed or was off: the consumer must rebuild everything */
    uint64_t from_epoch; /* the epoch the previous take left the consumer at */
    uint64_t to_epoch;   /* the epoch now */
    const embi_style_change *changes;
    uint32_t nchanges;
    const embi_style_origin *origins;
    uint32_t norigins;
} embi_style_delta;

/* Starts or stops recording the change set of a document (see above). Off by default. */
void embi_style_track(embi_style_engine *e, embi_document *doc, bool on);
/* Resolves the document, then hands out and clears the change set recorded since the last take. The arrays are valid until
 * the next call. A document the engine does not know gives lost = true. */
void embi_style_take_delta(embi_style_engine *e, embi_document *doc, embi_style_delta *out);
/* Releases the old styles of the changes handed out before (the consumer's caches no longer hold them). */
void embi_style_release_grave(embi_style_engine *e, embi_document *doc);
/* W6e: releases the styles of the grave that referenced(user, style) does not claim, and keeps the others in the grave (a box
 * the update did not refresh still holds one). Returns the number kept. */
uint32_t embi_style_release_grave_unreferenced(embi_style_engine *e, embi_document *doc,
                                               bool (*referenced)(void *user, const embi_style *s), void *user);
/* How many old styles the grave holds. */
uint32_t embi_style_grave_count(const embi_style_engine *e, const embi_document *doc);
/* The i-th old style of the grave (i < embi_style_grave_count), borrowed. */
const embi_style *embi_style_grave_at(const embi_style_engine *e, const embi_document *doc, uint32_t i);
/* The number of elements whose style was recomputed by the last resolve that did work (a counter for tests and benches). */
uint32_t embi_style_restyled_count(const embi_style_engine *e, const embi_document *doc);

typedef struct embi_style_restyle_stats {
    uint32_t last_restyled;        /* elements restyled by the last resolve */
    uint64_t total_restyled;       /* since the document was attached */
    uint64_t partial_passes, full_passes;
    uint64_t skipped;              /* resolves that found nothing to do for a marked document */
    uint32_t last_layout, last_paint, last_refresh; /* class counts of the last delta */
} embi_style_restyle_stats;
void embi_style_restyle_stats_get(const embi_style_engine *e, const embi_document *doc, embi_style_restyle_stats *out);

/* The classification of the change from old to new style of one element (EMBI_SCH_*). */
uint8_t embi_style_classify(const embi_style *old_style, const embi_style *new_style);
/* The same, with the crossings named by in_place counted as paint changes (W6e): opacity across 1, and transform, translate,
 * rotate or scale across none. They flip the stacking-context and containing-block flags of the element's fragment, which
 * layout redoes in place. */
#define EMBI_INPLACE_OPACITY 1u
#define EMBI_INPLACE_TRANSFORM 2u
uint8_t embi_style_classify_in_place(const embi_style *old_style, const embi_style *new_style, uint32_t in_place);

/* The verification mode (EMBER_VERIFY_INCREMENTAL): 0 off, 1 on (a mismatch logs and aborts), 2 on (a mismatch is only
 * counted, for tests). The environment variable sets it to 1 on first use when it is "1". */
void embi_incr_set_verify(int mode);
int embi_incr_verify_mode(void);
/* Counts a mismatch found by a verification (logs it; aborts in mode 1). */
void embi_incr_verify_fail(const char *what);
uint32_t embi_incr_verify_failures(void);

/* ------------------------------------------------------------------ animation hooks (W6c) */

/* The animation layer (src/anim/animator.c) overlays animated values on the computed style of an element. apply is called
 * by the resolve for every element whose base style was just computed (cascade or sharing cache), before the style is stored
 * in the slot and before its children and pseudo-elements are computed from it. It takes over the reference to base and
 * returns an owned style: base itself, or an overlay built with embi_style_overlay. old_slot is what the slot held before
 * (nullptr the first time), parent the final style of the parent (the initial style for the root). Inside apply the
 * embi_style_anim_* helpers below can be used. gone is called when an element leaves the document. */
typedef struct embi_style_anim_hooks {
    void *user;
    const embi_style *(*apply)(void *user, embi_style_engine *e, embi_element *el, const embi_style *old_slot,
                               const embi_style *base, const embi_style *parent, bool is_root);
    void (*gone)(void *user, embi_element *el);
} embi_style_anim_hooks;
/* Installs (or, with nullptr, removes) the hooks of the engine. The engine restyles everything on the next resolve. */
void embi_style_set_anim_hooks(embi_style_engine *e, const embi_style_anim_hooks *h);
/* Marks el for a restyle of its own style at the next resolve (an animation moved), and the document dirty. */
void embi_style_mark_element(embi_style_engine *e, embi_element *el);
/* W8b (state_mark.c): the checkedness of a checkbox or radio or the selectedness of an option el changed without a DOM
 * change: el and what a :checked rule reaches from it are restyled at the next resolve, and the document is marked dirty. */
void embi_style_mark_state(embi_style_engine *e, embi_element *el);
/* CF1: the user-action state pc (EMBI_PC_HOVER, _ACTIVE, _FOCUS, _FOCUS_VISIBLE or _FOCUS_WITHIN) of el flipped, which the
 * selector state hooks answer: el and what the rules reading pc reach from it are restyled at the next resolve (nothing when
 * no rule reads pc), and the document is marked dirty. The caller marks every element whose state flipped (for :hover,
 * :active and :focus-within, the ancestors that contain only one of the old and the new target). False for another pc; true
 * for a user-action pc even when el's document has no style state yet (nothing is resolved, so there is nothing to mark). */
bool embi_style_mark_user_state(embi_style_engine *e, embi_element *el, embi_sel_pc pc);
/* Inside apply: the cascade of el under another parent style (the rules as they are now, the parent as it was): the
 * before-change style of a transition once the parent's own change is left out. Returns an owned style (embi_style_release). */
const embi_style *embi_style_anim_cascade_under(embi_style_engine *e, embi_element *el, const embi_style *parent);
/* Inside apply: prepares the computation of values for el (base is its style, parent the parent's). */
void embi_style_anim_ctx(embi_style_engine *e, embi_element *el, const embi_style *base, const embi_style *parent,
                         bool is_root);
/* Inside apply, after embi_style_anim_ctx: the computed value of a specified value of prop for the element (relative
 * units resolved, interned: it lives as long as the engine). */
const embi_value *embi_style_anim_compute(embi_style_engine *e, embi_prop_id prop, const embi_value *specified);
/* Inside apply, after embi_style_anim_ctx: for each of the n properties (physical ids), whether a declaration of the
 * element that is !important sets it (such a declaration beats the animations). */
void embi_style_anim_important(embi_style_engine *e, const embi_prop_id *props, uint32_t n, bool *out);
/* WPTASAN: why the element that apply styles is restyled (embi_style_anim_own_change). */
enum {
    EMBI_OWN_NONE = 0, /* only because its parent's style changed or an animation marked it: its own inputs are as before */
    EMBI_OWN_SELF = 1, /* its own inputs may have changed (its rules, attributes, state, or a selector reaching it) */
    EMBI_OWN_ALL = 2,  /* a full resolve: every element's inputs may have changed (sheets, media, viewport, root font) */
};
/* Inside apply: EMBI_OWN_* for the element being styled. */
uint8_t embi_style_anim_own_change(const embi_style_engine *e);
/* Counts the resolve passes of the engine; inside apply, the number of the running one. */
uint64_t embi_style_anim_pass(const embi_style_engine *e);
/* A style equal to base except that prop[i] has the value vals[i] (physical ids; logical ones map through base's writing
 * mode and direction). The values must outlive every group made from them: the animation layer puts them in a value block
 * (below). Owned by the caller. */
embi_style *embi_style_overlay(embi_style_engine *e, const embi_style *base, const embi_prop_id *props,
                               const embi_value *const *vals, uint32_t n);
/* A reference on a style: the animation layer keeps the before-change style of an element (embi_style_release, above,
 * drops it). */
void embi_style_retain(const embi_style *s);
/* CF2: the identity of the interned group that holds prop's computed value in s (nullptr for a property that is not
 * stored, or no style). Two styles with the same key have the same values for every property of that group. */
const void *embi_style_group_key(const embi_style *s, embi_prop_id prop);
/* ---- WPTASAN: value blocks, the lifetime rule of animated values.
 *
 * Every value the animation layer puts in a style lives in a value block, a small arena that the engine owns; a block is
 * freed only once no style group points into it. A group pins the blocks that its slots point into when it is created,
 * whatever creates it (an overlay, a cascade that inherits an animated value from the parent, a pseudo-element, a cascade
 * under an old parent), and unpins them when it is freed. Styles hold their groups, so a style keeps its values alive for as
 * long as anyone keeps the style: a slot not restyled for many frames, the sharing cache, the change tracking, a layout
 * grave, the animator's kept base and parent styles. Nothing in a block points outside it (the animation layer fills it with
 * deep copies, embi_value_copy), so pinning the block of a value keeps all of the value alive. Other value memory of the
 * engine (its arena, the sheets it retains) lives as long as the engine, so groups never point into memory that goes first.
 * Cost: one page-map lookup per slot when a group is created while blocks exist, one hash lookup when such a group is
 * freed, O(1) per block sealed and per block freed (see vblock.c); nothing while no block exists (no animation). Use: open a block, copy the values into its arena, seal it, build the
 * overlay, drop it. */
typedef struct embi_style_vblock embi_style_vblock;
/* A new block that the caller holds (block_size: the size of its arena blocks). */
embi_style_vblock *embi_style_vblock_open(embi_style_engine *e, size_t block_size);
embi_arena *embi_style_vblock_arena(embi_style_vblock *b);
/* The block's memory is complete: groups made from now on pin the block when they point into it. */
void embi_style_vblock_seal(embi_style_engine *e, embi_style_vblock *b);
/* The caller lets go: the block goes once no group pins it (at once when none does). */
void embi_style_vblock_drop(embi_style_engine *e, embi_style_vblock *b);
/* The blocks alive, and their bytes. */
uint32_t embi_style_vblock_count(const embi_style_engine *e);
size_t embi_style_vblock_bytes(const embi_style_engine *e);
/* A test hook: the sealed block whose memory contains p, or nullptr. */
const embi_style_vblock *embi_style_vblock_debug_find(const embi_style_engine *e, const void *p);
/* WPTASAN, with embi_arena_debug_poison on (tests): the number of s's values, and parts of them at any depth, that lie in
 * poisoned memory (memory given back while the style still pointed at it). */
uint32_t embi_style_debug_poisoned(const embi_style *s);
/* With poison on: the number of such slots in the styles that restyles read (each element's old style and its parent's
 * style, before apply runs), since the engine was created. */
uint32_t embi_style_debug_poison_reads(const embi_style_engine *e);
/* WPTASAN: the identity of s's custom property map (maps are interned: two styles with the same key have the same custom
 * properties). */
const void *embi_style_custom_key(const embi_style *s);
/* True when the two styles have the same computed values (and custom properties), whatever the objects. */
bool embi_style_same_content(const embi_style *a, const embi_style *b);
/* The properties (physical ids) whose computed values differ between the styles, up to cap. Returns the count. */
uint32_t embi_style_diff_props(const embi_style *a, const embi_style *b, embi_prop_id *out, uint32_t cap);
