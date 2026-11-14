#pragma once
/*
 * Abstract operations of ECMA-262 (clauses 7 and 6.2.6): type conversion, equality and
 * comparison, property access helpers, calls, descriptors and integrity levels.
 *
 * Errors. A function that can throw returns EMBI_JS_EXCEPTION (value results), nullptr (pointer
 * results), false (bool results) or EMBI_JS_THREW (int tristates) with the exception pending on the
 * runtime. Throw helpers create an Error object of the given prototype, set its message, and
 * return EMBI_JS_EXCEPTION.
 */
#include "js/bigint.h"
#include "js/object.h"
#include "js/runtime.h"
#include "js/string.h"
#include "js/value.h"

#include <stdbool.h>

typedef enum embi_js_hint {
    EMBI_JS_HINT_DEFAULT,
    EMBI_JS_HINT_NUMBER,
    EMBI_JS_HINT_STRING
} embi_js_hint;

/* AbstractRelationalComparison result. */
#define EMBI_JS_CMP_FALSE 0
#define EMBI_JS_CMP_TRUE 1
#define EMBI_JS_CMP_UNDEFINED 2 /* NaN involved */
#define EMBI_JS_CMP_THREW (-1)

/* ------------------------------------------------------------------ errors */

/* Creates an error of the given intrinsic prototype (falls back to a null prototype when the
 * realm did not set it), sets its message, makes it the pending exception and returns
 * EMBI_JS_EXCEPTION. fmt is printf-style. */
embi_js_value embi_js_throw_error(embi_js_runtime *rt, enum embi_js_proto which, const char *fmt,
                                  ...) __attribute__((format(printf, 3, 4)));
#define embi_js_throw_type_error(rt, ...)                                                      \
    embi_js_throw_error((rt), EMBI_JS_PROTO_TYPE_ERROR, __VA_ARGS__)
#define embi_js_throw_range_error(rt, ...)                                                     \
    embi_js_throw_error((rt), EMBI_JS_PROTO_RANGE_ERROR, __VA_ARGS__)
#define embi_js_throw_reference_error(rt, ...)                                                 \
    embi_js_throw_error((rt), EMBI_JS_PROTO_REFERENCE_ERROR, __VA_ARGS__)
#define embi_js_throw_syntax_error(rt, ...)                                                    \
    embi_js_throw_error((rt), EMBI_JS_PROTO_SYNTAX_ERROR, __VA_ARGS__)

/* Makes v the pending exception and returns EMBI_JS_EXCEPTION. */
embi_js_value embi_js_throw_value(embi_js_runtime *rt, embi_js_value v);

/* ------------------------------------------------------------------ type conversion */

static inline bool embi_js_to_boolean(embi_js_value v)
{
    if (embi_js_is_bool(v)) {
        return v == EMBI_JS_TRUE;
    }
    if (embi_js_is_number(v)) {
        if (embi_js_is_int32(v)) {
            return embi_js_as_int32(v) != 0;
        }
        double d = embi_js_as_double(v);
        return d == d && d != 0.0;
    }
    if (embi_js_is_string(v)) {
        return embi_js_str_len(embi_js_as_string(v)) != 0;
    }
    if (embi_js_is_nullish(v)) {
        return false;
    }
    if (embi_js_is_bigint(v)) {
        return !embi_js_bigint_is_zero(embi_js_as_bigint(v));
    }
    return true; /* objects and symbols */
}

