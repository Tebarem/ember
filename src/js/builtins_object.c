/*
 * Object: Object.prototype (created by embi_js_init_builtins_object, before any function object
 * exists) and the Object constructor with its statics and prototype methods. The methods need
 * function objects, so they are installed by embi_js_builtins_object_install once Function.prototype
 * exists. builtins_global.c calls it, which also defines the global Object binding.
 *
 * GC rule used here: a value that user code can run past (a getter, a setter, a proxy-less
 * [[Get]]) is kept in a GC-visible varray, never only in an EMBI_VEC on the C heap.
 */
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"

#define EXC EMBI_JS_EXCEPTION
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* Slots per property in the descriptor list built by define_properties. */
#define DEF_SLOTS 5u

enum enum_kind {
    ENUM_KEYS,
    ENUM_VALUES,
    ENUM_ENTRIES
};

/* ------------------------------------------------------------------ helpers */

/* The own keys of o in [[OwnPropertyKeys]] order, copied into a GC-visible array. Returns nullptr
 * when a key cannot be read (no user code runs here, but the copy keeps symbols alive if a later
 * step does run user code). */
static embi_js_varray *own_keys_of(embi_js_runtime *rt, embi_js_object *o, uint32_t *n)
{
    embi_js_key_vec kv = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, o, &kv) != EMBI_JS_YES) {
        embi_vec_free(&kv);
        return nullptr;
    }
    embi_js_varray *a = embi_js_varray_new(rt, kv.len);
    for (uint32_t i = 0; i < kv.len; i++) {
        EMBI_JS_STORE(a, a->elems[i], kv.data[i]);
    }
    *n = kv.len;
    embi_vec_free(&kv);
    return a;
}

/* CreateDataPropertyOrThrow on an array index. Returns YES or THREW. */
static int put_index(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v)
{
    return embi_js_create_data_property_or_throw(rt, o, embi_js_key_from_index(i), v);
}

/* Appends ASCII text to a string builder. */
static void sb_text(embi_sb *sb, const char *s)
{
    for (; *s; s++) {
        embi_sb_append_char(sb, *s);
    }
}

/* ObjectDefineProperties(O, Properties). Every descriptor is read before any is applied, so the
 * descriptors are kept in a GC-visible varray of DEF_SLOTS values per property: key, value, get,
 * set, and a flags int (has bits, then attributes shifted by 8). Returns YES or THREW. */
