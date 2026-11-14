#pragma once
/*
 * CSS syntax (CSS Syntax Level 3) and the stylesheet model (W2a). Internal to Ember; the style
 * engine (src/style) and the CSSOM (W5) build on these types.
 *
 * Ownership: syntax results live in the arena the caller passes in and stay valid until that arena
 * is reset or freed. A stylesheet owns its arena, is refcounted, and is immutable after parse.
 *
 * Offsets (start/end) index the preprocessed text, not the original input: CR, CRLF and FF become
 * LF, NUL and surrogate code points become U+FFFD, and invalid UTF-8 becomes U+FFFD.
 *
 * Seams for later tasks: declarations keep their values as component-value slices. The property
 * database (W2c) plugs in through embi_css_hooks, which validates declarations and evaluates
 * @supports selector() conditions. Shorthand expansion and value parsing are not done here.
 */
#include "base/base.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Inputs must be smaller than 4 GiB. Larger ones are rejected (empty tokens, or unsupported bytes),
 * because offsets and lengths are 32-bit. */

/* ------------------------------------------------------------------ tokens */

typedef enum embi_css_type {
    EMBI_CSS_WS,
    EMBI_CSS_IDENT,
    EMBI_CSS_FUNCTION,
    EMBI_CSS_AT_KEYWORD,
    EMBI_CSS_HASH,
    EMBI_CSS_STRING,
    EMBI_CSS_BAD_STRING,
    EMBI_CSS_URL,
    EMBI_CSS_BAD_URL,
    EMBI_CSS_DELIM,
    EMBI_CSS_NUMBER,
    EMBI_CSS_PERCENTAGE,
    EMBI_CSS_DIMENSION,
    EMBI_CSS_UNICODE_RANGE,
    EMBI_CSS_INCLUDE_MATCH, /* ~= */
    EMBI_CSS_DASH_MATCH,    /* |= */
    EMBI_CSS_PREFIX_MATCH,  /* ^= */
    EMBI_CSS_SUFFIX_MATCH,  /* $= */
    EMBI_CSS_SUBSTRING_MATCH, /* *= */
    EMBI_CSS_COLUMN,        /* || */
    EMBI_CSS_CDO,           /* <!-- */
    EMBI_CSS_CDC,           /* --> */
    EMBI_CSS_COLON,
    EMBI_CSS_SEMICOLON,
    EMBI_CSS_COMMA,
    EMBI_CSS_LBRACKET,
    EMBI_CSS_RBRACKET,
    EMBI_CSS_LPAREN,
    EMBI_CSS_RPAREN,
    EMBI_CSS_LBRACE,
    EMBI_CSS_RBRACE,
} embi_css_type;

typedef struct embi_css_token {
    uint8_t type;        /* embi_css_type */
    bool flag;           /* HASH: id-type. NUMBER, PERCENTAGE, DIMENSION: integer.
                          * STRING: EOF ended it (eof-in-string). URL: EOF ended it (eof-in-url).
                          * The parse error itself is not a token. */
    uint32_t start, end; /* offsets into the preprocessed text */
    embi_str value;      /* IDENT, FUNCTION, AT_KEYWORD, HASH, STRING, URL: decoded value.
                          * DIMENSION: unit. */
    embi_str repr;       /* NUMBER, PERCENTAGE, DIMENSION: source text */
    double num;          /* NUMBER, PERCENTAGE, DIMENSION: value */
    uint32_t cp;         /* DELIM: code point. UNICODE_RANGE: start. */
    uint32_t cp_end;     /* UNICODE_RANGE: end */
} embi_css_token;

typedef struct embi_css_tokens {
    const embi_css_token *p;
    uint32_t n;
} embi_css_tokens;

/* ------------------------------------------------------------------ component values */

/* A component value is a preserved token, a function, or a simple block. Blocks and functions
 * have their opening token in tok (LBRACE, LBRACKET, LPAREN, or FUNCTION) and their contents in
 * kids. Unmatched closing tokens stay as plain tokens. end is the offset just past the closing
 * token (for a block left open at EOF: the end of the last token). */
typedef struct embi_css_cv {
    embi_css_token tok;
    const struct embi_css_cv *kids;
    uint32_t nkids;
    uint32_t end;
} embi_css_cv;

/* Internal: a growable token vector. Callers free it with embi_vec_free. */
typedef EMBI_VEC(embi_css_token) embi_css_token_vec;

/* Internal: tokenizes into a heap vector. Token strings live in a. Leaves the vector empty when
 * the input is 4 GiB or more. */
void embi_css_tokenize_vec(embi_arena *a, embi_str text, embi_css_token_vec *out);

