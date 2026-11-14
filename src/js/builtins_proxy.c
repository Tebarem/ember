/*
 * Proxy (ECMA-262 10.5 and 28.2) and Reflect (28.1).
 *
 * A proxy's internal methods are the class hooks of embi_js_class_proxy and
 * embi_js_class_proxy_callable (object.h). Each hook looks up its trap on the handler, forwards to
 * the target when the trap is absent, and checks the spec invariants after the trap returns. Every
 * generic object operation (get, set, has, delete, defineProperty, ownKeys, the prototype and
 * extensibility methods, call and construct) reaches these hooks, and Reflect calls the same
 * internal methods, so proxies behave the same through every path.
 *
 * The proxy's internal is a varray: [target, handler, scratch]. Revocation sets target and handler
 * to EMBI_JS_NULL. The scratch slot holds the distinct symbol keys that the ownKeys trap returned
 * (pin_symbol), so they stay alive after the call; string keys are atoms, which are always rooted.
 */
#include "js/builtins.h"
#include "js/ic.h"
#include "js/js.h"

#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

#define SLOT_TARGET 0
#define SLOT_HANDLER 1
#define SLOT_SCRATCH 2

/* Trap caches (J5d): one handler shape (SLOT_TCSHAPE) and one int32 slot per trap (SLOT_TRAP(id)),
 * -1 when the trap has no entry. The three traps share the shape, so a proxy costs 4 values more
 * than before rather than 6: the shape is the handler shape all filled entries were read under.
 * A fill under a different shape clears the other entries first. */
enum { TRAP_GET, TRAP_SET, TRAP_HAS, TRAP_COUNT };
#define SLOT_TCSHAPE (SLOT_SCRATCH + 1u)
#define SLOT_TRAP(id) (SLOT_TCSHAPE + 1u + (uint32_t)(id))
#define PROXY_SLOTS SLOT_TRAP(TRAP_COUNT)

/* Largest key list a trap may return (CreateListFromArrayLike limit, as for apply). */
#define PROXY_MAX_KEYS (1u << 24)

/* ------------------------------------------------------------------ parts and helpers */

bool embi_js_proxy_parts(const embi_js_object *o, embi_js_value *target, embi_js_value *handler)
{
    if (!embi_js_obj_is_proxy(o)) {
        *target = EMBI_JS_UNDEFINED;
        *handler = EMBI_JS_NULL;
        return false;
    }
    const embi_js_varray *d = (const embi_js_varray *)o->internal;
    *target = d->elems[SLOT_TARGET];
    *handler = d->elems[SLOT_HANDLER];
    return true;
}

static embi_js_varray *proxy_slots(const embi_js_object *o)
{
    return (embi_js_varray *)o->internal;
}

static void proxy_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

/* Throws a TypeError with a fixed message and returns THREW (for int-returning hooks). */
static int throw_thr(embi_js_runtime *rt, const char *msg)
{
    (void)embi_js_throw_type_error(rt, "%s", msg);
    return EMBI_JS_THREW;
}

/* Throws a TypeError with a fixed message and returns EXC (for value-returning hooks). */
static embi_js_value throw_exc(embi_js_runtime *rt, const char *msg)
{
    return embi_js_throw_type_error(rt, "%s", msg);
}

/* The handler's trap key (the trap name is the operation name), or UNDEFINED when the handler has
 * none. op names the operation in the message. Sets *target and *handler. Returns EXC when the
 * proxy is revoked or the trap is not callable. key is the interned name of op: get and set pass
 * the runtime's interned names, the other traps intern theirs on each call. */
static embi_js_value get_trap(embi_js_runtime *rt, const embi_js_object *p, const char *op,
                              embi_js_key key, embi_js_value *target, embi_js_value *handler)
{
    embi_js_proxy_parts(p, target, handler);
    if (embi_js_is_null(*handler)) {
        return embi_js_throw_type_error(rt, "Cannot perform '%s' on a proxy that has been revoked",
                                        op);
    }
    return embi_js_get_method(rt, *handler, key);
}

/* A handler whose own data properties are the trap slots a cache entry can name: an ordinary class
 * with no named hooks, in shape mode (or a prototype, whose dictionary shape renews on each change). */
static bool handler_trap_cacheable(const embi_js_object *h)
{
    const embi_js_class *k = h->klass;
    if (!(k->flags & EMBI_JS_CLASS_IC) || k->get_own || k->get || k->has) {
        return false;
    }
    return !(h->shape->hdr.flags & EMBI_JS_SHAPE_DICT) || (h->hdr.flags & EMBI_JS_OBJ_IS_PROTO);
}

/* The trap a lookup found, as get_method reports it: undefined when absent, the callable otherwise. */
static embi_js_value trap_of(embi_js_runtime *rt, embi_js_value f)
{
    if (embi_js_is_nullish(f)) {
        return UNDEF;
    }
    if (!embi_js_is_callable(f)) {
        return embi_js_throw_type_error(rt, "property is not a function");
    }
    return f;
}

/* get_trap for the cached traps (id is TRAP_*). A hit reads the trap from the cached slot of a handler
 * with the same shape; a miss fills the entry when the trap is an own data property of a cacheable
 * handler, and otherwise looks it up as get_trap does. The key is the trap name for id. */