static int define_properties(embi_js_runtime *rt, embi_js_object *o, embi_js_value props_v)
{
    embi_js_object *props = embi_js_to_object(rt, props_v);
    if (!props) {
        return EMBI_JS_THREW;
    }
    uint32_t n = 0;
    embi_js_varray *keys = own_keys_of(rt, props, &n);
    if (!keys) {
        return EMBI_JS_THREW;
    }
    embi_js_varray *list = embi_js_varray_new(rt, n * DEF_SLOTS);
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_key k = keys->elems[i];
        embi_js_pdesc pd;
        int st = embi_js_obj_get_own_property(rt, props, k, &pd);
        if (st == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (st != EMBI_JS_YES || !(pd.attrs & ATTR_E)) {
            continue;
        }
        embi_js_value dv = embi_js_obj_get(rt, props, k, embi_js_from_object(props));
        if (dv == EXC) {
            return EMBI_JS_THREW;
        }
        embi_js_pdesc d;
        if (!embi_js_to_property_descriptor(rt, dv, &d)) {
            return EMBI_JS_THREW;
        }
        uint32_t base = m * DEF_SLOTS;
        EMBI_JS_STORE(list, list->elems[base], k);
        EMBI_JS_STORE(list, list->elems[base + 1], (d.has & EMBI_JS_PD_VALUE) ? d.value : EMBI_JS_UNDEFINED);
        EMBI_JS_STORE(list, list->elems[base + 2], (d.has & EMBI_JS_PD_GET) ? d.get : EMBI_JS_UNDEFINED);
        EMBI_JS_STORE(list, list->elems[base + 3], (d.has & EMBI_JS_PD_SET) ? d.set : EMBI_JS_UNDEFINED);
        EMBI_JS_STORE(list, list->elems[base + 4], embi_js_from_int32((int32_t)(d.has | ((uint32_t)d.attrs << 8))));
        m++;
    }
    for (uint32_t j = 0; j < m; j++) {
        uint32_t base = j * DEF_SLOTS;
        int32_t flags = embi_js_as_int32(list->elems[base + 4]);
        embi_js_pdesc d;
        d.has = (uint8_t)(flags & 0xFF);
        d.attrs = (uint8_t)((uint32_t)flags >> 8);
        d.value = list->elems[base + 1];
        d.get = list->elems[base + 2];
        d.set = list->elems[base + 3];
        if (embi_js_define_property_or_throw(rt, o, list->elems[base], &d) == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
    }
    return EMBI_JS_YES;
}

/* EnumerableOwnProperties(O, kind). Keys are read first; each string key is then checked and read
 * in turn, so a getter that deletes a later key is seen. */
static embi_js_value enumerable_props(embi_js_runtime *rt, embi_js_value v, enum enum_kind kind)
{
    embi_js_object *o = embi_js_to_object(rt, v);
    if (!o) {
        return EXC;
    }
    uint32_t n = 0;
    embi_js_varray *keys = own_keys_of(rt, o, &n);
    if (!keys) {
        return EXC;
    }
    embi_js_object *res = embi_js_array_create_default(rt, 0.0);
    if (!res) {
        return EXC;
    }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_key k = keys->elems[i];
        if (embi_js_is_symbol(k)) {
            continue;
        }
        embi_js_pdesc pd;
        int st = embi_js_obj_get_own_property(rt, o, k, &pd);
        if (st == EMBI_JS_THREW) {
            return EXC;
        }
        if (st != EMBI_JS_YES || !(pd.attrs & ATTR_E)) {
            continue;
        }
        embi_js_value item;
        if (kind == ENUM_KEYS) {
            item = embi_js_key_to_value(rt, k);
        } else {
            embi_js_value val = embi_js_obj_get(rt, o, k, embi_js_from_object(o));
            if (val == EXC) {
                return EXC;
            }
            if (kind == ENUM_VALUES) {
                item = val;
            } else {
                embi_js_object *pair = embi_js_array_create_default(rt, 2.0);
                if (!pair) {
                    return EXC;
                }
                embi_js_value kv = embi_js_key_to_value(rt, k);
                if (put_index(rt, pair, 0, kv) == EMBI_JS_THREW ||
                    put_index(rt, pair, 1, val) == EMBI_JS_THREW) {
                    return EXC;
                }
                item = embi_js_from_object(pair);
            }
        }
        if (put_index(rt, res, m, item) == EMBI_JS_THREW) {
            return EXC;
        }
        m++;
    }
    return embi_js_from_object(res);
}

/* ------------------------------------------------------------------ Object statics */

static embi_js_value object_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic,
                                 embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_object(new_target) && embi_js_as_object(new_target) != r->object_ctor) {
        embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_OBJECT);
        if (proto == EXC) {
            return EXC;
        }
        return embi_js_from_object(embi_js_object_new(rt, proto));
    }
    if (argc == 0 || embi_js_is_nullish(argv[0])) {
        return embi_js_from_object(embi_js_object_new_plain(rt));
    }
    embi_js_object *o = embi_js_to_object(rt, argv[0]);
    return o ? embi_js_from_object(o) : EXC;
}

