/*
 * Number: the Number constructor (called as a function it converts with ToNumeric, with new it
 * makes a Number wrapper), the constants, Number.isFinite/isInteger/isNaN/isSafeInteger, the
 * parseFloat and parseInt functions (the same objects as the globals, as ES2015 requires), and
 * Number.prototype (a Number wrapper of +0). toString(radix), toFixed, toExponential and
 * toPrecision use the exact formatting in src/num.
 *
 * toLocaleString returns the same text as toString: no locale data is available.
 */
#include "js/bigint.h"
#include "js/builtins.h"
#include "js/js.h"
#include "num/num.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EXC EMBI_JS_EXCEPTION
#define UNDEF EMBI_JS_UNDEFINED
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE
#define ATTR_WC (EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE)

/* 2^53 - 1, the largest safe integer. */
#define MAX_SAFE 9007199254740991.0

static embi_js_value arg_at(int argc, const embi_js_value *argv, int i)
{
    return i < argc ? argv[i] : UNDEF;
}

static bool is_finite(double d)
{
    return d - d == 0.0;
}

static embi_js_value ascii_val(embi_js_runtime *rt, const char *p, size_t n)
{
    embi_js_string *s = embi_js_str_new_latin1(rt, (const uint8_t *)p, n);
    return s ? embi_js_from_string(s) : EXC;
}

/* thisNumberValue: a number, or a Number wrapper. */
static bool this_number(embi_js_value v, double *out)
{
    if (embi_js_is_number(v)) {
        *out = embi_js_as_double(v);
        return true;
    }
    if (embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_number) {
        *out = embi_js_as_double(embi_js_wrapper_value(embi_js_as_object(v)));
        return true;
    }
    return false;
}

static embi_js_value number_to_str(embi_js_runtime *rt, double x)
{
    char buf[EMBI_NUM_STR_MAX];
    size_t n = embi_num_to_str(x, buf);
    return ascii_val(rt, buf, n);
}

static embi_js_value make_wrapper(embi_js_runtime *rt, embi_js_value proto, double d)
{
    embi_js_object *o = embi_js_object_new_class(rt, proto, &embi_js_class_number);
    o->internal = (void *)(uintptr_t)embi_js_number(d);
    EMBI_JS_WB_STRUCT(o);
    return embi_js_from_object(o);
}

/* Converts the argument as Number(value) does: no argument gives +0, otherwise ToNumeric. */
/* Number(value): ToNumeric, then a BigInt becomes the nearest Number (ES2020 21.1.1.1). */
static embi_js_value to_number_arg(embi_js_runtime *rt, int argc, const embi_js_value *argv)
{
    if (argc == 0) {
        return embi_js_from_int32(0);
    }
    embi_js_value n = embi_js_to_numeric(rt, argv[0]);
    if (n == EXC || !embi_js_is_bigint(n)) {
        return n;
    }
    return embi_js_number(embi_js_bigint_to_double(embi_js_as_bigint(n)));
}

static embi_js_value number_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)f;
    (void)this_v;
    return to_number_arg(rt, argc, argv);
}

static embi_js_value number_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target)
{
    (void)f;
    embi_js_value n = to_number_arg(rt, argc, argv);
    if (n == EXC) {
        return EXC;
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_NUMBER);
    if (proto == EXC) {
        return EXC;
    }
    return make_wrapper(rt, proto, embi_js_as_double(n));
}

static const embi_js_class number_ctor_class = {
    .name = "Function",
    .call = number_call,
    .construct = number_construct,
};

/* ------------------------------------------------------------------ statics */

/* Number.isFinite (magic 0), isInteger (1), isNaN (2), isSafeInteger (3). */
static embi_js_value number_is(embi_js_realm *r, embi_js_value this_v, int argc,
                               const embi_js_value *argv, int magic)
{
    (void)r;
    (void)this_v;
    embi_js_value v = arg_at(argc, argv, 0);
    if (!embi_js_is_number(v)) {
        return EMBI_JS_FALSE;
    }
    double d = embi_js_as_double(v);
    switch (magic) {
    case 0:
        return embi_js_from_bool(is_finite(d));
    case 1:
        return embi_js_from_bool(is_finite(d) && trunc(d) == d);
    case 2:
        return embi_js_from_bool(d != d);
    default:
        return embi_js_from_bool(is_finite(d) && trunc(d) == d && fabs(d) <= MAX_SAFE);
    }
}

