#pragma once
/*
 * Binary data (J4e): ArrayBuffer and SharedArrayBuffer
 * (builtins_arraybuffer.c), the integer-indexed TypedArray objects (builtins_typedarray.c),
 * DataView (builtins_dataview.c) and Atomics (builtins_atomics.c). This header holds the shared
 * layouts, the intrinsic record and the element conversions.
 *
 * Layouts. A buffer is an object of class ArrayBuffer or SharedArrayBuffer whose `internal` slot is
 * an embi_js_abuf. Its bytes live in embi_alloc memory that the class finalizer frees, and its trace
 * reports the size as external storage. A view (TypedArray or DataView) is an object whose
 * `internal` slot is a value array (EMBI_JS_VIEW_* slots), so the collector traces the buffer
 * through it.
 *
 * Intrinsics. The binary builtins' prototypes and constructors live in one value array that is a
 * runtime root (embi_js_binary_intrinsics), so the realm needs no extra field.
 *
 * Byte order. TypedArrays use little-endian element order (every target is little-endian). DataView
 * takes the order from its argument. The byte loops below do not depend on the host.
 */
#include "js/bigint.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/string.h"
#include "js/value.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* embi_js_abuf flags. */
#define EMBI_JS_ABUF_DETACHED 0x01u
#define EMBI_JS_ABUF_RESIZABLE 0x02u /* resizable (or growable); a fixed buffer has max_len == len */
#define EMBI_JS_ABUF_SHARED 0x04u
#define EMBI_JS_ABUF_IMMUTABLE 0x08u

/* Largest byte length of one buffer. Above it the constructors throw a RangeError. On a target
 * whose size_t is narrower than 2^32 the limit is clamped to half of SIZE_MAX, so the shift is
 * never out of range and sizes computed from element counts cannot wrap. */
#define EMBI_JS_ABUF_MAX_BYTES                                                                    \
    ((SIZE_MAX < ((uint64_t)1 << 32)) ? (size_t)(SIZE_MAX / 2) : (size_t)((uint64_t)1 << 32))

typedef struct embi_js_abuf {
    uint8_t *data;  /* nullptr when cap is 0 or the buffer is detached */
    size_t len;     /* current byte length (0 once detached) */
    size_t max_len; /* maximum byte length (equal to len for a fixed-length buffer) */
    size_t cap;     /* bytes allocated behind data */
    uint8_t flags;  /* EMBI_JS_ABUF_* */
} embi_js_abuf;

/* TypedArray element kinds, in the order of the constructors' table in builtins_typedarray.c. */
enum embi_js_ta_kind {
    EMBI_JS_TA_INT8,
    EMBI_JS_TA_UINT8,
    EMBI_JS_TA_UINT8C,
    EMBI_JS_TA_INT16,
    EMBI_JS_TA_UINT16,
    EMBI_JS_TA_INT32,
    EMBI_JS_TA_UINT32,
    EMBI_JS_TA_FLOAT16,
    EMBI_JS_TA_FLOAT32,
    EMBI_JS_TA_FLOAT64,
    EMBI_JS_TA_BIGINT64,
    EMBI_JS_TA_BIGUINT64,
    EMBI_JS_TA_KINDS
};

/* Slots of a view record (the internal value array of a TypedArray or DataView). A TypedArray's
 * length slot is a number (fixed length) or undefined (length-tracking). A DataView's is a byte
 * length or undefined. The kind slot is used by TypedArrays only. */
enum {
    EMBI_JS_VIEW_BUFFER,
    EMBI_JS_VIEW_OFFSET,
    EMBI_JS_VIEW_LENGTH,
    EMBI_JS_VIEW_KIND,
    EMBI_JS_VIEW_SLOTS
};

/* Slots of the intrinsic record. The concrete TypedArray entries are base + kind. */
enum embi_js_bin_intr {
    EMBI_JS_BI_ARRAYBUFFER_PROTO,
    EMBI_JS_BI_ARRAYBUFFER_CTOR,
    EMBI_JS_BI_SHARED_PROTO,
    EMBI_JS_BI_SHARED_CTOR,
    EMBI_JS_BI_TYPEDARRAY_PROTO,
    EMBI_JS_BI_TYPEDARRAY_CTOR,
    EMBI_JS_BI_TA_KEYS,        /* Array.prototype.keys (the TypedArray keys calls it) */
    EMBI_JS_BI_TA_ENTRIES,     /* Array.prototype.entries */
    EMBI_JS_BI_TA_VALUES,      /* Array.prototype.values */
    EMBI_JS_BI_ARRAY_TOSTRING, /* Array.prototype.toString, shared with %TypedArray%.prototype */
    EMBI_JS_BI_DATAVIEW_PROTO,
    EMBI_JS_BI_TA_PROTO_BASE,
    EMBI_JS_BI_TA_CTOR_BASE = EMBI_JS_BI_TA_PROTO_BASE + EMBI_JS_TA_KINDS,
    EMBI_JS_BI_COUNT = EMBI_JS_BI_TA_CTOR_BASE + EMBI_JS_TA_KINDS
};

