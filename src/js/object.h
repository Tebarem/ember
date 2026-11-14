#pragma once
/*
 * Objects: layout, property descriptors, the class table (exotic behavior as hooks) and the
 * internal methods of ECMA-262 section 10.1, plus object creation helpers.
 *
 * Storage. A named property lives in a shape slot (shape mode) or in the dictionary `dict`
 * (dictionary mode). An index property lives in the dense `elements` array (holes are EMPTY) or,
 * once sparse, in the `sparse` dictionary. Dictionary mode starts past 64 named properties, after a
 * deletion that is not the last property, or after an attribute change on a named property. A
 * prototype change keeps shape mode: the chain is rebuilt from the new prototype's root.
 * Elements turn sparse on a write past index 2 * elements_len + 1024, or on any element with
 * non-default attributes. The extensible flag lives in the object header (not the shape).
 *
 * Internal methods return int tristates: EMBI_JS_NO, EMBI_JS_YES, or EMBI_JS_THREW when a JS
 * exception is pending (rt->pending_exception). Value results return EMBI_JS_EXCEPTION on throw.
 */
#include "js/runtime.h"
#include "js/shape.h"
#include "js/value.h"

#include <stdint.h>

#define EMBI_JS_NO 0
#define EMBI_JS_YES 1
#define EMBI_JS_THREW (-1)

#define EMBI_JS_OBJ_EXTENSIBLE 0x0100u  /* in hdr.flags; cleared by preventExtensions */
#define EMBI_JS_OBJ_CONSTRUCTOR 0x0200u /* has [[Construct]] */
#define EMBI_JS_OBJ_REGEXP_LEGACY 0x0400u /* RegExp object with the Annex B legacy features (J4c): created with %RegExp% as new.target, or a literal; RegExp.prototype.compile needs it */
#define EMBI_JS_OBJ_IS_PROTO 0x0800u    /* J5c: this object is some object's prototype; dictionary mutations renew its shape (object.c) */
#define EMBI_JS_OBJ_PACKED 0x1000u      /* J5e: an array with no hole below elements_len (object.c) */
#define EMBI_JS_OBJ_WATCHED 0x4000u     /* J5e: an intrinsic whose stores can clear a protector (object.c) */
#define EMBI_JS_OBJ_LAZY_PROTO 0x2000u  /* J5n2: a function whose `prototype` object is created on first use (see below) */
#define EMBI_JS_OBJ_INLINE_SLOTS 4u

/* Protector bits (rt->protect). All set at creation and only cleared.
 * NO_PROTO_ELEMENTS: no index property on any prototype object, so a hole read on an array whose
 * prototype is the intrinsic Array.prototype is undefined. ARRAY_SPECIES: Array.prototype.constructor
 * and %Array%[@@species] are intact. ARRAY_ITERATOR: Array.prototype[@@iterator] and
 * %ArrayIteratorPrototype%.next are intact. */
#define EMBI_JS_PROTECT_NO_PROTO_ELEMENTS 0x1u
#define EMBI_JS_PROTECT_ARRAY_SPECIES 0x2u
#define EMBI_JS_PROTECT_ARRAY_ITERATOR 0x4u

/* Property descriptor fields that are present (has). */
#define EMBI_JS_PD_VALUE 0x01u
#define EMBI_JS_PD_WRITABLE 0x02u
#define EMBI_JS_PD_GET 0x04u
#define EMBI_JS_PD_SET 0x08u
#define EMBI_JS_PD_ENUMERABLE 0x10u
#define EMBI_JS_PD_CONFIGURABLE 0x20u

/* A property descriptor. value/get/set are valid only when their has bit is set. attrs holds the
 * EMBI_JS_ATTR_WRITABLE / ENUMERABLE / CONFIGURABLE bits for the fields that have has bits. */
typedef struct embi_js_pdesc {
    embi_js_value value;
    embi_js_value get;
    embi_js_value set;
    uint8_t has;
    uint8_t attrs;
} embi_js_pdesc;

/* A stored own property: its value (an accessor cell when ACCESSOR is set) and attributes. */
typedef struct embi_js_prop {
    embi_js_value value;
    uint8_t attrs;
} embi_js_prop;

typedef struct embi_js_object embi_js_object;

