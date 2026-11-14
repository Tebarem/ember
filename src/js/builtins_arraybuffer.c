/*
 * ArrayBuffer and SharedArrayBuffer (ECMA-262 25.1 and 25.2) with resizable and growable buffers,
 * transfer, and the immutable-buffer extension. This file also owns the
 * binary intrinsic record and the allocation and ToIndex helpers that the TypedArray, DataView and
 * Atomics builtins share (js/typedarray.h).
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
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

static embi_js_value arg(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value str_val(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* ------------------------------------------------------------------ intrinsic record */

/* The record is found again by scanning the runtime's roots for this function, so no runtime or
 * realm field is needed. Its user pointer is the record cell itself.
 *
 * Known limitation (J4e review item 10): the record is per runtime, not per realm. A second realm
 * created on the same runtime runs embi_js_init_builtins_arraybuffer again and overwrites the
 * record, so the TypedArray, ArrayBuffer and DataView default prototypes and species defaults
 * then point at the newest realm. Per-realm storage needs a field in runtime.h (owned by the
 * object model), so it is deferred; the scan also stays linear in the root count until then. */
static void intrinsics_root(embi_js_tracer *t, void *user)
{
    embi_js_mark_cell(t, (embi_js_cell *)user);
}

embi_js_varray *embi_js_binary_intrinsics(embi_js_runtime *rt)
{
    for (uint32_t i = 0; i < rt->roots.len; i++) {
        if (rt->roots.data[i].fn == intrinsics_root) {
            return (embi_js_varray *)rt->roots.data[i].user;
        }
    }
    embi_js_varray *v = embi_js_varray_new(rt, EMBI_JS_BI_COUNT);
    for (uint32_t i = 0; i < EMBI_JS_BI_COUNT; i++) {
        EMBI_JS_STORE(v, v->elems[i], UNDEF);
    }
    embi_js_root_add(rt, intrinsics_root, v);
    return v;
}

embi_js_value embi_js_binary_proto_from(embi_js_runtime *rt, embi_js_value nt,
                                        enum embi_js_bin_intr which)
{
    if (embi_js_is_object(nt)) {
        embi_js_value p = embi_js_obj_get(rt, embi_js_as_object(nt),
                                          embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE), nt);
        if (p == EXC) {
            return EXC;
        }
        if (embi_js_is_object(p)) {
            return p;
        }
    }
    return embi_js_binary_intrinsic(rt, which);
}

bool embi_js_bin_to_index(embi_js_runtime *rt, embi_js_value v, size_t *out)
{
    double d;
    if (!embi_js_to_integer_or_infinity(rt, v, &d)) {
        return false;
    }
    /* Compare as doubles before the conversion: a value above size_t's range is out of range,
     * never an (undefined) overflowing cast. */
    if (d < 0.0 || d > 9007199254740991.0 || d > (double)SIZE_MAX) {
        (void)embi_js_throw_range_error(rt, "Invalid index");
        return false;
    }
    *out = (size_t)d;
    return true;
}

/* ------------------------------------------------------------------ buffer storage */

static void ab_trace(embi_js_object *o, embi_js_tracer *t)
{
    const embi_js_abuf *b = o->internal;
    if (b) {
        t->external += b->cap + sizeof *b;
    }
}

static void ab_finalize(embi_js_object *o)
{
    embi_js_abuf *b = o->internal;
    if (!b) {
        return;
    }
    if (b->data) {
        embi_free(b->data, b->cap);
    }
    embi_free(b, sizeof *b);
    o->internal = nullptr;
}

const embi_js_class embi_js_class_arraybuffer = {
    .name = "ArrayBuffer",
    .trace = ab_trace,
    .finalize = ab_finalize,
};

const embi_js_class embi_js_class_sharedarraybuffer = {
    .name = "SharedArrayBuffer",
    .trace = ab_trace,
    .finalize = ab_finalize,
};

