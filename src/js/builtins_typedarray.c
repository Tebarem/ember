/*
 * %TypedArray% and the twelve concrete TypedArray constructors (ECMA-262 23.2, ES2026 base64 and hex
 * methods), with the integer-indexed exotic object behaviour installed as class hooks
 *. Element conversions for DataView and Atomics live here too.
 *
 * Non-numeric keys of a TypedArray take the ordinary path. The object layer (object.c) does not
 * export its ordinary store lookup, so ord_* below run the generic internal method with the class
 * swapped for the plain object class. Those calls never run user code, and the view record stays
 * on the C stack across them, since the swapped class does not trace it.
 */
#include "base/base.h"
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/typedarray.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define THREW EMBI_JS_THREW
#define YES EMBI_JS_YES
#define NO EMBI_JS_NO
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_ALL (ATTR_W | ATTR_E | ATTR_C)

/* Keeps p reachable from the stack up to this point (the conservative stack scan finds it). A
 * volatile sink is portable C and the compiler cannot drop the store. */
static void keep_alive(const void *p)
{
    const void *volatile sink = p;
    (void)sink;
}

typedef enum embi_js_ta_kind kind_t;

static embi_js_value arg(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value str_val(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* Exact comparison of a string value with an ASCII literal. */
static bool str_eq(embi_js_runtime *rt, embi_js_value v, const char *want)
{
    if (!embi_js_is_string(v)) {
        return false;
    }
    const embi_js_string *s = embi_js_str_flat(rt, v);
    size_t n = strlen(want);
    if (embi_js_str_len(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (embi_js_str_at(s, (uint32_t)i) != (uint8_t)want[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ element kinds */

static const struct {
    const char *name;
    uint8_t size;
    bool big;
} kinds[EMBI_JS_TA_KINDS] = {
    {"Int8Array", 1, false},     {"Uint8Array", 1, false},    {"Uint8ClampedArray", 1, false},
    {"Int16Array", 2, false},    {"Uint16Array", 2, false},   {"Int32Array", 4, false},
    {"Uint32Array", 4, false},   {"Float16Array", 2, false},  {"Float32Array", 4, false},
    {"Float64Array", 8, false},  {"BigInt64Array", 8, true},  {"BigUint64Array", 8, true},
};

size_t embi_js_bin_size(kind_t k)
{
    return kinds[k].size;
}

bool embi_js_bin_is_big(kind_t k)
{
    return kinds[k].big;
}

const char *embi_js_bin_name(kind_t k)
{
    return kinds[k].name;
}

void embi_js_bin_store(uint8_t *p, size_t n, uint64_t bits, bool big_endian)
{
    for (size_t i = 0; i < n; i++) {
        size_t j = big_endian ? n - 1 - i : i;
        p[j] = (uint8_t)(bits >> (8 * i));
    }
}

uint64_t embi_js_bin_load(const uint8_t *p, size_t n, bool big_endian)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        size_t j = big_endian ? n - 1 - i : i;
        v |= (uint64_t)p[j] << (8 * i);
    }
    return v;
}

/* ToInt8..ToUint32 and ToBigInt64 wrap modulo 2^64: the low bits of the integer part of d. */
static uint64_t to_u64_mod(double d)
{
    if (!isfinite(d)) {
        return 0;
    }
    d = trunc(d);
    if (d >= -9.2e18 && d <= 9.2e18) {
        return (uint64_t)(int64_t)d;
    }
    double r = fmod(d, 18446744073709551616.0);
    if (r < 0) {
        r += 18446744073709551616.0;
    }
    return (uint64_t)r;
}

/* ToUint8Clamp: round half to even, clamped to [0, 255]. */
static uint64_t clamp_u8(double d)
{
    if (!(d > 0)) {
        return 0;
    }
    if (d >= 255.0) {
        return 255;
    }
    double f = floor(d);
    double diff = d - f;
    if (diff < 0.5) {
        return (uint64_t)f;
    }
    if (diff > 0.5) {
        return (uint64_t)f + 1;
    }
    return fmod(f, 2.0) == 0.0 ? (uint64_t)f : (uint64_t)f + 1;
}

/* The low 64 bits of a BigInt, in two's complement. */
static uint64_t big_low64(const embi_js_bigint *b)
{
    uint64_t lo = b->hdr.aux > 0 ? b->limbs[0] : 0;
    uint64_t hi = b->hdr.aux > 1 ? b->limbs[1] : 0;
    uint64_t mag = lo | (hi << 32);
    return embi_js_bigint_is_neg(b) ? (uint64_t)0 - mag : mag;
}

bool embi_js_bin_coerce(embi_js_runtime *rt, kind_t k, embi_js_value v, uint64_t *bits)
{
    if (kinds[k].big) {
        embi_js_value b = embi_js_to_bigint(rt, v);
        if (b == EXC) {
            return false;
        }
        *bits = big_low64(embi_js_as_bigint(b));
        return true;
    }
    double d;
    if (!embi_js_to_number(rt, v, &d)) {
        return false;
    }
    switch (k) {
    case EMBI_JS_TA_UINT8C:
        *bits = clamp_u8(d);
        break;
    case EMBI_JS_TA_FLOAT16:
        *bits = embi_js_f16_from_double(d);
        break;
    case EMBI_JS_TA_FLOAT32: {
        float f = (float)d;
        uint32_t u;
        memcpy(&u, &f, sizeof u);
        *bits = u;
        break;
    }
    case EMBI_JS_TA_FLOAT64:
        memcpy(bits, &d, sizeof d);
        break;
    default:
        *bits = to_u64_mod(d);
        break;
    }
    return true;
}

embi_js_value embi_js_bin_box(embi_js_runtime *rt, kind_t k, uint64_t bits)
{
    switch (k) {
    case EMBI_JS_TA_INT8:
        return embi_js_from_int32((int8_t)bits);
    case EMBI_JS_TA_UINT8:
    case EMBI_JS_TA_UINT8C:
        return embi_js_from_int32((uint8_t)bits);
    case EMBI_JS_TA_INT16:
        return embi_js_from_int32((int16_t)bits);
    case EMBI_JS_TA_UINT16:
        return embi_js_from_int32((uint16_t)bits);
    case EMBI_JS_TA_INT32:
        return embi_js_from_int32((int32_t)bits);
    case EMBI_JS_TA_UINT32:
        return embi_js_number((double)(uint32_t)bits);
    case EMBI_JS_TA_FLOAT16:
        return embi_js_number(embi_js_f16_to_double((uint16_t)bits));
    case EMBI_JS_TA_FLOAT32: {
        uint32_t u = (uint32_t)bits;
        float f;
        memcpy(&f, &u, sizeof f);
        return embi_js_number((double)f);
    }
    case EMBI_JS_TA_FLOAT64: {
        double d;
        memcpy(&d, &bits, sizeof d);
        return embi_js_number(d);
    }
    case EMBI_JS_TA_BIGINT64:
        return embi_js_from_bigint(embi_js_bigint_from_i64(rt, (int64_t)bits));
    case EMBI_JS_TA_BIGUINT64: {
        if (bits <= (uint64_t)INT64_MAX) {
            return embi_js_from_bigint(embi_js_bigint_from_i64(rt, (int64_t)bits));
        }
        char digits[24];
        int n = SDL_snprintf(digits, sizeof digits, "%llu", (unsigned long long)bits);
        return embi_js_from_bigint(
            embi_js_bigint_from_digits(rt, digits, (size_t)n, 10, false));
    }
    default:
        return UNDEF;
    }
}

/* ------------------------------------------------------------------ view records */

static kind_t kind_of(const embi_js_varray *v)
{
    return (kind_t)embi_js_as_int32(v->elems[EMBI_JS_VIEW_KIND]);
}

static size_t esz_of(const embi_js_varray *v)
{
    return kinds[kind_of(v)].size;
}

static bool is_ta(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_is_typedarray_obj(embi_js_as_object(v));
}

/* Ordinary own-property access for the keys the TypedArray hooks do not take (see the file
 * comment). The lookup reads the object's own storage (embi_js_priv_get_own, object.c), so it
 * never reaches the class hooks and needs no class swap. */
static int ord_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    (void)rt;
    return embi_js_priv_get_own(o, k, out) ? YES : NO;
}

/* define, delete and own_keys still go through the generic internal methods with the class
 * swapped for the plain class (object.c exports no ordinary entry point for them yet; see the
 * J4e notes). Those paths never run user code, and the view record is kept on the stack. */
static int ord_define(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                      const embi_js_pdesc *d)
{
    const embi_js_class *c = o->klass;
    embi_js_varray *keep = o->internal;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_define_own_property(rt, o, k, d);
    o->klass = c;
    keep_alive(keep);
    return r;
}

static int ord_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    const embi_js_class *c = o->klass;
    embi_js_varray *keep = o->internal;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_delete(rt, o, k);
    o->klass = c;
    keep_alive(keep);
    return r;
}

static int ord_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    const embi_js_class *c = o->klass;
    embi_js_varray *keep = o->internal;
    o->klass = &embi_js_class_object;
    int r = embi_js_obj_own_keys(rt, o, out);
    o->klass = c;
    keep_alive(keep);
    return r;
}

/* CanonicalNumericIndexString: true when k is a numeric key (an array index, or a string that
 * round-trips through Number and String), with its numeric value in *out. */
static bool canon_numeric(embi_js_runtime *rt, embi_js_key k, double *out)
{
    if (embi_js_key_is_index(k)) {
        *out = (double)embi_js_key_index(k);
        return true;
    }
    if (!embi_js_is_string(k)) {
        return false;
    }
    embi_js_string *s = embi_js_str_flat(rt, k);
    uint32_t n = embi_js_str_len(s);
    if (n == 0) {
        return false;
    }
    uint16_t c0 = embi_js_str_at(s, 0);
    if (!((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == 'I' || c0 == 'N')) {
        return false;
    }
    if (n == 2 && c0 == '-' && embi_js_str_at(s, 1) == '0') {
        *out = -0.0;
        return true;
    }
    double d = embi_js_str_to_number(s);
    embi_js_string *back = embi_js_number_to_str(rt, d);
    if (!back || !embi_js_str_equal(back, s)) {
        return false;
    }
    *out = d;
    return true;
}

/* IsValidIntegerIndex: the index d names an element of the view as it is now. */
static bool valid_index(const embi_js_varray *v, double d, size_t *idx)
{
    size_t len;
    if (!embi_js_view_len(v, esz_of(v), &len)) {
        return false;
    }
    if (!(d >= 0.0 && d < (double)len)) {
        return false;
    }
    if (d != floor(d)) {
        return false;
    }
    if (d == 0.0 && signbit(d)) {
        return false;
    }
    *idx = (size_t)d;
    return true;
}

static embi_js_value read_elem(embi_js_runtime *rt, const embi_js_varray *v, size_t idx)
{
    kind_t k = kind_of(v);
    size_t n = kinds[k].size;
    uint64_t bits = embi_js_bin_load(embi_js_view_base(v) + idx * n, n, false);
    return embi_js_bin_box(rt, k, bits);
}

static void write_bits(const embi_js_varray *v, size_t idx, uint64_t bits)
{
    size_t n = esz_of(v);
    embi_js_bin_store(embi_js_view_base(v) + idx * n, n, bits, false);
}

/* TypedArrayGetElement: the element at index d, or undefined when the index is not valid. */
static embi_js_value get_at(embi_js_runtime *rt, embi_js_object *o, double d)
{
    embi_js_varray *v = o->internal;
    size_t idx;
    if (!valid_index(v, d, &idx)) {
        return UNDEF;
    }
    return read_elem(rt, v, idx);
}

/* TypedArraySetElement: converts v, then stores it when the index is still valid. Returns YES or
 * THREW. */
static int set_element(embi_js_runtime *rt, embi_js_object *o, double d, embi_js_value v)
{
    embi_js_varray *view = o->internal;
    size_t idx;
    /* An immutable buffer with a valid index throws before the value is converted (test262's
     * custom-ctor-returns-immutable-arraybuffer cases observe no valueOf call). */
    if (valid_index(view, d, &idx) && (embi_js_view_abuf(view)->flags & EMBI_JS_ABUF_IMMUTABLE)) {
        (void)embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
        return THREW;
    }
    uint64_t bits;
    if (!embi_js_bin_coerce(rt, kind_of(view), v, &bits)) {
        return THREW;
    }
    if (!valid_index(view, d, &idx)) {
        return YES;
    }
    write_bits(view, idx, bits);
    return YES;
}

/* OrdinarySet's tail: stores v as a data property of the receiver. */
static int set_on_receiver(embi_js_runtime *rt, embi_js_value receiver, embi_js_key k,
                           embi_js_value v)
{
    if (!embi_js_is_object(receiver)) {
        return NO;
    }
    embi_js_object *rcv = embi_js_as_object(receiver);
    embi_js_pdesc ex;
    int r = embi_js_obj_get_own_property(rt, rcv, k, &ex);
    if (r == THREW) {
        return THREW;
    }
    if (r == YES) {
        if (embi_js_pd_is_accessor(&ex) || !(ex.attrs & ATTR_W)) {
            return NO;
        }
        embi_js_pdesc vd = {v, UNDEF, UNDEF, EMBI_JS_PD_VALUE, 0};
        return embi_js_obj_define_own_property(rt, rcv, k, &vd);
    }
    embi_js_pdesc nd = {v, UNDEF, UNDEF,
                        EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                            EMBI_JS_PD_CONFIGURABLE,
                        ATTR_ALL};
    return embi_js_obj_define_own_property(rt, rcv, k, &nd);
}

/* ------------------------------------------------------------------ integer-indexed hooks */

static int ta_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    double d;
    if (!canon_numeric(rt, k, &d)) {
        return ord_get_own(rt, o, k, out);
    }
    size_t idx;
    if (!valid_index(o->internal, d, &idx)) {
        return NO;
    }
    embi_js_value v = read_elem(rt, o->internal, idx);
    if (v == EXC) {
        return THREW;
    }
    out->value = v;
    out->get = UNDEF;
    out->set = UNDEF;
    out->has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
               EMBI_JS_PD_CONFIGURABLE;
    out->attrs = ATTR_ALL;
    return YES;
}

static int ta_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                         const embi_js_pdesc *d)
{
    double n;
    if (!canon_numeric(rt, k, &n)) {
        return ord_define(rt, o, k, d);
    }
    size_t idx;
    if (!valid_index(o->internal, n, &idx)) {
        return NO;
    }
    if ((d->has & EMBI_JS_PD_CONFIGURABLE) && !(d->attrs & EMBI_JS_ATTR_CONFIGURABLE)) {
        return NO;
    }
    if ((d->has & EMBI_JS_PD_ENUMERABLE) && !(d->attrs & EMBI_JS_ATTR_ENUMERABLE)) {
        return NO;
    }
    if (embi_js_pd_is_accessor(d)) {
        return NO;
    }
    if ((d->has & EMBI_JS_PD_WRITABLE) && !(d->attrs & EMBI_JS_ATTR_WRITABLE)) {
        return NO;
    }
    if (d->has & EMBI_JS_PD_VALUE) {
        return set_element(rt, o, n, d->value);
    }
    return YES;
}

static int ta_has(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    double d;
    if (canon_numeric(rt, k, &d)) {
        size_t idx;
        return valid_index(o->internal, d, &idx) ? YES : NO;
    }
    embi_js_pdesc pd;
    int r = ord_get_own(rt, o, k, &pd);
    if (r != NO) {
        return r;
    }
    embi_js_value p = embi_js_obj_proto(o);
    if (!embi_js_is_object(p)) {
        return NO;
    }
    return embi_js_obj_has_property(rt, embi_js_as_object(p), k);
}

static int ta_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    double d;
    if (canon_numeric(rt, k, &d)) {
        size_t idx;
        return valid_index(o->internal, d, &idx) ? NO : YES;
    }
    return ord_delete(rt, o, k);
}