/* Lazy `prototype` (J5n2). A new closure of an ordinary function (CONSTRUCTOR code that is not a
 * generator) gets its three properties with the third, `prototype`, flagged EMBI_JS_ATTR_LAZY in the
 * shape and holding EMPTY in its slot (slot EMBI_JS_LAZY_PROTO_SLOT), and the header flag above. Most
 * such functions are never used with `new` and never have their prototype read, and the object that
 * the old code made for each one was about a third of the function's memory. Every access that can see
 * the value (the generic property reads and writes, the conversion to dictionary mode) first calls
 * embi_js_obj_lazy_proto_realize, which makes the object, stores it, moves the object to the shape
 * without the flag and clears the header flag. The inline caches never cache the lazy property: its
 * shape carries the flag, so a cache filled from a realized function (a different shape) is never hit by
 * a lazy one. The flag, the shape bit and the EMPTY slot always change together. */
#define EMBI_JS_LAZY_PROTO_SLOT 2u
void embi_js_obj_lazy_proto_realize(embi_js_runtime *rt, embi_js_object *o);
typedef struct embi_js_dict embi_js_dict;

/* Rarely used state, allocated on first need (J5j): a dictionary-mode object, an object with sparse
 * elements, or a prototype with a cached root shape for its children. Ordinary objects have none. */
typedef struct embi_js_rare {
    embi_js_dict *dict;             /* named properties in dictionary mode */
    embi_js_dict *sparse;           /* index -> (value, attrs) once the elements are sparse */
    embi_js_shape *child_root;      /* cached root shape of objects whose proto is this one */
} embi_js_rare;

/* 96 bytes with 4 inline slots (J5j). The dense elements block carries its capacity in the 8 bytes
 * in front of the first element (elements[-1], only when elements is non-null); elements_len stays
 * here, beside slot_cap, so an access never tests for a null block. */
struct embi_js_object {
    embi_js_cell hdr;                  /* flags: EXTENSIBLE, CONSTRUCTOR, PACKED, ... */
    embi_js_shape *shape;              /* the prototype is shape->proto */
    embi_js_value *slots;              /* out-of-line named slots: slot i >= INLINE at slots[i-4] */
    uint32_t slot_cap;
    uint32_t elements_len;             /* dense storage in use (0 once sparse) */
    embi_js_value *elements;           /* dense index storage, EMPTY = hole; capacity at [-1] */
    const struct embi_js_class *klass;
    void *internal;                    /* class data: wrapped primitive, host pointer, ... */
    embi_js_rare *rare;                /* nullptr until a dictionary, sparse or child root exists */
    embi_js_value inline_slots[EMBI_JS_OBJ_INLINE_SLOTS];
};

/* ------------------------------------------------------------------ class table */