embi_js_object *embi_js_arraybuffer_alloc(embi_js_runtime *rt, embi_js_value proto, size_t len,
                                          size_t max_len, uint8_t flags)
{
    if (len > EMBI_JS_ABUF_MAX_BYTES || max_len > EMBI_JS_ABUF_MAX_BYTES) {
        (void)embi_js_throw_range_error(rt, "Array buffer allocation failed");
        return nullptr;
    }
    embi_js_abuf *b = embi_calloc(1, sizeof *b);
    if (!b) {
        (void)embi_js_throw_range_error(rt, "Array buffer allocation failed");
        return nullptr;
    }
    b->len = len;
    b->max_len = max_len;
    b->cap = len;
    b->flags = flags;
    if (len) {
        b->data = embi_calloc(1, len);
        if (!b->data) {
            embi_free(b, sizeof *b);
            (void)embi_js_throw_range_error(rt, "Array buffer allocation failed");
            return nullptr;
        }
    }
    const embi_js_class *klass = (flags & EMBI_JS_ABUF_SHARED) ? &embi_js_class_sharedarraybuffer
                                                                : &embi_js_class_arraybuffer;
    embi_js_object *o = embi_js_object_new_class(rt, proto, klass);
    o->internal = b;
    EMBI_JS_WB_STRUCT(o);
    embi_js_heap_note_external(&rt->heap, len);
    return o;
}

/* Frees the bytes and marks the buffer detached (DetachArrayBuffer). */
static void ab_detach(embi_js_abuf *b)
{
    if (b->data) {
        embi_free(b->data, b->cap);
    }
    b->data = nullptr;
    b->cap = 0;
    b->len = 0;
    b->flags |= EMBI_JS_ABUF_DETACHED;
}

/* DetachArrayBuffer for embedders (the test262 host's $262.detachArrayBuffer). Returns false when
 * v is not a non-shared ArrayBuffer object. */
bool embi_js_arraybuffer_detach(embi_js_value v)
{
    if (!embi_js_is_object(v) || embi_js_as_object(v)->klass != &embi_js_class_arraybuffer) {
        return false;
    }
    ab_detach(embi_js_abuf_of(embi_js_as_object(v)));
    return true;
}

/* Sets the byte length of a resizable buffer. Growth zeroes the new bytes. */
static bool ab_set_len(embi_js_abuf *b, size_t n)
{
    if (n > b->cap) {
        uint8_t *p = b->data ? embi_realloc(b->data, b->cap, n) : embi_calloc(1, n);
        if (!p) {
            return false;
        }
        b->data = p;
        b->cap = n;
    }
    if (n > b->len) {
        memset(b->data + b->len, 0, n - b->len);
    }
    b->len = n;
    return true;
}

/* ------------------------------------------------------------------ receivers */

/* The buffer of this (an ArrayBuffer, or a SharedArrayBuffer when shared), or nullptr with a
 * TypeError pending. */
static embi_js_abuf *this_buffer(embi_js_runtime *rt, embi_js_value v, bool shared,
                                 const char *method)
{
    if (embi_js_is_object(v)) {
        const embi_js_object *o = embi_js_as_object(v);
        const embi_js_class *want = shared ? &embi_js_class_sharedarraybuffer
                                           : &embi_js_class_arraybuffer;
        if (o->klass == want) {
            return o->internal;
        }
    }
    (void)embi_js_throw_type_error(rt, "%s called on an incompatible receiver", method);
    return nullptr;
}

/* ------------------------------------------------------------------ ArrayBuffer constructor */

/* The max-byte-length option (GetArrayBufferMaxByteLengthOption). Returns false when it throws;
 * *present reports whether the option was given. */
static bool max_length_option(embi_js_runtime *rt, embi_js_value options, bool *present,
                              size_t *max)
{
    *present = false;
    if (!embi_js_is_object(options)) {
        return true;
    }
    embi_js_value m = embi_js_obj_get(rt, embi_js_as_object(options),
                                      embi_js_key_c(rt, "maxByteLength"), options);
    if (m == EXC) {
        return false;
    }
    if (m == UNDEF) {
        return true;
    }
    if (!embi_js_bin_to_index(rt, m, max)) {
        return false;
    }
    *present = true;
    return true;
}

/* ArrayBuffer (magic 0) and SharedArrayBuffer (magic 1) constructors. */
static embi_js_value buffer_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    bool shared = magic != 0;
    if (embi_js_is_undefined(nt)) {
        return embi_js_throw_type_error(rt, "Constructor %s requires 'new'",
                                        shared ? "SharedArrayBuffer" : "ArrayBuffer");
    }
    size_t len;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &len)) {
        return EXC;
    }
    bool resizable;
    size_t max = len;
    if (!max_length_option(rt, arg(argc, argv, 1), &resizable, &max)) {
        return EXC;
    }
    if (resizable && len > max) {
        return embi_js_throw_range_error(rt, "byteLength exceeds maxByteLength");
    }
    embi_js_value proto = embi_js_binary_proto_from(
        rt, nt, shared ? EMBI_JS_BI_SHARED_PROTO : EMBI_JS_BI_ARRAYBUFFER_PROTO);
    if (proto == EXC) {
        return EXC;
    }
    uint8_t flags = (resizable ? EMBI_JS_ABUF_RESIZABLE : 0) |
                    (shared ? EMBI_JS_ABUF_SHARED : 0);
    embi_js_object *o = embi_js_arraybuffer_alloc(rt, proto, len, resizable ? max : len, flags);
    if (!o) {
        return EXC;
    }
    return embi_js_from_object(o);
}

