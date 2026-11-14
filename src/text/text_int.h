#pragma once
/*
 * Private layouts and helpers shared by the text module's translation units. Only the files of
 * src/text include this file. Other modules see text.h.
 */

#include "text/text.h"

#include <SDL3/SDL_atomic.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One variation instance of a face: the user-space axis values and the normalized coordinates
 * that the outline and shaping code read. */
typedef struct embi_face_inst {
    float *user;      /* axis_count user values, fvar order */
    int16_t *coords;  /* axis_count normalized F2DOT14 values, fvar order */
} embi_face_inst;

struct embi_face {
    uint32_t uid;            /* process-unique, never 0: keys the glyph and blob caches */
    uint32_t index;          /* face index inside its file (TTC) */
    embi_font font;          /* opened face; borrows data */
    uint8_t *data;           /* the bytes font borrows */
    size_t data_len;
    char *name_buf;          /* owned family override, or nullptr */
    embi_str family;         /* effective family name (name ID 1, or the descriptor override) */
    embi_str typo_family;    /* typographic family (name ID 16), empty when absent */
    uint16_t weight_min, weight_max;
    uint8_t style;           /* EMBI_STYLE_* */
    float stretch_min, stretch_max;
    embi_unicode_range *ranges;
    uint32_t range_count;
    int32_t ax_wght, ax_wdth, ax_slnt, ax_ital, ax_opsz; /* axis index, or -1 */
    embi_face_inst *insts;
    uint32_t inst_count, inst_cap;
    void *kb;                /* kbts_font, created on first shaping */
    bool kb_failed;
    int8_t smcp;             /* the face maps 'a' to another glyph under smcp: 1 yes, -1 no, 0 not probed (shape.c) */
    uint32_t refs;           /* shaped layouts whose runs use this face (shape.c); a face with refs is kept */
    uint8_t *own;           /* file bytes a system face owns (SDL memory), or nullptr */
    uint32_t sys_ent1;       /* index + 1 of the system entry this face was loaded from; 0 for registered faces */
    bool color;              /* COLR, CBDT or sbix: a colour face (emoji presentation prefers it) */
    bool superseded;         /* a later @font-face source whose earlier local() source resolved since: never matched */
};

/* A local() source of @font-face that was looked up before the system index was ready (fontdb.c). It is resolved when a
 * family lookup names its family once the index is ready. */
typedef struct embi_lpend {
    char *name;              /* the local() name (owned, NUL-terminated) */
    char *family;            /* the @font-face family (owned, NUL-terminated, may be empty) */
    embi_unicode_range *ranges; /* owned copy of the unicode-range */
    embi_face_desc desc;     /* the descriptors; desc.family and desc.ranges point at the owned copies */
    uint32_t group;          /* the sources of one rule share a group, in source order */
    uint32_t alt_first, alt_n; /* faces (db->faces indexes) a later source of the rule added: superseded on a hit */
    uint8_t state;           /* EMBI_LP_* */
} embi_lpend;

enum { EMBI_LP_WAITING, EMBI_LP_RESOLVED, EMBI_LP_DEAD };

/* CSS generic families (CSS Fonts 4 section 3.2 and the ui-* keywords). The order is the enum's. */
enum {
    EMBI_GEN_SERIF,
    EMBI_GEN_SANS_SERIF,
    EMBI_GEN_MONOSPACE,
    EMBI_GEN_CURSIVE,
    EMBI_GEN_FANTASY,
    EMBI_GEN_SYSTEM_UI,
    EMBI_GEN_UI_SERIF,
    EMBI_GEN_UI_SANS_SERIF,
    EMBI_GEN_UI_MONOSPACE,
    EMBI_GEN_UI_ROUNDED,
    EMBI_GEN_EMOJI,
    EMBI_GEN_MATH,
    EMBI_GEN_FANGSONG,
    EMBI_GEN_COUNT,
};

typedef struct embi_sysindex embi_sysindex;

