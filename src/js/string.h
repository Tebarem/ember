#pragma once
/*
 * Flat immutable JS strings and the per-runtime atom table.
 *
 * Representation is canonical: a string is stored as Latin-1 (one byte per unit) unless some code
 * unit exceeds 0xFF, in which case it is stored as UTF-16. Equal contents therefore always have
 * the same representation, so comparing bytes is enough for equality.
 *
 * Property keys (embi_js_key): an array index 0 .. 2^32-2 is an int32-tagged value carrying the
 * u32; any other string key is an atom; symbols are symbol cells.
 */
#include "js/runtime.h"
#include "js/value.h"

#include <stddef.h>
#include <stdint.h>

#define EMBI_JS_STR_WIDE_BIT 0x80000000u
#define EMBI_JS_STR_MAX_LEN 0x7FFFFFFFu

typedef struct embi_js_string {
    embi_js_cell hdr;      /* flags: EMBI_JS_FLAG_ATOM when interned */
    uint32_t len_flags;    /* bit 31: UTF-16; bits 0..30: length in code units */
    uint32_t hash;         /* 0 = not computed yet */
    /* Characters follow at offset 16: Latin-1 bytes or UTF-16 units. */
} embi_js_string;

static_assert(sizeof(embi_js_string) == 16, "string header layout");

/* Ropes (J5). A rope is a STRING cell whose hdr.flags carry
 * EMBI_JS_STR_ROPE. Its first 16 bytes match embi_js_string; left and right are the children.
 * hdr.aux is the depth (flat strings and flattened ropes count as 0). A flattened rope keeps
 * EMBI_JS_STR_ROPE and adds EMBI_JS_STR_FLATTENED: left is then the flat string, right is EMPTY.
 * Char-reading accessors need a flat string (see embi_js_str_flat). */
#define EMBI_JS_STR_ROPE 0x0002u
#define EMBI_JS_STR_FLATTENED 0x0004u
#define EMBI_JS_ROPE_LEAF_MAX 256u     /* merged-leaf threshold for appends */
#define EMBI_JS_ROPE_DEPTH_MAX 4096u   /* deeper ropes are flattened at the next concat */

typedef struct embi_js_rope {
    embi_js_cell hdr;      /* aux: depth; flags: ROPE, FLATTENED once flattened */
    uint32_t len_flags;    /* same as embi_js_string */
    uint32_t hash;         /* same as embi_js_string; 0 = not computed yet */
    embi_js_value left;
    embi_js_value right;
} embi_js_rope;

static_assert(sizeof(embi_js_rope) == 32, "rope layout");

/* The string hash: FNV-1a over code units, then the finalizer below (never 0). Exposed so the atom
 * table can look up ASCII text without building a string (js_atom.c). */
#define EMBI_JS_STR_HASH_SEED 0x811C9DC5u
#define EMBI_JS_STR_HASH_PRIME 0x01000193u
static inline uint32_t embi_js_str_hash_finish(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h == 0 ? 1u : h;
}

static inline uint32_t embi_js_str_len(const embi_js_string *s)
{
    return s->len_flags & EMBI_JS_STR_MAX_LEN;
}

static inline bool embi_js_str_is_wide(const embi_js_string *s)
{
    return (s->len_flags & EMBI_JS_STR_WIDE_BIT) != 0;
}

static inline bool embi_js_str_is_atom(const embi_js_string *s)
{
    return (s->hdr.flags & EMBI_JS_FLAG_ATOM) != 0;
}

/* True for a rope node, flattened or not. Such a string has no characters of its own. */
static inline bool embi_js_str_is_rope(const embi_js_string *s)
{
    return (s->hdr.flags & EMBI_JS_STR_ROPE) != 0;
}

static inline const uint8_t *embi_js_str_latin1(const embi_js_string *s)
{
    EMBI_ASSERT(!embi_js_str_is_rope(s));
    return (const uint8_t *)s + sizeof(embi_js_string);
}

static inline const uint16_t *embi_js_str_wide(const embi_js_string *s)
{
    EMBI_ASSERT(!embi_js_str_is_rope(s));
    return (const uint16_t *)((const uint8_t *)s + sizeof(embi_js_string));
}

static inline uint16_t embi_js_str_at(const embi_js_string *s, uint32_t i)
{
    EMBI_ASSERT(i < embi_js_str_len(s));
    return embi_js_str_is_wide(s) ? embi_js_str_wide(s)[i] : embi_js_str_latin1(s)[i];
}

static inline bool embi_js_is_string(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_STRING;
}

static inline embi_js_string *embi_js_as_string(embi_js_value v)
{
    return (embi_js_string *)embi_js_as_cell(v);
}

static inline embi_js_value embi_js_from_string(const embi_js_string *s)
{
    return embi_js_from_cell(s);
}