static embi_js_value buffer_species(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

static embi_js_value buffer_is_view(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)magic;
    embi_js_value v = arg(argc, argv, 0);
    if (!embi_js_is_object(v)) {
        return EMBI_JS_FALSE;
    }
    const embi_js_object *o = embi_js_as_object(v);
    return embi_js_from_bool(embi_js_is_typedarray_obj(o) || embi_js_is_dataview_obj(o));
}

/* ------------------------------------------------------------------ prototype accessors */

/* byteLength (magic 0), maxByteLength (1), resizable (2), detached (3), immutable (4) on
 * ArrayBuffer.prototype; for SharedArrayBuffer.prototype: byteLength (5), growable (6),
 * maxByteLength (7). */
static embi_js_value ab_getter(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    bool shared = magic >= 5;
    embi_js_abuf *b = this_buffer(rt, this_v, shared,
                                  shared ? "SharedArrayBuffer getter" : "ArrayBuffer getter");
    if (!b) {
        return EXC;
    }
    bool detached = (b->flags & EMBI_JS_ABUF_DETACHED) != 0;
    switch (magic) {
    case 0:
    case 5:
        return embi_js_number(detached ? 0.0 : (double)b->len);
    case 1:
    case 7:
        return embi_js_number(detached ? 0.0 : (double)b->max_len);
    case 2:
        return embi_js_from_bool((b->flags & EMBI_JS_ABUF_RESIZABLE) != 0);
    case 3:
        return embi_js_from_bool(detached);
    case 4:
        return embi_js_from_bool((b->flags & EMBI_JS_ABUF_IMMUTABLE) != 0);
    case 6:
        return embi_js_from_bool((b->flags & EMBI_JS_ABUF_RESIZABLE) != 0);
    default:
        return UNDEF;
    }
}

/* ArrayBuffer.prototype.resize(newLength). */
static embi_js_value ab_resize(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_abuf *b = this_buffer(rt, this_v, false, "ArrayBuffer.prototype.resize");
    if (!b) {
        return EXC;
    }
    if (!(b->flags & EMBI_JS_ABUF_RESIZABLE)) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is not resizable");
    }
    size_t n;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &n)) {
        return EXC;
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    if (n > b->max_len) {
        return embi_js_throw_range_error(rt, "new length exceeds maxByteLength");
    }
    size_t old_len = b->len;
    if (!ab_set_len(b, n)) {
        return embi_js_throw_range_error(rt, "Array buffer allocation failed");
    }
    if (n > old_len) {
        embi_js_heap_note_external(&rt->heap, n - old_len);
    }
    return UNDEF;
}

/* SharedArrayBuffer.prototype.grow(newLength). */
static embi_js_value sab_grow(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_abuf *b = this_buffer(rt, this_v, true, "SharedArrayBuffer.prototype.grow");
    if (!b) {
        return EXC;
    }
    if (!(b->flags & EMBI_JS_ABUF_RESIZABLE)) {
        return embi_js_throw_type_error(rt, "SharedArrayBuffer is not growable");
    }
    size_t n;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &n)) {
        return EXC;
    }
    if (n > b->max_len || n < b->len) {
        return embi_js_throw_range_error(rt, "invalid length for SharedArrayBuffer.grow");
    }
    size_t old_len = b->len;
    if (!ab_set_len(b, n)) {
        return embi_js_throw_range_error(rt, "Array buffer allocation failed");
    }
    if (n > old_len) {
        embi_js_heap_note_external(&rt->heap, n - old_len);
    }
    return UNDEF;
}

/* Clamps a relative index (start or end) to [0, len]. */
static bool rel_index(embi_js_runtime *rt, embi_js_value v, double len, double dflt, double *out)
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

