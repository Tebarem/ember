#pragma once
/*
 * Selectors Level 4 (W2b): parsing, specificity, matching, rule buckets and the ancestor bloom
 * filter. Internal to Ember.
 *
 * Parsing takes component values (a style rule prelude from an embi_css_sheet). Compiled selectors
 * live in the arena the caller passes in and are immutable afterwards, so one compiled list can be
 * shared by every document that uses the sheet.
 *
 * Matching runs on src/dom elements. An embi_sel_ctx carries the :scope element, the dynamic and
 * form state hooks, and the :has() memo. The memo and the hook answers go stale when the DOM or the
 * state changes, so call embi_sel_ctx_reset after any such change.
 *
 * Bloom filter: embi_bloom counts the tag, id and class buckets of the ancestors of the element being
 * matched. The caller pushes an element when it enters it and pops it when it leaves. A top-level
 * match with a bloom rejects a complex selector whose required ancestor buckets are absent, without
 * walking the tree. Quirks mode ignores the bloom, because class and id matching is case-insensitive.
 */
#include "css/css.h"
#include "dom/dom.h"

/* ------------------------------------------------------------------ names and enums */

typedef enum embi_sel_pseudo {
    EMBI_PE_NONE = 0,
    EMBI_PE_BEFORE,
    EMBI_PE_AFTER,
    EMBI_PE_MARKER,
    EMBI_PE_PLACEHOLDER,
    EMBI_PE_SELECTION,
    EMBI_PE_BACKDROP,
    EMBI_PE_FIRST_LINE,
    EMBI_PE_FIRST_LETTER,
    EMBI_PE_FILE_SELECTOR_BUTTON,
} embi_sel_pseudo;

/* Pseudo-classes. The state ones (EMBI_PC_HOVER up to EMBI_PC_OUT_OF_RANGE) are answered by the
 * state hook when one is supplied. */
typedef enum embi_sel_pc {
    EMBI_PC_ROOT,
    EMBI_PC_EMPTY,
    EMBI_PC_FIRST_CHILD,
    EMBI_PC_LAST_CHILD,
    EMBI_PC_ONLY_CHILD,
    EMBI_PC_FIRST_OF_TYPE,
    EMBI_PC_LAST_OF_TYPE,
    EMBI_PC_ONLY_OF_TYPE,
    EMBI_PC_SCOPE,
    EMBI_PC_LINK,    /* :link and :any-link */
    EMBI_PC_VISITED, /* never matches */
    EMBI_PC_HOVER,
    EMBI_PC_ACTIVE,
    EMBI_PC_FOCUS,
    EMBI_PC_FOCUS_VISIBLE,
    EMBI_PC_FOCUS_WITHIN,
    EMBI_PC_TARGET,
    EMBI_PC_MODAL,
    EMBI_PC_POPOVER_OPEN,
    EMBI_PC_OPEN,
    EMBI_PC_DEFINED,
    EMBI_PC_ENABLED,
    EMBI_PC_DISABLED,
    EMBI_PC_CHECKED,
    EMBI_PC_INDETERMINATE,
    EMBI_PC_REQUIRED,
    EMBI_PC_OPTIONAL,
    EMBI_PC_READ_ONLY,
    EMBI_PC_READ_WRITE,
    EMBI_PC_PLACEHOLDER_SHOWN,
    EMBI_PC_DEFAULT,
    EMBI_PC_VALID,
    EMBI_PC_INVALID,
    EMBI_PC_IN_RANGE,
    EMBI_PC_OUT_OF_RANGE,
} embi_sel_pc;

typedef enum embi_sel_kind {
    EMBI_SS_TYPE = 1, /* name (as written), lname (ASCII lowercase), nsk, ns_uri */
    EMBI_SS_UNIVERSAL,
    EMBI_SS_ID,    /* name */
    EMBI_SS_CLASS, /* name */
    EMBI_SS_ATTR,  /* name, lname, nsk, ns_uri, sub (op), value, flags */
    EMBI_SS_PC,    /* sub: embi_sel_pc */
    EMBI_SS_NTH,   /* a, b, flags (EMBI_SNF_*), list (of S, may be nullptr) */
    EMBI_SS_IS,    /* list (forgiving). Also CSS Nesting's & when a parent list is given. */
    EMBI_SS_WHERE, /* list (forgiving) */
    EMBI_SS_NOT,   /* list */
    EMBI_SS_HAS,   /* list of relative complex selectors */
    EMBI_SS_LANG,  /* strs, nstrs */
    EMBI_SS_DIR,   /* sub: EMBI_DIR_* */
} embi_sel_kind;

/* Namespace constraint of type and attribute selectors. */
typedef enum embi_sel_nsk {
    EMBI_NSK_ANY = 0, /* no constraint (*|x) */
    EMBI_NSK_NONE,    /* no namespace (|x, or an unprefixed attribute name) */
    EMBI_NSK_URI,     /* the namespace whose URI is ns_uri */
} embi_sel_nsk;