static embi_js_value object_assign(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *to = embi_js_to_object(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!to) {
        return EXC;
    }
    for (int i = 1; i < argc; i++) {
        if (embi_js_is_nullish(argv[i])) {
            continue;
        }
        embi_js_object *from = embi_js_to_object(rt, argv[i]);
        if (!from) {
            return EXC;
        }
        uint32_t n = 0;
        embi_js_varray *keys = own_keys_of(rt, from, &n);
        if (!keys) {
            return EXC;
        }
        for (uint32_t j = 0; j < n; j++) {
            embi_js_key k = keys->elems[j];
            embi_js_pdesc pd;
            int st = embi_js_obj_get_own_property(rt, from, k, &pd);
            if (st == EMBI_JS_THREW) {
                return EXC;
            }
            if (st != EMBI_JS_YES || !(pd.attrs & ATTR_E)) {
                continue;
            }
            embi_js_value v = embi_js_obj_get(rt, from, k, embi_js_from_object(from));
            if (v == EXC) {
                return EXC;
            }
            if (embi_js_put(rt, to, k, v, true) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    return embi_js_from_object(to);
}

static embi_js_value object_create(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value proto = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(proto) && !embi_js_is_null(proto)) {
        return embi_js_throw_type_error(rt, "Object prototype may only be an Object or null");
    }
    embi_js_object *o = embi_js_object_new(rt, proto);
    if (argc > 1 && !embi_js_is_undefined(argv[1])) {
        if (define_properties(rt, o, argv[1]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

static embi_js_value object_define_property(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value ov = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(ov)) {
        return embi_js_throw_type_error(rt, "Object.defineProperty called on non-object");
    }
    embi_js_key k = embi_js_to_property_key(rt, argc > 1 ? argv[1] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    embi_js_pdesc d;
    if (!embi_js_to_property_descriptor(rt, argc > 2 ? argv[2] : EMBI_JS_UNDEFINED, &d)) {
        return EXC;
    }
    if (embi_js_define_property_or_throw(rt, embi_js_as_object(ov), k, &d) == EMBI_JS_THREW) {
        return EXC;
    }
    return ov;
}

static embi_js_value object_define_properties(embi_js_realm *r, embi_js_value this_v, int argc,
                                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value ov = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(ov)) {
        return embi_js_throw_type_error(rt, "Object.defineProperties called on non-object");
    }
    if (define_properties(rt, embi_js_as_object(ov), argc > 1 ? argv[1] : EMBI_JS_UNDEFINED) ==
        EMBI_JS_THREW) {
        return EXC;
    }
    return ov;
}

static embi_js_value object_get_own_property_descriptor(embi_js_realm *r, embi_js_value this_v,
                                                        int argc, const embi_js_value *argv,
                                                        int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, argc > 1 ? argv[1] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    embi_js_pdesc d;
    int st = embi_js_obj_get_own_property(rt, o, k, &d);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return EMBI_JS_UNDEFINED;
    }
    return embi_js_from_property_descriptor(rt, &d);
}

static embi_js_value object_get_own_property_descriptors(embi_js_realm *r, embi_js_value this_v,
                                                         int argc, const embi_js_value *argv,
                                                         int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!o) {
        return EXC;
    }
    uint32_t n = 0;
    embi_js_varray *keys = own_keys_of(rt, o, &n);
    if (!keys) {
        return EXC;
    }
    embi_js_object *res = embi_js_object_new_plain(rt);
    for (uint32_t i = 0; i < n; i++) {
        embi_js_key k = keys->elems[i];
        embi_js_pdesc d;
        int st = embi_js_obj_get_own_property(rt, o, k, &d);
        if (st == EMBI_JS_THREW) {
            return EXC;
        }
        if (st != EMBI_JS_YES) {
            continue;
        }
        embi_js_value dv = embi_js_from_property_descriptor(rt, &d);
        if (dv == EXC) {
            return EXC;
        }
        if (embi_js_create_data_property_or_throw(rt, res, k, dv) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(res);
}

/* getOwnPropertyNames (symbols false) and getOwnPropertySymbols (symbols true). */
static embi_js_value own_names(embi_js_realm *r, embi_js_value v, bool symbols)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, v);
    if (!o) {
        return EXC;
    }
    uint32_t n = 0;
    embi_js_varray *keys = own_keys_of(rt, o, &n);
    if (!keys) {
        return EXC;
    }
    embi_js_object *res = embi_js_array_create_default(rt, 0.0);
    if (!res) {
        return EXC;
    }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_js_key k = keys->elems[i];
        if (embi_js_is_symbol(k) != symbols) {
            continue;
        }
        if (put_index(rt, res, m, embi_js_key_to_value(rt, k)) == EMBI_JS_THREW) {
            return EXC;
        }
        m++;
    }
    return embi_js_from_object(res);
}

static embi_js_value object_get_own_property_names(embi_js_realm *r, embi_js_value this_v,
                                                   int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return own_names(r, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, false);
}

static embi_js_value object_get_own_property_symbols(embi_js_realm *r, embi_js_value this_v,
                                                     int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return own_names(r, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, true);
}

static embi_js_value object_get_prototype_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    embi_js_value p;
    if (!o || embi_js_obj_get_prototype_of(rt, o, &p) == EMBI_JS_THREW) {
        return EXC;
    }
    return p;
}

static embi_js_value object_is(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)magic;
    return embi_js_from_bool(embi_js_same_value(argc > 0 ? argv[0] : EMBI_JS_UNDEFINED,
                                                argc > 1 ? argv[1] : EMBI_JS_UNDEFINED));
}

