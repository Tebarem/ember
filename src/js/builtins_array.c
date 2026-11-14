/*
 * Array: the Array constructor (Array, Array.isArray, Array.of, Array[@@species]), Array.prototype
 * (the ES5 methods plus find/findIndex/findLast/findLastIndex/fill/copyWithin/includes/flat/
 * flatMap/at) and Array.prototype[@@unscopables]. The iterator methods (keys, values, entries,
 * @@iterator) belong to J2.
 *
 * Generic over array-likes: the receiver goes through ToObject, lengths through
 * LengthOfArrayLike, and element access through HasProperty, Get, Set and DeletePropertyOrThrow
 * with index keys (indices above 2^32-2 become string keys, so lengths up to 2^53-1 work).
 *
 * Fast path. A dense element that is present is an own data property with default attributes
 * (an element with other attributes, or an accessor, turns the store sparse), so reading it
 * directly gives the same result as the ordinary [[Get]]. Holes always take the full path, so a
 * prototype's index property is still seen. The fast path is taken only for the ordinary and Array
 * classes, which have no get/has hooks.
 *
 * GC rule: a value that user code can run past (a callback, a comparator, a getter, toString) is
 * held in a local or in a GC-visible varray, never only in a C-heap vector. Join copies element
 * strings into a C vector of code units right after converting them, which needs no rooting.
 */
#include "js/typedarray.h"
#include "js/builtins.h"
#include "js/interp.h"
#include "js/js.h"
#include "js/promise.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* 2^53 - 1: the largest length or index an array-like can have (ES2025 LengthOfArrayLike). */
#define MAX_SAFE 9007199254740991.0
/* The largest index that is an index key (2^32 - 2); a number above it becomes a string key. */
#define MAX_INDEX_KEY ((double)EMBI_JS_MAX_ARRAY_INDEX)

typedef EMBI_VEC(uint16_t) u16_vec;

/* ------------------------------------------------------------------ helpers */

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

/* The property key of the integer index i (0 <= i <= 2^53 - 1). */
static embi_js_key key_of(embi_js_runtime *rt, double i)
{
    if (i <= MAX_INDEX_KEY) {
        return embi_js_key_from_index((uint32_t)i);
    }
    return embi_js_to_property_key(rt, embi_js_number(i));
}

/* Every element access below probes the dense store first (object.c embi_js_obj_index_probe and
 * friends). A probe is taken again on every call, so a callback that changes the array (a hole, a
 * new length, a freeze, a prototype change) is seen by the next element, as the generic path would
 * see it. A probe that cannot decide falls back to the generic operation with the same key. */

/* HasProperty(O, ToString(i)): YES, NO or THREW. */
static int has_at(embi_js_runtime *rt, embi_js_object *o, double i)
{
    if (i < 4294967295.0) {
        embi_js_value v;
        int p = embi_js_obj_index_probe(rt, o, (uint32_t)i, &v);
        if (p >= 0) {
            return p ? EMBI_JS_YES : EMBI_JS_NO;
        }
    }
    return embi_js_obj_has_property(rt, o, key_of(rt, i));
}

/* Get(O, ToString(i)). Returns the value or EXC. */
static embi_js_value get_at(embi_js_runtime *rt, embi_js_object *o, double i)
{
    embi_js_value v;
    if (i < 4294967295.0 && embi_js_obj_index_get_fast(rt, o, (uint32_t)i, &v)) {
        return v;
    }
    return embi_js_obj_get(rt, o, key_of(rt, i), embi_js_from_object(o));
}

/* Set(O, ToString(i), v, true): YES or THREW. */
static int set_at(embi_js_runtime *rt, embi_js_object *o, double i, embi_js_value v)
{
    if (i < 4294967295.0 && embi_js_obj_index_set_fast(rt, o, (uint32_t)i, v)) {
        return EMBI_JS_YES;
    }
    return embi_js_put(rt, o, key_of(rt, i), v, true);
}

/* CreateDataPropertyOrThrow(O, ToString(i), v): YES or THREW. */
static int create_at(embi_js_runtime *rt, embi_js_object *o, double i, embi_js_value v)
{
    if (i < 4294967295.0 && embi_js_obj_create_index_fast(rt, o, (uint32_t)i, v)) {
        return EMBI_JS_YES;
    }
    return embi_js_create_data_property_or_throw(rt, o, key_of(rt, i), v);
}

/* DeletePropertyOrThrow(O, ToString(i)): YES or THREW. */
static int delete_at(embi_js_runtime *rt, embi_js_object *o, double i)
{
    return embi_js_delete_or_throw(rt, o, key_of(rt, i));
}

/* Set(O, "length", n, true): YES or THREW. */
static int set_length(embi_js_runtime *rt, embi_js_object *o, double n)
{
    return embi_js_put(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(n), true);
}

/* ToIntegerOrInfinity of v, relative to len (a start or end index): the result is in [0, len]. */
static bool rel_index(embi_js_runtime *rt, embi_js_value v, double len, double *out)
{
    double r;
    if (!embi_js_to_integer_or_infinity(rt, v, &r)) {
        return false;
    }
    if (r < 0) {
        r += len;
        *out = r < 0 ? 0 : r;
    } else {
        *out = r > len ? len : r;
    }
    return true;
}

/* ArraySpeciesCreate(o, len) as an object, or nullptr when it throws. */
static embi_js_object *species_create(embi_js_runtime *rt, embi_js_object *o, double len)
{
    return embi_js_array_species_create(rt, o, len);
}

/* The length of an object's array-like view. Returns false when it throws. */
static bool length_of(embi_js_runtime *rt, embi_js_object *o, double *len)
{
    return embi_js_length_of_array_like(rt, o, len);
}

static embi_js_value throw_type(embi_js_runtime *rt, const char *msg)
{
    return embi_js_throw_type_error(rt, "%s", msg);
}

static embi_js_value throw_range(embi_js_runtime *rt, const char *msg)
{
    return embi_js_throw_range_error(rt, "%s", msg);
}

/* Appends the code units of s to v. Returns false (nothing appended) when the result would be
 * longer than a string can be. */
static bool u16_append(u16_vec *v, const embi_js_string *s)
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

/* ------------------------------------------------------------------ iteration methods */

enum iter_kind {
    IT_EVERY,
    IT_SOME,
    IT_FOR_EACH,
    IT_MAP,
    IT_FILTER
};

/* every, some, forEach, map and filter: the callback sees (value, index, O) for each present
 * element, with thisArg as its this. */
static embi_js_value iterate(embi_js_realm *r, embi_js_value this_v, int argc,
                             const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    enum iter_kind kind = (enum iter_kind)magic;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_value cb = arg_at(argc, argv, 0);
    if (!embi_js_is_callable(cb)) {
        return throw_type(rt, "callback is not a function");
    }
    embi_js_value this_arg = arg_at(argc, argv, 1);
    embi_js_object *out = nullptr;
    if (kind == IT_MAP) {
        out = species_create(rt, o, len);
    } else if (kind == IT_FILTER) {
        out = species_create(rt, o, 0.0);
    }
    if (kind == IT_MAP || kind == IT_FILTER) {
        if (!out) {
            return EXC;
        }
    }
    double to = 0.0;
    for (double k = 0.0; k < len; k++) {
        int h = has_at(rt, o, k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_NO) {
            continue;
        }
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value av[3] = {v, embi_js_number(k), embi_js_from_object(o)};
        embi_js_value res = embi_js_call(rt, cb, this_arg, 3, av);
        if (res == EXC) {
            return EXC;
        }
        switch (kind) {
        case IT_EVERY:
            if (!embi_js_to_boolean(res)) {
                return EMBI_JS_FALSE;
            }
            break;
        case IT_SOME:
            if (embi_js_to_boolean(res)) {
                return EMBI_JS_TRUE;
            }
            break;
        case IT_FOR_EACH:
            break;
        case IT_MAP:
            if (create_at(rt, out, k, res) == EMBI_JS_THREW) {
                return EXC;
            }
            break;
        case IT_FILTER:
            if (embi_js_to_boolean(res)) {
                if (create_at(rt, out, to, v) == EMBI_JS_THREW) {
                    return EXC;
                }
                to++;
            }
            break;
        }
    }
    switch (kind) {
    case IT_EVERY:
        return EMBI_JS_TRUE;
    case IT_SOME:
        return EMBI_JS_FALSE;
    case IT_FOR_EACH:
        return UNDEF;
    case IT_MAP:
    case IT_FILTER:
        return embi_js_from_object(out);
    }
    return UNDEF;
}

/* reduce (magic 0) and reduceRight (magic 1). */
static embi_js_value reduce_impl(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    bool right = magic != 0;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_value cb = arg_at(argc, argv, 0);
    if (!embi_js_is_callable(cb)) {
        return throw_type(rt, "callback is not a function");
    }
    double step = right ? -1.0 : 1.0;
    double k = right ? len - 1.0 : 0.0;
    embi_js_value acc = UNDEF;
    if (argc >= 2) {
        acc = argv[1];
    } else {
        bool found = false;
        for (; right ? k >= 0.0 : k < len; k += step) {
            int h = has_at(rt, o, k);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_YES) {
                acc = get_at(rt, o, k);
                if (acc == EXC) {
                    return EXC;
                }
                found = true;
                k += step;
                break;
            }
        }
        if (!found) {
            return throw_type(rt, "Reduce of empty array with no initial value");
        }
    }
    for (; right ? k >= 0.0 : k < len; k += step) {
        int h = has_at(rt, o, k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_NO) {
            continue;
        }
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value av[4] = {acc, v, embi_js_number(k), embi_js_from_object(o)};
        acc = embi_js_call(rt, cb, UNDEF, 4, av);
        if (acc == EXC) {
            return EXC;
        }
    }
    return acc;
}