embi_js_value embi_js_to_primitive(embi_js_runtime *rt, embi_js_value v, embi_js_hint hint);
/* ToNumber. Returns false when it throws. */
bool embi_js_to_number(embi_js_runtime *rt, embi_js_value v, double *out);
/* ToNumeric: a number or a BigInt value, or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_to_numeric(embi_js_runtime *rt, embi_js_value v);
bool embi_js_to_integer_or_infinity(embi_js_runtime *rt, embi_js_value v, double *out);
bool embi_js_to_int32(embi_js_runtime *rt, embi_js_value v, int32_t *out);
bool embi_js_to_uint32(embi_js_runtime *rt, embi_js_value v, uint32_t *out);
bool embi_js_to_length(embi_js_runtime *rt, embi_js_value v, double *out);
/* ToString. Returns nullptr when it throws. */
embi_js_string *embi_js_to_string(embi_js_runtime *rt, embi_js_value v);
/* ToPropertyKey. Returns EMBI_JS_EXCEPTION when it throws. */
embi_js_key embi_js_to_property_key(embi_js_runtime *rt, embi_js_value v);
/* The value of a property key: a string for an index or atom key, or the symbol. */
embi_js_value embi_js_key_to_value(embi_js_runtime *rt, embi_js_key k);
/* ToObject. Returns nullptr when it throws. */
embi_js_object *embi_js_to_object(embi_js_runtime *rt, embi_js_value v);
/* typeof, as an interned string. */
embi_js_value embi_js_typeof(embi_js_runtime *rt, embi_js_value v);

/* ------------------------------------------------------------------ equality and comparison */

bool embi_js_same_value(embi_js_value a, embi_js_value b);
bool embi_js_same_value_zero(embi_js_value a, embi_js_value b);
bool embi_js_strict_equal(embi_js_value a, embi_js_value b);
/* IsLooselyEqual: YES, NO or THREW. */
int embi_js_loose_equal(embi_js_runtime *rt, embi_js_value a, embi_js_value b);
/* IsLessThan(x, y, leftFirst) as EMBI_JS_CMP_*. */
int embi_js_less_than(embi_js_runtime *rt, embi_js_value x, embi_js_value y, bool left_first);

/* ------------------------------------------------------------------ property access */

/* GetV: property of any value (primitives use their wrapper prototype). */
embi_js_value embi_js_get_v(embi_js_runtime *rt, embi_js_value v, embi_js_key k);
/* Get(O, P). */
static inline embi_js_value embi_js_get(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    return embi_js_obj_get(rt, o, k, embi_js_from_object(o));
}
/* Set(O, P, V, Throw): YES, NO (only when throw_on_fail is false), or THREW. */
int embi_js_put(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v,
                bool throw_on_fail);
/* GetMethod(V, P): undefined, a callable, or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_get_method(embi_js_runtime *rt, embi_js_value v, embi_js_key k);
/* CreateDataProperty: YES, NO or THREW. */
int embi_js_create_data_property(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_value v);
/* CreateDataPropertyOrThrow: YES or THREW. */
int embi_js_create_data_property_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                          embi_js_value v);
/* DefinePropertyOrThrow: YES or THREW. */
int embi_js_define_property_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                     const embi_js_pdesc *d);
/* DeletePropertyOrThrow: YES or THREW. */
int embi_js_delete_or_throw(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* HasOwnProperty via [[GetOwnProperty]] wrapper of the abstract op: YES, NO or THREW. */
int embi_js_has_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k);
/* LengthOfArrayLike. */
bool embi_js_length_of_array_like(embi_js_runtime *rt, embi_js_object *o, double *out);
/* CreateListFromArrayLike into a fresh value array (kept alive by the returned pointer). The
 * length is written to *len. Throws TypeError for non-objects and RangeError above 2^24 items. */
embi_js_varray *embi_js_list_from_array_like(embi_js_runtime *rt, embi_js_value v, uint32_t *len);
/* The intrinsic JSON parse of a text without a reviver (builtins_json.c), used for JSON modules. */
embi_js_value embi_js_json_parse_text(embi_js_runtime *rt, const embi_js_string *src);
/* EnumerableOwnProperties with the key kind: appends the enumerable string keys of o. */
int embi_js_enumerable_own_keys(embi_js_runtime *rt, embi_js_object *o, embi_js_key_vec *out);
/* SetIntegrityLevel / TestIntegrityLevel: YES/NO/THREW, frozen selects "frozen" over "sealed". */
int embi_js_set_integrity(embi_js_runtime *rt, embi_js_object *o, bool frozen);
int embi_js_test_integrity(embi_js_runtime *rt, embi_js_object *o, bool frozen);

