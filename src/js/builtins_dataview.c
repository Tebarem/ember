/*
 * DataView (ECMA-262 25.3): the view over a buffer with explicit byte order, its getters and setters
 * for every element type including Float16 and the BigInt pair, and length tracking over resizable
 * buffers.
 */
#include "base/base.h"
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "js/typedarray.h"

#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

static embi_js_value arg(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static void dv_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

const embi_js_class embi_js_class_dataview = {
    .name = "DataView",
    .trace = dv_trace,
};

/* The element kinds a DataView accesses, in the order of the getters' magic values. */
static const enum embi_js_ta_kind dv_kinds[] = {
    EMBI_JS_TA_INT8,    EMBI_JS_TA_UINT8,   EMBI_JS_TA_INT16,   EMBI_JS_TA_UINT16,
    EMBI_JS_TA_INT32,   EMBI_JS_TA_UINT32,  EMBI_JS_TA_FLOAT16, EMBI_JS_TA_FLOAT32,
    EMBI_JS_TA_FLOAT64, EMBI_JS_TA_BIGINT64, EMBI_JS_TA_BIGUINT64,
};
#define DV_KIND_COUNT ((int)(sizeof dv_kinds / sizeof dv_kinds[0]))

static const char *const dv_names[] = {
    "Int8",    "Uint8",   "Int16",   "Uint16",  "Int32",      "Uint32",
    "Float16", "Float32", "Float64", "BigInt64", "BigUint64",
};

static bool is_dataview(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_is_dataview_obj(embi_js_as_object(v));
}

/* DataView(buffer, byteOffset, byteLength). */
static embi_js_value dv_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic, embi_js_value nt)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(nt)) {
        return embi_js_throw_type_error(rt, "Constructor DataView requires 'new'");
    }
    embi_js_value bv = arg(argc, argv, 0);
    if (!embi_js_is_object(bv) || !embi_js_is_buffer_obj(embi_js_as_object(bv))) {
        return embi_js_throw_type_error(rt, "First argument to DataView constructor must be an ArrayBuffer");
    }
    size_t off;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 1), &off)) {
        return EXC;
    }
    embi_js_object *buf = embi_js_as_object(bv);
    embi_js_abuf *b = embi_js_abuf_of(buf);
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    size_t buflen = b->len;
    if (off > buflen) {
        return embi_js_throw_range_error(rt, "start offset %llu is outside the buffer", (unsigned long long)off);
    }
    bool fixed = (b->flags & EMBI_JS_ABUF_RESIZABLE) == 0;
    bool tracking = false;
    size_t vlen = 0;
    embi_js_value lenv = arg(argc, argv, 2);
    if (lenv == UNDEF) {
        if (fixed) {
            vlen = buflen - off;
        } else {
            tracking = true;
        }
    } else {
        if (!embi_js_bin_to_index(rt, lenv, &vlen)) {
            return EXC;
        }
        if (vlen > buflen - off) {
            return embi_js_throw_range_error(rt, "Invalid DataView length %llu", (unsigned long long)vlen);
        }
    }
    embi_js_value proto = embi_js_binary_proto_from(rt, nt, EMBI_JS_BI_DATAVIEW_PROTO);
    if (proto == EXC) {
        return EXC;
    }
    /* The buffer may have been detached or shrunk by the prototype lookup. */
    b = embi_js_abuf_of(buf);
    if (b->flags & EMBI_JS_ABUF_DETACHED) {
        return embi_js_throw_type_error(rt, "ArrayBuffer is detached");
    }
    buflen = b->len;
    if (off > buflen) {
        return embi_js_throw_range_error(rt, "start offset %llu is outside the buffer", (unsigned long long)off);
    }
    if (!tracking && vlen > buflen - off) {
        return embi_js_throw_range_error(rt, "Invalid DataView length %llu", (unsigned long long)vlen);
    }
    embi_js_varray *view = embi_js_varray_new(rt, EMBI_JS_VIEW_SLOTS);
    EMBI_JS_STORE(view, view->elems[EMBI_JS_VIEW_BUFFER], embi_js_from_object(buf));
    EMBI_JS_STORE(view, view->elems[EMBI_JS_VIEW_OFFSET], embi_js_number((double)off));
    EMBI_JS_STORE(view, view->elems[EMBI_JS_VIEW_LENGTH], tracking ? UNDEF : embi_js_number((double)vlen));
    EMBI_JS_STORE(view, view->elems[EMBI_JS_VIEW_KIND], embi_js_from_int32(-1));
    embi_js_object *o = embi_js_object_new_class(rt, proto, &embi_js_class_dataview);
    o->internal = view;
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