extern const embi_js_class embi_js_class_arraybuffer;
extern const embi_js_class embi_js_class_sharedarraybuffer;
extern const embi_js_class embi_js_class_typedarray;
extern const embi_js_class embi_js_class_dataview;

/* The intrinsic record (created on first use, rooted for the runtime's lifetime). */
embi_js_varray *embi_js_binary_intrinsics(embi_js_runtime *rt);
static inline embi_js_value embi_js_binary_intrinsic(embi_js_runtime *rt, enum embi_js_bin_intr w)
{
    return embi_js_binary_intrinsics(rt)->elems[w];
}

/* GetPrototypeFromConstructor for the binary builtins: new_target's "prototype" when it is an
 * object, else the intrinsic. Returns EMBI_JS_EXCEPTION when the Get throws. */
embi_js_value embi_js_binary_proto_from(embi_js_runtime *rt, embi_js_value new_target,
                                        enum embi_js_bin_intr which);

/* ToIndex (7.1.22): the value as a size_t, or false with a RangeError pending. */
bool embi_js_bin_to_index(embi_js_runtime *rt, embi_js_value v, size_t *out);

/* Allocates a buffer object with proto and len zeroed bytes (max_len is the maximum byte length;
 * flags are EMBI_JS_ABUF_*, with SHARED selecting SharedArrayBuffer). Returns nullptr with a
 * RangeError pending when the size is too large or the memory is not available. */
embi_js_object *embi_js_arraybuffer_alloc(embi_js_runtime *rt, embi_js_value proto, size_t len,
                                          size_t max_len, uint8_t flags);

/* DetachArrayBuffer for embedders (test262's $262.detachArrayBuffer). False when v is not an
 * ArrayBuffer object (a SharedArrayBuffer cannot be detached). */
bool embi_js_arraybuffer_detach(embi_js_value v);

static inline embi_js_abuf *embi_js_abuf_of(const embi_js_object *o)
{
    return (embi_js_abuf *)o->internal;
}

static inline embi_js_varray *embi_js_view_of(const embi_js_object *o)
{
    return (embi_js_varray *)o->internal;
}

static inline bool embi_js_is_typedarray_obj(const embi_js_object *o)
{
    return o->klass == &embi_js_class_typedarray;
}

static inline bool embi_js_is_dataview_obj(const embi_js_object *o)
{
    return o->klass == &embi_js_class_dataview;
}

static inline bool embi_js_is_buffer_obj(const embi_js_object *o)
{
    return o->klass == &embi_js_class_arraybuffer || o->klass == &embi_js_class_sharedarraybuffer;
}

static inline double embi_js_bin_num(embi_js_value v)
{
    return embi_js_is_int32(v) ? (double)embi_js_as_int32(v) : embi_js_as_double(v);
}

/* The buffer behind a view record. */
static inline embi_js_abuf *embi_js_view_abuf(const embi_js_varray *v)
{
    return embi_js_abuf_of(embi_js_as_object(v->elems[EMBI_JS_VIEW_BUFFER]));
}

static inline size_t embi_js_view_offset(const embi_js_varray *v)
{
    return (size_t)embi_js_bin_num(v->elems[EMBI_JS_VIEW_OFFSET]);
}

/* The view's extent in units of esz bytes. Returns false when the view is out of bounds (a
 * detached buffer, an offset past the end, or a fixed extent that runs past the end). */
static inline bool embi_js_view_len(const embi_js_varray *v, size_t esz, size_t *len)
{
    const embi_js_abuf *b = embi_js_view_abuf(v);
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return false;
    }
    size_t off = embi_js_view_offset(v);
    if (off > b->len) {
        return false;
    }
    size_t avail = (b->len - off) / esz;
    if (v->elems[EMBI_JS_VIEW_LENGTH] == EMBI_JS_UNDEFINED) {
        *len = avail;
        return true;
    }
    size_t n = (size_t)embi_js_bin_num(v->elems[EMBI_JS_VIEW_LENGTH]);
    if (n > avail) {
        return false;
    }
    *len = n;
    return true;
}