static embi_js_value get_trap_c(embi_js_runtime *rt, const embi_js_object *p, uint32_t id,
                                const char *op, embi_js_value *target, embi_js_value *handler)
{
    embi_js_proxy_parts(p, target, handler);
    if (embi_js_is_null(*handler)) {
        return embi_js_throw_type_error(rt, "Cannot perform '%s' on a proxy that has been revoked",
                                        op);
    }
    if (!embi_js_is_object(*handler)) {
        return embi_js_get_method(rt, *handler, embi_js_key_c(rt, op));
    }
    embi_js_object *h = embi_js_as_object(*handler);
    if (!handler_trap_cacheable(h)) {
        return embi_js_get_method(rt, *handler, embi_js_key_c(rt, op));
    }
    embi_js_varray *d = (embi_js_varray *)p->internal;
    uint32_t at = SLOT_TRAP(id);
    embi_js_value shape_v = embi_js_from_cell(h->shape);
    if (d->elems[SLOT_TCSHAPE] == shape_v) {
        int32_t s = embi_js_as_int32(d->elems[at]);
        if (s >= 0) {
            return trap_of(rt, *embi_js_obj_ic_value(h, (uint32_t)s));
        }
    }
    /* The key is interned on a miss only; a hit never touches the atom table. */
    embi_js_key key = embi_js_key_c(rt, op);
    uint32_t slot;
    uint8_t attrs;
    if (embi_js_obj_ic_own(h, key, &slot, &attrs) && !(attrs & EMBI_JS_ATTR_ACCESSOR)) {
        if (d->elems[SLOT_TCSHAPE] != shape_v) {
            EMBI_JS_STORE(d, d->elems[SLOT_TCSHAPE], shape_v);
            for (uint32_t i = 0; i < TRAP_COUNT; i++) {
                EMBI_JS_STORE(d, d->elems[SLOT_TRAP(i)], embi_js_from_int32(-1));
            }
            EMBI_JS_WB(d, shape_v);
        }
        EMBI_JS_STORE(d, d->elems[at], embi_js_from_int32((int32_t)slot));
        return trap_of(rt, *embi_js_obj_ic_value(h, slot));
    }
    return embi_js_get_method(rt, *handler, key);
}

bool embi_js_proxy_trap_entry(const embi_js_object *p, uint32_t id, const embi_js_shape **shape,
                              int32_t *slot)
{
    if (!embi_js_obj_is_proxy(p) || id >= TRAP_COUNT) {
        return false;
    }
    const embi_js_varray *d = (const embi_js_varray *)p->internal;
    *shape = embi_js_is_cell(d->elems[SLOT_TCSHAPE])
                 ? (const embi_js_shape *)embi_js_as_cell(d->elems[SLOT_TCSHAPE])
                 : nullptr;
    *slot = embi_js_as_int32(d->elems[SLOT_TRAP(id)]);
    return true;
}

/* Keeps a symbol key alive for the life of the proxy. A trap can return symbols that nothing else
 * holds, and the key vector given to the caller is not GC-visible, so each distinct symbol is
 * appended once to the scratch slot. Strings need no pin: every atom is a root. The slot holds a
 * varray: elems[0] is the pin count (int32), elems[1..] the pinned symbols, and hdr.aux the
 * capacity plus one; it doubles when full. pinned is a per-call set of the symbols already
 * pinned, built lazily from the slot on the first symbol, so a repeat is found in O(1). */
static void pin_symbol(embi_js_runtime *rt, embi_js_object *o, embi_js_value sym,
                       embi_hashmap *pinned, bool *built)
{
    embi_js_varray *d = proxy_slots(o);
    embi_js_value cur = d->elems[SLOT_SCRATCH];
    embi_js_varray *pins = embi_js_is_cell(cur) ? (embi_js_varray *)embi_js_as_cell(cur) : nullptr;
    uint32_t n = pins ? (uint32_t)embi_js_as_int32(pins->elems[0]) : 0;
    if (!*built) {
        for (uint32_t i = 0; i < n; i++) {
            embi_hashmap_put(pinned, pins->elems[1 + i], 1);
        }
        *built = true;
    }
    uint64_t dummy;
    if (embi_hashmap_get(pinned, sym, &dummy)) {
        return;
    }
    embi_hashmap_put(pinned, sym, 1);
    if (!pins || n + 1 >= pins->hdr.aux) {
        uint32_t cap = pins ? pins->hdr.aux * 2 : 8;
        embi_js_varray *grown = embi_js_varray_new(rt, cap);
        if (pins) {
            for (uint32_t i = 0; i < n; i++) {
                EMBI_JS_STORE(grown, grown->elems[1 + i], pins->elems[1 + i]);
                EMBI_JS_WB(grown, pins->elems[1 + i]);
            }
        }
        pins = grown;
        EMBI_JS_STORE(d, d->elems[SLOT_SCRATCH], embi_js_from_cell(pins));
        EMBI_JS_WB(d, pins);
    }
    EMBI_JS_STORE(pins, pins->elems[1 + n], sym);
    EMBI_JS_WB(pins, sym);
    EMBI_JS_STORE(pins, pins->elems[0], embi_js_from_int32((int32_t)(n + 1)));
}