typedef int (*embi_js_get_own_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                  embi_js_pdesc *out);
typedef int (*embi_js_define_own_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                     const embi_js_pdesc *d);
typedef int (*embi_js_has_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
typedef int (*embi_js_delete_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
typedef int (*embi_js_own_keys_fn)(embi_js_runtime *rt, embi_js_object *o,
                                   embi_js_key_vec *out);
typedef embi_js_value (*embi_js_get_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                        embi_js_value receiver);
typedef int (*embi_js_set_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                              embi_js_value v, embi_js_value receiver);
typedef embi_js_value (*embi_js_call_fn)(embi_js_runtime *rt, embi_js_object *f,
                                         embi_js_value this_v, int argc,
                                         const embi_js_value *argv);
typedef embi_js_value (*embi_js_construct_fn)(embi_js_runtime *rt, embi_js_object *f, int argc,
                                              const embi_js_value *argv,
                                              embi_js_value new_target);
typedef void (*embi_js_class_trace_fn)(embi_js_object *o, embi_js_tracer *t);
typedef void (*embi_js_class_finalize_fn)(embi_js_object *o);
/* [[GetPrototypeOf]] (returns YES or THREW), [[SetPrototypeOf]] (YES, NO or THREW),
 * [[IsExtensible]] and [[PreventExtensions]] (YES, NO or THREW). */
typedef int (*embi_js_get_proto_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_value *out);
typedef int (*embi_js_set_proto_fn)(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto);
typedef int (*embi_js_extensible_fn)(embi_js_runtime *rt, embi_js_object *o);

/* A null hook means ordinary behavior, which is the fast path. get_own, define_own, own_keys,
 * has, delete, get and set replace the corresponding internal method. call and construct are the
 * [[Call]] and [[Construct]] of the objects of this class (an object is callable when call is
 * set, and a constructor when EMBI_JS_OBJ_CONSTRUCTOR is set). trace and finalize handle the
 * class's internal data. */
/* Class flag: named-key property access may be cached by inline caches (ic.c). Set only on classes
 * with no named-key get_own, get or has hook. The IC also checks the hooks at each use. */
#define EMBI_JS_CLASS_IC 0x01u

typedef struct embi_js_class {
    const char *name;
    uint32_t flags;          /* EMBI_JS_CLASS_* */
    embi_js_get_own_fn get_own;
    embi_js_define_own_fn define_own;
    embi_js_has_fn has;
    embi_js_delete_fn delete;
    embi_js_own_keys_fn own_keys;
    embi_js_get_fn get;
    embi_js_set_fn set;
    embi_js_call_fn call;
    embi_js_construct_fn construct;
    embi_js_class_trace_fn trace;
    embi_js_class_finalize_fn finalize;
    embi_js_get_proto_fn get_proto;         /* null: the shape's prototype */
    embi_js_set_proto_fn set_proto;         /* null: ordinary OrdinarySetPrototypeOf */
    embi_js_extensible_fn is_extensible;    /* null: the object header flag */
    embi_js_extensible_fn prevent_extensions; /* null: clears the header flag */
} embi_js_class;

extern const embi_js_class embi_js_class_object;   /* ordinary objects */
extern const embi_js_class embi_js_class_array;    /* exotic: length and index define rules */
extern const embi_js_class embi_js_class_string;   /* String wrapper: index properties */
extern const embi_js_class embi_js_class_number;
extern const embi_js_class embi_js_class_boolean;
extern const embi_js_class embi_js_class_symbol;
extern const embi_js_class embi_js_class_error;
extern const embi_js_class embi_js_class_date;     /* Date objects (builtins_date.c, J4d) */
extern const embi_js_class embi_js_class_bound;    /* bound function exotic object */
/* Proxy exotic objects (builtins_proxy.c). The class is chosen at creation from the target:
 * embi_js_class_proxy_callable when the target is callable (call is set), else embi_js_class_proxy.
 * Their internal is a 3-slot varray: [target, handler, scratch], with target and handler both
 * EMBI_JS_NULL once revoked. scratch holds the symbol keys that a trap returned from ownKeys, so
 * they stay alive after the call (builtins_proxy.c). */
extern const embi_js_class embi_js_class_proxy;
extern const embi_js_class embi_js_class_proxy_callable;

static inline bool embi_js_obj_is_proxy(const embi_js_object *o)
{
    return o->klass == &embi_js_class_proxy || o->klass == &embi_js_class_proxy_callable;
}

/* Proxy target and handler (EMBI_JS_NULL for both once revoked). Returns false for non-proxies. */
bool embi_js_proxy_parts(const embi_js_object *o, embi_js_value *target, embi_js_value *handler);

/* ------------------------------------------------------------------ setup and predicates */

/* Registers the object and shape cell ops and the GC root hook. embi_js_runtime_new calls it. */
void embi_js_objects_init(embi_js_runtime *rt);

/* Interned property names and well-known symbols are created on first use by the accessors
 * below, so a fresh runtime's atom table stays empty. */
void embi_js_names_init(embi_js_runtime *rt);

static inline embi_js_value embi_js_name_val(embi_js_runtime *rt, enum embi_js_name n)
{
    if (embi_js_is_empty(rt->names[EMBI_JS_NAME_LENGTH])) {
        embi_js_names_init(rt);
    }
    return rt->names[n];
}

static inline embi_js_value embi_js_wk_sym(embi_js_runtime *rt, enum embi_js_wk s)
{
    if (embi_js_is_empty(rt->names[EMBI_JS_NAME_LENGTH])) {
        embi_js_names_init(rt);
    }
    return rt->wk[s];
}

static inline bool embi_js_is_object(embi_js_value v)
{
    return embi_js_is_cell(v) && embi_js_as_cell(v)->type == EMBI_JS_CELL_OBJECT;
}

static inline embi_js_object *embi_js_as_object(embi_js_value v)
{
    EMBI_ASSERT(embi_js_is_object(v));
    return (embi_js_object *)embi_js_as_cell(v);
}

static inline embi_js_value embi_js_from_object(const embi_js_object *o)
{
    return embi_js_from_cell(o);
}

static inline embi_js_value embi_js_obj_proto(const embi_js_object *o)
{
    return o->shape->proto;
}

static inline bool embi_js_obj_is_extensible(const embi_js_object *o)
{
    return (o->hdr.flags & EMBI_JS_OBJ_EXTENSIBLE) != 0;
}

static inline bool embi_js_obj_is_constructor(const embi_js_object *o)
{
    return (o->hdr.flags & EMBI_JS_OBJ_CONSTRUCTOR) != 0;
}

static inline bool embi_js_is_callable(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass->call != nullptr;
}

static inline bool embi_js_is_constructor(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_obj_is_constructor(embi_js_as_object(v));
}

/* ------------------------------------------------------------------ dense elements (J5e)
 *
 * Every element access outside object.c goes through these, so the storage layout can change in
 * one place. A dense store holds the own index properties of an
 * ordinary or Array object with default attributes; EMPTY marks a hole. Packed arrays have no hole
 * below elements_len (EMBI_JS_OBJ_PACKED). */

/* The dictionary of a dictionary-mode object, the sparse elements of a sparse one, or nullptr. */
static inline embi_js_dict *embi_js_obj_dict(const embi_js_object *o)
{
    return o->rare ? o->rare->dict : nullptr;
}

static inline embi_js_dict *embi_js_obj_sparse(const embi_js_object *o)
{
    return o->rare ? o->rare->sparse : nullptr;
}

static inline bool embi_js_obj_dense_plain(const embi_js_object *o)
{
    return (o->klass == &embi_js_class_array || o->klass == &embi_js_class_object) &&
           embi_js_obj_sparse(o) == nullptr;
}

static inline embi_js_value *embi_js_obj_elems(embi_js_object *o)
{
    return o->elements;
}

static inline uint32_t embi_js_obj_elems_len(const embi_js_object *o)
{
    return o->elements_len;
}

static inline bool embi_js_obj_is_packed(const embi_js_object *o)
{
    return (o->hdr.flags & EMBI_JS_OBJ_PACKED) != 0;
}

/* Fast [[Get]] of index i for a dense object: a present element, or undefined for a hole of an
 * Array whose prototype is the intrinsic Array.prototype while NO_PROTO_ELEMENTS holds. Returns
 * false when the generic path must run. */
bool embi_js_obj_index_get_fast(embi_js_runtime *rt, const embi_js_object *o, uint32_t i,
                                embi_js_value *out);
/* Fast [[Set]] of index i (the value is stored as the generic path would): an existing element, a
 * hole filled under the protector, or an append to a packed array. Returns false when the generic
 * path must run. */
bool embi_js_obj_index_set_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v);
/* CreateDataProperty of index length(o) on a packed array with length(o) == elements_len (the push
 * of an array literal or a spread). Returns false when the generic path must run. */
bool embi_js_array_push_fast(embi_js_runtime *rt, embi_js_object *a, embi_js_value v);
/* Array.prototype.pop on a packed array with length(a) == elements_len > 0: removes the last
 * element into *out and shrinks length. Returns false when the generic path must run. */
bool embi_js_array_pop_fast(embi_js_runtime *rt, embi_js_object *a, embi_js_value *out);
/* The own-or-inherited index i of o, decided without the generic path when it can be: returns 1
 * with *out = the element when present, 0 when absent for certain (a hole of an Array under
 * NO_PROTO_ELEMENTS with the intrinsic prototype), and -1 when the generic path must decide. The
 * builtins probe every element access through this, so each one re-reads the array after a
 * callback (J5e). */
int embi_js_obj_index_probe(embi_js_runtime *rt, const embi_js_object *o, uint32_t i,
                            embi_js_value *out);
/* CreateDataProperty(o, i, v) for a dense object: an element below elements_len (a hole needs an
 * extensible object), or the append at elements_len of a packed array (or of a plain object). Returns
 * false when the generic path must run. */
bool embi_js_obj_create_index_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t i, embi_js_value v);
/* The length of an Array object (its own length property, which [[Get]] would return); false for any other
 * object. */