/* First byte of the view. Only meaningful when the view is in bounds and non-empty. */
static inline uint8_t *embi_js_view_base(const embi_js_varray *v)
{
    const embi_js_abuf *b = embi_js_view_abuf(v);
    size_t off = embi_js_view_offset(v);
    return b->data ? b->data + off : nullptr;
}

/* Element conversions shared by the TypedArray, DataView and Atomics builtins. */

/* Bytes per element and whether the kind holds BigInts. */
size_t embi_js_bin_size(enum embi_js_ta_kind k);
bool embi_js_bin_is_big(enum embi_js_ta_kind k);
/* The constructor name ("Uint8Array") of a kind. */
const char *embi_js_bin_name(enum embi_js_ta_kind k);
/* NumericToRawBytes: converts v (ToNumber, or ToBigInt for BigInt kinds) and returns the element
 * bits in the low bytes of *bits. Returns false with an exception pending when the conversion
 * throws. */
bool embi_js_bin_coerce(embi_js_runtime *rt, enum embi_js_ta_kind k, embi_js_value v,
                        uint64_t *bits);
/* RawBytesToNumeric: the JS value of the element bits (a number or a BigInt). Returns
 * EMBI_JS_EXCEPTION only when a BigInt cannot be allocated. */
embi_js_value embi_js_bin_box(embi_js_runtime *rt, enum embi_js_ta_kind k, uint64_t bits);
/* Byte store and load of an n-byte integer (n is 1, 2, 4 or 8), in the given byte order. */
void embi_js_bin_store(uint8_t *p, size_t n, uint64_t bits, bool big_endian);
uint64_t embi_js_bin_load(const uint8_t *p, size_t n, bool big_endian);

/* Float16 (IEEE binary16) conversions. embi_js_f16_from_double rounds to nearest, ties to even,
 * and overflows to infinity from 65520 up (the rounding of 65520 itself is a tie to infinity). */
static inline uint16_t embi_js_f16_from_double(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    uint16_t sign = (uint16_t)((bits >> 48) & 0x8000u);
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7FFu);
    uint64_t mant = bits & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FFu) {
        return mant ? (uint16_t)(sign | 0x7E00u) : (uint16_t)(sign | 0x7C00u);
    }
    if (exp == 0) {
        return sign; /* zero, or a double subnormal far below the smallest half subnormal */
    }
    int e = (int)exp - 1023;
    uint64_t m = (1ull << 52) | mant;
    if (e > 15) {
        return (uint16_t)(sign | 0x7C00u);
    }
    if (e >= -14) {
        uint64_t hm = m >> 42; /* 11 significant bits, in [1024, 2048) */
        uint64_t rem = m & ((1ull << 42) - 1);
        const uint64_t half = 1ull << 41;
        if (rem > half || (rem == half && (hm & 1u))) {
            hm++;
        }
        if (hm == 2048) {
            hm = 1024;
            e++;
        }
        if (e > 15) {
            return (uint16_t)(sign | 0x7C00u);
        }
        return (uint16_t)(sign | ((uint32_t)(e + 15) << 10) | (uint32_t)(hm & 0x3FFu));
    }
    /* Subnormal half: the unit is 2^-24, and the value in units is m * 2^(e - 28). */
    int shift = 28 - e;
    if (shift >= 64) {
        return sign;
    }
    uint64_t q = m >> shift;
    uint64_t rem = m & ((1ull << shift) - 1);
    uint64_t half = 1ull << (shift - 1);
    if (rem > half || (rem == half && (q & 1u))) {
        q++;
    }
    return (uint16_t)(sign | q); /* q == 1024 is the smallest normal, with the right bits */
}

static inline double embi_js_f16_to_double(uint16_t h)
{
    double sign = (h & 0x8000u) ? -1.0 : 1.0;
    uint32_t e = (h >> 10) & 0x1Fu;
    uint32_t m = h & 0x3FFu;
    if (e == 0) {
        return sign * ldexp((double)m, -24);
    }
    if (e == 31) {
        return m ? NAN : sign * INFINITY;
    }
    return sign * ldexp((double)(1024u + m), (int)e - 25);
}