/* Fills the missing fields of a descriptor with their defaults (CompletePropertyDescriptor). */
static void complete_pd(embi_js_pdesc *d)
{
    if (!embi_js_pd_is_accessor(d)) {
        if (!(d->has & EMBI_JS_PD_VALUE)) {
            d->value = UNDEF;
            d->has |= EMBI_JS_PD_VALUE;
        }
        if (!(d->has & EMBI_JS_PD_WRITABLE)) {
            d->attrs = (uint8_t)(d->attrs & ~ATTR_W);
            d->has |= EMBI_JS_PD_WRITABLE;
        }
    } else {
        if (!(d->has & EMBI_JS_PD_GET)) {
            d->get = UNDEF;
            d->has |= EMBI_JS_PD_GET;
        }
        if (!(d->has & EMBI_JS_PD_SET)) {
            d->set = UNDEF;
            d->has |= EMBI_JS_PD_SET;
        }
    }
    if (!(d->has & EMBI_JS_PD_ENUMERABLE)) {
        d->attrs = (uint8_t)(d->attrs & ~ATTR_E);
        d->has |= EMBI_JS_PD_ENUMERABLE;
    }
    if (!(d->has & EMBI_JS_PD_CONFIGURABLE)) {
        d->attrs = (uint8_t)(d->attrs & ~ATTR_C);
        d->has |= EMBI_JS_PD_CONFIGURABLE;
    }
}

/* An array holding argv[0..argc), built with CreateArrayFromList. */
static embi_js_object *array_from_list(embi_js_runtime *rt, int argc, const embi_js_value *argv)
{
    embi_js_object *a = embi_js_array_create_default(rt, 0.0);
    if (!a) {
        return nullptr;
    }
    for (int i = 0; i < argc; i++) {
        if (embi_js_create_data_property_or_throw(rt, a, embi_js_key_from_index((uint32_t)i),
                                                  argv[i]) == EMBI_JS_THREW) {
            return nullptr;
        }
    }
    return a;
}

/* ------------------------------------------------------------------ [[GetPrototypeOf]] */

static int proxy_get_proto(embi_js_runtime *rt, embi_js_object *o, embi_js_value *out)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "getPrototypeOf",
                                  embi_js_key_c(rt, "getPrototypeOf"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_get_prototype_of(rt, t, out);
    }
    embi_js_value hp = embi_js_call(rt, trap, handler, 1, &target);
    if (hp == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_object(hp) && !embi_js_is_null(hp)) {
        return throw_thr(rt, "getPrototypeOf trap returned neither an object nor null");
    }
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (ext == EMBI_JS_YES) {
        *out = hp;
        return EMBI_JS_YES;
    }
    embi_js_value tp;
    if (embi_js_obj_get_prototype_of(rt, t, &tp) == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_same_value(hp, tp)) {
        return throw_thr(rt, "getPrototypeOf trap result differs from the non-extensible target");
    }
    *out = hp;
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[SetPrototypeOf]] */

static int proxy_set_proto(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "setPrototypeOf",
                                  embi_js_key_c(rt, "setPrototypeOf"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_set_prototype_of(rt, t, proto);
    }
    embi_js_value argv[2] = {target, proto};
    embi_js_value r = embi_js_call(rt, trap, handler, 2, argv);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_to_boolean(r)) {
        return EMBI_JS_NO;
    }
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (ext == EMBI_JS_YES) {
        return EMBI_JS_YES;
    }
    embi_js_value tp;
    if (embi_js_obj_get_prototype_of(rt, t, &tp) == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_same_value(proto, tp)) {
        return throw_thr(rt, "setPrototypeOf trap returned true for a different prototype");
    }
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[IsExtensible]] and [[PreventExtensions]] */

static int proxy_is_extensible(embi_js_runtime *rt, embi_js_object *o)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "isExtensible",
                                  embi_js_key_c(rt, "isExtensible"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_extensible(rt, t);
    }
    embi_js_value r = embi_js_call(rt, trap, handler, 1, &target);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    bool b = embi_js_to_boolean(r);
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if ((ext == EMBI_JS_YES) != b) {
        return throw_thr(rt, "isExtensible trap result does not reflect the target");
    }
    return b ? EMBI_JS_YES : EMBI_JS_NO;
}

static int proxy_prevent_extensions(embi_js_runtime *rt, embi_js_object *o)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "preventExtensions",
                                  embi_js_key_c(rt, "preventExtensions"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_prevent_extensions(rt, t);
    }
    embi_js_value r = embi_js_call(rt, trap, handler, 1, &target);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    bool b = embi_js_to_boolean(r);
    if (b) {
        int ext = embi_js_obj_extensible(rt, t);
        if (ext == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (ext == EMBI_JS_YES) {
            return throw_thr(rt, "preventExtensions trap returned true but the target is extensible");
        }
    }
    return b ? EMBI_JS_YES : EMBI_JS_NO;
}

/* ------------------------------------------------------------------ [[GetOwnProperty]] */