/* ------------------------------------------------------------------ prototype */

static embi_js_value num_to_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                   const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            rt, "Number.prototype.toString requires that 'this' be a Number");
    }
    embi_js_value rv = arg_at(argc, argv, 0);
    unsigned radix = 10;
    if (!embi_js_is_undefined(rv)) {
        double rd;
        if (!embi_js_to_integer_or_infinity(rt, rv, &rd)) {
            return EXC;
        }
        if (rd < 2 || rd > 36) {
            return embi_js_throw_range_error(rt, "toString() radix must be between 2 and 36");
        }
        radix = (unsigned)rd;
    }
    if (radix == 10) {
        return number_to_str(rt, x);
    }
    char buf[EMBI_NUM_STR_MAX];
    size_t n = embi_num_to_radix_str(x, radix, buf);
    return ascii_val(rt, buf, n);
}

/* toLocaleString ignores its arguments and returns the toString(10) text. */
static embi_js_value num_to_locale_string(embi_js_realm *r, embi_js_value this_v, int argc,
                                          const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            rt, "Number.prototype.toLocaleString requires that 'this' be a Number");
    }
    return number_to_str(rt, x);
}

static embi_js_value num_to_fixed(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            rt, "Number.prototype.toFixed requires that 'this' be a Number");
    }
    double f;
    if (!embi_js_to_integer_or_infinity(rt, arg_at(argc, argv, 0), &f)) {
        return EXC;
    }
    if (!is_finite(f) || f < 0 || f > 100) {
        return embi_js_throw_range_error(rt, "toFixed() digits argument must be between 0 and 100");
    }
    if (!is_finite(x)) {
        return number_to_str(rt, x);
    }
    char buf[EMBI_NUM_STR_MAX];
    size_t n = embi_num_to_fixed(x, (unsigned)f, buf);
    return ascii_val(rt, buf, n);
}

static embi_js_value num_to_exponential(embi_js_realm *r, embi_js_value this_v, int argc,
                                        const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            rt, "Number.prototype.toExponential requires that 'this' be a Number");
    }
    embi_js_value fv = arg_at(argc, argv, 0);
    double f = 0;
    if (!embi_js_to_integer_or_infinity(rt, fv, &f)) {
        return EXC;
    }
    if (!is_finite(x)) {
        return number_to_str(rt, x);
    }
    if (f < 0 || f > 100) {
        return embi_js_throw_range_error(rt,
                                         "toExponential() argument must be between 0 and 100");
    }
    char buf[EMBI_NUM_STR_MAX];
    /* An undefined argument selects the shortest round-trip digits. */
    int digits = embi_js_is_undefined(fv) ? -1 : (int)f;
    size_t n = embi_num_to_exponential(x, digits, buf);
    return ascii_val(rt, buf, n);
}

static embi_js_value num_to_precision(embi_js_realm *r, embi_js_value this_v, int argc,
                                      const embi_js_value *argv, int magic)
{
    (void)magic;
    embi_js_runtime *rt = r->rt;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            rt, "Number.prototype.toPrecision requires that 'this' be a Number");
    }
    embi_js_value pv = arg_at(argc, argv, 0);
    if (embi_js_is_undefined(pv)) {
        return number_to_str(rt, x);
    }
    double p;
    if (!embi_js_to_integer_or_infinity(rt, pv, &p)) {
        return EXC;
    }
    if (!is_finite(x)) {
        return number_to_str(rt, x);
    }
    if (p < 1 || p > 100) {
        return embi_js_throw_range_error(rt, "toPrecision() argument must be between 1 and 100");
    }
    char buf[EMBI_NUM_STR_MAX];
    size_t n = embi_num_to_precision(x, (unsigned)p, buf);
    return ascii_val(rt, buf, n);
}