/* ArrayBuffer.prototype.slice (magic 0) and SharedArrayBuffer.prototype.slice (magic 1). */
static embi_js_value buffer_slice(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    bool shared = magic != 0;
    embi_js_abuf *b = this_buffer(rt, this_v, shared,
                                  shared ? "SharedArrayBuffer.prototype.slice"
                                         : "ArrayBuffer.prototype.slice");
    if (!b) {
        return EXC;
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    double len = (double)b->len;
    double first;
    double final;
    if (!rel_index(rt, arg(argc, argv, 0), len, 0.0, &first) ||
        !rel_index(rt, arg(argc, argv, 1), len, len, &final)) {
        return EXC;
    }
    double newlen = final - first > 0 ? final - first : 0.0;
    embi_js_value dflt = embi_js_binary_intrinsic(
        rt, shared ? EMBI_JS_BI_SHARED_CTOR : EMBI_JS_BI_ARRAYBUFFER_CTOR);
    embi_js_value ctor = embi_js_species_constructor(rt, embi_js_as_object(this_v), dflt);
    if (ctor == EXC) {
        return EXC;
    }
    embi_js_value lenv = embi_js_number(newlen);
    embi_js_value nv = embi_js_construct(rt, ctor, 1, &lenv, ctor);
    if (nv == EXC) {
        return EXC;
    }
    const embi_js_class *want = shared ? &embi_js_class_sharedarraybuffer
                                       : &embi_js_class_arraybuffer;
    if (!embi_js_is_object(nv) || embi_js_as_object(nv)->klass != want) {
        return embi_js_throw_type_error(rt, "species constructor did not return a buffer");
    }
    embi_js_abuf *nb = embi_js_as_object(nv)->internal;
    if (nb->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "species constructor returned a detached buffer");
    }
    if (nb->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "species constructor returned an immutable buffer");
    }
    if (nv == this_v) {
        return embi_js_throw_type_error(rt, "species constructor returned the same buffer");
    }
    if ((double)nb->len < newlen) {
        return embi_js_throw_type_error(rt, "species constructor returned a too small buffer");
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer was detached during slice");
    }
    double cur = (double)b->len;
    if (first < cur) {
        double count = newlen;
        if (cur - first < count) {
            count = cur - first;
        }
        if (count > 0) {
            memcpy(nb->data, b->data + (size_t)first, (size_t)count);
        }
    }
    return nv;
}

/* ArrayBuffer.prototype.sliceToImmutable(start, end). */
static embi_js_value ab_slice_immutable(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_abuf *b = this_buffer(rt, this_v, false, "ArrayBuffer.prototype.sliceToImmutable");
    if (!b) {
        return EXC;
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    double len = (double)b->len;
    double first;
    double final;
    if (!rel_index(rt, arg(argc, argv, 0), len, 0.0, &first) ||
        !rel_index(rt, arg(argc, argv, 1), len, len, &final)) {
        return EXC;
    }
    /* The index conversions ran user code: re-check the source (detached, or shrunk below end). */
    b = this_buffer(rt, this_v, false, "ArrayBuffer.prototype.sliceToImmutable");
    if (!b) {
        return EXC;
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    if ((double)b->len < final) {
        return embi_js_throw_range_error(rt, "ArrayBuffer was resized below the end index");
    }
    size_t newlen = final - first > 0 ? (size_t)(final - first) : 0;
    embi_js_value proto = embi_js_binary_intrinsic(rt, EMBI_JS_BI_ARRAYBUFFER_PROTO);
    embi_js_object *o = embi_js_arraybuffer_alloc(rt, proto, newlen, newlen,
                                                  EMBI_JS_ABUF_IMMUTABLE);
    if (!o) {
        return EXC;
    }
    embi_js_abuf *nb = embi_js_abuf_of(o);
    if (newlen > 0) {
        memcpy(nb->data, b->data + (size_t)first, newlen);
    }
    return embi_js_from_object(o);
}

/* ArrayBuffer.prototype.transfer (magic 0: resizability preserved), transferToFixedLength (1)
 * and transferToImmutable (2). */
static embi_js_value ab_transfer(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_abuf *b = this_buffer(rt, this_v, false, "ArrayBuffer.prototype.transfer");
    if (!b) {
        return EXC;
    }
    size_t newlen;
    if (arg(argc, argv, 0) == UNDEF) {
        newlen = b->len;
    } else if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &newlen)) {
        return EXC;
    }
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    if (b->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is immutable");
    }
    bool preserve = magic == 0;
    bool immutable = magic == 2;
    bool resizable = preserve && (b->flags & EMBI_JS_ABUF_RESIZABLE) != 0;
    size_t newmax = resizable ? b->max_len : newlen;
    if (resizable && newlen > newmax) {
        return embi_js_throw_range_error(rt, "new length exceeds maxByteLength");
    }
    uint8_t flags = (resizable ? EMBI_JS_ABUF_RESIZABLE : 0) |
                    (immutable ? EMBI_JS_ABUF_IMMUTABLE : 0);
    embi_js_value proto = embi_js_binary_intrinsic(rt, EMBI_JS_BI_ARRAYBUFFER_PROTO);
    embi_js_object *o = embi_js_arraybuffer_alloc(rt, proto, newlen, newmax, flags);
    if (!o) {
        return EXC;
    }
    b = embi_js_as_object(this_v)->internal;
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    embi_js_abuf *nb = embi_js_abuf_of(o);
    size_t n = newlen < b->len ? newlen : b->len;
    if (n > 0) {
        memcpy(nb->data, b->data, n);
    }
    ab_detach(b);
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ SharedArrayBuffer */

/* Installs a getter with the name "get <name>" on o. */
static void define_getter(embi_js_realm *r, embi_js_object *o, const char *name,
                          embi_js_native fn, int magic)
{
    char buf[64];
    SDL_snprintf(buf, sizeof buf, "get %s", name);
    embi_js_object *g = embi_js_native_function_new(r, buf, 0, fn, magic);
    embi_js_define_accessor_raw(r->rt, o, embi_js_key_c(r->rt, name), embi_js_from_object(g),
                                UNDEF, ATTR_C);
}

static void define_species(embi_js_realm *r, embi_js_object *ctor)
{
    embi_js_object *g = embi_js_native_function_new(r, "get [Symbol.species]", 0,
                                                    buffer_species, 0);
    embi_js_define_accessor_raw(r->rt, ctor, embi_js_wk_sym(r->rt, EMBI_JS_WK_SPECIES),
                                embi_js_from_object(g), UNDEF, ATTR_C);
}

/* Creates the constructor and prototype for one buffer class and installs them on the global. */
static embi_js_object *make_buffer_class(embi_js_realm *r, const char *name, int magic,
                                         embi_js_object **proto_out)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_object *ctor = embi_js_native_constructor_new(r, name, 1, buffer_ctor, magic);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, name), embi_js_from_object(ctor),
                       ATTR_WC);
    define_species(r, ctor);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), str_val(rt, name),
                       ATTR_C);
    *proto_out = proto;
    return ctor;
}