static int proxy_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                         embi_js_pdesc *out)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "getOwnPropertyDescriptor",
                                  embi_js_key_c(rt, "getOwnPropertyDescriptor"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_get_own_property(rt, t, k, out);
    }
    embi_js_value argv[2] = {target, embi_js_key_to_value(rt, k)};
    embi_js_value res = embi_js_call(rt, trap, handler, 2, argv);
    if (res == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_object(res) && !embi_js_is_undefined(res)) {
        return throw_thr(rt, "getOwnPropertyDescriptor trap returned neither an object nor undefined");
    }
    embi_js_pdesc td;
    int tr = embi_js_obj_get_own_property(rt, t, k, &td);
    if (tr == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (embi_js_is_undefined(res)) {
        if (tr == EMBI_JS_NO) {
            return EMBI_JS_NO;
        }
        if (!(td.attrs & ATTR_C)) {
            return throw_thr(rt, "getOwnPropertyDescriptor trap hid a non-configurable property");
        }
        int ext = embi_js_obj_extensible(rt, t);
        if (ext == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (ext == EMBI_JS_NO) {
            return throw_thr(rt, "getOwnPropertyDescriptor trap hid a property of a non-extensible target");
        }
        return EMBI_JS_NO;
    }
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    embi_js_pdesc rd;
    if (!embi_js_to_property_descriptor(rt, res, &rd)) {
        return EMBI_JS_THREW;
    }
    complete_pd(&rd);
    bool compat = tr == EMBI_JS_YES ? embi_js_pd_compatible(&rd, &td) : ext == EMBI_JS_YES;
    if (!compat) {
        return throw_thr(rt, "getOwnPropertyDescriptor trap result is incompatible with the target");
    }
    if (!(rd.attrs & ATTR_C)) {
        if (tr == EMBI_JS_NO) {
            return throw_thr(rt, "getOwnPropertyDescriptor trap reported a non-configurable absent property");
        }
        if (td.attrs & ATTR_C) {
            return throw_thr(rt, "getOwnPropertyDescriptor trap reported a configurable property as non-configurable");
        }
        if (!embi_js_pd_is_accessor(&rd) && !(rd.attrs & ATTR_W) && !embi_js_pd_is_accessor(&td) &&
            (td.attrs & ATTR_W)) {
            return throw_thr(rt, "getOwnPropertyDescriptor trap reported a writable property as non-writable");
        }
    }
    *out = rd;
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[DefineOwnProperty]] */

static int proxy_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                            const embi_js_pdesc *d)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "defineProperty",
                                  embi_js_key_c(rt, "defineProperty"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_define_own_property(rt, t, k, d);
    }
    embi_js_value descv = embi_js_from_property_descriptor(rt, d);
    if (descv == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_value argv[3] = {target, embi_js_key_to_value(rt, k), descv};
    embi_js_value r = embi_js_call(rt, trap, handler, 3, argv);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_to_boolean(r)) {
        return EMBI_JS_NO;
    }
    embi_js_pdesc td;
    int tr = embi_js_obj_get_own_property(rt, t, k, &td);
    if (tr == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    bool setting_config_false = (d->has & EMBI_JS_PD_CONFIGURABLE) && !(d->attrs & ATTR_C);
    if (tr == EMBI_JS_NO) {
        if (ext == EMBI_JS_NO) {
            return throw_thr(rt, "defineProperty trap added a property to a non-extensible target");
        }
        if (setting_config_false) {
            return throw_thr(rt, "defineProperty trap reported a non-configurable absent property");
        }
    } else {
        if (!embi_js_pd_compatible(d, &td)) {
            return throw_thr(rt, "defineProperty trap result is incompatible with the target property");
        }
        if (setting_config_false && (td.attrs & ATTR_C)) {
            return throw_thr(rt, "defineProperty trap reported a configurable property as non-configurable");
        }
        if (!embi_js_pd_is_accessor(&td) && !(td.attrs & ATTR_C) && (td.attrs & ATTR_W) &&
            (d->has & EMBI_JS_PD_WRITABLE) && !(d->attrs & ATTR_W)) {
            return throw_thr(rt, "defineProperty trap made a non-configurable writable property read-only");
        }
    }
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[HasProperty]] */

static int proxy_has(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap_c(rt, o, TRAP_HAS, "has",
                                  &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_has_property(rt, t, k);
    }
    embi_js_value argv[2] = {target, embi_js_key_to_value(rt, k)};
    embi_js_value r = embi_js_call(rt, trap, handler, 2, argv);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    bool b = embi_js_to_boolean(r);
    if (!b) {
        embi_js_pdesc td;
        int tr = embi_js_obj_get_own_property(rt, t, k, &td);
        if (tr == EMBI_JS_THREW) {
            return EMBI_JS_THREW;
        }
        if (tr == EMBI_JS_YES) {
            if (!(td.attrs & ATTR_C)) {
                return throw_thr(rt, "has trap hid a non-configurable property");
            }
            int ext = embi_js_obj_extensible(rt, t);
            if (ext == EMBI_JS_THREW) {
                return EMBI_JS_THREW;
            }
            if (ext == EMBI_JS_NO) {
                return throw_thr(rt, "has trap hid a property of a non-extensible target");
            }
        }
    }
    return b ? EMBI_JS_YES : EMBI_JS_NO;
}

/* ------------------------------------------------------------------ [[Get]] and [[Set]] */

static embi_js_value proxy_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                               embi_js_value receiver)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap_c(rt, o, TRAP_GET, "get",
                                  &target, &handler);
    if (trap == EXC) {
        return EXC;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_get(rt, t, k, receiver);
    }
    embi_js_value argv[3] = {target, embi_js_key_to_value(rt, k), receiver};
    embi_js_value res = embi_js_call(rt, trap, handler, 3, argv);
    if (res == EXC) {
        return EXC;
    }
    embi_js_pdesc td;
    int tr = embi_js_obj_get_own_property(rt, t, k, &td);
    if (tr == EMBI_JS_THREW) {
        return EXC;
    }
    if (tr == EMBI_JS_YES && !(td.attrs & ATTR_C)) {
        if (!embi_js_pd_is_accessor(&td)) {
            if (!(td.attrs & ATTR_W) && !embi_js_same_value(res, td.value)) {
                return throw_exc(rt, "get trap result differs from a non-writable, non-configurable value");
            }
        } else if (td.get == UNDEF && !embi_js_is_undefined(res)) {
            return throw_exc(rt, "get trap returned a value for a non-configurable accessor without a getter");
        }
    }
    return res;
}