static embi_js_value object_is_extensible(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(v)) {
        return EMBI_JS_FALSE;
    }
    int st = embi_js_obj_extensible(rt, embi_js_as_object(v));
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

/* isFrozen (frozen true) and isSealed (frozen false). */
static embi_js_value test_level(embi_js_realm *r, embi_js_value v, bool frozen)
{
    if (!embi_js_is_object(v)) {
        return EMBI_JS_TRUE;
    }
    int st = embi_js_test_integrity(r->rt, embi_js_as_object(v), frozen);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value object_is_frozen(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return test_level(r, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, true);
}

static embi_js_value object_is_sealed(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return test_level(r, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, false);
}

/* freeze (magic 1) and seal (magic 0) use SetIntegrityLevel. */
static embi_js_value object_set_level(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)this_v;
    embi_js_runtime *rt = r->rt;
    embi_js_value ov = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(ov)) {
        return ov;
    }
    bool frozen = magic == 1;
    int st = embi_js_set_integrity(rt, embi_js_as_object(ov), frozen);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return embi_js_throw_type_error(rt, frozen ? "Cannot freeze" : "Cannot seal");
    }
    return ov;
}

/* Object.hasOwn (ES2022): ToObject on the first argument, then ToPropertyKey on the second. */
static embi_js_value object_has_own(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, argc > 1 ? argv[1] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    int st = embi_js_has_own(rt, o, k);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value object_keys(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return enumerable_props(r->rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, ENUM_KEYS);
}

static embi_js_value object_values(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return enumerable_props(r->rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, ENUM_VALUES);
}

static embi_js_value object_entries(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    return enumerable_props(r->rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED, ENUM_ENTRIES);
}

static embi_js_value object_prevent_extensions(embi_js_realm *r, embi_js_value this_v, int argc,
                                               const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value ov = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(ov)) {
        return ov;
    }
    int st = embi_js_obj_prevent_extensions(rt, embi_js_as_object(ov));
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return embi_js_throw_type_error(rt, "Cannot prevent extensions");
    }
    return ov;
}

static embi_js_value object_set_prototype_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value ov = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    embi_js_value proto = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    if (embi_js_is_nullish(ov)) {
        return embi_js_throw_type_error(rt, "Object.setPrototypeOf called on null or undefined");
    }
    if (!embi_js_is_object(proto) && !embi_js_is_null(proto)) {
        return embi_js_throw_type_error(rt, "Object prototype may only be an Object or null");
    }
    if (!embi_js_is_object(ov)) {
        return ov;
    }
    int st = embi_js_obj_set_prototype_of(rt, embi_js_as_object(ov), proto);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return embi_js_throw_type_error(rt, "Object.setPrototypeOf: prototype cannot be changed");
    }
    return ov;
}

/* ------------------------------------------------------------------ Object.prototype */