/* ------------------------------------------------------------------ construction */

/* Each constructor canonicalizes the result. Input pointers may be nullptr when the length is 0. */
[[nodiscard]] embi_js_string *embi_js_str_new_latin1(embi_js_runtime *rt, const uint8_t *p,
                                                     size_t len);
[[nodiscard]] embi_js_string *embi_js_str_new_utf16(embi_js_runtime *rt, const uint16_t *p,
                                                    size_t len);
/* Invalid UTF-8 becomes U+FFFD (WHATWG rules). Lengths above EMBI_JS_STR_MAX_LEN abort. */
[[nodiscard]] embi_js_string *embi_js_str_new_utf8(embi_js_runtime *rt, const char *p,
                                                   size_t len);
/* Flat concatenation (copies). Rope operands are flattened first. */
[[nodiscard]] embi_js_string *embi_js_str_concat(embi_js_runtime *rt, const embi_js_string *a,
                                                 const embi_js_string *b);
/* Rope-building concatenation of two string values (J5, section 5). Returns a flat string when
 * the result is shorter than rope_min, and a rope otherwise. When one side is empty it returns
 * the other side unchanged (a rope stays a rope). May allocate and collect. */
[[nodiscard]] embi_js_value embi_js_str_concat_v(embi_js_runtime *rt, embi_js_value a,
                                                 embi_js_value b);
/* The flat form of a string value: a flat string is returned as is, a rope is flattened in place
 * (it becomes an indirection to the new flat string, and a later call is one load). May allocate
 * and collect; do not hold a char pointer across the call. */
[[nodiscard]] embi_js_string *embi_js_str_flat(embi_js_runtime *rt, embi_js_value v);
/* Sets the length at or above which embi_js_str_concat_v builds a rope (J5, section 5). Tests
 * use 1 to make every concat a rope. 0 restores the default, 256. */
void embi_js_set_rope_min(embi_js_runtime *rt, uint32_t n);
/* Code units [start, end). Clamped to the string's bounds. */
[[nodiscard]] embi_js_string *embi_js_str_substring(embi_js_runtime *rt, const embi_js_string *s,
                                                    uint32_t start, uint32_t end);
/* Number to string per Number::toString. */
[[nodiscard]] embi_js_string *embi_js_number_to_str(embi_js_runtime *rt, double d);

/* ------------------------------------------------------------------ queries */

bool embi_js_str_equal(const embi_js_string *a, const embi_js_string *b);
/* Code-unit order: negative, zero or positive. */
int embi_js_str_compare(const embi_js_string *a, const embi_js_string *b);
uint32_t embi_js_str_hash(const embi_js_string *s);
/* Appends the UTF-8 encoding; lone surrogates become U+FFFD. */
void embi_js_str_append_utf8(const embi_js_string *s, embi_sb *sb);
/* Like embi_js_str_append_utf8, but a lone surrogate is written as its three-byte WTF-8 form
 * instead of U+FFFD, so source text built from a string (eval, Function) keeps its code units. The
 * lexer reads that form back as the lone surrogate. */
void embi_js_str_append_wtf8(const embi_js_string *s, embi_sb *sb);
/* StringToNumber (ES2025 7.1.4.1.1). */
double embi_js_str_to_number(const embi_js_string *s);
/* True when the string is a canonical array index 0 .. 2^32-2. */
bool embi_js_str_is_array_index(const embi_js_string *s, uint32_t *out);

/* ------------------------------------------------------------------ atoms (atom.c) */

void embi_js_atoms_init(embi_js_atoms *a);
void embi_js_atoms_free(embi_js_atoms *a);
/* Returns the atom with the same contents. When none exists, `s` itself becomes the atom (its
 * ATOM flag is set) and is returned. */
embi_js_string *embi_js_atom_intern(embi_js_runtime *rt, embi_js_string *s);
embi_js_string *embi_js_atom_intern_utf8(embi_js_runtime *rt, const char *p, size_t len);
uint32_t embi_js_atoms_count(const embi_js_runtime *rt);

/* ------------------------------------------------------------------ property keys */

#define EMBI_JS_MAX_ARRAY_INDEX 4294967294u

static inline embi_js_key embi_js_key_from_index(uint32_t i)
{
    EMBI_ASSERT(i <= EMBI_JS_MAX_ARRAY_INDEX);
    return EMBI_JS_INT_TAG | (uint64_t)i;
}

static inline bool embi_js_key_is_index(embi_js_key k)
{
    return embi_js_is_int32(k);
}

static inline uint32_t embi_js_key_index(embi_js_key k)
{
    return (uint32_t)(k & 0xFFFFFFFFu);
}

/* Canonicalizes a string key: array indices become index keys, others become atoms. */
embi_js_key embi_js_key_from_str(embi_js_runtime *rt, const embi_js_string *s);