bool embi_js_array_length_fast(embi_js_runtime *rt, embi_js_object *o, uint32_t *out);
/* Reserves element storage for n elements of a dense array (a literal's length), without changing
 * elements_len. */
void embi_js_array_reserve(embi_js_runtime *rt, embi_js_object *a, uint32_t n);

static inline void *embi_js_obj_internal(const embi_js_object *o)
{
    return o->internal;
}

static inline void embi_js_obj_set_internal(embi_js_object *o, void *p)
{
    o->internal = p;
}

/* ------------------------------------------------------------------ inline cache support (ic.c)
 *
 * Named-property storage as the caches see it. A shape-mode object reads slot i from the inline or
 * out-of-line array. A dictionary-mode object (shape EMBI_JS_SHAPE_DICT) keeps its entries in
 * `dict`, and embi_js_obj_ic_value resolves a position there. */

static inline embi_js_value *embi_js_obj_slot(embi_js_object *o, uint32_t i)
{
    return i < EMBI_JS_OBJ_INLINE_SLOTS ? &o->inline_slots[i] : &o->slots[i - EMBI_JS_OBJ_INLINE_SLOTS];
}

/* Own named property k of o, without the class hooks: slot receives the shape slot index or the
 * dictionary position, attrs the EMBI_JS_ATTR_* bits. Index keys are never answered (false). */