static embi_js_value proto_has_own_property(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_key k = embi_js_to_property_key(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    int st = embi_js_has_own(rt, o, k);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value proto_is_prototype_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_value v = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(v)) {
        return EMBI_JS_FALSE;
    }
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_value p;
    if (embi_js_obj_get_prototype_of(rt, embi_js_as_object(v), &p) == EMBI_JS_THREW) {
        return EXC;
    }
    while (embi_js_is_object(p)) {
        if (embi_js_as_object(p) == o) {
            return EMBI_JS_TRUE;
        }
        if (embi_js_obj_get_prototype_of(rt, embi_js_as_object(p), &p) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return EMBI_JS_FALSE;
}

static embi_js_value proto_property_is_enumerable(embi_js_realm *r, embi_js_value this_v, int argc,
                                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_key k = embi_js_to_property_key(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_pdesc d;
    int st = embi_js_obj_get_own_property(rt, o, k, &d);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES && (d.attrs & ATTR_E));
}

static embi_js_value proto_to_locale_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    return embi_js_invoke(r->rt, this_v, embi_js_name_val(r->rt, EMBI_JS_NAME_TO_STRING), 0,
                          nullptr);
}

static embi_js_value proto_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(this_v)) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "[object Undefined]", 18));
    }
    if (embi_js_is_null(this_v)) {
        return embi_js_from_string(embi_js_atom_intern_utf8(rt, "[object Null]", 13));
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    const char *builtin = "Object";
    embi_js_value ov = embi_js_from_object(o);
    int is_arr = embi_js_is_array_checked(rt, ov);
    if (is_arr == EMBI_JS_THREW) {
        return EXC;
    }
    if (is_arr == EMBI_JS_YES) {
        builtin = "Array";
    } else if (o->klass == &embi_js_class_arguments) {
        builtin = "Arguments";
    } else if (embi_js_is_callable(ov)) {
        builtin = "Function";
    } else if (o->klass == &embi_js_class_error) {
        builtin = "Error";
    } else if (o->klass == &embi_js_class_boolean) {
        builtin = "Boolean";
    } else if (o->klass == &embi_js_class_number) {
        builtin = "Number";
    } else if (o->klass == &embi_js_class_string) {
        builtin = "String";
    } else if (o->klass == &embi_js_class_date) {
        builtin = "Date";
    } else if (embi_js_is_regexp(ov)) {
        builtin = "RegExp";
    }
    embi_js_value tag = embi_js_obj_get(rt, o, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG), ov);
    if (tag == EXC) {
        return EXC;
    }
    embi_sb sb = {nullptr, 0, 0};
    sb_text(&sb, "[object ");
    if (embi_js_is_string(tag)) {
        embi_js_str_append_utf8(embi_js_as_string(tag), &sb);
    } else {
        sb_text(&sb, builtin);
    }
    embi_sb_append_char(&sb, ']');
    embi_js_string *s = embi_js_str_new_utf8(rt, sb.data, sb.len);
    embi_sb_free(&sb);
    return s ? embi_js_from_string(s) : EXC;
}

static embi_js_value proto_value_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_object *o = embi_js_to_object(r->rt, this_v);
    return o ? embi_js_from_object(o) : EXC;
}

/* __defineGetter__ (magic 0) and __defineSetter__ (magic 1), Annex B.2.2.2 and B.2.2.3. */
static embi_js_value proto_define_accessor(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_value fn = argc > 1 ? argv[1] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_callable(fn)) {
        return embi_js_throw_type_error(rt, "Object.prototype.__define%cetter__: accessor is not "
                                            "a function",
                                        magic == 0 ? 'G' : 'S');
    }
    embi_js_key k = embi_js_to_property_key(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    embi_js_pdesc d;
    d.has = (uint8_t)((magic == 0 ? EMBI_JS_PD_GET : EMBI_JS_PD_SET) | EMBI_JS_PD_ENUMERABLE |
                      EMBI_JS_PD_CONFIGURABLE);
    d.attrs = ATTR_E | ATTR_C;
    d.value = EMBI_JS_UNDEFINED;
    d.get = magic == 0 ? fn : EMBI_JS_UNDEFINED;
    d.set = magic == 1 ? fn : EMBI_JS_UNDEFINED;
    if (embi_js_define_property_or_throw(rt, o, k, &d) == EMBI_JS_THREW) {
        return EXC;
    }
    return EMBI_JS_UNDEFINED;
}

/* __lookupGetter__ (magic 0) and __lookupSetter__ (magic 1), Annex B.2.2.4 and B.2.2.5. */
static embi_js_value proto_lookup_accessor(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (k == EXC) {
        return EXC;
    }
    while (true) {
        embi_js_pdesc d;
        int st = embi_js_obj_get_own_property(rt, o, k, &d);
        if (st == EMBI_JS_THREW) {
            return EXC;
        }
        if (st == EMBI_JS_YES) {
            if (!embi_js_pd_is_accessor(&d)) {
                return EMBI_JS_UNDEFINED;
            }
            embi_js_value f = magic == 0 ? d.get : d.set;
            return (d.has & (magic == 0 ? EMBI_JS_PD_GET : EMBI_JS_PD_SET)) ? f : EMBI_JS_UNDEFINED;
        }
        embi_js_value p;
        if (embi_js_obj_get_prototype_of(rt, o, &p) == EMBI_JS_THREW) {
            return EXC;
        }
        if (!embi_js_is_object(p)) {
            return EMBI_JS_UNDEFINED;
        }
        o = embi_js_as_object(p);
    }
}