void embi_js_init_builtins_arraybuffer(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *intr = embi_js_binary_intrinsics(rt);

    /* ArrayBuffer */
    embi_js_object *ab_proto;
    embi_js_object *ab_ctor = make_buffer_class(r, "ArrayBuffer", 0, &ab_proto);
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_ARRAYBUFFER_PROTO], embi_js_from_object(ab_proto));
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_ARRAYBUFFER_CTOR], embi_js_from_object(ab_ctor));
    embi_js_define_method(r, ab_ctor, "isView", buffer_is_view, 1, 0);
    define_getter(r, ab_proto, "byteLength", ab_getter, 0);
    define_getter(r, ab_proto, "maxByteLength", ab_getter, 1);
    define_getter(r, ab_proto, "resizable", ab_getter, 2);
    define_getter(r, ab_proto, "detached", ab_getter, 3);
    define_getter(r, ab_proto, "immutable", ab_getter, 4);
    embi_js_define_method(r, ab_proto, "resize", ab_resize, 1, 0);
    embi_js_define_method(r, ab_proto, "slice", buffer_slice, 2, 0);
    embi_js_define_method(r, ab_proto, "transfer", ab_transfer, 0, 0);
    embi_js_define_method(r, ab_proto, "transferToFixedLength", ab_transfer, 0, 1);
    embi_js_define_method(r, ab_proto, "transferToImmutable", ab_transfer, 0, 2);
    embi_js_define_method(r, ab_proto, "sliceToImmutable", ab_slice_immutable, 2, 0);

    /* SharedArrayBuffer */
    embi_js_object *sab_proto;
    embi_js_object *sab_ctor = make_buffer_class(r, "SharedArrayBuffer", 1, &sab_proto);
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_SHARED_PROTO], embi_js_from_object(sab_proto));
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_SHARED_CTOR], embi_js_from_object(sab_ctor));
    define_getter(r, sab_proto, "byteLength", ab_getter, 5);
    define_getter(r, sab_proto, "growable", ab_getter, 6);
    define_getter(r, sab_proto, "maxByteLength", ab_getter, 7);
    embi_js_define_method(r, sab_proto, "grow", sab_grow, 1, 0);
    embi_js_define_method(r, sab_proto, "slice", buffer_slice, 2, 1);
}
