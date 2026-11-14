#pragma once
/*
 * JS value representation (JSC-style NaN-boxing) and the 8-byte header shared by every heap cell.
 *
 *   heap pointer  top 16 bits zero, low 48 bits hold the cell address (never below 0x10)
 *   specials      EMPTY, NULL, FALSE, TRUE, UNDEFINED, EXCEPTION (all below 0x10)
 *   int32         0xFFFE'0000'0000'0000 | (uint32)i
 *   double        bits + 2^49 (NaNs are canonicalized before encoding)
 *
 * Every pointer conversion goes through embi_js_ptr_encode/decode.
 */
#include "base/base.h"

#include <stdint.h>
#include <string.h>

typedef struct embi_js_runtime embi_js_runtime;

typedef uint64_t embi_js_value;
/* Property key: an int32-tagged array index (0 .. 2^32-2), an atom string, or a symbol cell. */
typedef embi_js_value embi_js_key;

static_assert(sizeof(void *) == 8, "the JS engine is 64-bit only");
static_assert(sizeof(embi_js_value) == 8, "value must be 64 bits");

#define EMBI_JS_EMPTY ((embi_js_value)0x00u)
#define EMBI_JS_NULL ((embi_js_value)0x02u)
#define EMBI_JS_FALSE ((embi_js_value)0x06u)
#define EMBI_JS_TRUE ((embi_js_value)0x07u)
#define EMBI_JS_UNDEFINED ((embi_js_value)0x0Au)
#define EMBI_JS_EXCEPTION ((embi_js_value)0x0Eu)

#define EMBI_JS_INT_TAG ((uint64_t)0xFFFE000000000000ull)
#define EMBI_JS_DOUBLE_OFFSET ((uint64_t)1ull << 49)
#define EMBI_JS_PTR_LIMIT ((uint64_t)1ull << 48)
#define EMBI_JS_CANON_NAN_BITS ((uint64_t)0x7FF8000000000000ull)

enum embi_js_cell_type {
    EMBI_JS_CELL_FREE = 0, /* free slot; never a live object */
    EMBI_JS_CELL_STRING,
    EMBI_JS_CELL_SYMBOL,
    EMBI_JS_CELL_OBJECT,
    EMBI_JS_CELL_SHAPE,
    EMBI_JS_CELL_CODE,
    EMBI_JS_CELL_BOX,
    EMBI_JS_CELL_ACCESSOR,
    EMBI_JS_CELL_BIGINT,
    EMBI_JS_CELL_VALUE_ARRAY,
    EMBI_JS_CELL_TYPE_COUNT
};

/* Cell header flag bits (the 16-bit flags field). */
#define EMBI_JS_FLAG_ATOM 0x0001u

typedef struct embi_js_cell {
    uint8_t type;  /* enum embi_js_cell_type */
    uint8_t mark;  /* GC mark bit, clear outside a collection */
    uint16_t flags;
    uint32_t aux;  /* per-type word; VALUE_ARRAY keeps its length here */
} embi_js_cell;

static_assert(sizeof(embi_js_cell) == 8, "cell header must be 8 bytes");

static inline embi_js_value embi_js_ptr_encode(const void *p)
{
    uint64_t u = (uint64_t)(uintptr_t)p;
    EMBI_ASSERT(u != 0 && u < EMBI_JS_PTR_LIMIT && (u & 7u) == 0);
    return u;
}

static inline embi_js_cell *embi_js_ptr_decode(embi_js_value v)
{
    return (embi_js_cell *)(uintptr_t)v;
}

/* ------------------------------------------------------------------ predicates */

static inline bool embi_js_is_cell(embi_js_value v)
{
    return v >= 0x10u && v < EMBI_JS_PTR_LIMIT;
}

/* Every number (int32 or double) sits at or above 2^49; pointers and specials sit below. */
static inline bool embi_js_is_number(embi_js_value v)
{
    return v >= EMBI_JS_DOUBLE_OFFSET;
}

static inline bool embi_js_is_int32(embi_js_value v)
{
    return (v >> 48) == 0xFFFEu;
}

static inline bool embi_js_is_double(embi_js_value v)
{
    return embi_js_is_number(v) && !embi_js_is_int32(v);
}

static inline bool embi_js_is_empty(embi_js_value v)
{
    return v == EMBI_JS_EMPTY;
}

static inline bool embi_js_is_undefined(embi_js_value v)
{
    return v == EMBI_JS_UNDEFINED;
}

static inline bool embi_js_is_null(embi_js_value v)
{
    return v == EMBI_JS_NULL;
}

static inline bool embi_js_is_nullish(embi_js_value v)
{
    return v == EMBI_JS_NULL || v == EMBI_JS_UNDEFINED;
}

static inline bool embi_js_is_bool(embi_js_value v)
{
    return v == EMBI_JS_FALSE || v == EMBI_JS_TRUE;
}

static inline bool embi_js_is_exception(embi_js_value v)
{
    return v == EMBI_JS_EXCEPTION;
}

/* ------------------------------------------------------------------ constructors */

static inline embi_js_value embi_js_from_bool(bool b)
{
    return b ? EMBI_JS_TRUE : EMBI_JS_FALSE;
}

static inline embi_js_value embi_js_from_int32(int32_t i)
{
    return EMBI_JS_INT_TAG | (uint64_t)(uint32_t)i;
}

/* Encodes the bits as a double. NaN is canonicalized, so every NaN has one encoding. */
static inline embi_js_value embi_js_from_double(double d)
{
    uint64_t bits;
    if (d != d) {
        bits = EMBI_JS_CANON_NAN_BITS;
    } else {
        memcpy(&bits, &d, sizeof bits);
    }
    return bits + EMBI_JS_DOUBLE_OFFSET;
}

/* Canonical number: an exact int32 (other than -0) is stored as an int, anything else as a
 * double. Use this for values computed at runtime. */
static inline embi_js_value embi_js_number(double d)
{
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int32_t i = (int32_t)d;
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        if ((double)i == d && (i != 0 || (bits >> 63) == 0)) {
            return embi_js_from_int32(i);
        }
    }
    return embi_js_from_double(d);
}

static inline embi_js_value embi_js_from_cell(const void *cell)
{
    return embi_js_ptr_encode(cell);
}

/* ------------------------------------------------------------------ accessors */

static inline bool embi_js_as_bool(embi_js_value v)
{
    return v == EMBI_JS_TRUE;
}

static inline int32_t embi_js_as_int32(embi_js_value v)
{
    return (int32_t)(uint32_t)(v & 0xFFFFFFFFu);
}

static inline double embi_js_as_double(embi_js_value v)
{
    double d;
    if (embi_js_is_int32(v)) {
        return (double)embi_js_as_int32(v);
    }
    uint64_t bits = v - EMBI_JS_DOUBLE_OFFSET;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static inline embi_js_cell *embi_js_as_cell(embi_js_value v)
{
    EMBI_ASSERT(embi_js_is_cell(v));
    return embi_js_ptr_decode(v);
}