static embi_js_value num_value_of(embi_js_realm *r, embi_js_value this_v, int argc,
                                  const embi_js_value *argv, int magic)
{
    (void)argc;
    (void)argv;
    (void)magic;
    double x;
    if (!this_number(this_v, &x)) {
        return embi_js_throw_type_error(
            r->rt, "Number.prototype.valueOf requires that 'this' be a Number");
    }
    return embi_js_number(x);
}

/* ------------------------------------------------------------------ installation */

static embi_js_object *make_ctor(embi_js_realm *r, const char *name, int length)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *f = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_FUNCTION],
                                                 &number_ctor_class);
    f->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                       embi_js_number((double)length), ATTR_C);
    embi_js_define_raw(rt, f, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, name, strlen(name))),
                       ATTR_C);
    return f;
}

struct const_def {
    const char *name;
    double value;
};

static const struct const_def number_consts[] = {
    {"EPSILON", 2.220446049250313e-16},
    {"MAX_SAFE_INTEGER", 9007199254740991.0},
    {"MAX_VALUE", 1.7976931348623157e308},
    {"MIN_SAFE_INTEGER", -9007199254740991.0},
    {"MIN_VALUE", 5e-324},
    {"NaN", NAN},
    {"NEGATIVE_INFINITY", -INFINITY},
    {"POSITIVE_INFINITY", INFINITY},
};

struct method_def {
    const char *name;
    embi_js_native fn;
    int length;
    int magic;
};

static const struct method_def statics[] = {
    {"isFinite", number_is, 1, 0},
    {"isInteger", number_is, 1, 1},
    {"isNaN", number_is, 1, 2},
    {"isSafeInteger", number_is, 1, 3},
};

static const struct method_def proto_methods[] = {
    {"toExponential", num_to_exponential, 1, 0},
    {"toFixed", num_to_fixed, 1, 0},
    {"toLocaleString", num_to_locale_string, 0, 0},
    {"toPrecision", num_to_precision, 1, 0},
    {"toString", num_to_string, 1, 0},
    {"valueOf", num_value_of, 0, 0},
};

void embi_js_init_builtins_number(embi_js_realm *r)
{
    embi_js_runtime *rt = r->rt;
    embi_js_value obj_proto = rt->protos[EMBI_JS_PROTO_OBJECT];

    /* Number.prototype is a Number wrapper whose value is +0. */
    embi_js_object *proto = embi_js_object_new_class(rt, obj_proto, &embi_js_class_number);
    proto->internal = (void *)(uintptr_t)embi_js_from_int32(0);
    EMBI_JS_WB_STRUCT(proto);
    rt->protos[EMBI_JS_PROTO_NUMBER] = embi_js_from_object(proto);

    embi_js_object *ctor = make_ctor(r, "Number", 1);
    embi_js_define_raw(rt, ctor, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                       embi_js_from_object(proto), 0);
    embi_js_define_raw(rt, proto, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                       embi_js_from_object(ctor), ATTR_WC);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Number"), embi_js_from_object(ctor),
                       ATTR_WC);

    for (size_t i = 0; i < sizeof number_consts / sizeof number_consts[0]; i++) {
        embi_js_define_raw(rt, ctor, embi_js_key_c(rt, number_consts[i].name),
                           embi_js_number(number_consts[i].value), 0);
    }
    for (size_t i = 0; i < sizeof statics / sizeof statics[0]; i++) {
        embi_js_define_method(r, ctor, statics[i].name, statics[i].fn, statics[i].length,
                              statics[i].magic);
    }

    /* Number.parseFloat and Number.parseInt are the global functions (the global object is
     * installed before this builtin). */
    static const char *const parse_names[] = {"parseFloat", "parseInt"};
    for (size_t i = 0; i < sizeof parse_names / sizeof parse_names[0]; i++) {
        embi_js_value fn = embi_js_get(rt, r->global, embi_js_key_c(rt, parse_names[i]));
        embi_js_define_raw(rt, ctor, embi_js_key_c(rt, parse_names[i]), fn, ATTR_WC);
    }

    for (size_t i = 0; i < sizeof proto_methods / sizeof proto_methods[0]; i++) {
        embi_js_define_method(r, proto, proto_methods[i].name, proto_methods[i].fn,
                              proto_methods[i].length, proto_methods[i].magic);
    }
}
