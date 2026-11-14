#pragma once
/*
 * BigInt values (ECMA-262 6.1.6.2, J4b).
 *
 * A BigInt is an immutable heap cell (EMBI_JS_CELL_BIGINT): sign and magnitude, the magnitude in
 * 32-bit limbs, little-endian, with no high zero limb (zero has no limbs and is never negative).
 * The limb count is hdr.aux, and EMBI_JS_FLAG_BIG_NEG in hdr.flags marks a negative value.
 *
 * Errors. Functions that can throw return EMBI_JS_EXCEPTION (value results) or false (bool
 * results) with the exception pending on the runtime, as in ops.h.
 */
#include "js/object.h"
#include "js/string.h"
#include "js/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cell flag: the value is negative. */
#define EMBI_JS_FLAG_BIG_NEG 0x0008u
/* Largest magnitude in bits: a result with more bits throws a RangeError before its cell is
 * allocated. */
#define EMBI_JS_BIGINT_MAX_BITS (1u << 30)

typedef struct embi_js_bigint {
    embi_js_cell hdr; /* aux: limb count; flags: EMBI_JS_FLAG_BIG_NEG when negative */
    uint32_t limbs[]; /* magnitude, little-endian, no high zero limb */
} embi_js_bigint;

static_assert(sizeof(embi_js_bigint) == 8, "bigint header layout");

static inline bool embi_js_is_bigint(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_BIGINT;
}

static inline embi_js_bigint *embi_js_as_bigint(embi_js_value v)
{
    return (embi_js_bigint *)embi_js_as_cell(v);
}

static inline embi_js_value embi_js_from_bigint(const embi_js_bigint *b)
{
    return embi_js_from_cell(b);
}

static inline bool embi_js_bigint_is_neg(const embi_js_bigint *b)
{
    return (b->hdr.flags & EMBI_JS_FLAG_BIG_NEG) != 0;
}

/* True for 0n. */
static inline bool embi_js_bigint_is_zero(const embi_js_bigint *b)
{
    return b->hdr.aux == 0;
}

/* The binary operators of BigInt arithmetic. */
enum embi_js_bigop {
    EMBI_JS_BIG_ADD,
    EMBI_JS_BIG_SUB,
    EMBI_JS_BIG_MUL,
    EMBI_JS_BIG_DIV,
    EMBI_JS_BIG_MOD,
    EMBI_JS_BIG_EXP,
    EMBI_JS_BIG_SHL,
    EMBI_JS_BIG_SAR,
    EMBI_JS_BIG_SHR, /* >>> always throws a TypeError */
    EMBI_JS_BIG_AND,
    EMBI_JS_BIG_OR,
    EMBI_JS_BIG_XOR,
};

/* ------------------------------------------------------------------ construction */

[[nodiscard]] embi_js_bigint *embi_js_bigint_from_i64(embi_js_runtime *rt, int64_t v);
/* d must be finite and integral. */
[[nodiscard]] embi_js_bigint *embi_js_bigint_from_double(embi_js_runtime *rt, double d);
/* Digits (ASCII, no prefix, sign or separators) in radix 2..36. */
[[nodiscard]] embi_js_bigint *embi_js_bigint_from_digits(embi_js_runtime *rt, const char *digits,
                                                         size_t n, unsigned radix, bool neg);
/* StringToBigInt (7.1.14). Returns false when the string is not a valid BigInt, with no exception
 * pending. On true, *out holds the value, or is nullptr with a RangeError pending when the value
 * is too large. */
bool embi_js_string_to_bigint(embi_js_runtime *rt, const embi_js_string *s, embi_js_bigint **out);
/* ToBigInt (7.1.13): a BigInt, or EMBI_JS_EXCEPTION with a TypeError or SyntaxError pending. */
embi_js_value embi_js_to_bigint(embi_js_runtime *rt, embi_js_value v);

/* ------------------------------------------------------------------ conversions and queries */

/* The string of the value in radix 2..36 (lowercase digits). */
[[nodiscard]] embi_js_string *embi_js_bigint_to_string(embi_js_runtime *rt,
                                                       const embi_js_bigint *b, unsigned radix);
/* Exact comparison of two BigInts: negative, zero or positive. */
int embi_js_bigint_compare(const embi_js_bigint *a, const embi_js_bigint *b);
/* Exact comparison with a double: -1, 0 or 1 as b is less than, equal to or greater than d, or 2
 * when d is NaN. */
int embi_js_bigint_compare_double(const embi_js_bigint *b, double d);
/* Number(bigint): the nearest double, ties to even (infinite when too large). */
double embi_js_bigint_to_double(const embi_js_bigint *b);
/* Hash of the value, equal for equal values (for Map and Set keys). */
uint64_t embi_js_bigint_hash(const embi_js_bigint *b);

/* ------------------------------------------------------------------ operators */

/* Both operands are BigInts. Returns the result, or EMBI_JS_EXCEPTION (RangeError for division by
 * zero, a negative exponent or an oversize result; TypeError for >>>). */
embi_js_value embi_js_bigint_binop(embi_js_runtime *rt, enum embi_js_bigop op, embi_js_value a,
                                   embi_js_value b);
/* Unary minus of a BigInt. Never throws. Bitwise not and the ++/-- step can grow the magnitude
 * by one bit, so they throw a RangeError at the size limit (and the caller checks for
 * EMBI_JS_EXCEPTION). */
embi_js_value embi_js_bigint_neg(embi_js_runtime *rt, embi_js_value a);
embi_js_value embi_js_bigint_not(embi_js_runtime *rt, embi_js_value a);
/* a + delta for delta = 1 or -1 (the ++ and -- operators). */
embi_js_value embi_js_bigint_inc(embi_js_runtime *rt, embi_js_value a, int delta);
/* BigInt.asIntN (is_signed) and BigInt.asUintN of a BigInt, with bits in 0 .. 2^53-1. */
embi_js_value embi_js_bigint_as_n(embi_js_runtime *rt, embi_js_value a, uint64_t bits,
                                  bool is_signed);

/* Test hook: the number of heap limb buffers the arithmetic has allocated so far. An operation on
 * values of up to 8 limbs allocates none (only its result cell). */
uint64_t embi_js_bigint_temp_allocs(void);

/* ------------------------------------------------------------------ wrappers */

/* The BigInt class of wrapper objects (BigInt.prototype's instances). Its internal slot holds the
 * primitive. */
extern const embi_js_class embi_js_class_bigint;
/* ToObject of a BigInt primitive (the prototype is the realm's BigInt.prototype). */
[[nodiscard]] embi_js_object *embi_js_bigint_wrapper_new(embi_js_runtime *rt, embi_js_value v);
/* The primitive of a BigInt wrapper. embi_js_wrapper_value (object.c) knows only the built-in
 * wrapper classes and returns undefined for a BigInt wrapper, so every caller reads BigInt
 * wrappers through this function (builtins_bigint.c does; object.c is not owned by J4b). */
embi_js_value embi_js_bigint_wrapper_value(const embi_js_object *o);