static int ta_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    embi_js_varray *v = o->internal;
    size_t len = 0;
    if (embi_js_view_len(v, esz_of(v), &len)) {
        for (size_t i = 0; i < len; i++) {
            embi_js_key kk = embi_js_key_from_index((uint32_t)i);
            embi_vec_push(out, kk);
        }
    }
    return ord_own_keys(rt, o, out);
}

static embi_js_value ta_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                            embi_js_value receiver)
{
    double d;
    if (canon_numeric(rt, k, &d)) {
        return get_at(rt, o, d);
    }
    embi_js_pdesc pd;
    int r = ord_get_own(rt, o, k, &pd);
    if (r == THREW) {
        return EXC;
    }
    if (r == YES) {
        if (embi_js_pd_is_accessor(&pd)) {
            if (pd.get == UNDEF) {
                return UNDEF;
            }
            return embi_js_call(rt, pd.get, receiver, 0, nullptr);
        }
        return pd.value;
    }
    embi_js_value p = embi_js_obj_proto(o);
    if (!embi_js_is_object(p)) {
        return UNDEF;
    }
    return embi_js_obj_get(rt, embi_js_as_object(p), k, receiver);
}

static int ta_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                  embi_js_value receiver)
{
    double d;
    if (canon_numeric(rt, k, &d)) {
        if (receiver == embi_js_from_object(o)) {
            return set_element(rt, o, d, v);
        }
        size_t idx;
        if (!valid_index(o->internal, d, &idx)) {
            return YES;
        }
        return set_on_receiver(rt, receiver, k, v);
    }
    embi_js_pdesc own;
    int r = ord_get_own(rt, o, k, &own);
    if (r == YES) {
        if (embi_js_pd_is_accessor(&own)) {
            if (own.set == UNDEF) {
                return NO;
            }
            embi_js_value res = embi_js_call(rt, own.set, receiver, 1, &v);
            return res == EXC ? THREW : YES;
        }
        if (!(own.attrs & ATTR_W)) {
            return NO;
        }
        return set_on_receiver(rt, receiver, k, v);
    }
    embi_js_value p = embi_js_obj_proto(o);
    if (embi_js_is_object(p)) {
        return embi_js_obj_set(rt, embi_js_as_object(p), k, v, receiver);
    }
    return set_on_receiver(rt, receiver, k, v);
}

static void ta_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

/* [[PreventExtensions]] (ECMA-262 10.4.5.x): a TypedArray that is not fixed-length (length-tracking,
 * or on a resizable buffer that is not shared) could gain elements when the buffer grows, so it
 * refuses to become non-extensible. Object.preventExtensions, Object.seal and Object.freeze then
 * throw a TypeError; Reflect.preventExtensions returns false. */
static int ta_prevent_extensions(embi_js_runtime *rt, embi_js_object *o)
{
    (void)rt;
    const embi_js_varray *view = embi_js_view_of(o);
    const embi_js_abuf *ab = embi_js_abuf_of(embi_js_as_object(view->elems[EMBI_JS_VIEW_BUFFER]));
    if (embi_js_is_undefined(view->elems[EMBI_JS_VIEW_LENGTH]) ||
        ((ab->flags & EMBI_JS_ABUF_RESIZABLE) && !(ab->flags & EMBI_JS_ABUF_SHARED))) {
        return EMBI_JS_NO;
    }
    o->hdr.flags &= (uint16_t)~EMBI_JS_OBJ_EXTENSIBLE;
    return EMBI_JS_YES;
}

const embi_js_class embi_js_class_typedarray = {
    .name = "TypedArray",
    .prevent_extensions = ta_prevent_extensions,
    .get_own = ta_get_own,
    .define_own = ta_define_own,
    .has = ta_has,
    .delete = ta_delete,
    .own_keys = ta_own_keys,
    .get = ta_get,
    .set = ta_set,
    .trace = ta_trace,
};

/* ------------------------------------------------------------------ creation and validation */

static embi_js_object *ta_create(embi_js_runtime *rt, embi_js_value proto, kind_t k,
                                 embi_js_object *buffer, size_t offset, bool tracking, size_t len)
{
    embi_js_varray *v = embi_js_varray_new(rt, EMBI_JS_VIEW_SLOTS);
    EMBI_JS_STORE(v, v->elems[EMBI_JS_VIEW_BUFFER], embi_js_from_object(buffer));
    EMBI_JS_STORE(v, v->elems[EMBI_JS_VIEW_OFFSET], embi_js_number((double)offset));
    EMBI_JS_STORE(v, v->elems[EMBI_JS_VIEW_LENGTH], tracking ? UNDEF : embi_js_number((double)len));
    EMBI_JS_STORE(v, v->elems[EMBI_JS_VIEW_KIND], embi_js_from_int32((int32_t)k));
    embi_js_object *o = embi_js_object_new_class(rt, proto, &embi_js_class_typedarray);
    o->internal = v;
    EMBI_JS_WB_STRUCT(o);
    return o;
}

/* A fresh zeroed buffer for n elements of kind k (AllocateArrayBuffer with %ArrayBuffer%). */
static embi_js_object *alloc_for(embi_js_runtime *rt, kind_t k, size_t n)
{
    size_t esz = kinds[k].size;
    /* Element counts stay below 2^32 - 1 so every index fits a uint32 key. */
    if (n > EMBI_JS_ABUF_MAX_BYTES / esz || n >= UINT32_MAX) {
        (void)embi_js_throw_range_error(rt, "Invalid typed array length");
        return nullptr;
    }
    embi_js_value bproto = embi_js_binary_intrinsic(rt, EMBI_JS_BI_ARRAYBUFFER_PROTO);
    return embi_js_arraybuffer_alloc(rt, bproto, n * esz, n * esz, 0);
}

/* A TypedArray of kind k with n zeroed elements and the given prototype. */
static embi_js_object *typed_new(embi_js_runtime *rt, embi_js_value proto, kind_t k, size_t n)
{
    embi_js_object *buf = alloc_for(rt, k, n);
    if (!buf) {
        return nullptr;
    }
    return ta_create(rt, proto, k, buf, 0, false, n);
}

