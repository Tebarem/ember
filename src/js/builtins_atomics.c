/*
 * The Atomics object (ECMA-262 25.4, and Atomics.pause from ES2025) over integer TypedArrays on
 * either buffer kind. Only one agent runs, so the operations are plain
 * reads and writes. Atomics.wait validates and coerces its arguments, then throws a TypeError
 * (AgentCanSuspend is false: the engine thread never blocks). Atomics.waitAsync is deliberately not
 * defined (no timers or agents to resolve its promise). Atomics.notify wakes nothing and returns 0.
 */
#include "base/base.h"
#include "js/builtins.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/typedarray.h"

#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

static embi_js_value arg(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

/* ValidateIntegerTypedArray: an in-bounds integer TypedArray (waitable: Int32 or BigInt64 only).
 * A write operation (writes) also rejects an immutable buffer, before any index or value is read.
 * Returns the object and its length, or false with a TypeError pending. */
static bool validate_int_ta(embi_js_runtime *rt, embi_js_value v, bool waitable, bool writes,
                            embi_js_object **out, size_t *len)
{
    if (!embi_js_is_object(v) || !embi_js_is_typedarray_obj(embi_js_as_object(v))) {
        (void)embi_js_throw_type_error(rt, "Atomics operation requires an integer TypedArray");
        return false;
    }
    embi_js_object *o = embi_js_as_object(v);
    embi_js_varray *view = o->internal;
    enum embi_js_ta_kind k = (enum embi_js_ta_kind)embi_js_as_int32(view->elems[EMBI_JS_VIEW_KIND]);
    bool ok;
    if (waitable) {
        ok = k == EMBI_JS_TA_INT32 || k == EMBI_JS_TA_BIGINT64;
    } else {
        ok = k == EMBI_JS_TA_INT8 || k == EMBI_JS_TA_UINT8 || k == EMBI_JS_TA_INT16 ||
             k == EMBI_JS_TA_UINT16 || k == EMBI_JS_TA_INT32 || k == EMBI_JS_TA_UINT32 ||
             k == EMBI_JS_TA_BIGINT64 || k == EMBI_JS_TA_BIGUINT64;
    }
    if (!ok) {
        (void)embi_js_throw_type_error(rt, "Atomics operation on an unsupported TypedArray type");
        return false;
    }
    if (writes && (embi_js_view_abuf(view)->flags & EMBI_JS_ABUF_IMMUTABLE)) {
        (void)embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
        return false;
    }
    if (!embi_js_view_len(view, embi_js_bin_size(k), len)) {
        (void)embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        return false;
    }
    *out = o;
    return true;
}

static enum embi_js_ta_kind kind_of_obj(embi_js_object *o)
{
    return (enum embi_js_ta_kind)embi_js_as_int32(((embi_js_varray *)o->internal)->elems[EMBI_JS_VIEW_KIND]);
}

/* ValidateAtomicAccess: the element index of v, which must be below the length. */
static bool access_index(embi_js_runtime *rt, embi_js_value v, size_t len, size_t *idx)
{
    if (!embi_js_bin_to_index(rt, v, idx)) {
        return false;
    }
    if (*idx >= len) {
        (void)embi_js_throw_range_error(rt, "Atomics access index is out of range");
        return false;
    }
    return true;
}

/* RevalidateAtomicAccess: after a conversion that may run user code, the element must still exist. */
static bool revalidate(embi_js_runtime *rt, embi_js_object *o, size_t idx)
{
    embi_js_varray *view = o->internal;
    size_t len = 0;
    if (!embi_js_view_len(view, embi_js_bin_size(kind_of_obj(o)), &len)) {
        (void)embi_js_throw_type_error(rt, "typed array is detached or out of bounds");
        return false;
    }
    if (idx >= len) {
        (void)embi_js_throw_range_error(rt, "Atomics access index is out of range");
        return false;
    }
    return true;
}

static uint8_t *elem_ptr(embi_js_object *o, size_t idx)
{
    embi_js_varray *view = o->internal;
    return embi_js_view_base(view) + idx * embi_js_bin_size(kind_of_obj(o));
}

/* Atomics.add (0), and (1), exchange (2), or (3), sub (4), xor (5). */
static embi_js_value atomic_rmw(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), false, true, &o, &len)) {
        return EXC;
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = kind_of_obj(o);
    uint64_t v;
    if (!embi_js_bin_coerce(rt, k, arg(argc, argv, 2), &v)) {
        return EXC;
    }
    if (!revalidate(rt, o, idx)) {
        return EXC;
    }
    size_t n = embi_js_bin_size(k);
    uint8_t *p = elem_ptr(o, idx);
    uint64_t old = embi_js_bin_load(p, n, false);
    uint64_t nv;
    switch (magic) {
    case 0:
        nv = old + v;
        break;
    case 1:
        nv = old & v;
        break;
    case 2:
        nv = v;
        break;
    case 3:
        nv = old | v;
        break;
    case 4:
        nv = old - v;
        break;
    default:
        nv = old ^ v;
        break;
    }
    embi_js_bin_store(p, n, nv, false);
    return embi_js_bin_box(rt, k, old);
}