static int proxy_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                     embi_js_value receiver)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap_c(rt, o, TRAP_SET, "set",
                                  &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_set(rt, t, k, v, receiver);
    }
    embi_js_value argv[4] = {target, embi_js_key_to_value(rt, k), v, receiver};
    embi_js_value r = embi_js_call(rt, trap, handler, 4, argv);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_to_boolean(r)) {
        return EMBI_JS_NO;
    }
    embi_js_pdesc td;
    int tr = embi_js_obj_get_own_property(rt, t, k, &td);
    if (tr == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (tr == EMBI_JS_YES && !(td.attrs & ATTR_C)) {
        if (!embi_js_pd_is_accessor(&td)) {
            if (!(td.attrs & ATTR_W) && !embi_js_same_value(v, td.value)) {
                return throw_thr(rt, "set trap changed a non-writable, non-configurable value");
            }
        } else if (td.set == UNDEF) {
            return throw_thr(rt, "set trap succeeded for a non-configurable accessor without a setter");
        }
    }
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[Delete]] */

static int proxy_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "deleteProperty",
                                  embi_js_key_c(rt, "deleteProperty"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_delete(rt, t, k);
    }
    embi_js_value argv[2] = {target, embi_js_key_to_value(rt, k)};
    embi_js_value r = embi_js_call(rt, trap, handler, 2, argv);
    if (r == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_to_boolean(r)) {
        return EMBI_JS_NO;
    }
    embi_js_pdesc td;
    int tr = embi_js_obj_get_own_property(rt, t, k, &td);
    if (tr == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (tr == EMBI_JS_NO) {
        return EMBI_JS_YES;
    }
    if (!(td.attrs & ATTR_C)) {
        return throw_thr(rt, "deleteProperty trap deleted a non-configurable property");
    }
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (ext == EMBI_JS_NO) {
        return throw_thr(rt, "deleteProperty trap deleted a property of a non-extensible target");
    }
    return EMBI_JS_YES;
}

/* ------------------------------------------------------------------ [[OwnPropertyKeys]] */

/* CreateListFromArrayLike(list, «String, Symbol») with a duplicate check: appends the property keys
 * to keep and records each one in seen. keep grows with the elements actually read, so a huge
 * claimed length costs nothing until the elements exist. Symbols are pinned (pin_symbol) as they
 * are read, so the plain vector needs no GC rooting. Returns YES or THREW. */
static int trap_key_list(embi_js_runtime *rt, embi_js_object *p, embi_js_object *list,
                         double count, embi_js_key_vec *keep, embi_hashmap *seen,
                         embi_js_value list_v)
{
    embi_hashmap pinned;
    embi_hashmap_init(&pinned);
    bool built = false;
    int result = EMBI_JS_YES;
    for (double di = 0; di < count && result == EMBI_JS_YES; di += 1.0) {
        if (di >= (double)PROXY_MAX_KEYS) {
            (void)embi_js_throw_range_error(rt, "ownKeys trap result is too long");
            result = EMBI_JS_THREW;
            break;
        }
        uint32_t i = (uint32_t)di;
        embi_js_value e = embi_js_obj_get(rt, list, embi_js_key_from_index(i), list_v);
        if (e == EXC) {
            result = EMBI_JS_THREW;
        } else if (!embi_js_is_string(e) && !embi_js_is_symbol(e)) {
            result = throw_thr(rt,
                               "ownKeys trap result contains a value that is not a string or symbol");
        } else {
            embi_js_key pk = embi_js_to_property_key(rt, e);
            uint64_t dummy;
            if (pk == EXC) {
                result = EMBI_JS_THREW;
            } else if (embi_hashmap_get(seen, pk, &dummy)) {
                result = throw_thr(rt, "ownKeys trap result contains duplicate entries");
            } else {
                embi_hashmap_put(seen, pk, 1);
                if (embi_js_is_symbol(pk)) {
                    pin_symbol(rt, p, pk, &pinned, &built);
                }
                embi_vec_push(keep, pk);
            }
        }
    }
    embi_hashmap_free(&pinned);
    return result;
}

/* The invariants of [[OwnPropertyKeys]] against the target. seen holds the trap keys; the
 * non-configurable target keys are removed from it, and when the target is not extensible the
 * configurable ones are too, which must leave it empty. */
