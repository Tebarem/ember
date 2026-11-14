/*
 * BigInt (ECMA-262 21.2): the BigInt function (a conversion, never a constructor: new throws a
 * TypeError), BigInt.asIntN and BigInt.asUintN, and BigInt.prototype (toString(radix),
 * toLocaleString, valueOf, @@toStringTag). The arithmetic lives in bigint.c.
 *
 * toLocaleString returns the same text as toString for now: the minimal Intl of J4g (section 7)
 * replaces it with en-US grouping.
 */
#include "js/bigint.h"
#include "js/builtins.h"
#include "js/js.h"
#include "num/num.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (ATTR_W | ATTR_C)

/* 2^53 - 1: the largest ToIndex value. */
#define MAX_INDEX 9007199254740991.0

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static embi_js_value atom_val(embi_js_runtime *rt, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(rt, s, strlen(s)));
}

/* thisBigIntValue: a BigInt, or a BigInt wrapper. Returns EXC with a TypeError otherwise. */
static embi_js_value this_bigint(embi_js_runtime *rt, embi_js_value v, const char *method)
{
    if (embi_js_is_bigint(v)) {
        return v;
    }
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_bigint) {
        return embi_js_bigint_wrapper_value(embi_js_as_object(v));
    }
    return embi_js_throw_type_error(rt, "BigInt.prototype.%s requires that 'this' be a BigInt",
                                    method);
}

/* NumberToBigInt (21.2.1.1.1): the number must be an integer. */
static embi_js_value number_to_bigint(embi_js_runtime *rt, double d)
{
    if (!(d - floor(d) == 0.0)) {
        char buf[EMBI_NUM_STR_MAX];
        (void)embi_num_to_str(d, buf);
        return embi_js_throw_range_error(
            rt, "The number %s cannot be converted to a BigInt because it is not an integer", buf);
    }
    return embi_js_from_bigint(embi_js_bigint_from_double(rt, d));
}

/* The BigInt function (21.2.1.1). */
static embi_js_value bigint_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    embi_js_value prim = embi_js_to_primitive(rt, arg_at(argc, argv, 0), EMBI_JS_HINT_NUMBER);
    if (embi_js_is_exception(prim)) {
        return EXC;
    }
    if (embi_js_is_number(prim)) {
        return number_to_bigint(rt, embi_js_as_double(prim));
    }
    return embi_js_to_bigint(rt, prim);
}

static embi_js_value bigint_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    (void)argc;
    (void)argv;
    (void)new_target;
    return embi_js_throw_type_error(rt, "BigInt is not a constructor");
}

static const embi_js_class bigint_ctor_class = {
    .name = "Function",
    .call = bigint_call,
    .construct = bigint_construct,
};

/* ToIndex (7.1.22) for the bits argument of asIntN and asUintN. */
static bool to_index(embi_js_runtime *rt, embi_js_value v, uint64_t *out)
{
    if (embi_js_is_undefined(v)) {
        *out = 0;
        return true;
    }
    double d = 0.0;
    if (!embi_js_to_integer_or_infinity(rt, v, &d)) {
        return false;
    }
    if (d < 0.0 || d > MAX_INDEX) {
        (void)embi_js_throw_range_error(rt, "Invalid value: not (convertible to) a safe integer");
        return false;
    }
    *out = (uint64_t)d;
    return true;
}

/* BigInt.asIntN (magic 0) and BigInt.asUintN (magic 1). */
static embi_js_value bigint_as_n(embi_js_realm *r, embi_js_value this_v, int argc,
                                 const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    uint64_t bits = 0;
    if (!to_index(r->rt, arg_at(argc, argv, 0), &bits)) {
        return EXC;
    }
    embi_js_value big = embi_js_to_bigint(r->rt, arg_at(argc, argv, 1));
    if (embi_js_is_exception(big)) {
        return EXC;
    }
    return embi_js_bigint_as_n(r->rt, big, bits, magic == 0);
}

/* BigInt.prototype.toString(radix). */
static embi_js_value proto_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                     const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value x = this_bigint(rt, this_v, "toString");
    if (embi_js_is_exception(x)) {
        return EXC;
    }
    unsigned radix = 10;
    embi_js_value rv = arg_at(argc, argv, 0);
    if (!embi_js_is_undefined(rv)) {
        double d = 0.0;
        if (!embi_js_to_integer_or_infinity(rt, rv, &d)) {
            return EXC;
        }
        if (d < 2.0 || d > 36.0) {
            return embi_js_throw_range_error(rt, "toString() radix must be between 2 and 36");
        }
        radix = (unsigned)d;
    }
    embi_js_string *s = embi_js_bigint_to_string(rt, embi_js_as_bigint(x), radix);
    return s ? embi_js_from_string(s) : EXC;
}

/* BigInt.prototype.toLocaleString: until J4g, the toString text in radix 10. */
static embi_js_value proto_to_locale_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                            const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    embi_js_value x = this_bigint(rt, this_v, "toLocaleString");
    if (embi_js_is_exception(x)) {
        return EXC;
    }
    embi_js_string *s = embi_js_bigint_to_string(rt, embi_js_as_bigint(x), 10);
    return s ? embi_js_from_string(s) : EXC;
}

/* BigInt.prototype.valueOf. */
static embi_js_value proto_value_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    return this_bigint(r->rt, this_v, "valueOf");
}

void embi_js_init_builtins_bigint(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;

    embi_js_object *proto;
    if (embi_js_is_empty(rt->protos[EMBI_JS_PROTO_BIGINT])) {
        proto = embi_js_object_new(rt, rt->protos[EMBI_JS_PROTO_OBJECT]);
        rt->protos[EMBI_JS_PROTO_BIGINT] = embi_js_from_object(proto);
    } else {
        proto = embi_js_as_object(rt->protos[EMBI_JS_PROTO_BIGINT]);
    }

    embi_js_object *ctor =
        embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION], &bigint_ctor_class);
    ctor->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(1.0),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_NAME), atom_val(rt, "BigInt"),
                       ATTR_C);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "BigInt"), embi_js_from_object(ctor),
                       ATTR_WC);

    embi_js_define_method(r, ctor, "asIntN", bigint_as_n, 2, 0);
    embi_js_define_method(r, ctor, "asUintN", bigint_as_n, 2, 1);

    embi_js_define_method(r, proto, "toString", proto_to_string, 0, 0);
    embi_js_define_method(r, proto, "toLocaleString", proto_to_locale_string, 0, 0);
    embi_js_define_method(r, proto, "valueOf", proto_value_of, 0, 0);
    embi_js_define_raw(rt, proto, embi_js_wk_sym(rt, EMBI_JS_WK_TO_STRING_TAG),
                       atom_val(rt, "BigInt"), ATTR_C);
}