/* The __proto__ accessor (Annex B.2.2.1). */
static embi_js_value proto_get_proto(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    embi_js_value p;
    if (!o || embi_js_obj_get_prototype_of(rt, o, &p) == EMBI_JS_THREW) {
        return EXC;
    }
    return p;
}

static embi_js_value proto_set_proto(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_nullish(this_v)) {
        return embi_js_throw_type_error(rt, "Object.prototype.__proto__ called on null or undefined");
    }
    embi_js_value proto = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (!embi_js_is_object(proto) && !embi_js_is_null(proto)) {
        return EMBI_JS_UNDEFINED;
    }
    if (!embi_js_is_object(this_v)) {
        return EMBI_JS_UNDEFINED;
    }
    int st = embi_js_obj_set_prototype_of(rt, embi_js_as_object(this_v), proto);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return embi_js_throw_type_error(rt, "Object.prototype.__proto__: prototype cannot be changed");
    }
    return EMBI_JS_UNDEFINED;
}

/* ------------------------------------------------------------------ Object.fromEntries (ES2019) */

/* Closes an iterator after an abrupt completion of the caller: the return method runs, its errors
 * are dropped, and the pending exception (the caller's) is kept. */
static void iter_close_quiet(embi_js_runtime *rt, embi_js_value it)
{
    embi_js_value pending = embi_js_exception_get(rt);
    embi_js_value m = embi_js_get_method(rt, it, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (m != EXC && m != EMBI_JS_UNDEFINED) {
        (void)embi_js_call(rt, m, it, 0, nullptr);
    }
    embi_js_exception_clear(rt);
    embi_js_exception_set(rt, pending);
}

/* Object.fromEntries(iterable): each entry is an object whose "0" and "1" properties give the key
 * and the value. An abrupt completion closes the iterator. */
static embi_js_value object_from_entries(embi_js_realm *r, embi_js_value this_v, int argc,
                                         const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value iterable = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (embi_js_is_nullish(iterable)) {
        return embi_js_throw_type_error(rt, "Object.fromEntries requires an iterable argument");
    }
    embi_js_value method = embi_js_get_method(rt, iterable, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method == EMBI_JS_UNDEFINED) {
        return embi_js_throw_type_error(rt, "Object.fromEntries: argument is not iterable");
    }
    embi_js_object *obj = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
    embi_js_value it = embi_js_call(rt, method, iterable, 0, nullptr);
    if (it == EXC) {
        return EXC;
    }
    if (!embi_js_is_object(it)) {
        return embi_js_throw_type_error(rt, "Result of the Symbol.iterator method is not an object");
    }
    embi_js_value next = embi_js_get(rt, embi_js_as_object(it), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
    if (next == EXC) {
        return EXC;
    }
    for (;;) {
        embi_js_value res = embi_js_call(rt, next, it, 0, nullptr);
        if (res == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(res)) {
            return embi_js_throw_type_error(rt, "Iterator result is not an object");
        }
        embi_js_value done = embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_DONE));
        if (done == EXC) {
            return EXC;
        }
        if (embi_js_to_boolean(done)) {
            break;
        }
        embi_js_value entry = embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_VALUE));
        if (entry == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(entry)) {
            (void)embi_js_throw_type_error(rt, "Iterator value is not an entry object");
            iter_close_quiet(rt, it);
            return EXC;
        }
        embi_js_value kv = embi_js_get(rt, embi_js_as_object(entry), embi_js_key_from_index(0));
        if (kv == EXC) {
            iter_close_quiet(rt, it);
            return EXC;
        }
        embi_js_value vv = embi_js_get(rt, embi_js_as_object(entry), embi_js_key_from_index(1));
        if (vv == EXC) {
            iter_close_quiet(rt, it);
            return EXC;
        }
        embi_js_key k = embi_js_to_property_key(rt, kv);
        if (k == EXC) {
            iter_close_quiet(rt, it);
            return EXC;
        }
        if (embi_js_create_data_property_or_throw(rt, obj, k, vv) == EMBI_JS_THREW) {
            iter_close_quiet(rt, it);
            return EXC;
        }
    }
    return embi_js_from_object(obj);
}