struct embi_fontdb {
    EMBI_VEC(embi_face *) faces;
    EMBI_VEC(uint8_t *) blobs; /* owned file bytes (SDL_malloc), freed with SDL_free */
    embi_face *default_face;
    char **fallback;         /* owned fallback family names */
    uint32_t fallback_count;
    embi_sysindex *sys;      /* the system font index (sysfont.c); nullptr when system fonts are off */
    char **gen_over[EMBI_GEN_COUNT];  /* embedder lists for a generic family (owned), replacing the platform list */
    uint32_t gen_over_n[EMBI_GEN_COUNT];
    embi_hashmap cover;      /* per-character fallback cache: key -> embi_face * (0 when no face covers it) */
    uint64_t fallback_uses;  /* the lookups that resolved a family or a code point through a fallback */
    EMBI_VEC(embi_lpend) lpend; /* local() sources seen before the index was ready */
    uint32_t lp_open;        /* lpend[lp_open..] belong to a rule whose later sources may still register */
    uint32_t lp_group;       /* the last group id */
    uint32_t lp_waiting;     /* records still EMBI_LP_WAITING */
    uint32_t *scratch;       /* per-character fallback: the candidate entries (match.c), kept between lookups */
    uint32_t scratch_cap;
    void (*warn_fn)(void *user, const char *msg); /* SZ3b: where the no-font warning goes (the runtime's log), or nullptr */
    void *warn_user;
    SDL_AtomicInt warned; /* the warning was given */
};

struct embi_text {
    embi_fontdb *db;
    void *shape;   /* shape.c state: kb storage, shaping cache */
    void *outline; /* draw.c state: glyph outline cache */
    embi_text_bitmap_fn bitmap_fn; /* draw.c: the sink of bitmap colour glyphs (nullptr: they are not drawn) */
    void *bitmap_user;
    void *bitmap;  /* draw.c: decoded bitmap glyph cache */
};

/* ------------------------------------------------------------------ fontdb.c */

/* A variation instance id of the face for user values (one per fvar axis). Adds it if new.
 * Returns 0 for a static face. */
uint32_t embi_face_instance(embi_face *f, const float *user);
const float *embi_face_inst_user(const embi_face *f, uint32_t inst);
const int16_t *embi_face_inst_coords(const embi_face *f, uint32_t inst);
/* Frees the shaping data of a face (kb font). Called before embi_face is freed. */
void embi_shape_face_release(embi_face *f);
/* Drops the shaping configs and the cached shapes of one face (by uid) before the face is freed (trim). */
void embi_shape_forget_face(embi_text *t, uint32_t uid);

/* ------------------------------------------------------------------ fontdb.c: system faces */

/* Opens face `index` of a container the face takes ownership of (SDL memory, freed with the face). WOFF and WOFF2 are
 * decoded first. Returns nullptr on failure, after which the bytes are freed. */
embi_face *embi_face_open_owned(uint8_t *bytes, size_t len, uint32_t index, const embi_face_desc *desc);
/* Frees a face that is not registered (a system face). Its layouts must be gone (refs == 0). */
void embi_face_release(embi_face *f);
/* Frees the loaded system faces that no shaped layout uses. t's shaping state forgets them first. Returns the count. */
uint32_t embi_fontdb_trim_system(embi_fontdb *db, embi_text *t);
/* The face of system entry i, loaded now when needed (nullptr when its file cannot be read or opened). */
embi_face *embi_fontdb_system_face(embi_fontdb *db, uint32_t i);

/* ------------------------------------------------------------------ sysfont.c: the system font index */


/* Start-up options. dirs == nullptr: the platform's font directories (or the embedder's test directories). */
typedef struct embi_sys_opts {
    const char *const *dirs;
    uint32_t ndirs;
    bool cache;        /* keep the index in SDL user storage (org "ember", app "fontcache") */
    bool sync;         /* build the index on the calling thread (tests) */
    uint32_t delay_ms; /* test hook: the job waits this long before it starts */
    const char *cache_name; /* test hook: the cache file name in the user storage (nullptr: the default) */
} embi_sys_opts;

typedef struct embi_sys_stats {
    bool ready;
    bool warm;             /* the cache was read (the entries of unchanged files were not parsed) */
    uint32_t files;        /* font files found */
    uint32_t entries;      /* faces indexed */
    uint32_t reparsed;     /* files parsed in this build (cold: all of them) */
    uint32_t faces_loaded; /* system faces whose bytes are loaded now */
    uint64_t font_bytes;   /* bytes of the loaded system faces */
    uint64_t index_bytes;  /* memory of the index itself */
    double index_ms;       /* wall time of the index build */
    uint64_t epoch;        /* counts the index builds that finished (1 once ready, 0 before) */
} embi_sys_stats;