/* find (0), findIndex (1), findLast (2) and findLastIndex (3). The predicate runs for every index,
 * holes included. */
static embi_js_value find_impl(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_value pred = arg_at(argc, argv, 0);
    if (!embi_js_is_callable(pred)) {
        return throw_type(rt, "predicate is not a function");
    }
    embi_js_value this_arg = arg_at(argc, argv, 1);
    bool last = magic >= 2;
    bool want_index = (magic & 1) != 0;
    double step = last ? -1.0 : 1.0;
    for (double k = last ? len - 1.0 : 0.0; last ? k >= 0.0 : k < len; k += step) {
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        embi_js_value av[3] = {v, embi_js_number(k), embi_js_from_object(o)};
        embi_js_value res = embi_js_call(rt, pred, this_arg, 3, av);
        if (res == EXC) {
            return EXC;
        }
        if (embi_js_to_boolean(res)) {
            return want_index ? embi_js_number(k) : v;
        }
    }
    return want_index ? embi_js_from_int32(-1) : UNDEF;
}

/* ------------------------------------------------------------------ search */

/* ToIntegerOrInfinity of the argument at index i (undefined reads as 0). */
static bool integer_arg(embi_js_runtime *rt, int argc, const embi_js_value *argv, int i,
                        double *out)
{
    return embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, i), out);
}

/* indexOf (magic 0) and lastIndexOf (magic 1), both with strict equality. */
static embi_js_value index_of(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (len == 0.0) {
        return embi_js_from_int32(-1);
    }
    embi_js_value target = arg_at(argc, argv, 0);
    if (magic == 0) {
        double n = 0.0;
        if (argc > 1 && !integer_arg(rt, argc, argv, 1, &n)) {
            return EXC;
        }
        if (n >= len) {
            return embi_js_from_int32(-1);
        }
        double k = n >= 0.0 ? n : (len + n < 0.0 ? 0.0 : len + n);
        for (; k < len; k++) {
            int h = has_at(rt, o, k);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_NO) {
                continue;
            }
            embi_js_value v = get_at(rt, o, k);
            if (v == EXC) {
                return EXC;
            }
            if (embi_js_strict_equal(target, v)) {
                return embi_js_number(k);
            }
        }
    } else {
        double n = len - 1.0;
        if (argc > 1 && !integer_arg(rt, argc, argv, 1, &n)) {
            return EXC;
        }
        double k = n >= 0.0 ? (n < len - 1.0 ? n : len - 1.0) : len + n;
        for (; k >= 0.0; k--) {
            int h = has_at(rt, o, k);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_NO) {
                continue;
            }
            embi_js_value v = get_at(rt, o, k);
            if (v == EXC) {
                return EXC;
            }
            if (embi_js_strict_equal(target, v)) {
                return embi_js_number(k);
            }
        }
    }
    return embi_js_from_int32(-1);
}

/* includes: SameValueZero, and holes read as undefined (no HasProperty). */
static embi_js_value proto_includes(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (len == 0.0) {
        return EMBI_JS_FALSE;
    }
    double n = 0.0;
    if (argc > 1 && !integer_arg(rt, argc, argv, 1, &n)) {
        return EXC;
    }
    if (n >= len) {
        return EMBI_JS_FALSE;
    }
    double k = n >= 0.0 ? n : (len + n < 0.0 ? 0.0 : len + n);
    embi_js_value target = arg_at(argc, argv, 0);
    for (; k < len; k++) {
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        if (embi_js_same_value_zero(target, v)) {
            return EMBI_JS_TRUE;
        }
    }
    return EMBI_JS_FALSE;
}

/* ------------------------------------------------------------------ join and toString */

/* Joins the elements of o (length len) with sep. Holes, undefined and null give empty parts.
 * With locale set, each other element is converted with its toLocaleString method. */
/* Units of join output buffered before a flat chunk is cut (J5g). */
#define JOIN_CHUNK_UNITS 4096u

/* Appends s to the join buffer, counting its units in total. False when the joined string would
 * be longer than a string can be (nothing appended). */
static bool join_part(u16_vec *out, uint64_t *total, const embi_js_string *s)
{
    uint64_t n = embi_js_str_len(s);
    if (*total + n > EMBI_JS_STR_MAX_LEN) {
        return false;
    }
    *total += n;
    return u16_append(out, s);
}

/* Cuts the buffered units into a flat chunk and joins it onto the rope *acc (J5g). Empties out.
 * *have is false until the first chunk exists. */
static bool join_flush(embi_js_runtime *rt, u16_vec *out, embi_js_value *acc, bool *have)
{
    embi_js_string *chunk = embi_js_str_new_utf16(rt, out->data, out->len);
    out->len = 0;
    if (!chunk) {
        return false;
    }
    embi_js_value cv = embi_js_from_string(chunk);
    *acc = *have ? embi_js_str_concat_v(rt, *acc, cv) : cv;
    *have = true;
    return true;
}

static embi_js_value join_core(embi_js_runtime *rt, embi_js_object *o, double len,
                               embi_js_string *sep, bool locale)
{
    if (!embi_js_native_stack_ok(rt)) {
        return throw_range(rt, "Maximum call stack size exceeded");
    }
    u16_vec out = {nullptr, 0, 0};
    embi_js_value result = EXC;
    /* J5g: a result longer than JOIN_CHUNK_UNITS is built as flat chunks joined with
     * embi_js_str_concat_v, so it comes out as a rope. */
    embi_js_value acc = UNDEF;
    bool have_acc = false;
    uint64_t total = 0;
    for (double k = 0.0; k < len; k++) {
        if (k > 0.0 && !join_part(&out, &total, sep)) {
            (void)throw_range(rt, "Invalid string length");
            goto done;
        }
        embi_js_value e = get_at(rt, o, k);
        if (e == EXC) {
            goto done;
        }
        if (embi_js_is_nullish(e)) {
            continue;
        }
        if (locale) {
            e = embi_js_invoke(rt, e, embi_js_key_c(rt, "toLocaleString"), 0, nullptr);
            if (e == EXC) {
                goto done;
            }
        }
        embi_js_string *s = embi_js_to_string(rt, e);
        if (!s) {
            goto done;
        }
        if (!join_part(&out, &total, s)) {
            (void)throw_range(rt, "Invalid string length");
            goto done;
        }
        if (out.len >= JOIN_CHUNK_UNITS && !join_flush(rt, &out, &acc, &have_acc)) {
            goto done;
        }
    }
    if (have_acc) {
        if (out.len > 0 && !join_flush(rt, &out, &acc, &have_acc)) {
            goto done;
        }
        result = acc;
    } else {
        embi_js_string *res = embi_js_str_new_utf16(rt, out.data, out.len);
        if (res) {
            result = embi_js_from_string(res);
        }
    }
done:
    embi_vec_free(&out);
    return result;
}

static embi_js_value proto_join(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_string *sep;
    embi_js_value sv = arg_at(argc, argv, 0);
    if (embi_js_is_undefined(sv)) {
        sep = embi_js_str_new_latin1(rt, (const uint8_t *)",", 1);
    } else {
        sep = embi_js_to_string(rt, sv);
    }
    if (!sep) {
        return EXC;
    }
    return join_core(rt, o, len, sep, false);
}

static embi_js_value proto_to_locale_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_string *sep = embi_js_str_new_latin1(rt, (const uint8_t *)",", 1);
    if (!sep) {
        return EXC;
    }
    return join_core(rt, o, len, sep, true);
}

/* Array.prototype.toString: calls join when it is callable, else %Object.prototype.toString%.
 * The fallback is the realm's saved %Object.prototype.toString%, so replacing or deleting
 * Object.prototype.toString does not change the result. */
static embi_js_value proto_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_value f = embi_js_get(rt, o, embi_js_key_c(rt, "join"));
    if (f == EXC) {
        return EXC;
    }
    if (!embi_js_is_callable(f)) {
        f = embi_js_from_object(r->obj_to_string);
    }
    return embi_js_call(rt, f, embi_js_from_object(o), 0, nullptr);
}

/* ------------------------------------------------------------------ mutators */