/* Atomics.load(typedArray, index). */
static embi_js_value atomic_load(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), false, false, &o, &len)) {
        return EXC;
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    if (!revalidate(rt, o, idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = kind_of_obj(o);
    uint64_t bits = embi_js_bin_load(elem_ptr(o, idx), embi_js_bin_size(k), false);
    return embi_js_bin_box(rt, k, bits);
}

/* Atomics.store(typedArray, index, value): returns the converted value. */
static embi_js_value atomic_store(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), false, true, &o, &len)) {
        return EXC;
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = kind_of_obj(o);
    embi_js_value ret;
    uint64_t bits;
    if (embi_js_bin_is_big(k)) {
        ret = embi_js_to_bigint(rt, arg(argc, argv, 2));
        if (ret == EXC) {
            return EXC;
        }
        if (!embi_js_bin_coerce(rt, k, ret, &bits)) {
            return EXC;
        }
    } else {
        double d;
        if (!embi_js_to_integer_or_infinity(rt, arg(argc, argv, 2), &d)) {
            return EXC;
        }
        if (d == 0) {
            d = 0; /* -0 becomes +0 */
        }
        ret = embi_js_number(d);
        if (!embi_js_bin_coerce(rt, k, ret, &bits)) {
            return EXC;
        }
    }
    if (!revalidate(rt, o, idx)) {
        return EXC;
    }
    embi_js_bin_store(elem_ptr(o, idx), embi_js_bin_size(k), bits, false);
    return ret;
}

/* Atomics.compareExchange(typedArray, index, expected, replacement). */
static embi_js_value atomic_cmpxchg(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), false, true, &o, &len)) {
        return EXC;
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = kind_of_obj(o);
    uint64_t expected;
    uint64_t replacement;
    if (!embi_js_bin_coerce(rt, k, arg(argc, argv, 2), &expected) ||
        !embi_js_bin_coerce(rt, k, arg(argc, argv, 3), &replacement)) {
        return EXC;
    }
    if (!revalidate(rt, o, idx)) {
        return EXC;
    }
    size_t n = embi_js_bin_size(k);
    uint64_t mask = n == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * n)) - 1);
    uint8_t *p = elem_ptr(o, idx);
    uint64_t old = embi_js_bin_load(p, n, false);
    if ((old & mask) == (expected & mask)) {
        embi_js_bin_store(p, n, replacement, false);
    }
    return embi_js_bin_box(rt, k, old);
}

/* Atomics.isLockFree(size). */
static embi_js_value atomic_is_lock_free(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    double n;
    if (!embi_js_to_integer_or_infinity(r->rt, arg(argc, argv, 0), &n)) {
        return EXC;
    }
    return embi_js_from_bool(n == 1 || n == 2 || n == 4 || n == 8);
}

/* Atomics.wait(typedArray, index, value, timeout): throws after the coercions, never blocks. */
static embi_js_value atomic_wait(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), true, false, &o, &len)) {
        return EXC;
    }
    if (!(((embi_js_abuf *)embi_js_view_abuf(o->internal))->flags & EMBI_JS_ABUF_SHARED)) {
        return embi_js_throw_type_error(rt, "Atomics.wait requires a shared buffer");
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = kind_of_obj(o);
    uint64_t want;
    if (!embi_js_bin_coerce(rt, k, arg(argc, argv, 2), &want)) {
        return EXC;
    }
    double timeout;
    if (!embi_js_to_number(rt, arg(argc, argv, 3), &timeout)) {
        return EXC;
    }
    if (timeout != timeout) {
        timeout = INFINITY;
    }
    if (timeout < 0) {
        timeout = 0;
    }
    /* AgentCanSuspend() is false for the embedder's thread: a script must never freeze it. */
    (void)want;
    (void)idx;
    return embi_js_throw_type_error(rt, "Atomics.wait cannot block on this thread");
}

/* Atomics.notify(typedArray, index, count): no agent waits, so nothing is woken. */
static embi_js_value atomic_notify(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o;
    size_t len;
    if (!validate_int_ta(rt, arg(argc, argv, 0), true, false, &o, &len)) {
        return EXC;
    }
    size_t idx;
    if (!access_index(rt, arg(argc, argv, 1), len, &idx)) {
        return EXC;
    }
    if (arg(argc, argv, 2) != UNDEF) {
        double c;
        if (!embi_js_to_integer_or_infinity(rt, arg(argc, argv, 2), &c)) {
            return EXC;
        }
    }
    return embi_js_from_int32(0);
}

/* Atomics.pause(N) (ES2025): N is undefined or an integral Number. */
static embi_js_value atomic_pause(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_value n = arg(argc, argv, 0);
    if (n != UNDEF) {
        if (!embi_js_is_number(n)) {
            return embi_js_throw_type_error(r->rt, "Atomics.pause requires an integral Number");
        }
        double d = embi_js_bin_num(n);
        if (d != d || isinf(d) || d != trunc(d)) {
            return embi_js_throw_type_error(r->rt, "Atomics.pause requires an integral Number");
        }
    }
    return UNDEF;
}

void embi_js_init_builtins_atomics(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *atomics = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Atomics"), embi_js_from_object(atomics),
                       ATTR_WC);
    static const char *const rmw[] = {"add", "and", "exchange", "or", "sub", "xor"};
    for (int i = 0; i < 6; i++) {
        embi_js_define_method(r, atomics, rmw[i], atomic_rmw, 3, i);
    }
    embi_js_define_method(r, atomics, "compareExchange", atomic_cmpxchg, 4, 0);
    embi_js_define_method(r, atomics, "isLockFree", atomic_is_lock_free, 1, 0);
    embi_js_define_method(r, atomics, "load", atomic_load, 2, 0);
    embi_js_define_method(r, atomics, "notify", atomic_notify, 3, 0);
    embi_js_define_method(r, atomics, "pause", atomic_pause, 0, 0);
    embi_js_define_method(r, atomics, "store", atomic_store, 3, 0);
    embi_js_define_method(r, atomics, "wait", atomic_wait, 4, 0);
    embi_js_define_raw(rt, atomics, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "Atomics", 7)),
                       EMBI_JS_ATTR_CONFIGURABLE);
}