typedef enum embi_sel_attr_op {
    EMBI_AO_EXISTS = 0,
    EMBI_AO_EQ,     /* = */
    EMBI_AO_WORD,   /* ~= */
    EMBI_AO_DASH,   /* |= */
    EMBI_AO_PREFIX, /* ^= */
    EMBI_AO_SUFFIX, /* $= */
    EMBI_AO_SUBSTR, /* *= */
} embi_sel_attr_op;

/* flags of attribute selectors */
#define EMBI_SAF_CI 0x01u      /* i flag */
#define EMBI_SAF_CS 0x02u      /* s flag */
#define EMBI_SAF_HTML_CI 0x04u /* the attribute name is case-insensitive on HTML elements */

/* flags of nth selectors */
#define EMBI_SNF_FROM_END 0x01u
#define EMBI_SNF_OF_TYPE 0x02u

typedef enum embi_sel_dir {
    EMBI_DIR_LTR = 1,
    EMBI_DIR_RTL = 2,
} embi_sel_dir;

/* Combinators. comb[i] of a complex selector relates compound i-1 to compound i. */
typedef enum embi_sel_comb {
    EMBI_SC_NONE = 0,
    EMBI_SC_DESC,   /* whitespace */
    EMBI_SC_CHILD,  /* > */
    EMBI_SC_NEXT,   /* + */
    EMBI_SC_SUBSEQ, /* ~ */
} embi_sel_comb;

/* ------------------------------------------------------------------ compiled selectors */

typedef struct embi_sel_spec {
    uint32_t a; /* ids */
    uint32_t b; /* classes, attributes, pseudo-classes */
    uint32_t c; /* types, pseudo-elements */
} embi_sel_spec;

/* Negative, zero or positive, comparing a, then b, then c. */
int embi_sel_spec_cmp(embi_sel_spec x, embi_sel_spec y);

typedef struct embi_sel_list embi_sel_list;

/* 40 bytes. The per-kind payload is a union: only the member of the kind is meaningful. */
typedef struct embi_sel_simple {
    uint8_t kind;  /* embi_sel_kind */
    uint8_t sub;   /* PC: embi_sel_pc. ATTR: embi_sel_attr_op. DIR: embi_sel_dir. */
    uint8_t nsk;   /* TYPE, UNIVERSAL, ATTR: embi_sel_nsk */
    uint8_t flags; /* ATTR: EMBI_SAF_*. NTH: EMBI_SNF_*. */
    embi_atom name;
    embi_atom lname;
    embi_atom ns_uri;
    union {
        struct {            /* NTH: An+B and the optional of S list. IS/WHERE/NOT/HAS: list. */
            int64_t a, b;
            const embi_sel_list *list;
        };
        embi_str value;     /* ATTR: the value, UTF-8 */
        struct {            /* LANG: the ranges */
            const embi_str *strs;
            uint32_t nstrs;
        };
    };
} embi_sel_simple;

typedef struct embi_sel_compound {
    const embi_sel_simple *ss;
    uint32_t n;
} embi_sel_compound;

typedef struct embi_sel_complex {
    embi_sel_spec spec;
    uint32_t nc;                       /* compounds, left to right; at least 1 */
    const embi_sel_compound *comp;
    const uint8_t *comb;               /* embi_sel_comb per compound; comb[0] is the leading one */
    uint8_t pseudo;                    /* embi_sel_pseudo of the subject, EMBI_PE_NONE if none */
    uint32_t nreq;
    const uint32_t *req;               /* bloom buckets that the strict ancestors must provide */
} embi_sel_complex;

struct embi_sel_list {
    uint32_t n;
    const embi_sel_complex *items;
    bool relative; /* :has() and nested selectors: each item may lead with a combinator */
};

/* Namespace prefixes in scope. prefix 0 is the default namespace (an @namespace rule without a
 * prefix). uri is an interned atom of the namespace URI. */
typedef struct embi_sel_nsmap {
    embi_atom prefix;
    embi_atom uri;
} embi_sel_nsmap;

typedef struct embi_sel_env {
    const embi_sel_nsmap *ns;
    uint32_t nns;
    /* The parent selector list for CSS Nesting's &. nullptr makes & mean :scope. */
    const embi_sel_list *nest_parent;
} embi_sel_env;

/* Parses a selector list (unforgiving: one invalid item invalidates the list). Returns nullptr when
 * the list is invalid. env may be nullptr (no namespaces, no nesting). */
const embi_sel_list *embi_sel_parse(embi_arena *a, embi_css_slice in, const embi_sel_env *env);
/* Parses a relative selector list: each item may start with a combinator (default: descendant).
 * This is the argument syntax of :has(). */
const embi_sel_list *embi_sel_parse_relative(embi_arena *a, embi_css_slice in,
                                             const embi_sel_env *env);
/* Convenience: tokenizes text and parses it as a selector list. */
const embi_sel_list *embi_sel_parse_text(embi_arena *a, embi_str text, const embi_sel_env *env);

/* Parses an <an+b> over component values (surrounding whitespace allowed). Returns false when the
 * input is not an An+B. */