/* The view record of this, or a TypeError pending. */
static embi_js_varray *this_view(embi_js_runtime *rt, embi_js_value v, const char *method)
{
    if (!is_dataview(v)) {
        (void)embi_js_throw_type_error(rt, "%s called on an incompatible receiver", method);
        return nullptr;
    }
    return embi_js_as_object(v)->internal;
}

/* DataView.prototype.buffer, byteLength (magic 1) and byteOffset (magic 2). */
static embi_js_value dv_getter(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_varray *v = this_view(rt, this_v, "DataView getter");
    if (!v) {
        return EXC;
    }
    if (magic == 0) {
        return v->elems[EMBI_JS_VIEW_BUFFER];
    }
    size_t len = 0;
    bool in = embi_js_view_len(v, 1, &len);
    if (magic == 1) {
        /* byteLength is 0 for an out-of-bounds view (a shrunk or detached buffer). */
        return embi_js_number(in ? (double)len : 0.0);
    }
    /* byteOffset is 0 for an out-of-bounds view, like byteLength. */
    return embi_js_number(in ? (double)embi_js_view_offset(v) : 0.0);
}

/* DataView.prototype.get<Type>(byteOffset, littleEndian); magic is the index into dv_kinds. */
static embi_js_value dv_get(embi_js_realm *r, embi_js_value this_v, int argc,
                            const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *v = this_view(rt, this_v, "DataView.prototype.get");
    if (!v) {
        return EXC;
    }
    size_t idx;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &idx)) {
        return EXC;
    }
    bool little = embi_js_to_boolean(arg(argc, argv, 1));
    enum embi_js_ta_kind k = dv_kinds[magic];
    size_t n = embi_js_bin_size(k);
    size_t vlen = 0;
    if (!embi_js_view_len(v, 1, &vlen)) {
        return embi_js_throw_type_error(rt, "DataView is detached or out of bounds");
    }
    if (idx > vlen || n > vlen - idx) {
        return embi_js_throw_range_error(rt, "Offset is outside the bounds of the DataView");
    }
    uint64_t bits = embi_js_bin_load(embi_js_view_base(v) + idx, n, !little);
    return embi_js_bin_box(rt, k, bits);
}

/* DataView.prototype.set<Type>(byteOffset, value, littleEndian); magic is the index into dv_kinds. */
static embi_js_value dv_set(embi_js_realm *r, embi_js_value this_v, int argc,
                            const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *v = this_view(rt, this_v, "DataView.prototype.set");
    if (!v) {
        return EXC;
    }
    /* Checked before any argument is read (the immutable-buffer test262 cases expect no calls). */
    if (embi_js_view_abuf(v)->flags & EMBI_JS_ABUF_IMMUTABLE) {
        return embi_js_throw_type_error(rt, "Cannot write to an immutable ArrayBuffer");
    }
    size_t idx;
    if (!embi_js_bin_to_index(rt, arg(argc, argv, 0), &idx)) {
        return EXC;
    }
    enum embi_js_ta_kind k = dv_kinds[magic];
    uint64_t bits;
    if (!embi_js_bin_coerce(rt, k, arg(argc, argv, 1), &bits)) {
        return EXC;
    }
    bool little = embi_js_to_boolean(arg(argc, argv, 2));
    size_t n = embi_js_bin_size(k);
    size_t vlen = 0;
    if (!embi_js_view_len(v, 1, &vlen)) {
        return embi_js_throw_type_error(rt, "DataView is detached or out of bounds");
    }
    if (idx > vlen || n > vlen - idx) {
        return embi_js_throw_range_error(rt, "Offset is outside the bounds of the DataView");
    }
    embi_js_bin_store(embi_js_view_base(v) + idx, n, bits, !little);
    return UNDEF;
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

void embi_js_init_builtins_dataview(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_varray *intr = embi_js_binary_intrinsics(rt);
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_object *ctor = embi_js_native_constructor_new(r, "DataView", 1, dv_ctor, 0);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "DataView"), embi_js_from_object(ctor),
                       ATTR_WC);
    EMBI_JS_STORE(intr, intr->elems[EMBI_JS_BI_DATAVIEW_PROTO], embi_js_from_object(proto));

    define_getter(r, proto, "buffer", dv_getter, 0);
    define_getter(r, proto, "byteLength", dv_getter, 1);
    define_getter(r, proto, "byteOffset", dv_getter, 2);

    for (int i = 0; i < DV_KIND_COUNT; i++) {
        char name[32];
        SDL_snprintf(name, sizeof name, "get%s", dv_names[i]);
        embi_js_define_method(r, proto, name, dv_get, 1, i);
        SDL_snprintf(name, sizeof name, "set%s", dv_names[i]);
        embi_js_define_method(r, proto, name, dv_set, 2, i);
    }

    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "DataView", 8)), ATTR_C);
}