static int own_keys_check(embi_js_runtime *rt, embi_js_object *t, embi_hashmap *seen)
{
    int ext = embi_js_obj_extensible(rt, t);
    if (ext == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    embi_js_key_vec tk = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, t, &tk) == EMBI_JS_THREW) {
        embi_vec_free(&tk);
        return EMBI_JS_THREW;
    }
    embi_js_key_vec conf = {nullptr, 0, 0};
    int result = EMBI_JS_YES;
    for (uint32_t i = 0; i < tk.len && result == EMBI_JS_YES; i++) {
        embi_js_pdesc d;
        int r = embi_js_obj_get_own_property(rt, t, tk.data[i], &d);
        if (r == EMBI_JS_THREW) {
            result = EMBI_JS_THREW;
        } else if (r == EMBI_JS_YES && !(d.attrs & ATTR_C)) {
            if (!embi_hashmap_remove(seen, tk.data[i])) {
                result = throw_thr(rt, "ownKeys trap result omits a non-configurable key");
            }
        } else if (r == EMBI_JS_YES) {
            embi_vec_push(&conf, tk.data[i]);
        }
    }
    if (result == EMBI_JS_YES && ext == EMBI_JS_NO) {
        for (uint32_t i = 0; i < conf.len && result == EMBI_JS_YES; i++) {
            if (!embi_hashmap_remove(seen, conf.data[i])) {
                result = throw_thr(rt, "ownKeys trap result omits a key of a non-extensible target");
            }
        }
        if (result == EMBI_JS_YES && embi_hashmap_len(seen) != 0) {
            result = throw_thr(rt, "ownKeys trap result adds keys to a non-extensible target");
        }
    }
    embi_vec_free(&tk);
    embi_vec_free(&conf);
    return result;
}

static int proxy_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, o, "ownKeys",
                                  embi_js_key_c(rt, "ownKeys"), &target, &handler);
    if (trap == EXC) {
        return EMBI_JS_THREW;
    }
    embi_js_object *t = embi_js_as_object(target);
    if (embi_js_is_undefined(trap)) {
        return embi_js_obj_own_keys(rt, t, out);
    }
    embi_js_value res = embi_js_call(rt, trap, handler, 1, &target);
    if (res == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_object(res)) {
        return throw_thr(rt, "ownKeys trap result is not an object");
    }
    embi_js_object *list = embi_js_as_object(res);
    double n = 0.0;
    if (!embi_js_length_of_array_like(rt, list, &n)) {
        return EMBI_JS_THREW;
    }
    embi_js_key_vec keep = {nullptr, 0, 0};
    embi_hashmap seen;
    embi_hashmap_init(&seen);
    int r = trap_key_list(rt, o, list, n, &keep, &seen, res);
    if (r == EMBI_JS_YES) {
        r = own_keys_check(rt, t, &seen);
    }
    if (r == EMBI_JS_YES) {
        for (uint32_t i = 0; i < keep.len; i++) {
            embi_vec_push(out, keep.data[i]);
        }
    }
    embi_vec_free(&keep);
    embi_hashmap_free(&seen);
    return r;
}

/* ------------------------------------------------------------------ [[Call]] and [[Construct]] */

static embi_js_value proxy_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                int argc, const embi_js_value *argv)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, f, "apply",
                                  embi_js_key_c(rt, "apply"), &target, &handler);
    if (trap == EXC) {
        return EXC;
    }
    if (embi_js_is_undefined(trap)) {
        return embi_js_call(rt, target, this_v, argc, argv);
    }
    embi_js_object *arr = array_from_list(rt, argc, argv);
    if (!arr) {
        return EXC;
    }
    embi_js_value av[3] = {target, this_v, embi_js_from_object(arr)};
    return embi_js_call(rt, trap, handler, 3, av);
}

static embi_js_value proxy_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                     const embi_js_value *argv, embi_js_value new_target)
{
    embi_js_value target;
    embi_js_value handler;
    embi_js_value trap = get_trap(rt, f, "construct",
                                  embi_js_key_c(rt, "construct"), &target, &handler);
    if (trap == EXC) {
        return EXC;
    }
    if (embi_js_is_undefined(trap)) {
        return embi_js_construct(rt, target, argc, argv, new_target);
    }
    embi_js_object *arr = array_from_list(rt, argc, argv);
    if (!arr) {
        return EXC;
    }
    embi_js_value av[3] = {target, embi_js_from_object(arr), new_target};
    embi_js_value res = embi_js_call(rt, trap, handler, 3, av);
    if (res == EXC) {
        return EXC;
    }
    if (!embi_js_is_object(res)) {
        return throw_exc(rt, "construct trap returned a non-object");
    }
    return res;
}

/* ------------------------------------------------------------------ classes */

const embi_js_class embi_js_class_proxy = {
    .name = "Proxy",
    .get_own = proxy_get_own,
    .define_own = proxy_define_own,
    .has = proxy_has,
    .delete = proxy_delete,
    .own_keys = proxy_own_keys,
    .get = proxy_get,
    .set = proxy_set,
    .trace = proxy_trace,
    .get_proto = proxy_get_proto,
    .set_proto = proxy_set_proto,
    .is_extensible = proxy_is_extensible,
    .prevent_extensions = proxy_prevent_extensions,
};

const embi_js_class embi_js_class_proxy_callable = {
    .name = "Proxy",
    .get_own = proxy_get_own,
    .define_own = proxy_define_own,
    .has = proxy_has,
    .delete = proxy_delete,
    .own_keys = proxy_own_keys,
    .get = proxy_get,
    .set = proxy_set,
    .call = proxy_call,
    .construct = proxy_construct,
    .trace = proxy_trace,
    .get_proto = proxy_get_proto,
    .set_proto = proxy_set_proto,
    .is_extensible = proxy_is_extensible,
    .prevent_extensions = proxy_prevent_extensions,
};

/* ------------------------------------------------------------------ creation */