static embi_js_value proto_push(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (len + (double)argc > MAX_SAFE) {
        return throw_type(rt, "Pushing elements past 2^53-1 length");
    }
    double n = len;
    /* J5e: a packed array appends directly (the length is then n after each element). Otherwise
     * each element is a Set and the length is set at the end. */
    bool need_len = argc == 0;
    for (int i = 0; i < argc; i++) {
        if (embi_js_array_push_fast(rt, o, argv[i])) {
            n++;
            continue;
        }
        need_len = true;
        if (set_at(rt, o, n, argv[i]) == EMBI_JS_THREW) {
            return EXC;
        }
        n++;
    }
    if (need_len && set_length(rt, o, n) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_number(n);
}

static embi_js_value proto_pop(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (len == 0.0) {
        if (set_length(rt, o, 0.0) == EMBI_JS_THREW) {
            return EXC;
        }
        return UNDEF;
    }
    embi_js_value fast;
    if (embi_js_array_pop_fast(rt, o, &fast)) {
        return fast;
    }
    double idx = len - 1.0;
    embi_js_value e = get_at(rt, o, idx);
    if (e == EXC) {
        return EXC;
    }
    if (delete_at(rt, o, idx) == EMBI_JS_THREW) {
        return EXC;
    }
    if (set_length(rt, o, idx) == EMBI_JS_THREW) {
        return EXC;
    }
    return e;
}

static embi_js_value proto_shift(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (len == 0.0) {
        if (set_length(rt, o, 0.0) == EMBI_JS_THREW) {
            return EXC;
        }
        return UNDEF;
    }
    embi_js_value first = get_at(rt, o, 0.0);
    if (first == EXC) {
        return EXC;
    }
    for (double k = 1.0; k < len; k++) {
        int h = has_at(rt, o, k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_YES) {
            embi_js_value v = get_at(rt, o, k);
            if (v == EXC || set_at(rt, o, k - 1.0, v) == EMBI_JS_THREW) {
                return EXC;
            }
        } else if (delete_at(rt, o, k - 1.0) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    if (delete_at(rt, o, len - 1.0) == EMBI_JS_THREW) {
        return EXC;
    }
    if (set_length(rt, o, len - 1.0) == EMBI_JS_THREW) {
        return EXC;
    }
    return first;
}

static embi_js_value proto_unshift(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    if (argc > 0) {
        if (len + (double)argc > MAX_SAFE) {
            return throw_type(rt, "Unshifting elements past 2^53-1 length");
        }
        for (double k = len; k > 0.0; k--) {
            double from = k - 1.0;
            double to = k + (double)argc - 1.0;
            int h = has_at(rt, o, from);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_YES) {
                embi_js_value v = get_at(rt, o, from);
                if (v == EXC || set_at(rt, o, to, v) == EMBI_JS_THREW) {
                    return EXC;
                }
            } else if (delete_at(rt, o, to) == EMBI_JS_THREW) {
                return EXC;
            }
        }
        for (int j = 0; j < argc; j++) {
            if (set_at(rt, o, (double)j, argv[j]) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    double n = len + (double)argc;
    if (set_length(rt, o, n) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_number(n);
}

static embi_js_value proto_reverse(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double middle = (double)((uint64_t)len / 2u);
    for (double lower = 0.0; lower != middle; lower++) {
        double upper = len - lower - 1.0;
        int lower_exists = has_at(rt, o, lower);
        if (lower_exists == EMBI_JS_THREW) {
            return EXC;
        }
        embi_js_value lower_v = UNDEF;
        if (lower_exists == EMBI_JS_YES) {
            lower_v = get_at(rt, o, lower);
            if (lower_v == EXC) {
                return EXC;
            }
        }
        int upper_exists = has_at(rt, o, upper);
        if (upper_exists == EMBI_JS_THREW) {
            return EXC;
        }
        embi_js_value upper_v = UNDEF;
        if (upper_exists == EMBI_JS_YES) {
            upper_v = get_at(rt, o, upper);
            if (upper_v == EXC) {
                return EXC;
            }
        }
        if (lower_exists == EMBI_JS_YES && upper_exists == EMBI_JS_YES) {
            if (set_at(rt, o, lower, upper_v) == EMBI_JS_THREW ||
                set_at(rt, o, upper, lower_v) == EMBI_JS_THREW) {
                return EXC;
            }
        } else if (upper_exists == EMBI_JS_YES) {
            if (set_at(rt, o, lower, upper_v) == EMBI_JS_THREW ||
                delete_at(rt, o, upper) == EMBI_JS_THREW) {
                return EXC;
            }
        } else if (lower_exists == EMBI_JS_YES) {
            if (delete_at(rt, o, lower) == EMBI_JS_THREW ||
                set_at(rt, o, upper, lower_v) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    return embi_js_from_object(o);
}

static embi_js_value proto_fill(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double k;
    if (!rel_index(rt, arg_at(argc, argv, 1), len, &k)) {
        return EXC;
    }
    double final = len;
    if (!embi_js_is_undefined(arg_at(argc, argv, 2))) {
        if (!rel_index(rt, argv[2], len, &final)) {
            return EXC;
        }
    }
    embi_js_value value = arg_at(argc, argv, 0);
    for (; k < final; k++) {
        if (set_at(rt, o, k, value) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

static embi_js_value proto_copy_within(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double to;
    if (!rel_index(rt, arg_at(argc, argv, 0), len, &to)) {
        return EXC;
    }
    double from;
    if (!rel_index(rt, arg_at(argc, argv, 1), len, &from)) {
        return EXC;
    }
    double final = len;
    if (!embi_js_is_undefined(arg_at(argc, argv, 2))) {
        if (!rel_index(rt, argv[2], len, &final)) {
            return EXC;
        }
    }
    double count = final - from < len - to ? final - from : len - to;
    if (count > 0.0) {
        double dir = 1.0;
        if (from < to && to < from + count) {
            dir = -1.0;
            from += count - 1.0;
            to += count - 1.0;
        }
        for (; count > 0.0; count--) {
            int h = has_at(rt, o, from);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_YES) {
                embi_js_value v = get_at(rt, o, from);
                if (v == EXC || set_at(rt, o, to, v) == EMBI_JS_THREW) {
                    return EXC;
                }
            } else if (delete_at(rt, o, to) == EMBI_JS_THREW) {
                return EXC;
            }
            from += dir;
            to += dir;
        }
    }
    return embi_js_from_object(o);
}

static embi_js_value proto_splice(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double start;
    if (!rel_index(rt, arg_at(argc, argv, 0), len, &start)) {
        return EXC;
    }
    double item_count = argc > 2 ? (double)(argc - 2) : 0.0;
    double dc;
    if (argc == 0) {
        dc = 0.0;
    } else if (argc == 1) {
        dc = len - start;
    } else {
        double d;
        if (!integer_arg(rt, argc, argv, 1, &d)) {
            return EXC;
        }
        double max = len - start;
        dc = d < 0.0 ? 0.0 : (d > max ? max : d);
    }
    if (len + item_count - dc > MAX_SAFE) {
        return throw_type(rt, "Splicing elements past 2^53-1 length");
    }
    embi_js_object *a = species_create(rt, o, dc);
    if (!a) {
        return EXC;
    }
    for (double k = 0.0; k < dc; k++) {
        int h = has_at(rt, o, start + k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_YES) {
            embi_js_value v = get_at(rt, o, start + k);
            if (v == EXC || create_at(rt, a, k, v) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    if (set_length(rt, a, dc) == EMBI_JS_THREW) {
        return EXC;
    }
    if (item_count < dc) {
        for (double k = start; k < len - dc; k++) {
            double from = k + dc;
            double to = k + item_count;
            int h = has_at(rt, o, from);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_YES) {
                embi_js_value v = get_at(rt, o, from);
                if (v == EXC || set_at(rt, o, to, v) == EMBI_JS_THREW) {
                    return EXC;
                }
            } else if (delete_at(rt, o, to) == EMBI_JS_THREW) {
                return EXC;
            }
        }
        for (double k = len; k > len - dc + item_count; k--) {
            if (delete_at(rt, o, k - 1.0) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    } else if (item_count > dc) {
        for (double k = len - dc; k > start; k--) {
            double from = k + dc - 1.0;
            double to = k + item_count - 1.0;
            int h = has_at(rt, o, from);
            if (h == EMBI_JS_THREW) {
                return EXC;
            }
            if (h == EMBI_JS_YES) {
                embi_js_value v = get_at(rt, o, from);
                if (v == EXC || set_at(rt, o, to, v) == EMBI_JS_THREW) {
                    return EXC;
                }
            } else if (delete_at(rt, o, to) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    for (int i = 0; i < (int)item_count; i++) {
        if (set_at(rt, o, start + (double)i, argv[2 + i]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    if (set_length(rt, o, len - dc + item_count) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ sort */

typedef struct sort_ctx {
    embi_js_runtime *rt;
    embi_js_value cmp;
} sort_ctx;

/* SortCompare: the sign of the comparison of x and y. undefined sorts last without calling the
 * comparator. A NaN result of the comparator reads as equal. On a throw, *threw is set and the
 * result is 0. */
static int sort_compare(sort_ctx *c, embi_js_value x, embi_js_value y, bool *threw)
{
    embi_js_runtime *rt = c->rt;
    bool xu = embi_js_is_undefined(x);
    bool yu = embi_js_is_undefined(y);
    if (xu || yu) {
        if (xu && yu) {
            return 0;
        }
        return xu ? 1 : -1;
    }
    if (!embi_js_is_undefined(c->cmp)) {
        embi_js_value av[2] = {x, y};
        embi_js_value r = embi_js_call(rt, c->cmp, UNDEF, 2, av);
        double d;
        if (r == EXC || !embi_js_to_number(rt, r, &d)) {
            *threw = true;
            return 0;
        }
        return d < 0.0 ? -1 : (d > 0.0 ? 1 : 0);
    }
    embi_js_string *xs = embi_js_to_string(rt, x);
    if (!xs) {
        *threw = true;
        return 0;
    }
    embi_js_string *ys = embi_js_to_string(rt, y);
    if (!ys) {
        *threw = true;
        return 0;
    }
    int d = embi_js_str_compare(xs, ys);
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

/* Stable bottom-up merge sort over the first n values of a. Both buffers are GC-visible varrays,
 * so values survive the comparator running a collection. Returns the sorted varray, or nullptr
 * with the exception pending. Any comparator result is safe: the merge always terminates. */
static embi_js_varray *merge_sort(sort_ctx *c, embi_js_varray *a, uint32_t n)
{
    embi_js_runtime *rt = c->rt;
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
                bool threw = false;
                int cmp = sort_compare(c, a->elems[i], a->elems[j], &threw);
                if (threw) {
                    return nullptr;
                }
                if (cmp <= 0) {
                    EMBI_JS_STORE(b, b->elems[k++], a->elems[i++]);
                } else {
                    EMBI_JS_STORE(b, b->elems[k++], a->elems[j++]);
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

static embi_js_value proto_sort(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value cmp = arg_at(argc, argv, 0);
    if (!embi_js_is_undefined(cmp) && !embi_js_is_callable(cmp)) {
        return throw_type(rt, "The comparison function must be either a function or undefined");
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    /* SortIndexedProperties with skip-holes: the present values, in index order. */
    uint32_t cap = 8;
    uint32_t n = 0;
    embi_js_varray *items = embi_js_varray_new(rt, cap);
    for (double k = 0.0; k < len; k++) {
        int h = has_at(rt, o, k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_NO) {
            continue;
        }
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        if (n == UINT32_MAX) {
            return throw_range(rt, "Invalid array length");
        }
        if (n == cap) {
            uint32_t grown = cap > UINT32_MAX / 2 ? UINT32_MAX - 1 : cap * 2;
            items = embi_js_varray_grow(rt, items, grown);
            cap = grown;
        }
        EMBI_JS_STORE(items, items->elems[n++], v);
    }
    sort_ctx c = {rt, cmp};
    embi_js_varray *sorted = merge_sort(&c, items, n);
    if (!sorted) {
        return EXC;
    }
    for (uint32_t j = 0; j < n; j++) {
        if (set_at(rt, o, (double)j, sorted->elems[j]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    for (double j = (double)n; j < len; j++) {
        if (delete_at(rt, o, j) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(o);
}

/* ------------------------------------------------------------------ slicing and concatenation */

static embi_js_value proto_slice(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double k;
    if (!rel_index(rt, arg_at(argc, argv, 0), len, &k)) {
        return EXC;
    }
    double final = len;
    if (!embi_js_is_undefined(arg_at(argc, argv, 1))) {
        if (!rel_index(rt, argv[1], len, &final)) {
            return EXC;
        }
    }
    double count = final > k ? final - k : 0.0;
    embi_js_object *a = species_create(rt, o, count);
    if (!a) {
        return EXC;
    }
    double n = 0.0;
    for (; k < final; k++, n++) {
        int h = has_at(rt, o, k);
        if (h == EMBI_JS_THREW) {
            return EXC;
        }
        if (h == EMBI_JS_YES) {
            embi_js_value v = get_at(rt, o, k);
            if (v == EXC || create_at(rt, a, n, v) == EMBI_JS_THREW) {
                return EXC;
            }
        }
    }
    if (set_length(rt, a, n) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(a);
}

/* IsConcatSpreadable: Symbol.isConcatSpreadable when set, else IsArray. YES, NO or THREW. */
static int is_concat_spreadable(embi_js_runtime *rt, embi_js_value v)
{
    if (!embi_js_is_object(v)) {
        return EMBI_JS_NO;
    }
    embi_js_value s = embi_js_get(rt, embi_js_as_object(v), embi_js_wk_sym(rt, EMBI_JS_WK_IS_CONCAT_SPREADABLE));
    if (s == EXC) {
        return EMBI_JS_THREW;
    }
    if (!embi_js_is_undefined(s)) {
        return embi_js_to_boolean(s) ? EMBI_JS_YES : EMBI_JS_NO;
    }
    return embi_js_is_array_checked(rt, v);
}

static embi_js_value proto_concat(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    embi_js_object *a = species_create(rt, o, 0.0);
    if (!a) {
        return EXC;
    }
    double n = 0.0;
    for (int i = -1; i < argc; i++) {
        embi_js_value e = i < 0 ? embi_js_from_object(o) : argv[i];
        int sp = is_concat_spreadable(rt, e);
        if (sp == EMBI_JS_THREW) {
            return EXC;
        }
        if (sp == EMBI_JS_YES) {
            embi_js_object *eo = embi_js_as_object(e);
            double len;
            if (!length_of(rt, eo, &len)) {
                return EXC;
            }
            if (n + len > MAX_SAFE) {
                return throw_type(rt, "Concatenating past 2^53-1 length");
            }
            for (double k = 0.0; k < len; k++, n++) {
                int h = has_at(rt, eo, k);
                if (h == EMBI_JS_THREW) {
                    return EXC;
                }
                if (h == EMBI_JS_YES) {
                    embi_js_value v = get_at(rt, eo, k);
                    if (v == EXC || create_at(rt, a, n, v) == EMBI_JS_THREW) {
                        return EXC;
                    }
                }
            }
        } else {
            if (n >= MAX_SAFE) {
                return throw_type(rt, "Concatenating past 2^53-1 length");
            }
            if (create_at(rt, a, n, e) == EMBI_JS_THREW) {
                return EXC;
            }
            n++;
        }
    }
    if (set_length(rt, a, n) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ flat and flatMap */

/* FlattenIntoArray. Returns the next target index, or -1 with the exception pending. mapper is
 * used only when has_mapper is set. */
static double flatten_into(embi_js_runtime *rt, embi_js_object *target, embi_js_object *source,
                           double source_len, double start, double depth, bool has_mapper,
                           embi_js_value mapper, embi_js_value this_arg)
{
    if (!embi_js_native_stack_ok(rt)) {
        (void)throw_range(rt, "Maximum call stack size exceeded");
        return -1.0;
    }
    double ti = start;
    for (double si = 0.0; si < source_len; si++) {
        int h = has_at(rt, source, si);
        if (h == EMBI_JS_THREW) {
            return -1.0;
        }
        if (h == EMBI_JS_NO) {
            continue;
        }
        embi_js_value el = get_at(rt, source, si);
        if (el == EXC) {
            return -1.0;
        }
        if (has_mapper) {
            embi_js_value av[3] = {el, embi_js_number(si), embi_js_from_object(source)};
            el = embi_js_call(rt, mapper, this_arg, 3, av);
            if (el == EXC) {
                return -1.0;
            }
        }
        int el_is_arr = embi_js_is_array_checked(rt, el);
        if (el_is_arr == EMBI_JS_THREW) {
            return -1.0;
        }
        if (depth > 0.0 && el_is_arr == EMBI_JS_YES) {
            embi_js_object *eo = embi_js_as_object(el);
            double el_len;
            if (!length_of(rt, eo, &el_len)) {
                return -1.0;
            }
            ti = flatten_into(rt, target, eo, el_len, ti, depth - 1.0, false, UNDEF, UNDEF);
            if (ti < 0.0) {
                return -1.0;
            }
        } else {
            if (ti >= MAX_SAFE) {
                (void)throw_type(rt, "Flattening past 2^53-1 length");
                return -1.0;
            }
            if (create_at(rt, target, ti, el) == EMBI_JS_THREW) {
                return -1.0;
            }
            ti++;
        }
    }
    return ti;
}

static embi_js_value proto_flat(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double depth = 1.0;
    if (!embi_js_is_undefined(arg_at(argc, argv, 0))) {
        if (!integer_arg(rt, argc, argv, 0, &depth)) {
            return EXC;
        }
        if (depth < 0.0) {
            depth = 0.0;
        }
    }
    embi_js_object *a = species_create(rt, o, 0.0);
    if (!a) {
        return EXC;
    }
    if (flatten_into(rt, a, o, len, 0.0, depth, false, UNDEF, UNDEF) < 0.0) {
        return EXC;
    }
    return embi_js_from_object(a);
}

static embi_js_value proto_flat_map(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_value mapper = arg_at(argc, argv, 0);
    if (!embi_js_is_callable(mapper)) {
        return throw_type(rt, "flatMap mapper is not a function");
    }
    embi_js_object *a = species_create(rt, o, 0.0);
    if (!a) {
        return EXC;
    }
    if (flatten_into(rt, a, o, len, 0.0, 1.0, true, mapper, arg_at(argc, argv, 1)) < 0.0) {
        return EXC;
    }
    return embi_js_from_object(a);
}

static embi_js_value proto_at(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double rel;
    if (!integer_arg(rt, argc, argv, 0, &rel)) {
        return EXC;
    }
    double k = rel >= 0.0 ? rel : len + rel;
    if (k < 0.0 || k >= len) {
        return UNDEF;
    }
    return get_at(rt, o, k);
}

/* ------------------------------------------------------------------ constructor and statics */

static embi_js_value array_ctor(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic,
                                embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_ARRAY);
    if (proto == EXC) {
        return EXC;
    }
    if (argc == 1 && embi_js_is_number(argv[0])) {
        uint32_t u;
        if (!embi_js_to_uint32(rt, argv[0], &u)) {
            return EXC;
        }
        if ((double)u != embi_js_as_double(argv[0])) {
            return throw_range(rt, "Invalid array length");
        }
        embi_js_object *a = embi_js_array_create(rt, (double)u, proto);
        return a ? embi_js_from_object(a) : EXC;
    }
    embi_js_object *a = embi_js_array_create(rt, 0.0, proto);
    if (!a) {
        return EXC;
    }
    for (int i = 0; i < argc; i++) {
        if (create_at(rt, a, (double)i, argv[i]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

static embi_js_value array_is_array(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    int st = embi_js_is_array_checked(r->rt, arg_at(argc, argv, 0));
    if (st == EMBI_JS_THREW) {
        return EMBI_JS_EXCEPTION;
    }
    return embi_js_from_bool(st == EMBI_JS_YES);
}

/* Array.of: constructs this(len) when this is a constructor, else an ordinary array. */
static embi_js_value array_of(embi_js_realm *r, embi_js_value this_v, int argc,
                              const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *a;
    embi_js_value lenv = embi_js_number((double)argc);
    if (embi_js_is_constructor(this_v)) {
        embi_js_value av = embi_js_construct(rt, this_v, 1, &lenv, this_v);
        if (av == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(av)) {
            return throw_type(rt, "Array.of: constructor did not return an object");
        }
        a = embi_js_as_object(av);
    } else {
        a = embi_js_array_create(rt, (double)argc, rt->protos[EMBI_JS_PROTO_ARRAY]);
        if (!a) {
            return EXC;
        }
    }
    for (int i = 0; i < argc; i++) {
        if (create_at(rt, a, (double)i, argv[i]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    if (set_length(rt, a, (double)argc) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(a);
}

static embi_js_value species_getter(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)r;
    (void)argc;
    (void)argv;
    (void)magic;
    return this_v;
}

/* ------------------------------------------------------------------ Array.from (ES2015) */

/* Closes an iterator after an abrupt completion of the caller: the return method runs, its errors
 * are dropped, and the pending exception (the caller's) is kept. */
static void iter_close_quiet(embi_js_runtime *rt, embi_js_value it)
{
    embi_js_value pending = embi_js_exception_get(rt);
    embi_js_value m = embi_js_get_method(rt, it, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (m != EXC && m != UNDEF) {
        (void)embi_js_call(rt, m, it, 0, nullptr);
    }
    embi_js_exception_clear(rt);
    embi_js_exception_set(rt, pending);
}

/* Array.from(items, mapfn, thisArg): the iterable path (GetMethod, then the iterator protocol with
 * IteratorClose on an abrupt completion), or the array-like path. this constructs the result when
 * it is a constructor. */
static embi_js_value array_from(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value items = arg_at(argc, argv, 0);
    embi_js_value mapfn = arg_at(argc, argv, 1);
    embi_js_value this_arg = arg_at(argc, argv, 2);
    bool mapping = !embi_js_is_undefined(mapfn);
    if (mapping && !embi_js_is_callable(mapfn)) {
        return throw_type(rt, "Array.from: when provided, the second argument must be a function");
    }
    bool use_ctor = embi_js_is_constructor(this_v);
    embi_js_value method = embi_js_get_method(rt, items, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method != UNDEF) {
        embi_js_object *a;
        if (use_ctor) {
            embi_js_value av = embi_js_construct(rt, this_v, 0, nullptr, this_v);
            if (av == EXC) {
                return EXC;
            }
            if (!embi_js_is_object(av)) {
                return throw_type(rt, "Array.from: constructor did not return an object");
            }
            a = embi_js_as_object(av);
        } else {
            a = embi_js_array_create(rt, 0.0, rt->protos[EMBI_JS_PROTO_ARRAY]);
            if (!a) {
                return EXC;
            }
        }
        embi_js_value it = embi_js_call(rt, method, items, 0, nullptr);
        if (it == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(it)) {
            return throw_type(rt, "Result of the Symbol.iterator method is not an object");
        }
        embi_js_value next =
            embi_js_get(rt, embi_js_as_object(it), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
        if (next == EXC) {
            return EXC;
        }
        double k = 0.0;
        for (;;) {
            embi_js_value res = embi_js_call(rt, next, it, 0, nullptr);
            if (res == EXC) {
                return EXC;
            }
            if (!embi_js_is_object(res)) {
                return throw_type(rt, "Iterator result is not an object");
            }
            embi_js_value done =
                embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_DONE));
            if (done == EXC) {
                return EXC;
            }
            if (embi_js_to_boolean(done)) {
                break;
            }
            embi_js_value v =
                embi_js_get(rt, embi_js_as_object(res), embi_js_iter_key(rt, EMBI_JS_IK_VALUE));
            if (v == EXC) {
                return EXC;
            }
            if (mapping) {
                embi_js_value av[2] = {v, embi_js_number(k)};
                v = embi_js_call(rt, mapfn, this_arg, 2, av);
                if (v == EXC) {
                    iter_close_quiet(rt, it);
                    return EXC;
                }
            }
            if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
                iter_close_quiet(rt, it);
                return EXC;
            }
            k += 1.0;
        }
        if (set_length(rt, a, k) == EMBI_JS_THREW) {
            return EXC;
        }
        return embi_js_from_object(a);
    }
    embi_js_object *src = embi_js_to_object(rt, items);
    if (!src) {
        return EXC;
    }
    double len;
    if (!length_of(rt, src, &len)) {
        return EXC;
    }
    embi_js_object *a;
    if (use_ctor) {
        embi_js_value lenv = embi_js_number(len);
        embi_js_value av = embi_js_construct(rt, this_v, 1, &lenv, this_v);
        if (av == EXC) {
            return EXC;
        }
        if (!embi_js_is_object(av)) {
            return throw_type(rt, "Array.from: constructor did not return an object");
        }
        a = embi_js_as_object(av);
    } else {
        a = embi_js_array_create(rt, len, rt->protos[EMBI_JS_PROTO_ARRAY]);
        if (!a) {
            return EXC;
        }
    }
    for (double k = 0.0; k < len; k++) {
        embi_js_value v = get_at(rt, src, k);
        if (v == EXC) {
            return EXC;
        }
        if (mapping) {
            embi_js_value av[2] = {v, embi_js_number(k)};
            v = embi_js_call(rt, mapfn, this_arg, 2, av);
            if (v == EXC) {
                return EXC;
            }
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    if (set_length(rt, a, len) == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ ES2023 copying methods */

/* ------------------------------------------------------------------ Array.fromAsync (J3b)
 * ECMA-262 Array.fromAsync. A native continuation: the state varray (fa_slot) keeps the phase and
 * the loop variables, and each Await is a promise reaction (class_fa_fn) that resumes the state. */
enum fa_phase {
    FA_ITER_AWAIT,    /* an iterator result from next() is awaited */
    FA_MAP_AWAIT,     /* the mapped value of an iterated item is awaited */
    FA_ARR_AWAIT,     /* an array-like item is awaited */
    FA_ARR_MAP_AWAIT, /* the mapped value of an array-like item is awaited */
    FA_CLOSE_AWAIT,   /* the return() result of a closed iterator is awaited (its outcome ignored) */
};

enum fa_slot {
    FA_S_PHASE,
    FA_S_PROMISE,
    FA_S_A,     /* the result object */
    FA_S_MAPFN,
    FA_S_THIS,  /* thisArg of the mapping function */
    FA_S_ITER,  /* the async iterator record [iterator, next, flags] */
    FA_S_SRC,   /* the array-like source */
    FA_S_K,     /* the next index */
    FA_S_LEN,   /* the array-like length */
    FA_S_ERR,   /* the completion to reject with after an iterator close */
    FA_S_FLAGS, /* FA_MAPPING | FA_USE_ITER */
    FA_S_N,
};
#define FA_MAPPING 1
#define FA_USE_ITER 2

static embi_js_value fa_resume(embi_js_runtime *rt, embi_js_varray *st, bool is_err,
                               embi_js_value v);

static embi_js_value fa_fn_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                int argc, const embi_js_value *argv)
{
    (void)this_v;
    const embi_js_varray *cs = (const embi_js_varray *)f->internal;
    embi_js_varray *st = (embi_js_varray *)embi_js_as_cell(cs->elems[0]);
    bool is_err = embi_js_as_int32(cs->elems[1]) != 0;
    return fa_resume(rt, st, is_err, argc > 0 ? argv[0] : UNDEF);
}

static void fa_fn_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal != nullptr) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_fa_fn = {
    .name = "Function",
    .call = fa_fn_call,
    .trace = fa_fn_trace,
};

/* A reaction closure that resumes st with its argument (is_err: as a rejection). */
static embi_js_value fa_fn_new(embi_js_runtime *rt, embi_js_varray *st, bool is_err)
{
    embi_js_varray *cs = embi_js_varray_new(rt, 2);
    EMBI_JS_STORE(cs, cs->elems[0], embi_js_from_cell(st));
    EMBI_JS_STORE(cs, cs->elems[1], embi_js_from_int32(is_err ? 1 : 0));
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, &class_fa_fn);
    f->internal = cs;
    EMBI_JS_WB_STRUCT(f);
    return embi_js_from_object(f);
}

static embi_js_value fa_take(embi_js_runtime *rt)
{
    embi_js_value ex = embi_js_exception_get(rt);
    embi_js_exception_clear(rt);
    return ex;
}

static embi_js_value fa_reject(embi_js_runtime *rt, embi_js_varray *st, embi_js_value reason)
{
    (void)embi_js_promise_reject_with(rt, embi_js_as_object(st->elems[FA_S_PROMISE]), reason);
    return UNDEF;
}

/* Rejects the result with the pending exception. */
static embi_js_value fa_fail(embi_js_runtime *rt, embi_js_varray *st)
{
    return fa_reject(rt, st, fa_take(rt));
}

/* Await(v) in the phase given. Returns false with the exception pending when v cannot be wrapped. */
static bool fa_await(embi_js_runtime *rt, embi_js_varray *st, int phase, embi_js_value v)
{
    EMBI_JS_STORE(st, st->elems[FA_S_PHASE], embi_js_from_int32(phase));
    embi_js_value pv = embi_js_promise_resolve(rt, rt->protos[EMBI_JS_PROTO_PROMISE_CTOR], v);
    if (pv == EXC) {
        return false;
    }
    embi_js_value ok = fa_fn_new(rt, st, false);
    embi_js_value err = fa_fn_new(rt, st, true);
    (void)embi_js_promise_then(rt, embi_js_as_object(pv), ok, err, nullptr);
    return true;
}

/* AsyncIteratorClose with the throw completion ex: the return method is called and awaited, its
 * outcome is ignored, and the result rejects with ex. */
static embi_js_value fa_close(embi_js_runtime *rt, embi_js_varray *st, embi_js_value ex)
{
    EMBI_JS_STORE(st, st->elems[FA_S_ERR], ex);
    embi_js_varray *rec = (embi_js_varray *)embi_js_as_cell(st->elems[FA_S_ITER]);
    embi_js_value it = rec->elems[0];
    embi_js_value rf = embi_js_get_method(rt, it, embi_js_iter_key(rt, EMBI_JS_IK_RETURN));
    if (rf == EXC) {
        (void)fa_take(rt);
        return fa_reject(rt, st, ex);
    }
    if (rf == UNDEF) {
        return fa_reject(rt, st, ex);
    }
    embi_js_value res = embi_js_call(rt, rf, it, 0, nullptr);
    if (res == EXC) {
        (void)fa_take(rt);
        return fa_reject(rt, st, ex);
    }
    if (!fa_await(rt, st, FA_CLOSE_AWAIT, res)) {
        (void)fa_take(rt);
        return fa_reject(rt, st, ex);
    }
    return UNDEF;
}

/* The next step of the loop, at index K. */
static embi_js_value fa_next(embi_js_runtime *rt, embi_js_varray *st)
{
    int flags = embi_js_as_int32(st->elems[FA_S_FLAGS]);
    double k = embi_js_as_double(st->elems[FA_S_K]);
    if ((flags & FA_USE_ITER) != 0) {
        embi_js_varray *rec = (embi_js_varray *)embi_js_as_cell(st->elems[FA_S_ITER]);
        embi_js_value res = embi_js_call(rt, rec->elems[1], rec->elems[0], 0, nullptr);
        if (res == EXC || !fa_await(rt, st, FA_ITER_AWAIT, res)) {
            return fa_fail(rt, st);
        }
        return UNDEF;
    }
    double len = embi_js_as_double(st->elems[FA_S_LEN]);
    if (k < len) {
        embi_js_value kv = get_at(rt, embi_js_as_object(st->elems[FA_S_SRC]), k);
        if (kv == EXC || !fa_await(rt, st, FA_ARR_AWAIT, kv)) {
            return fa_fail(rt, st);
        }
        return UNDEF;
    }
    if (set_length(rt, embi_js_as_object(st->elems[FA_S_A]), len) == EMBI_JS_THREW) {
        return fa_fail(rt, st);
    }
    (void)embi_js_promise_resolve_with(rt, embi_js_as_object(st->elems[FA_S_PROMISE]),
                                       st->elems[FA_S_A]);
    return UNDEF;
}

/* Resumes the state with the settled value v of the await of its phase. */
static embi_js_value fa_resume(embi_js_runtime *rt, embi_js_varray *st, bool is_err, embi_js_value v)
{
    int phase = embi_js_as_int32(st->elems[FA_S_PHASE]);
    int flags = embi_js_as_int32(st->elems[FA_S_FLAGS]);
    double k = embi_js_as_double(st->elems[FA_S_K]);
    embi_js_object *a = embi_js_as_object(st->elems[FA_S_A]);
    switch (phase) {
    case FA_ITER_AWAIT: {
        if (is_err) {
            return fa_reject(rt, st, v);
        }
        if (!embi_js_is_object(v)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            return fa_fail(rt, st);
        }
        embi_js_value done =
            embi_js_get(rt, embi_js_as_object(v), embi_js_iter_key(rt, EMBI_JS_IK_DONE));
        if (done == EXC) {
            return fa_fail(rt, st);
        }
        if (embi_js_to_boolean(done)) {
            if (set_length(rt, a, k) == EMBI_JS_THREW) {
                return fa_fail(rt, st);
            }
            (void)embi_js_promise_resolve_with(rt, embi_js_as_object(st->elems[FA_S_PROMISE]),
                                               st->elems[FA_S_A]);
            return UNDEF;
        }
        embi_js_value value =
            embi_js_get(rt, embi_js_as_object(v), embi_js_iter_key(rt, EMBI_JS_IK_VALUE));
        if (value == EXC) {
            return fa_fail(rt, st);
        }
        if ((flags & FA_MAPPING) != 0) {
            embi_js_value av[2] = {value, embi_js_number(k)};
            embi_js_value mapped =
                embi_js_call(rt, st->elems[FA_S_MAPFN], st->elems[FA_S_THIS], 2, av);
            if (mapped == EXC) {
                return fa_close(rt, st, fa_take(rt));
            }
            if (!fa_await(rt, st, FA_MAP_AWAIT, mapped)) {
                return fa_close(rt, st, fa_take(rt));
            }
            return UNDEF;
        }
        if (create_at(rt, a, k, value) == EMBI_JS_THREW) {
            return fa_close(rt, st, fa_take(rt));
        }
        EMBI_JS_STORE(st, st->elems[FA_S_K], embi_js_number(k + 1.0));
        return fa_next(rt, st);
    }
    case FA_MAP_AWAIT:
        if (is_err) {
            return fa_close(rt, st, v);
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return fa_close(rt, st, fa_take(rt));
        }
        EMBI_JS_STORE(st, st->elems[FA_S_K], embi_js_number(k + 1.0));
        return fa_next(rt, st);
    case FA_ARR_AWAIT:
        if (is_err) {
            return fa_reject(rt, st, v);
        }
        if ((flags & FA_MAPPING) != 0) {
            embi_js_value av[2] = {v, embi_js_number(k)};
            embi_js_value mapped =
                embi_js_call(rt, st->elems[FA_S_MAPFN], st->elems[FA_S_THIS], 2, av);
            if (mapped == EXC || !fa_await(rt, st, FA_ARR_MAP_AWAIT, mapped)) {
                return fa_fail(rt, st);
            }
            return UNDEF;
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return fa_fail(rt, st);
        }
        EMBI_JS_STORE(st, st->elems[FA_S_K], embi_js_number(k + 1.0));
        return fa_next(rt, st);
    case FA_ARR_MAP_AWAIT:
        if (is_err) {
            return fa_reject(rt, st, v);
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return fa_fail(rt, st);
        }
        EMBI_JS_STORE(st, st->elems[FA_S_K], embi_js_number(k + 1.0));
        return fa_next(rt, st);
    case FA_CLOSE_AWAIT:
        return fa_reject(rt, st, st->elems[FA_S_ERR]);
    default:
        return UNDEF;
    }
}

/* Array.fromAsync(asyncItems, mapfn, thisArg): every error rejects the returned promise, and the
 * steps run until the first Await. */
static embi_js_value array_from_async(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value items = arg_at(argc, argv, 0);
    embi_js_value mapfn = arg_at(argc, argv, 1);
    embi_js_value this_arg = arg_at(argc, argv, 2);
    embi_js_object *promise = embi_js_promise_new(rt);
    embi_js_varray *st = embi_js_varray_new(rt, FA_S_N);
    for (uint32_t i = 0; i < FA_S_N; i++) {
        EMBI_JS_STORE(st, st->elems[i], UNDEF);
    }
    EMBI_JS_STORE(st, st->elems[FA_S_PHASE], embi_js_from_int32(0));
    EMBI_JS_STORE(st, st->elems[FA_S_PROMISE], embi_js_from_object(promise));
    EMBI_JS_STORE(st, st->elems[FA_S_MAPFN], mapfn);
    EMBI_JS_STORE(st, st->elems[FA_S_THIS], this_arg);
    EMBI_JS_STORE(st, st->elems[FA_S_K], embi_js_number(0.0));
    EMBI_JS_STORE(st, st->elems[FA_S_LEN], embi_js_number(0.0));
    EMBI_JS_STORE(st, st->elems[FA_S_FLAGS], embi_js_from_int32(0));
    embi_js_value result = embi_js_from_object(promise);
    bool mapping = !embi_js_is_undefined(mapfn);
    if (mapping && !embi_js_is_callable(mapfn)) {
        (void)throw_type(rt, "Array.fromAsync: when provided, the second argument must be a function");
        (void)fa_fail(rt, st);
        return result;
    }
    EMBI_JS_STORE(st, st->elems[FA_S_FLAGS], embi_js_from_int32(mapping ? FA_MAPPING : 0));
    embi_js_value uas =
        embi_js_get_method(rt, items, embi_js_wk_sym(rt, EMBI_JS_WK_ASYNC_ITERATOR));
    if (uas == EXC) {
        (void)fa_fail(rt, st);
        return result;
    }
    embi_js_value us = UNDEF;
    if (uas == UNDEF) {
        us = embi_js_get_method(rt, items, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
        if (us == EXC) {
            (void)fa_fail(rt, st);
            return result;
        }
    }
    if (uas != UNDEF || us != UNDEF) {
        embi_js_value av;
        if (embi_js_is_constructor(this_v)) {
            av = embi_js_construct(rt, this_v, 0, nullptr, this_v);
        } else {
            embi_js_object *o = embi_js_array_create(rt, 0.0, rt->protos[EMBI_JS_PROTO_ARRAY]);
            av = o != nullptr ? embi_js_from_object(o) : EXC;
        }
        if (av == EXC) {
            (void)fa_fail(rt, st);
            return result;
        }
        if (!embi_js_is_object(av)) {
            (void)throw_type(rt, "Array.fromAsync: constructor did not return an object");
            (void)fa_fail(rt, st);
            return result;
        }
        EMBI_JS_STORE(st, st->elems[FA_S_A], av);
        embi_js_value it;
        if (uas != UNDEF) {
            it = embi_js_call(rt, uas, items, 0, nullptr);
            if (it == EXC) {
                (void)fa_fail(rt, st);
                return result;
            }
            if (!embi_js_is_object(it)) {
                (void)throw_type(rt, "Result of the Symbol.asyncIterator method is not an object");
                (void)fa_fail(rt, st);
                return result;
            }
        } else {
            /* A sync iterable: its iterator is wrapped by CreateAsyncFromSyncIterator. */
            embi_js_value sit = embi_js_call(rt, us, items, 0, nullptr);
            if (sit == EXC) {
                (void)fa_fail(rt, st);
                return result;
            }
            if (!embi_js_is_object(sit)) {
                (void)throw_type(rt, "Result of the Symbol.iterator method is not an object");
                (void)fa_fail(rt, st);
                return result;
            }
            embi_js_value snx =
                embi_js_get(rt, embi_js_as_object(sit), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
            if (snx == EXC) {
                (void)fa_fail(rt, st);
                return result;
            }
            embi_js_varray *srec = embi_js_varray_new(rt, 3);
            EMBI_JS_STORE(srec, srec->elems[0], sit);
            EMBI_JS_STORE(srec, srec->elems[1], snx);
            EMBI_JS_STORE(srec, srec->elems[2], embi_js_from_int32(0));
            it = embi_js_async_from_sync_create(rt, embi_js_from_cell(srec));
        }
        embi_js_value nx =
            embi_js_get(rt, embi_js_as_object(it), embi_js_iter_key(rt, EMBI_JS_IK_NEXT));
        if (nx == EXC) {
            (void)fa_fail(rt, st);
            return result;
        }
        embi_js_varray *rec = embi_js_varray_new(rt, 3);
        EMBI_JS_STORE(rec, rec->elems[0], it);
        EMBI_JS_STORE(rec, rec->elems[1], nx);
        EMBI_JS_STORE(rec, rec->elems[2], embi_js_from_int32(0));
        EMBI_JS_STORE(st, st->elems[FA_S_ITER], embi_js_from_cell(rec));
        EMBI_JS_STORE(st, st->elems[FA_S_FLAGS], embi_js_from_int32((mapping ? FA_MAPPING : 0) | FA_USE_ITER));
        (void)fa_next(rt, st);
        return result;
    }
    embi_js_object *src = embi_js_to_object(rt, items);
    if (src == nullptr) {
        (void)fa_fail(rt, st);
        return result;
    }
    double len = 0;
    if (!length_of(rt, src, &len)) {
        (void)fa_fail(rt, st);
        return result;
    }
    embi_js_value av;
    if (embi_js_is_constructor(this_v)) {
        embi_js_value lenv = embi_js_number(len);
        av = embi_js_construct(rt, this_v, 1, &lenv, this_v);
    } else {
        embi_js_object *o = embi_js_array_create(rt, len, rt->protos[EMBI_JS_PROTO_ARRAY]);
        av = o != nullptr ? embi_js_from_object(o) : EXC;
    }
    if (av == EXC) {
        (void)fa_fail(rt, st);
        return result;
    }
    if (!embi_js_is_object(av)) {
        (void)throw_type(rt, "Array.fromAsync: constructor did not return an object");
        (void)fa_fail(rt, st);
        return result;
    }
    EMBI_JS_STORE(st, st->elems[FA_S_A], av);
    EMBI_JS_STORE(st, st->elems[FA_S_SRC], embi_js_from_object(src));
    EMBI_JS_STORE(st, st->elems[FA_S_LEN], embi_js_number(len));
    (void)fa_next(rt, st);
    return result;
}

/* Array.prototype.toReversed (ES2023 23.1.3.34). */
static embi_js_value proto_to_reversed(embi_js_realm *r, embi_js_value this_v, int argc,
                                       const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_object *a = embi_js_array_create(rt, len, rt->protos[EMBI_JS_PROTO_ARRAY]);
    if (!a) {
        return EXC;
    }
    for (double k = 0.0; k < len; k++) {
        embi_js_value v = get_at(rt, o, len - k - 1.0);
        if (v == EXC) {
            return EXC;
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

/* Array.prototype.toSorted (ES2023 23.1.3.35): every index is read (holes as undefined), the values
 * are sorted with undefined last, and the result is a new array. */
static embi_js_value proto_to_sorted(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value cmp = arg_at(argc, argv, 0);
    if (!embi_js_is_undefined(cmp) && !embi_js_is_callable(cmp)) {
        return throw_type(rt, "The comparison function must be either a function or undefined");
    }
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    embi_js_object *a = embi_js_array_create(rt, len, rt->protos[EMBI_JS_PROTO_ARRAY]);
    if (!a) {
        return EXC;
    }
    uint32_t n = (uint32_t)len;
    embi_js_varray *items = embi_js_varray_new(rt, n);
    for (uint32_t k = 0; k < n; k++) {
        embi_js_value v = get_at(rt, o, (double)k);
        if (v == EXC) {
            return EXC;
        }
        EMBI_JS_STORE(items, items->elems[k], v);
    }
    sort_ctx c = {rt, cmp};
    embi_js_varray *sorted = merge_sort(&c, items, n);
    if (!sorted) {
        return EXC;
    }
    for (uint32_t j = 0; j < n; j++) {
        if (create_at(rt, a, (double)j, sorted->elems[j]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

/* Array.prototype.toSpliced (ES2023 23.1.3.36): splice on a copy, without touching this. */
static embi_js_value proto_to_spliced(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double actual_start;
    if (!rel_index(rt, arg_at(argc, argv, 0), len, &actual_start)) {
        return EXC;
    }
    double skip = 0.0;
    if (argc == 1) {
        skip = len - actual_start;
    } else if (argc >= 2) {
        double sc;
        if (!embi_js_to_integer_or_infinity(rt, argv[1], &sc)) {
            return EXC;
        }
        double room = len - actual_start;
        skip = sc < 0.0 ? 0.0 : (sc > room ? room : sc);
    }
    int item_count = argc > 2 ? argc - 2 : 0;
    double new_len = len + item_count - skip;
    if (new_len > MAX_SAFE) {
        return throw_type(rt, "Invalid array length");
    }
    embi_js_object *a = embi_js_array_create(rt, new_len, rt->protos[EMBI_JS_PROTO_ARRAY]);
    if (!a) {
        return EXC;
    }
    double i = 0.0;
    for (double k = 0.0; k < actual_start; k++, i++) {
        embi_js_value v = get_at(rt, o, k);
        if (v == EXC) {
            return EXC;
        }
        if (create_at(rt, a, i, v) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    for (int j = 0; j < item_count; j++, i++) {
        if (create_at(rt, a, i, argv[2 + j]) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    for (double from = actual_start + skip; i < new_len; i++, from++) {
        embi_js_value v = get_at(rt, o, from);
        if (v == EXC) {
            return EXC;
        }
        if (create_at(rt, a, i, v) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

/* Array.prototype.with (ES2023 23.1.3.39): a copy with one element replaced. */
static embi_js_value proto_with(embi_js_realm *r, embi_js_value this_v, int argc,
                                const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EXC;
    }
    double len;
    if (!length_of(rt, o, &len)) {
        return EXC;
    }
    double rel;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &rel)) {
        return EXC;
    }
    double actual = rel >= 0.0 ? rel : len + rel;
    if (actual >= len || actual < 0.0) {
        return throw_range(rt, "Invalid index");
    }
    embi_js_object *a = embi_js_array_create(rt, len, rt->protos[EMBI_JS_PROTO_ARRAY]);
    if (!a) {
        return EXC;
    }
    embi_js_value replacement = arg_at(argc, argv, 1);
    for (double k = 0.0; k < len; k++) {
        embi_js_value v;
        if (k == actual) {
            v = replacement;
        } else {
            v = get_at(rt, o, k);
            if (v == EXC) {
                return EXC;
            }
        }
        if (create_at(rt, a, k, v) == EMBI_JS_THREW) {
            return EXC;
        }
    }
    return embi_js_from_object(a);
}

/* ------------------------------------------------------------------ installation */

typedef struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
    int magic;
} method_def;

static const method_def proto_methods[] = {
    {"at", proto_at, 1, 0},
    {"concat", proto_concat, 1, 0},
    {"copyWithin", proto_copy_within, 2, 0},
    {"every", iterate, 1, IT_EVERY},
    {"fill", proto_fill, 1, 0},
    {"filter", iterate, 1, IT_FILTER},
    {"find", find_impl, 1, 0},
    {"findIndex", find_impl, 1, 1},
    {"findLast", find_impl, 1, 2},
    {"findLastIndex", find_impl, 1, 3},
    {"flat", proto_flat, 0, 0},
    {"flatMap", proto_flat_map, 1, 0},
    {"forEach", iterate, 1, IT_FOR_EACH},
    {"includes", proto_includes, 1, 0},
    {"indexOf", index_of, 1, 0},
    {"join", proto_join, 1, 0},
    {"lastIndexOf", index_of, 1, 1},
    {"map", iterate, 1, IT_MAP},
    {"pop", proto_pop, 0, 0},
    {"push", proto_push, 1, 0},
    {"reduce", reduce_impl, 1, 0},
    {"reduceRight", reduce_impl, 1, 1},
    {"reverse", proto_reverse, 0, 0},
    {"shift", proto_shift, 0, 0},
    {"slice", proto_slice, 2, 0},
    {"some", iterate, 1, IT_SOME},
    {"sort", proto_sort, 1, 0},
    {"splice", proto_splice, 2, 0},
    {"toLocaleString", proto_to_locale_string, 0, 0},
    {"toReversed", proto_to_reversed, 0, 0},
    {"toSorted", proto_to_sorted, 1, 0},
    {"toSpliced", proto_to_spliced, 2, 0},
    {"toString", proto_to_string, 0, 0},
    {"unshift", proto_unshift, 1, 0},
    {"with", proto_with, 2, 0},
};

/* Names in Array.prototype[@@unscopables] (ES2025 23.1.3.45). */
static const char *const unscopables[] = {
    "at",          "copyWithin", "entries", "fill",      "find",       "findIndex", "findLast",
    "findLastIndex", "flat",     "flatMap", "includes",  "keys",       "toReversed", "toSorted",
    "toSpliced",   "values",
};

/* ------------------------------------------------------------------ array iterators (J2c) */

/* An array iterator's state is a varray [array, next index, kind]. The array slot is undefined
 * once the iteration is over. kind: 0 keys, 1 values, 2 entries. */
enum { AI_OBJ, AI_INDEX, AI_KIND, AI_N };

static void array_iter_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal) {
        embi_js_mark_cell(t, &((embi_js_varray *)o->internal)->hdr);
    }
}

static const embi_js_class class_array_iter = {.name = "Array Iterator", .trace = array_iter_trace};

/* Array.prototype.keys (magic 0), values (1) and entries (2). */
static embi_js_value array_iter_make(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_to_object(rt, this_v);
    if (!o) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_varray *st = embi_js_varray_new(rt, AI_N);
    EMBI_JS_STORE(st, st->elems[AI_OBJ], embi_js_from_object(o));
    EMBI_JS_WB(st, st->elems[AI_OBJ]);
    EMBI_JS_STORE(st, st->elems[AI_INDEX], embi_js_from_int32(0));
    EMBI_JS_STORE(st, st->elems[AI_KIND], embi_js_from_int32(magic));
    embi_js_object *it = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_ARRAY_ITERATOR],
                                                  &class_array_iter);
    it->internal = st;
    EMBI_JS_WB_STRUCT(it);
    return embi_js_from_object(it);
}

/* %ArrayIteratorPrototype%.next. */
static embi_js_value array_iter_next(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (!embi_js_is_object(this_v) || embi_js_as_object(this_v)->klass != &class_array_iter) {
        return throw_type(rt, "Array Iterator next called on incompatible receiver");
    }
    embi_js_varray *st = embi_js_as_object(this_v)->internal;
    if (!embi_js_is_object(st->elems[AI_OBJ])) {
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    embi_js_object *a = embi_js_as_object(st->elems[AI_OBJ]);
    double len = 0;
    if (embi_js_is_typedarray_obj(a)) {
        /* ArrayIterator.next on a TypedArray (23.1.5.1): an out-of-bounds or detached view throws. */
        embi_js_varray *vw = embi_js_view_of(a);
        size_t tl = 0;
        size_t esz = embi_js_bin_size((enum embi_js_ta_kind)embi_js_as_int32(vw->elems[EMBI_JS_VIEW_KIND]));
        if (!embi_js_view_len(vw, esz, &tl)) {
            return throw_type(rt, "TypedArray is detached or out of bounds");
        }
        len = (double)tl;
    } else if (!embi_js_length_of_array_like(rt, a, &len)) {
        return EMBI_JS_EXCEPTION;
    }
    double idx = embi_js_is_int32(st->elems[AI_INDEX]) ? (double)embi_js_as_int32(st->elems[AI_INDEX])
                                                       : embi_js_as_double(st->elems[AI_INDEX]);
    if (idx >= len) {
        EMBI_JS_STORE(st, st->elems[AI_OBJ], UNDEF);
        return embi_js_iter_result_new(rt, UNDEF, true);
    }
    EMBI_JS_STORE(st, st->elems[AI_INDEX], embi_js_number(idx + 1.0));
    EMBI_JS_WB(st, st->elems[AI_INDEX]);
    int kind = embi_js_as_int32(st->elems[AI_KIND]);
    if (kind == 0) {
        return embi_js_iter_result_new(rt, embi_js_number(idx), false);
    }
    embi_js_value v;
    if (idx >= 4294967295.0 || !embi_js_obj_index_get_fast(rt, a, (uint32_t)idx, &v)) {
        v = embi_js_obj_get(rt, a, embi_js_key_from_index((uint32_t)idx), st->elems[AI_OBJ]);
    }
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_JS_EXCEPTION;
    }
    if (kind == 1) {
        return embi_js_iter_result_new(rt, v, false);
    }
    embi_js_object *pair = embi_js_array_create_default(rt, 0.0);
    if (!embi_js_obj_create_index_fast(rt, pair, 0, embi_js_number(idx)) &&
        embi_js_create_data_property(rt, pair, embi_js_key_from_index(0), embi_js_number(idx)) ==
            EMBI_JS_THREW) {
        return EMBI_JS_EXCEPTION;
    }
    if (!embi_js_obj_create_index_fast(rt, pair, 1, v) &&
        embi_js_create_data_property(rt, pair, embi_js_key_from_index(1), v) == EMBI_JS_THREW) {
        return EMBI_JS_EXCEPTION;
    }
    return embi_js_iter_result_new(rt, embi_js_from_object(pair), false);
}

/* %ArrayIteratorPrototype%, the next function the for-of fast path compares with, and the
 * Array.prototype iterator methods. values is also Array.prototype[@@iterator]. */
static void array_iterators_init(embi_js_realm *r, embi_js_object *array_proto)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_ITERATOR]);
    rt->protos[EMBI_JS_PROTO_ARRAY_ITERATOR] = embi_js_from_object(proto);
    embi_js_object *next = embi_js_native_function_new(r, "next", 0, array_iter_next, 0);
    rt->protos[EMBI_JS_PROTO_ARRAY_ITER_NEXT] = embi_js_from_object(next);
    embi_js_define_raw(rt, proto, embi_js_key_c(rt, "next"), embi_js_from_object(next), ATTR_WC);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, "Array Iterator", 14)),
                       ATTR_C);

    embi_js_object *values = embi_js_native_function_new(r, "values", 0, array_iter_make, 1);
    rt->protos[EMBI_JS_PROTO_ARRAY_VALUES] = embi_js_from_object(values);
    embi_js_define_raw(rt, array_proto, embi_js_key_c(rt, "values"), embi_js_from_object(values),
                       ATTR_WC);
    embi_js_define_raw(rt, array_proto, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       embi_js_from_object(values), ATTR_WC);
    embi_js_define_method(r, array_proto, "keys", array_iter_make, 0, 0);
    embi_js_define_method(r, array_proto, "entries", array_iter_make, 0, 2);
}

void embi_js_init_builtins_array(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *proto = embi_js_array_create(rt, 0.0, rt->protos[EMBI_JS_PROTO_OBJECT]);
    rt->protos[EMBI_JS_PROTO_ARRAY] = embi_js_from_object(proto);

    embi_js_object *ctor = embi_js_native_constructor_new(r, "Array", 1, array_ctor, 0);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Array"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, ctor, "isArray", array_is_array, 1, 0);
    embi_js_define_method(r, ctor, "from", array_from, 1, 0);
    embi_js_define_method(r, ctor, "of", array_of, 0, 0);
    embi_js_define_method(r, ctor, "fromAsync", array_from_async, 1, 0);

    embi_js_object *getter =
        embi_js_native_function_new(r, "get [Symbol.species]", 0, species_getter, 0);
    embi_js_define_accessor_raw(rt, ctor, embi_js_wk_sym(rt, EMBI_JS_WK_SPECIES),
                                embi_js_from_object(getter), UNDEF, ATTR_C);

    for (size_t i = 0; i < sizeof proto_methods / sizeof proto_methods[0]; i++) {
        const method_def *m = &proto_methods[i];
        embi_js_define_method(r, proto, m->name, m->fn, m->length, m->magic);
    }

    embi_js_object *unsc = embi_js_object_new(rt, EMBI_JS_NULL);
    for (size_t i = 0; i < sizeof unscopables / sizeof unscopables[0]; i++) {
        embi_js_define_raw(rt, unsc, embi_js_key_c(rt, unscopables[i]), EMBI_JS_TRUE,
                           EMBI_JS_ATTR_DEFAULT);
    }
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_UNSCOPABLES),
                       embi_js_from_object(unsc), ATTR_C);
    array_iterators_init(r, proto);

    /* J5e: the intrinsics whose stores clear a protector (object.c protect_named_store). Set last, so
     * the setup above runs with the protectors intact. */
    proto->hdr.flags |= EMBI_JS_OBJ_IS_PROTO | EMBI_JS_OBJ_WATCHED;
    ctor->hdr.flags |= EMBI_JS_OBJ_WATCHED;
    embi_js_as_object(rt->protos[EMBI_JS_PROTO_ARRAY_ITERATOR])->hdr.flags |=
        EMBI_JS_OBJ_IS_PROTO | EMBI_JS_OBJ_WATCHED;
}