/* Test hook: the directories that every later index scan reads instead of the platform's (a copy of the list; n 0 or
 * dirs nullptr restores the platform's), and how long the index job waits before it starts. Such an index keeps its
 * cache in its own file. Not thread-safe: call it before the runtime or the registry is created. */
void embi_sysfont_set_dirs(const char *const *dirs, uint32_t n, uint32_t delay_ms);
/* Starts the index: a job thread, or the calling thread with opts->sync. Returns false when no index is kept. */
bool embi_sys_start(embi_fontdb *db, const embi_sys_opts *opts);
/* Cancels the job, waits for it and frees the index. Safe with no index. */
void embi_sys_stop(embi_fontdb *db);
bool embi_sys_ready(const embi_fontdb *db);
void embi_sys_stats_get(const embi_fontdb *db, embi_sys_stats *out);

/* Metadata of a face for the matching rank (match.c). */
typedef struct embi_fmeta {
    float wlo, whi; /* weight range */
    float slo, shi; /* stretch range, percent */
    uint8_t style;  /* EMBI_STYLE_* */
} embi_fmeta;

uint32_t embi_sys_count(const embi_sysindex *x);
bool embi_sys_meta(const embi_sysindex *x, uint32_t i, embi_fmeta *m, bool *color);
/* True when the 256-code-point block of cp has its bit set: a superset of the faces that cover cp. */
bool embi_sys_block(const embi_sysindex *x, uint32_t i, uint32_t cp);
/* Entries whose typographic family, then legacy family, then localized family equals name (the first tier with a
 * match wins). Writes up to cap entries, returns the count. */
uint32_t embi_sys_family(const embi_sysindex *x, embi_str name, uint32_t *out, uint32_t cap);
/* Entries whose full name or PostScript name equals name (ASCII case-insensitive). */
uint32_t embi_sys_local(const embi_sysindex *x, embi_str name, uint32_t *out, uint32_t cap);
/* The path and the face index of entry i. */
const char *embi_sys_path(const embi_sysindex *x, uint32_t i, uint32_t *face_index);
/* Reads the whole file of entry i into SDL memory (the caller frees it with SDL_free). */
bool embi_sys_read(const embi_sysindex *x, uint32_t i, uint8_t **data, size_t *len);
/* The face slot of entry i: the loaded face or nullptr. */
embi_face **embi_sys_face_slot(embi_sysindex *x, uint32_t i);
/* Accounting of the loaded system faces (fontdb.c). */
void embi_sys_loaded(embi_sysindex *x, int32_t faces, int64_t bytes);
/* A system entry whose file failed to read or open is remembered, so a lookup does not read it again. */
bool embi_sys_bad(const embi_sysindex *x, uint32_t i);
void embi_sys_mark_bad(embi_sysindex *x, uint32_t i);

/* The generic family of a CSS keyword (-1 when name is not one). */
int embi_gen_index(embi_str name);
/* The platform's list of a generic family: nullptr-terminated. */
const char *const *embi_sys_generic_default(uint32_t gen);
/* The preferred families of the script of cp for lang (nullptr-terminated, possibly empty). */
const char *const *embi_sys_script_prefs(uint32_t cp, embi_str lang);

/* ------------------------------------------------------------------ match.c: families and fallback */

/* The face of a family name for a style: the registered faces first (typographic, then legacy, then localized name),
 * then the system index once it is ready. nullptr when neither has the family. */
embi_face *embi_fontdb_resolve_family(embi_fontdb *db, embi_str name, const embi_style_match *m);
/* Per-character fallback for code point cp: the generic family list, the script preferences, then any system face
 * that covers cp (closest weight, style and stretch first). nullptr when none does, and the caller uses the bundled
 * default. lang is the BCP 47 tag of the text (may be empty). Cached per (cp, style, lang). */
embi_face *embi_fontdb_cover_face(embi_fontdb *db, uint32_t cp, const embi_style_match *m, embi_str lang);
/* Lookups that used a fallback so far. A layout that reads a larger count than before knows it needs a restyle. */
uint64_t embi_fontdb_fallback_uses(const embi_fontdb *db);
/* Drops the cached per-character lookups (after a trim, which frees faces they point at). */
void embi_fontdb_cover_clear(embi_fontdb *db);
/* The index epoch (0 without an index): it changes when the index arrives. */
uint64_t embi_fontdb_sys_epoch(const embi_fontdb *db);
/* A copy in SDL memory of the bytes of the system font whose full or PostScript name is name (local()). A miss while
 * the index is not ready. */