bool embi_js_obj_ic_own(const embi_js_object *o, embi_js_key k, uint32_t *slot, uint8_t *attrs);
/* The value cell at slot, in whichever mode o is in (an accessor cell for ACCESSOR properties). */
embi_js_value *embi_js_obj_ic_value(embi_js_object *o, uint32_t slot);
/* Adds the named property that shape `next` (the transition of o's shape, one more key with the
 * default attributes) adds, with value v. The caller has checked extensibility. */
void embi_js_obj_ic_add(embi_js_runtime *rt, embi_js_object *o, embi_js_shape *next, embi_js_value v);

/* ------------------------------------------------------------------ creation */

/* Ordinary object with the given prototype (an object or EMBI_JS_NULL). */
[[nodiscard]] embi_js_object *embi_js_object_new(embi_js_runtime *rt, embi_js_value proto);
[[nodiscard]] embi_js_object *embi_js_object_new_class(embi_js_runtime *rt, embi_js_value proto,
                                                       const embi_js_class *klass);
/* Ordinary object whose prototype is the intrinsic Object.prototype. */
[[nodiscard]] embi_js_object *embi_js_object_new_plain(embi_js_runtime *rt);
/* ArrayCreate(len, proto). Throws RangeError and returns nullptr when len > 2^32 - 1. */
[[nodiscard]] embi_js_object *embi_js_array_create(embi_js_runtime *rt, double len,
                                                   embi_js_value proto);
[[nodiscard]] embi_js_object *embi_js_array_create_default(embi_js_runtime *rt, double len);
/* Wrapper object for a Boolean, Number, String or Symbol primitive (ToObject). Never throws. */
[[nodiscard]] embi_js_object *embi_js_primitive_wrapper_new(embi_js_runtime *rt,
                                                            embi_js_value prim);
/* BoundFunctionCreate with the target's [[GetPrototypeOf]]. args may be nullptr when argc is 0.
 * Returns nullptr when that throws (a proxy target's trap). */
[[nodiscard]] embi_js_object *embi_js_bound_function_new(embi_js_runtime *rt,
                                                         embi_js_object *target,
                                                         embi_js_value bound_this, int argc,
                                                         const embi_js_value *args);
/* The target and bound-this of a bound function object. Returns false for other objects. */
bool embi_js_bound_target(const embi_js_object *o, embi_js_value *target);

/* Returns the wrapped primitive of a Boolean, Number, String or Symbol wrapper (else UNDEFINED). */
embi_js_value embi_js_wrapper_value(const embi_js_object *o);

/* ------------------------------------------------------------------ internal methods */

/* [[GetOwnProperty]]: fills a complete descriptor (value/get/set plus E and C, and W for data). */
int embi_js_obj_get_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_pdesc *out);
/* [[DefineOwnProperty]] (returns YES, NO, or THREW for hook-thrown errors). */
int embi_js_obj_define_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                    const embi_js_pdesc *d);
int embi_js_obj_has_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
int embi_js_obj_has_own_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* [[Get]](k, receiver). Returns the value or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_obj_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                              embi_js_value receiver);
/* [[Set]](k, v, receiver). */
int embi_js_obj_set(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                    embi_js_value receiver);
int embi_js_obj_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* [[OwnPropertyKeys]]: appends indices ascending, then strings, then symbols, each in creation
 * order. */