typedef struct embi_css_slice {
    const embi_css_cv *p;
    uint32_t n;
} embi_css_slice;

static inline embi_css_slice embi_css_slice_make(const embi_css_cv *p, uint32_t n)
{
    return (embi_css_slice){p, n};
}

static inline void *embi_css_arena_copy(embi_arena *a, const void *src, size_t count, size_t sz)
{
    void *p = embi_arena_alloc(a, count * sz, 8);
    if (count != 0) {
        memcpy(p, src, count * sz);
    }
    return p;
}

/* ------------------------------------------------------------------ syntax items */

typedef enum embi_css_item_kind {
    EMBI_CSS_ITEM_QUALIFIED, /* qualified rule: prelude, block */
    EMBI_CSS_ITEM_AT,        /* at-rule: name, prelude, optional block */
    EMBI_CSS_ITEM_DECL,      /* declaration: name, value (prelude), important */
    EMBI_CSS_ITEM_ERROR,     /* a rule or declaration that was dropped: error */
} embi_css_item_kind;

typedef struct embi_css_item {
    uint8_t kind;           /* embi_css_item_kind */
    bool important;         /* DECL */
    bool has_block;         /* AT */
    embi_str name;          /* AT: at-keyword value. DECL: property name as written. */
    embi_css_slice prelude; /* QUALIFIED, AT: prelude. DECL: value. */
    embi_css_slice block;   /* QUALIFIED, AT with block: block contents. */
    const char *error;      /* ERROR: reason ("invalid", "empty", "extra-input") */
    uint32_t start, end;    /* source offsets of the item: first token to end of last token */
} embi_css_item;

typedef struct embi_css_items {
    const embi_css_item *p;
    uint32_t n;
} embi_css_items;

/* ------------------------------------------------------------------ syntax entry points */

/* Tokenizes text (preprocessing it into the arena first). */
embi_css_tokens embi_css_tokenize(embi_arena *a, embi_str text);

/* Parse a list of component values: the whole text as a component value tree. */
embi_css_slice embi_css_parse_cvs(embi_arena *a, embi_str text);

/* Parse a list of rules (stylesheet when top_level, rule list otherwise). CDO and CDC are ignored
 * at top level only. */
embi_css_items embi_css_consume_rules(embi_arena *a, embi_css_slice in, bool top_level);

/* Parse a list of declarations (declaration_list): declarations and at-rules only. */
embi_css_items embi_css_consume_declarations(embi_arena *a, embi_css_slice in);

/* Parse a block's contents: declarations, at-rules and qualified rules. */
embi_css_items embi_css_consume_block_contents(embi_arena *a, embi_css_slice in);

/* Parse a single rule. Returns an ERROR item with error "empty", "invalid" or "extra-input"
 * on failure. */
embi_css_item embi_css_parse_rule(embi_arena *a, embi_css_slice in);

/* Parse a single declaration. Returns an ERROR item on failure. */
embi_css_item embi_css_parse_declaration(embi_arena *a, embi_css_slice in);

/* Parse a single component value. Returns nullptr on success and sets *out, or returns the
 * error reason ("empty" or "extra-input"). */
const char *embi_css_parse_component_value(embi_css_slice in, const embi_css_cv **out);

/* Determines the encoding of a byte stream (BOM, protocol label, @charset, environment label,
 * then UTF-8) and decodes it. *enc receives the canonical lowercase name. Returns false when the
 * encoding is not supported (only UTF-8, UTF-16LE and UTF-16BE decode) or the input is 4 GiB or
 * more. The UTF-8 text may alias the input bytes; UTF-16 text is copied into the arena. */
bool embi_css_decode_bytes(embi_arena *a, const unsigned char *p, size_t n, embi_str protocol,
                           embi_str env, embi_str *text, const char **enc);

/* ------------------------------------------------------------------ stylesheet model */

typedef struct embi_css_sheet embi_css_sheet;

typedef enum embi_css_rule_kind {
    EMBI_CSS_RULE_STYLE,    /* style rule (or keyframe). prelude: selector list. decls, kids. */
    EMBI_CSS_RULE_MEDIA,    /* prelude: media query list. kids, decls when nested in a style rule. */
    EMBI_CSS_RULE_SUPPORTS, /* cond: the condition's value. kids, decls. */
    EMBI_CSS_RULE_CONTAINER, /* name: container name (0 if none). prelude: condition. kids, decls. */
    EMBI_CSS_RULE_SCOPE,    /* prelude: scope range (raw). kids, decls. */
    EMBI_CSS_RULE_LAYER,    /* block: block form. layers: statement names. name: block name (0 = anon). kids. */
    EMBI_CSS_RULE_IMPORT,   /* url, cond (supports), has_layer, name (layer), prelude (media), sheet */
    EMBI_CSS_RULE_NAMESPACE, /* name: prefix (0 = default). url. */
    EMBI_CSS_RULE_FONT_FACE, /* decls: descriptors (raw, not validated against properties). */
    EMBI_CSS_RULE_KEYFRAMES, /* name. kids: keyframe rules (STYLE kind, decls). */
    EMBI_CSS_RULE_PROPERTY,  /* name: the registered custom property. decls: descriptors (raw). */
    EMBI_CSS_RULE_COUNTER_STYLE, /* name: the counter style's name (as written). decls: descriptors (raw). */
} embi_css_rule_kind;