/* ProxyCreate(target, handler). Returns nullptr when it throws. */
static embi_js_object *proxy_create(embi_js_runtime *rt, embi_js_value target, embi_js_value handler)
{
    if (!embi_js_is_object(target) || !embi_js_is_object(handler)) {
        (void)embi_js_throw_type_error(rt, "Cannot create proxy with a non-object as target or handler");
        return nullptr;
    }
    const embi_js_class *k = embi_js_is_callable(target) ? &embi_js_class_proxy_callable
                                                         : &embi_js_class_proxy;
    embi_js_object *p = embi_js_object_new_class(rt, EMBI_JS_NULL, k);
    if (embi_js_is_constructor(target)) {
        p->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    }
    embi_js_varray *d = embi_js_varray_new(rt, PROXY_SLOTS);
    EMBI_JS_STORE(d, d->elems[SLOT_TARGET], target);
    EMBI_JS_STORE(d, d->elems[SLOT_HANDLER], handler);
    EMBI_JS_STORE(d, d->elems[SLOT_SCRATCH], UNDEF);
    EMBI_JS_STORE(d, d->elems[SLOT_TCSHAPE], UNDEF);
    for (uint32_t i = 0; i < TRAP_COUNT; i++) {
        EMBI_JS_STORE(d, d->elems[SLOT_TRAP(i)], embi_js_from_int32(-1));
    }
    EMBI_JS_WB(d, target);
    EMBI_JS_WB(d, handler);
    p->internal = d;
    EMBI_JS_WB_STRUCT(p);
    return p;
}

/* The revoke function of Proxy.revocable. Its internal is the proxy (nullptr once revoked). */
static embi_js_value revoke_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)rt;
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_js_object *p = (embi_js_object *)f->internal;
    if (p) {
        embi_js_varray *d = proxy_slots(p);
        /* The scratch pins stay: the caller of an earlier ownKeys may still hold those symbols. */
        EMBI_JS_STORE(d, d->elems[SLOT_TARGET], EMBI_JS_NULL);
        EMBI_JS_STORE(d, d->elems[SLOT_HANDLER], EMBI_JS_NULL);
        EMBI_JS_STORE(d, d->elems[SLOT_TCSHAPE], UNDEF);
        f->internal = nullptr;
    }
    return UNDEF;
}

static void revoke_trace(embi_js_object *f, embi_js_tracer *t)
{
    if (f->internal) {
        embi_js_mark_cell(t, (embi_js_cell *)f->internal);
    }
}

static const embi_js_class revoke_class = {
    .name = "Function",
    .call = revoke_call,
    .trace = revoke_trace,
};

static embi_js_value proxy_revocable(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *p = proxy_create(rt, argc > 0 ? argv[0] : UNDEF, argc > 1 ? argv[1] : UNDEF);
    if (!p) {
        return EXC;
    }
    embi_js_object *rv = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], &revoke_class);
    embi_js_define_raw(rt, rv, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(0.0), ATTR_C);
    embi_js_define_raw(rt, rv, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "", 0)), ATTR_C);
    rv->internal = p;
    EMBI_JS_WB_STRUCT(rv);
    embi_js_object *res = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, res, embi_js_key_c(rt, "proxy"), embi_js_from_object(p),
                       EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, res, embi_js_key_c(rt, "revoke"), embi_js_from_object(rv),
                       EMBI_JS_ATTR_DEFAULT);
    return embi_js_from_object(res);
}

/* The Proxy constructor: a call throws, new creates the proxy. */
static embi_js_value proxy_ctor_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                          const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    (void)new_target;
    embi_js_object *p = proxy_create(rt, argc > 0 ? argv[0] : UNDEF, argc > 1 ? argv[1] : UNDEF);
    return p ? embi_js_from_object(p) : EXC;
}

static embi_js_value proxy_ctor_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                     int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    (void)argc;
    (void)argv;
    return embi_js_throw_type_error(rt, "Constructor Proxy requires 'new'");
}

static const embi_js_class proxy_ctor_class = {
    .name = "Function",
    .call = proxy_ctor_call,
    .construct = proxy_ctor_construct,
};

/* ------------------------------------------------------------------ Reflect */

static embi_js_value arg_v(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

/* Target argument of a Reflect function: an object, or a TypeError. */
static embi_js_object *reflect_target(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        (void)embi_js_throw_type_error(rt, "Reflect method called on non-object");
        return nullptr;
    }
    return embi_js_as_object(v);
}

static embi_js_value reflect_apply(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value f = arg_v(argc, argv, 0);
    if (!embi_js_is_callable(f)) {
        return embi_js_throw_type_error(rt, "Reflect.apply target is not callable");
    }
    uint32_t len = 0;
    embi_js_varray *list = embi_js_list_from_array_like(rt, arg_v(argc, argv, 2), &len);
    if (!list) {
        return EXC;
    }
    /* argv points into the list cell: keep the cell itself alive (EMBI_KEEP_ALIVE). */
    embi_js_value res = embi_js_call(rt, f, arg_v(argc, argv, 1), (int)len, list->elems);
    EMBI_KEEP_ALIVE(list);
    return res;
}

static embi_js_value reflect_construct(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value f = arg_v(argc, argv, 0);
    if (!embi_js_is_constructor(f)) {
        return embi_js_throw_type_error(rt, "Reflect.construct target is not a constructor");
    }
    embi_js_value nt = argc > 2 ? argv[2] : f;
    if (!embi_js_is_constructor(nt)) {
        return embi_js_throw_type_error(rt, "Reflect.construct newTarget is not a constructor");
    }
    uint32_t len = 0;
    embi_js_varray *list = embi_js_list_from_array_like(rt, arg_v(argc, argv, 1), &len);
    if (!list) {
        return EXC;
    }
    /* argv points into the list cell: keep the cell itself alive (EMBI_KEEP_ALIVE). */
    embi_js_value res = embi_js_construct(rt, f, (int)len, list->elems, nt);
    EMBI_KEEP_ALIVE(list);
    return res;
}