/* ------------------------------------------------------------------ installation */

typedef struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
    int magic;
} method_def;

static const method_def object_statics[] = {
    {"assign", object_assign, 2, 0},
    {"create", object_create, 2, 0},
    {"defineProperties", object_define_properties, 2, 0},
    {"defineProperty", object_define_property, 3, 0},
    {"entries", object_entries, 1, 0},
    {"freeze", object_set_level, 1, 1},
    {"fromEntries", object_from_entries, 1, 0},
    {"getOwnPropertyDescriptor", object_get_own_property_descriptor, 2, 0},
    {"getOwnPropertyDescriptors", object_get_own_property_descriptors, 1, 0},
    {"getOwnPropertyNames", object_get_own_property_names, 1, 0},
    {"getOwnPropertySymbols", object_get_own_property_symbols, 1, 0},
    {"getPrototypeOf", object_get_prototype_of, 1, 0},
    {"hasOwn", object_has_own, 2, 0},
    {"is", object_is, 2, 0},
    {"isExtensible", object_is_extensible, 1, 0},
    {"isFrozen", object_is_frozen, 1, 0},
    {"isSealed", object_is_sealed, 1, 0},
    {"keys", object_keys, 1, 0},
    {"preventExtensions", object_prevent_extensions, 1, 0},
    {"seal", object_set_level, 1, 0},
    {"setPrototypeOf", object_set_prototype_of, 2, 0},
    {"values", object_values, 1, 0},
};

static const method_def object_proto_methods[] = {
    {"hasOwnProperty", proto_has_own_property, 1, 0},
    {"isPrototypeOf", proto_is_prototype_of, 1, 0},
    {"propertyIsEnumerable", proto_property_is_enumerable, 1, 0},
    {"toLocaleString", proto_to_locale_string, 0, 0},
    {"toString", proto_to_string, 0, 0},
    {"valueOf", proto_value_of, 0, 0},
};

/* Creates Object.prototype, the root of every ordinary object. It runs first, before any function
 * object exists, so it creates only the prototype object itself. */
void embi_js_init_builtins_object(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, EMBI_JS_NULL);
    rt->protos[EMBI_JS_PROTO_OBJECT] = embi_js_from_object(proto);
}

/* Installs the Object constructor, its statics and the Object.prototype methods, and binds Object
 * on the global object. Each method_def passes its magic value to its function: freeze and seal
 * share one function, and magic 1 is freeze. */
void embi_js_builtins_object_install(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_as_object(rt->protos[EMBI_JS_PROTO_OBJECT]);

    embi_js_object *ctor = embi_js_native_constructor_new(r, "Object", 1, object_ctor, 0);
    r->object_ctor = ctor;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Object"), embi_js_from_object(ctor),
                       ATTR_WC);

    for (size_t i = 0; i < sizeof object_statics / sizeof object_statics[0]; i++) {
        const method_def *m = &object_statics[i];
        embi_js_define_method(r, ctor, m->name, m->fn, m->length, m->magic);
    }
    for (size_t i = 0; i < sizeof object_proto_methods / sizeof object_proto_methods[0]; i++) {
        const method_def *m = &object_proto_methods[i];
        embi_js_define_method(r, proto, m->name, m->fn, m->length, m->magic);
    }
    /* Annex B legacy accessor methods. */
    embi_js_define_method(r, proto, "__defineGetter__", proto_define_accessor, 2, 0);
    embi_js_define_method(r, proto, "__defineSetter__", proto_define_accessor, 2, 1);
    embi_js_define_method(r, proto, "__lookupGetter__", proto_lookup_accessor, 1, 0);
    embi_js_define_method(r, proto, "__lookupSetter__", proto_lookup_accessor, 1, 1);

    embi_js_object *getter = embi_js_native_function_new(r, "get __proto__", 0, proto_get_proto, 0);
    embi_js_object *setter = embi_js_native_function_new(r, "set __proto__", 1, proto_set_proto, 0);
    embi_js_define_accessor_raw(rt, proto, embi_js_key_c(rt, "__proto__"),
                                embi_js_from_object(getter), embi_js_from_object(setter),
                                ATTR_C);
}