/* ToPropertyDescriptor and FromPropertyDescriptor. */
bool embi_js_to_property_descriptor(embi_js_runtime *rt, embi_js_value v, embi_js_pdesc *out);
embi_js_value embi_js_from_property_descriptor(embi_js_runtime *rt, const embi_js_pdesc *d);

/* ------------------------------------------------------------------ calls and construction */

/* Call(F, V, args). Throws TypeError when f is not callable. */
embi_js_value embi_js_call(embi_js_runtime *rt, embi_js_value f, embi_js_value this_v, int argc,
                           const embi_js_value *argv);
/* Annex-B style caller/arguments for sloppy functions: a getter that is the realm's
 * %ThrowTypeError% read through the key "caller" or "arguments" on a sloppy ordinary function
 * answers like an own data property (the calling sloppy function or null, and null) instead of
 * throwing. Returns false when the getter, receiver or key is not that case. */
bool embi_js_restricted_get(embi_js_runtime *rt, embi_js_value getter, embi_js_value receiver,
                            embi_js_key key, embi_js_value *out);
/* Construct(F, args, newTarget). Throws TypeError when f is not a constructor. */
embi_js_value embi_js_construct(embi_js_runtime *rt, embi_js_value f, int argc,
                                const embi_js_value *argv, embi_js_value new_target);
/* Invoke(V, P, args). */
embi_js_value embi_js_invoke(embi_js_runtime *rt, embi_js_value v, embi_js_key k, int argc,
                             const embi_js_value *argv);
/* InstanceofOperator(V, target): YES, NO or THREW. */
int embi_js_instance_of(embi_js_runtime *rt, embi_js_value v, embi_js_value target);
/* OrdinaryHasInstance(C, O): YES, NO or THREW. */
int embi_js_ordinary_has_instance(embi_js_runtime *rt, embi_js_value c, embi_js_value v);
/* GetPrototypeFromConstructor: the object prototype of new_target, else the intrinsic `which`.
 * Returns the prototype value or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_proto_from_ctor(embi_js_runtime *rt, embi_js_value new_target,
                                      enum embi_js_proto which);
/* IsArray: looks through proxies. embi_js_is_array_checked throws for a revoked proxy (YES, NO or
 * THREW); embi_js_is_array never throws and reads a revoked proxy as not an array. */
int embi_js_is_array_checked(embi_js_runtime *rt, embi_js_value v);
bool embi_js_is_array(embi_js_value v);

/* ------------------------------------------------------------------ symbols (J2e) */

/* Cell flag on a symbol created by Symbol.for (builtins_symbol.c). A registered symbol is never
 * a valid WeakMap key (ES2023 CanBeHeldWeakly), and Symbol.keyFor returns its description. */
#define EMBI_JS_FLAG_SYM_REGISTERED 0x0002u
/* Cell flag on a private name (J2d): a symbol that keys a class private element. It is never an
 * own property key: [[OwnPropertyKeys]] leaves it out and private access skips the class hooks. */
#define EMBI_JS_FLAG_SYM_PRIVATE 0x0004u
bool embi_js_symbol_is_registered(embi_js_value v);
/* HasBinding of an object environment record (the with statement): HasProperty, then the
 * @@unscopables check of ECMA-262 9.1.1.2.1. Returns YES, NO or THREW. */
int embi_js_with_has_binding(embi_js_runtime *rt, embi_js_object *obj, embi_js_key k);
/* SpeciesConstructor(O, default). Returns the constructor or EMBI_JS_EXCEPTION. */
embi_js_value embi_js_species_constructor(embi_js_runtime *rt, embi_js_object *o,
                                          embi_js_value default_ctor);
/* ArraySpeciesCreate(original, length). Returns the new object or nullptr when it throws. */
embi_js_object *embi_js_array_species_create(embi_js_runtime *rt, embi_js_object *original,
                                             double len);