bool embi_fontdb_local(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len);
/* The local() source of @font-face. Once the index is ready it is embi_fontdb_local. Before that (with an index being
 * built) it returns a small placeholder blob: embi_fontdb_add records it as a pending local() source of the face's
 * descriptors, adds no face (returns 0, so the rule's next source is tried), and the source is resolved when a family
 * lookup names the family after the index arrives. A later source of the same rule that registers in the meantime is
 * superseded by the local face if the name resolves. False without an index. */
bool embi_fontdb_local_source(embi_fontdb *db, embi_str name, uint8_t **data, size_t *len);
/* The pending local() sources of family: resolved now when the index is ready (false); true when some are waiting for
 * the index (the family's answer can change when it arrives). */
bool embi_fontdb_local_settle(embi_fontdb *db, embi_str family);
/* Whether the system index may cover cp in entry i, read from the entry's cmap only (no whole-file load): 0 no, 1 yes,
 * -1 unknown (WOFF, or the table could not be read). */
int embi_sys_probe(const embi_sysindex *x, uint32_t i, uint32_t cp);
/* Replaces the platform list of a generic family (count 0 removes the override). */
bool embi_fontdb_set_generic(embi_fontdb *db, uint32_t gen, const embi_str *families, uint32_t count);

/* ------------------------------------------------------------------ match.c */

/* The variation instance user values a style asks for (wght, wdth, ital, slnt, opsz from the
 * style, then font-variation-settings overrides). Writes axis_count values into user. */
void embi_face_user_coords(const embi_face *f, const embi_text_style *st, float *user);

/* ------------------------------------------------------------------ bidi.c */

typedef struct embi_bidi_run {
    uint32_t begin, len;
    uint8_t level;
} embi_bidi_run;
typedef EMBI_VEC(embi_bidi_run) embi_bidi_run_vec;

/* Replaces the default allocator of SheenBidi with an SDL-backed one. Thread-safe, runs once. */
void embi_bidi_init(void);

/* Splits text into paragraphs (SheenBidi) and resolves the level of every byte (levels receives
 * len bytes). dir is EMBI_DIR_*. When any level or paragraph level is non-zero, *cache receives
 * the line-order cache (one SheenBidi paragraph per entry of paras); otherwise it is nullptr and
 * lines are already in visual order. Free the cache with embi_bidi_cache_free. */
bool embi_bidi_analyze(const char *text, uint32_t len, uint8_t dir, bool after, embi_para_vec *paras,
                       uint8_t *levels, embi_bidi_cache **cache);
void embi_bidi_cache_free(embi_bidi_cache *c);
/* The visual runs of [start, end) inside paragraph para_index (UAX #9 L2), left to right. */
bool embi_bidi_line_runs(const embi_bidi_cache *c, uint32_t para_index, uint32_t start,
                         uint32_t end, embi_bidi_run_vec *runs);

/* ------------------------------------------------------------------ shape.c */

bool embi_shape_init(embi_text *t);
void embi_shape_fini(embi_text *t);
void embi_shape_set_budget(embi_text *t, size_t bytes);
void embi_shape_clear(embi_text *t);
/* Cache counters for tests: hits, misses and the bytes the cache holds. */
/* Test hooks: the break flags (kb's KBTS_BREAK_FLAG_* bits) and scripts per byte, from the production loop and from
 * kbts_BreakEntireStringUtf8. Outputs hold len + 1 entries. */
void embi_shape_breaks(const char *text, uint32_t len, uint8_t dir, uint8_t line_break,
                       uint32_t *out_flags, uint32_t *out_scripts);
void embi_shape_breaks_reference(const char *text, uint32_t len, uint8_t dir, uint8_t line_break,
                                 uint32_t *out_flags, uint32_t *out_scripts);
void embi_shape_stats(const embi_text *t, uint64_t *hits, uint64_t *misses, size_t *bytes);

/* Glyph outline cache, draw.c. */
void embi_outline_fini(embi_text *t);
void embi_outline_clear(embi_text *t);