static embi_js_value reflect_define_property(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    embi_js_pdesc d;
    if (!embi_js_to_property_descriptor(rt, arg_v(argc, argv, 2), &d)) {
        return EXC;
    }
    int st = embi_js_obj_define_own_property(rt, o, k, &d);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_delete_property(embi_js_realm *r, embi_js_value this_v, int argc,
                                             const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    int st = embi_js_obj_delete(rt, o, k);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_get(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    embi_js_value receiver = argc > 2 ? argv[2] : embi_js_from_object(o);
    return embi_js_obj_get(rt, o, k, receiver);
}

static embi_js_value reflect_get_own_property_descriptor(embi_js_realm *r, embi_js_value this_v,
                                                         int argc, const embi_js_value *argv,
                                                         int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    embi_js_pdesc d;
    int st = embi_js_obj_get_own_property(rt, o, k, &d);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    if (st == EMBI_JS_NO) {
        return UNDEF;
    }
    return embi_js_from_property_descriptor(rt, &d);
}

static embi_js_value reflect_get_prototype_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_value p;
    if (embi_js_obj_get_prototype_of(rt, o, &p) == EMBI_JS_THREW) {
        return EXC;
    }
    return p;
}

static embi_js_value reflect_has(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    int st = embi_js_obj_has_property(rt, o, k);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_is_extensible(embi_js_realm *r, embi_js_value this_v, int argc,
                                           const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    int st = embi_js_obj_extensible(rt, o);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_own_keys(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key_vec kv = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, o, &kv) == EMBI_JS_THREW) {
        embi_vec_free(&kv);
        return EXC;
    }
    /* Root the keys (a trap may have produced symbols that nothing else holds) before any
     * allocation below. */
    uint32_t n = kv.len;
    embi_js_varray *keys = embi_js_varray_new(rt, n);
    for (uint32_t i = 0; i < n; i++) {
        EMBI_JS_STORE(keys, keys->elems[i], kv.data[i]);
        EMBI_JS_WB(keys, kv.data[i]);
    }
    embi_vec_free(&kv);
    embi_js_object *arr = embi_js_array_create_default(rt, 0.0);
    if (!arr) {
        return EXC;
    }
    for (uint32_t i = 0; i < n; i++) {
        embi_js_value kvv = embi_js_key_to_value(rt, keys->elems[i]);
        if (embi_js_create_data_property_or_throw(rt, arr, embi_js_key_from_index(i), kvv) ==
            EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(arr);
}

static embi_js_value reflect_prevent_extensions(embi_js_realm *r, embi_js_value this_v, int argc,
                                                const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    int st = embi_js_obj_prevent_extensions(rt, o);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_set(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_key k = embi_js_to_property_key(rt, arg_v(argc, argv, 1));
    if (k == EXC) {
        return EXC;
    }
    embi_js_value receiver = argc > 3 ? argv[3] : embi_js_from_object(o);
    int st = embi_js_obj_set(rt, o, k, arg_v(argc, argv, 2), receiver);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

static embi_js_value reflect_set_prototype_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                              const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = reflect_target(rt, arg_v(argc, argv, 0));
    if (!o) {
        return EXC;
    }
    embi_js_value proto = arg_v(argc, argv, 1);
    if (!embi_js_is_object(proto) && !embi_js_is_null(proto)) {
        return embi_js_throw_type_error(rt, "Object prototype may only be an Object or null");
    }
    int st = embi_js_obj_set_prototype_of(rt, o, proto);
    if (st == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

/* ------------------------------------------------------------------ installation */

typedef struct reflect_def {
    const char *name;
    embi_js_native fn;
    int length;
} reflect_def;

static const reflect_def reflect_methods[] = {
    {"apply", reflect_apply, 3},
    {"construct", reflect_construct, 2},
    {"defineProperty", reflect_define_property, 3},
    {"deleteProperty", reflect_delete_property, 2},
    {"get", reflect_get, 2},
    {"getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2},
    {"getPrototypeOf", reflect_get_prototype_of, 1},
    {"has", reflect_has, 2},
    {"isExtensible", reflect_is_extensible, 1},
    {"ownKeys", reflect_own_keys, 1},
    {"preventExtensions", reflect_prevent_extensions, 1},
    {"set", reflect_set, 3},
    {"setPrototypeOf", reflect_set_prototype_of, 2},
};

void embi_js_init_builtins_proxy(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;

    embi_js_object *ctor = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION],
                                                    &proxy_ctor_class);
    ctor->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(2.0),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "Proxy", 5)), ATTR_C);
    embi_js_define_method(r, ctor, "revocable", proxy_revocable, 2, 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Proxy"), embi_js_from_object(ctor), ATTR_WC);

    embi_js_object *reflect = embi_js_object_new_plain(rt);
    for (size_t i = 0; i < sizeof reflect_methods / sizeof reflect_methods[0]; i++) {
        embi_js_define_method(r, reflect, reflect_methods[i].name, reflect_methods[i].fn,
                              reflect_methods[i].length, 0);
    }
    embi_js_define_raw(rt, reflect, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "Reflect", 7)), ATTR_C);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Reflect"), embi_js_from_object(reflect),
                       ATTR_WC);
}