typedef struct embi_css_decl {
    embi_atom name;         /* property name: ASCII-lowercased, or exact for --custom properties */
    uint32_t prop;          /* property id, assigned by the property database (W2c). 0 until then. */
    bool important;
    embi_css_slice value;   /* component values, raw until W2c */
    uint32_t order;         /* source offset of the name. Orders decls against kids of one block. */
} embi_css_decl;

typedef struct embi_css_rule {
    uint8_t kind;           /* embi_css_rule_kind */
    bool cond;              /* SUPPORTS, IMPORT (supports()): the condition holds */
    uint32_t order;         /* source offset of the rule's first token. Orders kids against decls
                             * of the same block, so CSS Nesting's interleaving is recoverable. */
    bool has_layer;         /* IMPORT: layer or layer(name) present */
    bool block;             /* LAYER: block form */
    embi_atom name;
    embi_str url;           /* IMPORT, NAMESPACE */
    embi_css_slice prelude;
    embi_css_decl *decls;
    uint32_t ndecls;
    struct embi_css_rule *kids;
    uint32_t nkids;
    embi_atom *layers;      /* LAYER statement: dotted layer names */
    uint32_t nlayers;
    embi_css_sheet *sheet;  /* IMPORT: imported sheet (owned reference), or nullptr */
} embi_css_rule;

/* Sheets are immutable after parse. refs is atomic, so a sheet can be shared across threads. */
struct embi_css_sheet {
    _Atomic uint32_t refs;
    embi_arena arena;
    embi_css_rule *rules;
    uint32_t nrules;
};

/* Fetches an @import target. base is the absolute URL of the sheet doing the import (the parse's
 * base_url for the root sheet), url is the URL as written. The embedder resolves url against base
 * and writes the absolute result to *resolved. The parser copies *resolved before the call
 * returns. An empty *resolved means the URL as written. The resolved URL is what cycle detection
 * compares, so two spellings of one file are one sheet. The bytes must stay valid until the
 * imported sheet is parsed (the call returns). Returns false when unavailable. Every @import
 * that is not disabled by supports() is fetched, including one that turns out to be a cycle. */
typedef struct embi_css_loader {
    void *user;
    bool (*fetch)(void *user, embi_str base, embi_str url, embi_str *resolved,
                  const unsigned char **bytes, size_t *n);
} embi_css_loader;

/* Property database hooks (W2c). Both may be nullptr. Without hooks every declaration is kept
 * and @supports declaration and selector conditions evaluate to false. */
typedef struct embi_css_hooks {
    void *user;
    /* Returns false when the declaration is invalid for its property (dropped per spec). name is
     * the normalized property name. */
    bool (*decl_ok)(void *user, embi_str name, embi_css_slice value, bool important);
    /* Returns true when the selector list is supported (for @supports selector()). */
    bool (*selector_ok)(void *user, embi_css_slice selector);
} embi_css_hooks;

typedef struct embi_css_parse_opts {
    const embi_css_loader *loader; /* may be nullptr: imports then have no sheet */
    const embi_css_hooks *hooks;   /* may be nullptr */
    embi_str base_url;             /* URL of the root sheet. May be empty. */
} embi_css_parse_opts;

/* Parses UTF-8 text into a new sheet with refcount 1. Never returns nullptr. */
embi_css_sheet *embi_css_sheet_parse(embi_str text, const embi_css_parse_opts *opts);

/* Decodes a byte stream and parses it. *encoding receives the canonical lowercase name. Returns
 * nullptr when the encoding is not supported (only UTF-8 and UTF-16 decode; legacy encodings
 * are recognized by name and reported) or the input is 4 GiB or more. */
embi_css_sheet *embi_css_sheet_parse_bytes(const unsigned char *p, size_t n, embi_str protocol,
                                           embi_str env, const embi_css_parse_opts *opts,
                                           const char **encoding);

void embi_css_sheet_ref(embi_css_sheet *s);
void embi_css_sheet_unref(embi_css_sheet *s);