int embi_js_obj_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out);
/* [[SetPrototypeOf]]. Object.prototype is an immutable prototype exotic object. */
int embi_js_obj_set_prototype_of(embi_js_runtime *rt, embi_js_object *o, embi_js_value proto);
/* [[PreventExtensions]]: YES, NO (a proxy trap refused) or THREW. */
int embi_js_obj_prevent_extensions(embi_js_runtime *rt, embi_js_object *o);
/* [[GetPrototypeOf]]: YES with *out set, or THREW. */
int embi_js_obj_get_prototype_of(embi_js_runtime *rt, embi_js_object *o, embi_js_value *out);
/* [[IsExtensible]]: YES, NO or THREW. */
int embi_js_obj_extensible(embi_js_runtime *rt, embi_js_object *o);
/* IsCompatiblePropertyDescriptor for a present current property: cur is complete, d may be
 * partial. When the current property is absent the answer is the extensible flag, which the
 * caller applies itself. */
bool embi_js_pd_compatible(const embi_js_pdesc *d, const embi_js_pdesc *cur);

/* ------------------------------------------------------------------ raw helpers (setup code) */

/* Creates or overwrites an own data property without checks or hooks. Used to build intrinsics.
 * attrs uses EMBI_JS_ATTR_* values. */
void embi_js_define_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                        uint8_t attrs);
/* Switches o to dictionary mode now (J6a: an object that is about to get many properties at once). */
void embi_js_obj_make_dict(embi_js_runtime *rt, embi_js_object *o);
/* Creates or overwrites an own accessor property without checks or hooks. getter and setter are
 * functions or EMBI_JS_UNDEFINED. */
void embi_js_define_accessor_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_value getter, embi_js_value setter, uint8_t attrs);
/* The own private element of o keyed by the private name k (J2d), without the class hooks. */
bool embi_js_priv_get_own(embi_js_object *o, embi_js_key k, embi_js_pdesc *out);
/* Removes an own property without checks or hooks. */
void embi_js_remove_raw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* J5m2: appends an own property that o does not have, without the lookup of embi_js_define_raw. k
 * must be a named key absent from o; o must be in shape mode (no dictionary, no sparse elements).
 * attrs uses EMBI_JS_ATTR_* values (only W/E/C count). */
void embi_js_define_fresh(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                          uint8_t attrs);
/* J5n1: appends n fresh named properties at once. `to` is the shape reached from o's current shape by
 * adding those properties in order (so to->count == o->shape->count + n), vals[i] is the value of the
 * i-th. o must be in shape mode with no sparse elements, and not watched. */
void embi_js_obj_append_shape(embi_js_runtime *rt, embi_js_object *o, embi_js_shape *to,
                              const embi_js_value *vals, uint32_t n);
/* J5m2: true when o's own named properties are exactly its shape's keys: an ordinary or Array
 * object (no exotic hooks), with no dense elements, no dictionary and no sparse elements. */
bool embi_js_obj_shape_only(const embi_js_object *o);
/* J5m2: appends the enumerable string keys of o in own-key order (no symbols). Requires
 * embi_js_obj_shape_only(o). Same result as embi_js_enumerable_own_keys for such an object. */
void embi_js_obj_enum_string_keys(const embi_js_object *o, embi_js_key_vec *out);

/* A key for a C string (interned, or an index key for canonical numeric strings). */
embi_js_key embi_js_key_c(embi_js_runtime *rt, const char *s);

/* Property descriptor helpers. Accessor vs data is decided by the has bits. */
static inline bool embi_js_pd_is_accessor(const embi_js_pdesc *d)
{
    return (d->has & (EMBI_JS_PD_GET | EMBI_JS_PD_SET)) != 0;
}

static inline bool embi_js_pd_is_data(const embi_js_pdesc *d)
{
    return (d->has & (EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE)) != 0;
}

static inline bool embi_js_pd_is_generic(const embi_js_pdesc *d)
{
    return !embi_js_pd_is_accessor(d) && !embi_js_pd_is_data(d);
}