/* ValidateTypedArray: the TypedArray in this (in bounds), with its current length. */
static bool validate(embi_js_runtime *rt, embi_js_value v, embi_js_object **out, size_t *len)
{
    if (!is_ta(v)) {
        (void)embi_js_throw_type_error(rt, "this is not a typed array");
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    if (!embi_js_view_len(o->internal, esz_of(o->internal), len)) {
        (void)embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        return false;
    }
    *out = o;
    return true;
}

/* ValidateTypedArray for a mutating method: validate, then a TypeError when the buffer is
 * immutable. Runs before any argument is read (test262 verifies mutability first). */
static bool validate_write(embi_js_runtime *rt, embi_js_value v, embi_js_object **out, size_t *len)
{
    if (!validate(rt, v, out, len)) {
        return false;
    }
    if (embi_js_view_abuf((*out)->internal)->flags & EMBI_JS_ABUF_IMMUTABLE) {
        (void)embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
        return false;
    }
    return true;
}

/* Constructs c with argc arguments and checks the result is an in-bounds TypedArray. When
 * check_len is set, its length must be at least want (the single numeric argument case). */
static embi_js_object *ctor_create(embi_js_runtime *rt, embi_js_value c, int argc,
                                   const embi_js_value *argv, bool check_len, double want,
                                   bool write)
{
    embi_js_value nv = embi_js_construct(rt, c, argc, argv, c);
    if (nv == EXC) {
        return nullptr;
    }
    if (!is_ta(nv)) {
        (void)embi_js_throw_type_error(rt, "constructor did not return a typed array");
        return nullptr;
    }
    embi_js_object *o = embi_js_as_object(nv);
    size_t len = 0;
    if (!embi_js_view_len(o->internal, esz_of(o->internal), &len)) {
        (void)embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        return nullptr;
    }
    if (write && (embi_js_view_abuf(o->internal)->flags & EMBI_JS_ABUF_IMMUTABLE)) {
        (void)embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
        return nullptr;
    }
    if (check_len && (double)len < want) {
        (void)embi_js_throw_type_error(rt, "typed array is too short");
        return nullptr;
    }
    return o;
}

/* TypedArraySpeciesCreate. */
static embi_js_object *species_create(embi_js_runtime *rt, embi_js_object *exemplar, int argc,
                                      const embi_js_value *argv, bool check_len, double want,
                                      bool write)
{
    kind_t k = kind_of(exemplar->internal);
    embi_js_value dflt = embi_js_binary_intrinsic(rt, EMBI_JS_BI_TA_CTOR_BASE + k);
    embi_js_value c = embi_js_species_constructor(rt, exemplar, dflt);
    if (c == EXC) {
        return nullptr;
    }
    embi_js_object *o = ctor_create(rt, c, argc, argv, check_len, want, write);
    if (!o) {
        return nullptr;
    }
    if (embi_js_bin_is_big(kind_of(o->internal)) != embi_js_bin_is_big(k)) {
        (void)embi_js_throw_type_error(rt, "species constructor returned a different content type");
        return nullptr;
    }
    return o;
}

/* TypedArrayCreateSameType. */
static embi_js_object *same_type_create(embi_js_runtime *rt, embi_js_object *exemplar, size_t n)
{
    kind_t k = kind_of(exemplar->internal);
    embi_js_value c = embi_js_binary_intrinsic(rt, EMBI_JS_BI_TA_CTOR_BASE + k);
    embi_js_value lenv = embi_js_number((double)n);
    return ctor_create(rt, c, 1, &lenv, true, (double)n, true);
}

/* Collects the values of an iterable with its @@iterator method. Returns a value array holding
 * count values, or nullptr with an exception pending. */
static embi_js_varray *collect_iter(embi_js_runtime *rt, embi_js_value src, embi_js_value method,
                                    uint32_t *count)
{
    embi_js_value it = embi_js_call(rt, method, src, 0, nullptr);
    if (it == EXC) {
        return nullptr;
    }
    if (!embi_js_is_object(it)) {
        (void)embi_js_throw_type_error(rt, "Result of the Symbol.iterator method is not an object");
        return nullptr;
    }
    embi_js_value next = embi_js_obj_get(rt, embi_js_as_object(it), embi_js_key_c(rt, "next"), it);
    if (next == EXC) {
        return nullptr;
    }
    embi_js_varray *arr = embi_js_varray_new(rt, 8);
    uint32_t n = 0;
    for (;;) {
        embi_js_value res = embi_js_call(rt, next, it, 0, nullptr);
        if (res == EXC) {
            return nullptr;
        }
        if (!embi_js_is_object(res)) {
            (void)embi_js_throw_type_error(rt, "iterator result is not an object");
            return nullptr;
        }
        embi_js_object *ro = embi_js_as_object(res);
        embi_js_value done = embi_js_obj_get(rt, ro, embi_js_key_c(rt, "done"), res);
        if (done == EXC) {
            return nullptr;
        }
        if (embi_js_to_boolean(done)) {
            break;
        }
        embi_js_value val = embi_js_obj_get(rt, ro, embi_js_key_c(rt, "value"), res);
        if (val == EXC) {
            return nullptr;
        }
        if (n == arr->hdr.aux) {
            arr = embi_js_varray_grow(rt, arr, n ? n * 2 : 8);
        }
        EMBI_JS_STORE(arr, arr->elems[n++], val);
    }
    *count = n;
    return arr;
}

/* ------------------------------------------------------------------ helpers for the methods */

/* The rel index of a method argument: clamped to [0, len] after a negative offset from the end. */
static bool rel_idx(embi_js_runtime *rt, embi_js_value v, double len, double dflt, double *out)
{
    if (v == UNDEF) {
        *out = dflt;
        return true;
    }
    double rel;
    if (!embi_js_to_integer_or_infinity(rt, v, &rel)) {
        return false;
    }
    if (rel < 0) {
        *out = rel + len < 0 ? 0 : rel + len;
    } else {
        *out = rel > len ? len : rel;
    }
    return true;
}

static embi_js_value call_fn(embi_js_runtime *rt, embi_js_value f, embi_js_value this_v, int argc,
                             const embi_js_value *argv)
{
    return embi_js_call(rt, f, this_v, argc, argv);
}

/* Throws a TypeError and returns false unless f is callable. */
static bool need_callable(embi_js_runtime *rt, embi_js_value f)
{
    if (!embi_js_is_callable(f)) {
        (void)embi_js_throw_type_error(rt, "callback is not a function");
        return false;
    }
    return true;
}

static embi_js_value num(double d)
{
    return embi_js_number(d);
}

/* The string of ToString(v), or nullptr with an exception pending. */
static embi_js_string *to_str(embi_js_runtime *rt, embi_js_value v)
{
    return embi_js_to_string(rt, v);
}

/* ------------------------------------------------------------------ %TypedArray% constructor */

static embi_js_value ta_abstract(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    (void)magic;
    (void)nt;
    return embi_js_throw_type_error(r->rt, "Abstract class TypedArray not directly constructable");
}

/* The concrete constructors (magic = the kind). */
static embi_js_value ta_construct(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    kind_t k = (kind_t)magic;
    if (embi_js_is_undefined(nt)) {
        return embi_js_throw_type_error(rt, "Constructor %s requires 'new'", kinds[k].name);
    }
    embi_js_value a0 = arg(argc, argv, 0);
    size_t esz = kinds[k].size;
    if (!embi_js_is_object(a0)) {
        size_t n;
        if (!embi_js_bin_to_index(rt, a0, &n)) {
            return EXC;
        }
        embi_js_value proto = embi_js_binary_proto_from(rt, nt, EMBI_JS_BI_TA_PROTO_BASE + k);
        if (proto == EXC) {
            return EXC;
        }
        embi_js_object *o = typed_new(rt, proto, k, n);
        return o ? embi_js_from_object(o) : EXC;
    }
    embi_js_value proto = embi_js_binary_proto_from(rt, nt, EMBI_JS_BI_TA_PROTO_BASE + k);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *src = embi_js_as_object(a0);

    if (embi_js_is_typedarray_obj(src)) {
        embi_js_varray *sv = src->internal;
        size_t slen;
        if (!embi_js_view_len(sv, esz_of(sv), &slen)) {
            return embi_js_throw_type_error(rt, "source typed array is detached or out of bounds");
        }
        kind_t sk = kind_of(sv);
        if (embi_js_bin_is_big(sk) != embi_js_bin_is_big(k)) {
            return embi_js_throw_type_error(rt, "content types of the typed arrays differ");
        }
        embi_js_object *o = typed_new(rt, proto, k, slen);
        if (!o) {
            return EXC;
        }
        embi_js_varray *dv = o->internal;
        if (sk == k) {
            size_t n = slen * esz;
            if (n > 0) {
                memcpy(embi_js_view_base(dv), embi_js_view_base(sv), n);
            }
        } else {
            for (size_t i = 0; i < slen; i++) {
                embi_js_value val = read_elem(rt, sv, i);
                uint64_t bits;
                if (val == EXC || !embi_js_bin_coerce(rt, k, val, &bits)) {
                    return EXC;
                }
                write_bits(dv, i, bits);
            }
        }
        return embi_js_from_object(o);
    }

    if (embi_js_is_buffer_obj(src)) {
        size_t off;
        if (!embi_js_bin_to_index(rt, arg(argc, argv, 1), &off)) {
            return EXC;
        }
        if (off % esz != 0) {
            return embi_js_throw_range_error(rt, "start offset of %s should be a multiple of %llu",
                                             kinds[k].name, (unsigned long long)esz);
        }
        bool has_len = arg(argc, argv, 2) != UNDEF;
        size_t newlen = 0;
        if (has_len && !embi_js_bin_to_index(rt, arg(argc, argv, 2), &newlen)) {
            return EXC;
        }
        embi_js_abuf *b = embi_js_abuf_of(src);
        if (b->flags & EMBI_JS_ABUF_DETACHED) {
            return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
        }
        size_t buflen = b->len;
        if (off > buflen) {
            return embi_js_throw_range_error(rt, "start offset %llu is outside the buffer", (unsigned long long)off);
        }
        bool tracking = false;
        size_t len = 0;
        if (!has_len) {
            if (b->flags & EMBI_JS_ABUF_RESIZABLE) {
                tracking = true;
            } else {
                if (buflen % esz != 0) {
                    return embi_js_throw_range_error(
                        rt, "byte length of %s should be a multiple of %llu", kinds[k].name, (unsigned long long)esz);
                }
                len = (buflen - off) / esz;
            }
        } else {
            if (newlen > (buflen - off) / esz) {
                return embi_js_throw_range_error(rt, "invalid typed array length");
            }
            len = newlen;
        }
        if (tracking ? (b->max_len - off) / esz >= (size_t)UINT32_MAX : len >= (size_t)UINT32_MAX) {
            return embi_js_throw_range_error(rt, "Invalid typed array length");
        }
        embi_js_object *o = ta_create(rt, proto, k, src, off, tracking, len);
        return embi_js_from_object(o);
    }

    /* An iterable, or an array-like object. */
    embi_js_value method = embi_js_get_method(rt, a0, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method != UNDEF) {
        uint32_t n = 0;
        embi_js_varray *vals = collect_iter(rt, a0, method, &n);
        if (!vals) {
            return EXC;
        }
        embi_js_object *o = typed_new(rt, proto, k, n);
        if (!o) {
            return EXC;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (embi_js_put(rt, o, embi_js_key_from_index(i), vals->elems[i], true) == THREW) {
                return EXC;
            }
        }
        return embi_js_from_object(o);
    }
    double len;
    if (!embi_js_length_of_array_like(rt, src, &len)) {
        return EXC;
    }
    if (len >= (double)UINT32_MAX) {
        return embi_js_throw_range_error(rt, "Invalid typed array length");
    }
    embi_js_object *o = typed_new(rt, proto, k, (size_t)len);
    if (!o) {
        return EXC;
    }
    for (double i = 0; i < len; i++) {
        embi_js_value val = embi_js_obj_get(rt, src, embi_js_key_from_index((uint32_t)i), a0);
        if (val == EXC) {
            return EXC;
        }
        if (embi_js_put(rt, o, embi_js_key_from_index((uint32_t)i), val, true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

/* %TypedArray%.from (magic 0) and %TypedArray%.of (magic 1). */
static embi_js_value ta_from_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_constructor(this_v)) {
        return embi_js_throw_type_error(rt, "this is not a constructor");
    }
    if (magic == 1) {
        embi_js_value lenv = embi_js_number((double)argc);
        embi_js_object *o = ctor_create(rt, this_v, 1, &lenv, true, (double)argc, true);
        if (!o) {
            return EXC;
        }
        for (int i = 0; i < argc; i++) {
            if (embi_js_put(rt, o, embi_js_key_from_index((uint32_t)i), argv[i], true) == THREW) {
                return EXC;
            }
        }
        return embi_js_from_object(o);
    }
    embi_js_value src = arg(argc, argv, 0);
    embi_js_value mapfn = arg(argc, argv, 1);
    embi_js_value this_arg = arg(argc, argv, 2);
    bool mapping = mapfn != UNDEF;
    if (mapping && !need_callable(rt, mapfn)) {
        return EXC;
    }
    embi_js_value method = embi_js_get_method(rt, src, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method != UNDEF) {
        uint32_t n = 0;
        embi_js_varray *vals = collect_iter(rt, src, method, &n);
        if (!vals) {
            return EXC;
        }
        embi_js_value lenv = embi_js_number((double)n);
        embi_js_object *o = ctor_create(rt, this_v, 1, &lenv, true, (double)n, true);
        if (!o) {
            return EXC;
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_js_value val = vals->elems[i];
            if (mapping) {
                embi_js_value args[2] = {val, embi_js_number((double)i)};
                val = call_fn(rt, mapfn, this_arg, 2, args);
                if (val == EXC) {
                    return EXC;
                }
            }
            if (embi_js_put(rt, o, embi_js_key_from_index(i), val, true) == THREW) {
                return EXC;
            }
        }
        return embi_js_from_object(o);
    }
    embi_js_object *array_like = embi_js_to_object(rt, src);
    if (!array_like) {
        return EXC;
    }
    double len;
    if (!embi_js_length_of_array_like(rt, array_like, &len)) {
        return EXC;
    }
    embi_js_value lenv = embi_js_number(len);
    embi_js_object *o = ctor_create(rt, this_v, 1, &lenv, true, len, true);
    if (!o) {
        return EXC;
    }
    for (double i = 0; i < len; i++) {
        uint32_t ki = (uint32_t)i;
        embi_js_value val = embi_js_obj_get(rt, array_like, embi_js_key_from_index(ki), src);
        if (val == EXC) {
            return EXC;
        }
        if (mapping) {
            embi_js_value args[2] = {val, embi_js_number(i)};
            val = call_fn(rt, mapfn, this_arg, 2, args);
            if (val == EXC) {
                return EXC;
            }
        }
        if (embi_js_put(rt, o, embi_js_key_from_index(ki), val, true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

static embi_js_value species_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

/* ------------------------------------------------------------------ prototype accessors */

static embi_js_value ta_buffer(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    if (!is_ta(this_v)) {
        return embi_js_throw_type_error(r->rt, "this is not a typed array");
    }
    embi_js_varray *v = embi_js_as_object(this_v)->internal;
    return v->elems[EMBI_JS_VIEW_BUFFER];
}

/* byteLength (magic 0), byteOffset (1), length (2): zero when out of bounds. */
static embi_js_value ta_extent(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    if (!is_ta(this_v)) {
        return embi_js_throw_type_error(r->rt, "this is not a typed array");
    }
    embi_js_varray *v = embi_js_as_object(this_v)->internal;
    size_t esz = esz_of(v);
    size_t len = 0;
    bool in = embi_js_view_len(v, esz, &len);
    switch (magic) {
    case 0:
        return num(in ? (double)(len * esz) : 0.0);
    case 1:
        return num(in ? (double)embi_js_view_offset(v) : 0.0);
    default:
        return num(in ? (double)len : 0.0);
    }
}

static embi_js_value ta_tag(embi_js_realm *r, embi_js_value this_v, int argc,
                            const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    if (!is_ta(this_v)) {
        return UNDEF;
    }
    kind_t k = kind_of(embi_js_as_object(this_v)->internal);
    return str_val(r->rt, kinds[k].name);
}

/* ------------------------------------------------------------------ prototype methods */

static embi_js_value ta_at(embi_js_realm *r, embi_js_value this_v, int argc,
                           const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    double rel;
    if (!embi_js_to_integer_or_infinity(rt, arg(argc, argv, 0), &rel)) {
        return EXC;
    }
    double k = rel >= 0 ? rel : (double)len + rel;
    if (k < 0 || k >= (double)len) {
        return UNDEF;
    }
    return get_at(rt, o, k);
}

static embi_js_value ta_copy_within(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_write(rt, this_v, &o, &len)) {
        return EXC;
    }
    double dl = (double)len;
    double to, from, fin;
    if (!rel_idx(rt, arg(argc, argv, 0), dl, 0, &to) ||
        !rel_idx(rt, arg(argc, argv, 1), dl, 0, &from) ||
        !rel_idx(rt, arg(argc, argv, 2), dl, dl, &fin)) {
        return EXC;
    }
    double count = fin - from;
    if (dl - to < count) {
        count = dl - to;
    }
    if (count > 0) {
        embi_js_varray *v = o->internal;
        size_t cur = 0;
        if (!embi_js_view_len(v, esz_of(v), &cur)) {
            return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        }
        size_t esz = esz_of(v);
        size_t off = embi_js_view_offset(v);
        size_t limit = cur * esz + off;
        size_t fb = off + (size_t)from * esz;
        size_t tb = off + (size_t)to * esz;
        size_t cb = (size_t)count * esz;
        /* The count is clamped to what is left of the possibly shrunk buffer (the test262
         * snapshot's coerced-target-start-end-shrink.js pins this, not the older byte-limit loop). */
        size_t n = 0;
        if (fb < limit && tb < limit) {
            n = cb;
            if (limit - fb < n) {
                n = limit - fb;
            }
            if (limit - tb < n) {
                n = limit - tb;
            }
        }
        if (n > 0) {
            uint8_t *base = embi_js_view_base(v);
            memmove(base + (tb - off), base + (fb - off), n);
        }
    }
    return this_v;
}

/* entries (magic 0), keys (1), values (2): the Array iterators over this. */
static embi_js_value ta_iters(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    enum embi_js_bin_intr w = magic == 0   ? EMBI_JS_BI_TA_ENTRIES
                              : magic == 1 ? EMBI_JS_BI_TA_KEYS
                                           : EMBI_JS_BI_TA_VALUES;
    return embi_js_call(rt, embi_js_binary_intrinsic(rt, w), this_v, 0, nullptr);
}

/* every (0), some (1), forEach (2), find (3), findIndex (4), findLast (5), findLastIndex (6). */
static embi_js_value ta_callback(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_value f = arg(argc, argv, 0);
    embi_js_value this_arg = arg(argc, argv, 1);
    if (!need_callable(rt, f)) {
        return EXC;
    }
    bool rev = magic == 5 || magic == 6;
    for (size_t j = 0; j < len; j++) {
        size_t k = rev ? len - 1 - j : j;
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value args[3] = {v, embi_js_number((double)k), this_v};
        embi_js_value res = call_fn(rt, f, this_arg, 3, args);
        if (res == EXC) {
            return EXC;
        }
        bool t = embi_js_to_boolean(res);
        switch (magic) {
        case 0:
            if (!t) {
                return EMBI_JS_FALSE;
            }
            break;
        case 1:
            if (t) {
                return EMBI_JS_TRUE;
            }
            break;
        case 3:
        case 5:
            if (t) {
                return v;
            }
            break;
        case 4:
        case 6:
            if (t) {
                return embi_js_number((double)k);
            }
            break;
        default:
            break;
        }
    }
    switch (magic) {
    case 0:
        return EMBI_JS_TRUE;
    case 1:
        return EMBI_JS_FALSE;
    case 4:
    case 6:
        return embi_js_from_int32(-1);
    default:
        return UNDEF;
    }
}

static embi_js_value ta_fill(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_write(rt, this_v, &o, &len)) {
        return EXC;
    }
    uint64_t bits;
    if (!embi_js_bin_coerce(rt, kind_of(o->internal), arg(argc, argv, 0), &bits)) {
        return EXC;
    }
    double dl = (double)len;
    double s, e;
    if (!rel_idx(rt, arg(argc, argv, 1), dl, 0, &s) ||
        !rel_idx(rt, arg(argc, argv, 2), dl, dl, &e)) {
        return EXC;
    }
    size_t cur = 0;
    if (!embi_js_view_len(o->internal, esz_of(o->internal), &cur)) {
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    if (e > (double)cur) {
        e = (double)cur;
    }
    for (double i = s; i < e; i++) {
        write_bits(o->internal, (size_t)i, bits);
    }
    return this_v;
}

static embi_js_value ta_filter(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_value f = arg(argc, argv, 0);
    embi_js_value this_arg = arg(argc, argv, 1);
    if (!need_callable(rt, f)) {
        return EXC;
    }
    embi_js_varray *kept = embi_js_varray_new(rt, 8);
    uint32_t n = 0;
    for (size_t k = 0; k < len; k++) {
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value args[3] = {v, embi_js_number((double)k), this_v};
        embi_js_value res = call_fn(rt, f, this_arg, 3, args);
        if (res == EXC) {
            return EXC;
        }
        if (embi_js_to_boolean(res)) {
            if (n == kept->hdr.aux) {
                kept = embi_js_varray_grow(rt, kept, n ? n * 2 : 8);
            }
            EMBI_JS_STORE(kept, kept->elems[n++], v);
        }
    }
    embi_js_value lenv = embi_js_number((double)n);
    embi_js_object *a = species_create(rt, o, 1, &lenv, true, (double)n, true);
    if (!a) {
        return EXC;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (embi_js_put(rt, a, embi_js_key_from_index(i), kept->elems[i], true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

/* HasProperty of the integer index d (YES, NO or THREW). */
static int has_idx(embi_js_runtime *rt, embi_js_object *o, double d)
{
    return embi_js_obj_has_property(rt, o, embi_js_key_from_index((uint32_t)d));
}

/* includes (magic 0), indexOf (1), lastIndexOf (2). */
static embi_js_value ta_search(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_value target = arg(argc, argv, 0);
    double dl = (double)len;
    if (len == 0) {
        return magic == 0 ? EMBI_JS_FALSE : embi_js_from_int32(-1);
    }
    if (magic == 2) {
        double n = dl - 1;
        if (argc > 1) {
            if (!embi_js_to_integer_or_infinity(rt, argv[1], &n)) {
                return EXC;
            }
        }
        if (n == -INFINITY) {
            return embi_js_from_int32(-1);
        }
        double k = n >= 0 ? (n < dl - 1 ? n : dl - 1) : dl + n;
        for (; k >= 0; k--) {
            int has = has_idx(rt, o, k);
            if (has == THREW) {
                return EXC;
            }
            if (has == YES) {
                embi_js_value e = get_at(rt, o, k);
                if (embi_js_strict_equal(e, target)) {
                    return embi_js_number(k);
                }
            }
        }
        return embi_js_from_int32(-1);
    }
    double n = 0;
    if (argc > 1) {
        if (!embi_js_to_integer_or_infinity(rt, argv[1], &n)) {
            return EXC;
        }
    }
    if (n == INFINITY) {
        return magic == 0 ? EMBI_JS_FALSE : embi_js_from_int32(-1);
    }
    if (n == -INFINITY) {
        n = 0;
    }
    double k = n >= 0 ? n : dl + n;
    if (k < 0) {
        k = 0;
    }
    for (; k < dl; k++) {
        if (magic == 0) {
            embi_js_value e = get_at(rt, o, k);
            if (embi_js_same_value_zero(e, target)) {
                return EMBI_JS_TRUE;
            }
        } else {
            int has = has_idx(rt, o, k);
            if (has == THREW) {
                return EXC;
            }
            if (has == YES) {
                embi_js_value e = get_at(rt, o, k);
                if (embi_js_strict_equal(e, target)) {
                    return embi_js_number(k);
                }
            }
        }
    }
    return magic == 0 ? EMBI_JS_FALSE : embi_js_from_int32(-1);
}

typedef EMBI_VEC(uint16_t) ta_u16_vec;

/* Appends the code units of s to v. Returns false when the result would exceed a string. */
static bool ta_u16_append(ta_u16_vec *v, const embi_js_string *s)
{
    uint32_t n = embi_js_str_len(s);
    if (n == 0) {
        return true;
    }
    if ((uint64_t)v->len + n > EMBI_JS_STR_MAX_LEN) {
        return false;
    }
    embi_vec_reserve(v, v->len + n);
    if (embi_js_str_is_wide(s)) {
        memcpy(v->data + v->len, embi_js_str_wide(s), (size_t)n * sizeof(uint16_t));
    } else {
        const uint8_t *p = embi_js_str_latin1(s);
        for (uint32_t i = 0; i < n; i++) {
            v->data[v->len + i] = p[i];
        }
    }
    v->len += n;
    return true;
}

static embi_js_value ta_join_core(embi_js_runtime *rt, embi_js_object *o, size_t len,
                                  embi_js_string *sep, bool locale)
{
    ta_u16_vec out = {nullptr, 0, 0};
    embi_js_value result = EXC;
    for (size_t k = 0; k < len; k++) {
        if (k > 0 && !ta_u16_append(&out, sep)) {
            (void)embi_js_throw_range_error(rt, "Invalid string length");
            goto done;
        }
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            goto done;
        }
        if (v == UNDEF || (locale && v == EMBI_JS_NULL)) {
            continue;
        }
        if (locale) {
            v = embi_js_invoke(rt, v, embi_js_key_c(rt, "toLocaleString"), 0, nullptr);
            if (v == EXC) {
                goto done;
            }
        }
        embi_js_string *s = to_str(rt, v);
        if (!s) {
            goto done;
        }
        if (!ta_u16_append(&out, s)) {
            (void)embi_js_throw_range_error(rt, "Invalid string length");
            goto done;
        }
    }
    {
        static const uint16_t empty[1] = {0};
        embi_js_string *res = embi_js_str_new_utf16(rt, out.len ? out.data : empty, out.len);
        if (res) {
            result = embi_js_from_string(res);
        }
    }
done:
    embi_vec_free(&out);
    return result;
}

static embi_js_value ta_join(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_string *sep = nullptr;
    if (arg(argc, argv, 0) == UNDEF) {
        sep = embi_js_atom_intern_utf8(rt, ",", 1);
    } else {
        sep = to_str(rt, argv[0]);
        if (!sep) {
            return EXC;
        }
    }
    return ta_join_core(rt, o, len, sep, false);
}

static embi_js_value ta_to_locale(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_string *comma = embi_js_atom_intern_utf8(rt, ",", 1);
    return ta_join_core(rt, o, len, comma, true);
}

static embi_js_value ta_reduce(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_value f = arg(argc, argv, 0);
    if (!need_callable(rt, f)) {
        return EXC;
    }
    bool rev = magic == 1;
    if (len == 0 && argc < 2) {
        return embi_js_throw_type_error(rt, "Reduce of empty typed array with no initial value");
    }
    size_t j = 0;
    embi_js_value acc;
    if (argc >= 2) {
        acc = argv[1];
    } else {
        acc = get_at(rt, o, (double)(rev ? len - 1 : 0));
        if (acc == EXC) {
            return EXC;
        }
        j = 1;
    }
    for (; j < len; j++) {
        size_t k = rev ? len - 1 - j : j;
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value args[4] = {acc, v, embi_js_number((double)k), this_v};
        acc = call_fn(rt, f, UNDEF, 4, args);
        if (acc == EXC) {
            return EXC;
        }
    }
    return acc;
}

static embi_js_value ta_map(embi_js_realm *r, embi_js_value this_v, int argc,
                            const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_value f = arg(argc, argv, 0);
    embi_js_value this_arg = arg(argc, argv, 1);
    if (!need_callable(rt, f)) {
        return EXC;
    }
    embi_js_value lenv = embi_js_number((double)len);
    embi_js_object *a = species_create(rt, o, 1, &lenv, true, (double)len, true);
    if (!a) {
        return EXC;
    }
    for (size_t k = 0; k < len; k++) {
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value args[3] = {v, embi_js_number((double)k), this_v};
        embi_js_value mapped = call_fn(rt, f, this_arg, 3, args);
        if (mapped == EXC) {
            return EXC;
        }
        if (embi_js_put(rt, a, embi_js_key_from_index((uint32_t)k), mapped, true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

static embi_js_value ta_reverse(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_write(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_varray *v = o->internal;
    size_t n = esz_of(v);
    for (size_t lo = 0, hi = len; lo + 1 < hi; lo++, hi--) {
        uint8_t *base = embi_js_view_base(v);
        uint8_t *a = base + lo * n;
        uint8_t *b = base + (hi - 1) * n;
        uint64_t x = embi_js_bin_load(a, n, false);
        uint64_t y = embi_js_bin_load(b, n, false);
        embi_js_bin_store(a, n, y, false);
        embi_js_bin_store(b, n, x, false);
    }
    return this_v;
}

/* set(source, offset). */
static embi_js_value ta_set_method(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_ta(this_v)) {
        return embi_js_throw_type_error(rt, "this is not a typed array");
    }
    embi_js_object *target = embi_js_as_object(this_v);
    if (embi_js_view_abuf(target->internal)->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
    }
    double off;
    if (!embi_js_to_integer_or_infinity(rt, arg(argc, argv, 1), &off)) {
        return EXC;
    }
    if (off < 0) {
        return embi_js_throw_range_error(rt, "offset is out of bounds");
    }
    embi_js_value src = arg(argc, argv, 0);
    size_t tlen;
    if (!embi_js_view_len(target->internal, esz_of(target->internal), &tlen)) {
        return embi_js_throw_type_error(rt, "target typed array is detached or out of bounds");
    }
    if (is_ta(src)) {
        embi_js_object *so = embi_js_as_object(src);
        embi_js_varray *sv = so->internal;
        size_t slen;
        if (!embi_js_view_len(sv, esz_of(sv), &slen)) {
            return embi_js_throw_type_error(rt, "source typed array is detached or out of bounds");
        }
        kind_t sk = kind_of(sv);
        kind_t tk = kind_of(target->internal);
        if (embi_js_bin_is_big(sk) != embi_js_bin_is_big(tk)) {
            return embi_js_throw_type_error(rt, "content types of the typed arrays differ");
        }
        if (off == INFINITY || (double)slen + off > (double)tlen) {
            return embi_js_throw_range_error(rt, "source is too large");
        }
        size_t o = (size_t)off;
        if (sk == tk) {
            size_t n = slen * esz_of(sv);
            if (n > 0) {
                uint8_t *dst = embi_js_view_base(target->internal) + o * esz_of(sv);
                memmove(dst, embi_js_view_base(sv), n);
            }
            return UNDEF;
        }
        /* Different kinds: snapshot the source values first, since the buffers may overlap. */
        embi_js_varray *snap = embi_js_varray_new(rt, (uint32_t)slen);
        for (size_t i = 0; i < slen; i++) {
            EMBI_JS_STORE(snap, snap->elems[i], read_elem(rt, sv, i));
            if (snap->elems[i] == EXC) {
                return EXC;
            }
        }
        for (size_t i = 0; i < slen; i++) {
            uint64_t bits;
            if (!embi_js_bin_coerce(rt, tk, snap->elems[i], &bits)) {
                return EXC;
            }
            write_bits(target->internal, o + i, bits);
        }
        return UNDEF;
    }
    embi_js_object *so = embi_js_to_object(rt, src);
    if (!so) {
        return EXC;
    }
    double slen;
    if (!embi_js_length_of_array_like(rt, so, &slen)) {
        return EXC;
    }
    if (off == INFINITY || slen + off > (double)tlen) {
        return embi_js_throw_range_error(rt, "source is too large");
    }
    for (double i = 0; i < slen; i++) {
        embi_js_value v = embi_js_obj_get(rt, so, embi_js_key_from_index((uint32_t)i), embi_js_from_object(so));
        if (v == EXC) {
            return EXC;
        }
        if (set_element(rt, target, off + i, v) == THREW) {
            return EXC;
        }
    }
    return UNDEF;
}

static embi_js_value ta_slice(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    double dl = (double)len;
    double k, fin;
    if (!rel_idx(rt, arg(argc, argv, 0), dl, 0, &k) ||
        !rel_idx(rt, arg(argc, argv, 1), dl, dl, &fin)) {
        return EXC;
    }
    double count = fin - k > 0 ? fin - k : 0;
    embi_js_value lenv = embi_js_number(count);
    embi_js_object *a = species_create(rt, o, 1, &lenv, true, count, true);
    if (!a) {
        return EXC;
    }
    if (count > 0) {
        size_t cur = 0;
        if (!embi_js_view_len(o->internal, esz_of(o->internal), &cur)) {
            return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        }
        if (fin > (double)cur) {
            fin = (double)cur;
        }
        count = fin - k > 0 ? fin - k : 0;
        kind_t sk = kind_of(o->internal);
        kind_t ak = kind_of(a->internal);
        if (sk == ak) {
            size_t n = (size_t)count * esz_of(o->internal);
            size_t acur = 0;
            if (!embi_js_view_len(a->internal, esz_of(a->internal), &acur) || acur < (size_t)count) {
                return embi_js_throw_type_error(rt, "species typed array is detached or too short");
            }
            if (n > 0) {
                /* A forward byte-by-byte copy, as the spec's element loop does. When the result
                 * shares the source buffer, a later element reads what an earlier one wrote, so
                 * this is deliberately not memmove. */
                const uint8_t *src = embi_js_view_base(o->internal) + (size_t)k * esz_of(o->internal);
                uint8_t *dst = embi_js_view_base(a->internal);
                for (size_t i = 0; i < n; i++) {
                    dst[i] = src[i];
                }
            }
        } else {
            for (double i = 0; i < count; i++) {
                embi_js_value v = get_at(rt, o, k + i);
                if (v == EXC) {
                    return EXC;
                }
                if (embi_js_put(rt, a, embi_js_key_from_index((uint32_t)i), v, true) == THREW) {
                    return EXC;
                }
            }
        }
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ sorting */

static int default_cmp(embi_js_value x, embi_js_value y)
{
    if (embi_js_is_bigint(x)) {
        return embi_js_bigint_compare(embi_js_as_bigint(x), embi_js_as_bigint(y));
    }
    double a = embi_js_bin_num(x);
    double b = embi_js_bin_num(y);
    if (a != a) {
        return b != b ? 0 : 1;
    }
    if (b != b) {
        return -1;
    }
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    if (a == 0.0 && b == 0.0) {
        bool na = signbit(a);
        bool nb = signbit(b);
        if (na == nb) {
            return 0;
        }
        return na ? -1 : 1;
    }
    return 0;
}

/* The sign of the comparison of x and y under cmpfn (undefined for the default order). Returns
 * false with an exception pending. */
static bool sort_cmp(embi_js_runtime *rt, embi_js_value cmpfn, embi_js_value x, embi_js_value y,
                     int *out)
{
    if (cmpfn == UNDEF) {
        *out = default_cmp(x, y);
        return true;
    }
    embi_js_value args[2] = {x, y};
    embi_js_value res = embi_js_call(rt, cmpfn, UNDEF, 2, args);
    if (res == EXC) {
        return false;
    }
    double d;
    if (!embi_js_to_number(rt, res, &d)) {
        return false;
    }
    *out = d < 0 ? -1 : (d > 0 ? 1 : 0);
    return true;
}

/* Stable bottom-up merge sort of the first n values of a. Returns the sorted array (a or a fresh
 * one), or nullptr with an exception pending. */
static embi_js_varray *merge_sort(embi_js_runtime *rt, embi_js_varray *a, uint32_t n,
                                  embi_js_value cmpfn)
{
    if (n < 2) {
        return a;
    }
    embi_js_varray *b = embi_js_varray_new(rt, n);
    for (uint64_t w = 1; w < n; w *= 2) {
        for (uint64_t lo = 0; lo < n; lo += 2 * w) {
            uint64_t mid = lo + w < n ? lo + w : n;
            uint64_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint64_t i = lo;
            uint64_t j = mid;
            uint64_t k = lo;
            while (i < mid && j < hi) {
                int c;
                if (!sort_cmp(rt, cmpfn, a->elems[j], a->elems[i], &c)) {
                    return nullptr;
                }
                if (c < 0) {
                    EMBI_JS_STORE(b, b->elems[k++], a->elems[j++]);
                } else {
                    EMBI_JS_STORE(b, b->elems[k++], a->elems[i++]);
                }
            }
            while (i < mid) {
                EMBI_JS_STORE(b, b->elems[k++], a->elems[i++]);
            }
            while (j < hi) {
                EMBI_JS_STORE(b, b->elems[k++], a->elems[j++]);
            }
        }
        embi_js_varray *t = a;
        a = b;
        b = t;
    }
    return a;
}

/* The values of o (all in bounds, since nothing runs between the read and the sort's calls). */
static embi_js_varray *read_values(embi_js_runtime *rt, embi_js_object *o, size_t len)
{
    embi_js_varray *a = embi_js_varray_new(rt, (uint32_t)len);
    for (size_t i = 0; i < len; i++) {
        EMBI_JS_STORE(a, a->elems[i], get_at(rt, o, (double)i));
    }
    return a;
}

static embi_js_value ta_sort(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value cmpfn = arg(argc, argv, 0);
    if (cmpfn != UNDEF && !need_callable(rt, cmpfn)) {
        return EXC;
    }
    embi_js_object *o;
    size_t len;
    if (!validate_write(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_varray *a = read_values(rt, o, len);
    a = merge_sort(rt, a, (uint32_t)len, cmpfn);
    if (!a) {
        return EXC;
    }
    for (size_t j = 0; j < len; j++) {
        if (embi_js_put(rt, o, embi_js_key_from_index((uint32_t)j), a->elems[j], true) == THREW) {
            return EXC;
        }
    }
    return this_v;
}

static embi_js_value ta_to_sorted(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value cmpfn = arg(argc, argv, 0);
    if (cmpfn != UNDEF && !need_callable(rt, cmpfn)) {
        return EXC;
    }
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_object *a = same_type_create(rt, o, len);
    if (!a) {
        return EXC;
    }
    embi_js_varray *vals = read_values(rt, o, len);
    vals = merge_sort(rt, vals, (uint32_t)len, cmpfn);
    if (!vals) {
        return EXC;
    }
    for (size_t j = 0; j < len; j++) {
        if (embi_js_put(rt, a, embi_js_key_from_index((uint32_t)j), vals->elems[j], true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

static embi_js_value ta_to_reversed(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    embi_js_object *a = same_type_create(rt, o, len);
    if (!a) {
        return EXC;
    }
    for (size_t k = 0; k < len; k++) {
        embi_js_value v = get_at(rt, o, (double)(len - 1 - k));
        if (v == EXC) {
            return EXC;
        }
        if (embi_js_put(rt, a, embi_js_key_from_index((uint32_t)k), v, true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

static embi_js_value ta_with(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate(rt, this_v, &o, &len)) {
        return EXC;
    }
    double rel;
    if (!embi_js_to_integer_or_infinity(rt, arg(argc, argv, 0), &rel)) {
        return EXC;
    }
    double actual = rel >= 0 ? rel : (double)len + rel;
    embi_js_value nv;
    if (kinds[kind_of(o->internal)].big) {
        nv = embi_js_to_bigint(rt, arg(argc, argv, 1));
    } else {
        double d;
        if (!embi_js_to_number(rt, arg(argc, argv, 1), &d)) {
            return EXC;
        }
        nv = embi_js_number(d);
    }
    if (nv == EXC) {
        return EXC;
    }
    size_t idx;
    if (!valid_index(o->internal, actual, &idx)) {
        return embi_js_throw_range_error(rt, "Invalid typed array index");
    }
    embi_js_object *a = same_type_create(rt, o, len);
    if (!a) {
        return EXC;
    }
    for (size_t k = 0; k < len; k++) {
        embi_js_value v = (double)k == actual ? nv : get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        if (embi_js_put(rt, a, embi_js_key_from_index((uint32_t)k), v, true) == THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

static embi_js_value ta_subarray(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!is_ta(this_v)) {
        return embi_js_throw_type_error(rt, "this is not a typed array");
    }
    embi_js_object *o = embi_js_as_object(this_v);
    embi_js_varray *v = o->internal;
    size_t esz = esz_of(v);
    size_t src_len = 0;
    bool in = embi_js_view_len(v, esz, &src_len);
    if (!in) {
        src_len = 0;
    }
    double rs;
    if (!rel_idx(rt, arg(argc, argv, 0), (double)src_len, 0, &rs)) {
        return EXC;
    }
    double begin_off = (double)embi_js_view_offset(v) + rs * (double)esz;
    embi_js_value args[3];
    int nargs;
    args[0] = v->elems[EMBI_JS_VIEW_BUFFER];
    args[1] = embi_js_number(begin_off);
    bool tracking = v->elems[EMBI_JS_VIEW_LENGTH] == UNDEF;
    if (tracking && arg(argc, argv, 1) == UNDEF) {
        nargs = 2;
    } else {
        double re;
        if (!rel_idx(rt, arg(argc, argv, 1), (double)src_len, (double)src_len, &re)) {
            return EXC;
        }
        double newlen = re - rs > 0 ? re - rs : 0;
        args[2] = embi_js_number(newlen);
        nargs = 3;
    }
    kind_t k = kind_of(v);
    embi_js_value dflt = embi_js_binary_intrinsic(rt, EMBI_JS_BI_TA_CTOR_BASE + k);
    embi_js_value c = embi_js_species_constructor(rt, o, dflt);
    if (c == EXC) {
        return EXC;
    }
    embi_js_object *a = ctor_create(rt, c, nargs, args, false, 0, false);
    if (!a) {
        return EXC;
    }
    if (embi_js_bin_is_big(kind_of(a->internal)) != embi_js_bin_is_big(k)) {
        return embi_js_throw_type_error(rt, "species constructor returned a different content type");
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ base64 and hex (ES2026) */

enum { LC_LOOSE, LC_STRICT, LC_STOP };

/* GetOptionsObject: undefined or an object, else a TypeError. Reads alphabet, then exactly one
 * of lastChunkHandling (omit is nullptr: fromBase64, setFromBase64) or omitPadding (omit is
 * non-null: toBase64). Options the method does not use are never read. */
static bool base64_options(embi_js_runtime *rt, embi_js_value opts, bool *url, int *mode,
                           bool *omit)
{
    if (opts != UNDEF && !embi_js_is_object(opts)) {
        (void)embi_js_throw_type_error(rt, "options must be an object");
        return false;
    }
    *url = false;
    *mode = LC_LOOSE;
    if (omit) {
        *omit = false;
    }
    if (opts == UNDEF) {
        return true;
    }
    embi_js_object *o = embi_js_as_object(opts);
    embi_js_value a = embi_js_obj_get(rt, o, embi_js_key_c(rt, "alphabet"), opts);
    if (a == EXC) {
        return false;
    }
    if (a != UNDEF) {
        if (str_eq(rt, a, "base64url")) {
            *url = true;
        } else if (!str_eq(rt, a, "base64")) {
            (void)embi_js_throw_type_error(rt, "alphabet must be \"base64\" or \"base64url\"");
            return false;
        }
    }
    if (omit) {
        embi_js_value p = embi_js_obj_get(rt, o, embi_js_key_c(rt, "omitPadding"), opts);
        if (p == EXC) {
            return false;
        }
        *omit = embi_js_to_boolean(p);
        return true;
    }
    embi_js_value l = embi_js_obj_get(rt, o, embi_js_key_c(rt, "lastChunkHandling"), opts);
    if (l == EXC) {
        return false;
    }
    if (l != UNDEF) {
        if (str_eq(rt, l, "strict")) {
            *mode = LC_STRICT;
        } else if (str_eq(rt, l, "stop-before-partial")) {
            *mode = LC_STOP;
        } else if (!str_eq(rt, l, "loose")) {
            (void)embi_js_throw_type_error(rt, "invalid lastChunkHandling");
            return false;
        }
    }
    return true;
}

static bool is_b64_ws(uint16_t c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

static int b64_digit(uint16_t c, bool url)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (!url) {
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
    } else {
        if (c == '-') {
            return 62;
        }
        if (c == '_') {
            return 63;
        }
    }
    return -1;
}

/* FromBase64. Decodes s into out (room for max bytes), writing the byte count and the number of
 * code units read. Returns false on a syntax error; the counts then hold the bytes decoded before
 * the error (the complete chunks), which the set methods still write (FromBase64's [[Bytes]]). */
static bool b64_decode(const embi_js_string *s, bool url, int mode, size_t max, uint8_t *out,
                       size_t *nout_p, size_t *read_p)
{
    uint32_t n = embi_js_str_len(s);
    size_t nout = 0;
    size_t read = 0;
    int acc[4] = {0, 0, 0, 0};
    int cl = 0;
    uint32_t i = 0;
#define B64_FAIL()                                                                               \
    do {                                                                                         \
        *nout_p = nout;                                                                          \
        *read_p = read;                                                                          \
        return false;                                                                            \
    } while (0)
#define B64_DONE()                                                                               \
    do {                                                                                         \
        *nout_p = nout;                                                                          \
        *read_p = read;                                                                          \
        return true;                                                                             \
    } while (0)
    if (max == 0) {
        B64_DONE();
    }
    for (;;) {
        while (i < n && is_b64_ws(embi_js_str_at(s, i))) {
            i++;
        }
        if (i >= n) {
            if (cl > 0) {
                if (mode == LC_STOP) {
                    B64_DONE();
                }
                if (mode == LC_STRICT || cl == 1) {
                    B64_FAIL();
                }
                size_t add = (size_t)cl - 1;
                out[nout] = (uint8_t)((acc[0] << 2) | (acc[1] >> 4));
                if (add == 2) {
                    out[nout + 1] = (uint8_t)(((acc[1] & 0xF) << 4) | (acc[2] >> 2));
                }
                nout += add;
            }
            read = n;
            B64_DONE();
        }
        uint16_t c = embi_js_str_at(s, i);
        i++;
        if (c == '=') {
            if (cl < 2) {
                B64_FAIL();
            }
            while (i < n && is_b64_ws(embi_js_str_at(s, i))) {
                i++;
            }
            if (cl == 2) {
                if (i >= n) {
                    if (mode == LC_STOP) {
                        B64_DONE();
                    }
                    B64_FAIL();
                }
                if (embi_js_str_at(s, i) == '=') {
                    i++;
                    while (i < n && is_b64_ws(embi_js_str_at(s, i))) {
                        i++;
                    }
                }
            }
            if (i < n) {
                B64_FAIL();
            }
            if (mode == LC_STRICT) {
                if (cl == 2 && (acc[1] & 0xF)) {
                    B64_FAIL();
                }
                if (cl == 3 && (acc[2] & 0x3)) {
                    B64_FAIL();
                }
            }
            size_t add = (size_t)cl - 1;
            out[nout] = (uint8_t)((acc[0] << 2) | (acc[1] >> 4));
            if (add == 2) {
                out[nout + 1] = (uint8_t)(((acc[1] & 0xF) << 4) | (acc[2] >> 2));
            }
            nout += add;
            read = n;
            B64_DONE();
        }
        int v = b64_digit(c, url);
        if (v < 0) {
            B64_FAIL();
        }
        size_t rem = max - nout;
        if ((rem == 1 && cl == 2) || (rem == 2 && cl == 3)) {
            B64_DONE();
        }
        acc[cl++] = v;
        if (cl == 4) {
            out[nout] = (uint8_t)((acc[0] << 2) | (acc[1] >> 4));
            out[nout + 1] = (uint8_t)(((acc[1] & 0xF) << 4) | (acc[2] >> 2));
            out[nout + 2] = (uint8_t)(((acc[2] & 0x3) << 6) | acc[3]);
            nout += 3;
            cl = 0;
            read = i;
            if (nout == max) {
                B64_DONE();
            }
        }
    }
#undef B64_DONE
#undef B64_FAIL
}

static embi_js_object *make_result(embi_js_runtime *rt, size_t read, size_t written)
{
    embi_js_object *res = embi_js_object_new_plain(rt);
    if (embi_js_create_data_property(rt, res, embi_js_key_c(rt, "read"),
                                     embi_js_number((double)read)) == THREW) {
        return nullptr;
    }
    if (embi_js_create_data_property(rt, res, embi_js_key_c(rt, "written"),
                                     embi_js_number((double)written)) == THREW) {
        return nullptr;
    }
    return res;
}

/* A Uint8Array holding a copy of bytes (n of them). */
static embi_js_value u8_from_bytes(embi_js_runtime *rt, const uint8_t *bytes, size_t n)
{
    embi_js_value proto = embi_js_binary_proto_from(rt, UNDEF, EMBI_JS_BI_TA_PROTO_BASE + EMBI_JS_TA_UINT8);
    embi_js_object *o = typed_new(rt, proto, EMBI_JS_TA_UINT8, n);
    if (!o) {
        return EXC;
    }
    if (n > 0) {
        memcpy(embi_js_view_base(o->internal), bytes, n);
    }
    return embi_js_from_object(o);
}

/* ValidateUint8Array: a Uint8Array (not Uint8ClampedArray) in this. */
static bool validate_u8(embi_js_runtime *rt, embi_js_value v, embi_js_object **out)
{
    if (!is_ta(v) || kind_of(embi_js_as_object(v)->internal) != EMBI_JS_TA_UINT8) {
        (void)embi_js_throw_type_error(rt, "this is not a Uint8Array");
        return false;
    }
    *out = embi_js_as_object(v);
    return true;
}

/* Uint8Array.fromBase64(string, options). */
static embi_js_value u8_from_base64(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value sv = arg(argc, argv, 0);
    if (!embi_js_is_string(sv)) {
        return embi_js_throw_type_error(rt, "fromBase64 requires a string");
    }
    bool url;
    int mode;
    if (!base64_options(rt, arg(argc, argv, 1), &url, &mode, nullptr)) {
        return EXC;
    }
    const embi_js_string *s = embi_js_str_flat(rt, sv);
    size_t cap = (size_t)embi_js_str_len(s) / 4 * 3 + 3;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    size_t nout = 0;
    size_t read = 0;
    if (!b64_decode(s, url, mode, SIZE_MAX, buf, &nout, &read)) {
        embi_free(buf, cap);
        return embi_js_throw_syntax_error(rt, "invalid base64 input");
    }
    embi_js_value out = u8_from_bytes(rt, buf, nout);
    embi_free(buf, cap);
    return out;
}

/* Uint8Array.fromHex(string). */
static int hex_val(uint16_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static embi_js_value u8_from_hex(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value sv = arg(argc, argv, 0);
    if (!embi_js_is_string(sv)) {
        return embi_js_throw_type_error(rt, "fromHex requires a string");
    }
    const embi_js_string *s = embi_js_str_flat(rt, sv);
    uint32_t n = embi_js_str_len(s);
    if (n % 2 != 0) {
        return embi_js_throw_syntax_error(rt, "hex string has an odd length");
    }
    size_t cap = n / 2 + 1;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    for (uint32_t i = 0; i < n / 2; i++) {
        int hi = hex_val(embi_js_str_at(s, 2 * i));
        int lo = hex_val(embi_js_str_at(s, 2 * i + 1));
        if (hi < 0 || lo < 0) {
            embi_free(buf, cap);
            return embi_js_throw_syntax_error(rt, "invalid hex digit");
        }
        buf[i] = (uint8_t)(hi * 16 + lo);
    }
    embi_js_value out = u8_from_bytes(rt, buf, n / 2);
    embi_free(buf, cap);
    return out;
}

/* Uint8Array.prototype.setFromBase64(string, options). */
static embi_js_value u8_set_from_base64(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    if (!validate_u8(rt, this_v, &o)) {
        return EXC;
    }
    if (embi_js_view_abuf(o->internal)->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
    }
    embi_js_value sv = arg(argc, argv, 0);
    if (!embi_js_is_string(sv)) {
        return embi_js_throw_type_error(rt, "setFromBase64 requires a string");
    }
    bool url;
    int mode;
    if (!base64_options(rt, arg(argc, argv, 1), &url, &mode, nullptr)) {
        return EXC;
    }
    size_t max = 0;
    if (!embi_js_view_len(o->internal, 1, &max)) {
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    const embi_js_string *s = embi_js_str_flat(rt, sv);
    /* The decoded size is bounded by the input length; do not size the temporary by the target. */
    size_t dmax = (size_t)embi_js_str_len(s) / 4 * 3 + 3;
    size_t lim = max < dmax ? max : dmax;
    size_t cap = lim + 3;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    size_t nout = 0;
    size_t read = 0;
    /* On a syntax error the bytes decoded so far are still written, and the error is thrown
     * after the write (SetUint8ArrayFromBase64 with FromBase64's [[Bytes]] and [[Error]]). */
    bool ok = b64_decode(s, url, mode, lim, buf, &nout, &read);
    size_t cur = 0;
    if (!embi_js_view_len(o->internal, 1, &cur)) {
        embi_free(buf, cap);
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    if (nout > cur) {
        nout = cur;
    }
    if (nout > 0) {
        memcpy(embi_js_view_base(o->internal), buf, nout);
    }
    embi_free(buf, cap);
    if (!ok) {
        return embi_js_throw_syntax_error(rt, "invalid base64 input");
    }
    embi_js_object *res = make_result(rt, read, nout);
    return res ? embi_js_from_object(res) : EXC;
}

/* Uint8Array.prototype.setFromHex(string). */
static embi_js_value u8_set_from_hex(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    if (!validate_u8(rt, this_v, &o)) {
        return EXC;
    }
    if (embi_js_view_abuf(o->internal)->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
    }
    embi_js_value sv = arg(argc, argv, 0);
    if (!embi_js_is_string(sv)) {
        return embi_js_throw_type_error(rt, "setFromHex requires a string");
    }
    size_t max = 0;
    if (!embi_js_view_len(o->internal, 1, &max)) {
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    const embi_js_string *s = embi_js_str_flat(rt, sv);
    uint32_t n = embi_js_str_len(s);
    /* FromHex rejects an odd-length string before decoding anything. */
    if (n % 2 != 0) {
        return embi_js_throw_syntax_error(rt, "hex string has an odd length");
    }
    size_t lim = max < (size_t)n / 2 ? max : (size_t)n / 2;
    size_t cap = lim + 1;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    size_t written = 0;
    size_t read = 0;
    bool bad = false;
    while (read + 2 <= n && written < lim) {
        int hi = hex_val(embi_js_str_at(s, (uint32_t)read));
        int lo = hex_val(embi_js_str_at(s, (uint32_t)read + 1));
        if (hi < 0 || lo < 0) {
            bad = true;
            break;
        }
        buf[written++] = (uint8_t)(hi * 16 + lo);
        read += 2;
    }
    size_t cur = 0;
    if (!embi_js_view_len(o->internal, 1, &cur)) {
        embi_free(buf, cap);
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    if (written > cur) {
        written = cur;
    }
    if (written > 0) {
        memcpy(embi_js_view_base(o->internal), buf, written);
    }
    embi_free(buf, cap);
    if (bad) {
        return embi_js_throw_syntax_error(rt, "invalid hex digit");
    }
    embi_js_object *res = make_result(rt, read, written);
    return res ? embi_js_from_object(res) : EXC;
}

/* Uint8Array.prototype.toHex(). */
static embi_js_value u8_to_hex(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    if (!validate_u8(rt, this_v, &o)) {
        return EXC;
    }
    size_t len;
    if (!embi_js_view_len(o->internal, 1, &len)) {
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    static const char digits[] = "0123456789abcdef";
    if (len > EMBI_JS_STR_MAX_LEN / 2) {
        return embi_js_throw_range_error(rt, "Invalid string length");
    }
    size_t cap = len * 2 + 1;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    const uint8_t *src = embi_js_view_base(o->internal);
    for (size_t i = 0; i < len; i++) {
        buf[2 * i] = (uint8_t)digits[src[i] >> 4];
        buf[2 * i + 1] = (uint8_t)digits[src[i] & 0xF];
    }
    embi_js_value out = embi_js_from_string(embi_js_str_new_latin1(rt, buf, len * 2));
    embi_free(buf, cap);
    return out;
}

/* Uint8Array.prototype.toBase64(options). */
static embi_js_value u8_to_base64(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    if (!validate_u8(rt, this_v, &o)) {
        return EXC;
    }
    bool url;
    int mode;
    bool omit;
    if (!base64_options(rt, arg(argc, argv, 0), &url, &mode, &omit)) {
        return EXC;
    }
    size_t len;
    if (!embi_js_view_len(o->internal, 1, &len)) {
        return embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
    }
    static const char std_tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char url_tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *tbl = url ? url_tbl : std_tbl;
    size_t outlen = len / 3 * 4;
    if (len % 3 != 0) {
        outlen += omit ? len % 3 + 1 : 4;
    }
    if (outlen > EMBI_JS_STR_MAX_LEN) {
        return embi_js_throw_range_error(rt, "Invalid string length");
    }
    size_t cap = (len + 2) / 3 * 4 + 4;
    uint8_t *buf = embi_alloc(cap);
    if (!buf) {
        return embi_js_throw_range_error(rt, "out of memory");
    }
    const uint8_t *in = embi_js_view_base(o->internal);
    size_t w = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        buf[w++] = (uint8_t)tbl[(v >> 18) & 63];
        buf[w++] = (uint8_t)tbl[(v >> 12) & 63];
        buf[w++] = (uint8_t)tbl[(v >> 6) & 63];
        buf[w++] = (uint8_t)tbl[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        buf[w++] = (uint8_t)tbl[(v >> 18) & 63];
        buf[w++] = (uint8_t)tbl[(v >> 12) & 63];
        if (!omit) {
            buf[w++] = '=';
            buf[w++] = '=';
        }
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        buf[w++] = (uint8_t)tbl[(v >> 18) & 63];
        buf[w++] = (uint8_t)tbl[(v >> 12) & 63];
        buf[w++] = (uint8_t)tbl[(v >> 6) & 63];
        if (!omit) {
            buf[w++] = '=';
        }
    }
    embi_js_value out = embi_js_from_string(embi_js_str_new_latin1(rt, buf, w));
    embi_free(buf, cap);
    return out;
}

/* ------------------------------------------------------------------ setup */

static embi_js_object *define_fn(embi_js_realm *r, embi_js_object *o, const char *name,
                                 embi_js_native fn, int length, int magic)
{
    embi_js_object *f = embi_js_native_function_new(r, name, length, fn, magic);
    embi_js_define_raw(r->rt, o, embi_js_key_c(r->rt, name), embi_js_from_object(f), ATTR_WC);
    return f;
}

static void define_getter(embi_js_realm *r, embi_js_object *o, const char *name,
                          embi_js_native fn, int magic)
{
    char buf[64];
    SDL_snprintf(buf, sizeof buf, "get %s", name);
    embi_js_object *g = embi_js_native_function_new(r, buf, 0, fn, magic);
    embi_js_define_accessor_raw(r->rt, o, embi_js_key_c(r->rt, name), embi_js_from_object(g),
                                UNDEF, ATTR_C);
}

void embi_js_init_builtins_typedarray(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *intr = embi_js_binary_intrinsics(rt);
    embi_js_object *array_proto = rt->protos[EMBI_JS_PROTO_ARRAY] == EMBI_JS_EMPTY
                                      ? nullptr
                                      : embi_js_as_object(rt->protos[EMBI_JS_PROTO_ARRAY]);

    /* %TypedArray% and its prototype. */
    embi_js_object *ta_proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_object *ta_ctor = embi_js_native_constructor_new(r, "TypedArray", 0, ta_abstract, 0);
    embi_js_define_raw(rt, ta_ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(ta_proto), 0);
    embi_js_define_raw(rt, ta_proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ta_ctor), ATTR_WC);
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TYPEDARRAY_PROTO], embi_js_from_object(ta_proto));
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TYPEDARRAY_CTOR], embi_js_from_object(ta_ctor));

    define_fn(r, ta_ctor, "from", ta_from_of, 1, 0);
    define_fn(r, ta_ctor, "of", ta_from_of, 0, 1);
    {
        embi_js_object *g = embi_js_native_function_new(r, "get [Symbol.species]", 0, species_get, 0);
        embi_js_define_accessor_raw(rt, ta_ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES),
                                    embi_js_from_object(g), UNDEF, ATTR_C);
    }

    define_getter(r, ta_proto, "buffer", ta_buffer, 0);
    define_getter(r, ta_proto, "byteLength", ta_extent, 0);
    define_getter(r, ta_proto, "byteOffset", ta_extent, 1);
    define_getter(r, ta_proto, "length", ta_extent, 2);
    {
        embi_js_object *g = embi_js_native_function_new(r, "get [Symbol.toStringTag]", 0, ta_tag, 0);
        embi_js_define_accessor_raw(rt, ta_proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                                    embi_js_from_object(g), UNDEF, ATTR_C);
    }

    define_fn(r, ta_proto, "at", ta_at, 1, 0);
    define_fn(r, ta_proto, "copyWithin", ta_copy_within, 2, 0);
    define_fn(r, ta_proto, "entries", ta_iters, 0, 0);
    define_fn(r, ta_proto, "every", ta_callback, 1, 0);
    define_fn(r, ta_proto, "fill", ta_fill, 1, 0);
    define_fn(r, ta_proto, "filter", ta_filter, 1, 0);
    define_fn(r, ta_proto, "find", ta_callback, 1, 3);
    define_fn(r, ta_proto, "findIndex", ta_callback, 1, 4);
    define_fn(r, ta_proto, "findLast", ta_callback, 1, 5);
    define_fn(r, ta_proto, "findLastIndex", ta_callback, 1, 6);
    define_fn(r, ta_proto, "forEach", ta_callback, 1, 2);
    define_fn(r, ta_proto, "includes", ta_search, 1, 0);
    define_fn(r, ta_proto, "indexOf", ta_search, 1, 1);
    define_fn(r, ta_proto, "join", ta_join, 1, 0);
    define_fn(r, ta_proto, "keys", ta_iters, 0, 1);
    define_fn(r, ta_proto, "lastIndexOf", ta_search, 1, 2);
    define_fn(r, ta_proto, "map", ta_map, 1, 0);
    define_fn(r, ta_proto, "reduce", ta_reduce, 1, 0);
    define_fn(r, ta_proto, "reduceRight", ta_reduce, 1, 1);
    define_fn(r, ta_proto, "reverse", ta_reverse, 0, 0);
    define_fn(r, ta_proto, "set", ta_set_method, 1, 0);
    define_fn(r, ta_proto, "slice", ta_slice, 2, 0);
    define_fn(r, ta_proto, "some", ta_callback, 1, 1);
    define_fn(r, ta_proto, "sort", ta_sort, 1, 0);
    define_fn(r, ta_proto, "subarray", ta_subarray, 2, 0);
    define_fn(r, ta_proto, "toLocaleString", ta_to_locale, 0, 0);
    define_fn(r, ta_proto, "toReversed", ta_to_reversed, 0, 0);
    define_fn(r, ta_proto, "toSorted", ta_to_sorted, 1, 0);
    define_fn(r, ta_proto, "with", ta_with, 2, 0);
    embi_js_object *values = define_fn(r, ta_proto, "values", ta_iters, 0, 2);
    embi_js_define_raw(rt, ta_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(values), ATTR_WC);

    /* The intrinsic Array functions that the iterators and toString share. */
    if (array_proto) {
        embi_js_value keys_fn = embi_js_obj_get(rt, array_proto, embi_js_key_c(rt, "keys"),
                                                embi_js_from_object(array_proto));
        embi_js_value entries_fn = embi_js_obj_get(rt, array_proto, embi_js_key_c(rt, "entries"),
                                                   embi_js_from_object(array_proto));
        embi_js_value values_fn = embi_js_obj_get(rt, array_proto, embi_js_key_c(rt, "values"),
                                                  embi_js_from_object(array_proto));
        embi_js_value tostr = embi_js_obj_get(rt, array_proto, embi_js_key_c(rt, "toString"),
                                              embi_js_from_object(array_proto));
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TA_KEYS], keys_fn);
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TA_ENTRIES], entries_fn);
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TA_VALUES], values_fn);
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_ARRAY_TOSTRING], tostr);
        embi_js_define_raw(rt, ta_proto, embi_js_key_c(rt, "toString"), tostr, ATTR_WC);
    }

    /* The twelve concrete constructors. */
    embi_js_object *u8_ctor = nullptr;
    embi_js_object *u8_proto = nullptr;
    for (int k = 0; k < EMBI_JS_TA_KINDS; k++) {
        const char *name = embi_js_bin_name((kind_t)k);
        size_t size = embi_js_bin_size((kind_t)k);
        embi_js_object *proto = embi_js_object_new(rt, embi_js_from_object(ta_proto));
        embi_js_object *ctor = embi_js_native_constructor_new(r, name, 3, ta_construct, k);
        embi_js_obj_set_prototype_of(rt, ctor, embi_js_from_object(ta_ctor));
        embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                           embi_js_from_object(proto), 0);
        embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                           embi_js_from_object(ctor), ATTR_WC);
        embi_js_define_raw(rt, ctor, embi_js_key_c(rt, "BYTES_PER_ELEMENT"),
                           embi_js_number((double)size), 0);
        embi_js_define_raw(rt, proto, embi_js_key_c(rt, "BYTES_PER_ELEMENT"),
                           embi_js_number((double)size), 0);
        embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor),
                           ATTR_WC);
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TA_PROTO_BASE + k], embi_js_from_object(proto));
        EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_TA_CTOR_BASE + k], embi_js_from_object(ctor));
        if (k == EMBI_JS_TA_UINT8) {
            u8_ctor = ctor;
            u8_proto = proto;
        }
    }

    /* Uint8Array's base64 and hex methods (ES2026). */
    define_fn(r, u8_ctor, "fromBase64", u8_from_base64, 1, 0);
    define_fn(r, u8_ctor, "fromHex", u8_from_hex, 1, 0);
    define_fn(r, u8_proto, "setFromBase64", u8_set_from_base64, 1, 0);
    define_fn(r, u8_proto, "setFromHex", u8_set_from_hex, 1, 0);
    define_fn(r, u8_proto, "toBase64", u8_to_base64, 0, 0);
    define_fn(r, u8_proto, "toHex", u8_to_hex, 0, 0);
}