bool embi_sel_parse_anb(embi_css_slice in, int64_t *a, int64_t *b);

/* Specificity of a complex selector (the one the parser computed). */
static inline embi_sel_spec embi_sel_specificity(const embi_sel_complex *c)
{
    return c->spec;
}

/* ASCII lowercase of an atom (the atom itself when it has no uppercase letters). */
embi_atom embi_sel_lower_atom(embi_atom a);
/* Drops the per-thread lowercase cache. Called by parsing, embi_bloom_clear and embi_sel_ctx_init;
 * call it after embi_atom_shutdown if atoms were rebuilt while this thread kept running. */
void embi_sel_lower_cache_reset(void);

/* ------------------------------------------------------------------ bloom filter */

#define EMBI_BLOOM_SIZE 256

/* Counting filter over ancestor buckets. Counts saturate at 255 and then stay there, so a saturated
 * bucket can give a false positive but never a false negative. Zeroed is empty. */
typedef struct embi_bloom {
    uint8_t count[EMBI_BLOOM_SIZE];
} embi_bloom;

uint32_t embi_bloom_hash_atom(embi_atom a);
void embi_bloom_clear(embi_bloom *b);
/* Adds the element's tag (exact and lowercase), id and classes. */
void embi_bloom_push(embi_bloom *b, embi_element *el);
/* Removes what embi_bloom_push added for the same element. */
void embi_bloom_pop(embi_bloom *b, embi_element *el);
bool embi_bloom_test(const embi_bloom *b, uint32_t bucket);

/* ------------------------------------------------------------------ matching */

/* Answers the state pseudo-classes for el (see EMBI_PC_HOVER..EMBI_PC_OUT_OF_RANGE). With a hook,
 * the hook answers all of them. Without one, the attribute-based answers apply (for example
 * :checked from the checked attribute), and the rest never match. */
typedef struct embi_sel_hooks {
    void *user;
    bool (*state)(void *user, embi_element *el, embi_sel_pc pc);
} embi_sel_hooks;

/* W8b: the checkedness of a checkbox or radio input and the selectedness of an option live in the form controls of the
 * bindings, not in the DOM (the checked and selected attributes are only their defaults). A provider returns 1 or 0 for
 * :checked of such an element, or -1 to leave it to the attributes. Process-wide; nullptr (the default) reads the
 * attributes. Used with or without a state hook, so script matching (matches, querySelector) and the cascade agree. */
typedef int (*embi_sel_checked_fn)(const embi_element *el);
void embi_sel_set_checked_fn(embi_sel_checked_fn fn);

typedef struct embi_sel_ctx {
    embi_element *scope;         /* :scope. nullptr means the document element. */
    const embi_sel_hooks *hooks; /* may be nullptr */
    embi_hashmap has_memo;       /* internal: element -> chain of :has() results */
} embi_sel_ctx;

void embi_sel_ctx_init(embi_sel_ctx *ctx, embi_element *scope, const embi_sel_hooks *hooks);
/* Drops the :has() memo. Call after any DOM mutation or state change. */
void embi_sel_ctx_reset(embi_sel_ctx *ctx);
void embi_sel_ctx_free(embi_sel_ctx *ctx);

/* Matches one complex selector with el as its subject. pe selects the pseudo-element: the item only
 * matches when its own pseudo-element is pe (EMBI_PE_NONE for the element itself). anc is the bloom
 * of el's strict ancestors, or nullptr to skip the bloom. */
bool embi_sel_match(const embi_sel_complex *c, embi_element *el, embi_sel_pseudo pe,
                    embi_sel_ctx *ctx, const embi_bloom *anc);
/* True when el matches any item of a (non-relative) list. */
bool embi_sel_match_list(const embi_sel_list *l, embi_element *el, embi_sel_ctx *ctx);

/* ------------------------------------------------------------------ rule buckets */

typedef struct embi_sel_bucket {
    uint64_t *items;
    uint32_t len;
    uint32_t cap;
} embi_sel_bucket;

/* Buckets keyed by the subject compound's rightmost id, class, type, attribute name, or universal.
 * Each payload is opaque to this module (W2d stores a rule index). Zeroed is empty. */
typedef struct embi_sel_index {
    embi_hashmap ids;     /* lowercase id atom -> embi_sel_bucket* */
    embi_hashmap classes; /* lowercase class atom -> embi_sel_bucket* */
    embi_hashmap tags;    /* lowercase type atom -> embi_sel_bucket* */
    embi_hashmap attrs;   /* lowercase attribute name atom -> embi_sel_bucket* */
    embi_sel_bucket universal;
} embi_sel_index;

void embi_sel_index_add(embi_sel_index *ix, const embi_sel_complex *c, uint64_t payload);
typedef void (*embi_sel_visit_fn)(void *user, uint64_t payload);
/* Calls fn once for each payload whose key can match el. The caller still runs embi_sel_match. */
void embi_sel_index_candidates(const embi_sel_index *ix, embi_element *el, embi_sel_visit_fn fn,
                               void *user);
void embi_sel_index_free(embi_sel_index *ix);
